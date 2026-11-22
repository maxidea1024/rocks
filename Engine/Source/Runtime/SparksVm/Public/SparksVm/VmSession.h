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

namespace rocks::sparks {

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
    // 있다).
    void Detach(uint32 instance) noexcept;

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
    // 인자는 Scalar·Ref·Buf별(Buf는 빌려 준다 — 항목이 참조를 더한다).
    Result<void> Emit(uint64 publisher, const VmEventKey& key, Span<const uint64> scalars,
                      Span<const uint64> refs, Span<VmBuffer* const> buffers) noexcept;

    // 마지막 진입(RunHook·Call·Emit·OpenPhase)에서 실행된 handler 기록이다(다음 진입이 지운다).
    struct Delivery {
        uint64 subscriber = 0;
        const VmLinkedModule* module = nullptr;
        uint32 function = NoBytecodeIndex;
        VmCallResult result;
    };

    Span<const Delivery> Deliveries() const noexcept {
        return deliveries_.View();
    }

    // 큐에 남은(이월) 항목 수다.
    uint32 QueuedCount() const noexcept;

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
    };

    // EMIT(실행 중)이 부른다: 항목을 넣는다. 반환은 fault 종류.
    VmFaultKind EnqueueFromScript(const VmLinkedModule& module, uint32 eventRef,
                                  Span<const uint64> scalars, Span<const uint64> refs,
                                  Span<VmBuffer* const> buffers, ErrorCode& error) noexcept;
    VmFaultKind Enqueue(uint64 publisher, const VmEventKey& key, const VmLinkedModule* module,
                        uint32 eventRef, uint32 depth, uint64 root, Span<const uint64> scalars,
                        Span<const uint64> refs, Span<VmBuffer* const> buffers,
                        ErrorCode& error) noexcept;
    // 로컬 필드 쓰기를 기록한다(조각에서 처음이면 시작 값 보관).
    void NoteWrite(uint32 instance, const VmLinkedModule& module, uint32 fieldRef, uint32 bank,
                   uint32 at, uint32 slots) noexcept;
    // 조각 끝: Changed 확정, 원천 필드가 바뀐 연결 다시 풀기. 실패는 할당자 오류.
    Result<void> EndFragment() noexcept;
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
        bool enabled = false; // Enable hook이 끝났고 Disable 전(연결이 살아 있다)
        Array<uint64> scalars;
        Array<uint64> refs;
        Array<VmBuffer*> buffers;
    };

    // 기반 클래스(같은 모듈, 또는 `<경로>:Class` — session 모듈)를 찾는다. 엔진 기반·없음은 false.
    bool BaseOf(const VmLinkedModule& module, uint32 classIndex, const VmLinkedModule*& baseModule,
                uint32& baseClass) const noexcept;
    // GETF·SETF의 로컬 필드 자리다(instance가 아니면 false).
    // instance·at은 instance 번호와 저장소 위치(Changed 기록)다.
    bool FieldSlots(uint64 handle, const VmLinkedModule& module, uint32 classIndex, uint32 bank,
                    uint32 offset, uint32 slots, uint64*& values, VmBuffer**& buffers,
                    uint32& instance, uint32& at) noexcept;
    void ReleaseFields(Instance& instance) noexcept;

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
};

} // namespace rocks::sparks
