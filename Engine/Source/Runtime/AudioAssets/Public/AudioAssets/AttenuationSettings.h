#pragma once

#include <AssetRuntime/Package.h>
#include <AudioAssets/AttenuationTypes.h>
#include <AudioAssets/MixBusSend.h>
#include <AudioAssets/SoundSourceRef.h>
#include <CurveAssets/FloatCurveSource.h>
#include <Object/StructRefArray.h>

// 감쇠 설정 에셋이다(Docs/Design/AudioSystem 3.9·3.35.3·3.42.1절, UE USoundAttenuation 대응). 거리
// 감쇠(모양 넷·Custom 곡선), 공기 흡수, 우선순위 거리 감쇠, 리스너 focus, 스테레오 spread, 비공간화
// 반경, 차폐 목표값을 가진다(리버브·서브믹스 send·플러그인은 뒤 단계 — 3.42.9절). 기본값은 UE
// FBaseAttenuationSettings·FSoundAttenuationSettings와 같다(Attenuation.cpp:17–27,
// SoundAttenuation.cpp:16–90).
//
// 스레드: 에셋 객체 Registry의 소유 스레드(게임 스레드) 전용이다. 재생 경로는 이 객체가 아니라
// ToSnapshot이 만든 값(곡선은 깊은 사본)을 쓴다.
namespace rocks {

// 거리 곡선 모양이다(UE EAttenuationDistanceModel). 값은 audio::DistanceAttenuationModel과 같다.
// Custom은 customDistanceCurve를 쓴다.
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
    explicit AttenuationSettings(Allocator& allocator) noexcept
        : customDistanceCurve(allocator), customLowPassAbsorptionCurve(allocator),
          customHighPassAbsorptionCurve(allocator), customPriorityAttenuationCurve(allocator),
          busSends(allocator) {
    }

    // 거리 감쇠를 한다(UE bAttenuate).
    ROCKS_PROPERTY() bool attenuate = true;
    // 리스너 방향으로 패닝한다(UE bSpatialize). false면 감쇠만 하고 2D 출력이다.
    ROCKS_PROPERTY() bool spatialize = true;
    ROCKS_PROPERTY() SoundDistanceModel model = SoundDistanceModel::Linear;
    // 감쇠 모양과 크기다(UE AttenuationShape·AttenuationShapeExtents — 성분의 뜻은
    // SoundAttenuationShape, 3.42.2절).
    ROCKS_PROPERTY() SoundAttenuationShape shape = SoundAttenuationShape::Sphere;
    ROCKS_PROPERTY() Vec3 shapeExtents = Vec3(DefaultAttenuationShapeRadius, 0.0f, 0.0f);
    // Cone 원점을 전방 반대로 미는 거리와 Cone 구 부분의 반경·falloff(cm)다.
    ROCKS_PROPERTY() float coneOffset = 0.0f;
    ROCKS_PROPERTY() float coneSphereRadius = 0.0f;
    ROCKS_PROPERTY() float coneSphereFalloffDistance = 0.0f;
    // 모양 밖에서 감쇠가 끝나는 거리(cm)다(UE FalloffDistance).
    ROCKS_PROPERTY() float falloffDistance = 3600.0f;
    // NaturalSound의 falloff 지점 감쇠(dB, 0 이하)와 그 밖의 동작이다.
    ROCKS_PROPERTY() float naturalSoundAttenuationDb = -60.0f;
    ROCKS_PROPERTY()
    SoundNaturalFalloffMode naturalSoundFalloffMode = SoundNaturalFalloffMode::Continues;
    // model이 Custom일 때의 거리 곡선이다(X = falloff로 정규화한 거리, 값 [0, 1]로 자름).
    ROCKS_PROPERTY() FloatCurveSource customDistanceCurve;

    // 공기 흡수(UE bAttenuateWithLPF 외 — 3.42.3절): 거리 구간 [absorptionDistanceMin, Max]에서
    // LPF· HPF 주파수를 AtMin → AtMax로 옮긴다.
    ROCKS_PROPERTY() bool attenuateWithLowPass = false;
    ROCKS_PROPERTY() SoundAbsorptionMethod absorptionMethod = SoundAbsorptionMethod::Linear;
    ROCKS_PROPERTY() bool logFrequencyScaling = false;
    ROCKS_PROPERTY() float absorptionDistanceMin = DefaultAbsorptionDistanceMin;
    ROCKS_PROPERTY() float absorptionDistanceMax = DefaultAbsorptionDistanceMax;
    ROCKS_PROPERTY() float lowPassFrequencyAtMin = MaxAttenuationFilterFrequency;
    ROCKS_PROPERTY() float lowPassFrequencyAtMax = MaxAttenuationFilterFrequency;
    ROCKS_PROPERTY() float highPassFrequencyAtMin = 0.0f;
    ROCKS_PROPERTY() float highPassFrequencyAtMax = 0.0f;
    ROCKS_PROPERTY() FloatCurveSource customLowPassAbsorptionCurve;
    ROCKS_PROPERTY() FloatCurveSource customHighPassAbsorptionCurve;

    // 우선순위 거리 감쇠(UE bEnablePriorityAttenuation 외 — 3.42.4절)다.
    ROCKS_PROPERTY() bool enablePriorityAttenuation = false;
    ROCKS_PROPERTY()
    SoundPriorityAttenuationMethod priorityAttenuationMethod =
        SoundPriorityAttenuationMethod::Linear;
    ROCKS_PROPERTY() float priorityAttenuationMin = 1.0f;
    ROCKS_PROPERTY() float priorityAttenuationMax = 1.0f;
    ROCKS_PROPERTY()
    float priorityAttenuationDistanceMin = DefaultPriorityAttenuationDistanceMin;
    ROCKS_PROPERTY()
    float priorityAttenuationDistanceMax = DefaultPriorityAttenuationDistanceMax;
    ROCKS_PROPERTY() float manualPriorityAttenuation = 1.0f;
    ROCKS_PROPERTY() FloatCurveSource customPriorityAttenuationCurve;

    // 리스너 focus(UE bEnableListenerFocus 외 — 3.42.5절)다. 방위각은 도다.
    ROCKS_PROPERTY() bool enableListenerFocus = false;
    ROCKS_PROPERTY() bool enableFocusInterpolation = false;
    ROCKS_PROPERTY() float focusAzimuth = DefaultFocusAzimuth;
    ROCKS_PROPERTY() float nonFocusAzimuth = DefaultNonFocusAzimuth;
    ROCKS_PROPERTY() float focusDistanceScale = 1.0f;
    ROCKS_PROPERTY() float nonFocusDistanceScale = 1.0f;
    ROCKS_PROPERTY() float focusPriorityScale = 1.0f;
    ROCKS_PROPERTY() float nonFocusPriorityScale = 1.0f;
    ROCKS_PROPERTY() float focusVolumeAttenuation = 1.0f;
    ROCKS_PROPERTY() float nonFocusVolumeAttenuation = 1.0f;
    ROCKS_PROPERTY() float focusAttackInterpSpeed = 1.0f;
    ROCKS_PROPERTY() float focusReleaseInterpSpeed = 1.0f;

    // 스테레오 원본의 좌우 간격(cm — UE StereoSpread)이다.
    ROCKS_PROPERTY() float stereoSpread = DefaultStereoSpread;
    // UE NonSpatializedRadiusStart·End(cm)·Mode다(3.35.7·3.42.6절).
    ROCKS_PROPERTY() float nonSpatializedRadiusStart = 0.0f;
    ROCKS_PROPERTY() float nonSpatializedRadiusEnd = 0.0f;
    ROCKS_PROPERTY()
    SoundNonSpatializedMode nonSpatializedRadiusMode = SoundNonSpatializedMode::OmniDirectional;

    // 차폐 목표값(UE bEnableOcclusion 외 — 3.42.7절)이다. 질의는 World의 차폐 공급자가 한다.
    ROCKS_PROPERTY() bool enableOcclusion = false;
    ROCKS_PROPERTY() float occlusionLowPassFrequency = MaxAttenuationFilterFrequency;
    ROCKS_PROPERTY() float occlusionVolumeAttenuation = 1.0f;
    ROCKS_PROPERTY() float occlusionInterpolationSeconds = DefaultOcclusionInterpolationSeconds;
    // 버스 send(UE bEnableSubmixSends·SubmixSendSettings — 3.45.7절, 최대 MaxMixBusSends)다. 단계는
    // 보지 않고 늘 post다(Pre면 스냅숏이 Warning을 내고 post로 쓴다).
    ROCKS_PROPERTY() bool enableBusSends = false;
    ROCKS_PROPERTY() StructRefArray<MixBusSend> busSends;

    // 값 스냅숏이다. 쓰이는 곡선(Custom 모델·CustomCurve 방법)은 FloatCurveSource::TryFlattenInto로
    // allocator에 깊이 복사한다(allocator는 스레드 안전해야 한다 — 제어 pipe가 마지막 참조를
    // 놓는다). 오류: 범위 밖·비유한 값 InvalidArgument(ValidateSoundAttenuation), 곡선 참조가
    // 풀리지 않으면 그 오류, 할당 실패 OutOfMemory.
    Result<SoundAttenuation> ToSnapshot(Allocator& allocator, ObjectRegistry& registry,
                                        const ObjectAccessContext& access) const noexcept;
};

} // namespace rocks
