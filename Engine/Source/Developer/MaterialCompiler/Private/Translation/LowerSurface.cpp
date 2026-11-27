#include <Core/Text/String.h>
#include <MaterialSchema/MaterialPrerequisite.h>
#include <MaterialSchema/MaterialTraitEnums.h>
#include <MaterialSchema/Nodes/Surface/SurfaceNodeEnums.h>

#include "LoweringContext.h"
#include "ClassicSurface.h"
#include "LoweringTable.h"
#include "SurfaceShadingModels.h"
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
// Slab 기본값이다(UE SubstrateSlabBSDF::Compile — MES:882–896). F0는 DielectricSpecularToF0(0.5)다.
constexpr float SurfaceDefaultDiffuseAlbedo = 0.18f;
constexpr float SurfaceDefaultF0 = 0.04f;
// Eye의 F0(UE SUBSTRATE_EYE_DEFAULT_F0 — S:170)를 고전 specular로 바꾼 값(0.028/0.08)이다.
constexpr float SurfaceEyeSpecular = 0.028f / 0.08f;
constexpr bool SurfaceFalse = false;
constexpr bool SurfaceTrue = true;

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

        return context.CoerceToWidth(input.Value(), width, key);
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

    auto vector = context.CoerceToWidth(input.Value(), 3, key);
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

// static shading model 하나인 고전 preset의 흐르는 종류다(UE MES:632–773).
uint16 ClassicFlow(rocks::ShadingModel model) noexcept {
    switch (model) {
    case rocks::ShadingModel::Unlit:
        return SurfaceFlowBit(SurfaceFlow::Unlit);
    case rocks::ShadingModel::Hair:
        return SurfaceFlowBit(SurfaceFlow::Hair);
    case rocks::ShadingModel::Eye:
        return SurfaceFlowBit(SurfaceFlow::Eye);
    case rocks::ShadingModel::SingleLayerWater:
        return SurfaceFlowBit(SurfaceFlow::SingleLayerWater);
    default:
        return SurfaceFlowBit(SurfaceFlow::Slab);
    }
}

// 영역 쓰임의 흐르는 종류다.
uint16 UsageFlow(SurfaceUsage usage) noexcept {
    switch (usage) {
    case SurfaceUsage::LightFunction:
        return SurfaceFlowBit(SurfaceFlow::LightFunction);
    case SurfaceUsage::PostProcess:
        return SurfaceFlowBit(SurfaceFlow::PostProcess);
    case SurfaceUsage::UserInterface:
        return SurfaceFlowBit(SurfaceFlow::UserInterface);
    case SurfaceUsage::Decal:
        return SurfaceFlowBit(SurfaceFlow::Decal);
    case SurfaceUsage::Volume:
        return SurfaceFlowBit(SurfaceFlow::VolumetricFogCloud);
    case SurfaceUsage::Surface:
        break;
    }

    return SurfaceFlowBit(SurfaceFlow::Unlit);
}

// 표면 출력이다.
Result<void> EmitSurface(LoweringContext& context, const SurfaceRecord& record) noexcept {
    auto surface = context.MakeSurface(record);
    if (!surface) {
        return Result<void>::Failure(surface.Error());
    }

    return context.Output("Result", surface.Value());
}

// 값이 있는 profile 속성은 선행(MPQ-SubsurfaceProfile·MPQ-SpecularProfile)이 없어 그 노드 자리의
// 늦은 진단이다.
Result<bool> ProfilePending(LoweringContext& context, StringView field,
                            MaterialPrerequisite prerequisite) noexcept {
    Guid profile;
    auto read = context.ReadAssetReference(field, profile);
    if (!read) {
        return Result<bool>::Failure(read.Error());
    }

    if (!read.Value() || profile == Guid{}) {
        return Result<bool>::Success(false);
    }

    // TODO(MPQ-SubsurfaceProfile): subsurface profile 에셋이 생기면 profile id를 파라미터로 낸다.
    // TODO(MPQ-SpecularProfile): specular profile 에셋이 생기면 F0·F90에 굽는다(UE
    // SubstrateBakeSpecularProfileToReflectivity).
    auto pending = context.Builder().Pending(IrSurfaceType(), IrPendingKind::PrerequisiteMissing,
                                             static_cast<uint8>(prerequisite), context.Node().id);
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

// 고전 입력의 원천이다(6.12절 MS2e1 — 핀 연결 또는 속성 칸). attributes면 entries의 칸이 입력이고
// 없는 칸은 연결 없음과 같다. waterPins면 물 계수를 노드 핀에서 읽고, 아니면 기본값이다.
struct ClassicSource {
    LoweringContext* context = nullptr;
    Span<const AttributeEntry> entries;
    bool attributes = false;
    bool waterPins = true;
};

// 고전 입력 키 → 속성 이름이다(UE SubstrateConvertMaterialAttributes::CompileCommon —
// MaterialExpressionSubstrate.cpp:3957–4156, custom 속성 4개 포함).
struct ClassicAttributeName {
    StringView key;
    StringView attribute;
};

constexpr ClassicAttributeName ClassicAttributeNames[] = {
    {"BaseColor", "BaseColor"},
    {"Metallic", "Metallic"},
    {"Specular", "Specular"},
    {"Roughness", "Roughness"},
    {"Anisotropy", "Anisotropy"},
    {"EmissiveColor", "EmissiveColor"},
    {"SubSurfaceColor", "SubsurfaceColor"},
    {"ClearCoat", "CustomData0"},
    {"ClearCoatRoughness", "CustomData1"},
    {"Opacity", "Opacity"},
    {"Normal", "Normal"},
    {"Tangent", "Tangent"},
    {"ShadingModel", "ShadingModel"},
    {"TransmittanceColor", "TransmittanceColor"},
    {"ThinTranslucentSurfaceCoverage", "ThinTranslucentSurfaceCoverage"},
    {"ClearCoatNormal", "ClearCoatBottomNormal"},
    {"CustomTangent", "CustomEyeTangent"},
};

// key 입력이 있는가(핀 연결 또는 칸)와 그 값이다(없으면 present 거짓, 값 NoIrValue — 위쪽 오류도
// NoIrValue이고 present 참).
Result<IrValueId> ClassicRead(ClassicSource& source, StringView key, bool& present) noexcept {
    LoweringContext& context = *source.context;
    present = false;
    if (!source.attributes) {
        auto connected = context.IsConnected(key);
        if (!connected) {
            return Result<IrValueId>::Failure(connected.Error());
        }

        if (!connected.Value()) {
            return Result<IrValueId>::Success(NoIrValue);
        }

        present = true;
        return context.Input(key);
    }

    for (const ClassicAttributeName& item : ClassicAttributeNames) {
        if (item.key != key) {
            continue;
        }

        const MaterialAttributeDefinition* definition = context.FindAttributeByName(item.attribute);
        if (definition == nullptr) {
            return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
        }

        for (const AttributeEntry& entry : source.entries) {
            if (entry.id == definition->id) {
                present = true;
                return Result<IrValueId>::Success(entry.value);
            }
        }
    }

    return Result<IrValueId>::Success(NoIrValue);
}

// 있으면 Float width로 맞춘 값, 없으면 fallback 상수다(UE CompileWithDefaultFloatN).
Result<IrValueId> ClassicFloat(ClassicSource& source, StringView key, uint8 width,
                               float fallback) noexcept {
    LoweringContext& context = *source.context;
    bool present = false;
    auto value = ClassicRead(source, key, present);
    if (!value) {
        return value;
    }

    if (present) {
        if (value.Value() == NoIrValue) {
            return value;
        }

        return context.CoerceToWidth(value.Value(), width, key);
    }

    float values[MaxIrComponents] = {fallback, fallback, fallback, fallback};
    return context.FloatConstant(Span<const float>(values, width), key);
}

// 노멀·tangent 입력이다(BasisOr와 같다 — 있으면 tangent 공간이면 world로 바꿔 정규화, 없으면 정점
// 기저). allowed가 거짓이면 없음으로 본다(속성 Tangent는 Anisotropy 칸이 있을 때만 — UE
// :3993–4003).
Result<IrValueId> ClassicBasis(ClassicSource& source, StringView key, StringView vertex,
                               bool allowed) noexcept {
    LoweringContext& context = *source.context;
    bool present = false;
    auto value = ClassicRead(source, key, present);
    if (!value) {
        return value;
    }

    if (!present || !allowed) {
        return context.ContextValue(vertex);
    }

    if (value.Value() == NoIrValue) {
        return value;
    }

    auto vector = context.CoerceToWidth(value.Value(), 3, key);
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

// 고전 preset 기록을 만든다(SurfaceFromShadingModel·SurfaceFromAttributes·출력 노드 속성 집합 변환
// — 6.11절 MS2d1, 6.12절 MS2e1). overrideField가 비어 있으면 shading model 기본은 DefaultLit다.
// 위쪽 오류면 false다.
Result<bool> BuildClassicRecord(ClassicSource& source, StringView overrideField,
                                SurfaceRecord& record) noexcept {
    LoweringContext& context = *source.context;
    record = SurfaceRecord();
    record.kind = SurfaceKind::Classic;
    bool present = false;
    auto modelInput = ClassicRead(source, "ShadingModel", present);
    if (!modelInput) {
        return Result<bool>::Failure(modelInput.Error());
    }

    bool foliage = false;
    if (present) {
        if (modelInput.Value() == NoIrValue) {
            return Result<bool>::Success(false);
        }

        record.Field(SurfaceField::ShadingModel) = modelInput.Value();
        // 픽셀마다 고르는 shading model은 slab 최악 경우이고(UE MES:632–773), 닿는 후보의 전용
        // BSDF(Hair·Eye·SingleLayerWater)는 연산 아래에서 막는다(6.11절 MS2d2).
        auto reached = ReachableShadingModels(context.Builder(), modelInput.Value());
        if (!reached) {
            return Result<bool>::Failure(reached.Error());
        }

        record.flow = SurfaceFlowBit(SurfaceFlow::Slab);
        // 컴파일된 집합에 TwoSidedFoliage가 있으면 SubSurfaceColor 기본값은 0이다(UE MADM:50–55).
        foliage = (reached.Value() &
                   (1u << static_cast<uint32>(rocks::ShadingModel::TwoSidedFoliage))) != 0;
        const rocks::ShadingModel special[] = {rocks::ShadingModel::Hair, rocks::ShadingModel::Eye,
                                               rocks::ShadingModel::SingleLayerWater};
        for (rocks::ShadingModel candidate : special) {
            if ((reached.Value() & (1u << static_cast<uint32>(candidate))) != 0) {
                record.flow |= ClassicFlow(candidate);
            }
        }
    } else {
        Result<IrValueId> model = Result<IrValueId>::Success(NoIrValue);
        if (!overrideField.IsEmpty()) {
            bool read = false;
            model = ShadingModelConstant(context, overrideField, read);
        } else {
            const uint32 number = static_cast<uint32>(rocks::ShadingModel::DefaultLit);
            model = context.Builder().UIntConstant(Span<const uint32>(&number, 1));
        }

        if (!model || model.Value() == NoIrValue) {
            return model ? Result<bool>::Success(false) : Result<bool>::Failure(model.Error());
        }

        record.Field(SurfaceField::ShadingModel) = model.Value();
        const IrValue& constant = context.Builder().Value(model.Value());
        foliage =
            constant.immediate[0] == static_cast<uint64>(rocks::ShadingModel::TwoSidedFoliage);
        record.flow = ClassicFlow(static_cast<rocks::ShadingModel>(constant.immediate[0]));
    }

    if (record.flow == 0) {
        // 픽셀마다 고르는 shading model은 slab 최악 경우다(UE MES:632–773 — 후보 검사는 연산이
        // 한다, 6.11절 MS2d2).
        record.flow = SurfaceFlowBit(SurfaceFlow::Slab);
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
        auto value = ClassicFloat(source, input.key, input.width, input.fallback);
        if (!value || value.Value() == NoIrValue) {
            return value ? Result<bool>::Success(false) : Result<bool>::Failure(value.Error());
        }

        record.Field(input.field) = value.Value();
    }

    // 속성 집합의 Tangent는 Anisotropy 칸이 있을 때만 읽는다(UE :3993–4003).
    bool anisotropic = true;
    if (source.attributes) {
        bool hasAnisotropy = false;
        auto anisotropy = ClassicRead(source, "Anisotropy", hasAnisotropy);
        if (!anisotropy) {
            return Result<bool>::Failure(anisotropy.Error());
        }

        anisotropic = hasAnisotropy;
    }

    auto normal = ClassicBasis(source, "Normal", "VertexNormal", true);
    auto tangent = ClassicBasis(source, "Tangent", "VertexTangent", anisotropic);
    if (!normal || !tangent) {
        return Result<bool>::Failure(!normal ? normal.Error() : tangent.Error());
    }

    if (normal.Value() == NoIrValue || tangent.Value() == NoIrValue) {
        return Result<bool>::Success(false);
    }

    record.Field(SurfaceField::Normal) = normal.Value();
    record.Field(SurfaceField::Tangent) = tangent.Value();
    // 얇은 투과·clear coat 아래 노멀이다(UE CompileCommon 기본 0.5·1 — 연산 아래 slab 변환과 MS2d3
    // 출력이 쓴다).
    auto transmittance = ClassicFloat(source, "TransmittanceColor", 3, 0.5f);
    auto thinCoverage = ClassicFloat(source, "ThinTranslucentSurfaceCoverage", 1, 1.0f);
    if (!transmittance || !thinCoverage) {
        return Result<bool>::Failure(!transmittance ? transmittance.Error() : thinCoverage.Error());
    }

    if (transmittance.Value() == NoIrValue || thinCoverage.Value() == NoIrValue) {
        return Result<bool>::Success(false);
    }

    record.thinTransmittance = transmittance.Value();
    record.thinCoverage = thinCoverage.Value();
    // clear coat 아래 노멀·custom tangent는 있을 때만 입력 원값이다(MS2d3 출력 — UE 고전 경로).
    const StringView basisKeys[] = {"ClearCoatNormal", "CustomTangent"};
    IrValueId* basisOut[] = {&record.secondNormal, &record.customTangent};
    for (usize index = 0; index < 2; ++index) {
        bool has = false;
        auto input = ClassicRead(source, basisKeys[index], has);
        if (!input) {
            return Result<bool>::Failure(input.Error());
        }

        if (!has) {
            continue;
        }

        if (input.Value() == NoIrValue) {
            return Result<bool>::Success(false);
        }

        auto vector = context.CoerceToWidth(input.Value(), 3, basisKeys[index]);
        if (!vector || vector.Value() == NoIrValue) {
            return vector ? Result<bool>::Success(false) : Result<bool>::Failure(vector.Error());
        }

        *basisOut[index] = vector.Value();
    }

    // 물 계수(UE CompileCommon 기본 0·0·0·1)다. 속성 집합 변환도 노드 핀에서 받는다.
    struct WaterInput {
        StringView key;
        uint8 width;
        float fallback;
    };

    const WaterInput waters[] = {{"WaterScatteringCoefficients", 3, 0.0f},
                                 {"WaterAbsorptionCoefficients", 3, 0.0f},
                                 {"WaterPhaseG", 1, 0.0f},
                                 {"ColorScaleBehindWater", 3, 1.0f}};
    for (usize index = 0; index < 4; ++index) {
        Result<IrValueId> value = Result<IrValueId>::Success(NoIrValue);
        if (source.waterPins) {
            value =
                FloatOr(context, waters[index].key, waters[index].width, waters[index].fallback);
        } else {
            float values[MaxIrComponents] = {waters[index].fallback, waters[index].fallback,
                                             waters[index].fallback, waters[index].fallback};
            value =
                context.FloatConstant(Span<const float>(values, waters[index].width), StringView());
        }

        if (!value || value.Value() == NoIrValue) {
            return value ? Result<bool>::Success(false) : Result<bool>::Failure(value.Error());
        }

        record.water[index] = value.Value();
    }

    return Result<bool>::Success(true);
}

// UE SubstrateShadingModels(MES:183–796) — 고전 입력이 그대로 지나가는 preset이다(10.3절).
Result<void> LowerSurfaceFromShadingModel(LoweringContext& context) noexcept {
    auto profiled =
        ProfilePending(context, "subsurfaceProfile", MaterialPrerequisite::SubsurfaceProfile);
    if (!profiled || profiled.Value()) {
        return profiled ? Result<void>::Success() : Result<void>::Failure(profiled.Error());
    }

    ClassicSource source;
    source.context = &context;
    SurfaceRecord record;
    auto built = BuildClassicRecord(source, "shadingModelOverride", record);
    if (!built || !built.Value()) {
        return built ? Result<void>::Success() : Result<void>::Failure(built.Error());
    }

    return EmitSurface(context, record);
}

// UE SubstrateConvertMaterialAttributes(MaterialExpressionSubstrate.cpp:3923–4470) — 속성 칸이 고전
// 입력이다(6.12절 MS2e1). 속성 입력이 없으면 모든 칸이 기본이다(UE CompileDefaultSlab).
Result<void> LowerSurfaceFromAttributes(LoweringContext& context) noexcept {
    auto profiled =
        ProfilePending(context, "subsurfaceProfile", MaterialPrerequisite::SubsurfaceProfile);
    if (!profiled || profiled.Value()) {
        return profiled ? Result<void>::Success() : Result<void>::Failure(profiled.Error());
    }

    SystemAllocator scratch;
    Array<AttributeEntry> entries(scratch);
    auto connected = context.IsConnected("MaterialAttributes");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (connected.Value()) {
        auto value = context.Input("MaterialAttributes");
        if (!value || value.Value() == NoIrValue) {
            return value ? Result<void>::Success() : Result<void>::Failure(value.Error());
        }

        auto copied = context.CopyAttributes(value.Value(), entries);
        if (!copied) {
            return Result<void>::Failure(copied.Error());
        }

        if (!copied.Value()) {
            return context.Error(
                MaterialDiagnosticCode::TypeMismatch, "MaterialAttributes",
                "Cannot connect a non MaterialAttributes node to a MaterialAttributes pin.");
        }
    }

    ClassicSource source;
    source.context = &context;
    source.entries = entries.View();
    source.attributes = true;
    SurfaceRecord record;
    auto built = BuildClassicRecord(source, "shadingModelOverride", record);
    if (!built || !built.Value()) {
        return built ? Result<void>::Success() : Result<void>::Failure(built.Error());
    }

    return EmitSurface(context, record);
}

// Unlit 표면이다(UE Unlit BSDF·영역 표면 — MES 1.6–1.10). colorKey는 emissive, opacityKey는
// opacity다.
Result<void> LowerUnlitSurface(LoweringContext& context, SurfaceUsage usage, StringView colorKey,
                               StringView opacityKey, StringView normalKey) noexcept {
    SurfaceRecord record;
    record.kind = SurfaceKind::Unlit;
    record.usage = usage;
    record.flow = UsageFlow(usage);
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
    record.flow = UsageFlow(SurfaceUsage::Volume);
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

        auto occlusion = context.IsConnected("AmbientOcclusion");
        if (!occlusion) {
            return Result<void>::Failure(occlusion.Error());
        }

        record.writesOcclusion = occlusion.Value();
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
    if (inner != nullptr && (inner->flow & SurfaceFlowBit(SurfaceFlow::Decal)) != 0) {
        return context.Error(MaterialDiagnosticCode::UnsupportedCapability, "DecalMaterial",
                             "Substrate Convert To Decal node must be the root of a parameter "
                             "blending sub tree: no more Substrate operations can be applied a "
                             "over its output.");
    }

    if (inner == nullptr || inner->flow != SurfaceFlowBit(SurfaceFlow::Slab)) {
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
    record.flow = UsageFlow(SurfaceUsage::Decal);
    record.children[0] = child.Value();
    record.operand = coverage.Value();
    return EmitSurface(context, record);
}

// 입력 묶음 하나를 FloatOr로 읽는다. 위쪽 오류면 false다.
struct SurfaceInput {
    StringView key;
    uint8 width;
    float fallback;
    IrValueId* out;
};

Result<bool> ReadSurfaceInputs(LoweringContext& context, Span<const SurfaceInput> inputs) noexcept {
    for (const SurfaceInput& input : inputs) {
        auto value = FloatOr(context, input.key, input.width, input.fallback);
        if (!value) {
            return Result<bool>::Failure(value.Error());
        }

        if (value.Value() == NoIrValue) {
            return Result<bool>::Success(false);
        }

        *input.out = value.Value();
    }

    return Result<bool>::Success(true);
}

// 기저 입력 둘(노멀·tangent)을 읽는다.
Result<bool> ReadBasis(LoweringContext& context, StringView normalKey, StringView normalDefault,
                       IrValueId& normal, StringView tangentKey, IrValueId& tangent) noexcept {
    auto n = BasisOr(context, normalKey, normalDefault);
    if (!n) {
        return Result<bool>::Failure(n.Error());
    }

    if (n.Value() == NoIrValue) {
        return Result<bool>::Success(false);
    }

    normal = n.Value();
    if (tangentKey.IsEmpty()) {
        return Result<bool>::Success(true);
    }

    auto t = BasisOr(context, tangentKey, "VertexTangent");
    if (!t) {
        return Result<bool>::Failure(t.Error());
    }

    if (t.Value() == NoIrValue) {
        return Result<bool>::Success(false);
    }

    tangent = t.Value();
    return Result<bool>::Success(true);
}

// 연결된 때만 기저 값(secondNormal)이다.
Result<bool> ReadSecondNormal(LoweringContext& context, StringView key, IrValueId& out) noexcept {
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<bool>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return Result<bool>::Success(true);
    }

    // custom 출력은 입력 원값이다(머티리얼 노멀 공간 — 6.11절 MS2d3).
    auto normal = context.Input(key);
    if (!normal || normal.Value() == NoIrValue) {
        return normal ? Result<bool>::Success(false) : Result<bool>::Failure(normal.Error());
    }

    auto vector = context.CoerceToWidth(normal.Value(), 3, key);
    if (!vector) {
        return Result<bool>::Failure(vector.Error());
    }

    out = vector.Value();
    return Result<bool>::Success(vector.Value() != NoIrValue);
}

// shading model 상수 칸이다.
Result<IrValueId> ModelConstant(LoweringContext& context, rocks::ShadingModel model) noexcept {
    const uint32 number = static_cast<uint32>(model);
    return context.Builder().UIntConstant(Span<const uint32>(&number, 1));
}

// UE SubstrateSlabBSDF(MES:815–1314) — 입력에 UE 단순화를 적용한 slab 기록이다(접을 때 두께·층
// 위치로 slab이 된다, 6.11절 MS2d2).
Result<void> LowerSurfaceSlab(LoweringContext& context) noexcept {
    auto profiled =
        ProfilePending(context, "subsurfaceProfile", MaterialPrerequisite::SubsurfaceProfile);
    if (!profiled || profiled.Value()) {
        return profiled ? Result<void>::Success() : Result<void>::Failure(profiled.Error());
    }

    auto specular =
        ProfilePending(context, "specularProfile", MaterialPrerequisite::SpecularProfile);
    if (!specular || specular.Value()) {
        return specular ? Result<void>::Success() : Result<void>::Failure(specular.Error());
    }

    int64 type = static_cast<int64>(SubsurfaceType::Diffusion);
    auto typeRead = context.ReadInteger("subSurfaceType", type, type);
    if (!typeRead || !typeRead.Value()) {
        return typeRead ? Result<void>::Success() : Result<void>::Failure(typeRead.Error());
    }

    SurfaceRecord record;
    record.kind = SurfaceKind::Slab;
    record.flow = SurfaceFlowBit(SurfaceFlow::Slab);
    SurfaceSlab& slab = record.slab;
    const SurfaceInput inputs[] = {
        {"DiffuseAlbedo", 3, SurfaceDefaultDiffuseAlbedo, &slab[SlabValue::DiffuseAlbedo]},
        {"F0", 3, SurfaceDefaultF0, &slab[SlabValue::F0]},
        {"F90", 3, 1.0f, &slab[SlabValue::F90]},
        {"Roughness", 1, SurfaceDefaultRoughness, &slab[SlabValue::Roughness]},
        {"Anisotropy", 1, 0.0f, &slab[SlabValue::Anisotropy]},
        {"SSSMFPScale", 1, 1.0f, &slab[SlabValue::RadiusScale]},
        {"SSSPhaseAnisotropy", 1, 0.0f, &slab[SlabValue::PhaseAnisotropy]},
        {"EmissiveColor", 3, 0.0f, &slab[SlabValue::Emissive]},
        {"SecondRoughness", 1, 0.0f, &slab[SlabValue::HazeRoughness]},
        {"SecondRoughnessWeight", 1, 0.0f, &slab[SlabValue::HazeWeight]},
        {"FuzzAmount", 1, 0.0f, &slab[SlabValue::FuzzAmount]},
        {"FuzzColor", 3, 0.0f, &slab[SlabValue::FuzzColor]},
    };
    auto read = ReadSurfaceInputs(context, inputs);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    // MFP는 subSurfaceType이 None이면 0이다(MES:898–926). 연결되지 않았으면 SSS가 없다.
    auto mfpConnected = context.IsConnected("SSSMFP");
    if (!mfpConnected) {
        return Result<void>::Failure(mfpConnected.Error());
    }

    const bool sss = mfpConnected.Value() && type != static_cast<int64>(SubsurfaceType::None);
    auto mfp = FloatOr(context, "SSSMFP", 3, 0.0f);
    if (mfp && !sss) {
        const float zeros[] = {0.0f, 0.0f, 0.0f};
        mfp = context.FloatConstant(Span<const float>(zeros, 3), StringView());
    }

    if (!mfp || mfp.Value() == NoIrValue) {
        return mfp ? Result<void>::Success() : Result<void>::Failure(mfp.Error());
    }

    slab[SlabValue::MeanFreePath] = mfp.Value();
    record.subsurfaceType = sss ? static_cast<uint32>(type) : 0u;
    // FuzzRoughness는 연결이 없으면 Roughness다(MES:893).
    auto fuzzConnected = context.IsConnected("FuzzRoughness");
    if (!fuzzConnected) {
        return Result<void>::Failure(fuzzConnected.Error());
    }

    slab[SlabValue::FuzzRoughness] = slab[SlabValue::Roughness];
    if (fuzzConnected.Value()) {
        auto fuzz = FloatOr(context, "FuzzRoughness", 1, SurfaceDefaultRoughness);
        if (!fuzz || fuzz.Value() == NoIrValue) {
            return fuzz ? Result<void>::Success() : Result<void>::Failure(fuzz.Error());
        }

        slab[SlabValue::FuzzRoughness] = fuzz.Value();
    }

    auto basis = ReadBasis(context, "Normal", "VertexNormal", slab[SlabValue::Normal], "Tangent",
                           slab[SlabValue::Tangent]);
    if (!basis || !basis.Value()) {
        return basis ? Result<void>::Success() : Result<void>::Failure(basis.Error());
    }

    // glint는 Rocks 렌더러에 없으므로 UE 미지원 경로처럼 F0·F90에 굽는다(S:1758–1768).
    auto glintConnected = context.IsConnected("GlintValue");
    if (!glintConnected) {
        return Result<void>::Failure(glintConnected.Error());
    }

    if (glintConnected.Value()) {
        auto glint = FloatOr(context, "GlintValue", 1, 1.0f);
        if (!glint || glint.Value() == NoIrValue) {
            return glint ? Result<void>::Success() : Result<void>::Failure(glint.Error());
        }

        auto coverage = context.Unary(IrOp::Saturate, glint.Value(), NumberRule::FloatOnly);
        if (!coverage) {
            return Result<void>::Failure(coverage.Error());
        }

        const SlabValue baked[] = {SlabValue::F0, SlabValue::F90};
        for (SlabValue value : baked) {
            auto scaled = context.Binary(IrOp::Multiply, slab[value], coverage.Value(),
                                         NumberRule::FloatOnly);
            if (!scaled) {
                return Result<void>::Failure(scaled.Error());
            }

            slab[value] = scaled.Value();
        }
    }

    auto simple = context.Builder().BoolConstant(Span<const bool>(&SurfaceFalse, 1));
    if (!simple) {
        return Result<void>::Failure(simple.Error());
    }

    slab[SlabValue::SimpleClearCoat] = simple.Value();
    return EmitSurface(context, record);
}

// UE SubstrateSimpleClearCoatBSDF(MES:1318–1513) — HazeRoughness·HazeWeight가 clear coat인
// slab이다.
Result<void> LowerSurfaceSimpleClearCoat(LoweringContext& context) noexcept {
    SurfaceRecord record;
    record.kind = SurfaceKind::Slab;
    record.flow = SurfaceFlowBit(SurfaceFlow::Slab);
    SurfaceSlab& slab = record.slab;
    const SurfaceInput inputs[] = {
        {"DiffuseAlbedo", 3, SurfaceDefaultDiffuseAlbedo, &slab[SlabValue::DiffuseAlbedo]},
        {"F0", 3, SurfaceDefaultF0, &slab[SlabValue::F0]},
        {"Roughness", 1, SurfaceDefaultRoughness, &slab[SlabValue::Roughness]},
        {"ClearCoatCoverage", 1, 1.0f, &slab[SlabValue::HazeWeight]},
        {"ClearCoatRoughness", 1, SurfaceDefaultClearCoatRoughness,
         &slab[SlabValue::HazeRoughness]},
        {"EmissiveColor", 3, 0.0f, &slab[SlabValue::Emissive]},
    };
    auto read = ReadSurfaceInputs(context, inputs);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    IrValueId unused = NoIrValue;
    auto basis =
        ReadBasis(context, "Normal", "VertexNormal", slab[SlabValue::Normal], StringView(), unused);
    if (!basis || !basis.Value()) {
        return basis ? Result<void>::Success() : Result<void>::Failure(basis.Error());
    }

    auto second = ReadSecondNormal(context, "BottomNormal", record.secondNormal);
    if (!second || !second.Value()) {
        return second ? Result<void>::Success() : Result<void>::Failure(second.Error());
    }

    const float ones[] = {1.0f, 1.0f, 1.0f};
    const float zeros[] = {0.0f, 0.0f, 0.0f};
    auto one3 = context.FloatConstant(Span<const float>(ones, 3), StringView());
    auto zero3 = context.FloatConstant(Span<const float>(zeros, 3), StringView());
    auto one = context.FloatScalar(1.0f);
    auto zero = context.FloatScalar(0.0f);
    auto tangent = context.ContextValue("VertexTangent");
    auto simple = context.Builder().BoolConstant(Span<const bool>(&SurfaceTrue, 1));
    if (!one3 || !zero3 || !one || !zero || !tangent || !simple) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    slab[SlabValue::F90] = one3.Value();
    slab[SlabValue::Anisotropy] = zero.Value();
    slab[SlabValue::Tangent] = tangent.Value();
    slab[SlabValue::MeanFreePath] = zero3.Value();
    slab[SlabValue::RadiusScale] = one.Value();
    slab[SlabValue::PhaseAnisotropy] = zero.Value();
    slab[SlabValue::FuzzAmount] = zero.Value();
    slab[SlabValue::FuzzColor] = zero3.Value();
    slab[SlabValue::FuzzRoughness] = slab[SlabValue::Roughness];
    slab[SlabValue::SimpleClearCoat] = simple.Value();
    return EmitSurface(context, record);
}

// UE SubstrateHairBSDF(MES:2120–2220) — 고전 Hair 칸이다(normal은 hair tangent — MES:2137).
Result<void> LowerSurfaceHair(LoweringContext& context) noexcept {
    SurfaceRecord record;
    record.kind = SurfaceKind::Hair;
    record.flow = SurfaceFlowBit(SurfaceFlow::Hair);
    const SurfaceInput inputs[] = {
        {"BaseColor", 3, 0.0f, &record.Field(SurfaceField::BaseColor)},
        {"Scatter", 1, 0.0f, &record.Field(SurfaceField::Metallic)},
        {"Specular", 1, SurfaceDefaultSpecular, &record.Field(SurfaceField::Specular)},
        {"Roughness", 1, SurfaceDefaultRoughness, &record.Field(SurfaceField::Roughness)},
        {"Backlit", 1, 0.0f, &record.Field(SurfaceField::CustomData0)},
        {"EmissiveColor", 3, 0.0f, &record.Field(SurfaceField::Emissive)},
    };
    auto read = ReadSurfaceInputs(context, inputs);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    IrValueId unused = NoIrValue;
    auto basis = ReadBasis(context, "Tangent", "VertexTangent", record.Field(SurfaceField::Normal),
                           StringView(), unused);
    if (!basis || !basis.Value()) {
        return basis ? Result<void>::Success() : Result<void>::Failure(basis.Error());
    }

    auto model = ModelConstant(context, rocks::ShadingModel::Hair);
    if (!model) {
        return Result<void>::Failure(model.Error());
    }

    record.Field(SurfaceField::ShadingModel) = model.Value();
    return EmitSurface(context, record);
}

// UE SubstrateEyeBSDF(MES:2222–2342) — 고전 Eye 칸이다(CorneaNormal 기본은 UE처럼 정점
// tangent — MES:2237, IrisNormal은 MS2d3 ClearCoatBottomNormal 출력).
Result<void> LowerSurfaceEye(LoweringContext& context) noexcept {
    auto profiled =
        ProfilePending(context, "subsurfaceProfile", MaterialPrerequisite::SubsurfaceProfile);
    if (!profiled || profiled.Value()) {
        return profiled ? Result<void>::Success() : Result<void>::Failure(profiled.Error());
    }

    SurfaceRecord record;
    record.kind = SurfaceKind::Eye;
    record.flow = SurfaceFlowBit(SurfaceFlow::Eye);
    const SurfaceInput inputs[] = {
        {"DiffuseColor", 3, 0.0f, &record.Field(SurfaceField::BaseColor)},
        {"Roughness", 1, SurfaceDefaultRoughness, &record.Field(SurfaceField::Roughness)},
        {"IrisMask", 1, 0.0f, &record.Field(SurfaceField::CustomData0)},
        {"IrisDistance", 1, 0.0f, &record.Field(SurfaceField::CustomData1)},
        {"EmissiveColor", 3, 0.0f, &record.Field(SurfaceField::Emissive)},
    };
    auto read = ReadSurfaceInputs(context, inputs);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    IrValueId unused = NoIrValue;
    auto cornea = ReadBasis(context, "CorneaNormal", "VertexTangent",
                            record.Field(SurfaceField::Normal), StringView(), unused);
    if (!cornea || !cornea.Value()) {
        return cornea ? Result<void>::Success() : Result<void>::Failure(cornea.Error());
    }

    // IrisPlaneNormal은 TangentOutput 출력이다(UE 고전 Eye GBuffer가 GetTangentOutput0을 읽는다 —
    // ShadingModelsMaterial.ush:160–166, 연결된 때만 입력 원값).
    auto plane = ReadSecondNormal(context, "IrisPlaneNormal", record.customTangent);
    if (!plane || !plane.Value()) {
        return plane ? Result<void>::Success() : Result<void>::Failure(plane.Error());
    }

    auto iris = ReadSecondNormal(context, "IrisNormal", record.secondNormal);
    if (!iris || !iris.Value()) {
        return iris ? Result<void>::Success() : Result<void>::Failure(iris.Error());
    }

    auto specular = context.FloatScalar(SurfaceEyeSpecular);
    auto metallic = context.FloatScalar(0.0f);
    auto model = ModelConstant(context, rocks::ShadingModel::Eye);
    if (!specular || !metallic || !model) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    record.Field(SurfaceField::Specular) = specular.Value();
    record.Field(SurfaceField::Metallic) = metallic.Value();
    record.Field(SurfaceField::ShadingModel) = model.Value();
    return EmitSurface(context, record);
}

// UE SubstrateSingleLayerWaterBSDF(MES:2345–2465) — 고전 SingleLayerWater 칸이다(metallic은 UE
// 내보내기처럼 0 — S:3965, 물 계수는 MS2d3 출력).
Result<void> LowerSurfaceSingleLayerWater(LoweringContext& context) noexcept {
    SurfaceRecord record;
    record.kind = SurfaceKind::SingleLayerWater;
    record.flow = SurfaceFlowBit(SurfaceFlow::SingleLayerWater);
    IrValueId metallic = NoIrValue;
    const SurfaceInput inputs[] = {
        {"BaseColor", 3, 0.0f, &record.Field(SurfaceField::BaseColor)},
        {"Metallic", 1, 0.0f, &metallic},
        {"Specular", 1, SurfaceDefaultSpecular, &record.Field(SurfaceField::Specular)},
        {"Roughness", 1, SurfaceDefaultRoughness, &record.Field(SurfaceField::Roughness)},
        {"EmissiveColor", 3, 0.0f, &record.Field(SurfaceField::Emissive)},
        {"TopMaterialOpacity", 1, 0.0f, &record.Field(SurfaceField::Opacity)},
    };
    auto read = ReadSurfaceInputs(context, inputs);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    IrValueId unused = NoIrValue;
    auto basis = ReadBasis(context, "Normal", "VertexNormal", record.Field(SurfaceField::Normal),
                           StringView(), unused);
    if (!basis || !basis.Value()) {
        return basis ? Result<void>::Success() : Result<void>::Failure(basis.Error());
    }

    auto zero = context.FloatScalar(0.0f);
    auto model = ModelConstant(context, rocks::ShadingModel::SingleLayerWater);
    if (!zero || !model) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    record.Field(SurfaceField::Metallic) = zero.Value();
    record.Field(SurfaceField::ShadingModel) = model.Value();
    // 물 계수는 고전 출력 형식으로 바꾼다(Scattering = Albedo·Extinction, Absorption =
    // Extinction − Scattering — UE SLC의 역, MS2d3).
    IrValueId albedo = NoIrValue;
    IrValueId extinction = NoIrValue;
    const SurfaceInput waters[] = {
        {"WaterAlbedo", 3, 0.0f, &albedo},
        {"WaterExtinction", 3, 0.0f, &extinction},
        {"WaterPhaseG", 1, 0.0f, &record.water[2]},
        {"ColorScaleBehindWater", 3, 1.0f, &record.water[3]},
    };
    auto water = ReadSurfaceInputs(context, waters);
    if (!water || !water.Value()) {
        return water ? Result<void>::Success() : Result<void>::Failure(water.Error());
    }

    auto scattering = context.Binary(IrOp::Multiply, albedo, extinction, NumberRule::FloatOnly);
    if (!scattering) {
        return Result<void>::Failure(scattering.Error());
    }

    auto absorption =
        context.Binary(IrOp::Subtract, extinction, scattering.Value(), NumberRule::FloatOnly);
    if (!absorption) {
        return Result<void>::Failure(absorption.Error());
    }

    record.water[0] = scattering.Value();
    record.water[1] = absorption.Value();
    return EmitSurface(context, record);
}

constexpr LoweringEntry SurfaceEntries[] = {
    {"Material.ShadingModel", &LowerShadingModel},
    {"Material.SurfaceFromShadingModel", &LowerSurfaceFromShadingModel},
    {"Material.SurfaceFromAttributes", &LowerSurfaceFromAttributes},
    {"Material.SurfaceUnlit", &LowerSurfaceUnlit},
    {"Material.SurfaceLightFunction", &LowerSurfaceLightFunction},
    {"Material.SurfacePostProcess", &LowerSurfacePostProcess},
    {"Material.SurfaceUserInterface", &LowerSurfaceUserInterface},
    {"Material.SurfaceVolumetricFogCloud", &LowerSurfaceVolumetricFogCloud},
    {"Material.SurfaceToDecal", &LowerSurfaceToDecal},
    {"Material.SurfaceSlab", &LowerSurfaceSlab},
    {"Material.SurfaceSimpleClearCoat", &LowerSurfaceSimpleClearCoat},
    {"Material.SurfaceHair", &LowerSurfaceHair},
    {"Material.SurfaceEye", &LowerSurfaceEye},
    {"Material.SurfaceSingleLayerWater", &LowerSurfaceSingleLayerWater},
};

} // namespace

Result<IrValueId> ClassicSurfaceFromAttributes(LoweringContext& context,
                                               Span<const AttributeEntry> entries) noexcept {
    // 출력 노드 속성 집합의 표면 변환이다(UE 숨은 변환 — HT:1545–1560, shading model 기본
    // DefaultLit, 물 계수 기본값 — 6.12절 MS2e1).
    ClassicSource source;
    source.context = &context;
    source.entries = entries;
    source.attributes = true;
    source.waterPins = false;
    SurfaceRecord record;
    auto built = BuildClassicRecord(source, StringView(), record);
    if (!built || !built.Value()) {
        return built ? Result<IrValueId>::Success(NoIrValue)
                     : Result<IrValueId>::Failure(built.Error());
    }

    return context.MakeSurface(record);
}

Span<const LoweringEntry> SurfaceLowerings() noexcept {
    return SurfaceEntries;
}

} // namespace rocks::material::detail
