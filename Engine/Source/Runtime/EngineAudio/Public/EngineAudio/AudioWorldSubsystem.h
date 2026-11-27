#pragma once

#include <Core/TypeKey.h>
#include <EngineAudio/AudioOcclusionQuery.h>
#include <EngineAudio/AudioPlayback.h>
#include <AudioAssets/SoundSourceRef.h>
#include <World/World.h>

// World 하나의 오디오 scene이다(Docs/Design/AudioSystem 3.8·3.35.7절, UE UWorld의 AudioDevice 연결·
// UGameplayStatics::PlaySoundAtLocation 대응). 엔진 쪽 AudioEngineSubsystem에 scene을 잡아 두고, 그
// World의 재생·리스너·emitter를 그 scene으로 보낸다. World가 끝나면(OnWorldEndPlay 또는
// Deinitialize 중 먼저) scene을 놓아 그 World 재생을 모두 끝낸다(맵 교체·Tryout Stop의 정리 계약).
//
// 호스트가 World Subsystem 목록에 넣는다:
//   config.worldSubsystems.TryPushBack(AudioWorldSubsystem::Type());
// 엔진 목록에 AudioEngineSubsystemType()도 있어야 한다. 없으면 모든 재생이 빈 handle이다.
//
// 사용 예:
//   auto* audio = world.GetSubsystem<AudioWorldSubsystem>();
//   (void)audio->PlayAt(footstep, hitLocation);
//
// 스레드: World의 게임 스레드 전용이다.
namespace rocks {

class AudioEngineSubsystem;

class ROCKS_ENGINEAUDIO_API AudioWorldSubsystem final : public WorldSubsystem {
public:
    explicit AudioWorldSubsystem(Allocator& allocator) noexcept;
    ~AudioWorldSubsystem() override;

    // World Subsystem 목록에 넣을 생성 정보다(이름 "Audio", 기본 생성 판정 — Game·Editor·Tryout).
    static WorldSubsystemType Type() noexcept;

    // 엔진 쪽을 찾아 scene을 잡는다. 엔진 쪽이 없어도 성공한다(재생은 빈 handle).
    Result<void> Initialize(World& world) noexcept override;
    void OnWorldEndPlay(World& world) noexcept override;
    // 편집 World처럼 OnWorldEndPlay가 없는 World도 여기서 scene을 놓는다. 바깥 조회 창은 닫혀
    // 있으므로 Initialize에서 보관한 포인터를 쓴다(엔진 Subsystem은 모든 World보다 오래 산다
    // — 3.35.7절).
    void Deinitialize(World& world) noexcept override;

    // 이 World 소유 재생이다. 오류는 AudioEngineSubsystem::Play2D와 같다. 엔진 오디오가 없으면 빈
    // handle(Status는 Finished)을 성공으로 돌려주고, scene이 끝났으면 Ignored handle이다.
    // - Play2D: 감쇠 없는 2D.
    // - PlayAt: 고정 world 위치(cm). 사운드에 감쇠 스냅숏이 없으면 2D다.
    // - PlayAttached: emitter(AudioComponent)를 따라간다.
    Result<PlaybackHandle> Play2D(const SoundSourceRef& sound, PlayOptions options = {}) noexcept;
    Result<PlaybackHandle> PlayAt(const SoundSourceRef& sound, const Vec3d& position,
                                  PlayOptions options = {}) noexcept;
    // 회전(UE PlaySoundAtLocation의 Rotation — 상자·캡슐·원뿔 감쇠 축)을 함께 준다.
    Result<PlaybackHandle> PlayAt(const SoundSourceRef& sound, const Vec3d& position,
                                  const Quatd& rotation, PlayOptions options = {}) noexcept;
    Result<PlaybackHandle> PlayAttached(AudioEmitterId emitter, const SoundSourceRef& sound,
                                        PlayOptions options = {}) noexcept;

    // AudioEngineSubsystem의 같은 이름 함수와 같되 이 World 소유 재생만 다룬다(다른 World·엔진 소유
    // handle은 낡은 handle처럼 아무 일도 하지 않고 Status는 Finished). 엔진 오디오가 없으면 성공(할
    // 일 없음)이다.
    Result<void> Stop(PlaybackHandle handle,
                      uint32 fadeFrames = audio::DefaultStopFadeFrames) noexcept;
    Result<void> SetParameter(PlaybackHandle handle, PlaybackParameter parameter, float value,
                              uint32 rampFrames = audio::DefaultParamRampFrames) noexcept;
    PlaybackStatus Status(PlaybackHandle handle) const noexcept;

    // 주 리스너다(플레이어 카메라 — 게임·호스트가 프레임마다 넣는다). 넣지 않으면 원점·항등
    // 회전이다.
    void SetListener(const Vec3d& position, const Quatd& rotation) noexcept;

    // emitter 등록·이동·해제다(AudioComponent가 부른다). 오류는 AudioEngineSubsystem과 같고, 엔진
    // 오디오가 없거나 scene이 끝났으면 InvalidArgument다.
    Result<AudioEmitterId> RegisterEmitter(const Vec3d& position,
                                           const Quatd& rotation = Quatd::Identity()) noexcept;
    void UpdateEmitter(AudioEmitterId emitter, const Vec3d& position,
                       const Quatd& rotation = Quatd::Identity()) noexcept;
    void UnregisterEmitter(AudioEmitterId emitter) noexcept;

    // 이 World의 차폐 질의 공급자다(3.42.7절 — nullptr이면 차폐되지 않음). 수명은 호출자 책임이고
    // World 해제(scene 해제)가 놓는다.
    void SetOcclusionQuery(AudioOcclusionQuery* query) noexcept;

    // 이 World의 base 믹스다(UE WorldSettings::DefaultBaseSoundMix — 3.43.5절). 지금 base로 걸고,
    // World가 끝나면 엔진 기본 base 믹스로 돌아간다. 오류는 AudioEngineSubsystem::SetBaseMix와
    // 같다.
    Result<void> SetBaseMix(const MixSnapshotRef& mix) noexcept;

    // 이 World의 scene이다(놓은 뒤·엔진 오디오 없음이면 무효 id).
    AudioSceneId Scene() const noexcept {
        return scene_;
    }

private:
    void ReleaseScene() noexcept;

    AudioEngineSubsystem* engine_ = nullptr;
    // 이 World가 base 믹스를 바꿨다(끝날 때 엔진 기본으로 되돌린다).
    bool changedBaseMix_ = false;
    AudioSceneId scene_;
};

} // namespace rocks

ROCKS_DECLARE_NATIVE_TYPE_KEY(rocks::AudioWorldSubsystem, ROCKS_ENGINEAUDIO_API)
