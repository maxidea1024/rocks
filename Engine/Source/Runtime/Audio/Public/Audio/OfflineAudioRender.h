#pragma once

#include <Audio/AudioMixer.h>

// Inline(결정적) 렌더러다(Docs/Design/AudioSystem.ko.md 3.22.9절, UE FMixerPlatformNonRealtime
// 대응). AudioRT 스레드 대신 호출 스레드에서 믹서 블록을 렌더한다. 모든 믹서 시험과 오프라인 캡처가
// 쓴다.
//
// 명령은 제어 API로 만들어 Submit으로 대기 목록에 넣는다. Render는 블록마다 Immediate를 그 블록
// 시작 frame으로 바꾸고 (mixFrame, sequence)로 정렬해 RenderBlock에 넘긴 뒤 소비한 만큼 지운다.
// 블록 루프는 실시간 오디오 FP 환경(RunWithRealtimeAudioFloatingPoint) 안에서 돈다. 모든 호출은
// 같은 스레드다.
//
// 사용 예:
//   auto renderer = OfflineAudioRender::TryCreate(AudioMixerConfig{}, allocator);
//   AudioMixer& mixer = renderer.Value().Mixer();
//   AudioVoiceHandle voice = mixer.AcquireVoice().Value();
//   auto play = mixer.PreparePlay(voice, clip, 0, false, AudioVoiceParams{});
//   (void)renderer.Value().Submit(play.Value());
//   float output[256 * 2 * 4];
//   (void)renderer.Value().Render(Span<float>(output), 4);  // 4블록
namespace rocks::audio {

class OfflineAudioRender {
public:
    // 믹서를 만든다(AudioMixer::TryCreate와 같은 오류).
    [[nodiscard]] static Result<OfflineAudioRender> TryCreate(const AudioMixerConfig& config,
                                                              Allocator& allocator) noexcept;

    OfflineAudioRender(OfflineAudioRender&& other) noexcept = default;
    OfflineAudioRender& operator=(OfflineAudioRender&& other) noexcept = default;
    OfflineAudioRender(const OfflineAudioRender&) = delete;
    OfflineAudioRender& operator=(const OfflineAudioRender&) = delete;
    ~OfflineAudioRender() noexcept = default;

    AudioMixer& Mixer() noexcept {
        return mixer_;
    }

    const AudioMixer& Mixer() const noexcept {
        return mixer_;
    }

    // 명령을 대기 목록에 넣는다. 순번은 제어 API가 매긴 값을 그대로 쓴다. 목록을 늘리지 못하면
    // OutOfMemory.
    Result<void> Submit(const AudioRtCommand& command) noexcept;

    // blockCount 블록을 렌더한다. interleaved는 blockCount × Q × 출력 채널 수 float여야 한다(아니면
    // InvalidArgument).
    Result<void> Render(Span<float> interleaved, uint32 blockCount) noexcept;

    // 대기 목록에 남은 명령 수다.
    usize PendingCommands() const noexcept {
        return pending_.Size();
    }

private:
    OfflineAudioRender(AudioMixer&& mixer, Allocator& allocator) noexcept;

    static void RenderBlocks(void* context) noexcept;

    AudioMixer mixer_;
    Array<AudioRtCommand> pending_;
    // Render가 RenderBlocks에 넘기는 인자다.
    Span<float> renderTarget_;
    uint32 renderBlocks_ = 0;
};

} // namespace rocks::audio
