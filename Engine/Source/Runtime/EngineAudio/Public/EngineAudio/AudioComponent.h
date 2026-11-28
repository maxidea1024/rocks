#pragma once

#include <EngineAudio/AudioPlayback.h>
#include <AudioAssets/SoundSourceRef.h>
#include <Core/Containers/Span.h>
#include <World/SceneComponent.h>

// 위치를 따라가는 사운드 컴포넌트다(Docs/Design/AudioSystem 3.8.3·3.35.8절, UE UAudioComponent
// 대응). 등록되면 오디오 시스템 상태(SystemStateKind::Audio)로 emitter를 잡고, 위치가 바뀔 때마다
// emitter를 옮긴다. 재생은 그 emitter에 붙으므로 컴포넌트를 해제하면(파괴·맵 교체·GC) 재생이
// 끝난다(UE bStopWhenOwnerDestroyed 기본과 같은 결과).
//
// 영속 사운드 참조(SoftObjectRef + AssetLease)는 AU6에서 준비 티켓과 함께 더한다. 지금은 SetSound로
// 실행 중에 정한다(World 객체는 에셋을 ObjectRef로 가리킬 수 없다).
//
// 사용 예:
//   auto component = world.AddComponent<AudioComponent>(actor);
//   world.With<AudioComponent>(component.Value(), [&](AudioComponent& audio) noexcept {
//       audio.SetSound(engineLoop);
//       return Result<void>::Success();
//   });   // autoActivate면 OnStart에서 재생한다
//
// 스레드: World의 게임 스레드 전용이다.
namespace rocks {

class AudioWorldSubsystem;

ROCKS_CLASS()
class ROCKS_ENGINEAUDIO_API AudioComponent final : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    explicit AudioComponent(Allocator& allocator) noexcept;

    // 사운드 값에 곱하는 배수다(volume [0, MaxVoiceGain], pitch [MinPitch, MaxPitch]).
    ROCKS_PROPERTY() float volumeMultiplier = 1.0f;
    ROCKS_PROPERTY() float pitchMultiplier = 1.0f;
    // OnStart에서 사운드가 있으면 재생한다(UE bAutoActivate 기본 true). 편집 World에는 OnStart가
    // 없어 자동 재생하지 않는다.
    ROCKS_PROPERTY() bool autoActivate = true;
    // 꺼지면(OnDisable) 재생을 멈춘다.
    ROCKS_PROPERTY() bool stopOnDisable = true;
    // 켜면 사운드 우선순위 대신 priority를 쓴다(UE bOverridePriority·Priority — 3.41.1절).
    ROCKS_PROPERTY() bool overridePriority = false;
    ROCKS_PROPERTY() float priority = DefaultSoundPriority;
    // 켜면 one-shot도 voice를 잃을 때 끝나지 않고 남아 다시 경쟁한다(UE
    // bShouldRemainActiveIfDropped).
    ROCKS_PROPERTY() bool remainActiveIfDropped = false;
    // 분류 값과 OR한다(UE bAlwaysPlay·bIsUISound — 3.43.4절).
    ROCKS_PROPERTY() bool alwaysPlay = false;
    ROCKS_PROPERTY() bool isUiSound = false;
    // 재생 LPF·HPF다(UE bEnableLowPassFilter·LowPassFilterFrequency 20000,
    // bEnableHighPassFilter·HighPassFilterFrequency 0 — 3.43.1절, [0, 20000] Hz).
    ROCKS_PROPERTY() bool enableLowPassFilter = false;
    ROCKS_PROPERTY() float lowPassFilterFrequency = MaxAttenuationFilterFrequency;
    ROCKS_PROPERTY() bool enableHighPassFilter = false;
    ROCKS_PROPERTY() float highPassFilterFrequency = 0.0f;
    // 차폐 검사 간격(초 — UE OcclusionCheckInterval 0.1, [−1, 60])이다. 컴포넌트 재생은 늘 이 값을
    // 쓴다.
    ROCKS_PROPERTY() float occlusionCheckInterval = 0.1f;

    // 다음 Play부터 쓸 사운드다. 재생 중인 소리는 바꾸지 않는다(UE SetSound는 멈추고 다시
    // 재생하지만 여기서는 호출자가 Play를 부른다).
    void SetSound(SoundSourceRef sound) noexcept {
        sound_ = Move(sound);
    }

    const SoundSourceRef& Sound() const noexcept {
        return sound_;
    }

    // 다음 Play부터 사운드의 동시 발음 그룹 대신 쓸 그룹이다(UE UAudioComponent::ConcurrencySet —
    // 3.41.2절). 빈 목록이면 사운드 것을 쓴다. 항목은 ConcurrencyGroup::MakeEntry로 만든다. 오류:
    // MaxConcurrencyGroups보다 많으면 CapacityExceeded, 키 0·범위 밖 설정은 InvalidArgument.
    Result<void> SetConcurrency(Span<const SoundConcurrencyEntry> entries) noexcept;

    // 다음 Play부터 사운드 분류 대신 쓸 분류 사슬이다(UE SoundClassOverride — 3.43.1절). 빈 ref면
    // 사운드 것을 쓴다.
    void SetCategory(SoundCategoryChainRef chain) noexcept {
        category_ = Move(chain);
    }

    // 버스 send(UE SetSubmixSend — 3.45.9절)다. 재생 중이면 그 재생에만 내고, 아니면 다음 Play
    // 한 번에 쓸 send로 둔다(같은 버스면 level만 바꾼다 — UE PendingSubmixSends). 빈 ref·음수·
    // 비유한은 InvalidArgument, 서로 다른 버스가 MaxVoiceSends를 넘으면 CapacityExceeded다.
    Result<void> SetBusSend(const MixBusRef& bus, float level) noexcept;

    // emitter에 붙여 재생한다. 재생 중이면 먼저 멈춘다(UE Play와 같다). 오류: 사운드 없음·범위 밖
    // 배수(InvalidArgument), 그 밖은 AudioWorldSubsystem::PlayAttached와 같다. 등록 전이거나
    // World에 AudioWorldSubsystem이 없으면 NotReady다.
    Result<void> Play(uint32 startFrame = 0) noexcept;

    // 재생을 멈춘다(없으면 할 일 없음).
    void Stop(uint32 fadeFrames = audio::DefaultStopFadeFrames) noexcept;

    // 재생 결과가 아직 끝나지 않았다(Pending·Playing).
    bool IsPlaying() const noexcept;

    // 배수를 바꾸고 재생 중이면 바로 반영한다. 범위 밖·비유한 값은 InvalidArgument다.
    Result<void> SetVolumeMultiplier(float value) noexcept;
    // 재생 LPF·HPF를 켜고 끄거나 주파수를 바꾼다(UE SetLowPassFilterEnabled·Frequency,
    // SetHighPassFilterEnabled·Frequency — 3.43.1절). 재생 중이면 바로 반영한다. 오류: [0, 20000]
    // 밖은 InvalidArgument.
    Result<void> SetLowPassFilter(bool enabled, float frequency) noexcept;
    Result<void> SetHighPassFilter(bool enabled, float frequency) noexcept;
    Result<void> SetPitchMultiplier(float value) noexcept;

    PlaybackHandle Playback() const noexcept {
        return playback_;
    }

    AudioEmitterId Emitter() const noexcept {
        return emitter_;
    }

    void OnStart() noexcept override;
    void OnDisable() noexcept override;

protected:
    bool WantsSystemState(SystemStateKind kind) const noexcept override;
    Result<void> CreateSystemState(SystemStateKind kind) noexcept override;
    void DestroySystemState(SystemStateKind kind) noexcept override;
    void OnTransformUpdated() noexcept override;

private:
    AudioWorldSubsystem* Subsystem() const noexcept;

    ROCKS_NATIVE() SoundSourceRef sound_;
    ROCKS_NATIVE() PlaybackHandle playback_;
    ROCKS_NATIVE() AudioEmitterId emitter_;
    ROCKS_NATIVE() uint32 concurrencyCount_ = 0;
    ROCKS_NATIVE() SoundConcurrencyEntry concurrency_[MaxConcurrencyGroups];
    ROCKS_NATIVE() SoundCategoryChainRef category_;
    // 다음 Play에 쓸 버스 send다(UE PendingSubmixSends).
    ROCKS_NATIVE() uint32 busSendCount_ = 0;
    ROCKS_NATIVE() PlayBusSend busSends_[audio::MaxVoiceSends];
};

} // namespace rocks
