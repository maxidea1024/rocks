#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <MaterialCompiler/MaterialIrValue.h>

// 표면 노드 하나의 기록이다(Docs/Design/MaterialSystem 6.11절 MS2d1). 표면 핀 값(IR SurfaceRef)이
// 가리키고, 출력 노드가 기록 트리를 closure 하나로 접어 SurfaceAttributes 칸으로 내보낸다. 칸 값은
// IR 값 번호이고 NoIrValue면 그 칸의 기본값이다.
namespace rocks::material::detail {

// 표면 종류다(UE Substrate BSDF·영역 표면·고전 preset).
enum class SurfaceKind : uint8 {
    // SurfaceFromShadingModel(UE SubstrateShadingModels) — 고전 칸이 그대로 지나간다.
    Classic = 0,
    // Unlit BSDF와 영역 표면(LightFunction·PostProcess·UserInterface — UE Unlit BSDF 하위 쓰임).
    Unlit = 1,
    // VolumetricFogCloud(Volume 영역 출력).
    VolumetricFogCloud = 2,
    // ToDecal(자식 표면 + coverage).
    Decal = 3
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
    // 원인 노드다(진단 위치).
    Guid node;
    IrValueId fields[SurfaceFieldCount] = {NoIrValue, NoIrValue, NoIrValue, NoIrValue, NoIrValue,
                                           NoIrValue, NoIrValue, NoIrValue, NoIrValue, NoIrValue,
                                           NoIrValue, NoIrValue, NoIrValue, NoIrValue};
    // Decal의 자식 표면 값(SurfaceRef)과 coverage다.
    IrValueId child = NoIrValue;
    IrValueId coverage = NoIrValue;

    IrValueId& Field(SurfaceField field) noexcept {
        return fields[static_cast<usize>(field)];
    }

    IrValueId Field(SurfaceField field) const noexcept {
        return fields[static_cast<usize>(field)];
    }
};

} // namespace rocks::material::detail
