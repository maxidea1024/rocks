#pragma once

#include <AssetRuntime/Package.h>
#include <AudioAssets/SoundSourceRef.h>

// 감쇠 설정 에셋이다(Docs/Design/AudioSystem 3.9·3.35.3절, UE USoundAttenuation 대응). AU3은 구
// 모양 거리 감쇠·패닝·비공간화 반경만 가진다(모양·공기 흡수·거리별 send는 AU7). 기본값은 UE
// FBaseAttenuationSettings·FSoundAttenuationSettings와 같다(Attenuation.cpp:18–24,
// SoundAttenuation.cpp:41–42).
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다. 재생 경로는 이 객체가 아니라
// ToSnapshot이 만든 값을 쓴다.
namespace rocks {

// 거리 곡선 모양이다(UE EAttenuationDistanceModel). 값은 audio::DistanceAttenuationModel과 같다
// (Custom 커브는 AU7 — 지금은 잘못된 값이다).
ROCKS_ENUM()
enum class SoundDistanceModel : uint8 {
    Linear = 0,
    Logarithmic = 1,
    Inverse = 2,
    LogReverse = 3,
    NaturalSound = 4,
    Custom = 5
};

// NaturalSound가 감쇠 거리 밖에서 하는 일이다(UE ENaturalSoundFalloffMode).
ROCKS_ENUM()
enum class SoundNaturalFalloffMode : uint8 {
    Hold = 0,
    Silent = 1,
    Continues = 2
};

ROCKS_CLASS()
class ROCKS_AUDIOASSETS_API AttenuationSettings final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit AttenuationSettings(Allocator& allocator) noexcept {
        (void)allocator;
    }

    // 거리 감쇠를 한다(UE bAttenuate).
    ROCKS_PROPERTY() bool attenuate = true;
    // 리스너 방향으로 패닝한다(UE bSpatialize). false면 감쇠만 하고 2D 출력이다.
    ROCKS_PROPERTY() bool spatialize = true;
    ROCKS_PROPERTY() SoundDistanceModel model = SoundDistanceModel::Linear;
    // 이 반경(cm) 안은 감쇠 1이다(UE AttenuationShapeExtents.X — 구).
    ROCKS_PROPERTY() float innerRadius = 400.0f;
    // innerRadius 밖에서 감쇠가 끝나는 거리(cm)다(UE FalloffDistance).
    ROCKS_PROPERTY() float falloffDistance = 3600.0f;
    // NaturalSound의 falloff 지점 감쇠(dB, 0 이하)와 그 밖의 동작이다.
    ROCKS_PROPERTY() float naturalSoundAttenuationDb = -60.0f;
    ROCKS_PROPERTY()
    SoundNaturalFalloffMode naturalSoundFalloffMode = SoundNaturalFalloffMode::Continues;
    // UE NonSpatializedRadiusStart·End(cm)다(3.35.7절 식).
    ROCKS_PROPERTY() float nonSpatializedRadiusStart = 0.0f;
    ROCKS_PROPERTY() float nonSpatializedRadiusEnd = 0.0f;

    // 값 스냅숏이다. 범위 밖·비유한 값이나 Custom 모델은 InvalidArgument다.
    Result<SoundAttenuation> ToSnapshot() const noexcept;
};

} // namespace rocks
