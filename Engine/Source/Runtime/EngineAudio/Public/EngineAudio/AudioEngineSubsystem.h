#pragma once

#include <Audio/AudioDecoder.h>
#include <Audio/AudioOutputDevice.h>
#include <Audio/AudioStreamService.h>
#include <AudioAssets/SoundClip.h>
#include <EngineAudio/AudioAttenuation.h>
#include <EngineAudio/AudioOcclusionQuery.h>
#include <AudioAssets/MixBusData.h>
#include <AudioAssets/MixSnapshotData.h>
#include <AudioAssets/SoundCategory.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/BoundedSpscQueue.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/TypeKey.h>
#include <Engine/Engine.h>
#include <EngineAudio/AudioControlStats.h>
#include <EngineAudio/AudioEngineHostOptions.h>
#include <EngineAudio/AudioPlayback.h>
#include <EngineAudio/AudioRunTraits.h>
#include <AudioAssets/SoundSourceRef.h>

// 엔진에 오디오 장치를 붙이는 Subsystem이다(Docs/Design/AudioSystem 3.8·3.35절, UE FAudioDevice·
// FAudioDeviceManager의 엔진 쪽 대응). 장치(실 backend 또는 Null)와 제어 pipe(AudioControl lane)를
// 가지고, 게임 스레드의 재생 요청을 묶음으로 모아 프레임 끝에 제어 쪽에 넘기며, 결과를 다음 프레임
// 시작에 전달한다.
//
// 사용 예(게임 실행 파일):
//   const EngineSubsystemType subsystems[] = {AudioEngineSubsystemType()};
//   config.subsystems = subsystems;
//   ...
//   auto* audio = engine.GetSubsystem<AudioEngineSubsystem>();
//   auto played = audio->Play2D(clickSound);   // 엔진 소유 2D 재생
//
// 스레드: 공개 함수는 모두 게임 스레드에서 부른다.
namespace rocks {

class AssetLease;
class AssetManager;
class AudioDeviceContext;
class Package;
struct ControlBatch;
struct ControlCommand;
struct OcclusionRequest;

// 진단·시험용 계수다.
struct AudioEngineStats {
    // 빈 묶음이 없어 보내지 못한 프레임 수(제어 쪽 밀림 — 명령은 다음 프레임에 함께 간다).
    uint64 deferredFrames = 0;
    // lane Post가 실패한 수(할당 실패 — 묶음은 다음 프레임에 다시 보낸다).
    uint64 postFailures = 0;
    // 보낸 묶음 수와 돌려받은 묶음 수.
    uint64 submittedBatches = 0;
    uint64 returnedBatches = 0;
    // 같은 묶음 안의 SetParameter를 앞 명령에 합친 수(3.35.5절).
    uint64 mergedParameters = 0;
    // 지금 살아 있는 재생 handle 수.
    uint32 livePlaybacks = 0;
};

class ROCKS_ENGINEAUDIO_API AudioEngineSubsystem final : public EngineSubsystem {
public:
    explicit AudioEngineSubsystem(Allocator& allocator) noexcept;
    AudioEngineSubsystem(const AudioEngineSubsystem&) = delete;
    AudioEngineSubsystem& operator=(const AudioEngineSubsystem&) = delete;
    ~AudioEngineSubsystem() override;

    // 실행 판정 → 장치 열기(실패는 Null) → lane 시작(3.35.9절). 실패: 할당 실패, lane 시작 실패.
    // 실패하면 만든 것을 되돌린다. 엔진에 설정 문맥(Settings)이 있으면 그 문맥에
    // RegisterEngineAudioSettings가 되어 있어야 한다(모듈 RegisterSettings — 없으면 CVar 읽기가
    // Fatal이다). 설정 문맥이 없으면 기본값을 쓴다.
    Result<void> Initialize(Engine& engine) noexcept override;
    // 돌려받은 묶음의 결과를 전달하고 Ignored 재생을 끝낸다.
    void OnFrameBegin(Engine& engine) noexcept override;
    // 마스터 gain을 붙여 이번 프레임 묶음을 제어 쪽에 보낸다.
    void OnFrameEnd(Engine& engine) noexcept override;
    // 새 재생을 닫는다(ShuttingDown).
    void BeginShutdown(Engine& engine) noexcept override;
    // 제어 pipe drain → stop-all → 꼬리 대기(상한 ShutdownDrainTimeoutMs) → 장치 Close → 참조 해제.
    // 실패하지 않는다(3.35.9절 — 수명 장벽은 backend Stop과 AudioRT join이다).
    Result<void> CompleteInFlightWork(Engine& engine) noexcept override;
    // CompleteInFlightWork를 거치지 않았으면(엔진 초기화 실패 정리) 같은 drain을 먼저 한다.
    void Deinitialize(Engine& engine) noexcept override;

    // 엔진 소유 2D 재생이다(World 교체를 넘는다 — 세션 음악·UI). 오류: 빈 사운드·범위 밖
    // 옵션(InvalidArgument), 재생 표 가득(CapacityExceeded), BeginShutdown 뒤(ShuttingDown), 할당
    // 실패. 장치가 없으면 Ignored handle이다.
    Result<PlaybackHandle> Play2D(const SoundSourceRef& sound, PlayOptions options = {}) noexcept;

    // 재생을 멈춘다(fadeFrames 동안 줄인 뒤 Ended(Stopped)). 낡은 handle은 아무 일도 하지 않는다.
    // 오류: 무효 handle(generation 0)은 InvalidArgument.
    Result<void> Stop(PlaybackHandle handle,
                      uint32 fadeFrames = audio::DefaultStopFadeFrames) noexcept;

    // volume([0, MaxVoiceGain])·pitch([MinPitch, MaxPitch]) 배수를 rampFrames 동안 바꾼다. 낡은
    // handle은 아무 일도 하지 않는다. 오류: 무효 handle·범위 밖·비유한 값은 InvalidArgument.
    Result<void> SetParameter(PlaybackHandle handle, PlaybackParameter parameter, float value,
                              uint32 rampFrames = audio::DefaultParamRampFrames) noexcept;

    PlaybackStatus Status(PlaybackHandle handle) const noexcept;

    // handle이 지금 그 scene 소유의 살아 있는 재생인지다(World 쪽 Stop·SetParameter·Status가 다른
    // World·엔진 소유 재생을 건드리지 않게 확인한다).
    bool IsPlaybackInScene(PlaybackHandle handle, AudioSceneId scene) const noexcept;

    // --- World 연결(AudioWorldSubsystem이 부른다 — 3.35.7절) ---

    // World 하나의 scene을 잡는다. 표가 차면 무효 id다(그 World 재생은 모두 Ignored).
    AudioSceneId RegisterScene() noexcept;

    // scene을 놓는다(멱등, 낡은 id는 아무 일도 하지 않는다). 그 scene 재생은 SceneRemoved로 끝나고,
    // 결과 콜백은 이 자리에서 비워져 더 불리지 않으며 Status는 바로 Finished다(파괴된 World를 잡은
    // 콜백이 뒤에 불리지 않게 — 3.35.4절). 그 scene의 emitter도 모두 놓는다.
    void UnregisterScene(AudioSceneId scene) noexcept;

    // scene 리스너다(UE 좌표·cm). 부르지 않으면 원점·항등 회전이다. 낡은 id는 무시한다.
    void SetSceneListener(AudioSceneId scene, const Vec3d& position,
                          const Quatd& rotation) noexcept;

    // scene 일시정지다(3.44.1절 — UE SetGamePaused의 오디오 쪽). 실효값은 이 값 OR World 값(World의
    // pause, 디버그 단일 스텝 frame은 아님)이다. 일시정지된 scene의 UI 아닌 재생(PlayOptions
    // isUiSound·분류 UI 표지가 아닌 재생 — 정지 중 포함)은 소리와 시계(fade·ramp·재생 위치)가
    // 멈추고, 풀면 멈춘 곳에서 이어진다. 다음 프레임 끝 묶음에 실린다. 예: 일시정지 메뉴를 열 때
    // SetScenePaused(scene, true), 닫을 때 false. 오류: 죽은·무효 scene과 엔진 scene은
    // InvalidArgument.
    Result<void> SetScenePaused(AudioSceneId scene, bool paused) noexcept;

    // 부착 재생의 위치·회전 원천을 잡는다(회전은 상자·캡슐·원뿔 감쇠의 축 — 3.42.2절, UE component
    // 회전). 오류: 죽은·무효 scene, 비유한 위치, 0 quaternion은 InvalidArgument, 표
    // 가득(MaxAudioEmitters)은 CapacityExceeded.
    Result<AudioEmitterId> RegisterEmitter(AudioSceneId scene, const Vec3d& position,
                                           const Quatd& rotation = Quatd::Identity()) noexcept;

    // emitter 위치·회전을 바꾼다(다음 프레임 끝 묶음에 실린다). 낡은 id·비유한 위치·0 quaternion은
    // 무시한다.
    void UpdateEmitter(AudioEmitterId emitter, const Vec3d& position,
                       const Quatd& rotation = Quatd::Identity()) noexcept;

    // 장치 전체 focus 배수다(UE FAudioDevice::SetGlobalFocusSettings — AudioDevice.cpp:7578–7589,
    // 3.42.5절). 다음 프레임 끝 묶음부터 쓰인다. 칸이 음수·비유한이면 InvalidArgument다.
    Result<void> SetGlobalFocusSettings(const GlobalFocusSettings& settings) noexcept;

    // 버스(Docs/Design/AudioSystem 3.45.9절)다. 다음 프레임 끝 묶음에 실린다.
    // - SetMasterBus: Master의 값(볼륨·wet·dry)을 정한다(UE UAudioSettings::MasterSubmix). 빈 ref면
    //   엔진 내장 Master다. 부모가 있는 버스는 InvalidArgument다. 이 키를 가리키는 버스·send는
    //   Master로 간다.
    // - SetBaseDefaultBus: 버스를 정하지 않은 재생의 기본 버스다(UE BaseDefaultSubmix). 그 뒤에
    //   voice를 받는 재생부터 쓴다. 빈 ref면 Master다.
    // - SetBusOutputVolume·SetBusWetLevel·SetBusDryLevel: 선형 실행 배수(≥ 0, UE
    //   SetSubmixOutputVolume 등)다. 버스가 아직 없어도 키로 남는다. 빈 ref·음수·비유한은
    //   InvalidArgument, 배수 표(MaxBusModifiers)가 차면 CapacityExceeded다.
    // - SetBusSend: 재생 단위 send(UE UAudioComponent::SetSubmixSend)다. 같은 버스가 사운드 send에
    //   있으면 level만 바꾸고, 없으면 덧붙인다. 끝난 재생은 아무 일도 하지 않는다(Stop과 같다).
    // 오류: 사슬 검사 실패(ValidateMixBusChain).
    Result<void> SetMasterBus(MixBusRef bus) noexcept;
    Result<void> SetBaseDefaultBus(MixBusRef bus) noexcept;
    Result<void> SetBusOutputVolume(const MixBusRef& bus, float volume) noexcept;
    Result<void> SetBusWetLevel(const MixBusRef& bus, float level) noexcept;
    Result<void> SetBusDryLevel(const MixBusRef& bus, float level) noexcept;
    Result<void> SetBusSend(PlaybackHandle handle, const MixBusRef& bus, float level) noexcept;

    // 엔진 기본 분류다(UE UAudioSettings::DefaultSoundClass — 3.43.1절). 분류가 없는 사운드의 다음
    // Play부터 쓴다. 빈 ref면 분류 없음이다. 오류: 사슬 검사 실패(ValidateSoundCategoryChain).
    Result<void> SetDefaultCategory(SoundCategoryChainRef chain) noexcept;

    // 믹스 스냅숏(3.43.7절 — UE PushSoundMixModifier·PopSoundMixModifier·ClearSoundMixModifier(s)·
    // SetBaseSoundMix)이다. 다음 프레임 끝 묶음에 실린다. 오류: 빈 ref·검사 실패 InvalidArgument.
    // 활성 표가 차면 제어 쪽이 무시하고 mixRejects를 센다.
    Result<void> PushMix(const MixSnapshotRef& mix) noexcept;
    Result<void> PopMix(const MixSnapshotRef& mix) noexcept;
    Result<void> ClearMix(const MixSnapshotRef& mix) noexcept;
    void ClearAllMixes() noexcept;
    Result<void> SetBaseMix(const MixSnapshotRef& mix) noexcept;
    // 엔진 기본 base 믹스다(UE DefaultBaseSoundMix — 3.43.5절): 지금 base로 걸고, World가 base를
    // 바꿨다 끝나면(RestoreDefaultBaseMix) 이것으로 돌아간다.
    Result<void> SetDefaultBaseMix(const MixSnapshotRef& mix) noexcept;
    void RestoreDefaultBaseMix() noexcept;

    // 분류 override(UE SetSoundMixClassOverride·ClearSoundMixClassOverride — 3.43.6절)다. volume ≥
    // 0, pitch [0, 8], seconds ≥ 0이 아니면 InvalidArgument다.
    Result<void> SetMixCategoryOverride(const MixSnapshotRef& mix, const SoundCategory& category,
                                        float volume, float pitch, float fadeSeconds,
                                        bool applyToChildren) noexcept;
    Result<void> ClearMixCategoryOverride(const MixSnapshotRef& mix, const SoundCategory& category,
                                          float fadeSeconds) noexcept;
    // 분류 감쇠 거리 배수(UE SetSoundClassDistanceScale — 3.43.6절)다. scale ≥ 0, seconds ≥ 0.
    Result<void> SetCategoryDistanceScale(const SoundCategory& category, float scale,
                                          float seconds) noexcept;

    // scene의 차폐 질의 공급자다(3.42.7절 — nullptr이면 차폐되지 않음). 수명은 호출자 책임이고
    // scene 해제가 놓는다. 낡은 id는 무시한다.
    void SetSceneOcclusionQuery(AudioSceneId scene, AudioOcclusionQuery* query) noexcept;

    const GlobalFocusSettings& FocusSettings() const noexcept {
        return focusSettings_;
    }

    // emitter를 놓는다(멱등). 붙은 재생은 EmitterRemoved로 끝난다.
    void UnregisterEmitter(AudioEmitterId emitter) noexcept;

    // scene 소유 재생이다. 2D(감쇠 없음)·고정 위치·emitter 부착 셋이다. 오류는 Play2D와 같고,
    // 위치가 비유한이면 InvalidArgument다. scene이 죽었거나 emitter가 낡았으면 Ignored handle이다.
    Result<PlaybackHandle> Play2DInScene(AudioSceneId scene, const SoundSourceRef& sound,
                                         PlayOptions options = {}) noexcept;
    Result<PlaybackHandle> PlayAtInScene(AudioSceneId scene, const SoundSourceRef& sound,
                                         const Vec3d& position, PlayOptions options = {}) noexcept;
    // 회전(UE PlaySoundAtLocation의 Rotation — 상자·캡슐·원뿔 감쇠 축)을 함께 주는 고정 위치
    // 재생이다. 0 quaternion은 InvalidArgument다.
    Result<PlaybackHandle> PlayAtInScene(AudioSceneId scene, const SoundSourceRef& sound,
                                         const Vec3d& position, const Quatd& rotation,
                                         PlayOptions options = {}) noexcept;
    Result<PlaybackHandle> PlayAttachedInScene(AudioSceneId scene, AudioEmitterId emitter,
                                               const SoundSourceRef& sound,
                                               PlayOptions options = {}) noexcept;

    // 호스트가 창 포커스 사건에서 부른다(audio.unfocused.volume — 3.35.9절). 기본은 true다.
    void SetApplicationForeground(bool foreground) noexcept {
        foreground_ = foreground;
    }

    // 엔진 디코더 표다(Docs/Design/AudioSystem 3.37.1절). Initialize가 내장 PCM16·ADPCM에 호스트
    // 디코더 등록부(AudioDecoderRegistry — codec 모듈이 Startup에서 채운다)의 factory를 더해
    // 만든다. 장치가 없는 실행에서도 만든다. 쿠킹 데이터 디코드(SoundClip::LoadCookedPcm)에 넘긴다.
    const audio::AudioDecoderTable& DecoderTable() const noexcept {
        return decoders_;
    }

    // 쿠킹 clip 로드(SoundClip::LoadCooked)의 엔진 설정 값이다(audio.stream.defaultloading —
    // 3.40.3절). 설정 문맥이 없으면 기본값이다.
    SoundCookedLoadOptions CookedLoadOptions() const noexcept;

    // 쿠킹 clip을 올리는 엔진 로드 도우미다(3.46.3절 — 쿠킹 clip을 쓰는 호스트는 이것 하나를
    // 부른다). 순서: 이미 같은 쿠킹 데이터면 아무것도 하지 않는다(sidecar를 열지 않는다) →
    // streamedPayload가 있으면 assets.OpenStreamedBulk(lease, ...)로 sidecar를 연다(Unsupported —
    // 그 bulk가 패키지 안이거나 범위 IO가 없음 — 이면 상주 경로) → SoundClip::LoadCooked(reader는
    // package.BulkBytes, 디코더 표·로드 옵션은 엔진 값). package는 lease가 가리키는 clip의
    // 패키지다. 예:
    //   auto loaded = audio->LoadCookedClip(*clip, *package, assets, lease);
    // 오류: OpenStreamedBulk의 그 밖 오류(StaleHandle 등), LoadCooked 오류. 실패하면 clip의 앞
    // 원본을 둔다. 게임 스레드 전용이다.
    Result<void> LoadCookedClip(SoundClip& clip, const Package& package, AssetManager& assets,
                                const AssetLease& lease) noexcept;

    // 스트림 디코드 서비스다(장치가 없으면 null — 진단·시험).
    audio::AudioStreamService* StreamService() noexcept {
        return streams_.Get();
    }

    // 초기화 때 정한 실행 판정이다.
    const AudioRunTraits& RunTraits() const noexcept {
        return traits_;
    }

    // 장치다(audioOutput이 false면 nullptr). 제어 쪽 면은 부르지 않는다 — Info·계수·Null Pump·
    // RenderAvailable(Manual 방식 시험)만 쓴다.
    audio::AudioOutputDevice* Device() noexcept {
        return device_.Get();
    }

    AudioEngineStats Stats() const noexcept;

    // 진단·시험: 지금까지 보낸 묶음의 제어 쪽 처리가 끝날 때까지 기다린다(Thread 제어 방식 시험이
    // 시간 대기 없이 순서를 만든다 — DedicatedLane::Flush). Inline 방식·장치 없음은 할 일이 없다.
    void FlushControl() noexcept;

    // 제어 쪽 계수다(진단·시험). Inline 제어 방식·종료 drain 뒤에는 지금 값이고, Thread 방식에서는
    // 마지막으로 돌려받은 묶음의 사본이다(원자값 없이 읽어도 경합이 없다).
    AudioControlStats ControlStats() const noexcept;

private:
    struct GamePlayback {
        uint32 generation = 1;
        bool live = false;
        PlaybackStatus status = PlaybackStatus::Finished;
        PlaybackEventCallback onEvent;
        // 현재 묶음 안의 SetParameter 명령 색인 + 1(0 = 없음)과 그 묶음 번호다(합치기 — 3.35.5절).
        uint64 parameterBatch = 0;
        uint32 parameterCommand[PlaybackParameterCount] = {};
        uint32 scene = 0;
        uint32 sceneEpoch = 0;
    };

    struct GameScene {
        // 0이면 빈 칸이다.
        uint32 epoch = 0;
        Vec3d listenerPosition;
        Quatd listenerRotation = Quatd::Identity();
        bool listenerDirty = false;
        AudioOcclusionQuery* occlusionQuery = nullptr;
        // 일시정지(3.44.1절): World 값·코드 값과 제어 쪽에 보낸 실효값이다.
        bool worldPaused = false;
        bool codePaused = false;
        bool sentPaused = false;

        bool Paused() const noexcept {
            return worldPaused || codePaused;
        }
    };

    struct GameEmitter {
        uint32 generation = 1;
        bool live = false;
        uint32 scene = 0;
        uint32 sceneEpoch = 0;
        Vec3d position;
        Quatd rotation = Quatd::Identity();
        // 현재 묶음 안의 위치 갱신 색인 + 1(0 = 없음)과 그 묶음 번호다(합치기).
        uint64 updateBatch = 0;
        uint32 updateIndex = 0;
    };

    // Play 요청의 위치 원천이다.
    struct Placement {
        PlayPlacement kind = PlayPlacement::NonSpatial;
        Vec3d position;
        Quatd rotation = Quatd::Identity();
        AudioEmitterId emitter;
    };

    Result<void> OpenDevice(Engine& engine) noexcept;
    Result<void> StartControl() noexcept;
    Result<PlaybackHandle> Accept(const SoundSourceRef& sound, PlayOptions& options,
                                  AudioSceneId scene, const Placement& placement) noexcept;
    bool IsLiveScene(AudioSceneId scene) const noexcept;
    // World 값(AudioWorldSubsystem이 프레임마다 넣는다 — 3.44.1절).
    friend class AudioWorldSubsystem;
    void SetSceneWorldPaused(AudioSceneId scene, bool paused) noexcept;
    // Play 수락 때의 첫 차폐 검사다(3.42.7절 — 공급자·위치가 있을 때만 checked).
    void FirstOcclusionCheck(const SoundSourceRef& sound, AudioSceneId scene,
                             const Placement& placement, uint64 owner,
                             ControlCommand& command) const noexcept;
    void PushEmitterUpdate(uint32 slot) noexcept;
    // 제어 쪽 차폐 요청에 답한다(다음 묶음의 SetOcclusion 명령).
    void AnswerOcclusion(const OcclusionRequest& request) noexcept;
    bool OcclusionDisabled(Engine& engine) const noexcept;
    void FlushDeferredOcclusionAnswers() noexcept;
    // 믹스·분류 명령을 이번 묶음에 넣는다(제어가 없으면 할 일 없음).
    Result<void> PushMixCommand(ControlCommand& command) noexcept;
    void SnapshotScenes() noexcept;
    Result<void> ValidateOptions(const SoundSourceRef& sound,
                                 const PlayOptions& options) const noexcept;
    GamePlayback* FindLive(PlaybackHandle handle) noexcept;
    const GamePlayback* FindLive(PlaybackHandle handle) const noexcept;
    void Deliver(const PlaybackEvent& event) noexcept;
    void FreeSlot(uint32 slot) noexcept;
    float MasterGainTarget(Engine& engine) const noexcept;
    void SubmitCurrent() noexcept;
    void ProcessBatch(ControlBatch* batch) noexcept;
    void Drain() noexcept;

    Allocator& allocator_;
    AudioRunTraits traits_;
    AudioEngineHostOptions options_;
    audio::AudioDecoderTable decoders_;
    // 장치보다 먼저 선언한다(믹서가 서비스의 깨우기 신호를 쥐므로 장치가 먼저 파괴된다).
    UniquePtr<audio::AudioStreamService> streams_;
    UniquePtr<audio::AudioOutputDevice> device_;
    UniquePtr<AudioDeviceContext> context_;
    // DedicatedLane은 Task 모듈(Private 의존)이라 불투명하게 둔다.
    struct Control;
    UniquePtr<Control> control_;
    Array<GamePlayback> playbacks_;
    Array<uint32> freeSlots_;
    Array<PlaybackHandle> ignored_;
    Array<PlaybackEvent> delivering_;
    Array<GameScene> scenes_;
    Array<uint32> sceneEpochs_;
    Array<GameEmitter> emitters_;
    GlobalFocusSettings focusSettings_;
    SoundCategoryChainRef defaultCategory_;
    MixSnapshotRef defaultBaseMix_;
    // 버스 설정과 실행 배수 표다(3.45.8절 — 묶음에는 바뀐 것만 싣는다).
    struct GameBusModifier {
        uint64 key = 0;
        float volume = 1.0f;
        float wet = 1.0f;
        float dry = 1.0f;
    };

    enum class BusModifierKind : uint8 {
        Volume,
        Wet,
        Dry
    };

    Result<void> SetBusModifier(const MixBusRef& bus, BusModifierKind kind, float value) noexcept;
    MixBusRef masterBus_;
    MixBusRef baseDefaultBus_;
    bool busSettingsDirty_ = false;
    Array<GameBusModifier> busModifiers_;
    // 이번 프레임 끝에 정한 차폐 허용(mute·audio.occlusion.disable이 아님)이다. 다음 프레임의 Play
    // 첫 검사와 요청 응답이 쓴다.
    bool occlusionEnabled_ = true;
    // 묶음에 넣지 못한 차폐 응답이다(다음 프레임 끝에 다시 넣는다 — 제어 쪽 대기가 남지 않게).
    struct DeferredOcclusionAnswer {
        PlaybackHandle handle;
        uint32 sequence = 0;
        bool occluded = false;
    };

    Array<DeferredOcclusionAnswer> deferredOcclusionAnswers_;
    Array<uint32> freeEmitters_;
    // 아직 제어 쪽에 보내지 못한 scene 해제다(명령 할당 실패 — 다음 프레임 끝에 다시 보낸다).
    Array<AudioSceneId> pendingSceneRemovals_;
    bool emitterFullWarned_ = false;
    Array<ControlBatch*> freeBatches_;
    ControlBatch* current_ = nullptr;
    BoundedSpscQueue<ControlBatch*> returned_;
    uint64 batchSerial_ = 1;
    AudioControlStats lastControlStats_;
    bool foreground_ = true;
    bool accepting_ = false;
    // 엔진에 설정 문맥이 있었는지다(CVar를 읽을 수 있다).
    bool hasSettings_ = false;
    bool drained_ = true;
    AudioEngineStats stats_;
};

// 종료 꼬리 판정이다(3.35.9절 4단계): 출력 링이 렌더된 frame을 renderedFrames까지 다 냈는지.
// 소비량에서 underrun 무음을 뺀 값과 비교한다(underrun 무음은 렌더한 소리가 아니다).
ROCKS_ENGINEAUDIO_API bool IsOutputTailPlayed(const audio::AudioOutputRingCounters& counters,
                                              uint64 renderedFrames) noexcept;

// Engine 설정 목록에 넣을 생성 정보다(이름 "Audio").
ROCKS_ENGINEAUDIO_API EngineSubsystemType AudioEngineSubsystemType() noexcept;

} // namespace rocks

ROCKS_DECLARE_NATIVE_TYPE_KEY(rocks::AudioEngineSubsystem, ROCKS_ENGINEAUDIO_API)
