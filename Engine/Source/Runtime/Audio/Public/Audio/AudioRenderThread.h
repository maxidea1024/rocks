#pragma once

#include <Audio/AudioMixer.h>
#include <Audio/AudioMixerTypes.h>
#include <Audio/AudioOutputRing.h>
#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Core/Containers/BoundedSpscQueue.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/Atomics.h>
#include <Platform/Sync.h>
#include <Platform/Sync/RealtimeWakeSignal.h>
#include <Platform/Thread.h>

namespace rocks::audio {

// 제어 쪽 → AudioRT 명령 링 용량(2의 거듭제곱)과 AudioRT가 소유하는 정렬 pending 배열 용량이다
// (Docs/Design/AudioSystem 3.22.1·3.33.3절).
inline constexpr uint32 AudioRtCommandCapacity = 1024;
inline constexpr uint32 AudioRtPendingCapacity = AudioRtCommandCapacity;
// 링이 찬 채로 깨우기를 기다리는 시간이다. 넘으면 장치 콜백이 멈춘 것으로 센다(3.2절).
inline constexpr uint32 RenderWaitTimeoutMs = 500;

// AudioRT 계수 스냅샷이다.
struct AudioRenderCounters {
    uint64 renderedBlocks = 0;
    // pending 배열이 차서 명령 링 소비를 멈춘 블록 수다.
    uint64 pendingFullStalls = 0;
    // 깨우기 대기가 RenderWaitTimeoutMs를 넘긴 수다.
    uint64 stalledWaits = 0;
};

// AudioRT다(3.2·3.33.3절). 명령 링에서 명령을 꺼내 (mixFrame, sequence) 순서의 pending 배열에 넣고,
// 출력 링에 빈 칸이 있는 동안 AudioMixer::RenderBlock으로 블록을 렌더한다. 링이 차면 장치 콜백이
// 칸을 비울 때까지 RealtimeWakeSignal을 기다린다. 렌더 루프 안에서 할당·잠금·로그가 없다.
//
// 두 방식이다.
// - 스레드: Start가 전용 스레드("AudioRT", Highest)를 만들고 링을 한 번 다 채운 뒤 돌아온다. Stop이
// 멈추고
//   Join한다.
// - 수동(시험·Inline): 스레드 없이 호출자가 RenderAvailable을 부른다(그 호출자가 RT 스레드
// 역할이다).
//
// 스레드 계약: Submit은 제어 쪽 한 스레드(명령 링 생산자)만 부른다. mixer의 렌더 쪽 메서드는
// AudioRT만 쓴다. Initialize·Start·Stop은 제어 쪽에서 부른다.
//
// 사용 예:
//   AudioRenderThread render;
//   (void)render.Initialize(allocator, mixer, ring);
//   (void)render.Start();                       // 링을 채운 뒤 돌아온다
//   (void)render.Submit(mixer.PreparePlay(...).Value());
//   render.Stop();
class ROCKS_AUDIO_API AudioRenderThread {
public:
    AudioRenderThread() noexcept = default;
    AudioRenderThread(const AudioRenderThread&) = delete;
    AudioRenderThread& operator=(const AudioRenderThread&) = delete;

    ~AudioRenderThread() noexcept;

    // 명령 링과 pending 배열을 할당한다. mixer·ring은 이 객체보다 오래 살아야 한다.
    // commandCapacity는 명령 링·pending 칸 수다(2의 거듭제곱 2 이상 — 시험이 포화를 재현할 때만
    // 줄인다). 실패: 이미 초기화됨·잘못된 용량은 InvalidArgument, 그 밖은 allocator 오류(상태는
    // 그대로).
    Result<void> Initialize(Allocator& allocator, AudioMixer& mixer, AudioOutputRing& ring,
                            uint32 commandCapacity = AudioRtCommandCapacity) noexcept;

    // 할당을 돌려준다(스레드가 멈춘 뒤).
    void Shutdown() noexcept;

    // 제어 쪽: 명령 하나를 넣는다. 링이 차면 Busy다(제어 쪽이 미룬다).
    Result<void> Submit(const AudioRtCommand& command) noexcept;

    // 스레드 방식: AudioRT를 시작하고 링을 다 채운 뒤 돌아온다. 실패: 초기화 전·이미 시작함은
    // InvalidArgument, 스레드를 만들지 못하면 Unsupported.
    Result<void> Start() noexcept;

    // 스레드 방식: 멈추고 Join한다. 시작하지 않았으면 아무 일도 하지 않는다.
    void Stop() noexcept;

    // 수동 방식(그리고 AudioRT 루프): 명령을 꺼내 넣고 링에 빈 칸이 있는 동안 렌더한다. 실시간 FP
    // 환경 안에서 부른다(스레드 방식은 루프가 그렇게 한다).
    void RenderAvailable() noexcept;

    // 장치 콜백이 칸을 비운 뒤 Signal하는 깨우기 신호다(출력 링에 넘긴다).
    platform::RealtimeWakeSignal& WakeSignal() noexcept {
        return wake_;
    }

    // AudioRT에 실제로 적용된 우선순위다. Start가 돌아온 뒤 읽는다. 조회하지 못했으면 false다.
    bool AppliedPriority(platform::ThreadPriority& priority) const noexcept;

    AudioRenderCounters CountersSnapshot() const noexcept;

    // pending 배열에 남은 명령 수다(AudioRT·수동 호출자만).
    uint32 PendingCount() const noexcept {
        return pendingCount_;
    }

private:
    static void ThreadMain(void* context) noexcept;
    static void Loop(void* context) noexcept;

    void DrainCommands() noexcept;
    void InsertPending(const AudioRtCommand& command) noexcept;

    BoundedSpscQueue<AudioRtCommand> commands_;
    AudioRtCommand* pending_ = nullptr;
    uint32 pendingCount_ = 0;
    uint32 pendingCapacity_ = 0;
    Allocator* allocator_ = nullptr;
    AudioMixer* mixer_ = nullptr;
    AudioOutputRing* ring_ = nullptr;
    platform::RealtimeWakeSignal wake_;
    platform::Thread thread_;
    platform::ManualResetEvent started_;
    platform::Atomic<uint32> stopRequested_;
    platform::Atomic<uint32> running_;
    platform::Atomic<uint32> priorityKnown_;
    platform::Atomic<uint32> appliedPriority_;
    platform::Atomic<uint64> renderedBlocks_;
    platform::Atomic<uint64> pendingFullStalls_;
    platform::Atomic<uint64> stalledWaits_;
};

} // namespace rocks::audio
