#include <Core/Text/String.h>
#include <MaterialSchema/MaterialPrerequisite.h>
#include <MaterialSchema/Nodes/MaterialAttributes/MaterialAttributesNodeEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"

// 속성 집합 노드 lowering이다(Docs/Design/MaterialSystem 6.12절 MS2e1). 속성 집합 값은 번역기 속성
// 기록(설정된 칸만)을 가리키는 IR AttributesRef이고, 설정되지 않은 칸은 쓰는 곳이 기본값을 넣는다
// (UE INDEX_NONE).
namespace rocks::material::detail {

namespace {

// UE ShadingModel 혼합의 문턱이다(CompileShadingModelBlendFunction — Alpha ≥ 0.5면 B).
constexpr float AttributeShadingModelThreshold = 0.5f;
// Blend의 정수·Bool 선택 문턱이다(UE MIR — Alpha < 0.5면 A).
constexpr float AttributeIntegerThreshold = 0.5f;

// Make·Break 핀 키 → 속성 이름이다(UE GetExpressionInput·BuildPropertyToIOIndexMap — ClearCoat 칸은
// CustomData0·1).
struct AttributePinName {
    StringView pin;
    StringView attribute;
};

constexpr AttributePinName MakePinNames[] = {
    {"BaseColor", "BaseColor"},
    {"Metallic", "Metallic"},
    {"Specular", "Specular"},
    {"Roughness", "Roughness"},
    {"Anisotropy", "Anisotropy"},
    {"EmissiveColor", "EmissiveColor"},
    {"Opacity", "Opacity"},
    {"OpacityMask", "OpacityMask"},
    {"Normal", "Normal"},
    {"Tangent", "Tangent"},
    {"WorldPositionOffset", "WorldPositionOffset"},
    {"SubsurfaceColor", "SubsurfaceColor"},
    {"ClearCoat", "CustomData0"},
    {"ClearCoatRoughness", "CustomData1"},
    {"AmbientOcclusion", "AmbientOcclusion"},
    {"Refraction", "Refraction"},
    {"CustomizedUVs_0", "CustomizedUV0"},
    {"CustomizedUVs_1", "CustomizedUV1"},
    {"CustomizedUVs_2", "CustomizedUV2"},
    {"CustomizedUVs_3", "CustomizedUV3"},
    {"CustomizedUVs_4", "CustomizedUV4"},
    {"CustomizedUVs_5", "CustomizedUV5"},
    {"CustomizedUVs_6", "CustomizedUV6"},
    {"CustomizedUVs_7", "CustomizedUV7"},
    {"PixelDepthOffset", "PixelDepthOffset"},
    {"ShadingModel", "ShadingModel"},
    {"Displacement", "Displacement"},
};

constexpr AttributePinName BreakPinNames[] = {
    {"BaseColor", "BaseColor"},
    {"Metallic", "Metallic"},
    {"Specular", "Specular"},
    {"Roughness", "Roughness"},
    {"Anisotropy", "Anisotropy"},
    {"EmissiveColor", "EmissiveColor"},
    {"Opacity", "Opacity"},
    {"OpacityMask", "OpacityMask"},
    {"Normal", "Normal"},
    {"Tangent", "Tangent"},
    {"WorldPositionOffset", "WorldPositionOffset"},
    {"SubsurfaceColor", "SubsurfaceColor"},
    {"ClearCoat", "CustomData0"},
    {"ClearCoatRoughness", "CustomData1"},
    {"AmbientOcclusion", "AmbientOcclusion"},
    {"Refraction", "Refraction"},
    {"CustomizedUV0", "CustomizedUV0"},
    {"CustomizedUV1", "CustomizedUV1"},
    {"CustomizedUV2", "CustomizedUV2"},
    {"CustomizedUV3", "CustomizedUV3"},
    {"CustomizedUV4", "CustomizedUV4"},
    {"CustomizedUV5", "CustomizedUV5"},
    {"CustomizedUV6", "CustomizedUV6"},
    {"CustomizedUV7", "CustomizedUV7"},
    {"PixelDepthOffset", "PixelDepthOffset"},
    {"ShadingModel", "ShadingModel"},
    {"Displacement", "Displacement"},
};

// 속성 집합 입력을 읽는다. 연결이 없으면 빈 기록(entries 비움), 속성 집합이 아니면 V1이고 false다.
Result<bool> AttributesInput(LoweringContext& context, StringView key, bool required,
                             Array<AttributeEntry>& entries) noexcept {
    entries.Clear();
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<bool>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        if (required) {
            if (auto reported =
                    context.Error(MaterialDiagnosticCode::MissingInput, key, "Missing input");
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }

            return Result<bool>::Success(false);
        }

        return Result<bool>::Success(true);
    }

    auto value = context.Input(key);
    if (!value) {
        return Result<bool>::Failure(value.Error());
    }

    if (value.Value() == NoIrValue) {
        return Result<bool>::Success(false);
    }

    auto copied = context.CopyAttributes(value.Value(), entries);
    if (!copied) {
        return Result<bool>::Failure(copied.Error());
    }

    if (!copied.Value()) {
        // UE MaterialShared.cpp:758–761.
        if (auto reported = context.Error(
                MaterialDiagnosticCode::TypeMismatch, key,
                "Cannot connect a non MaterialAttributes node to a MaterialAttributes pin.");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    return Result<bool>::Success(true);
}

// 칸 하나를 넣는다(같은 Guid는 덮는다).
Result<void> AttributeSet(Array<AttributeEntry>& entries, Guid id, IrValueId value) noexcept {
    for (AttributeEntry& entry : entries) {
        if (entry.id == id) {
            entry.value = value;
            return Result<void>::Success();
        }
    }

    return entries.TryPushBack(AttributeEntry{id, value});
}

// UE MakeMaterialAttributes(ME:6123–6171): 연결된 핀만 칸이다.
Result<void> LowerMakeAttributes(LoweringContext& context) noexcept {
    SystemAllocator scratch;
    Array<AttributeEntry> entries(scratch);
    for (const AttributePinName& item : MakePinNames) {
        const MaterialAttributeDefinition* definition = context.FindAttributeByName(item.attribute);
        if (definition == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto connected = context.IsConnected(item.pin);
        if (!connected) {
            return Result<void>::Failure(connected.Error());
        }

        if (!connected.Value()) {
            continue;
        }

        auto value = context.Input(item.pin);
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        auto fitted = context.FitAttribute(value.Value(), *definition, item.pin);
        if (!fitted) {
            return Result<void>::Failure(fitted.Error());
        }

        if (fitted.Value() == NoIrValue) {
            // 위쪽 오류 또는 타입 오류다(진단했다).
            return Result<void>::Success();
        }

        if (auto added = AttributeSet(entries, definition->id, fitted.Value()); !added) {
            return added;
        }
    }

    auto made = context.MakeAttributes(entries.View());
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    return context.Output("Result", made.Value());
}

// 칸 값을 출력 핀 타입으로 맞춰 낸다.
Result<void> AttributeOutput(LoweringContext& context, Name pinName, StringView pin,
                             MaterialPinType pinType, Span<const AttributeEntry> entries,
                             const MaterialAttributeDefinition& definition) noexcept {
    auto value = context.AttributeValue(entries, definition);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    IrValueId out = value.Value();
    if (out != NoIrValue && pinType != MaterialPinType::Any &&
        pinType != MaterialPinType::ShadingModel && pinType != MaterialPinType::Surface) {
        auto coerced = context.Coerce(out, pinType, pin);
        if (!coerced) {
            return Result<void>::Failure(coerced.Error());
        }

        out = coerced.Value();
    }

    if (out == NoIrValue) {
        return Result<void>::Success();
    }

    return pin.IsEmpty() ? context.OutputByName(pinName, out) : context.Output(pin, out);
}

// UE BreakMaterialAttributes(ME:6206–6382).
Result<void> LowerBreakAttributes(LoweringContext& context) noexcept {
    SystemAllocator scratch;
    Array<AttributeEntry> entries(scratch);
    auto read = AttributesInput(context, "MaterialAttributes", true, entries);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    for (const AttributePinName& item : BreakPinNames) {
        const MaterialAttributeDefinition* definition = context.FindAttributeByName(item.attribute);
        const MaterialPinDefinition* pin = nullptr;
        for (const MaterialPinDefinition& candidate : context.Definition().pins) {
            if (candidate.key == item.pin &&
                candidate.direction == nodegraph::PinDirection::Output) {
                pin = &candidate;
            }
        }

        if (definition == nullptr || pin == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto written =
                AttributeOutput(context, Name(), item.pin, pin->type, entries.View(), *definition);
            !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// Get·Set의 속성 id 목록을 읽고 검사한다(UE ME:6438–6452 — 중복·없는 속성).
Result<bool> AttributeIds(LoweringContext& context, StringView field,
                          Array<const MaterialAttributeDefinition*>& out) noexcept {
    out.Clear();
    auto properties = context.Properties();
    if (!properties || properties.Value() == nullptr) {
        return properties ? Result<bool>::Success(false)
                          : Result<bool>::Failure(properties.Error());
    }

    SystemAllocator scratch;
    Array<Guid> ids(scratch);
    if (auto read = properties.Value()->ReadGuidArray(field, ids); !read) {
        if (auto reported = context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                          "Material attribute list cannot be read");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    for (usize index = 0; index < ids.Size(); ++index) {
        for (usize other = 0; other < index; ++other) {
            if (ids[other] == ids[index]) {
                if (auto reported = context.Error(MaterialDiagnosticCode::InvalidProperty,
                                                  StringView(), "Duplicate attribute types.");
                    !reported) {
                    return Result<bool>::Failure(reported.Error());
                }

                return Result<bool>::Success(false);
            }
        }

        const MaterialAttributeDefinition* definition = context.FindAttribute(ids[index]);
        if (definition == nullptr) {
            if (auto reported = context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                              "Property type doesn't exist, needs re-mapping?");
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }

            return Result<bool>::Success(false);
        }

        if (auto added = out.TryPushBack(definition); !added) {
            return Result<bool>::Failure(added.Error());
        }
    }

    return Result<bool>::Success(true);
}

// 동적 핀 중 direction 쪽만 순서대로다.
Result<bool> AttributePins(LoweringContext& context, nodegraph::PinDirection direction,
                           Array<nodegraph::PinDescriptor>& out) noexcept {
    out.Clear();
    auto pins = context.DynamicPins();
    if (!pins) {
        return Result<bool>::Failure(pins.Error());
    }

    for (const nodegraph::PinDescriptor& pin : pins.Value()) {
        if (pin.direction == direction) {
            if (auto added = out.TryPushBack(pin); !added) {
                return Result<bool>::Failure(added.Error());
            }
        }
    }

    return Result<bool>::Success(true);
}

// UE GetMaterialAttributes(ME:6384–6640): 첫 출력은 그대로, 나머지는 칸 값·기본값이다.
Result<void> LowerGetAttributes(LoweringContext& context) noexcept {
    SystemAllocator scratch;
    Array<const MaterialAttributeDefinition*> definitions(scratch);
    auto ids = AttributeIds(context, "attributeGetTypes", definitions);
    if (!ids || !ids.Value()) {
        return ids ? Result<void>::Success() : Result<void>::Failure(ids.Error());
    }

    Array<AttributeEntry> entries(scratch);
    auto read = AttributesInput(context, "MaterialAttributes", true, entries);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto passthrough = context.Input("MaterialAttributes");
    if (!passthrough) {
        return Result<void>::Failure(passthrough.Error());
    }

    if (auto written = context.Output("MaterialAttributesOut", passthrough.Value()); !written) {
        return written;
    }

    Array<nodegraph::PinDescriptor> outputs(scratch);
    auto pins = AttributePins(context, nodegraph::PinDirection::Output, outputs);
    if (!pins) {
        return Result<void>::Failure(pins.Error());
    }

    for (usize index = 0; index < outputs.Size() && index < definitions.Size(); ++index) {
        const MaterialAttributeDefinition& definition = *definitions[index];
        if (auto written = AttributeOutput(context, outputs[index].key, StringView(),
                                           definition.type, entries.View(), definition);
            !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// UE SetMaterialAttributes(ME:6646–7047): 입력 기록(연결 없으면 빈 기록)을 연결된 속성 입력으로
// 덮는다.
Result<void> LowerSetAttributes(LoweringContext& context) noexcept {
    SystemAllocator scratch;
    Array<const MaterialAttributeDefinition*> definitions(scratch);
    auto ids = AttributeIds(context, "attributeSetTypes", definitions);
    if (!ids || !ids.Value()) {
        return ids ? Result<void>::Success() : Result<void>::Failure(ids.Error());
    }

    Array<AttributeEntry> entries(scratch);
    auto read = AttributesInput(context, "MaterialAttributes", false, entries);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    Array<nodegraph::PinDescriptor> inputs(scratch);
    auto pins = AttributePins(context, nodegraph::PinDirection::Input, inputs);
    if (!pins) {
        return Result<void>::Failure(pins.Error());
    }

    for (usize index = 0; index < inputs.Size() && index < definitions.Size(); ++index) {
        auto value = context.DynamicInput(inputs[index].key, Optional<float>(), false);
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        if (value.Value() == NoIrValue) {
            continue;
        }

        auto fitted = context.FitAttribute(value.Value(), *definitions[index], StringView());
        if (!fitted) {
            return Result<void>::Failure(fitted.Error());
        }

        if (fitted.Value() == NoIrValue) {
            return Result<void>::Success();
        }

        if (auto added = AttributeSet(entries, definitions[index]->id, fitted.Value()); !added) {
            return added;
        }
    }

    auto made = context.MakeAttributes(entries.View());
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    return context.Output("Result", made.Value());
}

// 칸 하나의 혼합이다(UE BlendMaterialAttributes::Compile — ME:7091–7125). mode는 그 칸의 빈도에
// 맞는 방식이다. ShadingModel은 Alpha ≥ 0.5면 B, 표면은 섞지 않는다(칸 없음).
Result<IrValueId> AttributeBlend(LoweringContext& context, MaterialAttributeBlend mode,
                                 Span<const AttributeEntry> a, Span<const AttributeEntry> b,
                                 IrValueId alpha,
                                 const MaterialAttributeDefinition& definition) noexcept {
    const auto find = [&definition](Span<const AttributeEntry> entries) noexcept {
        for (const AttributeEntry& entry : entries) {
            if (entry.id == definition.id) {
                return entry.value;
            }
        }

        return NoIrValue;
    };

    if (mode == MaterialAttributeBlend::UseA) {
        return Result<IrValueId>::Success(find(a));
    }

    if (mode == MaterialAttributeBlend::UseB) {
        return Result<IrValueId>::Success(find(b));
    }

    if (definition.type == MaterialPinType::Surface) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    auto x = context.AttributeValue(a, definition);
    auto y = context.AttributeValue(b, definition);
    if (!x || !y) {
        return !x ? Move(x) : Move(y);
    }

    // 같은 값이면 Alpha를 보기 전에 그 값이다(UE ME:7108–7112 — Alpha가 없어도 칸이 남는다).
    if (x.Value() == y.Value() || x.Value() == NoIrValue || y.Value() == NoIrValue) {
        return x;
    }

    if (alpha == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (definition.type == MaterialPinType::ShadingModel) {
        auto threshold = context.FloatScalar(AttributeShadingModelThreshold);
        if (!threshold) {
            return threshold;
        }

        auto condition = context.Binary(IrOp::GreaterThanOrEquals, alpha, threshold.Value(),
                                        NumberRule::FloatOnly);
        if (!condition) {
            return condition;
        }

        return context.Select(condition.Value(), y.Value(), x.Value());
    }

    return context.Ternary(IrOp::Lerp, x.Value(), y.Value(), alpha, NumberRule::FloatOnly);
}

// a·b 어느 쪽에든 있는 칸마다 혼합한다. vertexWpoOnly면 정점 방식은 WPO만이다(UE MIR
// BlendAggregate — 고전 BlendAttributes는 정의 빈도).
Result<IrValueId> BlendRecords(LoweringContext& context, MaterialAttributeBlend pixelMode,
                               MaterialAttributeBlend vertexMode, Span<const AttributeEntry> a,
                               Span<const AttributeEntry> b, IrValueId alpha,
                               bool vertexWpoOnly) noexcept {
    SystemAllocator scratch;
    Array<AttributeEntry> merged(scratch);
    const Span<const AttributeEntry> sides[] = {a, b};
    for (Span<const AttributeEntry> side : sides) {
        for (const AttributeEntry& entry : side) {
            bool seen = false;
            for (const AttributeEntry& item : merged) {
                seen = seen || item.id == entry.id;
            }

            if (seen) {
                continue;
            }

            const MaterialAttributeDefinition* definition = context.FindAttribute(entry.id);
            if (definition == nullptr) {
                return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
            }

            const bool vertex = vertexWpoOnly
                                    ? definition->name == StringView("WorldPositionOffset")
                                    : definition->frequency == MaterialVertexStage;
            auto blended =
                AttributeBlend(context, vertex ? vertexMode : pixelMode, a, b, alpha, *definition);
            if (!blended) {
                return blended;
            }

            if (auto added = merged.TryPushBack(AttributeEntry{entry.id, blended.Value()});
                !added) {
                return Result<IrValueId>::Failure(added.Error());
            }
        }
    }

    return context.MakeAttributes(merged.View());
}

// 정수 속성(열거) 하나를 읽는다.
Result<bool> AttributeEnum(LoweringContext& context, StringView field, int64& out) noexcept {
    return context.ReadInteger(field, out, out);
}

// UE BlendMaterialAttributes(ME:7049–7140).
Result<void> LowerBlendAttributes(LoweringContext& context) noexcept {
    int64 pixel = static_cast<int64>(MaterialAttributeBlend::Blend);
    int64 vertex = static_cast<int64>(MaterialAttributeBlend::Blend);
    auto pixelRead = AttributeEnum(context, "pixelAttributeBlendType", pixel);
    auto vertexRead = AttributeEnum(context, "vertexAttributeBlendType", vertex);
    if (!pixelRead || !vertexRead) {
        return Result<void>::Failure(!pixelRead ? pixelRead.Error() : vertexRead.Error());
    }

    if (!pixelRead.Value() || !vertexRead.Value()) {
        return Result<void>::Success();
    }

    SystemAllocator scratch;
    Array<AttributeEntry> a(scratch);
    Array<AttributeEntry> b(scratch);
    auto readA = AttributesInput(context, "A", false, a);
    auto readB = AttributesInput(context, "B", false, b);
    if (!readA || !readB) {
        return Result<void>::Failure(!readA ? readA.Error() : readB.Error());
    }

    if (!readA.Value() || !readB.Value()) {
        return Result<void>::Success();
    }

    // Alpha가 연결되지 않으면 Blend 칸은 없음이다(UE Lerp INDEX_NONE).
    auto alphaConnected = context.IsConnected("Alpha");
    if (!alphaConnected) {
        return Result<void>::Failure(alphaConnected.Error());
    }

    IrValueId alpha = NoIrValue;
    if (alphaConnected.Value()) {
        auto value = context.Input("Alpha");
        if (!value || value.Value() == NoIrValue) {
            return value ? Result<void>::Success() : Result<void>::Failure(value.Error());
        }

        auto coerced = context.CoerceToWidth(value.Value(), 1, "Alpha");
        if (!coerced || coerced.Value() == NoIrValue) {
            return coerced ? Result<void>::Success() : Result<void>::Failure(coerced.Error());
        }

        alpha = coerced.Value();
    }

    auto blended =
        BlendRecords(context, static_cast<MaterialAttributeBlend>(pixel),
                     static_cast<MaterialAttributeBlend>(vertex), a.View(), b.View(), alpha, false);
    if (!blended) {
        return Result<void>::Failure(blended.Error());
    }

    return context.Output("Result", blended.Value());
}

// 타입 이름이다(Blend 문구).
StringView AttributeTypeName(IrType type) noexcept {
    if (type.IsSurface()) {
        return "Surface";
    }

    return LoweringTypeName(type);
}

// UE Blend(MIR:3857–3989 — NewMaterialTranslator 전용, MD10).
Result<void> LowerBlend(LoweringContext& context) noexcept {
    int64 pixel = static_cast<int64>(LayerBlendMode::Blend);
    int64 vertex = static_cast<int64>(LayerBlendMode::Blend);
    auto pixelRead = AttributeEnum(context, "pixelAttributesBlendMode", pixel);
    auto vertexRead = AttributeEnum(context, "vertexAttributesBlendMode", vertex);
    if (!pixelRead || !vertexRead) {
        return Result<void>::Failure(!pixelRead ? pixelRead.Error() : vertexRead.Error());
    }

    if (!pixelRead.Value() || !vertexRead.Value()) {
        return Result<void>::Success();
    }

    auto aConnected = context.IsConnected("A");
    auto bConnected = context.IsConnected("B");
    if (!aConnected || !bConnected) {
        return Result<void>::Failure(!aConnected ? aConnected.Error() : bConnected.Error());
    }

    if (!aConnected.Value() && !bConnected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, StringView(),
                             "No input value provided.");
    }

    IrValueId sides[2] = {NoIrValue, NoIrValue};
    const StringView keys[] = {"A", "B"};
    const bool connected[] = {aConnected.Value(), bConnected.Value()};
    for (usize index = 0; index < 2; ++index) {
        if (!connected[index]) {
            continue;
        }

        auto value = context.Input(keys[index]);
        if (!value || value.Value() == NoIrValue) {
            return value ? Result<void>::Success() : Result<void>::Failure(value.Error());
        }

        sides[index] = value.Value();
    }

    // 한쪽이 없으면 다른 쪽 타입의 기본 상수(0 — 속성 집합은 빈 기록)다.
    for (usize index = 0; index < 2; ++index) {
        if (sides[index] != NoIrValue) {
            continue;
        }

        const IrType other = context.TypeOf(sides[1 - index]);
        auto fallback = other.IsAttributes() ? context.MakeAttributes({})
                        : other.IsOpaque()   ? Result<IrValueId>::Success(NoIrValue)
                                             : context.Zero(other);
        if (!fallback) {
            return Result<void>::Failure(fallback.Error());
        }

        sides[index] = fallback.Value();
    }

    auto alphaValue = context.Input("Alpha");
    if (!alphaValue) {
        return Result<void>::Failure(alphaValue.Error());
    }

    IrValueId alpha = alphaValue.Value();
    if (alpha == NoIrValue) {
        auto zero = context.FloatScalar(0.0f);
        if (!zero) {
            return Result<void>::Failure(zero.Error());
        }

        alpha = zero.Value();
    } else {
        auto coerced = context.CoerceToWidth(alpha, 1, "Alpha");
        if (!coerced || coerced.Value() == NoIrValue) {
            return coerced ? Result<void>::Success() : Result<void>::Failure(coerced.Error());
        }

        alpha = coerced.Value();
    }

    const IrType a = sides[0] != NoIrValue ? context.TypeOf(sides[0]) : IrFloat(1);
    const IrType b = sides[1] != NoIrValue ? context.TypeOf(sides[1]) : IrFloat(1);
    if (a.IsAttributes() && b.IsAttributes()) {
        SystemAllocator scratch;
        Array<AttributeEntry> first(scratch);
        Array<AttributeEntry> second(scratch);
        auto copiedA = context.CopyAttributes(sides[0], first);
        auto copiedB = context.CopyAttributes(sides[1], second);
        if (!copiedA || !copiedB) {
            return Result<void>::Failure(!copiedA ? copiedA.Error() : copiedB.Error());
        }

        auto blended = BlendRecords(context, static_cast<MaterialAttributeBlend>(pixel),
                                    static_cast<MaterialAttributeBlend>(vertex), first.View(),
                                    second.View(), alpha, true);
        if (!blended) {
            return Result<void>::Failure(blended.Error());
        }

        return context.Output("Result", blended.Value());
    }

    if (a.IsOpaque() || b.IsOpaque() || sides[0] == NoIrValue || sides[1] == NoIrValue) {
        SystemAllocator scratch;
        String message(scratch);
        const StringView parts[] = {"Cannot blend values of type '",
                                    AttributeTypeName(a.IsOpaque() ? a : b), "'."};
        for (StringView part : parts) {
            if (auto appended = message.TryAppend(part); !appended) {
                return appended;
            }
        }

        return context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(), message.View());
    }

    const IrValueId pair[] = {sides[0], sides[1]};
    auto common = context.CommonType(Span<const IrValueId>(pair, 2), NumberRule::Equality);
    if (!common || !common.Value()) {
        return common ? Result<void>::Success() : Result<void>::Failure(common.Error());
    }

    const IrType type = common.Value().Value();
    auto x = context.Cast(sides[0], type);
    auto y = context.Cast(sides[1], type);
    if (!x || !y) {
        return Result<void>::Failure(!x ? x.Error() : y.Error());
    }

    if (type.scalar == IrScalar::Float || type.scalar == IrScalar::Double) {
        auto lerped =
            context.Ternary(IrOp::Lerp, x.Value(), y.Value(), alpha, NumberRule::Arithmetic);
        if (!lerped) {
            return Result<void>::Failure(lerped.Error());
        }

        return context.Output("Result", lerped.Value());
    }

    // 정수·Bool은 Alpha < 0.5면 A다.
    auto threshold = context.FloatScalar(AttributeIntegerThreshold);
    if (!threshold) {
        return Result<void>::Failure(threshold.Error());
    }

    auto condition =
        context.Binary(IrOp::LessThan, alpha, threshold.Value(), NumberRule::FloatOnly);
    if (!condition) {
        return Result<void>::Failure(condition.Error());
    }

    auto selected = context.Select(condition.Value(), x.Value(), y.Value());
    if (!selected) {
        return Result<void>::Failure(selected.Error());
    }

    return context.Output("Result", selected.Value());
}

// UE Aggregate(MaterialAttributes 종류 — MIR:3919–3955).
Result<void> LowerAggregate(LoweringContext& context) noexcept {
    int64 kind = static_cast<int64>(AggregateKind::MaterialAttributes);
    auto kindRead = AttributeEnum(context, "kind", kind);
    if (!kindRead || !kindRead.Value()) {
        return kindRead ? Result<void>::Success() : Result<void>::Failure(kindRead.Error());
    }

    if (kind != static_cast<int64>(AggregateKind::MaterialAttributes)) {
        // TODO(MPQ-MaterialAggregate): 사용자 aggregate 에셋이 생기면 그 칸 정의로 번역한다.
        auto pending = context.Builder().Pending(
            IrAttributesType(), IrPendingKind::PrerequisiteMissing,
            static_cast<uint8>(MaterialPrerequisite::MaterialAggregate), context.Node().id);
        if (!pending) {
            return Result<void>::Failure(pending.Error());
        }

        return context.Output("Result", pending.Value());
    }

    SystemAllocator scratch;
    Array<AttributeEntry> entries(scratch);
    auto read = AttributesInput(context, "PrototypeInput", false, entries);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto pins = context.DynamicPins();
    if (!pins) {
        return Result<void>::Failure(pins.Error());
    }

    // 동적 입력은 속성 이름(19.13절 — 키 = 이름)이다.
    Array<const MaterialAttributeDefinition*> definitions(scratch);
    for (const nodegraph::PinDescriptor& pin : pins.Value()) {
        if (pin.direction != nodegraph::PinDirection::Input) {
            continue;
        }

        auto found = context.FindAttributeByKey(pin.key);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        const MaterialAttributeDefinition* definition = found.Value();
        if (definition == nullptr) {
            auto name = context.Names().Acquire(pin.key);
            if (!name) {
                return Result<void>::Failure(name.Error());
            }

            SystemAllocator scratch;
            String message(scratch);
            const StringView parts[] = {"Unknown material attribute '", name.Value().View(), "'"};
            for (StringView part : parts) {
                if (auto appended = message.TryAppend(part); !appended) {
                    return appended;
                }
            }

            return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                 message.View());
        }

        auto value = context.DynamicInput(pin.key, Optional<float>(), false);
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        if (value.Value() != NoIrValue) {
            auto fitted = context.FitAttribute(value.Value(), *definition, StringView());
            if (!fitted || fitted.Value() == NoIrValue) {
                return fitted ? Result<void>::Success() : Result<void>::Failure(fitted.Error());
            }

            if (auto added = AttributeSet(entries, definition->id, fitted.Value()); !added) {
                return added;
            }
        }

        if (auto added = definitions.TryPushBack(definition); !added) {
            return added;
        }
    }

    auto made = context.MakeAttributes(entries.View());
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    if (auto written = context.Output("Result", made.Value()); !written) {
        return written;
    }

    usize output = 0;
    for (const nodegraph::PinDescriptor& pin : pins.Value()) {
        if (pin.direction != nodegraph::PinDirection::Output || output >= definitions.Size()) {
            continue;
        }

        const MaterialAttributeDefinition& definition = *definitions[output++];
        if (auto written = AttributeOutput(context, pin.key, StringView(), definition.type,
                                           entries.View(), definition);
            !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

constexpr LoweringEntry AttributeEntries[] = {
    {"Material.MakeAttributes", &LowerMakeAttributes},
    {"Material.BreakAttributes", &LowerBreakAttributes},
    {"Material.GetAttributes", &LowerGetAttributes},
    {"Material.SetAttributes", &LowerSetAttributes},
    {"Material.BlendAttributes", &LowerBlendAttributes},
    {"Material.Blend", &LowerBlend},
    {"Material.Aggregate", &LowerAggregate},
};

} // namespace

Span<const LoweringEntry> AttributeLowerings() noexcept {
    return AttributeEntries;
}

} // namespace rocks::material::detail
