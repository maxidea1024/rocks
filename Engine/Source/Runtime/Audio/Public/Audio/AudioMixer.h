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
// - 제어 쪽(제어 스레드 하나): AcquireVoice·PreparePlay·Make*·PublishParams·PublishRoute·
//   RequestStop·StopAll·ReleaseVoice·RegisterClip·UnregisterClip·QueryVoice·CollectFinished·
//   AcquireBus·ReleaseBus·PublishBusParams·PublishBusPlan·AdoptedPlanSerial·CountersSnapshot·
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
    // - route(3.45.5절): 시작 라우팅이다. 가장 최근에 게시한 plan에 없는 버스, 같은 버스 중복,
    //   범위 밖 값은 InvalidArgument다. route가 없는 형태는 {Master, level 1, post}다.
    Result<AudioRtCommand> PreparePlay(AudioVoiceHandle voice, AudioClipId clip, uint32 startFrame,
                                       bool loop, const AudioVoiceParams& params,
                                       const AudioVoiceRoute& route) noexcept;
    Result<AudioRtCommand> PreparePlay(AudioVoiceHandle voice, AudioClipId clip, uint32 startFrame,
                                       bool loop, const AudioVoiceParams& params) noexcept;

    // Master 하나로 가는 기본 라우팅이다(level 1, post, 필터 없음).
    static AudioVoiceRoute MasterRoute() noexcept;

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

    // 바뀐 라우팅을 라우팅 seqlock에 순번과 함께 출판한다(3.45.5절). 다음 블록 시작에 적용된다.
    // 같은 버스의 level·필터 변화와 새 send는 DefaultParamRampFrames로 ramp하고, 빠진 send는 0으로
    // ramp한 뒤 칸을 비운다. 거부 규칙은 MakeStop과 route 규칙(PreparePlay)을 합친 것이다.
    Result<void> PublishRoute(AudioVoiceHandle voice, const AudioVoiceRoute& route) noexcept;

    // --- 제어 쪽: 버스(3.45.3–3.45.4절) ---

    // Master 칸 handle이다. 늘 plan에 있고 놓을 수 없다.
    static AudioBusHandle MasterBus() noexcept {
        return AudioBusHandle{MasterBusIndex, MasterBusGeneration};
    }

    // 빈 버스 칸을 잡는다. plan에 넣기 전까지 RT가 보지 않는다. 칸이 없으면 CapacityExceeded.
    Result<AudioBusHandle> AcquireBus() noexcept;

    // 버스 칸을 놓고 generation을 올린다. Master는 InvalidArgument, 이전 generation은 StaleHandle,
    // 가장 최근 plan에 있거나 RT가 그 칸이 빠진 plan을 아직 채택하지 않았으면 Busy.
    Result<void> ReleaseBus(AudioBusHandle bus) noexcept;

    // 버스 파라미터를 버스 seqlock에 출판한다. 다음 블록 plan 채택 뒤 읽고 한 블록(Q frame)에 걸쳐
    // ramp한다(그 generation이 같은 블록에 처음 들어오면 바로). 값이 [0, MaxBusGain] 밖이거나
    // 비유한이면 InvalidArgument, 잡지 않은 칸·이전 generation은 StaleHandle.
    Result<void> PublishBusParams(AudioBusHandle bus, const AudioBusParams& params) noexcept;

    // 버스 graph plan을 게시한다(3.45.4절). links는 Master를 뺀 모든 버스다. 버스 중복·없는 부모·
    // 순환·Master·범위 밖 첫 값은 InvalidArgument, 잡지 않은 칸·이전 generation은 StaleHandle,
    // 채택되지 않은 plan이 MaxPendingPlans개면 Busy(다음에 다시 낸다).
    Result<void> PublishBusPlan(Span<const AudioBusLink> links) noexcept;

    // RT가 마지막으로 채택한 plan 일련번호다(ACK). 처음 plan(Master 하나)은 1이다.
    uint64 AdoptedPlanSerial() const noexcept;

    // 가장 최근에 게시한 plan 일련번호다.
    uint64 PublishedPlanSerial() const noexcept {
        return publishedPlanSerial_;
    }

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

    // 블록 하나를 렌더한다(3.22.6절, 3.45.6절 순서: stop-all → mailbox → voice seqlock(파라미터·
    // 라우팅) → plan 채택 → 버스 seqlock → 명령, 블록 끝에 버스 처리 → 마스터).
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

    // 장면 일시정지 상태다(3.44.5절).
    enum class PauseState : uint8 {
        Running,
        // pause gain을 0으로 내리는 중이다(원본은 읽는다).
        PausingOut,
        // 원본을 읽지 않는다(출력 0, 정지 fade만 진행).
        Paused,
        // pause gain을 1로 올리는 중이다.
        ResumingIn
    };

    // voice의 send 칸이다(RT 스레드만 쓴다 — 3.45.5절). used이고 active가 아니면 빠지는 중이다.
    struct SendSlot {
        AudioBusHandle bus;
        GainRamp level{0.0f};
        AudioSendStage stage = AudioSendStage::PostAttenuation;
        bool used = false;
        bool active = false;
        bool lowPassEnabled = false;
        bool highPassEnabled = false;
        // 빠지기 시작한 순서다(넘칠 때 가장 오래된 칸을 끊는다).
        uint64 removalSerial = 0;
        OnePoleLowPass lowPass;
        OnePoleHighPass highPass;
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
        // 일시정지된 Play라 시작을 미뤘다(시작 시각·원본 소비 없음 — 3.44.5절).
        bool startDeferred = false;
        // 일시정지 상태·전용 gain ramp(slot.gain과 따로 — 공간 재생은 묶음마다 gain을 다시 낸다)와
        // 읽지 않은 누적 mix frame, 이번 구간에 게시할 변화가 있는지다.
        PauseState pauseState = PauseState::Running;
        GainRamp pauseGain;
        uint64 unreadFrames = 0;
        bool unreadDirty = false;
        LinearResampler resampler;
        OnePoleLowPass lowPass;
        OnePoleHighPass highPass;
        GainRamp gain;
        GainRamp attenuation;
        GainRamp fade;
        // [원본 채널][출력 채널] 이득 ramp다.
        GainRamp channelGains[MaxClipChannels][MaxClipChannels];
        // 라우팅(3.45.5절): send 칸, 마지막으로 본 라우팅 seqlock version과 적용한 순번.
        SendSlot sends[MaxVoiceSendSlots];
        uint32 lastRouteVersion = 0;
        uint64 lastRouteSequence = 0;
    };

    // seqlock 내용 word 수다: generation, 순번 하위·상위, 파라미터 9개(비트 패턴). AudioMixer.cpp의
    // SeqlockWord 열거와 static_assert로 묶는다.
    static constexpr uint32 SeqlockWords = 12;

    // 라우팅 seqlock 내용 word 수다: generation, 순번 하위·상위, send 수, send마다 RouteSendWords.
    static constexpr uint32 RouteSendWords = 6;
    static constexpr uint32 RouteHeaderWords = 4;
    static constexpr uint32 RouteWords = RouteHeaderWords + RouteSendWords * MaxVoiceSends;

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
        // 읽지 않은 누적 frame seqlock(3.44.5절 — RT가 쓰고 제어가 읽는다): version(홀수 = 쓰는
        // 중)과 generation·Paused 표지·누적·기준 frame.
        uint32 unreadVersion = 0;
        uint32 unreadGeneration = 0;
        uint32 unreadPaused = 0;
        uint64 unreadFrames = 0;
        uint64 unreadAsOfFrame = 0;
        // 라우팅 seqlock(3.45.5절 — 제어가 쓰고 RT가 읽는다).
        uint32 routeVersion = 0;
        uint32 routeContents[RouteWords] = {};
        // 시작 라우팅이다. PreparePlay가 Play 명령을 내기 전에 쓰고 그 수명 동안 다시 쓰지 않는다
        // (명령 링 게시가 RT에 보이게 한다 — 원자값이 아니다).
        AudioVoiceRoute startRoute;
    };

    // 제어 쪽 버스 칸이다.
    struct ControlBus {
        uint32 generation = 1;
        bool acquired = false;
        // 가장 최근에 게시한 plan에 있다.
        bool inLatestPlan = false;
        // 이 칸이 빠진 첫 plan 일련번호다(0 = 빠진 적 없음).
        uint64 removedAtSerial = 0;
    };

    // 버스 seqlock 내용 word 수다: generation, output, wet, dry.
    static constexpr uint32 BusSeqlockWords = 4;

    // 제어와 RT가 원자값으로만 주고받는 버스 칸이다.
    struct SharedBus {
        uint32 version = 0;
        uint32 contents[BusSeqlockWords] = {};
    };

    // 미리 잡은 plan 버퍼다(3.45.4절). 게시 뒤에는 다시 쓸 때까지 바꾸지 않는다. 칸 색인 배열은
    // maxBuses 크기이고 generation 0은 plan에 없다는 뜻이다.
    struct BusPlan {
        explicit BusPlan(Allocator& allocator) noexcept
            : order(allocator), parent(allocator), generation(allocator), initial(allocator) {
        }

        uint64 serial = 0;
        uint32 orderCount = 0;
        Array<uint32> order;
        Array<uint32> parent;
        Array<uint32> generation;
        Array<AudioBusParams> initial;
    };

    // RT 쪽 버스 칸이다.
    struct RtBus {
        uint32 generation = 0;
        GainRamp output;
        GainRamp wet;
        GainRamp dry{0.0f};
        bool touched = false;
        // 이 generation이 이번 블록에 처음 들어왔다(버스 seqlock 값을 ramp 없이 쓴다).
        bool fresh = false;
        uint32 lastVersion = 0;
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
    // 시작 시각을 출판하거나(비스트림) 스트림 시작 대기에 들어간다.
    void BeginPlayback(uint32 index, VoiceSlot& slot, uint64 frame) noexcept;
    // paused 파라미터를 보고 일시정지 상태를 옮긴다(구간 시작마다).
    static void UpdatePauseState(VoiceSlot& slot) noexcept;
    void PublishUnread(uint32 index, const VoiceSlot& slot, uint64 asOfFrame) noexcept;
    void ReadUnread(uint32 index, uint32 generation, AudioVoiceStatus& status) const noexcept;
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
    // 버스 표·plan 버퍼·scratch를 만든다(TryCreate에서만).
    Result<void> CreateBuses(Allocator& allocator) noexcept;

    // 라우팅·버스(AudioMixerBuses.cpp).
    Result<void> ValidateRoute(const AudioVoiceRoute& route) const noexcept;
    void WriteRouteSeqlock(AudioVoiceHandle voice, const AudioVoiceRoute& route) noexcept;
    bool ReadRouteSeqlock(uint32 index, uint32& version, uint32& generation, uint64& sequence,
                          AudioVoiceRoute& route) noexcept;
    void ApplyRouteSeqlock(uint32 index, VoiceSlot& slot) noexcept;
    void StartRoute(uint32 index, VoiceSlot& slot, uint64 playSequence) noexcept;
    void ApplyRoute(VoiceSlot& slot, const AudioVoiceRoute& route, bool immediate) noexcept;
    void MixSends(VoiceSlot& slot, const PlanarView& post, const PlanarView* pre, uint32 offset,
                  uint32 frames) noexcept;
    void AdvanceRemovingSends(VoiceSlot& slot, uint32 frames) noexcept;
    void FreeFinishedSends(VoiceSlot& slot, bool all) noexcept;
    bool HasPreSend(const VoiceSlot& slot) const noexcept;
    bool IsSilentState(const VoiceSlot& slot) const noexcept;
    void AdoptPlan() noexcept;
    void ProcessBusSeqlocks() noexcept;
    void ProcessBuses() noexcept;
    PlanarView BusInput(uint32 index) noexcept;

    AudioMixerConfig config_;
    // 제어 스레드 상태.
    Array<ControlVoice> controlVoices_;
    Array<ClipSlot> clips_;
    Array<ControlBus> controlBuses_;
    // plan 게시용 scratch(칸 → links 색인, 깊이)다.
    Array<uint32> planLinkOf_;
    Array<uint32> planDepth_;
    uint64 publishedPlanSerial_ = 0;
    uint64 nextSequence_ = 0;
    uint32 nextVoiceSearch_ = 0;
    uint32 controlStopAllEpoch_ = 0;
    // 공유 원자값.
    Array<SharedVoice> sharedVoices_;
    Array<SharedBus> sharedBuses_;
    Array<BusPlan> plans_;
    // 게시한 plan {일련번호:56, 버퍼 색인:8}이다(제어 → RT).
    uint64 planWord_ = 0;
    // RT가 채택한 plan 일련번호다(RT → 제어).
    uint64 adoptedPlanSerial_ = 0;
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
    // 버스 입력 버퍼(칸 0은 master_)와 RT 버스 칸, 채택한 plan 버퍼 색인이다.
    Array<PlanarBuffer> busInputs_;
    Array<RtBus> rtBuses_;
    uint32 rtPlan_ = 0;
    // voice send scratch(pre 사본, pre·post pan 출력, send 필터 사본)와 dry 사본, 마스터 gain 곡선.
    PlanarBuffer preScratch_;
    PlanarBuffer prePan_;
    PlanarBuffer postPan_;
    PlanarBuffer sendScratch_;
    PlanarBuffer dryScratch_;
    PlanarBuffer masterGainCurve_;
    uint64 nextRemovalSerial_ = 0;
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
