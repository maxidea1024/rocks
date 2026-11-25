#pragma once

#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Environment/SkyAtmosphereSettings.h>
#include <World/SceneComponent.h>

namespace rocks {

// 하늘·공기 원근을 만드는 대기다(UE USkyAtmosphereComponent — Docs/Design/EngineRuntime 27.2절).
// 데이터만 있다: 렌더(RS9) 연결 전에는 그리지 않는다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API SkyAtmosphereComponent : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // km ↔ cm다(UE SkyAtmosphereCommonData.cpp:15 — CmToSkyUnit 1e-5).
    static constexpr double KilometersPerCentimeter = 1.0e-5;

    explicit SkyAtmosphereComponent(Allocator& allocator) noexcept : SceneComponent(allocator) {
    }

    const SkyAtmosphereSettings& Settings() const noexcept {
        return settings_;
    }

    // 오류: 범위 밖(IsValidSkyAtmosphereSettings 거짓 — InvalidArgument), 게임 World 초기화 뒤
    // Static (UE AreDynamicDataChangesAllowed — StaticComponentDataChanged). 실패하면 바뀌지
    // 않는다.
    Result<void> SetSettings(const SkyAtmosphereSettings& settings) noexcept;

    // 행성 중심(km)이다(UE SkyAtmosphereCommonData.cpp:166 — 27.2절).
    Vec3d PlanetCenterKm() const noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "settings") SkyAtmosphereSettings settings_;
};

} // namespace rocks
