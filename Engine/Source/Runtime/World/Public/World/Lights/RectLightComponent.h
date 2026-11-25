#pragma once

#include <Meta/Annotations.h>
#include <World/Lights/LightUnits.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/Lights/LocalLightComponent.h>

namespace rocks {

// 사각 면 광원이다(UE URectLightComponent — Docs/Design/EngineRuntime 24.2.2절). 면은 Component의
// +X 쪽을 비춘다. barn door(차광판)는 면 가장자리의 판이다.
// 데이터만 있다: 렌더 장면 등록(RS1·RS3) 전에는 아무것도 비추지 않는다(24.1절).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API RectLightComponent : public LocalLightComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 면 크기(cm, UE 64)와 허용 범위(UE PostEditChange 1 이상·ClampMax 100000)다.
    static constexpr float DefaultSourceSize = 64.0f;
    static constexpr float MinSourceSize = 1.0f;
    static constexpr float MaxSourceSize = 100000.0f;
    // barn door 최대 각(도, UE GetRectLightBarnDoorMaxAngle 88 — 기본값이기도 하다)과 기본 길이(cm,
    // 20)다.
    static constexpr float MaxBarnDoorAngle = 88.0f;
    static constexpr float DefaultBarnDoorLength = 20.0f;
    // barn door 길이 하한(cm, UE SetBarnDoorLength의 Max(0.1) — RectLightComponent.cpp:160)이다.
    static constexpr float MinBarnDoorLength = 0.1f;

    explicit RectLightComponent(Allocator& allocator) noexcept : LocalLightComponent(allocator) {
    }

    // 단위별 배율이다(UE RectLightComponent.cpp:181 — Nits는 면적 W·H, Lumens는 1e4/π).
    float ComputeLightBrightness() const noexcept override;

    float SourceWidth() const noexcept {
        return sourceWidth_;
    }

    // 오류: [MinSourceSize, MaxSourceSize] 밖·비유한, Mobility. 높이도 같다.
    Result<void> SetSourceWidth(float width) noexcept;

    float SourceHeight() const noexcept {
        return sourceHeight_;
    }

    Result<void> SetSourceHeight(float height) noexcept;

    float BarnDoorAngle() const noexcept {
        return barnDoorAngle_;
    }

    // 오류: [0, MaxBarnDoorAngle] 밖·비유한, Mobility(UE는 clamp — Rocks setter는 거부한다).
    Result<void> SetBarnDoorAngle(float degrees) noexcept;

    float BarnDoorLength() const noexcept {
        return barnDoorLength_;
    }

    // 오류: MinBarnDoorLength 미만·비유한, Mobility(UE는 clamp — Rocks setter는 거부한다).
    Result<void> SetBarnDoorLength(float length) noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "sourceWidth") float sourceWidth_ = DefaultSourceSize;
    ROCKS_PROPERTY(ReadOnly, Name = "sourceHeight") float sourceHeight_ = DefaultSourceSize;
    ROCKS_PROPERTY(ReadOnly, Name = "barnDoorAngle") float barnDoorAngle_ = MaxBarnDoorAngle;
    ROCKS_PROPERTY(ReadOnly, Name = "barnDoorLength") float barnDoorLength_ = DefaultBarnDoorLength;
};

} // namespace rocks
