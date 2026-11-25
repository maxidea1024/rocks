#pragma once

#include <Meta/Annotations.h>
#include <World/Environment/HeightFogSettings.h>
#include <World/SceneComponent.h>

namespace rocks {

// 높이에 따라 짙어지는 안개다(UE UExponentialHeightFogComponent — 이름은 ComponentCatalog CD6,
// Docs/Design/EngineRuntime 27.3절). 안개 높이는 컴포넌트 월드 Z다. 데이터만 있다: 렌더 연결 전에는
// 그리지 않는다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API HeightFogComponent : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // 안개가 보이는 최소 밀도 합·불투명도(UE UE_DELTA 1e-5 — UnrealMathUtility.h:186)와 밀도의 표시
    // 배율(1000 — UE "artist scale")이다. UE처럼 float로 비교한다.
    static constexpr float VisibilityEpsilon = 1.0e-5f;
    static constexpr float DensityArtistScale = 1000.0f;

    explicit HeightFogComponent(Allocator& allocator) noexcept : SceneComponent(allocator) {
    }

    const HeightFogSettings& Settings() const noexcept {
        return settings_;
    }

    // 오류: 범위 밖(InvalidArgument — 바뀌지 않는다). UE 안개 setter처럼 Mobility는 보지 않는다.
    Result<void> SetSettings(const HeightFogSettings& settings) noexcept;

    // (밀도 + 둘째 밀도) × 1000 > 1e-5이고 최대 불투명도 > 1e-5다(UE AddFogIfNeeded — :126).
    bool IsFogVisible() const noexcept;

    // 첫 겹·둘째 겹 높이(cm, 월드 Z — UE :135, SceneCore.cpp:398)다.
    double FogHeight() const noexcept;
    double SecondFogHeight() const noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "settings") HeightFogSettings settings_;
};

} // namespace rocks
