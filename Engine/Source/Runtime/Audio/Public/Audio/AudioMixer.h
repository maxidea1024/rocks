#pragma once

#include <Audio/AudioBuffer.h>
#include <Audio/AudioMixerTypes.h>
#include <Audio/DynamicsProcessor.h>
#include <Audio/GainRamp.h>
#include <Audio/LinearResampler.h>
#include <Audio/OnePoleFilter.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>

// 믹서 코어다(Docs/Design/AudioSystem 3.22–3.23절, UE FMixerDevice·FMixerSourceManager 대응).
//
// 두 면이 있고 서로 다른 스레드에서 불러도 된다(3.23절 스레드 경계).
// - 제어 쪽(제어 스레드 하나): AcquireVoice·PreparePlay·Make*·PublishParams·RequestStop·StopAll·
//   ReleaseVoice·RegisterClip·UnregisterClip·QueryVoice·CollectFinished·CountersSnapshot·
//   CompletedMixFrame. 제어 슬롯만 쓰고, RT와는 원자값(seqlock·mailbox·epoch·시작/terminal·ACK)과
//   명령으로만 주고받는다. 할당하지 않는다(표는 생성 때 고정).
// - 렌더 쪽(RT 스레드 하나): RenderBlock. 블록 하나(Q frame)를 만든다. 할당·대기·로그가 없다.
//   Counters()·MixFrame()은 RT 스레드나 Inline 렌더 사이에서만 부른다.
// RenderBlock은 실시간 오디오 FP 환경(platform::RunWithRealtimeAudioFloatingPoint) 안에서 불러야
// 한다 (진입 assert). 보통은 OfflineAudioRender나 AudioRT 루프가 부른다.
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

class AudioMixerTestAccess;

class ROCKS_AUDIO_API AudioMixer {
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

    // --- 제어 쪽 ---

    // clip을 등록한다. 채널 1–2, frame 1 이상, rate [MinClipSampleRate, MaxClipSampleRate], loop
    // 구간이 올바르고 모든 표본이 유한해야 한다(아니면 InvalidArgument). 표가 차면
    // CapacityExceeded.
    Result<AudioClipId> RegisterClip(const AudioClipDesc& desc) noexcept;

    // 스트림 clip을 등록한다(3.40.1절). 채널 1–2(링 채널 수와 같음), frame 1 이상, rate 범위, loop
    // 구간, startFrame < frameCount(loop면 < loop 끝)가 맞아야 한다(아니면 InvalidArgument). 표가
    // 차면 CapacityExceeded. voice는 하나만 묶을 수 있다(SPSC 링).
    Result<AudioClipId> RegisterStreamClip(const AudioStreamClipDesc& desc) noexcept;

    // clip을 해제한다. 묶인 voice가 있으면 Busy, 이전 generation이면 StaleHandle. 성공한 뒤에만
    // 호출자가 PCM 메모리를 돌려준다.
    Result<void> UnregisterClip(AudioClipId clip) noexcept;

    // 빈 voice 슬롯을 Reserved로 잡는다. 없으면 CapacityExceeded.
    Result<AudioVoiceHandle> AcquireVoice() noexcept;

    // voice에 clip을 묶고 Play 명령을 만든다(mixFrame은 ImmediateMixFrame, 호출자가 바꿔도 된다).
    // - 이전 generation의 voice·clip은 StaleHandle.
    // - 스트림 clip: startFrame·loop가 등록 값과 다르거나 이미 묶인 voice가 있으면 InvalidArgument.
    //   Play가 적용돼도 링이 min(StreamStartFrames, 남은 frame)을 채우거나 끝 상태가 될 때까지
    //   시작하지 않는다(시작 시각은 실제 시작 블록 frame이다).
    // - Reserved가 아닌 voice, startFrame ≥ frameCount, loop인데 startFrame ≥ loop 끝, 범위 밖
    //   파라미터, stereo clip의 Azimuth는 InvalidArgument.
    Result<AudioRtCommand> PreparePlay(AudioVoiceHandle voice, AudioClipId clip, uint32 startFrame,
                                       bool loop, const AudioVoiceParams& params) noexcept;

    // PreparePlay로 만든 Play 명령을 장치에 보내지 않고 버린다(명령 링이 차서 미뤘던 Play를 제어
    // 쪽이 거둘 때 — Docs/Design/AudioSystem 3.41.1절). voice는 Reserved로 돌아가고(ReleaseVoice로
    // 바로 놓을 수 있다) clip 사용 수를 내린다. AudioRT는 그 명령을 본 적이 없으므로 순번 틈은
    // 문제가 없다(순번은 voice 수명 안에서만 비교한다). 호출자 계약: 그 명령을 Submit하지 않았어야
    // 한다(보냈다면 AudioRT가 낡은 generation의 Play로 버린다 — 소리는 나지 않지만 쓰지 말 것).
    // 오류: 이전 generation은 StaleHandle, Play를 준비한 상태가 아니면 InvalidArgument.
    Result<void> AbandonPreparedPlay(AudioVoiceHandle voice) noexcept;

    // 시각 지정 Stop·SetParams·SetMasterGain 명령을 만들고 순번을 매긴다. Reserved voice는
    // InvalidArgument, 이전 generation은 StaleHandle, 범위 밖 파라미터는 InvalidArgument.
    Result<AudioRtCommand> MakeStop(AudioVoiceHandle voice, uint64 mixFrame,
                                    uint32 fadeFrames) noexcept;
    Result<AudioRtCommand> MakeSetParams(AudioVoiceHandle voice, uint64 mixFrame,
                                         const AudioVoiceParams& params,
                                         uint32 rampFrames) noexcept;
    Result<AudioRtCommand> MakeSetMasterGain(uint64 mixFrame, float gain,
                                             uint32 rampFrames) noexcept;

    // 최신 파라미터를 seqlock 슬롯에 출판한다(3.23.2절). 다음 블록 시작에 DefaultParamRampFrames로
    // 적용된다. 거부 규칙은 MakeSetParams와 같다.
    Result<void> PublishParams(AudioVoiceHandle voice, const AudioVoiceParams& params) noexcept;

    // stop mailbox에 쓴다(3.23.3절). 다음 블록 시작에 적용된다. 거부 규칙은 MakeStop과 같다.
    Result<void> RequestStop(AudioVoiceHandle voice, uint32 fadeFrames) noexcept;

    // 장치 stop-all epoch를 올린다(3.23.4절). 그 시점에 살아 있던 voice와 그 전에 만든 Play를
    // 끝낸다.
    void StopAll(uint32 fadeFrames) noexcept;

    // voice를 놓는다. Reserved(Play 없음)거나, terminal을 수거했고 CompletedMixFrame() ≥
    // releaseMixFrame일 때만 된다(아니면 Busy). 이전 generation은 StaleHandle. generation을 올리고
    // clip 묶음을 푼다.
    Result<void> ReleaseVoice(AudioVoiceHandle voice) noexcept;

    // voice 상태를 시작·terminal 슬롯에서 읽는다. 이전 generation은 StaleHandle.
    Result<AudioVoiceStatus> QueryVoice(AudioVoiceHandle voice) const noexcept;

    // 아직 수거하지 않은 terminal을 out에 채우고 수거 표시한다. 채운 수를 돌려준다.
    uint32 CollectFinished(Span<AudioVoiceTerminal> out) noexcept;

    // RT가 마지막으로 끝낸 블록의 다음 frame(ACK)이다.
    uint64 CompletedMixFrame() const noexcept;

    // RT가 블록마다 게시하는 계수 스냅샷이다.
    AudioMixerCounters CountersSnapshot() const noexcept;

    // --- 렌더 쪽 ---

    // 블록 하나를 렌더한다(3.22.6절, 3.23.7절 순서: stop-all → mailbox → seqlock → 명령).
    // - commands: (mixFrame, sequence) 오름차순, ImmediateMixFrame 없음(어기면 assert).
    // - interleavedOut: Q × OutputChannelCount() float(다르면 assert).
    // 반환: 앞에서부터 소비한 명령 수. 블록 끝 이후 명령과 구간 상한을 넘는 명령은 남긴다.
    RenderBlockResult RenderBlock(Span<const AudioRtCommand> commands,
                                  Span<float> interleavedOut) noexcept;

    // 다음 블록의 시작 frame(mix clock)이다. RT 스레드나 Inline 렌더 사이에서만 부른다.
    uint64 MixFrame() const noexcept {
        return mixFrame_;
    }

    // RT 계수다. RT 스레드나 Inline 렌더 사이에서만 부른다(다른 스레드는 CountersSnapshot).
    const AudioMixerCounters& Counters() const noexcept {
        return counters_;
    }

private:
    friend class AudioMixerTestAccess;

    enum class ControlState : uint8 {
        Idle,
        Reserved,
        Submitted,
        Finished
    };

    struct ClipSlot {
        AudioClipDesc desc;
        uint32 generation = 1;
        uint32 users = 0;
        bool registered = false;
        // 스트림 clip이면 링과 그 시작 frame·loop다(desc.channels는 비어 있다).
        AudioStreamRing* stream = nullptr;
        uint32 streamStartFrame = 0;
        bool streamLoop = false;
    };

    // 제어 스레드만 쓴다.
    struct ControlVoice {
        uint32 generation = 1;
        ControlState state = ControlState::Idle;
        uint32 clipIndex = 0;
        uint64 releaseMixFrame = UnsetMixFrame;
        // 이 수명에서 RequestStop으로 낸 fade의 최소값이다. mailbox에는 줄어든 값만 쓴다(3.23.3절).
        uint32 requestedStopFade = NumericLimits<uint32>::Max();
    };

    // RT 스레드만 쓴다.
    struct VoiceSlot {
        uint32 generation = 0;
        AudioVoiceState state = AudioVoiceState::Idle;
        uint32 clipIndex = 0;
        bool loop = false;
        // 리샘플러가 아직 소비하지 않은 첫 원본 frame(loop면 감싼 값)이다.
        uint64 sourceCursor = 0;
        AudioVoiceParams params;
        uint64 lastAppliedSequence = 0;
        uint64 pendingParamsSequence = 0;
        bool hasPendingParams = false;
        uint64 lastMailbox = 0;
        // fade가 끝날 때 낼 terminal 사유(Stopped·StoppedAll)다.
        AudioVoiceEndReason stopReason = AudioVoiceEndReason::Stopped;
        // 스트림 원본이 시작 수위를 기다리는 중이다(시작 시각을 아직 출판하지 않았다).
        bool awaitingStream = false;
        LinearResampler resampler;
        OnePoleLowPass lowPass;
        OnePoleHighPass highPass;
        GainRamp gain;
        GainRamp fade;
        // [원본 채널][출력 채널] 이득 ramp다.
        GainRamp channelGains[MaxClipChannels][MaxClipChannels];
    };

    // seqlock 내용 word 수다: generation, 순번 하위·상위, 파라미터 7개(비트 패턴). AudioMixer.cpp의
    // SeqlockWord 열거와 static_assert로 묶는다.
    static constexpr uint32 SeqlockWords = 11;

    // 계수 스냅샷 word 수다. AudioMixerCounters의 uint64 필드 수와 static_assert로 묶는다.
    static constexpr uint32 CounterSnapshotWords = sizeof(AudioMixerCounters) / sizeof(uint64);

    // 제어와 RT가 원자값으로만 주고받는 voice 슬롯이다(3.23.2–3.23.5절). 필드는 Atomic 함수로만
    // 읽고 쓴다.
    struct SharedVoice {
        // seqlock: version(홀수 = 쓰는 중)과 내용.
        uint32 version = 0;
        uint32 contents[SeqlockWords] = {};
        // stop mailbox {generation:32, fadeFrames:32}. 0은 비었다는 뜻이다.
        uint64 mailbox = 0;
        // 시작: startMixFrame을 쓴 뒤 startGeneration을 출판한다.
        uint64 startMixFrame = UnsetMixFrame;
        uint32 startGeneration = 0;
        // terminal: 두 frame을 쓴 뒤 word {generation:32, reason:8}를 출판한다.
        uint64 endMixFrame = UnsetMixFrame;
        uint64 releaseMixFrame = UnsetMixFrame;
        uint64 terminal = 0;
    };

    AudioMixer(const AudioMixerConfig& config, Allocator& allocator) noexcept;

    ControlVoice* FindControl(AudioVoiceHandle voice) noexcept;
    const ControlVoice* FindControl(AudioVoiceHandle voice) const noexcept;
    Result<void> CheckCommandTarget(AudioVoiceHandle voice) const noexcept;
    bool ReadTerminal(uint32 index, uint32 generation, AudioVoiceTerminal& terminal,
                      uint64& releaseMixFrame) const noexcept;

    void ProcessStopAll(uint64 frame) noexcept;
    void ProcessMailboxes(uint64 frame) noexcept;
    void ProcessSeqlocks() noexcept;
    bool ReadSeqlock(uint32 index, uint32& generation, uint64& sequence,
                     AudioVoiceParams& params) noexcept;
    void ApplySeqlock(uint32 index, VoiceSlot& slot, bool immediate) noexcept;
    void AdoptLifetime(VoiceSlot& slot, uint32 generation) noexcept;
    void ApplyStop(uint32 index, VoiceSlot& slot, uint32 fadeFrames, uint64 frame,
                   AudioVoiceEndReason reason, bool countIgnored) noexcept;
    void ApplyCommand(const AudioRtCommand& command, uint64 frame) noexcept;
    void StartVoice(uint32 index, VoiceSlot& slot, const AudioRtCommand& command,
                    uint64 frame) noexcept;
    void ApplyParams(VoiceSlot& slot, const AudioVoiceParams& params, uint32 rampFrames,
                     bool immediate) noexcept;
    void Finish(uint32 index, VoiceSlot& slot, AudioVoiceEndReason reason, uint64 frame) noexcept;
    void RenderSegment(uint32 offset, uint32 frames) noexcept;
    void RenderVoice(uint32 index, VoiceSlot& slot, uint32 offset, uint32 frames) noexcept;
    uint32 FillInput(VoiceSlot& slot, uint32 frames) noexcept;
    void AdvanceCursor(VoiceSlot& slot, uint32 consumed) noexcept;
    void PublishStart(uint32 index, VoiceSlot& slot, uint64 frame) noexcept;
    bool StreamReadyToStart(const VoiceSlot& slot) const noexcept;
    uint32 StreamRenderableFrames(VoiceSlot& slot, uint32 chunk, uint32 available) const noexcept;
    void SignalStreamRefill() noexcept;
    void FinishMaster(Span<float> interleavedOut) noexcept;
    void PublishBlockEnd() noexcept;

    AudioMixerConfig config_;
    // 제어 스레드 상태.
    Array<ControlVoice> controlVoices_;
    Array<ClipSlot> clips_;
    uint64 nextSequence_ = 0;
    uint32 nextVoiceSearch_ = 0;
    uint32 controlStopAllEpoch_ = 0;
    // 공유 원자값.
    Array<SharedVoice> sharedVoices_;
    uint64 stopAllWord_ = 0;
    // RT가 마지막으로 처리한 stop-all epoch다(RT → 제어). 제어는 아직 처리되지 않은 epoch의 fade와
    // 새 fade 중 짧은 쪽을 낸다.
    uint32 publishedStopAllEpoch_ = 0;
    // 제어가 마지막으로 낸 stop-all fade다(제어 스레드).
    uint32 controlStopAllFade_ = 0;
    uint64 completedMixFrame_ = 0;
    uint64 counterSnapshot_[CounterSnapshotWords] = {};
    // RT 스레드 상태.
    Array<VoiceSlot> voices_;
    PlanarBuffer inputScratch_;
    PlanarBuffer voiceScratch_;
    PlanarBuffer master_;
    DynamicsProcessor limiter_;
    GainRamp masterGain_;
    uint64 mixFrame_ = 0;
    uint32 processedStopAllEpoch_ = 0;
    // 시험 hook: seqlock 내용을 읽은 뒤 version을 다시 읽기 전에 부른다(찢어진 읽기 주입). 평소
    // nullptr이다.
    void (*seqlockReadHook_)(AudioMixer& mixer, uint32 voiceIndex) noexcept = nullptr;
    AudioMixerCounters counters_;
};

} // namespace rocks::audio
