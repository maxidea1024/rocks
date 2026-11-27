#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <EngineAudio/AudioControlStats.h>
#include <EngineAudio/AudioPlayback.h>
#include <EngineAudio/AudioSpatialization.h>
#include <AudioAssets/SoundSourceRef.h>

// 게임 ↔ 제어 pipe 일괄 묶음이다(Docs/Design/AudioSystem 3.35.5절). 게임이 한 프레임 명령을 모아
// OnFrameEnd에 넘기고, 제어 쪽이 처리한 뒤 결과를 같은 묶음에 적어 돌려준다. 한 묶음은 한 번에 한
// 쪽만 만진다(넘기기는 lane Post, 돌려주기는 반환 SPSC 큐 — 둘 다 happens-before를 준다).
namespace rocks {

enum class ControlCommandType : uint8 {
    Play,
    Stop,
    SetParameter,
    // scene(World) 해제: 그 scene 재생을 SceneRemoved로 끝낸다(scene·sceneEpoch).
    RemoveScene,
    // emitter 해제: 붙은 재생을 EmitterRemoved로 끝낸다(emitter).
    RemoveEmitter
};

struct ControlCommand {
    ControlCommandType type = ControlCommandType::Play;
    PlaybackHandle handle;
    // Play.
    SoundSourceRef sound;
    uint32 scene = 0;
    uint32 sceneEpoch = 0;
    PlayPlacement placement = PlayPlacement::NonSpatial;
    Vec3d position;
    AudioEmitterId emitter;
    float volume = 1.0f;
    float pitch = 1.0f;
    uint32 startFrame = 0;
    uint32 fadeInFrames = 0;
    // 실효 우선순위(옵션 재정의 또는 사운드 값)와 voice를 잃어도 남을지다(3.41.1절).
    float priority = DefaultSoundPriority;
    bool remainActiveIfDropped = false;
    // 동시 발음 소유자와 실효 그룹(옵션 재정의 또는 사운드 것 — 3.41.2절)이다.
    uint64 owner = 0;
    uint32 concurrencyCount = 0;
    SoundConcurrencyEntry concurrency[MaxConcurrencyGroups];
    // Stop.
    uint32 fadeFrames = 0;
    // SetParameter.
    PlaybackParameter parameter = PlaybackParameter::Volume;
    float value = 0.0f;
    uint32 rampFrames = 0;
};

// 가상화 설정 기본값이다(UE au.VirtualLoops.UpdateRate.Min·Max·PerfDistance·
// ForceUpdateListenerMoveDistance — AudioVirtualLoop.cpp:9–42). CVar 정의도 이 값을 쓴다.
inline constexpr float DefaultVirtualUpdateMinSeconds = 0.1f;
inline constexpr float DefaultVirtualUpdateMaxSeconds = 3.0f;
inline constexpr float DefaultVirtualPerfDistance = 15000.0f;
inline constexpr float DefaultVirtualForceUpdateDistance = 2500.0f;

// 가상화 설정이다(Docs/Design/AudioSystem 3.41.3절, UE au.VirtualLoops.* —
// AudioVirtualLoop.cpp:9–42).
struct VirtualizationSettings {
    // audio.virtual.enabled다(UE au.VirtualLoops.Enabled).
    bool enabled = true;
    // audio.virtual.allowplaywhensilent다(UE bAllowPlayWhenSilent). 끄면 PlayWhenSilent를 Restart로
    // 본다.
    bool allowPlayWhenSilent = true;
    // 검사 간격 하한·상한(초)과 그 사이를 거리로 보간하는 길이(cm)다(UE
    // UpdateRate.Min·Max·PerfDistance).
    float updateMinSeconds = DefaultVirtualUpdateMinSeconds;
    float updateMaxSeconds = DefaultVirtualUpdateMaxSeconds;
    float perfDistance = DefaultVirtualPerfDistance;
    // 리스너가 묶음 사이에 이만큼(cm)보다 많이 움직이면 바로 검사한다(UE
    // ForceUpdateListenerMoveDistance).
    float forceUpdateDistance = DefaultVirtualForceUpdateDistance;
    // 최대 가청 거리가 이 값 이상이면 늘 들린다(UE au.MaxWorldDistance).
    float maxWorldDistance = MaxAudioWorldDistance;
};

// scene 리스너 스냅숏이다(바뀐 프레임에만).
struct SceneUpdate {
    uint32 index = 0;
    uint32 epoch = 0;
    Vec3d position;
    Quatd rotation = Quatd::Identity();
};

// emitter 위치 스냅숏이다(등록·이동한 프레임에만).
struct EmitterUpdate {
    AudioEmitterId emitter;
    Vec3d position;
};

struct ControlBatch {
    explicit ControlBatch(Allocator& allocator) noexcept
        : scenes(allocator), emitters(allocator), commands(allocator), events(allocator) {
    }

    // 게임 → 제어. 스냅숏(scenes·emitters)을 명령보다 먼저 적용한다(같은 프레임에 등록하고 재생한
    // emitter가 위치를 가진다).
    Array<SceneUpdate> scenes;
    Array<EmitterUpdate> emitters;
    Array<ControlCommand> commands;
    float masterGain = 1.0f;
    // audio.concurrency.mindistancedelta(cm — StopFarthest 동거리 폭, 3.41.2절)다.
    float concurrencyMinDistanceDelta = 0.0f;
    // audio.virtual.* 값이다(3.41.3절 — 묶음마다 읽는다).
    VirtualizationSettings virtualization;
    // 제어 → 게임.
    Array<PlaybackEvent> events;
    AudioControlStats stats;

    // 돌려받은 묶음을 다음 프레임에 쓰도록 비운다(용량은 둔다 — 사운드 참조는 여기서 놓는다).
    void Recycle() noexcept {
        scenes.Clear();
        emitters.Clear();
        commands.Clear();
        events.Clear();
    }
};

} // namespace rocks
