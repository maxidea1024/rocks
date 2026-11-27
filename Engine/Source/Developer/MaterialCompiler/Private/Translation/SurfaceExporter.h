#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Result.h>

#include <RenderData/MaterialEnums.h>

#include "SurfaceMath.h"
#include "SurfaceRecord.h"

// 표면 기록 트리를 closure 하나로 접어 고전 칸(SurfaceAttributes)으로 내보낸다(Docs/Design/
// MaterialSystem 6.11절 MS2d2). 출력 노드의 LoweringContext 안에서 쓴다(만든 IR 값의 위치는 출력
// 노드다).
//
// 사용 예:
//   SurfaceExporter exporter(translator, context);
//   SurfaceRecord exported;
//   auto done = exporter.Export(surface, exported); // false면 진단을 냈다
namespace rocks::material::detail {

class LoweringContext;
class Translator;

class SurfaceExporter {
public:
    SurfaceExporter(Translator& translator, LoweringContext& context) noexcept;

    SurfaceExporter(const SurfaceExporter&) = delete;
    SurfaceExporter& operator=(const SurfaceExporter&) = delete;

    // surface(SurfaceRef)를 내보낸다. out은 고전 칸과 coverage다(쓰지 않는 칸은 NoIrValue). 그래프
    // 오류를 진단했으면 false다.
    Result<bool> Export(IrValueId surface, SurfaceRecord& out) noexcept;

private:
    // 접은 slab 하나다(같은 기록·두께·층 위치면 다시 접지 않는다 — DAG가 지수로 늘지 않게).
    struct FoldedSlab {
        IrValueId surface = NoIrValue;
        IrValueId thickness = NoIrValue;
        bool bottom = true;
        SurfaceSlab slab;
    };

    // underOperator면 고전 preset을 slab으로 바꿔 내보낸다(연산 아래 — 6.11절 MS2d2).
    Result<bool> ExportSurface(IrValueId surface, bool underOperator, SurfaceRecord& out) noexcept;
    // 뿌리 Select: 두 쪽을 내보내고 칸마다 고른다.
    Result<bool> ExportSelect(const SurfaceRecord& record, SurfaceRecord& out) noexcept;
    // 트리를 slab 하나로 접는다(thickness는 cm, bottom은 층 바닥 여부).
    Result<void> Fold(IrValueId surface, IrValueId thickness, bool bottom,
                      SurfaceSlab& out) noexcept;
    // slab 입력(raw — MeanFreePath는 입력 MFP, RadiusScale 자리는 MFP Scale)으로 slab 값 묶음을
    // 만든다(UE InternalGetSubstrateSlabBSDF). profile이면 Diffusion이 Profile이 된다.
    Result<void> BuildSlab(const SurfaceSlab& in, SlabSubsurface type, bool profile,
                           IrValueId thickness, bool bottom, SurfaceSlab& out) noexcept;
    // 고전 preset을 slab으로 바꾼다(UE SubstrateConvertLegacyMaterialDynamic).
    Result<void> LegacySlab(const SurfaceRecord& record, SurfaceSlab& out) noexcept;
    Result<void> LegacyModelSlab(const SurfaceRecord& record, ShadingModel model,
                                 SurfaceSlab& out) noexcept;
    // 연산 parameter blending이다(UE S:2853–3361).
    void BlendHorizontal(SurfaceSlab& background, SurfaceSlab& foreground, IrValueId mix,
                         SurfaceSlab& out) noexcept;
    void BlendAdd(SurfaceSlab& a, SurfaceSlab& b, SurfaceSlab& out) noexcept;
    void BlendVertical(SurfaceSlab& top, SurfaceSlab& base, SurfaceSlab& out) noexcept;
    void SelectSlab(IrValueId condition, const SurfaceSlab& whenTrue, const SurfaceSlab& whenFalse,
                    SurfaceSlab& out) noexcept;
    // 섞기 전 F90 굽기의 NoV다(그 slab의 노멀 — HLSLT:13597–13598).
    IrValueId SlabNoV(const SurfaceSlab& slab) noexcept;
    // Profile 병합 뒤 SSS 종류를 정리한다(RadiusScale 0이면 None, 그 밖은 MFP 0이면 None).
    IrValueId MergedSubsurface(IrValueId merged, IrValueId radiusScale, IrValueId mfp) noexcept;
    // slab의 F90을 F0에 굽는다(UE SubstrateBakeF90ToReflectivity).
    void BakeF90(SurfaceSlab& slab, IrValueId noV) noexcept;
    // slab을 고전 칸으로 내보낸다(UE SubstrateMaterialExportOut — origin은 경고 위치).
    Result<void> ExportSlab(const SurfaceSlab& slab, const Guid& origin,
                            SurfaceRecord& out) noexcept;
    // 칸 하나의 기본값이다(Select가 한쪽만 쓴 칸을 채운다 — DefaultSurfaceAttributes).
    IrValueId FieldDefault(SurfaceField field) noexcept;

    Translator& translator_;
    LoweringContext& context_;
    SurfaceMath math_;
    SystemAllocator scratch_;
    Array<FoldedSlab> folded_;
    IrValueId camera_ = NoIrValue;
};

} // namespace rocks::material::detail
