#pragma once

#include <Audio/AudioFormat.h>
#include <Audio/DynamicsProcessor.h>
#include <Audio/Panner.h>
#include <Base/NumericLimits.h>

// 믹서 설정·handle·명령·파라미터·결과 형식이다(Docs/Design/AudioSystem 3.22절).
namespace rocks::audio {

// 블록 크기 Q의 기본값이다(계획 §18). 허용값은 64·128·256·512·1024다.
inline constexpr uint32 DefaultMixBlockFrames = 256;
inline constexpr uint32 MinMixBlockFrames = 64;
inline constexpr uint32 MaxMixBlockFrames = 1024;

// voice·clip 표 크기의 기본값과 상한이다.
inline constexpr uint32 DefaultMaxVoices = 128;
inline constexpr uint32 MaxMixerVoices = 1024;
inline constexpr uint32 DefaultMaxClips = 256;
inline constexpr uint32 MaxMixerClips = 65536;

// 한 블록을 명령 offset으로 나눈 구간 수의 상한이다(3.3절 AD16).
inline constexpr uint32 MaxBlockSegments = 16;

// clip(메모리 PCM)의 채널 수 상한(AU1)과 원본 rate 범위다.
inline constexpr uint32 MaxClipChannels = 2;
inline constexpr uint32 MinClipSampleRate = 1000;
inline constexpr uint32 MaxClipSampleRate = 384000;

// voice 파라미터 범위다. pitch는 UE GlobalMin/MaxPitchScale 기본값이다.
inline constexpr float MaxVoiceGain = 4.0f;

// 마스터 이득의 상한과 기본값이다(SetMasterGain).
inline constexpr float MaxMasterGain = 4.0f;
inline constexpr float DefaultMasterGain = 1.0f;
inline constexpr float MinPitch = 0.4f;
inline constexpr float MaxPitch = 2.0f;

// 제어 쪽 기본값이다. stop fade는 UE au.SourceFadeMin(512 frame)이다.
inline constexpr uint32 DefaultStopFadeFrames = 512;
inline constexpr uint32 DefaultParamRampFrames = 256;

// 장면 일시정지의 fade 길이다(3.44.5절 — 0에서 1까지 전체 길이, 중간에서 바뀌면 남은 비율만큼).
inline constexpr uint32 PauseFadeFrames = 64;

// 다음 블록 시작에 실행하라는 mixFrame 표지다. 호출자가 RenderBlock 전에 블록 시작 frame으로
// 바꾼다.
inline constexpr uint64 ImmediateMixFrame = NumericLimits<uint64>::Max();

// 아직 정해지지 않은 mix frame(시작 전·끝 전)이다.
inline constexpr uint64 UnsetMixFrame = NumericLimits<uint64>::Max();

// voice 슬롯 handle이다. generation 0은 무효다.
struct AudioVoiceHandle {
    uint32 index = 0;
    uint32 generation = 0;
};

class AudioStreamRing;

} // namespace rocks::audio

namespace rocks::platform {

class RealtimeWakeSignal;

} // namespace rocks::platform

namespace rocks::audio {

// 스트림 원본이 시작하는 링 수위(frame)다(3.40.1절 시작 대기 — StreamRingMinFrames의 절반).
inline constexpr uint32 StreamStartFrames = 4096;

// clip 표 handle이다. generation 0은 무효다.
struct AudioClipId {
    uint32 index = 0;
    uint32 generation = 0;
};

// 소유하지 않는 planar float PCM이다. 메모리는 UnregisterClip이 성공할 때까지 살아 있어야 한다.
// loopBegin·loopEnd가 둘 다 0이면 loop 구간은 clip 전체다.
struct AudioClipDesc {
    const float* channels[MaxClipChannels] = {};
    uint32 channelCount = 0;
    uint32 frameCount = 0;
    uint32 sampleRate = 0;
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
};

// 스트림 clip이다(3.40.1절). ring은 RegisterStreamClip이 성공한 뒤 UnregisterClip이 성공할 때까지
// 살아 있어야 한다. 링의 첫 frame은 startFrame이고, 생산자가 재생 순서대로(loop면 loop 구간을
// 감아서) 쓴다. startFrame·loop는 PreparePlay에 같은 값을 넘겨야 한다(생산자와 RT가 같은 값을 쓰게
// 한 곳에 둔다). loopBegin·loopEnd가 둘 다 0이면 loop 구간은 clip 전체다.
struct AudioStreamClipDesc {
    AudioStreamRing* ring = nullptr;
    uint32 channelCount = 0;
    uint32 frameCount = 0;
    uint32 sampleRate = 0;
    uint32 loopBegin = 0;
    uint32 loopEnd = 0;
    uint32 startFrame = 0;
    bool loop = false;
};

enum class AudioPanMode : uint8 {
    // mono는 양쪽 0.5(UE Linear 업믹스), stereo는 L→L·R→R.
    Direct,
    // 방위각 equal-power(3.20.6절). mono clip은 azimuthDegrees 하나, stereo clip은 L 채널
    // azimuthDegrees·R 채널 rightAzimuthDegrees(stereo spread — 3.42.6절). 3채널 이상 clip은 받지
    // 않는다.
    Azimuth
};

// voice 파라미터다. 거리 감쇠·분류 gain은 제어 쪽이 gain에 곱해 넣는다.
struct AudioVoiceParams {
    float gain = 1.0f;
    float pitch = 1.0f;
    AudioPanMode panMode = AudioPanMode::Direct;
    float azimuthDegrees = 0.0f;
    float rightAzimuthDegrees = 0.0f;
    float nonSpatializedAmount = 0.0f;
    // 비공간화 비율로 섞는 이득의 배치다(3.42.6절).
    AudioOmniMode omniMode = AudioOmniMode::OmniDirectional;
    float lowPassCutoffHz = 20000.0f;
    float highPassCutoffHz = 0.0f;
    // 장면 일시정지다(3.44.5절). 켜지면 PauseFadeFrames 동안 줄인 뒤 원본을 읽지 않고(출력 0),
    // 꺼지면 같은 길이로 이어 낸다. 시작 전 voice는 시작을 미룬다. 정지 중 voice는 일시정지되면
    // 소리 없이 정지 fade만 마친다.
    bool paused = false;
};

enum class AudioRtCommandType : uint8 {
    Play,
    Stop,
    SetParams,
    SetMasterGain
};

inline constexpr uint32 AudioRtCommandTypeCount = 4;

// 믹서에 넘기는 고정 크기 POD 명령이다(3.5.2절). 문자열·소유 포인터를 담지 않는다. 제어
// API(PreparePlay· MakeStop·MakeSetParams·MakeSetMasterGain)가 만들고 sequence를 매긴다. 손으로
// 채운 명령은 시험 전용이다.
// - Play: params가 초기 파라미터, clip 묶음(clipIndex·startFrame·loop)과 제출 시점의 stopAllEpoch를
// 싣는다.
// - Stop: fadeFrames(0이면 그 frame에서 바로 끝).
// - SetParams: params와 rampFrames.
// - SetMasterGain: masterGain(선형)과 rampFrames.
struct AudioRtCommand {
    AudioRtCommandType type = AudioRtCommandType::Play;
    AudioVoiceHandle voice;
    uint64 mixFrame = ImmediateMixFrame;
    uint64 sequence = 0;
    AudioVoiceParams params;
    uint32 rampFrames = DefaultParamRampFrames;
    uint32 fadeFrames = DefaultStopFadeFrames;
    float masterGain = DefaultMasterGain;
    uint32 clipIndex = NumericLimits<uint32>::Max();
    uint32 startFrame = 0;
    bool loop = false;
    uint32 stopAllEpoch = 0;
};

enum class AudioVoiceState : uint8 {
    Idle,
    Reserved,
    Prepared,
    Playing,
    Stopping,
    Finished
};

enum class AudioVoiceEndReason : uint8 {
    None,
    NaturalEnd,
    // Stop 명령·RequestStop.
    Stopped,
    // StopAll(장치 stop-all epoch).
    StoppedAll,
    // clip 묶음이 잘못된 Play(제어 API를 거치지 않은 손 명령).
    StartFailed,
    // 스트림 링이 Failed이거나 Ended인데 남은 PCM이 모자람(3.40.1절).
    StreamFailed
};

// QueryVoice 결과다. 제어 쪽은 RT의 Stopping을 따로 보지 못하므로 재생 중이면 Playing이다.
struct AudioVoiceStatus {
    AudioVoiceState state = AudioVoiceState::Idle;
    uint64 startMixFrame = UnsetMixFrame;
    uint64 endMixFrame = UnsetMixFrame;
    AudioVoiceEndReason endReason = AudioVoiceEndReason::None;
    // terminal을 게시한 블록의 끝 frame이다. CompletedMixFrame()이 이 값에 닿아야 voice를 놓을 수
    // 있다.
    uint64 releaseMixFrame = UnsetMixFrame;
    // 일시정지로 원본을 읽지 않은 누적 mix frame과 그 값이 맞는 frame, 그 frame에 Paused였는지다
    // (3.44.5절). 이 수명에서 게시한 적이 없으면 unreadAsOfFrame이 UnsetMixFrame이다(누적 0).
    uint64 unreadFrames = 0;
    uint64 unreadAsOfFrame = UnsetMixFrame;
    bool pausedAtAsOf = false;
    // 읽지 않은 누적 seqlock을 MaxUnreadReadAttempts번 모두 찢어진 채로 읽었다(값을 모른다 — 0으로
    // 보지 말고 다음 조회에 다시 본다).
    bool unreadReadFailed = false;
};

// CollectFinished가 돌려주는 terminal 기록이다.
struct AudioVoiceTerminal {
    AudioVoiceHandle voice;
    AudioVoiceEndReason reason = AudioVoiceEndReason::None;
    uint64 endMixFrame = UnsetMixFrame;
};

// 믹서 계수다(진단·시험).
struct AudioMixerCounters {
    uint64 lateCommands = 0;
    uint64 lateFrames = 0;
    uint64 deferredCommands = 0;
    uint64 staleCommands = 0;
    // 상태 표(3.22.4절)가 무시한 명령을 종류별로 센다. 색인은 AudioRtCommandType이다.
    uint64 ignoredCommands[AudioRtCommandTypeCount] = {};
    // 홀수 version·찢어진 읽기로 건너뛴 seqlock 읽기 수다.
    uint64 skippedSeqlockReads = 0;
    uint64 clampedParams = 0;
    uint64 nonFiniteSamples = 0;
    uint64 clippedSamples = 0;
    // 스트림 원본이 링에 PCM이 없어 무음으로 낸 frame 수와, 시작 수위를 기다린 frame
    // 수다(3.40.1절).
    uint64 streamStarvedFrames = 0;
    uint64 streamStartDelayFrames = 0;
    // 지금 일시정지로 원본을 읽지 않는(Paused·시작 미룸) voice 수다(누적이 아닌 현재값 — 3.44.5절).
    uint64 pausedVoices = 0;
};

// 마스터 limiter 기본 프리셋이다(3.22.7절): −1 dBFS, 지연 0, 즉시 어택, 100 ms 릴리스.
inline DynamicsSettings DefaultMasterLimiterSettings() noexcept {
    DynamicsSettings settings;
    settings.mode = DynamicsMode::Limiter;
    settings.thresholdDb = -1.0f;
    settings.kneeBandwidthDb = 0.0f;
    settings.peakMode = EnvelopePeakMode::Peak;
    settings.linkMode = DynamicsLinkMode::Peak;
    settings.attackMs = 0.0f;
    settings.releaseMs = 100.0f;
    settings.lookaheadMs = 0.0f;
    settings.analogMode = false;
    return settings;
}

struct AudioMixerConfig {
    uint32 sampleRate = DefaultMixSampleRate;
    ChannelLayout layout = ChannelLayout::Stereo;
    uint32 blockFrames = DefaultMixBlockFrames;
    uint32 maxVoices = DefaultMaxVoices;
    uint32 maxClips = DefaultMaxClips;
    DynamicsSettings masterLimiter = DefaultMasterLimiterSettings();
    bool masterLimiterEnabled = true;
    // 스트림 링이 refill 수위(용량의 절반) 아래로 내려가면 블록 끝에 Signal할 신호다(디코드
    // 서비스가 기다린다 — 3.40.1절). null이면 알리지 않는다(Inline 시험). 믹서보다 오래 살아야
    // 한다.
    platform::RealtimeWakeSignal* streamRefillSignal = nullptr;
};

// RenderBlock 결과다. consumedCommands는 commands 앞에서부터 소비한 수다.
struct RenderBlockResult {
    uint32 consumedCommands = 0;
};

} // namespace rocks::audio
