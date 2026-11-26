#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <MaterialCompiler/MaterialIrValue.h>

#include "SurfaceSlab.h"

// 표면 노드 하나의 기록이다(Docs/Design/MaterialSystem 6.11절 MS2d1·MS2d2). 표면 핀 값(IR
// SurfaceRef)이 가리키고, 출력 노드가 기록 트리를 closure 하나로 접어 SurfaceAttributes 칸으로
// 내보낸다. 칸 값은 IR 값 번호이고 NoIrValue면 그 칸의 기본값이다.
namespace rocks::material::detail {

// 표면 종류다(UE Substrate BSDF·연산·영역 표면·고전 preset).
enum class SurfaceKind : uint8 {
    // SurfaceFromShadingModel(UE SubstrateShadingModels) — 고전 칸이 그대로 지나간다.
    Classic = 0,
    // Unlit BSDF와 영역 표면(LightFunction·PostProcess·UserInterface — UE Unlit BSDF 하위 쓰임).
    Unlit = 1,
    // VolumetricFogCloud(Volume 영역 출력).
    VolumetricFogCloud = 2,
    // ToDecal(자식 표면 + coverage).
    Decal = 3,
    // Slab·SimpleClearCoat BSDF(slab 입력 — 접을 때 SurfaceSlab이 된다).
    Slab = 4,
    // 전용 BSDF(고전 칸으로 바로 낸다).
    Hair = 5,
    Eye = 6,
    SingleLayerWater = 7,
    // 표면 연산(SurfaceOperator).
    Operator = 8
};

// 표면 연산이다(UE Substrate operator).
enum class SurfaceOperator : uint8 {
    HorizontalMix = 0,
    VerticalLayer = 1,
    Add = 2,
    Weight = 3,
    Select = 4
};

// 표면을 쓰는 영역 쓰임이다(UE Substrate SubUsage — 영역 검사 V6).
enum class SurfaceUsage : uint8 {
    Surface = 0,
    LightFunction = 1,
    PostProcess = 2,
    UserInterface = 3,
    Decal = 4,
    Volume = 5
};

// 하위 트리에 흐르는 BSDF 종류 비트다(UE MTV_* — 허용 표 검사, 6.11절 MS2d2).
enum class SurfaceFlow : uint16 {
    Unlit = 1u << 0,
    Slab = 1u << 1,
    Hair = 1u << 2,
    Eye = 1u << 3,
    SingleLayerWater = 1u << 4,
    Toon = 1u << 5,
    VolumetricFogCloud = 1u << 6,
    UserInterface = 1u << 7,
    Decal = 1u << 8,
    PostProcess = 1u << 9,
    LightFunction = 1u << 10
};

inline constexpr uint16 SurfaceFlowBit(SurfaceFlow flow) noexcept {
    return static_cast<uint16>(flow);
}

// 내보내는 칸이다(SurfaceAttributes — UE 고전 픽셀 속성, 6.11절).
enum class SurfaceField : uint8 {
    BaseColor = 0,
    Metallic = 1,
    Specular = 2,
    Roughness = 3,
    Anisotropy = 4,
    Emissive = 5,
    Normal = 6,
    Tangent = 7,
    SubsurfaceColor = 8,
    CustomData0 = 9,
    CustomData1 = 10,
    Opacity = 11,
    ShadingModel = 12,
    AmbientOcclusion = 13
};

inline constexpr usize SurfaceFieldCount = 14;

struct SurfaceRecord {
    SurfaceKind kind = SurfaceKind::Classic;
    SurfaceUsage usage = SurfaceUsage::Surface;
    SurfaceOperator op = SurfaceOperator::HorizontalMix;
    // 하위 트리에 흐르는 종류(SurfaceFlow 비트)다.
    uint16 flow = 0;
    // 원인 노드다(진단 위치).
    Guid node;
    IrValueId fields[SurfaceFieldCount] = {NoIrValue, NoIrValue, NoIrValue, NoIrValue, NoIrValue,
                                           NoIrValue, NoIrValue, NoIrValue, NoIrValue, NoIrValue,
                                           NoIrValue, NoIrValue, NoIrValue, NoIrValue};
    // Decal·연산의 자식 표면 값(SurfaceRef)이다. Decal·Weight는 [0]만 쓴다. HorizontalMix는
    // (Background, Foreground), VerticalLayer는 (Top, Base), Add·Select는 (A, B)다.
    IrValueId children[2] = {NoIrValue, NoIrValue};
    // 연산 매개변수다: Decal Coverage, HorizontalMix Mix, VerticalLayer Thickness, Weight Weight,
    // Select SelectValue.
    IrValueId operand = NoIrValue;
    // Select의 Threshold 속성이다.
    float threshold = 0.0f;
    // 내보낸 표면의 coverage다(반투명 Opacity 진입점 — 6.11절 MS2d2, 없으면 opacity 칸).
    IrValueId coverage = NoIrValue;
    // Slab 기록의 입력이다(UE 단순화 적용 — MeanFreePath는 입력 MFP, RadiusScale 자리는 MFP
    // Scale, Coverage·두께·표지는 접을 때 정한다). SimpleClearCoat 표지만 기록 때 정한다.
    SurfaceSlab slab;
    // Slab의 subSurfaceType 속성(SlabSubsurface 번호, MFP가 연결된 때만 — 아니면 None)이다.
    uint32 subsurfaceType = 0;
    // 두 번째 노멀(SimpleClearCoat BottomNormal·Eye IrisNormal·고전 ClearCoatNormal — MS2d3
    // ClearCoatBottomNormal 출력, 연결된 때만)이다.
    IrValueId secondNormal = NoIrValue;
    // 고전 preset의 얇은 투과 입력(TransmittanceColor·ThinTranslucentSurfaceCoverage — slab 변환과
    // MS2d3 ThinTranslucentMaterialOutput 출력)이다.
    IrValueId thinTransmittance = NoIrValue;
    IrValueId thinCoverage = NoIrValue;

    IrValueId& Field(SurfaceField field) noexcept {
        return fields[static_cast<usize>(field)];
    }

    IrValueId Field(SurfaceField field) const noexcept {
        return fields[static_cast<usize>(field)];
    }
};

} // namespace rocks::material::detail
