#pragma once

#include "ControlBatch.h"

#include <Audio/AudioMixer.h>
#include <Audio/AudioOutputDevice.h>
#include <Audio/AudioStreamService.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <EngineAudio/AudioControlStats.h>
#include <EngineAudio/AudioSpatialization.h>

// 제어 쪽 재생 상태다(Docs/Design/AudioSystem 3.35.6·3.41절). 제어 pipe 한 스레드만
// 부른다(Inline이면 게임 스레드, 종료 drain에서는 lane이 멈춘 뒤 게임 스레드). 믹서 제어 면과 장치
// Submit을 혼자 쓴다.
//
// 수명 순서(3.5.5절): terminal 수거 → Ended 결과 → voice를 놓기 대기 목록에 둠 → ReleaseVoice가
// 되면(ACK ≥ releaseMixFrame) clip 사용 수를 줄임 → 사용 0이면 UnregisterClip → 성공하면 PCM 참조를
// 놓는다. 그래서 AudioRT가 읽을 수 있는 PCM은 해제되지 않는다.
//
// voice 배정(3.41.1절): 재생 수락과 voice는 따로다. 묶음마다 살아 있는 재생을 들리는 크기 ×
// 우선순위로 정렬해 audio.voices.max까지만 voice를 준다. 상한 밖 one-shot은 끝나고,
// loop·remainActive 재생은 voice 없이(Voiceless) 남아 다시 경쟁한다. 재생에서 떼어 낸 voice는 분리
// 목록에서 fade를 마치고 이벤트 없이 회수된다.
namespace rocks {

class AudioDeviceContext {
public:
    // 보내기 전 Stop이 없다는 표지다.
    static constexpr uint32 NoPendingStop = NumericLimits<uint32>::Max();
    // 가청 거리 밖이면 만들지 않는 짧은 one-shot의 길이 상한(초)이다(UE
    // au.SoundDistanceOptimizationLength — 3.41.1절).
    static constexpr uint32 ShortSoundSeconds = 1;
    // 이 값 이하로 들리는 재생은 voice를 받지 않는다(UE au.WaveInstanceMinVolume =
    // UE_KINDA_SMALL_NUMBER — 3.41.1절 무음 컬링).
    static constexpr float SilentVolumeThreshold = 1.0e-4f;
    // 들리지 않는 재생의 볼륨 가중 우선순위 offset이다(UE GetVolumeWeightedPriority의 P − 201).
    static constexpr float InaudibleWeightOffset = 201.0f;
    // always-play의 볼륨 가중 우선순위(UE FLT_MAX / MAX_VOLUME — SoundWave.cpp:3846–3850)와 동시
    // 발음 우선순위 offset(UE MAX_SOUND_PRIORITY + 1 — ActiveSound.cpp:435–437)이다(3.43.4절).
    static constexpr float AlwaysPlayWeight = NumericLimits<float>::Max() / 4.0f;
    static constexpr float AlwaysPlayConcurrencyOffset = MaxSoundPriority + 1.0f;
    // 스테레오 정규화 배수(UE bApplyNormalizationToStereoSounds ×0.5)다.
    static constexpr float StereoNormalizationGain = 0.5f;
    // 믹스 표·override·분류 거리 배수 표 용량이다(3.43.6·3.43.7절 — 차면 무시하고 센다).
    static constexpr uint32 MaxActiveMixes = 64;
    static constexpr uint32 MaxMixOverrides = 128;
    static constexpr uint32 MaxCategoryDistanceScales = 256;
    // voice를 되찾을 때(SeekRestart 위치) 거는 fade-in 길이(mix frame)다.
    static constexpr uint32 ResumeFadeFrames = audio::DefaultParamRampFrames;
    // 여유가 없을 때 정지 fade 중 voice를 끊는 fade(mix frame)다(UE StopNow 대신 짧은 fade — 끊김
    // 방지).
    static constexpr uint32 MinStopFadeFrames = 64;
    // 동시 발음 그룹 표 용량이다(3.41.2절 — 차면 그 재생은 Failed).
    static constexpr uint32 MaxActiveConcurrencyGroups = 1024;
    // 이 값 이하인 동시 발음 volume scale은 0으로 본다(UE au.Concurrency.MinVolumeScale).
    static constexpr float MinConcurrencyVolumeScale = 1.0e-3f;
    // steal fader의 끝 dB다(UE FVolumeFader Logarithmic 바닥 KINDA_SMALL_NUMBER = −80 dB).
    static constexpr float StealFadeFloorDb = -80.0f;
    // volume scale ramp의 dB 바닥이다(MinConcurrencyVolumeScale = −60 dB).
    static constexpr float ConcurrencyScaleFloorDb = -60.0f;
    // cull 규칙 그룹의 loop가 이 초보다 짧게 voice를 냈으면 steal fade 없이 멈춘다(UE AS 825–856).
    static constexpr double StealImmediateSeconds = 0.1;
    // cull·순위 정렬의 동률 판정 폭이다(UE IsNearlyEqual·UE_KINDA_SMALL_NUMBER).
    static constexpr double ConcurrencyTieTolerance = 1.0e-4;

    // 믹서 범위로 자른 실효 pitch다(사용자 ramp 값 × 사운드 pitch — 믹서가 쓰는 값과 같다).
    static float ClampVoicePitch(float value) noexcept {
        return math::Clamp(value, audio::MinPitch, audio::MaxPitch);
    }

    // streams·decoders는 스트림 재생(3.40.3절)에 쓴다(null이면 스트림 재생은 Failed). 둘 다
    // context보다 오래 살아야 한다.
    AudioDeviceContext(Allocator& allocator, audio::AudioOutputDevice& device,
                       audio::AudioStreamService* streams,
                       const audio::AudioDecoderTable* decoders) noexcept;
    AudioDeviceContext(const AudioDeviceContext&) = delete;
    AudioDeviceContext& operator=(const AudioDeviceContext&) = delete;
    ~AudioDeviceContext() noexcept;

    // 표를 미리 잡는다(처리 중 할당을 줄인다). maxActiveVoices는 audio.voices.max다.
    Result<void> Initialize(uint32 maxActiveVoices) noexcept;

    // 묶음 하나를 처리한다(3.41.4절 순서): 미룬 Play 재시도 → 시작·terminal 수거·회수 → 명령 →
    // 공간·추정 갱신 → voice 배정 → 마스터 gain·파라미터 게시.
    void Process(ControlBatch& batch) noexcept;

    // 종료: 장치 stop-all epoch를 올리고 voice 없는 재생을 끝낸다. 이 뒤로는 수락·voice 배정을 하지
    // 않는다(종료 drain은 수거·회수만 — 3.41.4절).
    void StopAll(uint32 fadeFrames) noexcept;

    // 살아 있는 재생·분리 voice·놓기 대기 voice·등록 clip이 모두 없다.
    bool IsIdle() const noexcept;

    // 장치를 닫고 디코드 서비스를 멈춘 뒤(AudioRT·콜백·디코드 스레드가 끝난 뒤) 모든 참조를 놓는다.
    // 믹서는 부르지 않는다.
    void Abandon() noexcept;

    AudioControlStats Stats() const noexcept;
    AudioControlStats StatsWithoutStreams() const noexcept;

private:
    enum class RecordState : uint8 {
        Inactive,
        // voice와 Play 명령을 만들었지만 명령 링이 차서 아직 보내지 못했다.
        Deferred,
        Submitted,
        // 살아 있지만 voice가 없다(3.41.1절 — 새 재생, 상한 밖 loop·remainActive, 무음 loop).
        Voiceless,
        // 가상이다(3.41.3절 — 가청 거리 밖 loop, 동시 발음 거부·빼앗김). voice 경쟁에 나가지 않고
        // 검사 간격마다 실현을 시도한다.
        Virtual
    };

    // 동시 발음 그룹 모드다(UE FSoundConcurrencyManager 맵 넷 — SoundConcurrency.h:439–450).
    enum class ConcurrencyMode : uint8 {
        // 에셋 그룹(전역 공유).
        Group,
        // 에셋 그룹 × 소유자.
        Owner,
        // 사운드 재정의(사운드마다).
        Sound,
        // 사운드 재정의 × 소유자.
        OwnerPerSound
    };

    // dB 선형 ramp다(동시 발음 volume scale·steal fader — UE SC 200–240·FVolumeFader Logarithmic).
    // 선형값 0은 floorDb로 본다. frames가 0이면 목표값이다.
    struct DecibelRamp {
        float fromDb = 0.0f;
        float toDb = 0.0f;
        float floorDb = 0.0f;
        uint64 startFrame = 0;
        uint32 frames = 0;

        void Set(float linear, float floor) noexcept;
        void StartTo(float targetLinear, uint64 frame, uint32 length) noexcept;
        float At(uint64 frame) const noexcept;
        bool IsRamping(uint64 frame) const noexcept;
    };

    // 재생이 속한 동시 발음 그룹 하나다(멤버십은 재생 기록이 쥔다 — 그룹 표는 수만 센다).
    struct ConcurrencyLink {
        uint32 group = 0;
        // Default 순서의 세대(들어올 때 그룹 수)·Distance/Priority 순서의 자리다.
        uint32 generation = 0;
        // 이 그룹의 volume scale과 그 목표(선형)다.
        DecibelRamp scale;
        float target = 1.0f;
        // 이번 묶음에 들어왔다(Distance/Priority 순서의 첫 목표는 바로 적용 — UE).
        bool fresh = true;
    };

    // 동시 발음 그룹 표의 칸이다.
    struct ConcurrencyGroupState {
        bool used = false;
        ConcurrencyMode mode = ConcurrencyMode::Group;
        uint64 owner = 0;
        uint64 key = 0;
        SoundConcurrencySettings settings;
        uint32 count = 0;
        uint64 lastPlayMixFrame = 0;
    };

    // 제어 쪽 값 ramp다(3.35.6절 — mix clock으로 평가한다). frames가 0이면 target이다.
    struct ValueRamp {
        float from = 1.0f;
        float target = 1.0f;
        uint64 startFrame = 0;
        uint32 frames = 0;

        void Set(float value) noexcept {
            from = value;
            target = value;
            frames = 0;
        }

        float At(uint64 frame) const noexcept;
        bool IsRamping(uint64 frame) const noexcept;
        bool IsRising(uint64 frame) const noexcept;
        void StartFrom(float current, float value, uint64 frame, uint32 length) noexcept;
    };

    // 분류 실효값이다(3.43.2절 — 분류 없음이면 기본값).
    struct CategoryValues {
        float volume = 1.0f;
        float pitch = 1.0f;
        float lowPassFrequency = MaxAttenuationFilterFrequency;
        float distanceScale = 1.0f;
        bool alwaysPlay = false;
        bool isUiSound = false;
        bool isMusic = false;
    };

    enum class MixPhase : uint8 {
        Inactive,
        FadingIn,
        Active,
        FadingOut,
        AwaitingRemoval
    };

    // 활성 믹스 하나다(3.43.7절, UE FSoundMixState). 시각은 mix frame이다.
    struct MixState {
        MixSnapshotRef ref;
        uint32 activeRefs = 0;
        uint32 passiveRefs = 0;
        bool base = false;
        bool remove = false;
        bool hasFadeOut = false;
        MixPhase phase = MixPhase::Inactive;
        float t = 0.0f;
        uint64 fadeInStart = 0;
        uint64 fadeInEnd = 0;
        uint64 fadeOutStart = 0;
        uint64 end = 0;
    };

    // 분류 override 하나다(3.43.6절 — UE FSoundMixClassOverride).
    struct MixOverride {
        uint64 mixKey = 0;
        uint64 categoryKey = 0;
        float targetVolume = 1.0f;
        float targetPitch = 1.0f;
        uint32 fadeFrames = 0;
        bool applyToChildren = false;
        bool applied = false;
        bool clearing = false;
        bool remove = false;
        ValueRamp volume;
        ValueRamp pitch;
    };

    // 이번 묶음 믹스 조정 하나다(보간·override가 반영된 배수).
    struct MixAdjustment {
        uint64 categoryKey = 0;
        float volume = 1.0f;
        float pitch = 1.0f;
        float lowPassFrequency = MaxAttenuationFilterFrequency;
        bool applyToChildren = false;
    };

    // 걸린 분류 감쇠 거리 배수다(3.43.6절 — 장치 수명 동안 남는다).
    struct CategoryScale {
        uint64 key = 0;
        ValueRamp ramp;
    };

    struct CategoryCache {
        const SoundCategoryChain* chain = nullptr;
        CategoryValues values;
    };

    // 지난 묶음에 passive로 건 믹스다.
    struct PassiveMix {
        uint64 mixKey = 0;
        bool seen = false;
    };

    struct Record {
        uint32 generation = 0;
        RecordState state = RecordState::Inactive;
        SoundSourceRef sound;
        uint32 clip = 0;
        audio::AudioVoiceHandle voice;
        audio::AudioRtCommand play;
        // 사용자 volume·pitch ramp(SetParameter·fadeInFrames)와 voice 복귀 fade-in(사용자 ramp와
        // 따로 — 사용자 SetParameter가 덮지 않게, 3.41.1절)이다.
        ValueRamp volume;
        ValueRamp pitch;
        ValueRamp resume;
        uint32 scene = 0;
        uint32 sceneEpoch = 0;
        PlayPlacement placement = PlayPlacement::NonSpatial;
        Vec3d position;
        Quatd rotation = Quatd::Identity();
        AudioEmitterId emitter;
        // 고정 위치·emitter 재생이면서 감쇠 스냅숏이 있으면 묶음마다 공간 값을 다시 계산한다.
        bool spatialized = false;
        SpatialParams spatial;
        // focus(3.42.5절): 계수와 첫 갱신 표지·마지막 갱신 mix frame, 지금 배수다.
        float focusFactor = 1.0f;
        bool focusInitialized = false;
        uint64 focusMixFrame = 0;
        FocusScales focus;
        // 분류(3.43절): 수락 때의 사슬, 이번 묶음 실효값, 옵션 always-play·UI와 실효값, 재생
        // LPF·HPF, 차폐 간격 재정의, 처음 배정됐는지(passive 다시 걸기).
        SoundCategoryChainRef category;
        CategoryValues categoryValues;
        bool optionAlwaysPlay = false;
        bool optionUiSound = false;
        bool alwaysPlay = false;
        bool isUiSound = false;
        float playbackLowPass = MaxAttenuationFilterFrequency;
        float playbackHighPass = 0.0f;
        bool hasOcclusionInterval = false;
        float occlusionInterval = 0.0f;
        bool everAssigned = false;
        // 차폐(3.42.7절): 응답 대기, 첫 결과를 받음, 마지막 요청 mix frame, LPF·볼륨 보간(목표는
        // ramp target)이다.
        bool occlusionPending = false;
        bool occlusionChecked = false;
        // 마지막 요청 순번이다(늦게 온 옛 요청의 응답을 새 요청의 답으로 받지 않게).
        uint32 occlusionSequence = 0;
        uint64 occlusionCheckFrame = 0;
        ValueRamp occlusionVolume;
        ValueRamp occlusionFilter;
        // 실효 우선순위(3.42.4절): voice 배정 W의 P_v와 동시 발음 우선순위 P_c다.
        float voicePriority = DefaultSoundPriority;
        float concurrencyPriority = DefaultSoundPriority;
        // 지금 voice가 시작했다(started)와 이 재생이 한 번이라도 시작했다(everStarted — Started
        // 이벤트는 한 번만).
        bool started = false;
        bool everStarted = false;
        bool paramsDirty = false;
        // 보내기 전에 받은 Stop의 fade다(NoPendingStop이면 없음).
        uint32 pendingStopFade = 0;
        // 처음 받은 정지의 이유다(terminal Stopped를 이 사유로 바꾼다). 정지 요청 뒤에는 voice
        // 경쟁에 나가지 않는다.
        bool stopRequested = false;
        PlaybackEndReason stopReason = PlaybackEndReason::Stopped;
        // 정지 요청의 가장 짧은 fade다(여유 부족 때 이미 짧은 voice는 끊지 않는다).
        uint32 stopFade = NoPendingStop;
        // 복귀 fade-in을 실제 시작(Started)에서 시작한다(시작 대기·Deferred 동안 0을 유지
        // — 3.41.1절).
        bool resumePending = false;
        // 수락 순번(voice 배정 동률 — 먼저 수락한 쪽이 이긴다)·실효 우선순위·voice를 잃어도 남을지.
        uint64 sequence = 0;
        float priority = DefaultSoundPriority;
        bool remainActive = false;
        // 요청한 시작 위치와 지금 voice가 시작한 위치(원본 frame)다.
        uint32 startFrame = 0;
        uint32 voiceStartFrame = 0;
        // 논리 커서(원본 frame — SeekRestart 복귀)와 그 값이 맞는 mix frame이다. cursorRunning이면
        // 묶음마다 실효 pitch로 진행한다(voice가 시작했거나 voice가 없을 때).
        double cursor = 0.0;
        uint64 cursorMixFrame = 0;
        bool cursorRunning = false;
        // 이번 묶음의 들리는 크기 추정 v와 볼륨 가중 우선순위 W다(3.41.1절).
        float estimate = 0.0f;
        float weight = 0.0f;
        // 동시 발음(3.41.2절): 속한 그룹, 감기지 않는 pitch 보정 재생 시간(mix frame — StopOldest),
        // 그룹 scale의 곱, scale을 빼기 전 크기(StopQuietest), 지금 voice가 시작한 mix frame,
        // steal fader(사용자 ramp와 따로)와 그 끝.
        uint32 linkCount = 0;
        ConcurrencyLink links[MaxConcurrencyGroups];
        double playbackTime = 0.0;
        uint64 playbackMixFrame = 0;
        // voice가 실제로 소리를 낸 pitch 보정 누적 시간(mix frame — UE PlaybackTimeNonVirtualized,
        // steal 즉시 예외)이다.
        double voicedTime = 0.0;
        float concurrencyScale = 1.0f;
        float unscaledEstimate = 0.0f;
        uint64 voiceStartMixFrame = 0;
        bool stealing = false;
        DecibelRamp stealFader;
        uint64 stealFadeEnd = 0;
        // 동시 발음 그룹 항목과 소유자다(가상 재생을 실현할 때 다시 평가한다 — 3.41.3절).
        uint64 owner = 0;
        uint32 concurrencyCount = 0;
        SoundConcurrencyEntry concurrency[MaxConcurrencyGroups];
        // 다음 가상 검사 mix frame과 마지막으로 정한 검사 간격(들리지만 실현이 거부되면 간격을
        // 그대로 다시 쓴다 — UE AVL 339–352)이다.
        uint64 virtualCheckFrame = 0;
        uint64 virtualInterval = 0;
    };

    struct ClipEntry {
        SharedPtr<const SoundPcm> pcm;
        uint32 loopBegin = 0;
        uint32 loopEnd = 0;
        audio::AudioClipId id;
        uint32 users = 0;
        bool registered = false;
        // 스트림 clip(3.40.3절 — 재생마다 하나, 공유하지 않는다): 압축 데이터·청크 공급자·생산자와
        // 서비스 슬롯이다. 디코더가 공급자·데이터를 가리키므로 ResetStream이 디코더부터 놓는다.
        SharedPtr<const SoundStreamData> stream;
        UniquePtr<audio::ResidentAudioChunkSource> chunks;
        UniquePtr<audio::AudioStreamDecoder> decoder;
        uint32 streamSlot = 0;
        bool hasStreamSlot = false;
        // 믹서 해제 뒤 서비스 반납을 기다린다.
        bool draining = false;
    };

    struct ControlScene {
        uint32 epoch = 0;
        Vec3d listenerPosition;
        Quatd listenerRotation = Quatd::Identity();
        // 이번 묶음에 리스너가 forceUpdateDistance보다 많이 움직였다(가상 재생을 바로 검사
        // — 3.41.3절).
        bool listenerJumped = false;
    };

    struct ControlEmitter {
        uint32 generation = 0;
        Vec3d position;
        Quatd rotation = Quatd::Identity();
    };

    struct ReleasingVoice {
        audio::AudioVoiceHandle voice;
        uint32 clip = 0;
    };

    // 재생에서 떼어 낸 voice다(3.41.1절 분리 목록). terminal이 오면 이벤트 없이 놓기 대기로 간다.
    struct DetachedVoice {
        audio::AudioVoiceHandle voice;
        uint32 clip = 0;
        // 떼어 낼 때의 W와 순번(여유 부족 때 끊을 순서)과 지금 정지 fade다.
        float weight = 0.0f;
        uint64 sequence = 0;
        uint32 stopFade = audio::DefaultStopFadeFrames;
        // 떼어 낸 재생의 scene이다(scene 해제가 fade 중 voice도 짧게 끊는다 — 3.41.3절).
        uint32 scene = 0;
        uint32 sceneEpoch = 0;
        // 동시 발음 steal fade를 이 voice에서 마저 한다(재생은 가상이 됐다 — 3.41.2절). 떼어 낼
        // 때의 파라미터에 fader를 곱해 묶음마다 게시하고, 끝나면 믹서 정지 fade다.
        bool fading = false;
        DecibelRamp fader;
        uint64 fadeEnd = 0;
        audio::AudioVoiceParams params;
    };

    // voice 배정 후보다.
    struct VoiceCandidate {
        uint32 slot = 0;
        float weight = 0.0f;
        uint64 sequence = 0;
    };

    // 동시 발음 평가 결과다(3.41.2절).
    enum class ConcurrencyOutcome : uint8 {
        Accepted,
        // 그룹이 받지 않았다(Ended(Concurrency)).
        Rejected,
        // 그룹 표가 찼다(Failed).
        TableFull
    };

    // voice 시작 결과다.
    enum class StartOutcome : uint8 {
        Started,
        // 믹서 voice·스트림 슬롯이 없다(다음 묶음에 다시 — 3.41.1절 여유 부족).
        NoRoom,
        NoStreamSlot,
        // clip 등록 실패 등 시작할 수 없다.
        Failed
    };

    void RetryDeferred() noexcept;
    void ApplyPlay(ControlCommand& command, ControlBatch& batch) noexcept;
    void ApplyStop(const ControlCommand& command, ControlBatch& batch) noexcept;
    void ApplySetParameter(const ControlCommand& command, uint64 now) noexcept;
    void ApplyRemoveScene(const ControlCommand& command, ControlBatch& batch) noexcept;
    void ApplyRemoveEmitter(const ControlCommand& command, ControlBatch& batch) noexcept;
    void ApplySnapshots(const ControlBatch& batch) noexcept;
    // 공간 갱신(3.42.8절 — UE 순서: 우선순위 감쇠 → focus → 거리 감쇠 → 공기 흡수). accepting이면
    // UE 수락 때 규칙(focus 계수는 spatialize·focus일 때만, H = 1 — 3.42.5절)이다.
    void UpdateSpatial(Record& record, uint64 now, bool accepting) noexcept;
    // 분류(AudioCategories.cpp — 3.43.2·3.43.6절).
    CategoryValues ComputeCategory(const SoundCategoryChain* chain, uint64 now) noexcept;
    float CategoryDistanceScale(uint64 key, const SoundCategoryProperties& properties,
                                uint64 now) const noexcept;
    void UpdateCategories(uint64 now) noexcept;
    void ApplySetCategoryDistanceScale(const ControlCommand& command, uint64 now) noexcept;
    // 믹스(AudioMixes.cpp — 3.43.6·3.43.7절).
    uint64 MixFrames(float seconds) const noexcept;
    MixState* FindMix(uint64 key) noexcept;
    void StartMix(MixState& state, uint64 now) noexcept;
    void RetriggerMix(MixState& state, uint64 now) noexcept;
    void PushMixState(const MixSnapshotRef& ref, bool passive, bool retrigger, uint64 now) noexcept;
    void PopMixState(uint64 key, bool passive, uint64 now) noexcept;
    void TryClearMix(MixState& state, uint64 now) noexcept;
    void ApplyMixCommand(const ControlCommand& command, uint64 now) noexcept;
    void ApplyBaseMix(const MixSnapshotRef& mix, uint64 now) noexcept;
    // 믹스·분류 명령인지다(묶음 앞에서 따로 적용한다 — 3.43.8절).
    static bool IsMixCommand(ControlCommandType type) noexcept;
    MixOverride* FindOverride(uint64 mixKey, uint64 categoryKey) noexcept;
    float AdjusterValueOf(uint64 mixKey, uint64 categoryKey, bool pitch,
                          bool interpolated) noexcept;
    void ApplySetMixOverride(const ControlCommand& command, uint64 now) noexcept;
    void ApplyClearMixOverride(const ControlCommand& command, uint64 now) noexcept;
    void UpdateMixes(uint64 now) noexcept;
    void UpdatePassiveMixes(uint64 now) noexcept;
    // 실효 pitch(사용자 × 사운드 × 분류, 자름 — 3.43.2절)와 동시 발음 우선순위(always-play면 +101 —
    // 3.43.4절)다.
    float EffectivePitch(const Record& record, uint64 frame) const noexcept;
    static float ConcurrencyPriorityOf(const Record& record) noexcept;
    // 차폐(3.42.7절): 간격이 된 재생의 질의를 묶음에 싣고, 응답·첫 검사 결과로 목표를 정한다.
    void UpdateOcclusion(ControlBatch& batch, uint64 now) noexcept;
    void ApplySetOcclusion(const ControlCommand& command, uint64 now) noexcept;
    // immediate면 보간 없이 바로 맞춘다(첫 결과·audio.occlusion.disable).
    void ApplyOcclusionResult(Record& record, bool occluded, uint64 now, bool immediate) noexcept;
    float OcclusionFilterAt(const Record& record, uint64 frame) const noexcept;
    void ApplyMasterGain(float gain) noexcept;
    void PublishParams(uint64 now) noexcept;
    void CollectStarted(ControlBatch& batch) noexcept;
    void CollectTerminals(ControlBatch& batch) noexcept;
    void ReclaimVoices() noexcept;
    void ReclaimClips() noexcept;

    // voice 배정(AudioVoiceAllocation.cpp — 3.41.1절).
    void UpdateEstimates(ControlBatch& batch, uint64 now) noexcept;
    void UpdateWeights(uint64 now) noexcept;
    void AssignVoices(ControlBatch& batch, uint64 now) noexcept;
    void HandleSilent(uint32 slot, ControlBatch& batch, uint64 now) noexcept;
    void DropVoice(uint32 slot, ControlBatch& batch, uint64 now) noexcept;
    StartOutcome StartVoice(uint32 slot, uint64 now) noexcept;
    void OnStartFailure(uint32 slot, StartOutcome outcome, ControlBatch& batch) noexcept;
    void DetachVoice(uint32 slot, ControlBatch& batch, uint64 now,
                     uint32 stealFadeFrames = 0) noexcept;
    void ReleaseDeferredVoice(uint32 slot, uint64 now) noexcept;
    bool CutStoppingVoice(bool streamOnly) noexcept;
    void EndVoiceless(uint32 slot, PlaybackEndReason reason, ControlBatch& batch) noexcept;
    // shortSound면 짧은 one-shot 판정(UE SoundIsAudible — AD 6355–6380)이다: PlayWhenSilent는
    // 허용값과 상관없이, focus 거리 배수(에셋·전역)가 1이 아니면 늘 들린다. 아니면 가상화
    // 판정(focus Distance 배수로 나눔)이다.
    bool IsAudible(const Record& record, bool shortSound = false) const noexcept;

    // 동시 발음(AudioConcurrency.cpp — 3.41.2절).
    ConcurrencyOutcome EvaluateConcurrency(uint32 slot, ControlBatch& batch, uint64 now,
                                           bool realization) noexcept;

    // 가상화(AudioVirtualization.cpp — 3.41.3절).
    bool IsVirtualizable(const Record& record) const noexcept;
    // voice를 놓고(steal이면 stealFadeFrames 동안 분리 fade) 그룹에서 빼고 가상으로 둔다.
    void Virtualize(uint32 slot, ControlBatch& batch, uint64 now,
                    uint32 stealFadeFrames = 0) noexcept;
    // 상태를 Virtual로 바꾸고 첫 검사를 정한다(voice·그룹은 이미 정리됐다).
    void EnterVirtual(uint32 slot, uint64 now) noexcept;
    void UpdateVirtualization(ControlBatch& batch, uint64 now) noexcept;
    // 가상 재생을 voice 후보(Voiceless)로 돌린다(공간 값·크기 추정을 지금으로).
    void Realize(Record& record, uint64 now) noexcept;
    float UnscaledEstimate(const Record& record, uint64 now) const noexcept;
    uint64 VirtualCheckDelay(const Record& record) const noexcept;
    uint32 FindConcurrencyGroup(ConcurrencyMode mode, uint64 owner, uint64 key) const noexcept;
    void GatherGroupMembers(uint32 group, uint32 except) noexcept;
    bool CanPlayInGroup(uint32 group, uint32 slot, bool realization, uint64 now,
                        uint32& victim) noexcept;
    void JoinConcurrencyGroup(uint32 slot, uint32 group, uint64 now) noexcept;
    void LeaveConcurrencyGroups(uint32 slot, uint64 now) noexcept;
    void StealPlayback(uint32 slot, bool evict, ControlBatch& batch, uint64 now) noexcept;
    void UpdateConcurrency(ControlBatch& batch, uint64 now) noexcept;
    void CullConcurrencyGroup(uint32 group, ControlBatch& batch, uint64 now) noexcept;
    void RankConcurrencyGroup(uint32 group, uint64 now) noexcept;
    double ListenerDistanceSquared(const Record& record) const noexcept;
    uint32 SecondsToMixFrames(double seconds) const noexcept;
    bool ConcurrencyRamping(const Record& record, uint64 now) const noexcept;

    Record* FindRecord(PlaybackHandle handle) noexcept;
    audio::AudioVoiceParams ParamsFor(const Record& record, uint64 frame) const noexcept;
    Result<uint32> AcquireClip(const SoundPlayable& playable, uint32 startFrame,
                               bool& slotRejected) noexcept;
    Result<uint32> AcquireStreamClip(const SoundPlayable& playable, uint32 startFrame,
                                     bool& slotRejected) noexcept;
    static void ResetStream(ClipEntry& entry) noexcept;
    void ReleaseClipUser(uint32 clip) noexcept;
    void RequestStop(Record& record, uint32 fadeFrames, PlaybackEndReason reason) noexcept;
    bool TrySubmitPlay(uint32 slot) noexcept;
    static void Emit(ControlBatch& batch, PlaybackHandle handle, PlaybackEventKind kind,
                     PlaybackEndReason reason, uint64 mixFrame) noexcept;

    Allocator& allocator_;
    audio::AudioOutputDevice& device_;
    audio::AudioMixer& mixer_;
    audio::AudioStreamService* streams_ = nullptr;
    const audio::AudioDecoderTable* decoders_ = nullptr;
    uint32 maxActiveVoices_ = 0;
    // 게임 재생 슬롯과 같은 색인이다(MaxPlaybacks).
    Array<Record> records_;
    // voice 색인 → 재생 슬롯 + 1(0 = 없음 — 분리 voice도 0이다).
    Array<uint32> voiceOwners_;
    Array<ClipEntry> clips_;
    Array<ControlScene> scenes_;
    Array<ControlEmitter> emitters_;
    Array<ReleasingVoice> releasing_;
    Array<DetachedVoice> detached_;
    // 미룬 Play의 슬롯(받은 순서 — 순서를 지키려고 앞이 남아 있으면 뒤도 미룬다).
    Array<uint32> deferred_;
    // 시작 결과를 아직 내지 않은 Submitted 재생의 슬롯이다.
    Array<uint32> awaitingStart_;
    Array<audio::AudioVoiceTerminal> terminalScratch_;
    Array<VoiceCandidate> candidates_;
    Array<ConcurrencyGroupState> groups_;
    // cull·순위 정렬의 재생 슬롯 scratch다.
    Array<uint32> groupScratch_;
    float masterGain_ = audio::DefaultMasterGain;
    // 첫 마스터 gain은 ramp 0으로 보낸다(음소거·비활성 시작에서 처음 블록이 새지 않게).
    bool masterGainSent_ = false;
    // StopAll 뒤다(수락·voice 배정을 하지 않는다).
    bool shuttingDown_ = false;
    // 다음 수락 순번이다.
    uint64 nextSequence_ = 0;
    // 이 묶음의 audio.concurrency.mindistancedelta(cm)·audio.virtual.* 값이다.
    float minDistanceDelta_ = 0.0f;
    VirtualizationSettings virtual_;
    GlobalFocusSettings focus_;
    bool focusInitializeOnFirstUpdate_ = true;
    OcclusionSettings occlusion_;
    UserVolumeSettings userVolume_;
    // 믹스 표(MaxActiveMixes)·override(MaxMixOverrides)·이번 묶음 조정·분류 거리 배수·사슬
    // 캐시·passive 집합이다(3.43절 — 모두 미리 잡는다).
    Array<MixState> mixes_;
    Array<MixOverride> overrides_;
    Array<MixAdjustment> adjustments_;
    Array<CategoryScale> categoryScales_;
    Array<CategoryCache> categoryCache_;
    Array<PassiveMix> passive_;
    // 믹스 fade·override ramp가 움직인다(분류 있는 재생을 묶음마다 다시 게시).
    bool mixesChanging_ = false;
    // 차폐 요청 순회 시작 slot이다(요청 칸이 찰 때 뒤 slot이 늘 밀리지 않게 묶음마다 이어 간다).
    uint32 occlusionCursor_ = 0;
    AudioControlStats stats_;
};

} // namespace rocks
