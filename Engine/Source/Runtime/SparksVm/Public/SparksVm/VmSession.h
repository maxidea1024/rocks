#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <SparksVm/Vm.h>
#include <SparksVm/VmHost.h>

// Sparks 실행 단위(session)다(Docs/Design/SparksRuntime.ko.md 2절·2.2절, LC6a). instance(host
// 객체에 붙은 Sparks 클래스 instance)의 필드 저장소와 실행 문맥 하나를 가진다. Sparks 클래스 필드의
// GETF·SETF는 session의 instance 저장소를 직접 쓴다(필드 로컬 묶음 — R2).
//
// 사용 예:
//   VmSession session(allocator, &host);
//   session.AddModule(linked);
//   auto instance = session.CreateInstance(handle, linked, classIndex, init);
//   auto enabled = session.RunHook(instance.Value(), BytecodeHookKind::Enable, {}, {});
//   session.Detach(instance.Value());
//
// 재진입: 실행 중(handler·hook의 native 호출 안)에 host가 부를 수 있는 진입은 Emit뿐이다(항목만
// 넣고 전달은 바깥 진입이 한다). 그 밖의 진입은 실행 중이면 InvalidArgument다(LC6b 리뷰 B4 —
// 활성 전환·파괴의 지연 적용은 LC6d).

namespace rocks::sparks {

namespace detail {
class VmLibrary;
} // namespace detail

class VmSession final {
public:
    VmSession(Allocator& allocator, VmHost* host) noexcept;
    // 남은 instance를 모두 Detach한다(CloseSession — 실행 문맥 ledger가 0이 된다).
    ~VmSession() noexcept;
    VmSession(const VmSession&) = delete;
    VmSession& operator=(const VmSession&) = delete;

    // session 모듈 목록에 더한다(다른 모듈 기반 클래스를 찾는 범위). linked는 링크에 성공했고
    // session의 host로 링크했어야 하며 session보다 오래 산다. 같은 경로의 다른 모듈은 거부한다
    // (기반 찾기가 섞이지 않게). 실패: 그 밖이면 InvalidArgument,
    // 할당 실패는 OutOfMemory.
    Result<void> AddModule(const VmLinkedModule& linked) noexcept;

    // handle에 module의 classIndex 클래스 instance를 붙이고 기반 먼저 `$init`을 부른다. 반환은
    // instance 번호, init은 초기화 실행 결과(fault면 instance를 만들지 않고 NoBytecodeIndex를
    // 돌려준다). 실패: 모듈이 session에 없거나 클래스 번호가 범위 밖이거나 handle에 이미 instance가
    // 있거나 handle이 살아 있지 않으면 InvalidArgument, 할당 실패는 OutOfMemory.
    Result<uint32> CreateInstance(uint64 handle, const VmLinkedModule& module, uint32 classIndex,
                                  VmCallResult& init) noexcept;

    // instance의 kind hook을 기반 사슬 순(기반 먼저)으로 부른다. scalars·refs는 this 뒤 인자(Tick의
    // dt 등)다. 첫 fault에서 멈추고 그 결과, 아니면 마지막 결과(hook이 없으면 Returned·0 steps)다.
    Result<VmCallResult> RunHook(uint32 instance, BytecodeHookKind kind, Span<const uint64> scalars,
                                 Span<const uint64> refs,
                                 Span<VmBuffer* const> buffers = Span<VmBuffer* const>()) noexcept;

    // instance를 this로 module의 function을 부른다(refs는 this 뒤 Ref 인자).
    Result<VmCallResult> Call(uint32 instance, const VmLinkedModule& module, uint32 function,
                              Span<const uint64> scalars, Span<const uint64> refs,
                              Span<VmBuffer* const> buffers) noexcept;

    // 필드 버퍼와 저장소를 놓고 instance를 표에서 뺀다(번호는 뒤의 CreateInstance가 다시 쓸 수
    // 있다). 연결·정지 실행·조각의 쓰기 기록·확보한 Changed 자리도 지운다. 실패: 실행 중이면
    // InvalidArgument(없는 번호는 성공 — 할 일 없음).
    Result<void> Detach(uint32 instance) noexcept;

    // 붙은 instance를 모두 Detach한다(CloseSession).
    void DetachAll() noexcept;

    // 붙은 instance 수다.
    uint32 AttachedCount() const noexcept;

    // handle에 붙은 instance 번호다(없으면 NoBytecodeIndex).
    uint32 Find(uint64 handle) const noexcept;

    VmExecution& Execution() noexcept {
        return execution_;
    }

    // ---- 발행·전달(SparksRuntime 2.3절 — LC6b) ----

    // 상한(SL 5.4절 — 프로젝트 설정의 기본값).
    static constexpr uint32 MaxCascadeDepth = 16;
    static constexpr uint32 MaxDeliveriesPerPhase = 65536;
    static constexpr uint32 MaxQueuedItems = 262144;

    // 새 스크립트 단계를 연다: 단계당 전달 수를 0으로 두고 이월 항목을 전달한다.
    Result<void> OpenPhase() noexcept;

    // host 진입 발행이다(native 이벤트·시험): 발행자 handle과 key의 항목을 깊이 1로 넣고 전달한다.
    // 인자는 Scalar·Ref·Buf별(Buf는 빌려 준다 — 항목이 참조를 더한다). 실행 중(native 호출 안)이면
    // 지금 조각의 깊이 + 1로 넣기만 한다(전달은 바깥 진입). key의 글자는 항목이 전달될 때까지(이월
    // 포함) 살아 있어야 한다 — 링크한 모듈의 key나 host가 고정한 글자를 쓴다.
    Result<void> Emit(uint64 publisher, const VmEventKey& key, Span<const uint64> scalars,
                      Span<const uint64> refs, Span<VmBuffer* const> buffers) noexcept;

    // 마지막 진입(RunHook·Call·Emit·OpenPhase)에서 실행된 handler 기록이다(다음 진입이 지운다).
    struct Delivery {
        uint64 subscriber = 0;
        const VmLinkedModule* module = nullptr;
        uint32 function = NoBytecodeIndex;
        VmCallResult result;
        bool resumed = false; // 정지했던 실행의 재개(SparksRuntime 2.4절)
        // 적용 루프·프레임 끝 정리가 부른 수명 hook(SparksRuntime 2.6절 — function은 없다).
        bool hook = false;
        BytecodeHookKind hookKind = BytecodeHookKind::Enable;
    };

    Span<const Delivery> Deliveries() const noexcept {
        return deliveries_.View();
    }

    // 큐에 남은(이월) 항목 수다.
    uint32 QueuedCount() const noexcept;

    // ---- async(SparksRuntime 2.4절 — LC6c) ----

    // 시계를 진행하고(프레임 +1) 가변 재개 단계(새 스크립트 단계 — 이월 항목 전달 포함)를 돈다:
    // 단계 시작 때 기한이 된 Delay·NextTick 대기의 실행을 (등록 프레임, instance 생성 번호, 등록
    // 순번) 순으로 재개한다. 재개된 실행은 Deliveries에 resumed로 남는다. 실패: 실행 중이거나
    // delta가 유한한 0 이상이 아니면 InvalidArgument, 할당 실패는 OutOfMemory.
    // paused면 `Clock.Game` Delay와 NextTick을 완료하지 않는다(SL 6.1절 — UI 시계 대기는 진행).
    Result<void> Advance(double gameDelta, double uiDelta, bool paused = false) noexcept;
    // 고정 재개 단계(NextFixedTick — 새 스크립트 단계)를 돈다.
    Result<void> FixedStep() noexcept;
    // 정지해 있는 실행 수다.
    uint32 SuspendedCount() const noexcept;
    // 대기 표의 살아 있는 항목 수다(재개·취소된 대기는 돌려받는다 — 장시간 반복에도 일정).
    uint32 WaitCount() const noexcept;
    // ---- 지연 적용·종료(SparksRuntime 2.6절 — LC6d2) ----

    static constexpr uint32 MaxApplyRounds = 64;           // 적용 루프(SL 9.3절)
    static constexpr uint64 TeardownInstructions = 100000; // Disable·Destroy hook 명령 예산

    // 활성 전환·파괴 요청을 넣는다(실행 중에도 — host native `SetEnabled`·`Destroy`가 부른다).
    // 실패: 없는 instance·session 종료 뒤면 InvalidArgument, 할당 실패는 OutOfMemory.
    Result<void> RequestEnabled(uint32 instance, bool enabled) noexcept;
    Result<void> RequestDestroy(uint32 instance) noexcept;
    // 스크립트 단계 끝: 요청 순으로 Enable·Disable hook을 돌리고(그 emit은 같은 큐로 전달) 그동안
    // 생긴 요청을 이어 처리한다(64회 — 남으면 다음 단계로). 파괴 요청은 PendingDestroy로 표시한다.
    Result<void> ApplyRequests() noexcept;
    // 프레임 끝 정리: PendingDestroy instance를 요청 순으로 Disable(활성이면) → Destroy hook →
    // Detach한다.
    Result<void> EndFrame() noexcept;
    // session을 닫는다(SL 9.1절): admission 차단 → 모든 instance Detach → 큐·Changed 자리 폐기 →
    // 연결·대기 표 해제. 뒤에는 실행 문맥 ledger가 0이다. 실행 중이면 InvalidArgument.
    Result<void> CloseSession() noexcept;
    // 남은(다음 단계로 넘긴) 요청 수다.
    uint32 PendingRequests() const noexcept {
        return static_cast<uint32>(requests_.Size());
    }

    // ---- fault 처리·격리(SparksRuntime 2.6절 — LC6d3, SL 8.3절) ----

    static constexpr uint32 IsolationFaults = 3;         // 이 수번째 fault에서 격리
    static constexpr uint64 IsolationWindowFrames = 600; // 그 fault들이 든 프레임 폭(경계 포함)

    enum class InstanceState : uint8 {
        Normal,
        TicksStopped, // Tick 계열 fault — 그 모듈이 다시 링크될 때까지 Tick·FixedTick·LateTick 없음
        Isolated      // admission은 Destroy hook만
    };

    InstanceState StateOf(uint32 instance) const noexcept;

    // fault 위치별 기록이다: (모듈, 함수, 주소, 종류)마다 첫 기록과 횟수(SL 8.3절 "보고").
    struct FaultRecord {
        const VmLinkedModule* module = nullptr;
        uint32 function = NoBytecodeIndex;
        uint32 address = NoBytecodeIndex;
        VmFaultKind kind = VmFaultKind::None;
        uint64 count = 0;
    };

    Span<const FaultRecord> Faults() const noexcept {
        return faults_.View();
    }

    // `@skipWhileBusy`로 버린 이벤트 수다(디버거 표시 — SL 6.1절 SD11).
    uint64 SkippedWhileBusy() const noexcept {
        return skippedWhileBusy_;
    }

    // instance의 필드 저장소(기반 사슬 전체 — bank별)다(시험·덤프용).
    struct FieldView {
        Span<const uint64> scalars;
        Span<const uint64> refs;
        Span<VmBuffer* const> buffers;
    };

    FieldView Fields(uint32 instance) const noexcept;
    // instance의 클래스다.
    const VmLinkedModule* ModuleOf(uint32 instance) const noexcept;
    uint32 ClassOf(uint32 instance) const noexcept;
    // 클래스 구역의 bank별 시작(기반 사슬의 slot 합)이다. 기반을 찾지 못하면 false.
    bool RegionStart(const VmLinkedModule& module, uint32 classIndex,
                     uint32 (&start)[BytecodeBankCount]) const noexcept;

private:
    friend class VmExecution;
    friend class detail::VmLibrary;

    // 이벤트 대기 handle을 만든다(WAITEV — 실패는 0과 error).
    uint64 NewEventWait(uint64 publisher, const VmEventKey& key, ErrorCode& error) noexcept;
    // 끝난 하위 대기 handle이다(CALLW 경계 frame이 반환·ENDEXEC로 끝남).
    uint64 NewChildDone(ErrorCode& error) noexcept;
    // CALLW 경계(frame boundary)부터 맨 위까지를 하위 정지 실행으로 옮기고 하위 대기 handle을
    // child에 둔다(AWAIT가 부른다 — 부르는 쪽 frame은 실행 문맥에 남는다).
    VmFaultKind SuspendChild(usize boundary, uint64 wait, uint32 resultRegister, uint32 resultBank,
                             uint32 resultSlots, uint64& child, ErrorCode& error) noexcept;

    // 정지한 실행이다(R9 — frame stack 전체).
    struct Suspended {
        explicit Suspended(Allocator& allocator) noexcept
            : frames(allocator), generations(allocator), held(allocator), scalars(allocator),
              refs(allocator), buffers(allocator) {
        }

        Array<VmExecution::Frame> frames;
        Array<uint64> generations; // frame 모듈의 링크 세대(다시 링크했으면 재개하지 않고 취소)
        Array<uint32> held;        // frame마다 held 구간(first << 16 | count — 정지 때 보관)
        Array<uint64> scalars;
        Array<uint64> refs;
        Array<VmBuffer*> buffers;
        uint32 resultRegister = 0;
        uint32 resultBank = 0;
        uint32 resultSlots = 0;
        uint64 wait = 0;
        uint32 instance = NoBytecodeIndex;
        uint64 serial = 0; // instance 생성 번호(재개 순서 key — 번호 재사용에 안정)
        uint64 order = 0;
        RandomStream random{0}; // 실행의 Random stream(재개가 이어 쓴다 — SL 6.3절)
        uint64 childWait = 0;   // 하위 실행이면 부모 묶음 안의 하위 대기 handle
        uint32 depthOffset = 0; // 바깥 frame 수(재개 때 누적 호출 깊이 — LC6c2 리뷰 B1)
        // 이벤트 handler로 시작된 실행이면 그 발행자·handler(재진입 판정 — SD11).
        uint64 publisher = 0;
        const VmLinkedModule* handlerModule = nullptr;
        uint32 handler = NoBytecodeIndex;
        bool live = false;
    };

    enum class WaitKind : uint8 {
        Delay,
        NextTick,
        NextFixedTick,
        All,
        Race,
        Event, // 발행자의 이벤트(SparksRuntime 2.5절)
        Child  // 하위 실행(CALLW)의 끝
    };

    // 대기 하나다(handle = VmWaitTag | 세대 << 32 | 번호 — 돌려받은 번호의 옛 handle은 무효).
    // All·Race의 자식은 firstChild·nextSibling 목록이다(인자 순).
    struct Wait {
        WaitKind kind = WaitKind::NextTick;
        uint8 clock = 0; // 0 Game, 1 UI
        double deadline = 0.0;
        uint64 frame = 0;     // 등록 프레임
        uint64 fixedStep = 0; // 등록 고정 step
        uint32 firstChild = NoBytecodeIndex;
        uint32 nextSibling = NoBytecodeIndex;
        uint32 parent = NoBytecodeIndex;
        uint32 suspension = NoBytecodeIndex; // 이 대기를 기다리는 정지 실행
        uint32 instance = NoBytecodeIndex;
        uint64 order = 0;
        uint32 generation = 0;
        // Event: 발행자·key·연결 순번(전달 순서)·등록 때 큐 항목 일련번호(그 뒤 항목만 받는다).
        uint64 publisher = 0;
        VmEventKey key;
        uint64 connectionOrder = 0;
        uint64 afterSequence = 0;
        uint32 child =
            NoBytecodeIndex; // Child: 하위 정지 실행 번호(끝났거나 아직 정지 전이면 없음)
        bool used = false;
        bool done = false;
        bool cancelled = false;
        bool awaited = false; // AWAIT가 한 번 기다렸다(다시 기다릴 수 없다)
    };

    // AWAIT(실행 중)이 부른다: 실행 문맥의 frame·레지스터를 옮겨 정지 실행을 만든다.
    VmFaultKind Suspend(uint64 wait, uint32 resultRegister, uint32 resultBank, uint32 resultSlots,
                        uint32& suspension, ErrorCode& error) noexcept;
    // 대기를 만든다(언어 함수 — 실패는 0과 error).
    uint64 NewWait(WaitKind kind, uint8 clock, double seconds, Span<const uint64> children,
                   ErrorCode& error) noexcept;
    // 단계 시작 때 완료된 대기의 정지 실행을 순서대로 재개한다.
    Result<void> ResumeDone() noexcept;
    // 정지 실행 하나를 재개한다(handle은 기다리던 대기 — 바뀌었으면 하지 않는다). depth·root는 재개
    // 조각의 연쇄 깊이·루트, inDrain이면 전달 순회 안(조각 끝만 정리하고 큐는 바깥이 비운다).
    Result<void> ResumeSuspended(uint32 suspended, uint64 wait, uint32 depth, uint64 root,
                                 bool inDrain) noexcept;
    // 하위 실행의 끝(반환·fault)을 부모 묶음에 반영한다.
    Result<void> FinishChild(uint64 childWait, const VmCallResult& result, uint32 depth,
                             uint64 root, bool inDrain) noexcept;
    // 대기가 완료되지 않은 채 끝났다(발행자 파괴·하위 실행 취소 — SD13): Race는 남은 인자를,
    // All·단독은 실행을 취소한다.
    void FailWait(uint32 wait) noexcept;
    // 발행자의 이벤트 대기를 끝낸다(Detach·엔진 객체 소멸).
    void EndEventWaits(uint64 publisher) noexcept;
    // 대기 done 전파(All·Race)와 Race의 나머지 취소(반복 — 깊이 제한 없음).
    void CompleteWait(uint32 wait) noexcept;
    // handle을 번호로 푼다(표지·범위·세대가 맞지 않으면 false).
    bool DecodeWait(uint64 handle, uint32& index) const noexcept;
    // 대기와 그 하위 묶음을 돌려받는다 / 취소 표시한다.
    void FreeWaitTree(uint32 root) noexcept;
    void CancelWaitTree(uint32 root) noexcept;
    uint32 WaitRoot(uint32 index) const noexcept;
    // 실행 하나(Call·Continue)가 끝난 뒤: 이 조각이 만든 대기 중 정지 실행이 기다리지 않는 것을
    // 돌려받는다(기다리지 않은 대기 — 언어는 바로 기다린다, 손상·fault 대비).
    void SettleWaits(const Result<VmCallResult>& called) noexcept;
    // instance의 정지 실행을 취소한다(Disable·Detach — SL 6.1절). frames면 그 instance를 this로
    // 쓰는 frame이 있는 실행도 취소한다(Detach — 다른 instance의 async 메서드 안에서 정지한 실행).
    void CancelExecutions(uint32 instance, bool frames) noexcept;
    // 정지 실행 하나를 취소한다(대기를 돌려받고 frame의 Buf를 놓는다). propagate면 하위 실행의
    // 취소를 부모 묶음에 알린다(FailWait — 묶음을 돌려받는 길에서는 끈다).
    void CancelSuspended(uint32 suspended, bool propagate = true) noexcept;
    // 같은 발행자로 시작된 handler 실행이 정지해 있으면 그 번호(없으면 NoBytecodeIndex).
    uint32 FindBusy(uint32 instance, const VmLinkedModule* module, uint32 handler,
                    uint64 publisher) const noexcept;
    void ReleaseSuspended(Suspended& suspended) noexcept;

    // 연결 하나(SL 5.5절 — 연결 표의 항목)다.
    struct Connection {
        uint64 publisher = 0;
        VmEventKey key;
        uint32 subscriber = NoBytecodeIndex; // instance 번호
        const VmLinkedModule* module = nullptr;
        uint32 handler = NoBytecodeIndex; // 그 모듈 handlers 번호
        bool fromArray = false;           // 배열 원천: 첫 인자가 발행한 원소
        bool active = true;
        uint64 order = 0; // 추가 순번(전달 순서)
    };

    // 발행 큐 항목이다(인자는 values_·buffers_ 저장소의 구간).
    struct Item {
        uint64 publisher = 0;
        VmEventKey key;
        const VmLinkedModule* module = nullptr; // 이벤트 모양의 모듈(없으면 인자 검사 없음)
        uint32 eventRef = NoBytecodeIndex;
        uint32 depth = 1;
        uint64 root = 0;
        uint32 firstScalar = 0;
        uint32 scalarCount = 0;
        uint32 firstRef = 0;
        uint32 refCount = 0;
        uint32 firstBuffer = 0;
        uint32 bufferCount = 0;
        bool carried = false;
        // 경로 원천이 바뀐 `on A.B.Changed` handler 하나에만 전달하는 항목(SL 5.3절 — 다시 연결한
        // 뒤 한 번 전달). target은 instance 번호, serial은 그 instance의 생성 번호(번호 재사용
        // 구별).
        uint32 target = NoBytecodeIndex;
        uint64 targetSerial = 0;
        uint64 sequence = 0; // 넣은 순 일련번호(이벤트 대기는 등록 뒤 항목만 받는다)
        const VmLinkedModule* targetModule = nullptr;
        uint32 targetHandler = NoBytecodeIndex;
    };

    // 조각 안에서 처음 쓴 필드(Changed — SL 5.3절)다.
    struct Write {
        uint32 instance = NoBytecodeIndex;
        const VmLinkedModule* module = nullptr;
        uint32 fieldRef = NoBytecodeIndex;
        uint32 bank = 0;
        uint32 at = 0; // instance 저장소 위치
        uint32 slots = 0;
        uint32 firstValue = 0; // writeValues_ 구간(Scalar·Ref) 또는 writeBuffers_ 번호(Buf)
        // 첫 쓰기에서 확보한 Changed 자리(SL 5.3절 — 활성 instance, 억제 문맥 밖).
        bool reserved = false;
        usize item = 0;
    };

    // EMIT(실행 중)이 부른다: 항목을 넣는다. 반환은 fault 종류.
    VmFaultKind EnqueueFromScript(const VmLinkedModule& module, uint32 eventRef,
                                  Span<const uint64> scalars, Span<const uint64> refs,
                                  Span<VmBuffer* const> buffers, ErrorCode& error) noexcept;
    VmFaultKind Enqueue(uint64 publisher, const VmEventKey& key, const VmLinkedModule* module,
                        uint32 eventRef, uint32 depth, uint64 root, Span<const uint64> scalars,
                        Span<const uint64> refs, Span<VmBuffer* const> buffers,
                        ErrorCode& error) noexcept;
    // 로컬 필드 쓰기를 기록한다(조각에서 처음이면 시작 값 보관과 Changed 자리 확보). 반환은 쓰기
    // 명령의 fault(Changed 자리의 BudgetExceeded·CascadeLimit), 할당 실패는 error.
    VmFaultKind NoteWrite(uint32 instance, const VmLinkedModule& module, uint32 fieldRef,
                          uint32 bank, uint32 at, uint32 slots, ErrorCode& error) noexcept;
    // 조각 끝: Changed 자리 확정·취소, 원천 필드가 바뀐 연결 다시 풀기. 실패는 할당자 오류.
    Result<void> EndFragment() noexcept;
    // instance의 쓰기 기록을 지운다(시작 값을 놓고 확보한 Changed 자리를 취소 — Detach).
    void DropWrites(uint32 instance) noexcept;
    // instance의 field key를 경로 첫 마디로 쓰는 handler 연결을 다시 풀고, `on A.B.Changed`면 한 번
    // 전달할 항목을 넣는다(SL 5.3·5.5절).
    Result<void> Reconnect(uint32 instance, const VmEventKey& field) noexcept;
    // 큐를 비운다(단계 전달 상한까지).
    Result<void> Drain() noexcept;
    // instance의 연결을 (다시) 푼다 — handler 하나(handler가 NoIndex면 전부).
    Result<void> Connect(uint32 instance, const VmLinkedModule* module, uint32 handler) noexcept;
    // 경로를 풀어 발행자 handle들을 publishers에 둔다(배열이면 fromArray).
    Result<void> ResolvePath(uint32 instance, const VmLinkedModule& module, uint32 firstPath,
                             uint32 pathCount, Array<uint64>& publishers, bool& fromArray) noexcept;
    void Disconnect(uint32 instance) noexcept;
    void ReleaseItem(Item& item) noexcept;
    // 진입 하나(RunHook·Call·Emit·OpenPhase)의 앞뒤: 기록 모음·전달.
    void BeginEntry() noexcept;

    struct Instance {
        explicit Instance(Allocator& allocator) noexcept
            : scalars(allocator), refs(allocator), buffers(allocator) {
        }

        uint64 handle = 0;
        const VmLinkedModule* module = nullptr;
        uint32 classIndex = NoBytecodeIndex;
        bool attached = false;
        bool enabled = false;        // Enable hook이 끝났고 Disable 전(연결이 살아 있다)
        uint64 serial = 0;           // 생성 번호(session 안에서 유일 — 번호 재사용 구별)
        bool pendingDestroy = false; // 파괴 적용됨(새 전달·admission 없음 — 프레임 끝에 정리)
        // fault 처리(LC6d3).
        bool isolated = false;
        bool ticksStopped = false;
        uint64 tickStopGeneration = 0; // Tick 중지 때 모듈 링크 세대(바뀌면 푼다)
        bool enabledOnce = false;      // 첫 Enable(spawn)이 끝났다 — 뒤 Enable은 재활성화
        uint64 faultFrames[IsolationFaults] = {};
        uint32 faultCount = 0;
        Array<uint64> scalars;
        Array<uint64> refs;
        Array<VmBuffer*> buffers;
    };

    // instance의 클래스 사슬(파생 → 기반)을 모은다. 반환은 사슬 길이(MaxBaseDepth 이하).
    static constexpr uint32 MaxBaseDepth = 64;
    uint32 Chain(uint32 instance, const VmLinkedModule* (&modules)[MaxBaseDepth],
                 uint32 (&classes)[MaxBaseDepth]) const noexcept;
    // 기반 클래스(같은 모듈, 또는 `<경로>:Class` — session 모듈)를 찾는다. 엔진 기반·없음은 false.
    bool BaseOf(const VmLinkedModule& module, uint32 classIndex, const VmLinkedModule*& baseModule,
                uint32& baseClass) const noexcept;
    // GETF·SETF의 로컬 필드 자리다(instance가 아니면 false).
    // instance·at은 instance 번호와 저장소 위치(Changed 기록)다.
    bool FieldSlots(uint64 handle, const VmLinkedModule& module, uint32 classIndex, uint32 bank,
                    uint32 offset, uint32 slots, uint64*& values, VmBuffer**& buffers,
                    uint32& instance, uint32& at) noexcept;
    void ReleaseFields(Instance& instance) noexcept;
    // instance의 유지 자원 필드를 선언 역순(파생 → 기반)으로 멈추고 null로 둔다(Disable 뒤·Detach —
    // SparksRuntime 2.6절).
    void ReleaseHeldFields(uint32 instance) noexcept;

    Allocator* allocator_;
    VmHost* host_;
    VmExecution execution_;
    Array<const VmLinkedModule*> modules_;
    Array<Instance> instances_;
    HashMap<uint64, uint32> byHandle_;
    // 발행·전달(LC6b).
    Array<Connection> connections_;
    Array<Item> queue_;
    usize queueHead_ = 0;
    Array<uint64> itemValues_;
    Array<VmBuffer*> itemBuffers_;
    Array<Write> writes_;
    Array<uint64> writeValues_;
    Array<VmBuffer*> writeBuffers_;
    Array<Delivery> deliveries_;
    uint64 nextOrder_ = 1;
    uint64 nextRoot_ = 1;
    uint32 currentDepth_ = 0; // 실행 중인 조각의 깊이(0은 단계를 시작한 조각)
    uint64 currentRoot_ = 0;
    uint32 deliveredInPhase_ = 0;
    bool suppressChanged_ = false; // 초기화·Awake·Enable·Disable·Destroy 문맥
    // async(LC6c).
    Array<Suspended> suspended_;
    Array<Wait> waits_;
    Array<uint32> freeWaits_;     // 돌려받은 대기 번호
    Array<uint64> fragmentWaits_; // 실행 중인 조각이 만든 대기 handle
    Array<uint32> resumeOrder_;   // 재개 단계의 순서(임시)
    Array<uint64> resumeWaits_;   // resumeOrder_ 항목이 기다리던 대기(재개 전 대조)
    uint64 currentChildWait_ = 0; // 재개 중인 하위 실행의 하위 대기(다시 정지하면 잇는다)
    uint64 itemSequence_ = 0;     // 큐 항목 일련번호(이벤트 대기 대조)
    double gameTime_ = 0.0;
    double uiTime_ = 0.0;
    uint64 frame_ = 0;
    uint64 fixedStep_ = 0;
    uint64 nextWaitOrder_ = 1;
    uint32 currentInstance_ = NoBytecodeIndex; // 실행 중인 조각의 instance(정지 실행의 주인)
    // 실행 중인 handler 조각의 발행자·handler(정지 실행에 남긴다).
    uint64 currentPublisher_ = 0;
    const VmLinkedModule* currentHandlerModule_ = nullptr;
    uint32 currentHandler_ = NoBytecodeIndex;
    uint64 skippedWhileBusy_ = 0;
    uint64 nextSerial_ = 1;
    // 지연 적용(LC6d2).
    enum class RequestKind : uint8 {
        Enable,
        Disable,
        Destroy
    };

    struct Request {
        uint32 instance = NoBytecodeIndex;
        uint64 serial = 0;
        RequestKind kind = RequestKind::Enable;
    };

    Array<Request> requests_;
    Array<Request> applying_;
    Array<Request> destroys_;
    bool closed_ = false;
    // fault 처리(LC6d3).
    Array<FaultRecord> faults_;
    uint64 activeFrame_ = 0; // 일시 정지가 아닌 프레임 수(격리 창의 기준)
    // fault를 위치별로 기록하고, counts면 instance의 격리 창에 센다(세 번째면 격리).
    void NoteFault(uint32 instance, const VmCallResult& result, bool counts) noexcept;
    void RecordFault(const VmFault& fault) noexcept;
    // 격리(SL 8.3절): Disable hook 없이 실행 취소 → 유지 자원 필드 → 연결·이벤트 대기 폐기. 값
    // 필드는 둔다.
    void Isolate(uint32 instance) noexcept;
    bool nested_ = false; // 적용 루프 안(진입 기록을 이어 모은다)
    // 수명 hook 하나를 적용 기록으로 돌린다.
    Result<void> ApplyHook(uint32 instance, BytecodeHookKind kind) noexcept;
};

} // namespace rocks::sparks
