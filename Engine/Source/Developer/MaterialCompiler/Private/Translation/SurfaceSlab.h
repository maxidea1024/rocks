#pragma once

#include <Base/Types.h>
#include <MaterialCompiler/MaterialIrValue.h>

// 접기에 쓰는 slab 값 묶음이다(Docs/Design/MaterialSystem 6.11절 MS2d2 — UE FSubstrateBSDF의 slab
// 칸). 값은 모두 IR 값 번호다. 표지는 Bool 스칼라, SSS 종류는 UInt 스칼라(SlabSubsurface 번호),
// 나머지는 Float(색·노멀은 float3)이다.
namespace rocks::material::detail {

enum class SlabValue : uint8 {
    DiffuseAlbedo = 0,
    F0 = 1,
    F90 = 2,
    Roughness = 3,
    Anisotropy = 4,
    Normal = 5,
    Tangent = 6,
    // SSS 평균 자유 경로(cm — UE SLAB_SSSMFP, Wrap이면 계산 공간)다.
    MeanFreePath = 7,
    // 부피 계산용 평균 자유 경로(cm — UE TmpMFP)다.
    VolumeMeanFreePath = 8,
    PhaseAnisotropy = 9,
    // SSS profile 반경 배율이다(Profile 종류만).
    RadiusScale = 10,
    Emissive = 11,
    HazeRoughness = 12,
    HazeWeight = 13,
    FuzzRoughness = 14,
    FuzzAmount = 15,
    FuzzColor = 16,
    // 층 두께(cm)다.
    Thickness = 17,
    Coverage = 18,
    HasF90 = 19,
    HasHaze = 20,
    SimpleClearCoat = 21,
    HasFuzz = 22,
    HasAnisotropy = 23,
    IsThin = 24,
    Subsurface = 25
};

inline constexpr usize SlabValueCount = 26;

// SSS 종류 번호다(UE SSS_TYPE_* — 병합은 max·min이므로 번호 순서가 의미다).
enum class SlabSubsurface : uint32 {
    None = 0,
    Wrap = 1,
    TwoSidedWrap = 2,
    Diffusion = 3,
    Profile = 4,
    SimpleVolume = 5
};

struct SurfaceSlab {
    IrValueId values[SlabValueCount];

    SurfaceSlab() noexcept {
        for (IrValueId& value : values) {
            value = NoIrValue;
        }
    }

    IrValueId& operator[](SlabValue value) noexcept {
        return values[static_cast<usize>(value)];
    }

    IrValueId operator[](SlabValue value) const noexcept {
        return values[static_cast<usize>(value)];
    }
};

} // namespace rocks::material::detail
