#include "LoweringContext.h"

#include <CoreTypeBindings/CoreValueTypes.generated.h>
#include <CoreTypeBindings/CoreValueTypes.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <MaterialSchema/MaterialAssetReference.generated.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/MaterialPinValue.generated.h>
#include <MaterialSchema/MaterialPinValue.h>
#include <Platform/Memory.h>
#include <MaterialSchema/MaterialAttributeTable.h>
#include <RenderData/MaterialEnums.h>

#include "Translator.h"

// lowering 문맥이다(Docs/Design/MaterialSystem 6.8절 "입력 값과 변환").
namespace rocks::material::detail {

namespace {

// 설정되지 않은 Displacement 칸의 값이다(UE CompileDefaultValue −1 — "연결 없음" 표지, 6.12절).
constexpr float LoweringDisplacementUnset = -1.0f;

// 즉시값 bit에서 float다(IR 상수 성분 — 6.8절 "값").
float LoweringFloatBits(uint64 bits) noexcept {
    const uint32 narrow = static_cast<uint32>(bits);
    float value = 0.0f;
    platform::MemoryCopy(&value, &narrow, sizeof(value));
    return value;
}

// NaN·Inf가 아닌가다(V11).
bool LoweringIsFinite(float value) noexcept {
    return value == value && value - value == 0.0f;
}

bool LoweringIsFinite(double value) noexcept {
    return value == value && value - value == 0.0;
}

} // namespace

bool LoweringPinIrType(MaterialPinType pin, IrType& out) noexcept {
    const uint32 value = static_cast<uint32>(pin);
    if (value >= static_cast<uint32>(MaterialPinType::Float1) &&
        value <= static_cast<uint32>(MaterialPinType::Float4)) {
        out = IrFloat(static_cast<uint8>(value - static_cast<uint32>(MaterialPinType::Float1) + 1));
        return true;
    }

    if (value >= static_cast<uint32>(MaterialPinType::Int1) &&
        value <= static_cast<uint32>(MaterialPinType::Int4)) {
        out = IrInt(static_cast<uint8>(value - static_cast<uint32>(MaterialPinType::Int1) + 1));
        return true;
    }

    if (value >= static_cast<uint32>(MaterialPinType::UInt1) &&
        value <= static_cast<uint32>(MaterialPinType::UInt4)) {
        out = IrUInt(static_cast<uint8>(value - static_cast<uint32>(MaterialPinType::UInt1) + 1));
        return true;
    }

    if (value >= static_cast<uint32>(MaterialPinType::Double1) &&
        value <= static_cast<uint32>(MaterialPinType::Double4)) {
        out =
            IrDouble(static_cast<uint8>(value - static_cast<uint32>(MaterialPinType::Double1) + 1));
        return true;
    }

    if (pin == MaterialPinType::Bool || pin == MaterialPinType::StaticBool) {
        out = IrBool(1);
        return true;
    }

    return false;
}

StringView LoweringTypeName(IrType type) noexcept {
    static constexpr StringView Floats[] = {"float", "float2", "float3", "float4"};
    static constexpr StringView Ints[] = {"int", "int2", "int3", "int4"};
    static constexpr StringView UInts[] = {"uint", "uint2", "uint3", "uint4"};
    static constexpr StringView Bools[] = {"bool", "bool2", "bool3", "bool4"};
    static constexpr StringView Doubles[] = {"double", "double2", "double3", "double4"};
    if (type.IsMatrix()) {
        return type.rows == 3 ? StringView("float3x3") : StringView("float4x4");
    }

    if (type.IsSurface()) {
        return "surface";
    }

    if (type.IsTexture()) {
        // UE DescribeType 이름이다.
        switch (type.texture) {
        case IrTextureKind::TextureCube:
            return "textureCube";
        case IrTextureKind::Texture2DArray:
            return "texture2DArray";
        case IrTextureKind::TextureCubeArray:
            return "textureCubeArray";
        case IrTextureKind::Texture3D:
            return "volumeTexture";
        case IrTextureKind::External:
            return "TextureExternal";
        default:
            return "texture2D";
        }
    }

    // 속성 집합·표면은 UE DescribeType 이름이다(HLSLMaterialTranslator.cpp:3132·3143 — 6.12절).
    if (type.scalar == IrScalar::Attributes) {
        return "MaterialAttributes";
    }

    if (type.scalar == IrScalar::Surface) {
        return "Substrate";
    }

    if (type.components < 1 || type.components > MaxIrComponents) {
        return "unknown";
    }

    const usize index = type.components - 1;
    switch (type.scalar) {
    case IrScalar::Texture:
    case IrScalar::Surface:
    case IrScalar::Attributes:
        // 위에서 돌려준다.
        break;
    case IrScalar::Float:
        return Floats[index];
    case IrScalar::Int:
        return Ints[index];
    case IrScalar::UInt:
        return UInts[index];
    case IrScalar::Bool:
        return Bools[index];
    case IrScalar::Double:
        return Doubles[index];
    }

    return "unknown";
}

LoweringContext::LoweringContext(Translator& translator, const nodegraph::GraphNode& node,
                                 const MaterialNodeDefinition& definition) noexcept
    : translator_(translator), node_(node), definition_(definition), dynamicPins_(scratch_) {
}

EvaluationContext LoweringContext::Context() const noexcept {
    return translator_.Context();
}

const MaterialTraitsDesc& LoweringContext::Traits() const noexcept {
    return translator_.Input().traits;
}

const MaterialTranslationTarget& LoweringContext::Target(MaterialTargetField field) noexcept {
    return translator_.Target(field);
}

const MaterialStaticParameterValue*
LoweringContext::FindStaticParameter(Guid id, MaterialStaticParameterKind kind) const noexcept {
    for (const MaterialStaticParameterValue& value : translator_.Input().staticParameters) {
        if (value.parameterId == id && value.kind == kind) {
            return &value;
        }
    }

    return nullptr;
}

const MaterialCollectionLayout* LoweringContext::FindCollection(Guid asset) const noexcept {
    for (const MaterialCollectionLayout& layout : translator_.Input().collections) {
        if (layout.asset == asset) {
            return &layout;
        }
    }

    return nullptr;
}

Result<IrValueId> LoweringContext::CoerceToWidth(IrValueId value, uint8 width,
                                                 StringView pin) noexcept {
    const MaterialPinType types[] = {MaterialPinType::Float1, MaterialPinType::Float2,
                                     MaterialPinType::Float3, MaterialPinType::Float4};
    if (width == 0 || width > MaxIrComponents) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    return Coerce(value, types[width - 1], pin);
}

Result<IrValueId> LoweringContext::MakeAttributes(Span<const AttributeEntry> entries) noexcept {
    return translator_.AddAttributes(entries);
}

Result<bool> LoweringContext::CopyAttributes(IrValueId value,
                                             Array<AttributeEntry>& out) const noexcept {
    return translator_.CopyAttributes(value, out);
}

const MaterialAttributeDefinition* LoweringContext::FindAttribute(Guid id) const noexcept {
    return translator_.FindAttribute(id);
}

const MaterialAttributeDefinition*
LoweringContext::FindAttributeByName(StringView name) const noexcept {
    return translator_.FindAttributeByName(name);
}

Result<const MaterialAttributeDefinition*> LoweringContext::FindAttributeByKey(Name key) noexcept {
    // 키 = 속성 이름이다(내장 → 등록 표 — 6.12절 MS2e1 Aggregate).
    auto text = translator_.Names().Acquire(key);
    if (!text) {
        return Result<const MaterialAttributeDefinition*>::Failure(text.Error());
    }

    return Result<const MaterialAttributeDefinition*>::Success(
        FindAttributeByName(text.Value().View()));
}

Result<IrValueId>
LoweringContext::AttributeDefault(const MaterialAttributeDefinition& definition) noexcept {
    // UE FMaterialAttributeDefinitionMap::CompileDefaultValue(MaterialAttributeDefinitionMap.cpp:
    // 46–109).
    const StringView name = definition.name;
    if (definition.type == MaterialPinType::Surface) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (name == StringView("Normal") || name == StringView("Tangent")) {
        if (!Traits().tangentSpaceNormal) {
            return ContextValue(name == StringView("Normal") ? StringView("VertexNormal")
                                                             : StringView("VertexTangent"));
        }
    }

    if (definition.type == MaterialPinType::ShadingModel) {
        // Rocks 머티리얼 속성에는 shading model이 없으므로 DefaultLit다(UE 첫 shading model).
        const uint32 number = static_cast<uint32>(rocks::ShadingModel::DefaultLit);
        return Builder().UIntConstant(Span<const uint32>(&number, 1));
    }

    if (name == StringView("Displacement")) {
        // UE는 −1로 "연결 없음"을 표시한다(:88–92).
        return FloatScalar(LoweringDisplacementUnset);
    }

    constexpr StringView CustomizedPrefix = "CustomizedUV";
    if (name.ByteCount() == CustomizedPrefix.ByteCount() + 1 &&
        StringView(name.Data(), CustomizedPrefix.ByteCount()) == CustomizedPrefix) {
        const uint32 index = static_cast<uint32>(name.Data()[CustomizedPrefix.ByteCount()] - '0');
        auto slot = Builder().UIntConstant(Span<const uint32>(&index, 1));
        if (!slot) {
            return slot;
        }

        const IrValueId arguments[] = {slot.Value()};
        return ContextValue("TexCoord", arguments);
    }

    IrType type;
    if (!LoweringPinIrType(definition.type, type)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    float values[MaxIrComponents] = {};
    for (uint8 index = 0; index < type.components && index < MaxIrComponents; ++index) {
        values[index] = index < definition.defaultValue.componentCount
                            ? definition.defaultValue.values[index]
                            : 0.0f;
    }

    return FloatConstant(Span<const float>(values, type.components), StringView());
}

Result<IrValueId>
LoweringContext::AttributeValue(Span<const AttributeEntry> entries,
                                const MaterialAttributeDefinition& definition) noexcept {
    for (const AttributeEntry& entry : entries) {
        if (entry.id == definition.id) {
            return Result<IrValueId>::Success(entry.value);
        }
    }

    return AttributeDefault(definition);
}

Result<IrValueId> LoweringContext::FitAttribute(IrValueId value,
                                                const MaterialAttributeDefinition& definition,
                                                StringView pin) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    const IrType type = TypeOf(value);
    if (definition.type == MaterialPinType::Surface || type.IsSurface()) {
        if (definition.type == MaterialPinType::Surface && type.IsSurface()) {
            return Result<IrValueId>::Success(value);
        }

        if (auto reported = TypeError("Surface value does not fit this attribute", type, type);
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    if (definition.type == MaterialPinType::ShadingModel) {
        if (type == IrUInt(1)) {
            return Result<IrValueId>::Success(value);
        }

        if (auto reported =
                TypeError("Shading model attribute needs a shading model value", type, IrUInt(1));
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    return Coerce(value, definition.type, pin);
}

bool LoweringContext::InVirtualTextureScope() const noexcept {
    return translator_.VirtualTextureScope();
}

Result<IrValueId> LoweringContext::MakeSurface(SurfaceRecord record) noexcept {
    record.node = node_.id;
    return translator_.AddSurface(record);
}

const SurfaceRecord* LoweringContext::SurfaceOf(IrValueId value) const noexcept {
    return translator_.FindSurface(value);
}

Result<void> LoweringContext::DeferError(IrValueId value, MaterialDiagnosticCode code,
                                         StringView message) noexcept {
    return translator_.DeferError(value, code, node_.id, message);
}

Result<bool> LoweringContext::UseUserSceneTexture(StringView name, uint32& index) noexcept {
    auto admitted = translator_.ReferenceUserSceneTexture(name, index);
    if (!admitted || admitted.Value()) {
        return admitted;
    }

    StringBuilder<> message(scratch_);
    message << "Too many unique UserSceneTexture inputs in the post process material -- max "
               "allowed is "
            << MaxUserSceneTextures;
    if (auto status = message.Status(); !status) {
        return Result<bool>::Failure(status.Error());
    }

    if (auto reported = Error(MaterialDiagnosticCode::BudgetExceeded, StringView(), message.View());
        !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

const MaterialTextureFacts* LoweringContext::FindTexture(Guid asset) const noexcept {
    for (const MaterialTextureFacts& facts : translator_.Input().textures) {
        if (facts.asset == asset) {
            return &facts;
        }
    }

    return nullptr;
}

const MaterialSourceProvider* LoweringContext::Sources() const noexcept {
    return translator_.Input().sources;
}

Result<const MaterialColorCurveSnapshot*> LoweringContext::FindColorCurve(Guid asset) noexcept {
    const MaterialSourceProvider* sources = translator_.Input().sources;
    const MaterialColorCurveSnapshot* snapshot =
        sources == nullptr || asset == Guid{} ? nullptr : sources->FindColorCurve(asset);
    if (snapshot == nullptr) {
        return Result<const MaterialColorCurveSnapshot*>::Success(nullptr);
    }

    if (auto added = translator_.AddDependency(MaterialDependencyRole::ColorCurve, asset); !added) {
        return Result<const MaterialColorCurveSnapshot*>::Failure(added.Error());
    }

    return Result<const MaterialColorCurveSnapshot*>::Success(snapshot);
}

Result<bool> LoweringContext::UseSampler(IrTextureSource source, Guid id) noexcept {
    uint32 count = 0;
    auto admitted = translator_.ReferenceSampler(static_cast<uint8>(source), id, count);
    if (!admitted || admitted.Value()) {
        return admitted;
    }

    const uint32 limit = translator_.Target(MaterialTargetField::MaxSamplers).maxSamplers;
    StringBuilder<> message(scratch_);
    message << "Material uses too many samplers (" << count << ", limit " << limit << ")";
    if (auto status = message.Status(); !status) {
        return Result<bool>::Failure(status.Error());
    }

    if (auto reported = Error(MaterialDiagnosticCode::BudgetExceeded, StringView(), message.View());
        !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

Result<bool> LoweringContext::UseCollection(Guid asset) noexcept {
    auto admitted = translator_.ReferenceCollection(asset);
    if (!admitted || admitted.Value()) {
        return admitted;
    }

    if (auto reported = Error(MaterialDiagnosticCode::BudgetExceeded, StringView(),
                              "Material references too many MaterialParameterCollections!  A "
                              "material may only reference 2 different collections.");
        !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

Result<IrValueId> LoweringContext::ContextValue(StringView name,
                                                Span<const IrValueId> arguments) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    usize index = table.Size();
    for (usize at = 0; at < table.Size(); ++at) {
        if (table[at].name == name) {
            index = at;
            break;
        }
    }

    if (index == table.Size() || arguments.Size() != table[index].argumentCount) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    for (IrValueId argument : arguments) {
        if (argument == NoIrValue) {
            return Result<IrValueId>::Success(NoIrValue);
        }
    }

    // 텍스처 값은 문맥 값 인자가 될 수 없다(V1 — Numeric 핀에 텍스처를 이은 경우, 6.10절 MS2c3).
    for (IrValueId argument : arguments) {
        if (auto refused = RejectTexture(argument, textureRefused_); !refused || textureRefused_) {
            return refused;
        }
    }

    const MaterialContextValue& value = table[index];
    IrType type;
    if (value.type == MaterialPinType::Float3x3) {
        type = IrFloatMatrix(3, 3);
    } else if (value.type == MaterialPinType::Float4x4) {
        type = IrFloatMatrix(4, 4);
    } else if (!LoweringPinIrType(value.type, type)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    // 빈도(6.9절): 정점 기원 값은 Vertex, 픽셀 단계 전용 값은 Pixel, 그 밖은 그리기·view 단위
    // Instance다(상수 식으로 끌어올리지 않는다).
    const bool vertexOrigin = value.feature == MaterialContextFeature::Vertex ||
                              value.feature == MaterialContextFeature::Skin ||
                              value.feature == MaterialContextFeature::Particle ||
                              value.feature == MaterialContextFeature::SpeedTree;
    IrFrequency frequency = IrFrequency::Instance;
    if ((value.stages & MaterialVertexStage) == 0) {
        frequency = IrFrequency::Pixel;
    } else if (vertexOrigin) {
        frequency = IrFrequency::Vertex;
    }

    if (translator_.Context() == EvaluationContext::Pixel && value.name == StringView("TexCoord") &&
        arguments.Size() == 1) {
        const IrValue& slot = Builder().Value(arguments[0]);
        if (slot.op == IrOp::Constant) {
            translator_.NotePixelTexCoord(static_cast<uint32>(slot.immediate[0]));
        }
    }

    const IrExternVariant variant =
        translator_.Context() == EvaluationContext::VertexPrevious && value.hasPrevious
            ? IrExternVariant::Previous
            : IrExternVariant::Current;
    return Builder().Extern(static_cast<uint32>(index), variant, type, frequency, arguments);
}

IrBuilder& LoweringContext::Builder() noexcept {
    // 입력을 lowering하는 동안 다른 노드가 원인 위치를 바꾸므로 부를 때마다 이 노드로 둔다.
    IrBuilder& builder = translator_.Builder();
    builder.SetSite(IrSite{node_.id, StringView()});
    return builder;
}

NamePool& LoweringContext::Names() noexcept {
    return translator_.Names();
}

Result<IrValueId> LoweringContext::InputInContext(EvaluationContext context,
                                                  StringView key) noexcept {
    auto name = PinName(key);
    if (!name) {
        return Result<IrValueId>::Failure(name.Error());
    }

    bool connected = false;
    auto value = translator_.LowerInContext(context, node_, name.Value(), connected);
    if (value && connected && value.Value() == NoIrValue) {
        poisoned_ = true;
    }

    return value;
}

Result<IrValueId> LoweringContext::NamedDynamicInput(StringView key,
                                                     Optional<float> fallback) noexcept {
    auto name = PinName(key);
    if (!name) {
        return Result<IrValueId>::Failure(name.Error());
    }

    return DynamicInput(name.Value(), fallback, false);
}

Result<Name> LoweringContext::PinName(StringView key) noexcept {
    return translator_.Names().TryIntern(key);
}

const MaterialPinDefinition* LoweringContext::FindPin(StringView key) const noexcept {
    for (const MaterialPinDefinition& pin : definition_.pins) {
        if (pin.direction == nodegraph::PinDirection::Input && pin.key == key) {
            return &pin;
        }
    }

    return nullptr;
}

Result<void> LoweringContext::Diagnose(nodegraph::GraphDiagnosticSeverity severity,
                                       MaterialDiagnosticCode code, StringView pin,
                                       StringView message) noexcept {
    Name name;
    if (!pin.IsEmpty()) {
        auto interned = PinName(pin);
        if (!interned) {
            return Result<void>::Failure(interned.Error());
        }

        name = interned.Value();
    }

    return translator_.Report(severity, code, node_.id, name, message);
}

Result<void> LoweringContext::Error(MaterialDiagnosticCode code, StringView pin,
                                    StringView message) noexcept {
    return Diagnose(nodegraph::GraphDiagnosticSeverity::Error, code, pin, message);
}

Result<void> LoweringContext::Warning(MaterialDiagnosticCode code, StringView pin,
                                      StringView message) noexcept {
    return Diagnose(nodegraph::GraphDiagnosticSeverity::Warning, code, pin, message);
}

Result<void> LoweringContext::Info(MaterialDiagnosticCode code, StringView pin,
                                   StringView message) noexcept {
    return Diagnose(nodegraph::GraphDiagnosticSeverity::Info, code, pin, message);
}

Result<void> LoweringContext::TypeError(StringView message, IrType left, IrType right) noexcept {
    // 문구: "<message> (<left>, <right>)" — UE "Arithmetic between types … are undefined"와 같은
    // 뜻.
    String text(scratch_);
    Result<void> built = text.TryAssign(message);
    const StringView parts[] = {" (", LoweringTypeName(left), ", ", LoweringTypeName(right), ")"};
    for (StringView part : parts) {
        if (built) {
            built = text.TryAppend(part);
        }
    }

    if (!built) {
        return built;
    }

    return Error(MaterialDiagnosticCode::TypeMismatch, StringView(), text.View());
}

Result<NodePropertyReader*> LoweringContext::Properties() noexcept {
    if (!propertiesOpened_) {
        propertiesOpened_ = true;
        if (definition_.propertiesSchema == nullptr) {
            return Result<NodePropertyReader*>::Success(nullptr);
        }

        auto reader = NodePropertyReader::Open(scratch_, node_.properties.bytes.View(),
                                               definition_.propertiesSchema());
        if (!reader) {
            if (reader.Error() == ErrorCode::OutOfMemory) {
                return Result<NodePropertyReader*>::Failure(reader.Error());
            }

            if (auto reported = Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                      "Node properties cannot be read");
                !reported) {
                return Result<NodePropertyReader*>::Failure(reported.Error());
            }

            return Result<NodePropertyReader*>::Success(nullptr);
        }

        properties_ = Move(reader.Value());
    }

    return Result<NodePropertyReader*>::Success(properties_ ? &properties_.Value() : nullptr);
}

namespace {

// 속성 읽기 공통 처리다: 성공이면 out에 넣고 true, 그래프 오류면 InvalidProperty 진단 뒤 false다.
template <class T, class Read>
Result<bool> LoweringReadProperty(LoweringContext& context, T fallback, T& out,
                                  Read read) noexcept {
    out = fallback;
    auto properties = context.Properties();
    if (!properties) {
        return Result<bool>::Failure(properties.Error());
    }

    if (properties.Value() == nullptr) {
        return Result<bool>::Success(false);
    }

    auto value = read(*properties.Value());
    if (!value) {
        if (value.Error() == ErrorCode::OutOfMemory) {
            return Result<bool>::Failure(value.Error());
        }

        if (auto reported = context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                          "Node property cannot be read");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    out = value.Value();
    return Result<bool>::Success(true);
}

} // namespace

Result<bool> LoweringContext::ReadFloat(StringView field, float fallback, float& out) noexcept {
    return LoweringReadProperty(*this, fallback, out, [&](NodePropertyReader& reader) {
        return reader.ReadFloat(field, fallback);
    });
}

Result<bool> LoweringContext::ReadDouble(StringView field, double fallback, double& out) noexcept {
    return LoweringReadProperty(*this, fallback, out, [&](NodePropertyReader& reader) {
        return reader.ReadDouble(field, fallback);
    });
}

Result<bool> LoweringContext::ReadInteger(StringView field, int64 fallback, int64& out) noexcept {
    return LoweringReadProperty(*this, fallback, out, [&](NodePropertyReader& reader) {
        return reader.ReadInteger(field, fallback);
    });
}

Result<bool> LoweringContext::ReadBool(StringView field, bool fallback, bool& out) noexcept {
    return LoweringReadProperty(*this, fallback, out, [&](NodePropertyReader& reader) {
        return reader.ReadBool(field, fallback);
    });
}

Result<bool> LoweringContext::ReadGuid(StringView field, Guid& out) noexcept {
    auto read = LoweringReadProperty(*this, Guid{}, out, [&](NodePropertyReader& reader) {
        return reader.ReadGuid(field, Guid{});
    });
    if (!read || !read.Value() || field != StringView("parameterId")) {
        return read;
    }

    // 파라미터 정체다(컬렉션 참조 노드는 shared가 없어 그대로 — 그 parameterId는 컬렉션 칸이다).
    auto properties = Properties();
    if (!properties) {
        return Result<bool>::Failure(properties.Error());
    }

    auto shared = properties.Value()->ReadBool("shared", false);
    if (!shared) {
        if (shared.Error() == ErrorCode::NotFound) {
            return read;
        }

        if (shared.Error() == ErrorCode::OutOfMemory) {
            return Result<bool>::Failure(shared.Error());
        }

        if (auto reported = Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                  "Node property cannot be read");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    out = translator_.ParameterIdentity(node_.id, out, shared.Value());
    return read;
}

bool LoweringContext::FunctionInputBinding(Guid& call, Name& pin) const noexcept {
    return translator_.FunctionInputBinding(node_.id, call, pin);
}

Result<IrValueId> LoweringContext::LayerValue(bool& passThrough) noexcept {
    passThrough = false;
    // 계획 때 순환(V2)으로 끊긴 의존이다(진단했다 — NamedRerouteUsage와 같게 실패로 끝난다).
    if (translator_.IsCut(node_.id, Name())) {
        poisoned_ = true;
        return Result<IrValueId>::Success(NoIrValue);
    }

    return translator_.LowerLayers(node_, passThrough);
}

Result<IrValueId> LoweringContext::CallerInput(Guid call, Name pin, bool& connected) noexcept {
    connected = false;
    const nodegraph::GraphNode* caller = translator_.FindNode(call);
    if (caller == nullptr) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    auto value = translator_.LowerConnection(*caller, pin, connected);
    if (value && connected && value.Value() == NoIrValue) {
        poisoned_ = true;
    }

    return value;
}

Result<bool> LoweringContext::ReadColor(StringView field, float (&out)[MaxIrComponents]) noexcept {
    for (float& component : out) {
        component = 0.0f;
    }

    auto properties = Properties();
    if (!properties) {
        return Result<bool>::Failure(properties.Error());
    }

    if (properties.Value() == nullptr) {
        return Result<bool>::Success(false);
    }

    auto color = properties.Value()->OpenStruct(field, GeneratedType<LinearColor>::Schema());
    Result<void> read = color ? Result<void>::Success() : Result<void>::Failure(color.Error());
    static constexpr StringView Channels[MaxIrComponents] = {"r", "g", "b", "a"};
    for (usize index = 0; read && index < MaxIrComponents; ++index) {
        auto channel = color.Value().ReadFloat(Channels[index], 0.0f);
        if (!channel) {
            read = Result<void>::Failure(channel.Error());
        } else {
            out[index] = channel.Value();
        }
    }

    if (!read) {
        if (read.Error() == ErrorCode::OutOfMemory) {
            return Result<bool>::Failure(read.Error());
        }

        if (auto reported = Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                  "Node color property cannot be read");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    return Result<bool>::Success(true);
}

Result<bool> LoweringContext::ReadDoubleVector(StringView field,
                                               double (&out)[MaxIrComponents]) noexcept {
    for (double& component : out) {
        component = 0.0;
    }

    auto properties = Properties();
    if (!properties) {
        return Result<bool>::Failure(properties.Error());
    }

    if (properties.Value() == nullptr) {
        return Result<bool>::Success(false);
    }

    auto vector = properties.Value()->OpenStruct(field, GeneratedType<Vec4d>::Schema());
    Result<void> read = vector ? Result<void>::Success() : Result<void>::Failure(vector.Error());
    static constexpr StringView Axes[MaxIrComponents] = {"x", "y", "z", "w"};
    for (usize index = 0; read && index < MaxIrComponents; ++index) {
        auto axis = vector.Value().ReadDouble(Axes[index], 0.0);
        if (!axis) {
            read = Result<void>::Failure(axis.Error());
        } else {
            out[index] = axis.Value();
        }
    }

    if (!read) {
        if (read.Error() == ErrorCode::OutOfMemory) {
            return Result<bool>::Failure(read.Error());
        }

        if (auto reported = Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                  "Node vector property cannot be read");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    return Result<bool>::Success(true);
}

Result<bool> LoweringContext::ReadAssetReference(StringView field, Guid& out) noexcept {
    out = Guid{};
    auto properties = Properties();
    if (!properties) {
        return Result<bool>::Failure(properties.Error());
    }

    if (properties.Value() == nullptr) {
        return Result<bool>::Success(false);
    }

    auto reference =
        properties.Value()->OpenStruct(field, GeneratedType<MaterialAssetReference>::Schema());
    Result<Guid> asset = reference ? reference.Value().ReadGuid("assetId", Guid{})
                                   : Result<Guid>::Failure(reference.Error());
    if (!asset) {
        if (asset.Error() == ErrorCode::OutOfMemory) {
            return Result<bool>::Failure(asset.Error());
        }

        if (auto reported = Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                  "Node asset reference property cannot be read");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    out = asset.Value();
    return Result<bool>::Success(true);
}

Result<Span<const nodegraph::PinDescriptor>> LoweringContext::DynamicPins() noexcept {
    if (!dynamicPinsComputed_) {
        dynamicPinsComputed_ = true;
        if (definition_.dynamicPins != nullptr) {
            const nodegraph::DynamicPinsInput input{
                translator_.Input().document, translator_.Graph(), node_, translator_.Names()};
            if (auto computed = definition_.dynamicPins(input, dynamicPins_); !computed) {
                dynamicPins_.Clear();
                if (computed.Error() == ErrorCode::OutOfMemory) {
                    return Result<Span<const nodegraph::PinDescriptor>>::Failure(computed.Error());
                }

                if (auto reported = Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                          "Node pins cannot be computed from its properties");
                    !reported) {
                    return Result<Span<const nodegraph::PinDescriptor>>::Failure(reported.Error());
                }
            }
        }
    }

    return Result<Span<const nodegraph::PinDescriptor>>::Success(dynamicPins_.View());
}

Result<IrValueId> LoweringContext::Connected(Name name, bool& connected) noexcept {
    auto value = translator_.LowerConnection(node_, name, connected);
    if (value && connected && value.Value() == NoIrValue) {
        poisoned_ = true;
    }

    return value;
}

Result<bool> LoweringContext::IsConnected(StringView key) noexcept {
    auto name = translator_.Names().TryFind(key);
    if (!name) {
        // 문서에 그 이름이 없으면 그 핀의 간선도 없다.
        return name.Error() == ErrorCode::NotFound ? Result<bool>::Success(false)
                                                   : Result<bool>::Failure(name.Error());
    }

    return Result<bool>::Success(translator_.HasConnection(node_, name.Value()));
}

Result<IrValueId> LoweringContext::PinValue(const MaterialPinDefinition& pin, Name name) noexcept {
    // 값 편집을 숨긴 핀(HideValue)은 사용자 값을 갖지 않는다(남아 있으면 무시 — NodeGraph도 쓰지
    // 않는다).
    const bool editable = !HasPinFlag(pin.flags, MaterialPinFlags::HideValue);
    for (const nodegraph::PinValueEntry& entry : node_.inputDefaults) {
        if (!editable || !(entry.pin == name)) {
            continue;
        }

        auto reader = NodePropertyReader::Open(scratch_, entry.value.bytes.View(),
                                               GeneratedType<MaterialPinValue>::Schema());
        float components[MaxIrComponents] = {0.0f, 0.0f, 0.0f, 0.0f};
        int64 count = 0;
        Result<void> read =
            reader ? Result<void>::Success() : Result<void>::Failure(reader.Error());
        static constexpr StringView Fields[MaxIrComponents] = {"x", "y", "z", "w"};
        for (usize index = 0; read && index < MaxIrComponents; ++index) {
            auto component = reader.Value().ReadFloat(Fields[index], 0.0f);
            if (!component) {
                read = Result<void>::Failure(component.Error());
            } else {
                components[index] = component.Value();
            }
        }

        if (read) {
            auto counted = reader.Value().ReadInteger("componentCount", 1);
            if (!counted) {
                read = Result<void>::Failure(counted.Error());
            } else {
                count = counted.Value();
            }
        }

        if (!read && read.Error() == ErrorCode::OutOfMemory) {
            return Result<IrValueId>::Failure(read.Error());
        }

        IrType typed;
        const bool hasWidth = LoweringPinIrType(pin.type, typed);
        if (!read || count < 1 || count > MaxIrComponents ||
            (hasWidth && count > typed.components)) {
            if (auto reported = Error(MaterialDiagnosticCode::InvalidPinValue, pin.key,
                                      "Pin value is invalid for this pin");
                !reported) {
                return Result<IrValueId>::Failure(reported.Error());
            }

            return Result<IrValueId>::Success(NoIrValue);
        }

        auto constant =
            FloatConstant(Span<const float>(components, static_cast<usize>(count)), pin.key);
        if (!constant || constant.Value() == NoIrValue || !hasWidth ||
            typed.scalar == IrScalar::Float) {
            return constant;
        }

        // 정수·Bool 핀의 값은 float로 담겨 있으므로 핀 타입으로 바꾼다(5.3절).
        return Convert(constant.Value(), typed.scalar);
    }

    if (pin.defaultValue.componentCount > 0) {
        return FloatConstant(
            Span<const float>(pin.defaultValue.values, pin.defaultValue.componentCount), pin.key);
    }

    if (HasPinFlag(pin.flags, MaterialPinFlags::Required)) {
        if (auto reported = Error(MaterialDiagnosticCode::MissingInput, pin.key, "Missing input");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }
    }

    return Result<IrValueId>::Success(NoIrValue);
}

Result<IrValueId> LoweringContext::Input(StringView key) noexcept {
    const MaterialPinDefinition* pin = FindPin(key);
    if (pin == nullptr) {
        // lowering 코드가 정의에 없는 키를 물었다(코드의 잘못).
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    auto name = PinName(key);
    if (!name) {
        return Result<IrValueId>::Failure(name.Error());
    }

    bool connected = false;
    auto value = Connected(name.Value(), connected);
    if (!value) {
        return value;
    }

    if (!connected) {
        return PinValue(*pin, name.Value());
    }

    if (value.Value() != NoIrValue && HasPinFlag(pin->flags, MaterialPinFlags::StaticOnly) &&
        static_cast<uint8>(translator_.Builder().Value(value.Value()).frequency) >
            static_cast<uint8>(IrFrequency::Static)) {
        if (auto reported = Error(MaterialDiagnosticCode::NonStaticValue, key,
                                  "Input accepts static values only");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    return value;
}

Result<IrValueId> LoweringContext::DynamicInput(Name key, Optional<float> fallback,
                                                bool required) noexcept {
    bool connected = false;
    auto value = Connected(key, connected);
    if (!value || connected) {
        return value;
    }

    if (fallback) {
        return FloatScalar(fallback.Value());
    }

    if (required) {
        if (auto reported = translator_.Report(nodegraph::GraphDiagnosticSeverity::Error,
                                               MaterialDiagnosticCode::MissingInput, node_.id, key,
                                               "Missing input");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }
    }

    return Result<IrValueId>::Success(NoIrValue);
}

Result<void> LoweringContext::Output(StringView key, IrValueId value) noexcept {
    auto name = PinName(key);
    if (!name) {
        return Result<void>::Failure(name.Error());
    }

    return OutputByName(name.Value(), value);
}

Result<void> LoweringContext::OutputByName(Name key, IrValueId value) noexcept {
    if (value == NoIrValue) {
        return Result<void>::Success();
    }

    return translator_.RecordOutput(node_.id, key, value);
}

Result<IrValueId> LoweringContext::NodeOutput(Guid node, StringView expectedType, StringView pin,
                                              bool& found) noexcept {
    found = false;
    // 사본 노드의 속성 id는 원본 그래프 id다(6.12절 MS2e2 노드 id 속성).
    const nodegraph::GraphNode* target =
        translator_.FindNode(translator_.CopiedNodeId(node_.id, node));
    if (target == nullptr) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    auto type = translator_.Names().TryFind(expectedType);
    if (!type) {
        return type.Error() == ErrorCode::NotFound ? Result<IrValueId>::Success(NoIrValue)
                                                   : Result<IrValueId>::Failure(type.Error());
    }

    if (!(target->type == type.Value())) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (translator_.IsCut(node_.id, Name())) {
        // 계획 때 순환(V2)으로 진단하고 끊은 의존이다.
        poisoned_ = true;
        return Result<IrValueId>::Success(NoIrValue);
    }

    found = true;
    auto name = PinName(pin);
    if (!name) {
        return Result<IrValueId>::Failure(name.Error());
    }

    auto value = translator_.LowerNodeOutput(*target, name.Value());
    if (value && value.Value() == NoIrValue) {
        poisoned_ = true;
    }

    return value;
}

IrType LoweringContext::TypeOf(IrValueId value) const noexcept {
    return translator_.Builder().Value(value).type;
}

namespace {

// 빌더 결과를 진단 규칙에 맞춘다: 타입 규칙 위반(InvalidArgument)은 V1 진단 뒤 NoIrValue,
// 그 밖의 실패는 시스템 오류다.
Result<IrValueId> LoweringBuilt(LoweringContext& context, Result<IrValueId> built) noexcept {
    if (built) {
        return built;
    }

    if (built.Error() != ErrorCode::InvalidArgument) {
        return built;
    }

    if (auto reported = context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(),
                                      "Operation is not defined for these types");
        !reported) {
        return Result<IrValueId>::Failure(reported.Error());
    }

    return Result<IrValueId>::Success(NoIrValue);
}

} // namespace

Result<IrValueId> LoweringContext::FloatConstant(Span<const float> components,
                                                 StringView pin) noexcept {
    for (float component : components) {
        if (!LoweringIsFinite(component)) {
            if (auto reported = Error(MaterialDiagnosticCode::NonFiniteConstant, pin,
                                      "Constant is NaN or infinite");
                !reported) {
                return Result<IrValueId>::Failure(reported.Error());
            }

            return Result<IrValueId>::Success(NoIrValue);
        }
    }

    return LoweringBuilt(*this, Builder().FloatConstant(components));
}

Result<IrValueId> LoweringContext::FloatScalar(float value) noexcept {
    return FloatConstant(Span<const float>(&value, 1), StringView());
}

Result<IrValueId> LoweringContext::DoubleConstant(double value, StringView pin) noexcept {
    if (!LoweringIsFinite(value)) {
        if (auto reported = Error(MaterialDiagnosticCode::NonFiniteConstant, pin,
                                  "Constant is NaN or infinite");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    return LoweringBuilt(*this, Builder().DoubleConstant(Span<const double>(&value, 1)));
}

namespace {

// type의 모든 성분이 value(0 또는 1)인 상수다.
Result<IrValueId> LoweringFilled(IrBuilder& builder, IrType type, int32 value) noexcept {
    const usize count = type.components;
    switch (type.scalar) {
    case IrScalar::Texture:
    case IrScalar::Surface:
    case IrScalar::Attributes:
        // 텍스처는 수 값이 아니다(6.10절 MS2c3 — 이 경로에 오지 않는다).
        break;
    case IrScalar::Float: {
        const float items[MaxIrComponents] = {float(value), float(value), float(value),
                                              float(value)};
        return builder.FloatConstant(Span<const float>(items, count));
    }
    case IrScalar::Int: {
        const int32 items[MaxIrComponents] = {value, value, value, value};
        return builder.IntConstant(Span<const int32>(items, count));
    }
    case IrScalar::UInt: {
        const uint32 items[MaxIrComponents] = {uint32(value), uint32(value), uint32(value),
                                               uint32(value)};
        return builder.UIntConstant(Span<const uint32>(items, count));
    }
    case IrScalar::Bool: {
        const bool items[MaxIrComponents] = {value != 0, value != 0, value != 0, value != 0};
        return builder.BoolConstant(Span<const bool>(items, count));
    }
    case IrScalar::Double: {
        const double items[MaxIrComponents] = {double(value), double(value), double(value),
                                               double(value)};
        return builder.DoubleConstant(Span<const double>(items, count));
    }
    }

    return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

Result<IrValueId> LoweringContext::Zero(IrType type) noexcept {
    return LoweringBuilt(*this, LoweringFilled(Builder(), type, 0));
}

Result<IrValueId> LoweringContext::One(IrType type) noexcept {
    return LoweringBuilt(*this, LoweringFilled(Builder(), type, 1));
}

Result<IrValueId> LoweringContext::RejectTexture(IrValueId value, bool& rejected) noexcept {
    rejected = value != NoIrValue && TypeOf(value).IsOpaque();
    if (!rejected) {
        return Result<IrValueId>::Success(value);
    }

    if (auto reported = TypeError(TypeOf(value).IsSurface()
                                      ? StringView("Surface values are not accepted here")
                                      : StringView("Texture values are not accepted here"),
                                  TypeOf(value), TypeOf(value));
        !reported) {
        return Result<IrValueId>::Failure(reported.Error());
    }

    return Result<IrValueId>::Success(NoIrValue);
}

Result<IrValueId> LoweringContext::Convert(IrValueId value, IrScalar scalar) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (auto refused = RejectTexture(value, textureRefused_); !refused || textureRefused_) {
        return refused;
    }

    return LoweringBuilt(*this, Builder().Convert(value, scalar));
}

Result<IrValueId> LoweringContext::Splat(IrValueId value, uint8 components) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (auto refused = RejectTexture(value, textureRefused_); !refused || textureRefused_) {
        return refused;
    }

    if (TypeOf(value).components == components) {
        return Result<IrValueId>::Success(value);
    }

    return LoweringBuilt(*this, Builder().Splat(value, components));
}

Result<IrValueId> LoweringContext::Prefix(IrValueId value, uint8 components) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (auto refused = RejectTexture(value, textureRefused_); !refused || textureRefused_) {
        return refused;
    }

    if (TypeOf(value).components == components) {
        return Result<IrValueId>::Success(value);
    }

    static constexpr uint8 Order[MaxIrComponents] = {0, 1, 2, 3};
    return Swizzle(value, Span<const uint8>(Order, components));
}

Result<IrValueId> LoweringContext::Swizzle(IrValueId value, Span<const uint8> components) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (auto refused = RejectTexture(value, textureRefused_); !refused || textureRefused_) {
        return refused;
    }

    return LoweringBuilt(*this, Builder().Swizzle(value, components));
}

Result<IrValueId> LoweringContext::Append(IrValueId left, IrValueId right) noexcept {
    if (left == NoIrValue || right == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    const IrValueId sides[] = {left, right};
    for (IrValueId side : sides) {
        if (auto refused = RejectTexture(side, textureRefused_); !refused || textureRefused_) {
            return refused;
        }
    }

    return LoweringBuilt(*this, Builder().Append(left, right));
}

Result<Optional<IrType>> LoweringContext::CommonType(Span<const IrValueId> values,
                                                     NumberRule rule) noexcept {
    uint8 width = 1;
    bool hasBool = false;
    bool hasFloat = false;
    bool hasDouble = false;
    bool hasInt = false;
    bool hasUInt = false;
    for (IrValueId value : values) {
        const IrType type = TypeOf(value);
        if (type.IsMatrix() || type.IsOpaque()) {
            if (auto reported = TypeError(
                    type.IsSurface()   ? StringView("Surface values are not accepted here")
                    : type.IsTexture() ? StringView("Texture values are not accepted here")
                                       : StringView("Matrix values are not accepted here"),
                    type, TypeOf(values[0]));
                !reported) {
                return Result<Optional<IrType>>::Failure(reported.Error());
            }

            return Result<Optional<IrType>>::Success(Optional<IrType>());
        }

        width = type.components > width ? type.components : width;
        hasBool = hasBool || type.scalar == IrScalar::Bool;
        hasFloat = hasFloat || type.scalar == IrScalar::Float;
        hasDouble = hasDouble || type.scalar == IrScalar::Double;
        hasInt = hasInt || type.scalar == IrScalar::Int;
        hasUInt = hasUInt || type.scalar == IrScalar::UInt;
    }

    for (IrValueId value : values) {
        const IrType type = TypeOf(value);
        if (type.components != 1 && type.components != width) {
            if (auto reported = TypeError("Arithmetic between these types is undefined",
                                          TypeOf(values[0]), type);
                !reported) {
                return Result<Optional<IrType>>::Failure(reported.Error());
            }

            return Result<Optional<IrType>>::Success(Optional<IrType>());
        }
    }

    const bool numeric = hasFloat || hasDouble || hasInt || hasUInt;
    IrScalar scalar = IrScalar::Float;
    StringView problem;
    if (hasBool) {
        if (numeric || (rule != NumberRule::Logical && rule != NumberRule::Equality)) {
            problem = "Boolean values are not accepted in arithmetic";
        }

        scalar = IrScalar::Bool;
    } else if (rule == NumberRule::Logical) {
        problem = "Logical operations need boolean values";
    } else if (hasInt && hasUInt && !hasFloat && !hasDouble) {
        problem = "Arithmetic between int and uint is undefined";
    } else {
        scalar = hasDouble  ? IrScalar::Double
                 : hasFloat ? IrScalar::Float
                 : hasInt   ? IrScalar::Int
                            : IrScalar::UInt;
        if (rule == NumberRule::NoDouble && scalar == IrScalar::Double) {
            scalar = IrScalar::Float;
        } else if (rule == NumberRule::FloatOnly) {
            scalar = IrScalar::Float;
        } else if (rule == NumberRule::IntegerOnly &&
                   (scalar == IrScalar::Float || scalar == IrScalar::Double)) {
            problem = "Integer operations need int or uint values";
        }
    }

    if (!problem.IsEmpty()) {
        const IrType first = TypeOf(values[0]);
        const IrType last = TypeOf(values[values.Size() - 1]);
        if (auto reported = TypeError(problem, first, last); !reported) {
            return Result<Optional<IrType>>::Failure(reported.Error());
        }

        return Result<Optional<IrType>>::Success(Optional<IrType>());
    }

    return Result<Optional<IrType>>::Success(Optional<IrType>(IrType{scalar, width, 1}));
}

Result<IrValueId> LoweringContext::Cast(IrValueId value, IrType type) noexcept {
    auto converted = Convert(value, type.scalar);
    if (!converted) {
        return converted;
    }

    return Splat(converted.Value(), type.components);
}

Result<IrValueId> LoweringContext::CoerceTo(IrValueId value, IrType target,
                                            StringView pin) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    const IrType source = TypeOf(value);
    StringView problem;
    if (source.IsOpaque() || target.IsOpaque()) {
        if (source == target) {
            return Result<IrValueId>::Success(value);
        }

        problem = source.IsSurface() || target.IsSurface()
                      ? StringView("Surface value cannot be used here")
                      : StringView("Texture value cannot be used here");
    } else if (source.IsMatrix() || target.IsMatrix()) {
        problem = "Matrix value cannot be used here";
    } else if ((source.scalar == IrScalar::Bool) != (target.scalar == IrScalar::Bool)) {
        problem = "Boolean and numeric values do not convert implicitly";
    } else if (source.components != target.components && source.components != 1 &&
               target.components != 1) {
        // UE CastValue: 스칼라가 아닌 대상으로는 자르지 않는다("Cannot cast from larger type").
        problem = "Cannot cast between vectors of different widths";
    }

    if (!problem.IsEmpty()) {
        String text(scratch_);
        Result<void> built = text.TryAssign(problem);
        const StringView parts[] = {" (", LoweringTypeName(source), " to ",
                                    LoweringTypeName(target), ")"};
        for (StringView part : parts) {
            if (built) {
                built = text.TryAppend(part);
            }
        }

        if (!built) {
            return Result<IrValueId>::Failure(built.Error());
        }

        if (auto reported = Error(MaterialDiagnosticCode::TypeMismatch, pin, text.View());
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    auto converted = Convert(value, target.scalar);
    if (!converted || converted.Value() == NoIrValue) {
        return converted;
    }

    if (target.components == 1) {
        return Prefix(converted.Value(), 1);
    }

    return Splat(converted.Value(), target.components);
}

Result<IrValueId> LoweringContext::Unary(IrOp op, IrValueId value, NumberRule rule) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (auto refused = RejectTexture(value, textureRefused_); !refused || textureRefused_) {
        return refused;
    }

    const IrType type = TypeOf(value);
    if (type.IsMatrix()) {
        // 행렬은 Transpose·Negate만 받는다(빌더 규칙).
        return LoweringBuilt(*this, Builder().Unary(op, value));
    }

    IrScalar target = type.scalar;
    StringView problem;
    switch (rule) {
    case NumberRule::Arithmetic:
    case NumberRule::Equality:
        if (type.scalar == IrScalar::Bool) {
            problem = "Boolean values are not accepted in arithmetic";
        } else if ((type.scalar == IrScalar::Int || type.scalar == IrScalar::UInt) &&
                   op != IrOp::Negate) {
            // UE IR 연산 서명 OPF_CastToAnyFloat(Negate만 정수를 그대로 둔다).
            target = IrScalar::Float;
        }

        break;
    case NumberRule::NoDouble:
        if (type.scalar == IrScalar::Bool) {
            problem = "Boolean values are not accepted in arithmetic";
        } else if (type.scalar == IrScalar::Double) {
            target = IrScalar::Float;
        }

        break;
    case NumberRule::FloatOnly:
        if (type.scalar == IrScalar::Bool) {
            problem = "Boolean values are not accepted in arithmetic";
        }

        target = IrScalar::Float;
        break;
    case NumberRule::IntegerOnly:
        if (type.scalar != IrScalar::Int && type.scalar != IrScalar::UInt) {
            problem = "Integer operations need int or uint values";
        }

        break;
    case NumberRule::Logical:
        if (type.scalar != IrScalar::Bool) {
            problem = "Logical operations need boolean values";
        }

        break;
    }

    if (!problem.IsEmpty()) {
        if (auto reported = TypeError(problem, type, type); !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    auto converted = Convert(value, target);
    if (!converted || converted.Value() == NoIrValue) {
        return converted;
    }

    return LoweringBuilt(*this, Builder().Unary(op, converted.Value()));
}

Result<IrValueId> LoweringContext::Binary(IrOp op, IrValueId left, IrValueId right,
                                          NumberRule rule) noexcept {
    if (left == NoIrValue || right == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    const IrValueId operands[] = {left, right};
    auto common = CommonType(Span<const IrValueId>(operands, 2), rule);
    if (!common || !common.Value()) {
        return common ? Result<IrValueId>::Success(NoIrValue)
                      : Result<IrValueId>::Failure(common.Error());
    }

    auto a = Cast(left, common.Value().Value());
    auto b = Cast(right, common.Value().Value());
    if (!a || !b) {
        return !a ? Move(a) : Move(b);
    }

    if (a.Value() == NoIrValue || b.Value() == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    auto result = LoweringBuilt(*this, Builder().Binary(op, a.Value(), b.Value()));
    const IrType leftType = TypeOf(left);
    if (!result || result.Value() == NoIrValue || op != IrOp::Subtract ||
        leftType.scalar != IrScalar::Double || !(leftType == TypeOf(right))) {
        return result;
    }

    // 같은 Double 타입 둘의 뺄셈은 상대 값으로 보고 Float로 자른다(6.9절 — UE Sub의
    // LWCTruncateMode 기본 2, HLSLMaterialTranslator.cpp:9550–9575).
    return Convert(result.Value(), IrScalar::Float);
}

Result<IrValueId> LoweringContext::Ternary(IrOp op, IrValueId first, IrValueId second,
                                           IrValueId third, NumberRule rule) noexcept {
    if (first == NoIrValue || second == NoIrValue || third == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    const IrValueId operands[] = {first, second, third};
    auto common = CommonType(Span<const IrValueId>(operands, 3), rule);
    if (!common || !common.Value()) {
        return common ? Result<IrValueId>::Success(NoIrValue)
                      : Result<IrValueId>::Failure(common.Error());
    }

    IrValueId cast[3] = {NoIrValue, NoIrValue, NoIrValue};
    for (usize index = 0; index < 3; ++index) {
        auto converted = Cast(operands[index], common.Value().Value());
        if (!converted) {
            return converted;
        }

        if (converted.Value() == NoIrValue) {
            return Result<IrValueId>::Success(NoIrValue);
        }

        cast[index] = converted.Value();
    }

    return LoweringBuilt(*this, Builder().Ternary(op, cast[0], cast[1], cast[2]));
}

Result<IrValueId> LoweringContext::SelectAttributes(IrValueId condition, IrValueId whenTrue,
                                                    IrValueId whenFalse, bool branch) noexcept {
    // 속성 집합 둘의 실행 중 선택은 칸마다다(UE 고전은 속성마다 compile — 6.12절 MS2e1). 한쪽에만
    // 있는 칸은 다른 쪽이 기본값이다.
    if (TypeOf(condition) != IrBool(1)) {
        if (auto reported =
                TypeError("Selection of material attributes needs a bool scalar condition",
                          TypeOf(condition), IrBool(1));
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    SystemAllocator scratch;
    Array<AttributeEntry> first(scratch);
    Array<AttributeEntry> second(scratch);
    auto a = CopyAttributes(whenTrue, first);
    auto b = CopyAttributes(whenFalse, second);
    if (!a || !b) {
        return Result<IrValueId>::Failure(!a ? a.Error() : b.Error());
    }

    Array<AttributeEntry> merged(scratch);
    const Array<AttributeEntry>* sides[] = {&first, &second};
    for (const Array<AttributeEntry>* side : sides) {
        for (const AttributeEntry& entry : *side) {
            bool seen = false;
            for (const AttributeEntry& item : merged) {
                seen = seen || item.id == entry.id;
            }

            if (seen) {
                continue;
            }

            const MaterialAttributeDefinition* definition = FindAttribute(entry.id);
            if (definition == nullptr) {
                return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
            }

            auto x = AttributeValue(first.View(), *definition);
            auto y = AttributeValue(second.View(), *definition);
            if (!x || !y) {
                return Result<IrValueId>::Failure(!x ? x.Error() : y.Error());
            }

            IrValueId chosen = NoIrValue;
            if (x.Value() != NoIrValue && y.Value() != NoIrValue &&
                !TypeOf(x.Value()).IsSurface()) {
                auto selected = branch ? Branch(condition, x.Value(), y.Value())
                                       : Select(condition, x.Value(), y.Value());
                if (!selected) {
                    return selected;
                }

                chosen = selected.Value();
            }

            if (auto added = merged.TryPushBack(AttributeEntry{entry.id, chosen}); !added) {
                return Result<IrValueId>::Failure(added.Error());
            }
        }
    }

    return MakeAttributes(merged.View());
}

Result<IrValueId> LoweringContext::Select(IrValueId condition, IrValueId whenTrue,
                                          IrValueId whenFalse) noexcept {
    if (condition == NoIrValue || whenTrue == NoIrValue || whenFalse == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (TypeOf(whenTrue).IsAttributes() && TypeOf(whenFalse).IsAttributes()) {
        return SelectAttributes(condition, whenTrue, whenFalse, false);
    }

    const IrValueId branches[] = {whenTrue, whenFalse};
    auto common = CommonType(Span<const IrValueId>(branches, 2), NumberRule::Equality);
    if (!common || !common.Value()) {
        return common ? Result<IrValueId>::Success(NoIrValue)
                      : Result<IrValueId>::Failure(common.Error());
    }

    const IrType type = common.Value().Value();
    const IrType conditionType = TypeOf(condition);
    if (conditionType.scalar != IrScalar::Bool || conditionType.IsMatrix() ||
        (conditionType.components != 1 && conditionType.components != type.components)) {
        if (auto reported = TypeError("Selection condition must be bool of the value width",
                                      conditionType, type);
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    auto a = Cast(whenTrue, type);
    auto b = Cast(whenFalse, type);
    if (!a || !b) {
        return !a ? Move(a) : Move(b);
    }

    if (a.Value() == NoIrValue || b.Value() == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    bool constant = false;
    if (ConstantBool(condition, constant)) {
        return Result<IrValueId>::Success(constant ? a.Value() : b.Value());
    }

    return LoweringBuilt(*this, Builder().Select(condition, a.Value(), b.Value()));
}

Result<IrValueId> LoweringContext::Branch(IrValueId condition, IrValueId whenTrue,
                                          IrValueId whenFalse) noexcept {
    if (condition == NoIrValue || whenTrue == NoIrValue || whenFalse == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (TypeOf(whenTrue).IsAttributes() && TypeOf(whenFalse).IsAttributes()) {
        return SelectAttributes(condition, whenTrue, whenFalse, true);
    }

    const IrType conditionType = TypeOf(condition);
    const IrValueId branches[] = {whenTrue, whenFalse};
    auto common = CommonType(Span<const IrValueId>(branches, 2), NumberRule::Equality);
    if (!common || !common.Value()) {
        return common ? Result<IrValueId>::Success(NoIrValue)
                      : Result<IrValueId>::Failure(common.Error());
    }

    if (conditionType != IrBool(1)) {
        if (auto reported = TypeError("Branch condition must be a bool scalar", conditionType,
                                      common.Value().Value());
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    auto a = Cast(whenTrue, common.Value().Value());
    auto b = Cast(whenFalse, common.Value().Value());
    if (!a || !b) {
        return !a ? Move(a) : Move(b);
    }

    if (a.Value() == NoIrValue || b.Value() == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    bool constant = false;
    if (ConstantBool(condition, constant)) {
        return Result<IrValueId>::Success(constant ? a.Value() : b.Value());
    }

    return LoweringBuilt(*this, Builder().Branch(condition, a.Value(), b.Value()));
}

Result<IrValueId> LoweringContext::Coerce(IrValueId value, MaterialPinType type,
                                          StringView pin) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    IrType target;
    if (!LoweringPinIrType(type, target)) {
        // Numeric·Any 등은 값 그대로다.
        return Result<IrValueId>::Success(value);
    }

    const IrType source = TypeOf(value);
    StringView problem;
    if (source.IsMatrix()) {
        problem = "Matrix value cannot be used here";
    } else if ((source.scalar == IrScalar::Bool) != (target.scalar == IrScalar::Bool)) {
        problem = "Boolean and numeric values do not convert implicitly";
    } else if ((target.scalar == IrScalar::Int || target.scalar == IrScalar::UInt) &&
               source.scalar != target.scalar) {
        problem = "Value does not convert to an integer implicitly";
    } else if (source.components != 1 && source.components < target.components) {
        problem = "Value has fewer components than the input; use Append";
    }

    if (!problem.IsEmpty()) {
        String text(scratch_);
        Result<void> built = text.TryAssign(problem);
        const StringView parts[] = {" (", LoweringTypeName(source), " to ",
                                    LoweringTypeName(target), ")"};
        for (StringView part : parts) {
            if (built) {
                built = text.TryAppend(part);
            }
        }

        if (!built) {
            return Result<IrValueId>::Failure(built.Error());
        }

        if (auto reported = Error(MaterialDiagnosticCode::TypeMismatch, pin, text.View());
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    IrValueId current = value;
    if (source.components > target.components) {
        if (auto reported = Info(MaterialDiagnosticCode::NarrowedValue, pin,
                                 "Only the leading components of the value are used");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        auto narrowed = Prefix(current, target.components);
        if (!narrowed || narrowed.Value() == NoIrValue) {
            return narrowed;
        }

        current = narrowed.Value();
    }

    return Cast(current, target);
}

bool LoweringContext::ConstantFloat(IrValueId value, float& out) const noexcept {
    if (value == NoIrValue) {
        return false;
    }

    const IrValue& item = translator_.Builder().Value(value);
    if (item.op != IrOp::Constant || item.type != IrFloat(1)) {
        return false;
    }

    out = LoweringFloatBits(item.immediate[0]);
    return true;
}

bool LoweringContext::ConstantBool(IrValueId value, bool& out) const noexcept {
    if (value == NoIrValue) {
        return false;
    }

    const IrValue& item = translator_.Builder().Value(value);
    if (item.op != IrOp::Constant || item.type != IrBool(1)) {
        return false;
    }

    out = item.immediate[0] != 0;
    return true;
}

} // namespace rocks::material::detail
