#include <Core/Text/StringBuilder.h>
#include <MaterialSchema/Nodes/Parameters/ParametersNodeEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// 파라미터 노드 lowering이다(Docs/Design/MaterialSystem 6.10절 MS2c2). 수 파라미터는 IR
// Parameter(빈도 Uniform — 상수 식 프로그램이 상수 블록으로 끌어올린다), static 파라미터는 번역
// 입력의 static 값(없으면 노드 기본값)으로 정하고, 컬렉션은 번역 입력의 layout으로
// CollectionValue를 읽는다. 검사 순서·문구는 UE 기본 번역기와
// 같다(MaterialExpressions.cpp·HLSLMaterialTranslator.cpp).
namespace rocks::material::detail {

namespace {

// custom primitive data의 float 수다(UE FCustomPrimitiveData::NumCustomPrimitiveDataFloats).
constexpr int64 MaxCustomPrimitiveDataFloats = 36;

// LWC tile 한 칸의 크기(cm)다(MaterialDouble.rsh MaterialDoubleTileSize — UE
// UE_LWC_RENDER_TILE_SIZE).
constexpr double CollectionTileSize = 2097152.0;

// 컬렉션 변환 종류마다 필요한 연속 벡터 수다(UE MaterialExpressions.cpp:17366–17405).
constexpr uint32 PositionMatrixVectors = 4;
constexpr uint32 DirectionMatrixVectors = 3;
constexpr uint32 LargeWorldMatrixVectors = 5;

constexpr StringView CustomPrimitiveDataUiMessage =
    "Custom Primitive Data can't be used with the UI material domain.";

Result<void> EmitParameter(LoweringContext& context, StringView key,
                           Result<IrValueId> value) noexcept {
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output(key, value.Value());
}

// parameterName 원문이다(진단 문구용 — 읽지 못하면 빈 문자열).
Result<StringView> ParameterNameText(LoweringContext& context) noexcept {
    auto properties = context.Properties();
    if (!properties) {
        return Result<StringView>::Failure(properties.Error());
    }

    if (properties.Value() == nullptr) {
        return Result<StringView>::Success(StringView());
    }

    auto name = properties.Value()->ReadText("parameterName");
    if (!name) {
        return name.Error() == ErrorCode::OutOfMemory ? Result<StringView>::Failure(name.Error())
                                                      : Result<StringView>::Success(StringView());
    }

    return name;
}

// prefix + parameterName + suffix 문구로 InvalidProperty를 진단한다(UE Errorf("... %s ...")).
Result<void> NamedParameterError(LoweringContext& context, StringView prefix,
                                 StringView suffix) noexcept {
    auto name = ParameterNameText(context);
    if (!name) {
        return Result<void>::Failure(name.Error());
    }

    SystemAllocator scratch;
    StringBuilder<> message(scratch);
    message << prefix << name.Value() << suffix;
    if (auto status = message.Status(); !status) {
        return status;
    }

    return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), message.View());
}

// 수 파라미터 읽기 값이다(parameterId·기본값 — 6.10절).
Result<IrValueId> NumericParameter(LoweringContext& context, IrParameterKind kind,
                                   const double (&defaults)[MaxIrComponents]) noexcept {
    Guid id;
    auto read = context.ReadGuid("parameterId", id);
    if (!read || !read.Value()) {
        return read ? Result<IrValueId>::Success(NoIrValue)
                    : Result<IrValueId>::Failure(read.Error());
    }

    IrParameter parameter;
    parameter.id = id;
    parameter.kind = kind;
    parameter.type = kind == IrParameterKind::Scalar   ? IrFloat(1)
                     : kind == IrParameterKind::Vector ? IrFloat(MaxIrComponents)
                                                       : IrDouble(MaxIrComponents);
    for (usize index = 0; index < MaxIrComponents; ++index) {
        parameter.defaults[index] = defaults[index];
    }

    return context.Builder().Parameter(parameter);
}

// custom primitive data 읽기다(UE CustomPrimitiveData — UI 영역은 오류, 번호는 0–35). 쓰지 않으면
// used가 거짓이다.
Result<IrValueId> CustomPrimitiveData(LoweringContext& context, StringView name,
                                      bool& used) noexcept {
    used = false;
    bool enabled = false;
    auto read = context.ReadBool("useCustomPrimitiveData", false, enabled);
    if (!read) {
        return Result<IrValueId>::Failure(read.Error());
    }

    if (!read.Value() || !enabled) {
        // 읽지 못했으면(진단함) 노드를 여기서 끝낸다.
        used = !read.Value();
        return Result<IrValueId>::Success(NoIrValue);
    }

    used = true;
    if (context.Traits().domain == MaterialDomain::UserInterface) {
        if (auto reported = context.Error(MaterialDiagnosticCode::UnsupportedCapability,
                                          StringView(), CustomPrimitiveDataUiMessage);
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    int64 index = 0;
    auto indexRead = context.ReadInteger("primitiveDataIndex", 0, index);
    if (!indexRead || !indexRead.Value()) {
        return indexRead ? Result<IrValueId>::Success(NoIrValue)
                         : Result<IrValueId>::Failure(indexRead.Error());
    }

    if (index < 0 || index >= MaxCustomPrimitiveDataFloats) {
        // UE는 check()다(편집기가 0–35로 묶는다).
        if (auto reported = context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                          "Custom primitive data index is out of range");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    const int32 slot = static_cast<int32>(index);
    auto argument = context.Builder().IntConstant(Span<const int32>(&slot, 1));
    if (!argument) {
        return argument;
    }

    const IrValueId arguments[] = {argument.Value()};
    return context.ContextValue(name, arguments);
}

// UE ScalarParameter(MaterialExpressions.cpp:8425–8440).
Result<void> LowerScalarParameter(LoweringContext& context) noexcept {
    bool usesData = false;
    auto data = CustomPrimitiveData(context, "CustomPrimitiveData1", usesData);
    if (!data || usesData) {
        return data ? context.Output("Result", data.Value()) : Result<void>::Failure(data.Error());
    }

    float value = 0.0f;
    auto read = context.ReadFloat("defaultValue", 0.0f, value);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    const double defaults[MaxIrComponents] = {value, 0.0, 0.0, 0.0};
    return EmitParameter(context, "Result",
                         NumericParameter(context, IrParameterKind::Scalar, defaults));
}

// RGB·R·G·B·A·RGBA 출력(UE VectorParameter 출력 마스크)을 낸다.
Result<void> EmitColorOutputs(LoweringContext& context, IrValueId vector) noexcept {
    if (vector == NoIrValue) {
        return Result<void>::Success();
    }

    static constexpr uint8 Rgb[] = {0, 1, 2};
    if (auto rgb = EmitParameter(context, "RGB", context.Swizzle(vector, Rgb)); !rgb) {
        return rgb;
    }

    static constexpr StringView Channels[MaxIrComponents] = {"R", "G", "B", "A"};
    for (uint8 index = 0; index < MaxIrComponents; ++index) {
        if (auto channel = EmitParameter(context, Channels[index],
                                         context.Swizzle(vector, Span<const uint8>(&index, 1)));
            !channel) {
            return channel;
        }
    }

    return context.Output("RGBA", vector);
}

// UE VectorParameter(MaterialExpressions.cpp:8032–8047).
Result<void> LowerVectorParameter(LoweringContext& context) noexcept {
    bool usesData = false;
    auto data = CustomPrimitiveData(context, "CustomPrimitiveData4", usesData);
    if (!data || usesData) {
        return data ? EmitColorOutputs(context, data.Value()) : Result<void>::Failure(data.Error());
    }

    float color[MaxIrComponents] = {};
    auto read = context.ReadColor("defaultValue", color);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    const double defaults[MaxIrComponents] = {color[0], color[1], color[2], color[3]};
    auto parameter = NumericParameter(context, IrParameterKind::Vector, defaults);
    if (!parameter) {
        return Result<void>::Failure(parameter.Error());
    }

    return EmitColorOutputs(context, parameter.Value());
}

// UE DoubleVectorParameter(MaterialExpressions.cpp:8200–8203): Double4, XYZ·X·Y·Z·W.
Result<void> LowerDoubleVectorParameter(LoweringContext& context) noexcept {
    double defaults[MaxIrComponents] = {};
    auto read = context.ReadDoubleVector("defaultValue", defaults);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto parameter = NumericParameter(context, IrParameterKind::DoubleVector, defaults);
    if (!parameter || parameter.Value() == NoIrValue) {
        return parameter ? Result<void>::Success() : Result<void>::Failure(parameter.Error());
    }

    static constexpr uint8 Xyz[] = {0, 1, 2};
    if (auto xyz = EmitParameter(context, "XYZ", context.Swizzle(parameter.Value(), Xyz)); !xyz) {
        return xyz;
    }

    static constexpr StringView Axes[MaxIrComponents] = {"X", "Y", "Z", "W"};
    for (uint8 index = 0; index < MaxIrComponents; ++index) {
        if (auto axis =
                EmitParameter(context, Axes[index],
                              context.Swizzle(parameter.Value(), Span<const uint8>(&index, 1)));
            !axis) {
            return axis;
        }
    }

    return Result<void>::Success();
}

// UE ForceCast(MCT_Float4, MFCF_ForceCast)다: Float로 바꾸고, 스칼라는 복제, 짧으면 0을 붙이고 길면
// 앞 넷이다.
Result<IrValueId> ForceFloat4(LoweringContext& context, IrValueId value) noexcept {
    const IrType type = context.TypeOf(value);
    if (type.IsMatrix() || type.scalar == IrScalar::Bool) {
        if (auto reported = context.Error(MaterialDiagnosticCode::TypeMismatch, "Input",
                                          "Failed to compile mask input");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    auto converted = context.Convert(value, IrScalar::Float);
    if (!converted) {
        return converted;
    }

    if (type.components == 1) {
        return context.Splat(converted.Value(), MaxIrComponents);
    }

    auto padding = context.Zero(IrFloat(static_cast<uint8>(MaxIrComponents - type.components)));
    if (!padding) {
        return padding;
    }

    return context.Append(converted.Value(), padding.Value());
}

// UE ChannelMaskParameter(MaterialExpressions.cpp:8352–8374): Dot(ForceCast(Input), Vector
// Parameter(defaultValue)). custom primitive data는 UE처럼 쓰지 않는다.
Result<void> LowerChannelMaskParameter(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("Input");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "Input", "Missing mask input");
    }

    auto input = context.Input("Input");
    if (!input || input.Value() == NoIrValue) {
        return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
    }

    auto vector = context.TypeOf(input.Value()).components == MaxIrComponents &&
                          context.TypeOf(input.Value()).scalar == IrScalar::Float
                      ? Result<IrValueId>::Success(input.Value())
                      : ForceFloat4(context, input.Value());
    if (!vector || vector.Value() == NoIrValue) {
        return vector ? Result<void>::Success() : Result<void>::Failure(vector.Error());
    }

    float color[MaxIrComponents] = {};
    auto read = context.ReadColor("defaultValue", color);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    const double defaults[MaxIrComponents] = {color[0], color[1], color[2], color[3]};
    auto parameter = NumericParameter(context, IrParameterKind::Vector, defaults);
    if (!parameter || parameter.Value() == NoIrValue) {
        return parameter ? Result<void>::Success() : Result<void>::Failure(parameter.Error());
    }

    return EmitParameter(
        context, "Result",
        context.Binary(IrOp::Dot, vector.Value(), parameter.Value(), NumberRule::FloatOnly));
}

// static Bool 파라미터 값이다(번역 입력 → 노드 defaultValue — UE StaticBoolParameter,
// HLSLMaterialTranslator.cpp:8947–8965). 읽지 못하면 found가 거짓이다.
Result<bool> StaticBoolValue(LoweringContext& context, bool& found) noexcept {
    found = false;
    Guid id;
    auto idRead = context.ReadGuid("parameterId", id);
    if (!idRead || !idRead.Value()) {
        return idRead ? Result<bool>::Success(false) : Result<bool>::Failure(idRead.Error());
    }

    bool value = false;
    auto read = context.ReadBool("defaultValue", false, value);
    if (!read || !read.Value()) {
        return read ? Result<bool>::Success(false) : Result<bool>::Failure(read.Error());
    }

    found = true;
    if (const MaterialStaticParameterValue* overridden =
            context.FindStaticParameter(id, MaterialStaticParameterKind::Bool)) {
        value = overridden->value;
    }

    return Result<bool>::Success(value);
}

Result<void> LowerStaticBoolParameter(LoweringContext& context) noexcept {
    bool found = false;
    auto value = StaticBoolValue(context, found);
    if (!value || !found) {
        return value ? Result<void>::Success() : Result<void>::Failure(value.Error());
    }

    const bool bit = value.Value();
    return EmitParameter(context, "Result",
                         context.Builder().BoolConstant(Span<const bool>(&bit, 1)));
}

// UE StaticSwitchParameter(MaterialExpressions.cpp:8644–8697): A·B 둘 다 연결이 필요하고 고른
// 가지만 번역한다(늦은 입력).
Result<void> LowerStaticSwitchParameter(LoweringContext& context) noexcept {
    bool found = false;
    auto value = StaticBoolValue(context, found);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    const StringView keys[] = {"A", "B"};
    const StringView messages[] = {"Missing A input", "Missing B input"};
    bool both = true;
    for (usize index = 0; index < 2; ++index) {
        auto connected = context.IsConnected(keys[index]);
        if (!connected) {
            return Result<void>::Failure(connected.Error());
        }

        if (!connected.Value()) {
            both = false;
            if (auto reported = context.Error(MaterialDiagnosticCode::MissingInput, keys[index],
                                              messages[index]);
                !reported) {
                return reported;
            }
        }
    }

    if (!both || !found) {
        return Result<void>::Success();
    }

    return EmitParameter(context, "Result", context.Input(value.Value() ? keys[0] : keys[1]));
}

// UE StaticComponentMaskParameter(MaterialExpressions.cpp:5465–5482, HLSLMaterialTranslator.cpp:
// 8967–8991): static mask(없으면 defaultR·G·B·A)로 ComponentMask와 같은 성분 선택이다.
Result<void> LowerStaticComponentMaskParameter(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("Input");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "Input",
                             "Missing ComponentMaskParameter input");
    }

    auto input = context.Input("Input");
    if (!input || input.Value() == NoIrValue) {
        return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
    }

    Guid id;
    auto idRead = context.ReadGuid("parameterId", id);
    if (!idRead || !idRead.Value()) {
        return idRead ? Result<void>::Success() : Result<void>::Failure(idRead.Error());
    }

    static constexpr StringView Fields[MaxIrComponents] = {"defaultR", "defaultG", "defaultB",
                                                           "defaultA"};
    bool mask[MaxIrComponents] = {false, false, false, false};
    for (usize index = 0; index < MaxIrComponents; ++index) {
        auto read = context.ReadBool(Fields[index], false, mask[index]);
        if (!read || !read.Value()) {
            return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
        }
    }

    if (const MaterialStaticParameterValue* overridden =
            context.FindStaticParameter(id, MaterialStaticParameterKind::ComponentMask)) {
        for (usize index = 0; index < MaxIrComponents; ++index) {
            mask[index] = overridden->mask[index];
        }
    }

    uint8 selected[MaxIrComponents] = {0, 0, 0, 0};
    uint8 count = 0;
    for (uint8 index = 0; index < MaxIrComponents; ++index) {
        if (mask[index]) {
            selected[count++] = index;
        }
    }

    // ComponentMask와 같은 검사다(HLSLMaterialTranslator.cpp:10776–10817).
    const IrType type = context.TypeOf(input.Value());
    if (count == 0) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(),
                             "Component mask selects no components");
    }

    if (type.IsMatrix() || (type.components > 1 && selected[count - 1] >= type.components)) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "Input",
                             "Not enough components in the input for the component mask");
    }

    if (type.components == 1) {
        for (uint8 index = 0; index < count; ++index) {
            selected[index] = 0;
        }
    }

    return EmitParameter(context, "Result",
                         context.Swizzle(input.Value(), Span<const uint8>(selected, count)));
}

// 컬렉션 파라미터 자리 찾기 결과다.
struct CollectionSlot {
    const MaterialCollectionLayout* layout = nullptr;
    const MaterialCollectionEntry* entry = nullptr;
    Guid asset;
};

// collection·parameterId 속성으로 layout과 항목을 찾는다(UE GetParameterIndex). 없으면 slot의
// 해당 칸이 nullptr이다. 속성을 읽지 못하면 read가 거짓이다(진단은 이미 했다).
Result<void> FindCollectionSlot(LoweringContext& context, CollectionSlot& slot,
                                bool& read) noexcept {
    read = false;
    auto assetRead = context.ReadAssetReference("collection", slot.asset);
    if (!assetRead || !assetRead.Value()) {
        return assetRead ? Result<void>::Success() : Result<void>::Failure(assetRead.Error());
    }

    Guid id;
    auto idRead = context.ReadGuid("parameterId", id);
    if (!idRead || !idRead.Value()) {
        return idRead ? Result<void>::Success() : Result<void>::Failure(idRead.Error());
    }

    read = true;
    slot.layout = slot.asset == Guid{} ? nullptr : context.FindCollection(slot.asset);
    if (slot.layout == nullptr) {
        return Result<void>::Success();
    }

    for (const MaterialCollectionEntry& entry : slot.layout->entries) {
        const bool placed =
            entry.vector < slot.layout->vectors &&
            (entry.component == MaterialCollectionWholeVector || entry.component < MaxIrComponents);
        if (entry.parameterId == id && placed) {
            slot.entry = &entry;
            break;
        }
    }

    return Result<void>::Success();
}

// UE CollectionParameter(MaterialExpressions.cpp:17179–17204, HLSLMaterialTranslator.cpp:
// 5058–5085).
Result<void> LowerParameterCollectionValue(LoweringContext& context) noexcept {
    CollectionSlot slot;
    bool read = false;
    if (auto found = FindCollectionSlot(context, slot, read); !found || !read) {
        return found;
    }

    if (slot.entry == nullptr) {
        if (slot.layout == nullptr) {
            return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                 "CollectionParameter has invalid Collection!");
        }

        return NamedParameterError(context, "CollectionParameter has invalid parameter ", "");
    }

    auto admitted = context.UseCollection(slot.asset);
    if (!admitted || !admitted.Value()) {
        return admitted ? Result<void>::Success() : Result<void>::Failure(admitted.Error());
    }

    auto vector =
        context.Builder().CollectionValue(slot.asset, slot.entry->vector, slot.layout->vectors);
    if (!vector) {
        return Result<void>::Failure(vector.Error());
    }

    if (slot.entry->component == MaterialCollectionWholeVector) {
        return context.Output("Result", vector.Value());
    }

    const uint8 component = slot.entry->component;
    return EmitParameter(context, "Result",
                         context.Swizzle(vector.Value(), Span<const uint8>(&component, 1)));
}

// value(Float3·Float4)와 행 rows의 행 벡터 곱 합이다(HLSL mul(v, float4x4(rows…))). homogeneous면
// 3성분 입력의 w를 1로 본다(마지막 행을 더한다).
Result<IrValueId> RowProduct(LoweringContext& context, IrValueId value, Span<const IrValueId> rows,
                             bool homogeneous) noexcept {
    const uint8 components = context.TypeOf(value).components;
    IrValueId sum = NoIrValue;
    for (uint8 index = 0; index < components && index < rows.Size(); ++index) {
        auto axis = context.Swizzle(value, Span<const uint8>(&index, 1));
        if (!axis) {
            return axis;
        }

        auto term =
            context.Binary(IrOp::Multiply, axis.Value(), rows[index], NumberRule::FloatOnly);
        if (!term) {
            return term;
        }

        if (sum == NoIrValue) {
            sum = term.Value();
            continue;
        }

        auto added = context.Binary(IrOp::Add, sum, term.Value(), NumberRule::FloatOnly);
        if (!added) {
            return added;
        }

        sum = added.Value();
    }

    if (homogeneous && components < rows.Size()) {
        return context.Binary(IrOp::Add, sum, rows[components], NumberRule::FloatOnly);
    }

    return Result<IrValueId>::Success(sum);
}

// tile 벡터(r4.xyz)의 Double 값(tile × tile 크기)이다. width가 4면 w는 0이다.
Result<IrValueId> TileOffset(LoweringContext& context, IrValueId row, uint8 width) noexcept {
    static constexpr uint8 Xyz[] = {0, 1, 2};
    auto tile = context.Swizzle(row, Xyz);
    if (!tile) {
        return tile;
    }

    IrValueId wide = tile.Value();
    if (width == MaxIrComponents) {
        auto zero = context.Zero(IrFloat(1));
        if (!zero) {
            return zero;
        }

        auto appended = context.Append(wide, zero.Value());
        if (!appended) {
            return appended;
        }

        wide = appended.Value();
    }

    auto promoted = context.Convert(wide, IrScalar::Double);
    if (!promoted) {
        return promoted;
    }

    auto size = context.DoubleConstant(CollectionTileSize, StringView());
    if (!size) {
        return size;
    }

    return context.Binary(IrOp::Multiply, promoted.Value(), size.Value(), NumberRule::Arithmetic);
}

// UE CollectionTransform(MaterialExpressions.cpp:17327–17410, HLSLMaterialTranslator.cpp:
// 5087–5153). 행렬 곱은 행 벡터 합으로 IR에 펼친다.
Result<void> LowerParameterCollectionTransform(LoweringContext& context) noexcept {
    auto connected = context.IsConnected("Input");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "Input",
                             "CollectionTransform missing input");
    }

    auto input = context.Input("Input");
    if (!input || input.Value() == NoIrValue) {
        return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
    }

    const IrType type = context.TypeOf(input.Value());
    if (type.IsMatrix() || (type.components != 3 && type.components != MaxIrComponents) ||
        (type.scalar != IrScalar::Float && type.scalar != IrScalar::Double)) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "Input",
                             "CollectionTransform requires vector input");
    }

    CollectionSlot slot;
    bool read = false;
    if (auto found = FindCollectionSlot(context, slot, read); !found || !read) {
        return found;
    }

    if (slot.layout == nullptr) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "CollectionTransform has invalid Collection!");
    }

    if (slot.entry == nullptr) {
        return NamedParameterError(context, "CollectionTransform has invalid parameter ", "");
    }

    if (slot.entry->component != MaterialCollectionWholeVector) {
        return NamedParameterError(context, "CollectionTransform parameter ",
                                   " is scalar, vectors are required");
    }

    int64 kindValue = 0;
    auto kindRead = context.ReadInteger(
        "transformType", static_cast<int64>(CollectionTransformType::Position), kindValue);
    if (!kindRead || !kindRead.Value()) {
        return kindRead ? Result<void>::Success() : Result<void>::Failure(kindRead.Error());
    }

    const CollectionTransformType kind = static_cast<CollectionTransformType>(kindValue);
    const bool largeWorld = kind == CollectionTransformType::LocalToWorld ||
                            kind == CollectionTransformType::WorldToLocal;
    const uint32 needed = largeWorld                                ? LargeWorldMatrixVectors
                          : kind == CollectionTransformType::Vector ? DirectionMatrixVectors
                                                                    : PositionMatrixVectors;
    if (slot.entry->vector + needed > slot.layout->vectors) {
        const StringView suffix =
            largeWorld ? StringView(" requires 5 vectors for LWC Matrix")
            : kind == CollectionTransformType::Vector
                ? StringView(" requires 3 vectors for Vector matrix")
                : StringView(" requires 4 vectors for Position or Projection matrix");
        return NamedParameterError(context, "CollectionTransform parameter ", suffix);
    }

    auto admitted = context.UseCollection(slot.asset);
    if (!admitted || !admitted.Value()) {
        return admitted ? Result<void>::Success() : Result<void>::Failure(admitted.Error());
    }

    IrValueId rows[LargeWorldMatrixVectors] = {NoIrValue, NoIrValue, NoIrValue, NoIrValue,
                                               NoIrValue};
    for (uint32 index = 0; index < needed; ++index) {
        auto row = context.Builder().CollectionValue(slot.asset, slot.entry->vector + index,
                                                     slot.layout->vectors);
        if (!row) {
            return Result<void>::Failure(row.Error());
        }

        rows[index] = row.Value();
    }

    const Span<const IrValueId> matrix(rows, PositionMatrixVectors);
    static constexpr uint8 Xyz[] = {0, 1, 2};
    switch (kind) {
    case CollectionTransformType::Vector: {
        // (mul(float4(WSDemote(in).xyz, 0), float4x4(r0, r1, r2, 0))).xyz
        auto demoted = context.Convert(input.Value(), IrScalar::Float);
        if (!demoted) {
            return Result<void>::Failure(demoted.Error());
        }

        auto direction = context.Swizzle(demoted.Value(), Xyz);
        if (!direction) {
            return Result<void>::Failure(direction.Error());
        }

        auto product = RowProduct(context, direction.Value(),
                                  Span<const IrValueId>(rows, DirectionMatrixVectors), false);
        if (!product || product.Value() == NoIrValue) {
            return product ? Result<void>::Success() : Result<void>::Failure(product.Error());
        }

        return EmitParameter(context, "Result", context.Swizzle(product.Value(), Xyz));
    }
    case CollectionTransformType::Projection: {
        // WSDemote(in).xyzz * float4(r0.x, r1.y, r2.zw) + float4(0, 0, r3.zw)
        auto demoted = context.Convert(input.Value(), IrScalar::Float);
        if (!demoted) {
            return Result<void>::Failure(demoted.Error());
        }

        static constexpr uint8 Xyzz[] = {0, 1, 2, 2};
        static constexpr uint8 Picks[][2] = {{0, 0}, {1, 1}, {2, 3}, {2, 3}};
        static constexpr uint8 PickCounts[] = {1, 1, 2, 2};
        auto spread = context.Swizzle(demoted.Value(), Xyzz);
        if (!spread) {
            return Result<void>::Failure(spread.Error());
        }

        IrValueId parts[PositionMatrixVectors] = {NoIrValue, NoIrValue, NoIrValue, NoIrValue};
        for (usize index = 0; index < PositionMatrixVectors; ++index) {
            auto part =
                context.Swizzle(rows[index], Span<const uint8>(Picks[index], PickCounts[index]));
            if (!part) {
                return Result<void>::Failure(part.Error());
            }

            parts[index] = part.Value();
        }

        auto zeros = context.Zero(IrFloat(2));
        if (!zeros) {
            return Result<void>::Failure(zeros.Error());
        }

        auto front = context.Append(parts[0], parts[1]);
        if (!front) {
            return Result<void>::Failure(front.Error());
        }

        auto scale = context.Append(front.Value(), parts[2]);
        if (!scale) {
            return Result<void>::Failure(scale.Error());
        }

        auto bias = context.Append(zeros.Value(), parts[3]);
        if (!bias) {
            return Result<void>::Failure(bias.Error());
        }

        auto scaled =
            context.Binary(IrOp::Multiply, spread.Value(), scale.Value(), NumberRule::FloatOnly);
        if (!scaled) {
            return Result<void>::Failure(scaled.Error());
        }

        return EmitParameter(
            context, "Result",
            context.Binary(IrOp::Add, scaled.Value(), bias.Value(), NumberRule::FloatOnly));
    }
    case CollectionTransformType::LocalToWorld: {
        // WSMultiply(WSDemote(in), MakeWSMatrix(r4.xyz, M)) = Double(in × M) + Double(r4) × tile.
        auto demoted = context.Convert(input.Value(), IrScalar::Float);
        if (!demoted) {
            return Result<void>::Failure(demoted.Error());
        }

        auto product = RowProduct(context, demoted.Value(), matrix, true);
        if (!product || product.Value() == NoIrValue) {
            return product ? Result<void>::Success() : Result<void>::Failure(product.Error());
        }

        auto local = type.components == 3 ? context.Swizzle(product.Value(), Xyz)
                                          : Result<IrValueId>::Success(product.Value());
        if (!local) {
            return Result<void>::Failure(local.Error());
        }

        auto offset = context.Convert(local.Value(), IrScalar::Double);
        auto tile = TileOffset(context, rows[MaxIrComponents], type.components);
        if (!offset || !tile) {
            return Result<void>::Failure(!offset ? offset.Error() : tile.Error());
        }

        return EmitParameter(
            context, "Result",
            context.Binary(IrOp::Add, tile.Value(), offset.Value(), NumberRule::Arithmetic));
    }
    case CollectionTransformType::WorldToLocal: {
        // WSMultiply(WSPromote(in), MakeWSInverseMatrix(r4.xyz, M)) = Float(in − Double(r4) ×
        // tile) × M(Float — 같은 타입 Double 뺄셈은 Float로 자른다, 5.3절).
        auto promoted = context.Convert(input.Value(), IrScalar::Double);
        auto tile = TileOffset(context, rows[MaxIrComponents], type.components);
        if (!promoted || !tile) {
            return Result<void>::Failure(!promoted ? promoted.Error() : tile.Error());
        }

        auto relative =
            context.Binary(IrOp::Subtract, promoted.Value(), tile.Value(), NumberRule::Arithmetic);
        if (!relative || relative.Value() == NoIrValue) {
            return relative ? Result<void>::Success() : Result<void>::Failure(relative.Error());
        }

        auto local = context.Convert(relative.Value(), IrScalar::Float);
        if (!local) {
            return Result<void>::Failure(local.Error());
        }

        auto product = RowProduct(context, local.Value(), matrix, true);
        if (!product || product.Value() == NoIrValue) {
            return product ? Result<void>::Success() : Result<void>::Failure(product.Error());
        }

        return EmitParameter(context, "Result",
                             type.components == 3 ? context.Swizzle(product.Value(), Xyz)
                                                  : Result<IrValueId>::Success(product.Value()));
    }
    case CollectionTransformType::Position:
    default: {
        // Float3: (mul(float4(WSDemote(in), 1), M)).xyz, Float4: mul(WSDemote(in), M).
        auto demoted = context.Convert(input.Value(), IrScalar::Float);
        if (!demoted) {
            return Result<void>::Failure(demoted.Error());
        }

        auto product = RowProduct(context, demoted.Value(), matrix, true);
        if (!product || product.Value() == NoIrValue) {
            return product ? Result<void>::Success() : Result<void>::Failure(product.Error());
        }

        return EmitParameter(context, "Result",
                             type.components == 3 ? context.Swizzle(product.Value(), Xyz)
                                                  : Result<IrValueId>::Success(product.Value()));
    }
    }
}

constexpr LoweringEntry ParameterEntries[] = {
    {"Material.ScalarParameter", &LowerScalarParameter},
    {"Material.VectorParameter", &LowerVectorParameter},
    {"Material.DoubleVectorParameter", &LowerDoubleVectorParameter},
    {"Material.ChannelMaskParameter", &LowerChannelMaskParameter},
    {"Material.StaticBoolParameter", &LowerStaticBoolParameter},
    {"Material.StaticSwitchParameter", &LowerStaticSwitchParameter, true},
    {"Material.StaticComponentMaskParameter", &LowerStaticComponentMaskParameter},
    {"Material.ParameterCollectionValue", &LowerParameterCollectionValue},
    {"Material.ParameterCollectionTransform", &LowerParameterCollectionTransform},
};

} // namespace

Span<const LoweringEntry> ParameterLowerings() noexcept {
    return ParameterEntries;
}

} // namespace rocks::material::detail
