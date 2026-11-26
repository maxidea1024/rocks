#pragma once

#include "ControlBatch.h"

#include <Audio/AudioMixer.h>
#include <Audio/AudioOutputDevice.h>
#include <Core/Containers/Array.h>
#include <EngineAudio/AudioControlStats.h>
#include <EngineAudio/AudioSpatialization.h>

// 제어 쪽 재생 상태다(Docs/Design/AudioSystem 3.35.6절). 제어 pipe 한 스레드만 부른다(Inline이면
// 게임 스레드, 종료 drain에서는 lane이 멈춘 뒤 게임 스레드). 믹서 제어 면과 장치 Submit을 혼자
// 쓴다.
//
// 수명 순서(3.5.5절): terminal 수거 → Ended 결과 → voice를 놓기 대기 목록에 둠 → ReleaseVoice가
// 되면(ACK ≥ releaseMixFrame) clip 사용 수를 줄임 → 사용 0이면 UnregisterClip → 성공하면 PCM 참조를
// 놓는다. 그래서 AudioRT가 읽을 수 있는 PCM은 해제되지 않는다.
namespace rocks {

class AudioDeviceContext {
public:
    AudioDeviceContext(Allocator& allocator, audio::AudioOutputDevice& device) noexcept;
    AudioDeviceContext(const AudioDeviceContext&) = delete;
    AudioDeviceContext& operator=(const AudioDeviceContext&) = delete;
    ~AudioDeviceContext() noexcept;

    // 표를 미리 잡는다(처리 중 할당을 줄인다). maxActiveVoices는 audio.voices.max다.
    Result<void> Initialize(uint32 maxActiveVoices) noexcept;

    // 묶음 하나를 처리한다: 미룬 Play 재시도 → 명령 → 마스터 gain → 파라미터 게시 → 시작·terminal
    // 수거(batch.events) → voice·clip 회수.
    void Process(ControlBatch& batch) noexcept;

    // 종료: 장치 stop-all epoch를 올린다. 살아 있는 재생의 Ended 사유는 ShuttingDown이다.
    void StopAll(uint32 fadeFrames) noexcept;

    // voice를 쥔 재생·놓기 대기 voice·등록 clip이 모두 없다.
    bool IsIdle() const noexcept;

    // 장치를 닫은 뒤(AudioRT·콜백이 끝난 뒤) 모든 참조를 놓는다. 믹서는 부르지 않는다.
    void Abandon() noexcept;

    AudioControlStats Stats() const noexcept;

private:
    enum class RecordState : uint8 {
        Inactive,
        // voice와 Play 명령을 만들었지만 명령 링이 차서 아직 보내지 못했다.
        Deferred,
        Submitted
    };

    // 사용자 값 ramp다(3.35.6절 — 제어 쪽이 mix clock으로 평가한다). frames가 0이면 target이다.
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
        void StartFrom(float current, float value, uint64 frame, uint32 length) noexcept;
    };

    struct Record {
        uint32 generation = 0;
        RecordState state = RecordState::Inactive;
        SoundSourceRef sound;
        uint32 clip = 0;
        audio::AudioVoiceHandle voice;
        audio::AudioRtCommand play;
        ValueRamp volume;
        ValueRamp pitch;
        uint32 scene = 0;
        uint32 sceneEpoch = 0;
        PlayPlacement placement = PlayPlacement::NonSpatial;
        Vec3d position;
        AudioEmitterId emitter;
        // 고정 위치·emitter 재생이면서 감쇠 스냅숏이 있으면 묶음마다 공간 값을 다시 계산한다.
        bool spatialized = false;
        SpatialParams spatial;
        bool started = false;
        bool paramsDirty = false;
        // 보내기 전에 받은 Stop의 fade다(NoPendingStop이면 없음).
        uint32 pendingStopFade = 0;
        // 처음 받은 정지의 이유다(terminal Stopped를 이 사유로 바꾼다).
        bool stopRequested = false;
        PlaybackEndReason stopReason = PlaybackEndReason::Stopped;
    };

    struct ClipEntry {
        SharedPtr<const SoundPcm> pcm;
        uint32 loopBegin = 0;
        uint32 loopEnd = 0;
        audio::AudioClipId id;
        uint32 users = 0;
        bool registered = false;
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

    void RetryDeferred() noexcept;
    void ApplyPlay(ControlCommand& command, ControlBatch& batch) noexcept;
    void ApplyStop(const ControlCommand& command, ControlBatch& batch) noexcept;
    void ApplySetParameter(const ControlCommand& command, uint64 now) noexcept;
    void ApplyRemoveScene(const ControlCommand& command) noexcept;
    void ApplyRemoveEmitter(const ControlCommand& command) noexcept;
    void ApplySnapshots(const ControlBatch& batch) noexcept;
    void UpdateSpatial(Record& record) noexcept;
    void ApplyMasterGain(float gain) noexcept;
    void PublishParams(uint64 now) noexcept;
    void CollectStarted(ControlBatch& batch) noexcept;
    void CollectTerminals(ControlBatch& batch) noexcept;
    void ReclaimVoices() noexcept;
    void ReclaimClips() noexcept;

    Record* FindRecord(PlaybackHandle handle) noexcept;
    audio::AudioVoiceParams ParamsFor(const Record& record, uint64 frame) const noexcept;
    Result<uint32> AcquireClip(const SoundPlayable& playable) noexcept;
    void ReleaseClipUser(uint32 clip) noexcept;
    void RequestStop(Record& record, uint32 fadeFrames, PlaybackEndReason reason) noexcept;
    bool TrySubmitPlay(uint32 slot) noexcept;
    static void Emit(ControlBatch& batch, PlaybackHandle handle, PlaybackEventKind kind,
                     PlaybackEndReason reason, uint64 mixFrame) noexcept;

    audio::AudioOutputDevice& device_;
    audio::AudioMixer& mixer_;
    uint32 maxActiveVoices_ = 0;
    // 게임 재생 슬롯과 같은 색인이다(MaxPlaybacks).
    Array<Record> records_;
    // voice 색인 → 재생 슬롯 + 1(0 = 없음).
    Array<uint32> voiceOwners_;
    Array<ClipEntry> clips_;
    Array<ControlScene> scenes_;
    Array<ControlEmitter> emitters_;
    Array<ReleasingVoice> releasing_;
    // 미룬 Play의 슬롯(받은 순서 — 순서를 지키려고 앞이 남아 있으면 뒤도 미룬다).
    Array<uint32> deferred_;
    // 시작 결과를 아직 내지 않은 Submitted 재생의 슬롯이다.
    Array<uint32> awaitingStart_;
    Array<audio::AudioVoiceTerminal> terminalScratch_;
    float masterGain_ = audio::DefaultMasterGain;
    // 첫 마스터 gain은 ramp 0으로 보낸다(음소거·비활성 시작에서 처음 블록이 새지 않게).
    bool masterGainSent_ = false;
    AudioControlStats stats_;
};

} // namespace rocks
