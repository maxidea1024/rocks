#pragma once

#include <Audio/AudioStreamDecoder.h>
#include <Audio/AudioStreamRing.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/BoundedSpscQueue.h>
#include <Core/Memory/UniquePtr.h>
#include <Platform/Sync.h>
#include <Platform/Sync/RealtimeWakeSignal.h>
#include <Platform/Thread.h>

// 스트림 디코드 서비스다(Docs/Design/AudioSystem 3.40.2절). 고정 개수의 스트림 슬롯(슬롯마다 미리
// 잡은 링)과 그 링을 채우는 디코드 실행을 맡는다.
// - 제어 쪽(제어 pipe 스레드 하나): AcquireSlot → StartJob(제어 스레드가 만든 AudioStreamDecoder) →
//   … → CancelJob → IsReturned가 true가 된 뒤 ReleaseSlot. Start·Cancel은 SPSC 큐로 넘기고 디코드
//   스레드를 깨운다.
// - 디코드 쪽: Thread 방식이면 전용 스레드("AudioDecode", AboveNormal)가 WakeSignal을 기다리고
//   (활성 슬롯이 있으면 StreamPumpTimeoutMs 시한, 없으면 시한 없음), 깨면 큐를 처리하고 읽을
//   frame이 적은 슬롯부터 Pump한다. Inline 방식이면 호출자가 PumpAll을 부른다.
// - 슬롯 재사용(늦은 완료가 새 작업 링에 섞이지 않게): 믹서가 clip 해제에 성공했고, 서비스가 그
//   작업의 반납을 출판했고(IsReturned), 회수는 제어 쪽만 한다(ReleaseSlot).
// - AudioRT는 WakeSignal()을 믹서 설정(streamRefillSignal)으로 받아 refill 수위 아래에서
// Signal한다.
//
// 사용 예(제어 쪽):
//   auto slot = service.AcquireSlot(channels);
//   auto decoder = AudioStreamDecoder::TryCreate(allocator, Move(codecDecoder), desc);
//   (void)service.StartJob(slot.Value(), decoder.Value().Get());   // decoder는 슬롯 회수까지 쥔다
//   mixer.RegisterStreamClip({&service.Ring(slot.Value()), ...});
//   ... 재생이 끝나면 UnregisterClip → service.CancelJob(slot) → IsReturned 확인 → ReleaseSlot.
namespace rocks::audio {

enum class AudioStreamServiceMode : uint8 {
    Thread,
    Inline
};

// 슬롯 수·링 용량의 기본값과 상한이다(3.40.3절 CVar 기본: 슬롯은 voice 상한의 여유 배수).
inline constexpr uint32 DefaultMaxStreams = 64;
inline constexpr uint32 MaxStreamSlots = 1024;
// 활성 슬롯이 있을 때 깨우기 대기 시한이다(refill 신호를 놓친 경우의 안전망).
inline constexpr uint32 StreamPumpTimeoutMs = 10;

struct AudioStreamServiceConfig {
    uint32 maxStreams = DefaultMaxStreams;
    uint32 ringFrames = StreamRingMinFrames;
    AudioStreamServiceMode mode = AudioStreamServiceMode::Thread;
};

class ROCKS_AUDIO_API AudioStreamService {
public:
    // 서비스와 슬롯 링을 만든다(Thread 방식도 스레드는 Start가 띄운다). maxStreams가 0이거나
    // MaxStreamSlots를 넘거나 ringFrames가 링 범위 밖이면 InvalidArgument, 깨우기 신호 초기화
    // 실패는 Unsupported, 할당 실패는 OutOfMemory다.
    [[nodiscard]] static Result<UniquePtr<AudioStreamService>>
    TryCreate(Allocator& allocator, const AudioStreamServiceConfig& config) noexcept;

    AudioStreamService(Allocator& allocator, const AudioStreamServiceConfig& config) noexcept;
    AudioStreamService(const AudioStreamService&) = delete;
    AudioStreamService& operator=(const AudioStreamService&) = delete;
    // Shutdown을 부른다.
    ~AudioStreamService() noexcept;

    // Thread 방식이면 디코드 스레드를 띄운다(Inline은 아무것도 하지 않는다). 이미 띄웠으면
    // 성공이다. 스레드 시작 실패는 Unsupported다.
    Result<void> Start() noexcept;

    // 디코드 스레드를 멈추고 join한다(활성 작업은 모두 반납된 것으로 출판한다). 여러 번 불러도
    // 된다. 제어 스레드가 부른다.
    void Shutdown() noexcept;

    AudioStreamServiceMode Mode() const noexcept {
        return config_.mode;
    }

    // AudioRT·제어 쪽이 Signal하는 깨우기 신호다(대기자는 디코드 스레드 하나).
    platform::RealtimeWakeSignal& WakeSignal() noexcept {
        return wake_;
    }

    // --- 제어 쪽 ---

    // 빈 슬롯을 잡고 그 링을 channels(1–2) 채널로 비운다. 빈 슬롯이 없으면 CapacityExceeded,
    // channels가 범위 밖이면 InvalidArgument다.
    Result<uint32> AcquireSlot(uint32 channels) noexcept;

    // 잡은 슬롯의 링이다.
    AudioStreamRing& Ring(uint32 slot) noexcept;

    // 잡은 슬롯에서 decoder로 작업을 시작한다(디코드 쪽이 그 슬롯 링을 채운다). decoder는
    // ReleaseSlot 때까지 살아야 한다(제어 쪽이 소유). 작업이 이미 있거나 슬롯을 잡지 않았으면
    // InvalidArgument다.
    Result<void> StartJob(uint32 slot, AudioStreamDecoder* decoder) noexcept;

    // 작업을 취소한다(디코드 쪽이 더 쓰지 않고 반납을 출판한다). 작업이 없거나 이미 취소했으면
    // 아무것도 하지 않는다.
    void CancelJob(uint32 slot) noexcept;

    // 그 슬롯의 작업을 디코드 쪽이 더 건드리지 않는지다(끝났거나 취소됐거나 작업이 없다).
    bool IsReturned(uint32 slot) const noexcept;

    // 슬롯을 돌려준다. IsReturned가 true여야 한다(아니면 Busy).
    Result<void> ReleaseSlot(uint32 slot) noexcept;

    // 잡은 슬롯 수다(진단).
    uint32 AcquiredSlots() const noexcept;

    // --- 실행 ---

    // Inline 방식: 큐를 처리하고 모든 활성 슬롯을 한 번 Pump한다(링이 차거나 끝 상태가 될 때까지).
    // Thread 방식에서 부르면 assert다.
    void PumpAll() noexcept;

    // 큐를 처리하고 모든 활성 슬롯을 가득·끝 상태까지 채운 뒤 돌아온다(시험·진단 gate — Thread
    // 방식은 디코드 스레드가 끝낼 때까지 기다리고, Inline은 PumpAll이다). Thread 방식에서 스레드가
    // 없으면 NotReady다.
    Result<void> Flush() noexcept;

private:
    enum class CommandType : uint8 {
        Start,
        Cancel
    };

    struct Command {
        CommandType type = CommandType::Start;
        uint32 slot = 0;
        uint64 job = 0;
        AudioStreamDecoder* decoder = nullptr;
    };

    // 제어 쪽 슬롯 기록이다.
    struct ControlSlot {
        bool acquired = false;
        uint64 job = 0;
        bool started = false;
        bool cancelled = false;
    };

    // 디코드 쪽 슬롯 기록이다(디코드 스레드만 쓴다).
    struct ActiveSlot {
        AudioStreamDecoder* decoder = nullptr;
        uint64 job = 0;
    };

    static void ThreadMain(void* context) noexcept;
    void Loop() noexcept;
    // 큐를 처리하고 활성 슬롯을 한 번씩 Pump한다. 처리한 활성 슬롯이 남았는지 돌려준다.
    bool ServicePass() noexcept;
    void Publish(uint32 slot, uint64 job) noexcept;
    Result<void> Enqueue(const Command& command) noexcept;

    AudioStreamServiceConfig config_;
    Array<UniquePtr<AudioStreamRing>> rings_;
    Array<ControlSlot> control_;
    Array<ActiveSlot> active_;
    // 디코드 쪽이 마지막으로 반납한 작업 번호다(슬롯마다 — 원자값).
    Array<uint64> returned_;
    Array<uint32> order_;
    BoundedSpscQueue<Command> commands_;
    uint64 nextJob_ = 0;
    platform::RealtimeWakeSignal wake_;
    platform::Thread thread_;
    platform::ManualResetEvent flushDone_;
    uint64 flushRequested_ = 0;
    uint64 flushServed_ = 0;
    uint32 running_ = 0;
    bool threadStarted_ = false;
};

} // namespace rocks::audio
