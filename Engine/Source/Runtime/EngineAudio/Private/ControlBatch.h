#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <EngineAudio/AudioControlStats.h>
#include <EngineAudio/AudioPlayback.h>
#include <EngineAudio/SoundSourceRef.h>

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

// 재생 위치 원천이다.
enum class PlayPlacement : uint8 {
    // 2D(감쇠·패닝 없음).
    NonSpatial,
    // 고정 world 위치(PlayAt).
    Fixed,
    // emitter 위치(PlayAttached).
    Emitter
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
    // Stop.
    uint32 fadeFrames = 0;
    // SetParameter.
    PlaybackParameter parameter = PlaybackParameter::Volume;
    float value = 0.0f;
    uint32 rampFrames = 0;
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
