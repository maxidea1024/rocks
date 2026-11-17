#pragma once

#include <Core/Function.h>
#include <Core/Memory/FrameArena.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>
#include <Graphics/Device.h>
#include <RenderBase/RenderHandle.h>
#include <RenderBase/RenderNotices.h>
#include <Task/DedicatedLane.h>

// 렌더 lane이다(Docs/Design/RenderBase.ko.md 5절 RB1, UE 렌더 스레드 대응). 전용 스레드 하나(CP1
// DedicatedLane)가 렌더 일을 직렬 FIFO로 처리하고, 게임 lane은 등록된 값 묶음(프레임 packet)과 렌더
// handle만 넘긴다. 게임 lane이 막히는 곳은 프레임 slot 확보뿐이다(결정 R3).
namespace rocks::render {

enum class RenderLaneMode : uint8 {
    // 전용 스레드가 렌더 작업을 처리한다. 장치 소유권을 그 스레드로 이관한다.
    Thread,
    // 게임 lane 스레드가 큐 순서대로 처리한다(헤드리스·시험). 장치 소유권은 옮기지 않는다. 작업
    // 순서와 장치 호출 열은 Thread 모드와 같다.
    Inline
};

// 전체 비우기(RenderLane::Drain)를 부른 이유다(5.3절). 계측에 남긴다. 일반 자원 해제는 Retire로
// 하고 비우기를 쓰지 않는다.
enum class RenderDrainReason : uint8 {
    Shutdown,
    DeviceRecreate,
    Test
};

inline constexpr uint32 RenderDrainReasonCount = 3;

// 프레임 slot 수의 범위다(계획 8.2절 in-flight 2–3).
inline constexpr uint32 MinFrameSlots = 2;
inline constexpr uint32 MaxFrameSlots = 3;
inline constexpr uint32 DefaultFrameSlots = 2;

// 메시지 펌프 hook이 있을 때 대기 중 펌프 간격이다(millisecond, UE 대기 루프와 같은 1 ms 단위).
// 완료 판정에 쓰지 않는다(완료는 이벤트로 깨운다).
inline constexpr uint32 FrameWaitPumpIntervalMs = 1;

// 대기 진단 상한의 기본값이다(UE 120초 hang 판정과 같다). 진단 전용이며 0이면 끈다.
inline constexpr uint32 DefaultHangTimeoutMs = 120000;

class RenderLane;
class RenderLaneContext;
class RenderLaneProcessor;

// 렌더 lane 작업 안에서만 쓰는 lane 쪽 문맥이다. 장치는 소유 스레드(렌더 lane) 전용 호출을 포함해
// 모두 쓸 수 있다. 문맥은 작업 호출 동안만 유효하며 저장하지 않는다.
class RenderLaneContext {
public:
    graphics::Device& Device() const noexcept;

    RenderHandleTable& Handles() const noexcept;

    // RenderLaneConfig::processor다(없으면 nullptr). lane 쪽 객체이므로 작업 함수가 자기 타입으로
    // 바꿔 쓴다.
    RenderLaneProcessor* Processor() const noexcept;

    // 렌더 lane 자신이다. lane 작업 안에서 후속 작업을 게시할 때 쓴다(재진입 게시는 큐 맨 뒤에
    // 붙는다).
    RenderLane& Lane() const noexcept {
        return lane_;
    }

    // 게임 lane에 결과를 알린다.
    void Notify(const RenderNotice& notice) const noexcept;

    // 렌더 lane의 치명 오류를 기록한다. 다음 프레임 slot 확보가 이 오류로 실패하고 LaneFailed
    // 통지가 간다(5.6절).
    void ReportFailure(ErrorCode error) const noexcept;

protected:
    explicit RenderLaneContext(RenderLane& lane) noexcept : lane_(lane) {
    }

    RenderLane& lane_;
    friend class RenderLane;
};

// 프레임 packet 작업의 문맥이다.
class RenderFrameContext : public RenderLaneContext {
public:
    // 게임 lane이 BeginFrame에서 받은 프레임 번호(1부터)다.
    uint64 FrameNumber() const noexcept {
        return frame_;
    }

    // 프레임 slot 번호다(0부터 slot 수 미만).
    uint32 SlotIndex() const noexcept {
        return slot_;
    }

private:
    RenderFrameContext(RenderLane& lane, uint32 slot, uint64 frame) noexcept
        : RenderLaneContext(lane), slot_(slot), frame_(frame) {
    }

    uint32 slot_;
    uint64 frame_;
    friend class RenderLane;
};

// 렌더 lane 쪽에만 사는 처리기다(예: Renderer를 감싼 객체). RenderLane이 lane 작업 안에서 부른다.
// 게임 lane은 이 객체를 만지지 않는다.
class RenderLaneProcessor {
public:
    // 장치 소유권이 lane으로 넘어온 뒤 lane에서 한 번 부른다(5.5절 시작 순서: 스왑체인 생성 →
    // 엔진 셰이더 집합 → 내장 자원 팩 생성·업로드). 반환 뒤 RenderLane이 그때까지의 제출 fence를
    // 기다린 다음 준비 완료(BuiltinReady)를 게시한다. 실패하면 프레임 slot 확보가 이 오류로
    // 실패한다.
    virtual Result<void> OnLaneStart(RenderLaneContext& context) noexcept {
        (void)context;
        return Result<void>::Success();
    }

    // 종료 때 lane에서 한 번 부른다(5.5절 종료 4·5단계: 엔진 셰이더 집합·내장 자원 Retire,
    // 파이프라인 캐시 비우기, 스왑체인 Retire). 반환 뒤 RenderLane이 회수 완료를 기다린다.
    virtual void OnLaneStop(RenderLaneContext& context) noexcept {
        (void)context;
    }

protected:
    ~RenderLaneProcessor() = default;
};

// 게임 lane이 slot 확보·handshake를 기다리는 동안 OS 메시지를 펌프하는 hook이다(5.3절). Engine과
// RenderBase는 창을 모르므로 실행 파일이 주입한다. pump는 기다리는 스레드(게임 lane)에서 불리며
// 렌더 lane API를 부르지 않는다(완료 통지 수거는 된다).
struct RenderLanePump {
    void (*pump)(void* context) noexcept = nullptr;
    void* context = nullptr;
};

struct RenderLaneConfig {
    RenderLaneMode mode = RenderLaneMode::Thread;
    // MinFrameSlots–MaxFrameSlots다.
    uint32 frameSlots = DefaultFrameSlots;
    // Thread 모드의 스레드 옵션이다. 이름이 비어 있으면 "RenderLane"이다.
    platform::ThreadOptions thread;
    RenderLanePump pump;
    // 프레임 slot마다 하나씩 만드는 프레임 arena 설정이다.
    FrameArenaConfig frameArena;
    // 대기 진단 상한이다(위 DefaultHangTimeoutMs). 넘으면 hang 계수를 올리고 그 대기가 실패한다.
    uint32 hangTimeoutMs = DefaultHangTimeoutMs;
    // lane 쪽 처리기다(선택). RenderLane보다 오래 살아야 한다.
    RenderLaneProcessor* processor = nullptr;
    // 시험용 lane 스레드 시작 경계다(DedicatedLaneConfig::launch).
    DedicatedLaneLauncher launch = nullptr;
    void* launchContext = nullptr;
};

// 렌더 lane 표지다(UE FRenderCommandFence 대응). PostMarker가 돌려주고, 표지 앞에 게시한 모든 lane
// 작업이 끝나면 도달한다.
struct LaneMarker {
    uint64 sequence = 0;
};

// 게임 lane이 확보한 프레임 slot이다. SubmitFrame으로 넘기거나 AbandonFrame으로 버린다. 이동
// 전용이다.
class FrameTicket {
public:
    FrameTicket() noexcept = default;
    FrameTicket(FrameTicket&& other) noexcept;
    FrameTicket& operator=(FrameTicket&& other) noexcept;
    FrameTicket(const FrameTicket&) = delete;
    FrameTicket& operator=(const FrameTicket&) = delete;

    bool IsValid() const noexcept {
        return frame_ != 0;
    }

    uint64 FrameNumber() const noexcept {
        return frame_;
    }

    uint32 SlotIndex() const noexcept {
        return slot_;
    }

private:
    FrameTicket(uint32 slot, uint64 frame) noexcept : slot_(slot), frame_(frame) {
    }

    uint32 slot_ = 0;
    uint64 frame_ = 0;
    friend class RenderLane;
};

struct RenderLaneStats {
    uint64 framesSubmitted = 0;
    // lane 처리가 끝난 프레임 수다(lane 처리 완료 표지).
    uint64 framesProcessed = 0;
    // GPU까지 끝난 프레임 수다(GPU 완료 표지).
    uint64 framesCompleted = 0;
    // BeginFrame이 빈 slot이 없어 기다린 횟수다.
    uint64 slotWaits = 0;
    // 대기 중 메시지 펌프를 부른 횟수다.
    uint64 pumpCalls = 0;
    uint64 hangReports = 0;
    uint64 drains[RenderDrainReasonCount] = {};
    // Retire를 받았고 아직 GPU 완료·회수를 마치지 않은 handle 수다.
    uint64 retiresPending = 0;
    uint64 retiresCompleted = 0;
    // CloseResources가 본 남은 렌더 handle 수와 장치 회수 대기 수다(closure 진단).
    uint64 liveHandles = 0;
    uint64 devicePendingRetires = 0;
    // 게시한 lane 작업 수와 끝난 작업 수다(DedicatedLaneStats).
    uint64 tasksPosted = 0;
    uint64 tasksCompleted = 0;
    // 장치 소유권을 lane으로 넘긴 뒤 되돌려 받았다(Thread 모드 종료 확인).
    bool ownershipReturned = false;
    bool running = false;
};

// 렌더 lane 작업 함수다. 캡처가 없는 함수만 받는다(게임 주소를 캡처로 넘기지 못하게 한다).
template <class T>
using RenderLaneFunction = void (*)(RenderLaneContext& context, T& value) noexcept;

template <class T>
using RenderFrameFunction = void (*)(RenderFrameContext& context, T& packet) noexcept;

// 렌더 lane이다.
//
// 사용 예(게임 lane):
//   RenderLane lane(allocator, device);
//   RenderLaneConfig config;
//   config.processor = &sceneRenderer;            // lane 쪽 객체
//   config.pump = {&PumpWindow, &window};         // 창 스레드면 대기 중 메시지 펌프
//   lane.Start(config);                           // 장치 소유권을 lane으로 넘긴다
//   for (;;) {
//       auto ticket = lane.BeginFrame();          // 빈 slot이 없으면 가장 오래된 slot의 GPU 완료를
//       기다린다 if (!ticket) { break; }                   // 장치 손실·lane 실패 auto items =
//       lane.Arena(ticket.Value()).TryNewArray<RenderItem>(count);
//       ...                                       // World에서 추출해 arena에 담는다
//       lane.SubmitFrame(Move(ticket.Value()), ScenePacket{view, items.Value()}, &DrawScene);
//       lane.Notices().TakeAll(notices);          // 완료 통지 수거
//   }
//   lane.Shutdown();                              // 5.5절 종료 순서 전체(또는 단계별 함수)
//
// 작업 네 종류(5.2절): 프레임 packet(SubmitFrame), 장치 자원 생성·갱신(PostResource)·Retire, 표면
// 작업(PostSurface·PostSurfaceAndWait), lane 표지(PostMarker). 값은 RenderTransferable만 받는다.
//
// 스레드:
// - Start·BeginFrame·Arena·SubmitFrame·AbandonFrame·대기 함수·종료 단계 함수는 Start를 부른
//   스레드(게임 lane) 전용이다. 다른 스레드는 WrongThread다.
// - ReserveHandle·PostResource·Retire·PostSurface·PostMarker·IsReached·Stats·Notices는 어느
//   스레드에서나 부를 수 있다(렌더 lane 작업 안 포함).
// - 장치는 Start 뒤로 렌더 lane이 소유한다(Thread 모드). 게임 lane은 스레드 안전 fence API만 쓴다.
//
// 수명: allocator·device·processor는 RenderLane보다 오래 산다. 실행 중(Start 뒤 Stop 전)에 파괴하면
// Fatal이다. 장치 파괴는 Stop(소유권 반납) 뒤 게임 lane이 한다.
class RenderLane {
public:
    RenderLane(Allocator& allocator, graphics::Device& device) noexcept;
    ~RenderLane() noexcept;

    RenderLane(const RenderLane&) = delete;
    RenderLane& operator=(const RenderLane&) = delete;

    // lane을 시작한다(5.5절 시작 순서). Thread 모드는 스레드를 만들고 장치 소유권을 넘긴 뒤,
    // lane에서 소유권 획득 → processor.OnLaneStart → 제출 fence 대기 → 준비 완료 게시를 실행한다.
    // 이 함수는 그 완료를 기다리지 않는다(첫 BeginFrame이 기다린다). 실패: 이미 시작 Busy, 잘못된
    // 설정 InvalidArgument, 호출 스레드가 장치 소유자가 아니면 WrongThread, lane 시작 실패는 그
    // 오류다. 실패하면 아무것도 바꾸지 않는다.
    Result<void> Start(const RenderLaneConfig& config) noexcept;

    // --- 프레임(게임 lane) ---

    // 다음 프레임 slot을 확보한다. 빈 slot이 없으면 가장 오래된 slot의 GPU 완료를 기다린다(펌프
    // hook이 있으면 메시지를 펌프하며). 확보한 slot의 프레임 arena는 비워져 있다. 실패: 시작
    // 전·종료 단계 ShuttingDown, 이전 ticket을 아직 넘기지 않음 Busy, 시작(OnLaneStart) 실패·장치
    // 손실 (Destroyed)·lane 실패는 그 오류, 대기 진단 상한 초과 DependencyFailed.
    Result<FrameTicket> BeginFrame() noexcept;

    // ticket slot의 프레임 arena다. 프레임 packet의 가변 길이 목록을 여기에 둔다. 렌더 lane이 그
    // 프레임을 처리할 때까지 유효하다. ticket이 유효하지 않으면 Fatal이다.
    FrameArena& Arena(const FrameTicket& ticket) noexcept;

    // 프레임 packet을 렌더 lane에 넘긴다. lane이 render(context, packet)를 부르고, 그 안의 장치
    // 제출 fence가 이 slot의 GPU 완료 표지가 된다. 성공하면 ticket을 소비한다. 실패: 잘못된 ticket
    // InvalidArgument, 종료 단계 ShuttingDown, Development 전달 검사 InvalidArgument, 게시 실패는
    // 그 오류(ticket은 그대로 남아 AbandonFrame할 수 있다). packet은 실패해도 소비된다.
    template <RenderTransferable T>
    Result<void> SubmitFrame(FrameTicket&& ticket, T&& packet,
                             RenderFrameFunction<T> render) noexcept;

    // 확보한 slot을 쓰지 않고 돌려준다.
    void AbandonFrame(FrameTicket&& ticket) noexcept;

    // --- 자원·표면·표지(어느 스레드에서나) ---

    // 렌더 handle을 예약한다(5.4절). 장치 자원은 PostResource 작업이 만들어 잇는다.
    Result<RenderHandle> ReserveHandle(RenderResourceKind kind) noexcept;

    // 장치 자원 생성·갱신 작업을 게시한다. 실패: 종료 뒤 ShuttingDown, 전달 검사·빈 함수
    // InvalidArgument, 게시 실패는 그 오류다. value는 실패해도 소비된다.
    template <RenderTransferable T>
    Result<void> PostResource(T&& value, RenderLaneFunction<T> run) noexcept;

    // 렌더 handle을 회수한다(6.2절 Retire). lane이 새 사용을 막고, 이어진 장치 자원을 장치 Retire로
    // 넘긴 뒤 그 시점까지의 제출 fence가 끝나면 장치에서 해제하고 ResourceRetired를 통지한다. 실패:
    // 모르는 handle StaleHandle, 게시 실패는 그 오류다(이미 Retire한 handle의 중복 요청은 lane이
    // 무시한다).
    Result<void> Retire(RenderHandle handle) noexcept;

    // 장치 자원 내용 갱신 작업이다(6.2절 "갱신", GX8 업로드 ring). lane이 handle의 장치 자원에
    // 업로드를 기록하고, 다음 제출 앞에서 실행된다. 같은 프레임 안의 갱신 순서는 lane FIFO가
    // 지킨다. Ready가 아닌 handle이면 그 갱신은 버리고 LaneFailed 통지를 하지
    // 않는다(ResourceFailed가 이미 갔거나 Retire 중이다). 실패: 모르는 handle StaleHandle, 게시
    // 실패는 그 오류다.
    Result<void> UpdateBuffer(RenderHandle buffer, uint64 offset, RenderBlob&& data) noexcept;
    Result<void> UpdateTexture(RenderHandle texture, uint32 mipLevel, RenderBlob&& pixels) noexcept;

    // 렌더 타깃 텍스처의 비동기 readback을 요청하고 번호를 돌려준다(GX8). lane이 지금까지의 그리기
    // 뒤에 복사를 제출하고, 그 fence가 끝나면 결과를 보관하고 ReadbackReady 통지(frame = 번호)를
    // 보낸다. 게임 lane은 TakeReadback으로 결과를 가져간다. 실패: 모르는 handle StaleHandle, 게시
    // 실패는 그 오류다.
    Result<uint64> RequestReadback(RenderHandle texture) noexcept;

    // 끝난 readback 결과를 가져간다(한 번). 실패: 아직 없거나 모르는 번호 NotFound.
    Result<RenderBlob> TakeReadback(uint64 readback) noexcept;

    // 표면 작업(스왑체인 생성·resize·파괴, 표면 소실·재생성)을 게시한다. 장치 소유권 이관 중에
    // 게시해도 lane이 소유권을 얻은 뒤에 실행된다.
    template <RenderTransferable T>
    Result<void> PostSurface(T&& value, RenderLaneFunction<T> run) noexcept;

    // 표면 소실 handshake다(5.3절 예외 1: Android surface 반납). 표면 작업을 게시하고 lane이 그것을
    // 마칠 때까지 게임 lane이 기다린다(펌프하며). 게임 lane 전용이다.
    template <RenderTransferable T>
    Result<void> PostSurfaceAndWait(T&& value, RenderLaneFunction<T> run) noexcept;

    // lane 표지를 게시한다.
    Result<LaneMarker> PostMarker() noexcept;

    bool IsReached(LaneMarker marker) const noexcept;

    // 표지에 도달할 때까지 기다린다(펌프하며, 게임 lane 전용). 실패: 대기 진단 상한 초과
    // DependencyFailed, 다른 스레드 WrongThread.
    Result<void> WaitMarker(LaneMarker marker) noexcept;

    // 전체 비우기다(5.3절 예외 2: 종료·장치 재생성·시험 전용). 지금까지 게시한 lane 작업을 모두
    // 끝낼 때까지 기다리고 이유를 계측에 남긴다. 게임 lane 전용이다.
    Result<void> Drain(RenderDrainReason reason) noexcept;

    RenderNotices& Notices() noexcept {
        return notices_;
    }

    RenderHandleTable& Handles() noexcept {
        return handles_;
    }

    // 렌더 lane 작업 안이면 true다(Inline 모드 포함, DedicatedLane::IsExecutingTask).
    bool IsExecutingLaneTask() const noexcept;

    // 기록된 lane 실패다(장치 손실이면 Destroyed). 없으면 None이다.
    ErrorCode Failure() const noexcept;

    // 시작 작업(OnLaneStart·fence)이 끝나 준비 완료(BuiltinReady)를 게시했는지다.
    bool IsReady() const noexcept;

    RenderLaneStats Stats() const noexcept;

    // --- 종료 단계(5.5절, 게임 lane 전용, CP6 hook이 차례로 부른다) ---

    // 1단계: 새 프레임 packet을 막는다. 이후 BeginFrame은 ShuttingDown이다.
    void BlockFrames() noexcept;

    // 2단계: 모든 프레임 slot의 lane 처리와 GPU 완료를 기다린다(펌프하며).
    Result<void> WaitFramesIdle() noexcept;

    // 3단계(반복): 게시된 Retire를 모두 lane에서 처리하고 그 GPU 완료·회수를 기다린다. 이번
    // 호출에서 끝난 Retire 수를 돌려준다. 호출자는 최종 GC와 이 함수를 반환값과 GC 회수가 모두 0이
    // 될 때까지 반복한다(closure).
    Result<uint64> DrainRetires() noexcept;

    // 4·5단계와 closure 확인: processor.OnLaneStop(내장 자원·스왑체인 Retire), 장치 WaitIdle·회수를
    // lane에서 실행하고, 남은 렌더 handle 0·Retire 대기 0·장치 회수 대기 0을 확인한다. 남은 것이
    // 있으면 Busy다(뒤 단계는 호출자가 계속 부른다).
    Result<void> CloseResources() noexcept;

    // 6·7단계: lane Drain(남은 작업 0 확인) → 장치 소유권을 게임 lane으로 반납 → lane Join. 남은
    // 작업이 있었으면 Busy를 돌려주되 lane은 닫는다.
    Result<void> Stop() noexcept;

    // 위 단계를 차례로 모두 실행한다. 실패한 단계가 있어도 뒤 단계를 건너뛰지 않고 첫 오류를
    // 돌려준다. 시작 전이면 성공이다.
    Result<void> Shutdown() noexcept;

private:
    struct RetireWait;
    struct ReadbackWait;

    // lane이 보관하는 readback 진행·결과다(readbackGate_ 보호).
    struct ReadbackEntry {
        uint64 id = 0;
        graphics::ReadbackHandle device;
        uint64 bytes = 0;
        RenderHandle texture;
        RenderBlob result;
        bool ready = false;
    };
    friend class RenderLaneContext;

    enum class SlotPhase : uint8 {
        // 비어 있다(GPU 완료 표지까지 끝났다).
        Free,
        // 게임 lane이 확보해 packet을 만드는 중이다.
        Recording,
        // 렌더 lane 큐에 있다.
        Queued,
        // lane 처리 완료 표지가 켜졌고 GPU 완료를 기다린다.
        Processed
    };

    enum class InboxKind : uint8 {
        FrameCompleted,
        RetireCompleted,
        ReadbackCompleted
    };

    // fence 완료 통지가 lane에 넘기는 항목이다.
    struct InboxItem {
        InboxKind kind = InboxKind::FrameCompleted;
        ErrorCode error = ErrorCode::None;
        uint32 slot = 0;
        uint64 frame = 0;
        RenderHandle handle;
    };

    struct SlotRecord {
        SlotPhase phase = SlotPhase::Free;
        uint64 frame = 0;
        ErrorCode error = ErrorCode::None;
    };

    // slot마다 하나인 fence 통지 문맥이다(통지가 끝나야 그 slot이 다시 비므로 겹치지 않는다).
    struct SlotFenceContext {
        RenderLane* lane = nullptr;
        uint32 slot = 0;
        uint64 frame = 0;
    };

    template <class Body>
    Result<void> PostLaneTask(Body&& body) noexcept;

    bool IsGameThread() const noexcept;
    void RecordFailure(ErrorCode error) noexcept;
    // 펌프하며 ready()(gate_ 안에서 부른다)가 참이 될 때까지 기다린다. Inline 모드면 기다리는 동안
    // fence 완료 통지를 lane 작업으로 처리한다. 게임 lane 전용이다.
    template <class Ready>
    Result<void> WaitUntil(Ready&& ready) noexcept;
    // 쌓인 fence 완료 통지를 처리한다: 장치 회수 → 렌더 통지 게시. 장치를 가진 실행 흐름에서만
    // 한다.
    void ProcessInbox() noexcept;
    void PushInbox(const InboxItem& item) noexcept;
    void RunStartup() noexcept;
    // SubmitFrame이 게시 전에 slot을 Queued로 바꾼다(Inline 모드는 게시 안에서 처리까지 끝난다).
    bool BeginSubmit(uint32 slot, uint64 frame) noexcept;
    void CancelSubmit(uint32 slot) noexcept;
    void FinishFrame(uint32 slot, uint64 frame, graphics::GpuFencePoint before) noexcept;
    void RunRetire(RenderHandle handle) noexcept;
    void RunReadback(uint64 id, RenderHandle texture) noexcept;
    void FinishReadback(uint64 id, ErrorCode error) noexcept;
    // GPU가 끝낸 프레임의 timestamp 결과를 장치에서 꺼내 GpuTimestamp 통지로 넣는다(lane 작업).
    void PublishTimestamps() noexcept;
    static void OnReadbackFence(ErrorCode result, void* context) noexcept;
    static void OnSlotFence(ErrorCode result, void* context) noexcept;
    static void OnRetireFence(ErrorCode result, void* context) noexcept;

    Allocator& allocator_;
    graphics::Device& device_;
    DedicatedLane lane_;
    RenderHandleTable handles_;
    RenderNotices notices_;
    RenderLaneConfig config_;
    char threadName_[platform::MaxThreadNameBytes + 1] = {};
    uint64 gameThread_ = 0;
    UniquePtr<FrameArena> arenas_[MaxFrameSlots];
    SlotFenceContext slotFence_[MaxFrameSlots];
    // readback 보관함이다. lane이 채우고 게임 lane이 가져간다.
    platform::Mutex readbackGate_;
    Array<ReadbackEntry> readbacks_;
    uint64 nextReadback_ = 1;
    // fence 완료 통지가 넣고 lane 작업이 처리한다. inboxGate_가 보호한다(gate_보다 먼저 잡지
    // 않는다).
    platform::Mutex inboxGate_;
    Array<InboxItem> inbox_;
    bool wakePending_ = false;
    // 아래는 gate_가 보호한다.
    mutable platform::Mutex gate_;
    mutable platform::ConditionVariable changed_;
    SlotRecord slots_[MaxFrameSlots];
    uint64 nextFrame_ = 1;
    uint64 markerPosted_ = 0;
    uint64 markerReached_ = 0;
    // 등록했지만 아직 끝나지 않은 장치 fence 통지 수다. Stop은 0이 될 때까지 기다린다.
    uint64 outstandingNotifies_ = 0;
    usize closeDevicePending_ = 0;
    RenderLaneStats stats_;
    ErrorCode failure_ = ErrorCode::None;
    ErrorCode startupError_ = ErrorCode::None;
    ErrorCode closeError_ = ErrorCode::None;
    bool running_ = false;
    bool startupDone_ = false;
    bool ready_ = false;
    bool framesBlocked_ = false;
    bool ticketOut_ = false;
    // 아래는 lane 실행 흐름(또는 lane이 없을 때 게임 lane)만 쓴다.
    bool ownershipMoved_ = false;
    bool laneOwnsDevice_ = false;
};

template <class Body>
Result<void> RenderLane::PostLaneTask(Body&& body) noexcept {
    auto task = Function<void()>::TryCreate(allocator_, Forward<Body>(body));
    if (!task) {
        return Result<void>::Failure(task.Error());
    }

    return lane_.Post(Move(task.Value()));
}

template <RenderTransferable T>
Result<void> RenderLane::SubmitFrame(FrameTicket&& ticket, T&& packet,
                                     RenderFrameFunction<T> render) noexcept {
    auto checked = CheckRenderTransfer<T>();
    if (!checked || render == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!IsGameThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    const uint32 slot = ticket.slot_;
    const uint64 frame = ticket.frame_;
    {
        platform::LockGuard lock(gate_);
        if (framesBlocked_) {
            return Result<void>::Failure(ErrorCode::ShuttingDown);
        }
    }

    if (!ticket.IsValid() || !BeginSubmit(slot, frame)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto posted =
        PostLaneTask([this, slot, frame, render, value = Move(packet)]() mutable noexcept {
            ProcessInbox();
            const graphics::GpuFencePoint before = device_.LastSubmittedFence();
            RenderFrameContext context(*this, slot, frame);
            render(context, value);
            FinishFrame(slot, frame, before);
        });
    if (!posted) {
        CancelSubmit(slot);
        return posted;
    }

    ticket.frame_ = 0;
    return Result<void>::Success();
}

template <RenderTransferable T>
Result<void> RenderLane::PostResource(T&& value, RenderLaneFunction<T> run) noexcept {
    auto checked = CheckRenderTransfer<T>();
    if (!checked || run == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return PostLaneTask([this, run, held = Move(value)]() mutable noexcept {
        ProcessInbox();
        RenderLaneContext context(*this);
        run(context, held);
    });
}

template <RenderTransferable T>
Result<void> RenderLane::PostSurface(T&& value, RenderLaneFunction<T> run) noexcept {
    return PostResource(Move(value), run);
}

template <RenderTransferable T>
Result<void> RenderLane::PostSurfaceAndWait(T&& value, RenderLaneFunction<T> run) noexcept {
    if (!IsGameThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    auto posted = PostSurface(Move(value), run);
    if (!posted) {
        return posted;
    }

    auto marker = PostMarker();
    if (!marker) {
        return Result<void>::Failure(marker.Error());
    }

    return WaitMarker(marker.Value());
}

} // namespace rocks::render
