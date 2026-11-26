#pragma once

#include <Audio/AudioMixer.h>
#include <Audio/AudioMixerTypes.h>
#include <Audio/AudioOutput.h>
#include <Audio/AudioOutputRing.h>
#include <Audio/AudioRenderThread.h>
#include <Audio/NullAudioOutput.h>
#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Core/Error.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Platform/Thread.h>

namespace rocks::audio {

// AudioRT를 어떻게 돌릴지다.
enum class AudioRenderMode : uint8 {
    // 전용 AudioRT 스레드(실장치·타이머 Null).
    Thread,
    // 스레드 없이 호출자가 RenderAvailable을 부른다(시험 — Null은 수동 Pump와 함께 쓴다).
    Manual
};

// 장치 열기 옵션이다.
struct AudioOutputDeviceOptions {
    AudioRenderMode renderMode = AudioRenderMode::Thread;
    // backend가 없거나 실패해 Null로 갈 때의 방식이다.
    NullAudioOutputMode nullMode = NullAudioOutputMode::Timer;
    // 출력 링 칸 수 요청값이다(0이면 계산값, 3.2절 audio.mixer.buffercount).
    uint32 requestedBufferCount = 0;
    // AudioRT 명령 링 칸 수다(2의 거듭제곱, 기본 AudioRtCommandCapacity). 시험이 링 포화를 재현할
    // 때만 줄인다.
    uint32 commandCapacity = AudioRtCommandCapacity;
};

// 열린 장치 정보다(로그·진단).
struct AudioOutputDeviceInfo {
    AudioOutputFormat format;
    uint32 bufferCount = 0;
    // 실제 쓰는 backend 이름이다(Null이면 "Null").
    const char* backendName = nullptr;
    // backend가 없거나 실패해 Null로 열었는지다(장치 상태 Unavailable — 3.7.3절).
    bool usingNull = false;
    // 실 backend가 실패했을 때의 오류와 원래 OS·라이브러리 코드다(실패가 없으면 None·0).
    ErrorCode backendError = ErrorCode::None;
    int32 backendNativeError = 0;
    // AudioRT에 실제로 적용된 우선순위다(스레드 방식에서 조회했을 때만).
    bool priorityKnown = false;
    platform::ThreadPriority priority = platform::ThreadPriority::Normal;
};

// 출력 장치 하나다(Docs/Design/AudioSystem 3.33.4절): backend(없으면 Null) + 믹서 + 출력 링 +
// AudioRT.
//
// 열기: backend Open → 협상 rate로 믹서 생성(장치가 rate를 정한다) → 출력 링·AudioRT → 링을 채운 뒤
// backend Start. backend가 없거나 Open·Start가 실패하거나 layout이 Mono·Stereo가 아니면 Null로
// 연다(원래 오류는 Info에 남긴다). 닫기: backend Stop(콜백 종료 장벽) → AudioRT 정지 → backend
// Close.
//
// 스레드 계약: Mixer()의 제어 쪽 메서드와 Submit은 제어 쪽 한 스레드에서 부른다. 객체는 옮기지
// 않는다 (콜백이 주소를 쥔다).
//
// 사용 예:
//   auto device = AudioOutputDevice::TryOpen(mixerConfig, Move(backend), request, {}, allocator);
//   AudioMixer& mixer = device.Value()->Mixer();
//   auto voice = mixer.AcquireVoice();
//   (void)device.Value()->Submit(mixer.PreparePlay(voice.Value(), clip, 0, false, {}).Value());
class ROCKS_AUDIO_API AudioOutputDevice {
public:
    // backend는 nullptr이어도 된다(Null로 연다). request.callbackFrames가 0이면 믹서 블록 frame
    // 수를 쓴다. 실패: 믹서 설정 오류는 InvalidArgument, 할당 실패는 OutOfMemory, Null조차 시작하지
    // 못하면 Unsupported.
    [[nodiscard]] static Result<UniquePtr<AudioOutputDevice>>
    TryOpen(const AudioMixerConfig& mixerConfig, UniquePtr<AudioOutputBackend> backend,
            const AudioOutputRequest& request, const AudioOutputDeviceOptions& options,
            Allocator& allocator) noexcept;

    explicit AudioOutputDevice(Allocator& allocator,
                               const AudioOutputDeviceOptions& options) noexcept;
    ~AudioOutputDevice() noexcept;
    AudioOutputDevice(const AudioOutputDevice&) = delete;
    AudioOutputDevice& operator=(const AudioOutputDevice&) = delete;

    // 장치를 닫는다. 두 번 불러도 된다.
    void Close() noexcept;

    // 믹서다. 제어 쪽 메서드만 부른다(렌더 쪽은 AudioRT가 쓴다).
    AudioMixer& Mixer() noexcept {
        return *mixer_;
    }

    // 제어 쪽: 명령 하나를 AudioRT에 넣는다. 링이 차면 Busy다.
    Result<void> Submit(const AudioRtCommand& command) noexcept {
        return render_.Submit(command);
    }

    const AudioOutputDeviceInfo& Info() const noexcept {
        return info_;
    }

    AudioRenderCounters RenderCountersSnapshot() const noexcept {
        return render_.CountersSnapshot();
    }

    AudioOutputRingCounters RingCountersSnapshot() const noexcept {
        return ring_.CountersSnapshot();
    }

    // 수동 방식: AudioRT 한 바퀴(명령을 넣고 링 빈 칸을 채운다)를 이 스레드에서 돈다.
    void RenderAvailable() noexcept;

    // Null로 열었을 때의 Null backend다(수동 Pump용). 실 backend면 nullptr이다.
    NullAudioOutput* NullOutput() noexcept {
        return info_.usingNull ? &null_ : nullptr;
    }

    // 출력 링이다(진단·시험).
    const AudioOutputRing& Ring() const noexcept {
        return ring_;
    }

private:
    // 장치 콜백이 부르는 링 읽기다.
    class RingCallback final : public AudioOutputCallback {
    public:
        explicit RingCallback(AudioOutputRing& ring) noexcept : ring_(ring) {
        }

        void Fill(const AudioOutputView& view) noexcept override {
            ring_.Read(view);
        }

    private:
        AudioOutputRing& ring_;
    };

    Result<void> OpenWith(AudioOutputBackend& backend, const AudioMixerConfig& mixerConfig,
                          const AudioOutputRequest& request) noexcept;
    void CloseActive() noexcept;

    Allocator& allocator_;
    AudioOutputDeviceOptions options_;
    UniquePtr<AudioOutputBackend> backend_;
    NullAudioOutput null_;
    AudioOutputBackend* active_ = nullptr;
    Optional<AudioMixer> mixer_;
    AudioOutputRing ring_;
    AudioRenderThread render_;
    RingCallback callback_;
    AudioOutputDeviceInfo info_;
    bool started_ = false;
};

} // namespace rocks::audio
