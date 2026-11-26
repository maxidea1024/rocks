#include <Core/Text/String.h>
#include <MaterialSchema/MaterialPrerequisite.h>
#include <MaterialSchema/MaterialTraitEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// 표면 노드 lowering이다(Docs/Design/MaterialSystem 6.11절 MS2d1). 표면 노드는 표면
// 기록(SurfaceRecord)을 만들어 그 표면 값(IR SurfaceRef)을 출력하고, 출력 노드가 기록을 접어
// SurfaceAttributes로 내보낸다. 입력 기본값은 UE Compile 값이다(MaterialExpressionSubstrate.cpp).
namespace rocks::material::detail {

namespace {

// 고전 preset 기본값이다(UE SubstrateShadingModels::CompileCommon — MES:232–377).
constexpr float SurfaceDefaultSpecular = 0.5f;
constexpr float SurfaceDefaultRoughness = 0.5f;
constexpr float SurfaceDefaultClearCoat = 1.0f;
constexpr float SurfaceDefaultClearCoatRoughness = 0.1f;

// 셰이딩 모델 preset의 Renderer 14.2절 번호다(번호표에 없으면 false — PreintegratedSkin·
// FromMaterialExpression).
bool ShadingModelNumber(int64 preset, uint32& number) noexcept {
    switch (static_cast<ShadingModelPreset>(preset)) {
    case ShadingModelPreset::Unlit:
    case ShadingModelPreset::DefaultLit:
    case ShadingModelPreset::Subsurface:
    case ShadingModelPreset::ClearCoat:
    case ShadingModelPreset::SubsurfaceProfile:
    case ShadingModelPreset::TwoSidedFoliage:
    case ShadingModelPreset::Hair:
    case ShadingModelPreset::Cloth:
    case ShadingModelPreset::Eye:
    case ShadingModelPreset::SingleLayerWater:
    case ShadingModelPreset::ThinTranslucent:
        number = static_cast<uint32>(preset);
        return true;
    default:
        return false;
    }
}

StringView ShadingModelPresetName(int64 preset) noexcept {
    switch (static_cast<ShadingModelPreset>(preset)) {
    case ShadingModelPreset::PreintegratedSkin:
        return "PreintegratedSkin";
    case ShadingModelPreset::FromMaterialExpression:
        return "FromMaterialExpression";
    default:
        return "Unknown";
    }
}

// 셰이딩 모델 preset 속성을 UInt 상수로 읽는다. 번호표에 없으면 UE 이름으로 진단하고 NoIrValue다.
Result<IrValueId> ShadingModelConstant(LoweringContext& context, StringView field,
                                       bool& read) noexcept {
    read = false;
    int64 preset = static_cast<int64>(ShadingModelPreset::DefaultLit);
    auto got = context.ReadInteger(field, preset, preset);
    if (!got || !got.Value()) {
        return got ? Result<IrValueId>::Success(NoIrValue)
                   : Result<IrValueId>::Failure(got.Error());
    }

    read = true;
    uint32 number = 0;
    if (!ShadingModelNumber(preset, number)) {
        StringView name = ShadingModelPresetName(preset);
        SystemAllocator scratch;
        String message(scratch);
        const StringView parts[] = {"Shading model is not supported: ", name};
        for (StringView part : parts) {
            if (auto appended = message.TryAppend(part); !appended) {
                return Result<IrValueId>::Failure(appended.Error());
            }
        }

        if (auto reported = context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                          message.View());
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    return context.Builder().UIntConstant(Span<const uint32>(&number, 1));
}

// 입력 key가 연결되면 Float width로 맞춘 값, 아니면 fallback 상수다(UE CompileWithDefaultFloatN).
Result<IrValueId> FloatOr(LoweringContext& context, StringView key, uint8 width,
                          float fallback) noexcept {
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (connected.Value()) {
        auto input = context.Input(key);
        if (!input || input.Value() == NoIrValue) {
            return input;
        }

        return context.CoerceTo(input.Value(), IrFloat(width), key);
    }

    float values[MaxIrComponents] = {fallback, fallback, fallback, fallback};
    return context.FloatConstant(Span<const float>(values, width), key);
}

// 노멀·tangent 입력이다(UE CompileWithDefaultNormalWS·TangentWS). 연결이 없으면 정점 기저
// 값(world), 연결되면 tangent 공간 노멀이면 world로 바꿔 정규화한다.
Result<IrValueId> BasisOr(LoweringContext& context, StringView key, StringView vertex) noexcept {
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.ContextValue(vertex);
    }

    auto input = context.Input(key);
    if (!input || input.Value() == NoIrValue) {
        return input;
    }

    auto vector = context.CoerceTo(input.Value(), IrFloat(3), key);
    if (!vector || vector.Value() == NoIrValue) {
        return vector;
    }

    IrValueId world = vector.Value();
    if (context.Traits().tangentSpaceNormal) {
        const IrValueId arguments[] = {world};
        auto transformed = context.ContextValue("TransformTangentVectorToWorld", arguments);
        if (!transformed) {
            return transformed;
        }

        world = transformed.Value();
    }

    return context.Unary(IrOp::Normalize, world, NumberRule::FloatOnly);
}

// 표면 출력이다.
Result<void> EmitSurface(LoweringContext& context, const SurfaceRecord& record) noexcept {
    auto surface = context.MakeSurface(record);
    if (!surface) {
        return Result<void>::Failure(surface.Error());
    }

    return context.Output("Result", surface.Value());
}

// 값이 있는 profile 속성은 선행(MPQ-SubsurfaceProfile)이 없어 그 노드 자리의 늦은 진단이다.
Result<bool> ProfilePending(LoweringContext& context, StringView field) noexcept {
    Guid profile;
    auto read = context.ReadAssetReference(field, profile);
    if (!read) {
        return Result<bool>::Failure(read.Error());
    }

    if (!read.Value() || profile == Guid{}) {
        return Result<bool>::Success(false);
    }

    // TODO(MPQ-SubsurfaceProfile): subsurface profile 에셋이 생기면 profile id를 파라미터로 낸다.
    auto pending = context.Builder().Pending(
        IrSurfaceType(), IrPendingKind::PrerequisiteMissing,
        static_cast<uint8>(MaterialPrerequisite::SubsurfaceProfile), context.Node().id);
    if (!pending) {
        return Result<bool>::Failure(pending.Error());
    }

    if (auto written = context.Output("Result", pending.Value()); !written) {
        return Result<bool>::Failure(written.Error());
    }

    return Result<bool>::Success(true);
}

// UE ShadingModel(MaterialExpressions.cpp:21282–21286): shading model 번호 상수다.
Result<void> LowerShadingModel(LoweringContext& context) noexcept {
    bool read = false;
    auto value = ShadingModelConstant(context, "shadingModel", read);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output("Result", value.Value());
}

// UE SubstrateShadingModels(MES:183–796) — 고전 입력이 그대로 지나가는 preset이다(10.3절).
Result<void> LowerSurfaceFromShadingModel(LoweringContext& context) noexcept {
    auto profiled = ProfilePending(context, "subsurfaceProfile");
    if (!profiled || profiled.Value()) {
        return profiled ? Result<void>::Success() : Result<void>::Failure(profiled.Error());
    }

    SurfaceRecord record;
    record.kind = SurfaceKind::Classic;
    auto connected = context.IsConnected("ShadingModel");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    bool foliage = false;
    if (connected.Value()) {
        auto model = context.Input("ShadingModel");
        if (!model || model.Value() == NoIrValue) {
            return model ? Result<void>::Success() : Result<void>::Failure(model.Error());
        }

        record.Field(SurfaceField::ShadingModel) = model.Value();
    } else {
        bool read = false;
        auto model = ShadingModelConstant(context, "shadingModelOverride", read);
        if (!model || model.Value() == NoIrValue) {
            return model ? Result<void>::Success() : Result<void>::Failure(model.Error());
        }

        record.Field(SurfaceField::ShadingModel) = model.Value();
        const IrValue& constant = context.Builder().Value(model.Value());
        foliage =
            constant.immediate[0] == static_cast<uint64>(rocks::ShadingModel::TwoSidedFoliage);
    }

    struct Input {
        SurfaceField field;
        StringView key;
        uint8 width;
        float fallback;
    };

    // SubSurfaceColor 기본값은 TwoSidedFoliage면 0이다(UE MADM:50–55).
    const Input inputs[] = {
        {SurfaceField::BaseColor, "BaseColor", 3, 0.0f},
        {SurfaceField::Metallic, "Metallic", 1, 0.0f},
        {SurfaceField::Specular, "Specular", 1, SurfaceDefaultSpecular},
        {SurfaceField::Roughness, "Roughness", 1, SurfaceDefaultRoughness},
        {SurfaceField::Anisotropy, "Anisotropy", 1, 0.0f},
        {SurfaceField::Emissive, "EmissiveColor", 3, 0.0f},
        {SurfaceField::SubsurfaceColor, "SubSurfaceColor", 3, foliage ? 0.0f : 1.0f},
        {SurfaceField::CustomData0, "ClearCoat", 1, SurfaceDefaultClearCoat},
        {SurfaceField::CustomData1, "ClearCoatRoughness", 1, SurfaceDefaultClearCoatRoughness},
        {SurfaceField::Opacity, "Opacity", 1, 1.0f},
    };
    for (const Input& input : inputs) {
        auto value = FloatOr(context, input.key, input.width, input.fallback);
        if (!value || value.Value() == NoIrValue) {
            return value ? Result<void>::Success() : Result<void>::Failure(value.Error());
        }

        record.Field(input.field) = value.Value();
    }

    auto normal = BasisOr(context, "Normal", "VertexNormal");
    auto tangent = BasisOr(context, "Tangent", "VertexTangent");
    if (!normal || !tangent) {
        return Result<void>::Failure(!normal ? normal.Error() : tangent.Error());
    }

    if (normal.Value() == NoIrValue || tangent.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    record.Field(SurfaceField::Normal) = normal.Value();
    record.Field(SurfaceField::Tangent) = tangent.Value();
    return EmitSurface(context, record);
}

// Unlit 표면이다(UE Unlit BSDF·영역 표면 — MES 1.6–1.10). colorKey는 emissive, opacityKey는
// opacity다.
Result<void> LowerUnlitSurface(LoweringContext& context, SurfaceUsage usage, StringView colorKey,
                               StringView opacityKey, StringView normalKey) noexcept {
    SurfaceRecord record;
    record.kind = SurfaceKind::Unlit;
    record.usage = usage;
    const uint32 unlit = static_cast<uint32>(rocks::ShadingModel::Unlit);
    auto model = context.Builder().UIntConstant(Span<const uint32>(&unlit, 1));
    if (!model) {
        return Result<void>::Failure(model.Error());
    }

    record.Field(SurfaceField::ShadingModel) = model.Value();
    auto color = FloatOr(context, colorKey, 3, 0.0f);
    if (!color || color.Value() == NoIrValue) {
        return color ? Result<void>::Success() : Result<void>::Failure(color.Error());
    }

    record.Field(SurfaceField::Emissive) = color.Value();
    if (!opacityKey.IsEmpty()) {
        auto opacity = FloatOr(context, opacityKey, 1, 1.0f);
        if (!opacity || opacity.Value() == NoIrValue) {
            return opacity ? Result<void>::Success() : Result<void>::Failure(opacity.Error());
        }

        record.Field(SurfaceField::Opacity) = opacity.Value();
        // UE SubstrateCreateUIMaterial: Additive는 emissive × opacity다(SLC:677–716).
        if (usage == SurfaceUsage::UserInterface &&
            context.Traits().blendMode == BlendMode::Additive) {
            auto scaled = context.Binary(IrOp::Multiply, color.Value(), opacity.Value(),
                                         NumberRule::FloatOnly);
            if (!scaled) {
                return Result<void>::Failure(scaled.Error());
            }

            record.Field(SurfaceField::Emissive) = scaled.Value();
        }
    }

    if (!normalKey.IsEmpty()) {
        auto normal = BasisOr(context, normalKey, "VertexNormal");
        if (!normal || normal.Value() == NoIrValue) {
            return normal ? Result<void>::Success() : Result<void>::Failure(normal.Error());
        }

        record.Field(SurfaceField::Normal) = normal.Value();
    }

    return EmitSurface(context, record);
}

// UE SubstrateUnlitBSDF(MES:2039–2116). 색 투과(TransmittanceColor)는 dual-source 블렌드가 필요해
// TODO다.
Result<void> LowerSurfaceUnlit(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("TransmittanceColor");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (connected.Value()) {
        // TODO(MPQ-ColoredTransmittance): 색 투과 반투명 경로가 생기면 transmittance 칸을 낸다.
        auto pending = context.Builder().Pending(
            IrSurfaceType(), IrPendingKind::PrerequisiteMissing,
            static_cast<uint8>(MaterialPrerequisite::ColoredTransmittance), context.Node().id);
        if (!pending) {
            return Result<void>::Failure(pending.Error());
        }

        return context.Output("Result", pending.Value());
    }

    return LowerUnlitSurface(context, SurfaceUsage::Surface, "EmissiveColor", StringView(),
                             "Normal");
}

Result<void> LowerSurfaceLightFunction(LoweringContext& context) noexcept {
    return LowerUnlitSurface(context, SurfaceUsage::LightFunction, "Color", StringView(),
                             StringView());
}

Result<void> LowerSurfacePostProcess(LoweringContext& context) noexcept {
    return LowerUnlitSurface(context, SurfaceUsage::PostProcess, "Color", "Opacity", StringView());
}

Result<void> LowerSurfaceUserInterface(LoweringContext& context) noexcept {
    return LowerUnlitSurface(context, SurfaceUsage::UserInterface, "Color", "Opacity",
                             StringView());
}

// UE SubstrateVolumetricFogCloudBSDF(MES:1517–1631): Volume 영역 칸(albedo → baseColor, extinction
// → subsurfaceColor — UE 고전 Volume 칸).
Result<void> LowerSurfaceVolumetricFogCloud(LoweringContext& context) noexcept {
    bool emissiveOnly = false;
    auto read = context.ReadBool("emissiveOnly", false, emissiveOnly);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    SurfaceRecord record;
    record.kind = SurfaceKind::VolumetricFogCloud;
    record.usage = SurfaceUsage::Volume;
    const uint32 number = static_cast<uint32>(emissiveOnly ? rocks::ShadingModel::Unlit
                                                           : rocks::ShadingModel::DefaultLit);
    auto model = context.Builder().UIntConstant(Span<const uint32>(&number, 1));
    auto emissive = FloatOr(context, "EmissiveColor", 3, 0.0f);
    if (!model || !emissive) {
        return Result<void>::Failure(!model ? model.Error() : emissive.Error());
    }

    if (emissive.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    record.Field(SurfaceField::ShadingModel) = model.Value();
    record.Field(SurfaceField::Emissive) = emissive.Value();
    if (!emissiveOnly) {
        struct Input {
            SurfaceField field;
            StringView key;
            uint8 width;
            float fallback;
        };

        const Input inputs[] = {
            {SurfaceField::BaseColor, "Albedo", 3, 0.0f},
            {SurfaceField::SubsurfaceColor, "Extinction", 3, 0.0f},
            {SurfaceField::AmbientOcclusion, "AmbientOcclusion", 1, 1.0f},
        };
        for (const Input& input : inputs) {
            auto value = FloatOr(context, input.key, input.width, input.fallback);
            if (!value || value.Value() == NoIrValue) {
                return value ? Result<void>::Success() : Result<void>::Failure(value.Error());
            }

            record.Field(input.field) = value.Value();
        }
    }

    return EmitSurface(context, record);
}

// UE SubstrateConvertToDecal(MES:1914–2035): 자식은 slab(고전 preset 포함)만이고 coverage가
// opacity가 된다.
Result<void> LowerSurfaceToDecal(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("DecalMaterial");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "DecalMaterial",
                             "Missing DecalMaterial input");
    }

    auto child = context.Input("DecalMaterial");
    if (!child || child.Value() == NoIrValue) {
        return child ? Result<void>::Success() : Result<void>::Failure(child.Error());
    }

    const IrValue& childValue = context.Builder().Value(child.Value());
    if (childValue.op == IrOp::Pending) {
        return context.Output("Result", child.Value());
    }

    const SurfaceRecord* inner = context.SurfaceOf(child.Value());
    if (inner == nullptr || inner->kind != SurfaceKind::Classic) {
        return context.Error(MaterialDiagnosticCode::UnsupportedCapability, "DecalMaterial",
                             "Substrate Convert To Decal node must receive SubstrateData a "
                             "parameter blended Substrate material sub tree.");
    }

    auto coverage = FloatOr(context, "Coverage", 1, 1.0f);
    if (!coverage || coverage.Value() == NoIrValue) {
        return coverage ? Result<void>::Success() : Result<void>::Failure(coverage.Error());
    }

    SurfaceRecord record;
    record.kind = SurfaceKind::Decal;
    record.usage = SurfaceUsage::Decal;
    record.child = child.Value();
    record.coverage = coverage.Value();
    return EmitSurface(context, record);
}

constexpr LoweringEntry SurfaceEntries[] = {
    {"Material.ShadingModel", &LowerShadingModel},
    {"Material.SurfaceFromShadingModel", &LowerSurfaceFromShadingModel},
    {"Material.SurfaceUnlit", &LowerSurfaceUnlit},
    {"Material.SurfaceLightFunction", &LowerSurfaceLightFunction},
    {"Material.SurfacePostProcess", &LowerSurfacePostProcess},
    {"Material.SurfaceUserInterface", &LowerSurfaceUserInterface},
    {"Material.SurfaceVolumetricFogCloud", &LowerSurfaceVolumetricFogCloud},
    {"Material.SurfaceToDecal", &LowerSurfaceToDecal},
};

} // namespace

Span<const LoweringEntry> SurfaceLowerings() noexcept {
    return SurfaceEntries;
}

} // namespace rocks::material::detail
