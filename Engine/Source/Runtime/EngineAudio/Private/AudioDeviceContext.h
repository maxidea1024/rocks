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
        Voiceless
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
        AudioEmitterId emitter;
        // 고정 위치·emitter 재생이면서 감쇠 스냅숏이 있으면 묶음마다 공간 값을 다시 계산한다.
        bool spatialized = false;
        SpatialParams spatial;
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
    };

    struct ControlEmitter {
        uint32 generation = 0;
        Vec3d position;
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
    void UpdateSpatial(Record& record) noexcept;
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
    void DetachVoice(uint32 slot, ControlBatch& batch, uint64 now) noexcept;
    void ReleaseDeferredVoice(uint32 slot, uint64 now) noexcept;
    bool CutStoppingVoice(bool streamOnly) noexcept;
    void EndVoiceless(uint32 slot, PlaybackEndReason reason, ControlBatch& batch) noexcept;
    bool IsAudible(const Record& record) const noexcept;

    // 동시 발음(AudioConcurrency.cpp — 3.41.2절).
    ConcurrencyOutcome EvaluateConcurrency(uint32 slot, const ControlCommand& command,
                                           ControlBatch& batch, uint64 now,
                                           bool realization) noexcept;
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
    // 이 묶음의 audio.concurrency.mindistancedelta(cm)다.
    float minDistanceDelta_ = 0.0f;
    AudioControlStats stats_;
};

} // namespace rocks
