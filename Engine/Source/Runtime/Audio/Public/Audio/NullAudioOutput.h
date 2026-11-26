#pragma once

#include <Audio/AudioOutput.h>
#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/Atomics.h>
#include <Platform/Thread.h>

namespace rocks::audio {

enum class NullAudioOutputMode : uint8 {
    // 자체 스레드가 콜백 주기마다 콜백을 부른다(장치가 없을 때 시간 진행).
    Timer,
    // 스레드 없이 Pump를 부르는 스레드가 콜백을 부른다(시험).
    Manual
};

// 장치 없이 시간만 진행하는 출력 backend다(Docs/Design/AudioSystem 3.7.2·3.33.5절, UE
// FMixerNullCallback 대응). 장치가 없거나 열기에 실패하면 AudioOutputDevice가 이것으로 연다. 콜백이
// 채운 PCM은 버린다 (CaptureTo로 시험이 받을 수 있다).
//
// 협상: sampleRate는 요청값(0이면 DefaultMixSampleRate), layout은 요청값, callbackFrames는
// 요청값(0이면 DefaultMixBlockFrames), 형식은 Float32다.
//
// 타이머 방식은 단조 시계로 콜백 주기의 누적 시한을 계산하고 남은 시간만 잔다(늦어도 드리프트가
// 쌓이지 않는다). 실시간 재생의 시간 진행이 목적이라 시간 기반 동작이 맞는 자리다.
//
// 사용 예(시험):
//   NullAudioOutput output(allocator, NullAudioOutputMode::Manual);
//   auto format = output.Open(request, callback);
//   (void)output.Start();
//   output.Pump(480);          // 콜백 한 번(480 frame)
class ROCKS_AUDIO_API NullAudioOutput final : public AudioOutputBackend {
public:
    NullAudioOutput(Allocator& allocator, NullAudioOutputMode mode) noexcept;
    ~NullAudioOutput() noexcept override;

    NullAudioOutput(const NullAudioOutput&) = delete;
    NullAudioOutput& operator=(const NullAudioOutput&) = delete;

    Result<AudioOutputFormat> Open(const AudioOutputRequest& request,
                                   AudioOutputCallback& callback) noexcept override;
    Result<void> Start() noexcept override;
    Result<void> Stop() noexcept override;
    void Close() noexcept override;
    Result<uint32> EnumerateDevices(Span<AudioDeviceInfo> out) noexcept override;
    const char* Name() const noexcept override;
    int32 LastNativeError() const noexcept override;

    // 수동 방식: 콜백을 frames(1 이상, callbackFrames 이하)로 한 번 부른다. Start 뒤·Stop 전에만
    // 불린다(아니면 아무 일도 하지 않는다). 타이머 방식에서는 아무 일도 하지 않는다.
    void Pump(uint32 frames) noexcept;

    // 콜백이 채운 PCM을 capture 앞에서부터 이어 받는다(다 차면 그만 받는다). nullptr이면 끈다. 수동
    // 방식 시험용이다(Pump 사이에 부른다).
    void CaptureTo(Span<float> capture) noexcept;

    // 지금까지 받은 표본 수다(CaptureTo 이후).
    usize CapturedSamples() const noexcept {
        return captured_;
    }

    // 콜백을 부른 횟수다.
    uint64 CallbackCount() const noexcept {
        return callbacks_.Load<platform::MemoryOrder::Acquire>();
    }

private:
    static void TimerMain(void* context) noexcept;
    void Deliver(uint32 frames) noexcept;

    Allocator& allocator_;
    NullAudioOutputMode mode_;
    AudioOutputCallback* callback_ = nullptr;
    AudioOutputFormat format_;
    float* buffer_ = nullptr;
    Span<float> capture_;
    usize captured_ = 0;
    platform::Thread timer_;
    platform::Atomic<uint32> running_;
    platform::Atomic<uint32> stopRequested_;
    platform::Atomic<uint64> callbacks_;
};

} // namespace rocks::audio
