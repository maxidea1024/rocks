#include "SurfaceExporter.h"

#include <MaterialSchema/MaterialTraitEnums.h>
#include <Platform/Memory.h>
#include <RenderData/MaterialEnums.h>

#include "LoweringContext.h"
#include "SurfaceShadingModels.h"
#include "Translator.h"

// 표면 접기·내보내기다(Docs/Design/MaterialSystem 6.11절 MS2d2). 식은 UE 5.8 Substrate
// (Substrate.ush(S)·SubstrateExport.ush(SE))와 같다.
namespace rocks::material::detail {

namespace {

// UE SUBSTRATE_EPSILON(S:169)이다.
constexpr float ExportEpsilon = 1e-10f;
// UE SUBSTRATE_LAYER_DEFAULT_THICKNESS_CM·SUBSTRATE_SIMPLEVOLUME_THICKNESS_CM이다.
constexpr float ExportDefaultThicknessCm = 0.01f;
constexpr float ExportSimpleVolumeThicknessCm = 100.0f;
constexpr float ExportCentimeterToMeter = 0.01f;
// 유전체 F0 기준(UE F0ToMetallic·F0ToDielectricSpecular — SC:107–197)이다.
constexpr float ExportDielectricF0 = 0.08f;
constexpr float ExportMetalF0Range = 0.4f - 0.08f;
// F90 굽기의 Fresnel 늦춤 배율(UE Pow5((1 − NoV) × 1.2) — S:3686)과 거칠기 페이드 배율이다.
constexpr float ExportF90FresnelShift = 1.2f;
constexpr float ExportF90RoughnessFade = 2.0f;
// SSS profile 반경 배율의 하한(UE SSSS_OPACITY_THRESHOLD_EPS — SubsurfaceProfileCommon.ush:32)이다.
constexpr float ExportProfileThreshold = 0.1f;
// 고전 기본값이다(DefaultSurfaceAttributes — SurfaceAttributes.rsh).
constexpr float ExportDefaultSpecular = 0.5f;
constexpr float ExportDefaultRoughness = 0.5f;
constexpr float ExportDefaultCustomData0 = 1.0f;
constexpr float ExportDefaultCustomData1 = 0.1f;

uint32 ExportModel(ShadingModel model) noexcept {
    return static_cast<uint32>(model);
}

uint32 ExportSubsurface(SlabSubsurface type) noexcept {
    return static_cast<uint32>(type);
}

bool ExportTranslucent(BlendMode mode) noexcept {
    return mode != BlendMode::Opaque && mode != BlendMode::Masked;
}

// 값이 float 상수 value(모든 성분)인가(내보낸 칸이 기본값이면 쓰지 않는다).
bool ExportConstantEquals(const IrBuilder& builder, IrValueId id, float value) noexcept {
    if (id == NoIrValue) {
        return false;
    }

    const IrValue& item = builder.Value(id);
    if (item.op != IrOp::Constant || item.type.scalar != IrScalar::Float) {
        return false;
    }

    for (uint8 index = 0; index < item.type.components; ++index) {
        const uint32 bits = static_cast<uint32>(item.immediate[index]);
        if (platform::BitCast<float>(bits) != value) {
            return false;
        }
    }

    return true;
}

// 고전 preset 변환의 상수다(UE SubstrateLegacyConversion.ush — MFP 하한 1e-5 cm, profile 위상
// 비등방 0.93).
constexpr float LegacyMinimumMfpCm = 1e-5f;
constexpr float LegacyMeterToCentimeter = 100.0f;
constexpr float LegacyProfilePhaseAnisotropy = 0.93f;
// TransmittanceToMeanFreePath의 하한(UE ParticipatingMediaCommon.ush:170–180)이다.
constexpr float LegacyTransmittanceFloor = 1e-12f;
// Vertical 접기의 상수다(UE S:3132–3297, ParticipatingMediaCommon.ush).
constexpr float VerticalMaxDielectricF0 = 0.99f;
constexpr float VerticalMinimumExtinction = 1e-6f;
constexpr float VerticalMinimumMfpMeter = 1e-12f;
constexpr float VerticalMinimumNoV = 1e-4f;
constexpr float VerticalMinimumCoverage = 1e-6f;
constexpr float VerticalTopNormalWeight = 0.5f;

// UE TransmittanceToMeanFreePath(T, d) = 1/max(ε, −log(clamp(T, ε, 1))/max(ε, d))(m)다.
IrValueId LegacyTransmittanceToMfp(SurfaceMath& m, IrValueId transmittance,
                                   IrValueId distance) noexcept {
    const IrValueId floor = m.Scalar(LegacyTransmittanceFloor);
    const IrValueId depth =
        m.Mul(m.Scalar(-1.0f), m.Log(m.Clamp(transmittance, floor, m.Scalar(1.0f))));
    return m.Div(m.Scalar(1.0f), m.Max(floor, m.Div(depth, m.Max(floor, distance))));
}

// UE IsotropicMediumSlabPunctualDirectionalAlbedoALU(ParticipatingMediaCommon.ush:212–225)와
// IsotropicMediumSlabDirectionalAlbedoFade(:197–209)다(baseColor는 위층 DiffuseAlbedo, mfp는 m).
IrValueId VerticalTopAlbedo(SurfaceMath& m, IrValueId baseColor, IrValueId mfp) noexcept {
    const IrValueId x = m.Max(m.Scalar(0.01f), mfp);
    const IrValueId one =
        m.Div(m.Scalar(0.0855674f),
              m.Add(m.Scalar(0.237742f),
                    m.Add(x, m.Div(m.Sub(m.Scalar(0.0310849f), x),
                                   m.Add(m.Mul(m.Scalar(1.95492f), x), m.Scalar(2.07238f))))));
    const IrValueId tenth =
        m.Div(m.Scalar(0.0167964f),
              m.Add(m.Mul(m.Scalar(0.541037f),
                          m.Mul(m.Pow(m.Vector3(1.17902f), m.Div(m.Scalar(-4.33046f), x)),
                                m.Add(m.Scalar(-0.294969f), x))),
                    m.Scalar(0.797592f)));
    const IrValueId evaluated =
        m.Lerp(tenth, one, m.Div(m.Sub(baseColor, m.Scalar(0.1f)), m.Scalar(0.9f)));
    const IrValueId fade =
        m.Mul(m.Saturate(m.Mul(baseColor, m.Scalar(10.0f))),
              m.Saturate(m.OneMinus(m.Div(m.Sub(x, m.Scalar(20.0f)), m.Scalar(980.0f)))));
    return m.Mul(evaluated, fade);
}

} // namespace

SurfaceExporter::SurfaceExporter(Translator& translator, LoweringContext& context) noexcept
    : translator_(translator), context_(context), math_(context), folded_(scratch_) {
}

Result<bool> SurfaceExporter::Export(IrValueId surface, SurfaceRecord& out) noexcept {
    return ExportSurface(surface, false, out);
}

Result<bool> SurfaceExporter::ExportSurface(IrValueId surface, bool underOperator,
                                            SurfaceRecord& out) noexcept {
    const SurfaceRecord* found = translator_.FindSurface(surface);
    if (found == nullptr) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    // 기록 배열은 내보내는 동안 늘지 않지만, 복사해 두면 아래 식이 그 수명에 기대지 않는다.
    const SurfaceRecord record = *found;
    bool fold = false;
    switch (record.kind) {
    case SurfaceKind::Classic:
        // 연산 아래의 고전 preset(slab 종류)은 slab으로 바꾼다(UE SubstrateConvertLegacyMaterial —
        // 뿌리에 바로 이어지면 고전 칸 그대로, 10.3절).
        fold = underOperator && record.flow == SurfaceFlowBit(SurfaceFlow::Slab);
        if (!fold) {
            out = record;
            return Result<bool>::Success(true);
        }

        break;
    case SurfaceKind::Unlit:
    case SurfaceKind::VolumetricFogCloud:
    case SurfaceKind::Hair:
    case SurfaceKind::Eye:
    case SurfaceKind::SingleLayerWater:
        out = record;
        return Result<bool>::Success(true);
    case SurfaceKind::Decal: {
        // UE SubstrateWeightParameterBlending: 자식 표면의 coverage(없으면 opacity 칸)에 Coverage가
        // 곱해진다.
        SurfaceRecord inner;
        auto exported = ExportSurface(record.children[0], true, inner);
        if (!exported || !exported.Value()) {
            return exported;
        }

        const IrValueId coverage = math_.Saturate(record.operand);
        const IrValueId base =
            inner.coverage != NoIrValue ? inner.coverage : inner.Field(SurfaceField::Opacity);
        const IrValueId opacity = base == NoIrValue ? coverage : math_.Mul(base, coverage);
        if (auto status = math_.Status(); !status) {
            return Result<bool>::Failure(status.Error());
        }

        out = inner;
        out.usage = record.usage;
        out.node = record.node;
        out.Field(SurfaceField::Opacity) = opacity;
        out.coverage = opacity;
        return Result<bool>::Success(true);
    }
    case SurfaceKind::Slab:
        fold = true;
        break;
    case SurfaceKind::Operator:
        if (record.op == SurfaceOperator::Select) {
            return ExportSelect(record, out);
        }

        if (record.op == SurfaceOperator::Weight &&
            record.flow == SurfaceFlowBit(SurfaceFlow::Unlit)) {
            // Unlit 위의 Weight: emissive × w, coverage × w(UE 내보내기 EmissiveLuminance).
            SurfaceRecord inner;
            auto exported = ExportSurface(record.children[0], true, inner);
            if (!exported || !exported.Value()) {
                return exported;
            }

            SurfaceMath& m = math_;
            const IrValueId weight = m.Saturate(record.operand);
            IrValueId base =
                inner.coverage != NoIrValue ? inner.coverage : inner.Field(SurfaceField::Opacity);
            if (base == NoIrValue) {
                base = m.Scalar(1.0f);
            }

            out = inner;
            out.node = record.node;
            if (inner.Field(SurfaceField::Emissive) != NoIrValue) {
                out.Field(SurfaceField::Emissive) =
                    m.Mul(inner.Field(SurfaceField::Emissive), weight);
            }

            out.coverage = m.Mul(base, weight);
            if (auto status = m.Status(); !status) {
                return Result<bool>::Failure(status.Error());
            }

            return Result<bool>::Success(true);
        }

        fold = true;
        break;
    }

    if (!fold) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    SurfaceSlab slab;
    if (auto folded = Fold(surface, math_.Scalar(ExportDefaultThicknessCm), true, slab); !folded) {
        return Result<bool>::Failure(folded.Error());
    }

    out = SurfaceRecord();
    out.kind = SurfaceKind::Slab;
    out.usage = record.usage;
    out.node = record.node;
    if (auto exported = ExportSlab(slab, record.node, out); !exported) {
        return Result<bool>::Failure(exported.Error());
    }

    // 뿌리 BSDF의 추가 칸(MS2d3 출력)이다. 연산 아래의 추가 칸은 버린다(closure 하나).
    if (record.kind == SurfaceKind::Slab) {
        out.secondNormal = record.secondNormal;
    }

    // slab IsThin의 투과 색(고전 ThinTranslucent 변환의 역 — T = exp(−0.01/MFP))이다.
    bool thin = false;
    if (!math_.ConstantBool(slab[SlabValue::IsThin], thin) || thin) {
        SurfaceMath& m = math_;
        out.thinTransmittance =
            m.Exp(m.Div(m.Scalar(-ExportDefaultThicknessCm),
                        m.Max(m.Scalar(ExportEpsilon), slab[SlabValue::MeanFreePath])));
        out.thinCoverage = m.Scalar(1.0f);
        if (auto status = m.Status(); !status) {
            return Result<bool>::Failure(status.Error());
        }
    }

    return Result<bool>::Success(true);
}

IrValueId SurfaceExporter::FieldDefault(SurfaceField field) noexcept {
    SurfaceMath& m = math_;
    switch (field) {
    case SurfaceField::BaseColor:
    case SurfaceField::Emissive:
        return m.Vector3(0.0f);
    case SurfaceField::SubsurfaceColor:
        return m.Vector3(1.0f);
    case SurfaceField::Metallic:
    case SurfaceField::Anisotropy:
        return m.Scalar(0.0f);
    case SurfaceField::Specular:
        return m.Scalar(ExportDefaultSpecular);
    case SurfaceField::Roughness:
        return m.Scalar(ExportDefaultRoughness);
    case SurfaceField::CustomData0:
        return m.Scalar(ExportDefaultCustomData0);
    case SurfaceField::CustomData1:
        return m.Scalar(ExportDefaultCustomData1);
    case SurfaceField::Opacity:
    case SurfaceField::AmbientOcclusion:
        return m.Scalar(1.0f);
    case SurfaceField::ShadingModel:
        return m.UInt(ExportModel(ShadingModel::DefaultLit));
    case SurfaceField::Normal:
    case SurfaceField::Tangent: {
        auto vertex =
            context_.ContextValue(field == SurfaceField::Normal ? "VertexNormal" : "VertexTangent");
        return vertex ? vertex.Value() : NoIrValue;
    }
    }

    return NoIrValue;
}

Result<bool> SurfaceExporter::ExportSelect(const SurfaceRecord& record,
                                           SurfaceRecord& out) noexcept {
    // 내보내기는 점별 함수이므로 BSDF를 고른 뒤 내보내는 것과 같다(UE S:3338–3361 — Hair·Eye도
    // 된다). SelectValue > Threshold면 B다(UE If — 같으면 A).
    SurfaceRecord sides[2];
    for (usize index = 0; index < 2; ++index) {
        auto exported = ExportSurface(record.children[index], true, sides[index]);
        if (!exported || !exported.Value()) {
            return exported;
        }
    }

    SurfaceMath& m = math_;
    const IrValueId condition = m.Greater(record.operand, m.Scalar(record.threshold));
    out = SurfaceRecord();
    out.kind = SurfaceKind::Operator;
    out.usage = record.usage;
    out.node = record.node;
    for (usize index = 0; index < SurfaceFieldCount; ++index) {
        const SurfaceField field = static_cast<SurfaceField>(index);
        IrValueId a = sides[0].Field(field);
        IrValueId b = sides[1].Field(field);
        if (a == NoIrValue && b == NoIrValue) {
            continue;
        }

        a = a == NoIrValue ? FieldDefault(field) : a;
        b = b == NoIrValue ? FieldDefault(field) : b;
        out.Field(field) = m.Select(condition, b, a);
    }

    IrValueId coverages[2] = {NoIrValue, NoIrValue};
    for (usize index = 0; index < 2; ++index) {
        coverages[index] =
            sides[index].coverage != NoIrValue ? sides[index].coverage : m.Scalar(1.0f);
    }

    out.coverage = m.Select(condition, coverages[1], coverages[0]);
    if (auto status = m.Status(); !status) {
        return Result<bool>::Failure(status.Error());
    }

    return Result<bool>::Success(true);
}

Result<void> SurfaceExporter::Fold(IrValueId surface, IrValueId thickness, bool bottom,
                                   SurfaceSlab& out) noexcept {
    for (const FoldedSlab& item : folded_) {
        if (item.surface == surface && item.thickness == thickness && item.bottom == bottom) {
            out = item.slab;
            return Result<void>::Success();
        }
    }

    const SurfaceRecord* found = translator_.FindSurface(surface);
    if (found == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const SurfaceRecord record = *found;
    SurfaceMath& m = math_;
    switch (record.kind) {
    case SurfaceKind::Slab: {
        if (auto built = BuildSlab(record.slab, static_cast<SlabSubsurface>(record.subsurfaceType),
                                   false, thickness, bottom, out);
            !built) {
            return built;
        }

        break;
    }
    case SurfaceKind::Classic:
        if (auto built = LegacySlab(record, out); !built) {
            return built;
        }

        break;
    case SurfaceKind::Operator: {
        SurfaceSlab first;
        SurfaceSlab second;
        switch (record.op) {
        case SurfaceOperator::HorizontalMix:
        case SurfaceOperator::Add:
        case SurfaceOperator::Select:
            if (auto folded = Fold(record.children[0], thickness, bottom, first); !folded) {
                return folded;
            }

            if (auto folded = Fold(record.children[1], thickness, bottom, second); !folded) {
                return folded;
            }

            if (record.op == SurfaceOperator::HorizontalMix) {
                BlendHorizontal(first, second, record.operand, out);
            } else if (record.op == SurfaceOperator::Add) {
                BlendAdd(first, second, out);
            } else {
                SelectSlab(m.Greater(record.operand, m.Scalar(record.threshold)), second, first,
                           out);
            }

            break;
        case SurfaceOperator::VerticalLayer:
            // 두께는 Top 쪽에만 쌓이고 Base는 부모 두께다(UE MES:2971–2988). Top은 층 위다.
            if (auto folded = Fold(record.children[0], record.operand, false, first); !folded) {
                return folded;
            }

            if (auto folded = Fold(record.children[1], thickness, bottom, second); !folded) {
                return folded;
            }

            BlendVertical(first, second, out);
            break;
        case SurfaceOperator::Weight:
            if (auto folded = Fold(record.children[0], thickness, bottom, out); !folded) {
                return folded;
            }

            // UE SubstrateWeightParameterBlending(S:3308–3327).
            out[SlabValue::Coverage] = m.Mul(out[SlabValue::Coverage], m.Saturate(record.operand));
            break;
        }

        break;
    }
    default:
        // 허용 표가 막는다(연산 lowering — 6.11절 MS2d2).
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto status = m.Status(); !status) {
        return status;
    }

    FoldedSlab item;
    item.surface = surface;
    item.thickness = thickness;
    item.bottom = bottom;
    item.slab = out;
    if (auto added = folded_.TryPushBack(item); !added) {
        return added;
    }

    return Result<void>::Success();
}

Result<void> SurfaceExporter::BuildSlab(const SurfaceSlab& in, SlabSubsurface type, bool profile,
                                        IrValueId thickness, bool bottom,
                                        SurfaceSlab& out) noexcept {
    SurfaceMath& m = math_;
    out = in;
    // UE InternalGetSubstrateSlabBSDF(S:1558–1771) 순서다.
    const IrValueId scale = m.Saturate(in[SlabValue::RadiusScale]);
    const IrValueId mfp = m.Mul(in[SlabValue::MeanFreePath], scale);
    const IrValueId mfpPlugged = m.Greater(m.Max3(mfp), m.Scalar(0.0f));
    // 반투명(forward)은 diffusion을 하지 못한다(S:1590–1593).
    if (ExportTranslucent(context_.Traits().blendMode) &&
        (type == SlabSubsurface::Diffusion || type == SlabSubsurface::Profile)) {
        type = SlabSubsurface::None;
    }

    const IrValueId none = m.UInt(ExportSubsurface(SlabSubsurface::None));
    IrValueId subsurface = NoIrValue;
    bool profiled = false;
    if (!bottom) {
        type = SlabSubsurface::SimpleVolume;
        subsurface = m.UInt(ExportSubsurface(type));
    } else if (type == SlabSubsurface::Diffusion && profile) {
        // profile이 있으면 Diffusion이 Profile이다(배율 0이면 MFP 규칙 — S:1599–1607).
        profiled = true;
        subsurface = m.Select(m.Greater(scale, m.Scalar(0.0f)),
                              m.UInt(ExportSubsurface(SlabSubsurface::Profile)),
                              m.Select(mfpPlugged, m.UInt(ExportSubsurface(type)), none));
    } else {
        // MFP가 모두 0이면 SSS가 없다.
        subsurface = m.Select(mfpPlugged, m.UInt(ExportSubsurface(type)), none);
    }

    out[SlabValue::Subsurface] = subsurface;
    out[SlabValue::VolumeMeanFreePath] = mfp;
    out[SlabValue::MeanFreePath] = mfp;
    out[SlabValue::Thickness] = thickness;
    if (type == SlabSubsurface::Wrap || type == SlabSubsurface::TwoSidedWrap) {
        // 계산 공간(100 cm slab)으로 바꾼다(S:1650–1654, RescaleMFPToComputationSpace).
        const IrValueId rescaled =
            m.Max(m.Mul(mfp, m.Div(m.Scalar(ExportSimpleVolumeThicknessCm),
                                   m.Max(m.Scalar(ExportEpsilon), thickness))),
                  m.Scalar(0.0f));
        out[SlabValue::MeanFreePath] = rescaled;
        out[SlabValue::Thickness] =
            m.Select(mfpPlugged, m.Scalar(ExportSimpleVolumeThicknessCm), thickness);
    }

    out[SlabValue::RadiusScale] = profiled ? scale : m.Scalar(0.0f);
    out[SlabValue::Coverage] = m.Scalar(1.0f);
    out[SlabValue::IsThin] = bottom && context_.Traits().thinSurface ? mfpPlugged : m.Bool(false);
    // 표지는 값으로 정한다(S:1657–1714).
    out[SlabValue::HasF90] = m.Less(m.Min3(in[SlabValue::F90]), m.Scalar(1.0f));
    out[SlabValue::HasFuzz] = m.Greater(in[SlabValue::FuzzAmount], m.Scalar(0.0f));
    out[SlabValue::HasHaze] =
        m.Or(m.And(m.NotEqual(in[SlabValue::HazeRoughness], in[SlabValue::Roughness]),
                   m.Greater(in[SlabValue::HazeWeight], m.Scalar(0.0f))),
             in[SlabValue::SimpleClearCoat]);
    out[SlabValue::HasAnisotropy] = m.NotEqual(in[SlabValue::Anisotropy], m.Scalar(0.0f));
    return m.Status();
}

Result<void> SurfaceExporter::LegacySlab(const SurfaceRecord& record, SurfaceSlab& out) noexcept {
    const IrValueId model = record.Field(SurfaceField::ShadingModel);
    const IrValue& value = context_.Builder().Value(model);
    if (value.op == IrOp::Constant) {
        return LegacyModelSlab(record, static_cast<ShadingModel>(value.immediate[0]), out);
    }

    // 픽셀마다 고르는 shading model은 닿는 상수마다 slab을 만들고 model == k로 고른다(UE
    // SubstrateConvertLegacyMaterialDynamic).
    auto reached = ReachableShadingModels(context_.Builder(), model);
    if (!reached) {
        return Result<void>::Failure(reached.Error());
    }

    SurfaceMath& m = math_;
    bool first = true;
    for (uint32 number = 0; number < MaxShadingModelCount; ++number) {
        if ((reached.Value() & (1u << number)) == 0) {
            continue;
        }

        SurfaceSlab candidate;
        if (auto built = LegacyModelSlab(record, static_cast<ShadingModel>(number), candidate);
            !built) {
            return built;
        }

        if (first) {
            out = candidate;
            first = false;
            continue;
        }

        SelectSlab(m.Equal(model, m.UInt(number)), candidate, out, out);
    }

    return m.Status();
}

Result<void> SurfaceExporter::LegacyModelSlab(const SurfaceRecord& record, ShadingModel model,
                                              SurfaceSlab& out) noexcept {
    SurfaceMath& m = math_;
    // 고전 입력이다(SLC static처럼 metallic·opacity·투과 색·coverage를 saturate한다).
    const IrValueId baseColor = record.Field(SurfaceField::BaseColor);
    const IrValueId metallic = m.Saturate(record.Field(SurfaceField::Metallic));
    const IrValueId specular = record.Field(SurfaceField::Specular);
    const IrValueId roughness = record.Field(SurfaceField::Roughness);
    const IrValueId opacity = m.Saturate(record.Field(SurfaceField::Opacity));
    const IrValueId subsurfaceColor = record.Field(SurfaceField::SubsurfaceColor);
    const IrValueId clearCoat = record.Field(SurfaceField::CustomData0);
    const IrValueId clearCoatRoughness = record.Field(SurfaceField::CustomData1);
    const bool translucent = ExportTranslucent(context_.Traits().blendMode);
    // anisotropy는 DefaultLit·ClearCoat만 받는다(UE LegacyShadingModelSupportsAnisotropy).
    const bool anisotropic = model == ShadingModel::DefaultLit || model == ShadingModel::ClearCoat;
    SurfaceSlab in;
    const IrValueId dielectric = m.Mul(m.Scalar(ExportDielectricF0), specular);
    in[SlabValue::DiffuseAlbedo] = m.Mul(baseColor, m.OneMinus(metallic));
    in[SlabValue::F0] = m.Lerp(m.Splat3(dielectric), baseColor, metallic);
    in[SlabValue::F90] = m.Vector3(1.0f);
    in[SlabValue::Roughness] = roughness;
    in[SlabValue::Anisotropy] =
        anisotropic ? record.Field(SurfaceField::Anisotropy) : m.Scalar(0.0f);
    in[SlabValue::Normal] = record.Field(SurfaceField::Normal);
    in[SlabValue::Tangent] = record.Field(SurfaceField::Tangent);
    in[SlabValue::MeanFreePath] = m.Vector3(0.0f);
    in[SlabValue::RadiusScale] = m.Scalar(1.0f);
    in[SlabValue::PhaseAnisotropy] = m.Scalar(0.0f);
    in[SlabValue::Emissive] = record.Field(SurfaceField::Emissive);
    in[SlabValue::HazeRoughness] = m.Scalar(0.0f);
    in[SlabValue::HazeWeight] = m.Scalar(0.0f);
    in[SlabValue::FuzzRoughness] = m.Add(m.Scalar(0.5f), m.Mul(m.Scalar(0.5f), roughness));
    in[SlabValue::FuzzAmount] = m.Scalar(0.0f);
    in[SlabValue::FuzzColor] = m.Vector3(0.0f);
    in[SlabValue::SimpleClearCoat] = m.Bool(false);
    SlabSubsurface type = SlabSubsurface::None;
    bool profile = false;
    IrValueId thickness = m.Scalar(ExportDefaultThicknessCm);
    IrValueId coverage = translucent ? opacity : m.Scalar(1.0f);
    switch (model) {
    case ShadingModel::Unlit:
        // 발광 slab이다(SLC:263–278).
        in[SlabValue::DiffuseAlbedo] = m.Vector3(0.0f);
        in[SlabValue::F0] = m.Vector3(0.0f);
        in[SlabValue::Roughness] = m.Scalar(0.0f);
        in[SlabValue::FuzzRoughness] = m.Scalar(0.5f);
        break;
    case ShadingModel::Subsurface:
        // 1 m slab 투과 색에서 MFP(SLC:300–340). 반투명이면 MFP 0·coverage = opacity다.
        type = SlabSubsurface::Wrap;
        thickness = m.Scalar(ExportSimpleVolumeThicknessCm);
        in[SlabValue::PhaseAnisotropy] = m.OneMinus(opacity);
        in[SlabValue::MeanFreePath] =
            translucent ? m.Vector3(0.0f)
                        : m.Max(m.Scalar(LegacyMinimumMfpCm),
                                m.Mul(LegacyTransmittanceToMfp(m, subsurfaceColor, m.Scalar(1.0f)),
                                      m.Scalar(LegacyMeterToCentimeter)));
        break;
    case ShadingModel::TwoSidedFoliage:
        type = SlabSubsurface::TwoSidedWrap;
        in[SlabValue::PhaseAnisotropy] = m.OneMinus(opacity);
        in[SlabValue::MeanFreePath] =
            m.Max(m.Scalar(LegacyMinimumMfpCm),
                  m.Mul(LegacyTransmittanceToMfp(
                            m, subsurfaceColor,
                            m.Scalar(ExportDefaultThicknessCm * ExportCentimeterToMeter)),
                        m.Scalar(LegacyMeterToCentimeter)));
        break;
    case ShadingModel::SubsurfaceProfile:
        // opacity가 하한보다 크면 profile(기본 profile — UE bSupportDefaultSSSProfile)이다.
        type = SlabSubsurface::Diffusion;
        profile = true;
        in[SlabValue::RadiusScale] =
            m.Select(m.Greater(opacity, m.Scalar(ExportProfileThreshold)), opacity, m.Scalar(0.0f));
        in[SlabValue::PhaseAnisotropy] = m.Scalar(LegacyProfilePhaseAnisotropy);
        break;
    case ShadingModel::ClearCoat:
        in[SlabValue::HazeWeight] = clearCoat;
        in[SlabValue::HazeRoughness] = clearCoatRoughness;
        in[SlabValue::SimpleClearCoat] =
            record.secondNormal != NoIrValue ? m.Bool(true) : m.Greater(clearCoat, m.Scalar(0.0f));
        break;
    case ShadingModel::Cloth:
        in[SlabValue::F0] = m.Mul(in[SlabValue::F0], m.OneMinus(clearCoat));
        in[SlabValue::FuzzAmount] = clearCoat;
        in[SlabValue::FuzzColor] = subsurfaceColor;
        break;
    case ShadingModel::ThinTranslucent: {
        // SLC:478–515.
        type = SlabSubsurface::SimpleVolume;
        const IrValueId transmitted =
            m.Lerp(m.Saturate(record.thinTransmittance), m.Vector3(0.0f), opacity);
        in[SlabValue::MeanFreePath] =
            m.Mul(LegacyTransmittanceToMfp(
                      m, transmitted, m.Scalar(ExportDefaultThicknessCm * ExportCentimeterToMeter)),
                  m.Scalar(LegacyMeterToCentimeter));
        const IrValueId top = m.Lerp(m.Splat3(dielectric), baseColor, metallic);
        in[SlabValue::F0] = m.Lerp(m.Splat3(dielectric), top, opacity);
        in[SlabValue::DiffuseAlbedo] =
            m.Lerp(m.Vector3(0.0f), m.Mul(baseColor, m.OneMinus(metallic)), opacity);
        coverage = m.Saturate(record.thinCoverage);
        break;
    }
    default:
        // DefaultLit(그리고 연산 lowering이 막는 Hair·Eye·SingleLayerWater)은 금속도 slab이다.
        break;
    }

    if (auto built = BuildSlab(in, type, profile, thickness, true, out); !built) {
        return built;
    }

    out[SlabValue::Coverage] = coverage;
    return m.Status();
}

IrValueId SurfaceExporter::SlabNoV(const SurfaceSlab& slab) noexcept {
    if (camera_ == NoIrValue) {
        auto camera = context_.ContextValue("CameraVector");
        if (!camera) {
            return NoIrValue;
        }

        camera_ = camera.Value();
    }

    return math_.Saturate(math_.Dot(slab[SlabValue::Normal], camera_));
}

IrValueId SurfaceExporter::MergedSubsurface(IrValueId merged, IrValueId radiusScale,
                                            IrValueId mfp) noexcept {
    SurfaceMath& m = math_;
    const IrValueId none = m.UInt(ExportSubsurface(SlabSubsurface::None));
    const IrValueId isProfile = m.Equal(merged, m.UInt(ExportSubsurface(SlabSubsurface::Profile)));
    return m.Select(isProfile, m.Select(m.Greater(radiusScale, m.Scalar(0.0f)), merged, none),
                    m.Select(m.Greater(m.Max3(mfp), m.Scalar(0.0f)), merged, none));
}

void SurfaceExporter::SelectSlab(IrValueId condition, const SurfaceSlab& whenTrue,
                                 const SurfaceSlab& whenFalse, SurfaceSlab& out) noexcept {
    SurfaceSlab result;
    for (usize index = 0; index < SlabValueCount; ++index) {
        result.values[index] =
            math_.Select(condition, whenTrue.values[index], whenFalse.values[index]);
    }

    out = result;
}

void SurfaceExporter::BlendHorizontal(SurfaceSlab& background, SurfaceSlab& foreground,
                                      IrValueId mix, SurfaceSlab& out) noexcept {
    SurfaceMath& m = math_;
    // UE SubstrateHorizontalMixingParameterBlending(S:2853–3006).
    BakeF90(background, SlabNoV(background));
    BakeF90(foreground, SlabNoV(foreground));
    const IrValueId safeMix = m.Saturate(mix);
    const IrValueId cB = m.Saturate(background[SlabValue::Coverage]);
    const IrValueId cF = m.Saturate(foreground[SlabValue::Coverage]);
    const IrValueId front = m.Mul(cF, safeMix);
    const IrValueId w =
        m.Div(front, m.Max(m.Scalar(ExportEpsilon), m.Add(front, m.Mul(cB, m.OneMinus(safeMix)))));
    SurfaceSlab result;
    const SlabValue lerped[] = {
        SlabValue::Emissive,      SlabValue::DiffuseAlbedo,      SlabValue::F0,
        SlabValue::F90,           SlabValue::FuzzColor,          SlabValue::Thickness,
        SlabValue::Roughness,     SlabValue::Anisotropy,         SlabValue::FuzzAmount,
        SlabValue::FuzzRoughness, SlabValue::HazeRoughness,      SlabValue::HazeWeight,
        SlabValue::MeanFreePath,  SlabValue::VolumeMeanFreePath, SlabValue::PhaseAnisotropy};
    for (SlabValue value : lerped) {
        result[value] = m.Lerp(background[value], foreground[value], w);
    }

    const SlabValue ored[] = {SlabValue::HasHaze, SlabValue::SimpleClearCoat, SlabValue::HasFuzz,
                              SlabValue::HasAnisotropy, SlabValue::IsThin};
    for (SlabValue value : ored) {
        result[value] = m.Or(background[value], foreground[value]);
    }

    result[SlabValue::HasF90] = m.Bool(false);
    const IrValueId profile = m.UInt(ExportSubsurface(SlabSubsurface::Profile));
    const IrValueId merged =
        m.MaxUInt(background[SlabValue::Subsurface], foreground[SlabValue::Subsurface]);
    // profile은 반경 배율만 섞는다(Rocks에는 profile id가 없다 — MPQ-SubsurfaceProfile).
    const IrValueId radius = m.Lerp(m.Select(m.Equal(background[SlabValue::Subsurface], profile),
                                             background[SlabValue::RadiusScale], m.Scalar(0.0f)),
                                    m.Select(m.Equal(foreground[SlabValue::Subsurface], profile),
                                             foreground[SlabValue::RadiusScale], m.Scalar(0.0f)),
                                    w);
    result[SlabValue::RadiusScale] = radius;
    result[SlabValue::Subsurface] =
        MergedSubsurface(merged, radius, result[SlabValue::MeanFreePath]);
    // coverage는 saturate 전 Mix로 섞는다(UE 그대로 — S:2992).
    result[SlabValue::Coverage] =
        m.Lerp(background[SlabValue::Coverage], foreground[SlabValue::Coverage], mix);
    result[SlabValue::Normal] =
        m.Normalize(m.Lerp(background[SlabValue::Normal], foreground[SlabValue::Normal], w));
    result[SlabValue::Tangent] =
        m.Normalize(m.Lerp(background[SlabValue::Tangent], foreground[SlabValue::Tangent], w));
    out = result;
}

void SurfaceExporter::BlendAdd(SurfaceSlab& a, SurfaceSlab& b, SurfaceSlab& out) noexcept {
    SurfaceMath& m = math_;
    // UE SubstrateAddParameterBlending(S:3008–3117).
    BakeF90(a, SlabNoV(a));
    BakeF90(b, SlabNoV(b));
    const IrValueId cA = m.Saturate(a[SlabValue::Coverage]);
    const IrValueId cB = m.Saturate(b[SlabValue::Coverage]);
    const IrValueId mixA = m.Div(cA, m.Max(m.Scalar(ExportEpsilon), m.Add(cA, cB)));
    SurfaceSlab result;
    result[SlabValue::Emissive] = m.Add(b[SlabValue::Emissive], a[SlabValue::Emissive]);
    const SlabValue summed[] = {SlabValue::DiffuseAlbedo, SlabValue::F0, SlabValue::F90,
                                SlabValue::FuzzColor};
    for (SlabValue value : summed) {
        result[value] = m.Saturate(m.Add(b[value], a[value]));
    }

    const SlabValue lerped[] = {
        SlabValue::Thickness,  SlabValue::Roughness,     SlabValue::Anisotropy,
        SlabValue::FuzzAmount, SlabValue::FuzzRoughness, SlabValue::HazeRoughness,
        SlabValue::HazeWeight, SlabValue::MeanFreePath,  SlabValue::VolumeMeanFreePath};
    for (SlabValue value : lerped) {
        result[value] = m.Lerp(b[value], a[value], mixA);
    }

    const SlabValue ored[] = {SlabValue::HasHaze, SlabValue::SimpleClearCoat, SlabValue::HasFuzz,
                              SlabValue::HasAnisotropy, SlabValue::IsThin};
    for (SlabValue value : ored) {
        result[value] = m.Or(a[value], b[value]);
    }

    // PhaseAnisotropy는 UE처럼 0이다(S:3008–3117에서 쓰지 않는다).
    result[SlabValue::PhaseAnisotropy] = m.Scalar(0.0f);
    result[SlabValue::HasF90] = m.Bool(false);
    const IrValueId profile = m.UInt(ExportSubsurface(SlabSubsurface::Profile));
    const IrValueId radius =
        m.Select(m.Equal(a[SlabValue::Subsurface], profile), a[SlabValue::RadiusScale],
                 m.Select(m.Equal(b[SlabValue::Subsurface], profile), b[SlabValue::RadiusScale],
                          m.Scalar(0.0f)));
    result[SlabValue::RadiusScale] = radius;
    result[SlabValue::Subsurface] =
        MergedSubsurface(m.MaxUInt(a[SlabValue::Subsurface], b[SlabValue::Subsurface]), radius,
                         result[SlabValue::MeanFreePath]);
    result[SlabValue::Coverage] = m.Saturate(m.Add(cB, cA));
    result[SlabValue::Normal] =
        m.Normalize(m.Lerp(b[SlabValue::Normal], a[SlabValue::Normal], mixA));
    result[SlabValue::Tangent] =
        m.Normalize(m.Lerp(b[SlabValue::Tangent], a[SlabValue::Tangent], mixA));
    out = result;
}

void SurfaceExporter::BlendVertical(SurfaceSlab& top, SurfaceSlab& base,
                                    SurfaceSlab& out) noexcept {
    SurfaceMath& m = math_;
    // UE SubstrateVerticalLayeringParameterBlending(S:3161–3306).
    const IrValueId topNoV = SlabNoV(top);
    BakeF90(top, topNoV);
    BakeF90(base, SlabNoV(base));
    const IrValueId cT = m.Saturate(top[SlabValue::Coverage]);
    const IrValueId cB = m.Saturate(base[SlabValue::Coverage]);
    const IrValueId originalBaseMfp = base[SlabValue::MeanFreePath];
    // 굴절 NoV(UE SubstrateComputeTransmittanceNoV — V를 IOR(F0.y)로 굴절, S:3132–3159).
    const IrValueId topF0 = m.Component(top[SlabValue::F0], 1);
    const IrValueId ior = m.Sub(
        m.Div(m.Scalar(2.0f), m.OneMinus(m.Sqrt(m.Min(topF0, m.Scalar(VerticalMaxDielectricF0))))),
        m.Scalar(1.0f));
    const IrValueId eta = m.Div(m.Scalar(1.0f), ior);
    const IrValueId refracted = m.Sqrt(
        m.Max(m.Scalar(0.0f),
              m.Sub(m.Scalar(1.0f), m.Mul(m.Mul(eta, eta), m.OneMinus(m.Mul(topNoV, topNoV))))));
    // 위층 매질(계산 공간 — EnableSlabBSDFSimpleVolumetric, CreateMediumFromBaseColorMFP).
    const IrValueId topMfpCm =
        m.Max(m.Mul(top[SlabValue::VolumeMeanFreePath],
                    m.Div(m.Scalar(ExportSimpleVolumeThicknessCm),
                          m.Max(m.Scalar(ExportEpsilon), top[SlabValue::Thickness]))),
              m.Scalar(0.0f));
    const IrValueId topMfp = m.Mul(topMfpCm, m.Scalar(ExportCentimeterToMeter));
    const IrValueId extinction =
        m.Max(m.Scalar(VerticalMinimumExtinction),
              m.Div(m.Scalar(1.0f), m.Max(m.Scalar(VerticalMinimumMfpMeter), topMfp)));
    // IsotropicMediumSlabTransmittance(1 m slab — 시선은 굴절 NoV, 빛은 수직).
    const IrValueId viewTransmittance = m.Exp(m.Mul(
        m.Scalar(-1.0f), m.Mul(extinction, m.Div(m.Scalar(1.0f), m.Max(m.Scalar(VerticalMinimumNoV),
                                                                       m.Abs(refracted))))));
    const IrValueId normalTransmittance = m.Exp(m.Mul(m.Scalar(-1.0f), extinction));
    const IrValueId slabTransmittance = m.Mul(viewTransmittance, normalTransmittance);
    // 위층 albedo는 ALU 근사다(IsotropicMediumSlabPunctualDirectionalAlbedoALU — Rocks에
    // SimpleVolume LUT가 없다, 편차).
    const IrValueId albedo = VerticalTopAlbedo(m, top[SlabValue::DiffuseAlbedo], topMfp);
    // GetVerticalLayeringInfo(SC:223–238).
    const IrValueId bothLayers = m.Mul(cT, cB);
    const IrValueId bottomOnly = m.Mul(m.OneMinus(cT), cB);
    const IrValueId coverage = m.Add(cT, bottomOnly);
    const IrValueId safeCoverage = m.Max(m.Scalar(ExportEpsilon), coverage);
    const IrValueId topToBottom = m.Saturate(
        m.Div(m.Add(m.Mul(bothLayers, m.Average3(slabTransmittance)), bottomOnly), safeCoverage));
    const IrValueId bottomFactor =
        m.Div(m.Add(m.Mul(bothLayers, slabTransmittance), bottomOnly), safeCoverage);
    const IrValueId topFactor = m.Div(cT, safeCoverage);
    SurfaceSlab result;
    result[SlabValue::Thickness] =
        m.Add(base[SlabValue::Thickness], m.Mul(cT, top[SlabValue::Thickness]));
    result[SlabValue::Emissive] = m.Add(m.Mul(bottomFactor, base[SlabValue::Emissive]),
                                        m.Mul(topFactor, top[SlabValue::Emissive]));
    result[SlabValue::DiffuseAlbedo] = m.Saturate(
        m.Add(m.Mul(bottomFactor, base[SlabValue::DiffuseAlbedo]), m.Mul(topFactor, albedo)));
    const SlabValue added[] = {SlabValue::F0, SlabValue::F90, SlabValue::FuzzColor};
    for (SlabValue value : added) {
        result[value] =
            m.Saturate(m.Add(m.Mul(bottomFactor, base[value]), m.Mul(topFactor, top[value])));
    }

    const SlabValue lerped[] = {SlabValue::Roughness, SlabValue::FuzzAmount,
                                SlabValue::FuzzRoughness, SlabValue::HazeRoughness,
                                SlabValue::HazeWeight};
    for (SlabValue value : lerped) {
        result[value] = m.Lerp(top[value], base[value], topToBottom);
    }

    result[SlabValue::Anisotropy] = m.Scalar(0.0f);
    result[SlabValue::HasAnisotropy] = m.Bool(false);
    result[SlabValue::PhaseAnisotropy] = m.Scalar(0.0f);
    result[SlabValue::RadiusScale] = m.Scalar(0.0f);
    result[SlabValue::HasF90] = m.Bool(false);
    // haze는 위층 것만, fuzz는 둘 다, thin은 아래층 것이다. SSS는 아래층만(profile은 버린다).
    result[SlabValue::HasHaze] = top[SlabValue::HasHaze];
    result[SlabValue::SimpleClearCoat] =
        m.Or(top[SlabValue::SimpleClearCoat], base[SlabValue::SimpleClearCoat]);
    result[SlabValue::HasFuzz] = m.Or(top[SlabValue::HasFuzz], base[SlabValue::HasFuzz]);
    result[SlabValue::IsThin] = base[SlabValue::IsThin];
    result[SlabValue::Subsurface] =
        m.MinUInt(base[SlabValue::Subsurface], m.UInt(ExportSubsurface(SlabSubsurface::Diffusion)));
    result[SlabValue::MeanFreePath] = m.Mul(originalBaseMfp, topToBottom);
    if (!ExportTranslucent(context_.Traits().blendMode)) {
        result[SlabValue::VolumeMeanFreePath] =
            m.Mul(base[SlabValue::VolumeMeanFreePath], topToBottom);
        result[SlabValue::Coverage] = coverage;
    } else {
        // 반투명: 최대 coverage로 정규화한 층 정보로 MFP를 섞고 계산 공간으로 다시
        // 바꾼다(S:3270–3297).
        const IrValueId maxCoverage =
            m.Max(m.Scalar(VerticalMinimumCoverage),
                  m.Max(top[SlabValue::Coverage], base[SlabValue::Coverage]));
        const IrValueId t = m.Div(top[SlabValue::Coverage], maxCoverage);
        const IrValueId b = m.Div(base[SlabValue::Coverage], maxCoverage);
        const IrValueId volume =
            m.Add(m.Add(m.Mul(m.Mul(m.OneMinus(t), b), base[SlabValue::VolumeMeanFreePath]),
                        m.Mul(m.Mul(m.OneMinus(b), t), top[SlabValue::VolumeMeanFreePath])),
                  m.Mul(m.Mul(t, b), m.Min(top[SlabValue::VolumeMeanFreePath],
                                           base[SlabValue::VolumeMeanFreePath])));
        result[SlabValue::VolumeMeanFreePath] = volume;
        result[SlabValue::Coverage] = maxCoverage;
        result[SlabValue::MeanFreePath] = m.Max(
            m.Mul(volume, m.Div(m.Scalar(ExportSimpleVolumeThicknessCm),
                                m.Max(m.Scalar(ExportEpsilon), result[SlabValue::Thickness]))),
            m.Scalar(0.0f));
        result[SlabValue::Subsurface] = m.UInt(ExportSubsurface(SlabSubsurface::SimpleVolume));
    }

    // 노멀은 위층 coverage의 절반으로 섞는다(UE
    // VerticalLayeringParameterBlendingBSDFCoverageToNormalMix).
    const IrValueId normalMix = m.Mul(cT, m.Scalar(VerticalTopNormalWeight));
    result[SlabValue::Normal] =
        m.Normalize(m.Lerp(base[SlabValue::Normal], top[SlabValue::Normal], normalMix));
    result[SlabValue::Tangent] =
        m.Normalize(m.Lerp(base[SlabValue::Tangent], top[SlabValue::Tangent], normalMix));
    out = result;
}

void SurfaceExporter::BakeF90(SurfaceSlab& slab, IrValueId noV) noexcept {
    SurfaceMath& m = math_;
    // UE SubstrateBakeF90ToReflectivity(S:3676–3696).
    const IrValueId f90 = m.Saturate(slab[SlabValue::F90]);
    const IrValueId divisor = m.Max3(f90);
    const IrValueId hue =
        m.Select(m.Greater(divisor, m.Scalar(0.0f)), m.Div(f90, divisor), m.Vector3(1.0f));
    const IrValueId shift = m.Pow5(m.Mul(m.OneMinus(noV), m.Scalar(ExportF90FresnelShift)));
    IrValueId scale = m.Lerp(m.Vector3(1.0f), hue, shift);
    scale = m.Lerp(m.Vector3(1.0f), scale,
                   m.Saturate(m.OneMinus(
                       m.Mul(m.Scalar(ExportF90RoughnessFade), slab[SlabValue::Roughness]))));
    const IrValueId hasF90 = slab[SlabValue::HasF90];
    slab[SlabValue::F0] = m.Select(hasF90, m.Mul(slab[SlabValue::F0], scale), slab[SlabValue::F0]);
    slab[SlabValue::F90] = m.Select(hasF90, m.Vector3(1.0f), slab[SlabValue::F90]);
    slab[SlabValue::HasF90] = m.Bool(false);
}

Result<void> SurfaceExporter::ExportSlab(const SurfaceSlab& input, const Guid& origin,
                                         SurfaceRecord& out) noexcept {
    SurfaceMath& m = math_;
    SurfaceSlab slab = input;
    // F90 굽기의 NoV는 정점 노멀 기준이다(UE BPPS SurfaceWorldNormal = TangentToWorld[2]).
    auto vertexNormal = context_.ContextValue("VertexNormal");
    auto camera = context_.ContextValue("CameraVector");
    if (!vertexNormal || !camera) {
        return Result<void>::Failure(!vertexNormal ? vertexNormal.Error() : camera.Error());
    }

    const IrValueId noV = m.Saturate(m.Dot(vertexNormal.Value(), camera.Value()));
    BakeF90(slab, noV);
    // SimpleClearCoat가 아닌 haze는 거칠기에 굽는다(S:3728–3746).
    const IrValueId hazeIntoRoughness =
        m.And(slab[SlabValue::HasHaze], m.Not(slab[SlabValue::SimpleClearCoat]));
    const IrValueId roughness =
        m.Select(hazeIntoRoughness,
                 m.Lerp(slab[SlabValue::Roughness], slab[SlabValue::HazeRoughness],
                        slab[SlabValue::HazeWeight]),
                 slab[SlabValue::Roughness]);
    const IrValueId f0 = slab[SlabValue::F0];
    const IrValueId f0Max = m.Max3(f0);
    const IrValueId metal =
        m.Saturate(m.Div(m.Sub(f0Max, m.Scalar(ExportDielectricF0)), m.Scalar(ExportMetalF0Range)));
    const IrValueId subsurface = slab[SlabValue::Subsurface];
    // shading model 사슬(SE:1137–1300, S:4080–4110 — 위가 우선).
    const IrValueId isProfile =
        m.And(m.Equal(subsurface, m.UInt(ExportSubsurface(SlabSubsurface::Profile))),
              m.Greater(slab[SlabValue::RadiusScale], m.Scalar(ExportProfileThreshold)));
    const IrValueId isWrap = m.Equal(subsurface, m.UInt(ExportSubsurface(SlabSubsurface::Wrap)));
    const IrValueId isFoliage =
        m.Equal(subsurface, m.UInt(ExportSubsurface(SlabSubsurface::TwoSidedWrap)));
    const IrValueId isCloth = slab[SlabValue::HasFuzz];
    const IrValueId isClearCoat = m.And(slab[SlabValue::HasHaze], slab[SlabValue::SimpleClearCoat]);
    const IrValueId isThin = slab[SlabValue::IsThin];
    const IrValueId chain[] = {isProfile, isWrap, isFoliage, isCloth, isClearCoat, isThin};
    const uint32 models[] = {
        ExportModel(ShadingModel::SubsurfaceProfile), ExportModel(ShadingModel::Subsurface),
        ExportModel(ShadingModel::TwoSidedFoliage),   ExportModel(ShadingModel::Cloth),
        ExportModel(ShadingModel::ClearCoat),         ExportModel(ShadingModel::ThinTranslucent)};
    constexpr usize ChainLength = sizeof(models) / sizeof(models[0]);
    IrValueId model = m.UInt(ExportModel(ShadingModel::DefaultLit));
    for (usize index = ChainLength; index > 0; --index) {
        model = m.Select(chain[index - 1], m.UInt(models[index - 1]), model);
    }

    // 표로 나타낼 수 없는 조합은 우선순위로 하나를 고르고 경고한다(10.3절).
    usize constantTrue = 0;
    for (IrValueId condition : chain) {
        bool value = false;
        if (m.ConstantBool(condition, value) && value) {
            ++constantTrue;
        }
    }

    if (constantTrue > 1) {
        if (auto reported = translator_.Report(
                nodegraph::GraphDiagnosticSeverity::Warning,
                MaterialDiagnosticCode::UnsupportedCapability, origin, Name(),
                "Surface combines features of several shading models; lower-priority features "
                "are ignored");
            !reported) {
            return reported;
        }
    }

    const IrValueId sssModel = m.Or(isProfile, m.Or(isWrap, isFoliage));
    const IrValueId wrapModel = m.Or(isWrap, isFoliage);
    const IrValueId clothModel = m.And(m.Not(sssModel), isCloth);
    const IrValueId clearCoatModel = m.And(m.And(m.Not(sssModel), m.Not(isCloth)), isClearCoat);
    // anisotropy는 DefaultLit·ClearCoat만 남는다(SE:1305–1309).
    const IrValueId keepsAnisotropy =
        m.And(m.And(m.Not(sssModel), m.Not(isCloth)), m.Or(isClearCoat, m.Not(isThin)));
    const IrValueId anisotropy = m.Select(m.And(slab[SlabValue::HasAnisotropy], keepsAnisotropy),
                                          slab[SlabValue::Anisotropy], m.Scalar(0.0f));
    // SSS 투과 색(1 m slab — SE SubsurfaceColor 접근자).
    const IrValueId transmitted = m.Exp(m.Div(
        m.Scalar(-1.0f), m.Max(m.Scalar(ExportEpsilon), m.Mul(slab[SlabValue::MeanFreePath],
                                                              m.Scalar(ExportCentimeterToMeter)))));
    const IrValueId subsurfaceColor = m.Select(
        wrapModel, transmitted, m.Select(clothModel, slab[SlabValue::FuzzColor], m.Vector3(1.0f)));
    const IrValueId opacity =
        m.Select(isProfile, slab[SlabValue::RadiusScale],
                 m.Select(wrapModel, m.OneMinus(slab[SlabValue::PhaseAnisotropy]), m.Scalar(1.0f)));
    const IrValueId customData0 = m.Select(
        clothModel, slab[SlabValue::FuzzAmount],
        m.Select(clearCoatModel, slab[SlabValue::HazeWeight], m.Scalar(ExportDefaultCustomData0)));
    const IrValueId customData1 = m.Select(clearCoatModel, slab[SlabValue::HazeRoughness],
                                           m.Scalar(ExportDefaultCustomData1));
    out.Field(SurfaceField::BaseColor) = m.Lerp(slab[SlabValue::DiffuseAlbedo], f0, metal);
    out.Field(SurfaceField::Metallic) = metal;
    out.Field(SurfaceField::Specular) = m.Saturate(m.Div(f0Max, m.Scalar(ExportDielectricF0)));
    out.Field(SurfaceField::Roughness) = roughness;
    out.Field(SurfaceField::Anisotropy) = anisotropy;
    out.Field(SurfaceField::Emissive) = m.Mul(slab[SlabValue::Emissive], slab[SlabValue::Coverage]);
    // 기저는 이미 정규화되어 있다(입력은 BasisOr, 섞은 노멀은 SubstrateBlendNormal).
    out.Field(SurfaceField::Normal) = slab[SlabValue::Normal];
    out.Field(SurfaceField::Tangent) = slab[SlabValue::Tangent];
    out.Field(SurfaceField::SubsurfaceColor) = subsurfaceColor;
    out.Field(SurfaceField::CustomData0) = customData0;
    out.Field(SurfaceField::CustomData1) = customData1;
    out.Field(SurfaceField::Opacity) = opacity;
    out.Field(SurfaceField::ShadingModel) = model;
    out.coverage = slab[SlabValue::Coverage];
    if (auto status = m.Status(); !status) {
        return status;
    }

    // 기본값과 같은 상수 칸은 쓰지 않는다(생성 원문이 DefaultSurfaceAttributes 값을 둔다).
    struct Default {
        SurfaceField field;
        float value;
    };

    static constexpr Default Defaults[] = {
        {SurfaceField::BaseColor, 0.0f},
        {SurfaceField::Metallic, 0.0f},
        {SurfaceField::Specular, ExportDefaultSpecular},
        {SurfaceField::Roughness, ExportDefaultRoughness},
        {SurfaceField::Anisotropy, 0.0f},
        {SurfaceField::Emissive, 0.0f},
        {SurfaceField::SubsurfaceColor, 1.0f},
        {SurfaceField::CustomData0, ExportDefaultCustomData0},
        {SurfaceField::CustomData1, ExportDefaultCustomData1},
        {SurfaceField::Opacity, 1.0f},
    };
    for (const Default& item : Defaults) {
        if (ExportConstantEquals(context_.Builder(), out.Field(item.field), item.value)) {
            out.Field(item.field) = NoIrValue;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::material::detail
