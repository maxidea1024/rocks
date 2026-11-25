#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Shapes.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Lights/LightingChannels.h>
#include <World/SceneComponent.h>
#include <World/Lights/LightUnits.h>

namespace rocks {

// 모든 광원 컴포넌트의 기반이다(UE ULightComponentBase — Docs/Design/EngineRuntime 24.2절).
// 데이터만 가진다: 렌더 장면 등록(CR12·RS1·RS3)이 생기기 전에는 아무것도 비추지 않는다. 값이 바뀌면
// Render 시스템 상태가 있을 때 다시 보내기를 표시한다(지금은 상태가 없어 아무 일도 없다).
//
// 규약(24.2.3절):
// - setter는 비유한·범위 밖 값을 InvalidArgument로 거부하고 값을 바꾸지 않는다.
// - 게임 World에서 초기화 뒤에는 Static 광원의 데이터를 바꿀 수 없다(InvalidArgument + Warning
//   StaticComponentDataChanged — UE AreDynamicDataChangesAllowed). Stationary는 세기·색 등을 바꿀
//   수 있다. Spawn·OnAwake 안과 편집 World에서는 막지 않는다.
// - 세기 단위는 파생이 정한다(IntensityUnits). 세기는 단위가 EV일 때만 음수를 받는다.
//
// 사용 예(Actor OnAwake):
//   auto light = GetWorld().AddComponent<PointLightComponent>(Handle());
//   GetWorld().With<PointLightComponent>(light.Value(), [](PointLightComponent& value) noexcept {
//       (void)value.SetIntensityUnits(LightUnits::Candelas);
//       return value.SetIntensity(8.0f);    // 8 cd
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API LightComponentBase : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 세기다(UE ULightComponentBase 생성자 π — LightComponent.cpp:436).
    static constexpr float DefaultIntensity = math::Pi<float>;

    explicit LightComponentBase(Allocator& allocator) noexcept : SceneComponent(allocator) {
    }

    float Intensity() const noexcept {
        return intensity_;
    }

    // 오류: 비유한, 단위가 EV가 아닌데 음수(InvalidArgument), Mobility(24.2.3절).
    Result<void> SetIntensity(float intensity) noexcept;

    // 세기의 단위다(기본 Unitless — 국소 광원이 재정의한다).
    virtual LightUnits IntensityUnits() const noexcept {
        return LightUnits::Unitless;
    }

    // 필터 색(sRGB 인코딩 byte, UE FColor LightColor)이다.
    Color LightColor() const noexcept {
        return lightColor_;
    }

    // sRGB 해독한 선형 색이다(UE GetLightColor — alpha는 byte/255).
    LinearColor LinearLightColor() const noexcept;

    // sRGB byte를 그대로 둔다. 오류: Mobility.
    Result<void> SetLightColor(Color color) noexcept;

    // srgb면 선형 색을 sRGB로 인코딩해 둔다(ToSRGB). 거짓이면 인코딩 없이 byte로 양자화한다
    // (QuantizeRound — UE SetLightColor(FLinearColor, bSRGB), LightComponent.cpp:1130). 오류:
    // 비유한 성분(InvalidArgument), Mobility.
    Result<void> SetLightColor(const LinearColor& color, bool srgb = true) noexcept;

    bool AffectsWorld() const noexcept {
        return affectsWorld_;
    }

    // 거짓이면 광원 전체가 꺼진다. 오류: 게임 World 초기화 뒤(ComponentDataLockedInGame — UE도 실행
    // 중 바꿀 수 없다).
    Result<void> SetAffectsWorld(bool affects) noexcept;

    bool CastsShadows() const noexcept {
        return castShadows_;
    }

    Result<void> SetCastShadows(bool cast) noexcept;

    bool CastsStaticShadows() const noexcept {
        return castStaticShadows_;
    }

    Result<void> SetCastStaticShadows(bool cast) noexcept;

    bool CastsDynamicShadows() const noexcept {
        return castDynamicShadows_;
    }

    Result<void> SetCastDynamicShadows(bool cast) noexcept;

    bool AffectsTranslucentLighting() const noexcept {
        return affectTranslucentLighting_;
    }

    Result<void> SetAffectTranslucentLighting(bool affects) noexcept;

    bool HasTransmission() const noexcept {
        return transmission_;
    }

    Result<void> SetTransmission(bool transmission) noexcept;

    bool CastsVolumetricShadow() const noexcept {
        return castVolumetricShadow_;
    }

    Result<void> SetCastVolumetricShadow(bool cast) noexcept;

    float IndirectLightingIntensity() const noexcept {
        return indirectLightingIntensity_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetIndirectLightingIntensity(float intensity) noexcept;

    float VolumetricScatteringIntensity() const noexcept {
        return volumetricScatteringIntensity_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetVolumetricScatteringIntensity(float intensity) noexcept;

    // Static이고 소유 Actor가 있다(UE HasStaticLighting — 굽는 조명 대상).
    bool HasStaticLighting() const noexcept;

    // Movable이 아니고 소유 Actor가 있다(UE HasStaticShadowing).
    bool HasStaticShadowing() const noexcept;

protected:
    // Mobility 검사 → 대입 → 바뀌었으면 Render 다시 보내기 표시다. 값 검증은 호출 전에 한다.
    template <typename T>
    Result<void> ChangeLightData(T& field, const T& value, const char* property,
                                 bool blockStationary = false) noexcept {
        if (auto allowed = CheckDynamicDataChange(property, blockStationary); !allowed) {
            return allowed;
        }

        if (!(field == value)) {
            field = value;
            MarkRenderStateDirtyIfPresent();
        }

        return Result<void>::Success();
    }

    // 파생 생성자가 기본값을 바꾼다(UE 파생 생성자처럼).
    ROCKS_PROPERTY(ReadOnly, Name = "intensity") float intensity_ = DefaultIntensity;
    ROCKS_PROPERTY(ReadOnly, Name = "lightColor") Color lightColor_ = Color::White();
    ROCKS_PROPERTY(ReadOnly, Name = "affectsWorld") bool affectsWorld_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castShadows") bool castShadows_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castStaticShadows") bool castStaticShadows_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castDynamicShadows") bool castDynamicShadows_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "affectTranslucentLighting")
    bool affectTranslucentLighting_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "transmission") bool transmission_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castVolumetricShadow") bool castVolumetricShadow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "indirectLightingIntensity")
    float indirectLightingIntensity_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "volumetricScatteringIntensity")
    float volumetricScatteringIntensity_ = 1.0f;
};

} // namespace rocks
