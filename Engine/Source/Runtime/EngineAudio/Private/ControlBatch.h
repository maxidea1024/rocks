#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <EngineAudio/AudioAttenuation.h>
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
    RemoveEmitter,
    // 차폐 질의 응답(3.42.7절 — handle·occluded).
    SetOcclusion,
    // 믹스 스냅숏(3.43.7절 — mix): push·pop·clear·전부 clear·base.
    PushMix,
    PopMix,
    ClearMix,
    ClearAllMixes,
    SetBaseMix,
    // 분류 override(3.43.6절 — mix·categoryKey·value(volume)·pitch·seconds·applyToChildren)와 해제.
    SetMixCategoryOverride,
    ClearMixCategoryOverride,
    // 분류 감쇠 거리 배수(3.43.6절 — categoryKey·assetValue·value·seconds).
    SetCategoryDistanceScale,
    // 재생 단위 버스 send(3.45.9절 — handle·busSends[0]).
    SetBusSend
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
    // 고정 위치 재생의 회전(상자·캡슐·원뿔 감쇠 축 — 3.42.2절)이다.
    Quatd rotation = Quatd::Identity();
    AudioEmitterId emitter;
    float volume = 1.0f;
    float pitch = 1.0f;
    uint32 startFrame = 0;
    uint32 fadeInFrames = 0;
    // 실효 우선순위(옵션 재정의 또는 사운드 값)와 voice를 잃어도 남을지다(3.41.1절).
    float priority = DefaultSoundPriority;
    bool remainActiveIfDropped = false;
    // Play: 분류 사슬과 재생 옵션(3.43.1절 — 사슬은 옵션 > 사운드 > 엔진 기본).
    SoundCategoryChainRef category;
    bool alwaysPlay = false;
    bool isUiSound = false;
    float lowPassFrequency = MaxAttenuationFilterFrequency;
    float highPassFrequency = 0.0f;
    bool hasOcclusionInterval = false;
    float occlusionInterval = 0.0f;
    // 믹스·분류 명령(3.43.6·3.43.7절).
    MixSnapshotRef mix;
    uint64 categoryKey = 0;
    float overridePitch = 1.0f;
    float seconds = 0.0f;
    float assetValue = 1.0f;
    bool applyToChildren = false;
    // Play: 게임 쪽 첫 차폐 검사 결과(occlusionChecked면 occluded — 3.42.7절). SetOcclusion: 응답.
    bool occlusionChecked = false;
    bool occluded = false;
    uint32 occlusionSequence = 0;
    // 동시 발음 소유자와 실효 그룹(옵션 재정의 또는 사운드 것 — 3.41.2절)이다.
    uint64 owner = 0;
    uint32 concurrencyCount = 0;
    SoundConcurrencyEntry concurrency[MaxConcurrencyGroups];
    // Play: 재생 단위 버스 send. SetBusSend: busSends[0] 하나(3.45.7절).
    uint32 busSendCount = 0;
    PlayBusSend busSends[audio::MaxVoiceSends];
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

// audio.occlusion.checkinterval 범위(초)다.
inline constexpr float MinOcclusionCheckSeconds = -1.0f;
inline constexpr float MaxOcclusionCheckSeconds = 60.0f;

// 차폐 설정이다(3.42.7절 — audio.occlusion.*, 묶음마다 읽는다).
struct OcclusionSettings {
    // audio.occlusion.disable(UE au.DisableOcclusion): 질의하지 않고 바로 차폐되지 않음으로 맞춘다.
    bool disabled = false;
    // 장치 mute(UE IsAudioDeviceMuted — ActiveSound.cpp:2178): 질의만 건너뛰고 지금 값은 둔다.
    bool muted = false;
    // 검사 간격(초 — audio.occlusion.checkinterval, UE DefaultOcclusionCheckInterval). 0보다 작으면
    // 첫 검사만이다.
    float checkIntervalSeconds = 0.1f;
    // 차폐 LPF를 0.25배 한다(audio.occlusion.filterscale — UE au.EnableOcclusionFilterScale).
    bool filterScale = false;
};

// 제어 → 게임 차폐 질의 요청이다(3.42.7절). 게임 쪽이 다음 프레임 시작에 scene의 공급자로 답한다.
struct OcclusionRequest {
    PlaybackHandle handle;
    // 재생 기록의 요청 순번이다(응답 명령이 그대로 돌려준다).
    uint32 sequence = 0;
    uint32 scene = 0;
    uint32 sceneEpoch = 0;
    Vec3d source;
    Vec3d listener;
    uint64 owner = 0;
};

// 사용자 볼륨 CVar 값이다(3.43.3절 — SoundUserVolume 색인, None은 1).
struct UserVolumeSettings {
    float values[SoundUserVolumeCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
};

// scene 리스너 스냅숏이다(바뀐 프레임에만).
struct SceneUpdate {
    uint32 index = 0;
    uint32 epoch = 0;
    Vec3d position;
    Quatd rotation = Quatd::Identity();
    // scene 일시정지(World 값 OR SetScenePaused — 3.44.1절)다.
    bool paused = false;
};

// 버스 실행 배수(3.45.8절 — 선형, UE SetSubmixOutputVolume·WetLevel·DryLevel)다. 바뀐 버스만
// 싣는다.
struct BusModifierUpdate {
    uint64 key = 0;
    float volume = 1.0f;
    float wet = 1.0f;
    float dry = 1.0f;
};

// emitter 위치 스냅숏이다(등록·이동한 프레임에만).
struct EmitterUpdate {
    AudioEmitterId emitter;
    Vec3d position;
    Quatd rotation = Quatd::Identity();
};

struct ControlBatch {
    explicit ControlBatch(Allocator& allocator) noexcept
        : scenes(allocator), emitters(allocator), commands(allocator), busModifiers(allocator),
          events(allocator), occlusionRequests(allocator) {
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
    // 장치 전체 focus 배수와 audio.focus.initializeonfirstupdate다(3.42.5절).
    GlobalFocusSettings focus;
    bool focusInitializeOnFirstUpdate = true;
    OcclusionSettings occlusion;
    UserVolumeSettings userVolume;
    // 버스(3.45.8·3.45.9절): Master·BaseDefault 설정(바뀐 묶음에만 — 지금 값 전체)과 실행 배수.
    bool busSettingsChanged = false;
    MixBusRef masterBus;
    MixBusRef baseDefaultBus;
    Array<BusModifierUpdate> busModifiers;
    // 제어 → 게임.
    Array<PlaybackEvent> events;
    // 차폐 질의 요청이다(용량은 미리 잡고 넘치면 다음 묶음에 다시 낸다).
    Array<OcclusionRequest> occlusionRequests;
    AudioControlStats stats;

    // 돌려받은 묶음을 다음 프레임에 쓰도록 비운다(용량은 둔다 — 사운드 참조는 여기서 놓는다).
    void Recycle() noexcept {
        scenes.Clear();
        emitters.Clear();
        commands.Clear();
        busModifiers.Clear();
        busSettingsChanged = false;
        masterBus = MixBusRef{};
        baseDefaultBus = MixBusRef{};
        events.Clear();
        occlusionRequests.Clear();
    }
};

} // namespace rocks
