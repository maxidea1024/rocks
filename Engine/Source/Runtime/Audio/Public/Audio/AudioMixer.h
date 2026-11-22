#pragma once

#include <Audio/AudioBuffer.h>
#include <Audio/AudioMixerTypes.h>
#include <Audio/DynamicsProcessor.h>
#include <Audio/GainRamp.h>
#include <Audio/LinearResampler.h>
#include <Audio/OnePoleFilter.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>

// 믹서 코어다(Docs/Design/AudioSystem.ko.md 3.22절, UE FMixerDevice·FMixerSourceManager 대응).
//
// 두 면이 있다.
// - 제어 쪽: AcquireVoice·PreparePlay·ReleaseVoice·RegisterClip·UnregisterClip·QueryVoice·
//   CollectFinished. 할당하지 않지만(표는 생성 때 고정) 실패를 Result로 돌려준다.
// - 렌더 쪽: RenderBlock(commands, out). 블록 하나(Q frame)를 만든다. 할당·대기·로그가 없다.
// Mixer1에서는 두 면을 같은 스레드에서 부른다(스레드 경계 원자화는 Mixer2 — 3.22절).
// RenderBlock은 실시간 오디오 FP 환경(platform::RunWithRealtimeAudioFloatingPoint) 안에서 불러야
// 한다(진입 assert). 보통은 OfflineAudioRender나 AudioRT 루프가 부른다.
//
// 사용 예(Inline):
//   auto mixer = AudioMixer::TryCreate(AudioMixerConfig{}, allocator);
//   auto clip = mixer.Value().RegisterClip(desc);
//   auto voice = mixer.Value().AcquireVoice();
//   AudioVoiceParams params;
//   auto play = mixer.Value().PreparePlay(voice.Value(), clip.Value(), 0, false, params);
//   play.Value().mixFrame = 0;
//   ... RunWithRealtimeAudioFloatingPoint 안에서 RenderBlock(Span(&play.Value(), 1), out) ...
namespace rocks::audio {

class AudioMixer {
public:
    // 믹서를 만든다. 설정이 3.22.2절 범위 밖이면 InvalidArgument, 할당 실패는 OutOfMemory(만든 것을
    // 모두 되돌린다). allocator는 믹서보다 오래 살아야 한다.
    [[nodiscard]] static Result<AudioMixer> TryCreate(const AudioMixerConfig& config,
                                                      Allocator& allocator) noexcept;

    AudioMixer(AudioMixer&& other) noexcept = default;
    AudioMixer& operator=(AudioMixer&& other) noexcept = default;
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    ~AudioMixer() noexcept = default;

    const AudioMixerConfig& Config() const noexcept {
        return config_;
    }

    uint32 OutputChannelCount() const noexcept {
        return ChannelCount(config_.layout);
    }

    // 다음 블록의 시작 frame(mix clock)이다.
    uint64 MixFrame() const noexcept {
        return mixFrame_;
    }

    const AudioMixerCounters& Counters() const noexcept {
        return counters_;
    }

    // --- clip 표(제어 쪽) ---

    // clip을 등록한다. 채널 1–2, frame 1 이상, rate [MinClipSampleRate, MaxClipSampleRate], loop
    // 구간이 올바르고 모든 표본이 유한해야 한다(아니면 InvalidArgument). 표가 차면
    // CapacityExceeded.
    Result<AudioClipId> RegisterClip(const AudioClipDesc& desc) noexcept;

    // clip을 해제한다. 묶인 voice가 있으면 Busy, 이전 generation이면 StaleHandle. 성공한 뒤에만
    // 호출자가 PCM 메모리를 돌려준다.
    Result<void> UnregisterClip(AudioClipId clip) noexcept;

    // --- voice(제어 쪽) ---

    // 빈 voice 슬롯을 Reserved로 잡는다. 없으면 CapacityExceeded.
    Result<AudioVoiceHandle> AcquireVoice() noexcept;

    // voice에 clip을 묶고 Play 명령을 만든다(mixFrame은 ImmediateMixFrame, 호출자가 바꿔도 된다).
    // - 이전 generation의 voice·clip은 StaleHandle.
    // - Reserved가 아닌 voice, startFrame ≥ frameCount, loop인데 startFrame ≥ loop 끝은
    // InvalidArgument.
    Result<AudioRtCommand> PreparePlay(AudioVoiceHandle voice, AudioClipId clip, uint32 startFrame,
                                       bool loop, const AudioVoiceParams& params) noexcept;

    // voice를 놓는다. Reserved(Play 없음)나 수거한 Finished에서만 된다. Prepared·Playing·Stopping과
    // 수거하지 않은 Finished는 Busy, 이전 generation은 StaleHandle. generation을 올리고 clip 묶음을
    // 푼다.
    Result<void> ReleaseVoice(AudioVoiceHandle voice) noexcept;

    // voice 상태를 읽는다. 이전 generation은 StaleHandle.
    Result<AudioVoiceStatus> QueryVoice(AudioVoiceHandle voice) const noexcept;

    // 아직 수거하지 않은 terminal을 out에 채우고 수거 표시한다. 채운 수를 돌려준다.
    uint32 CollectFinished(Span<AudioVoiceTerminal> out) noexcept;

    // --- 렌더 쪽 ---

    // 블록 하나를 렌더한다(3.22.6절).
    // - commands: (mixFrame, sequence) 오름차순, ImmediateMixFrame 없음(어기면 assert).
    // - interleavedOut: Q × OutputChannelCount() float(다르면 assert).
    // 반환: 앞에서부터 소비한 명령 수. 블록 끝 이후 명령과 구간 상한을 넘는 명령은 남긴다.
    RenderBlockResult RenderBlock(Span<const AudioRtCommand> commands,
                                  Span<float> interleavedOut) noexcept;

private:
    struct ClipSlot {
        AudioClipDesc desc;
        uint32 generation = 1;
        uint32 users = 0;
        bool registered = false;
    };

    struct VoiceSlot {
        uint32 generation = 1;
        AudioVoiceState state = AudioVoiceState::Idle;
        bool collected = false;
        uint32 clipIndex = 0;
        uint32 startFrame = 0;
        bool loop = false;
        // 리샘플러가 아직 소비하지 않은 첫 원본 frame(loop면 감싼 값)이다.
        uint64 sourceCursor = 0;
        AudioVoiceParams params;
        uint64 pendingParamsSequence = 0;
        bool hasPendingParams = false;
        LinearResampler resampler;
        OnePoleLowPass lowPass;
        OnePoleHighPass highPass;
        GainRamp gain;
        GainRamp fade;
        // [원본 채널][출력 채널] 이득 ramp다.
        GainRamp channelGains[MaxClipChannels][MaxClipChannels];
        uint64 startMixFrame = UnsetMixFrame;
        uint64 endMixFrame = UnsetMixFrame;
        AudioVoiceEndReason endReason = AudioVoiceEndReason::None;
    };

    AudioMixer(const AudioMixerConfig& config, Allocator& allocator) noexcept;

    VoiceSlot* FindVoice(AudioVoiceHandle voice) noexcept;
    const VoiceSlot* FindVoice(AudioVoiceHandle voice) const noexcept;

    void ApplyCommand(const AudioRtCommand& command, uint64 frame) noexcept;
    void StartVoice(VoiceSlot& slot, const AudioRtCommand& command, uint64 frame) noexcept;
    void ApplyParams(VoiceSlot& slot, const AudioVoiceParams& params, uint32 rampFrames,
                     bool immediate) noexcept;
    void Finish(VoiceSlot& slot, AudioVoiceEndReason reason, uint64 frame) noexcept;
    void RenderSegment(uint32 offset, uint32 frames) noexcept;
    void RenderVoice(VoiceSlot& slot, uint32 offset, uint32 frames) noexcept;
    uint32 FillInput(VoiceSlot& slot, uint32 frames) noexcept;
    void AdvanceCursor(VoiceSlot& slot, uint32 consumed) noexcept;
    void FinishMaster(Span<float> interleavedOut) noexcept;

    AudioMixerConfig config_;
    Array<VoiceSlot> voices_;
    Array<ClipSlot> clips_;
    PlanarBuffer inputScratch_;
    PlanarBuffer voiceScratch_;
    PlanarBuffer master_;
    DynamicsProcessor limiter_;
    GainRamp masterGain_;
    uint64 mixFrame_ = 0;
    uint32 nextVoiceSearch_ = 0;
    AudioMixerCounters counters_;
};

} // namespace rocks::audio
