#include "MaterialDynamicPins.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <MaterialSchema/MaterialAssetReference.generated.h>
#include <MaterialSchema/MaterialAttributeTable.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.h>
#include <MaterialSchema/Nodes/Custom/CustomInput.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomOutput.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallInput.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallOutput.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionInputProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionInputProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AggregateProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AggregateProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/GetAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackInput.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/SetAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ConvertInput.generated.h>
#include <MaterialSchema/Nodes/Math/ConvertOutput.generated.h>
#include <MaterialSchema/Nodes/Math/ConvertProperties.generated.h>
#include <MaterialSchema/Nodes/Math/MathNodeEnums.h>
#include <MaterialSchema/Nodes/Math/OperatorInput.generated.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.generated.h>
#include <MaterialSchema/Nodes/Math/SwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Outputs/MaterialCacheAttribute.generated.h>
#include <MaterialSchema/Nodes/Outputs/MaterialCacheOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Particles/DynamicParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Switches/SwitchInput.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectFromCollectionProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/TransformPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/VectorsNodeEnums.h>
#include <MaterialSchema/Nodes/VirtualTexture/VirtualTextureNodeEnums.h>

// 엔진 노드의 동적 핀 함수다(Docs/Design/MaterialSystem 19.13.3절). 근거는 함수마다 UE 함수 이름을
// 적는다(RE/Private/Materials/MaterialExpressions.cpp).
namespace rocks::material::detail {

namespace {

using nodegraph::DynamicPinsInput;
using nodegraph::PinDescriptor;
using nodegraph::PinList;

// 색인 키 버퍼 길이다("Output" + uint32 10진 10자리, 또는 "P_" + 32자 hex).
constexpr usize KeyBufferBytes = 40;
// 색인 키의 진법이다.
constexpr usize DecimalBase = 10;
// Guid 하나의 16진 자릿수다.
constexpr usize GuidHexDigits = 32;
// 16진 한 자리의 bit 수와 mask다.
constexpr int HexDigitBits = 4;
constexpr uint64 HexDigitMask = 0xF;
// Guid 반쪽(uint64)의 16진 자릿수다.
constexpr int HalfHexDigits = static_cast<int>(sizeof(uint64) * 8) / HexDigitBits;
// 문자로 이름을 붙이는 Operator 입력 수다(A–Z, UE GetInputName).
constexpr usize OperatorLetterInputs = 26;

constexpr StringView OperatorLetters[OperatorLetterInputs] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
};

// DynamicParameter의 성분 출력 수와 기본 출력 이름이다(UE 생성자 ParamNames).
constexpr usize DynamicParameterOutputs = 4;
constexpr StringView DynamicParameterDefaultNames[DynamicParameterOutputs] = {"Param1", "Param2",
                                                                              "Param3", "Param4"};

Result<Name> Intern(NamePool& names, StringView text) noexcept {
    return names.TryIntern(text);
}

// prefix 뒤에 index 10진을 붙인 키 글자다("Input0"). buffer를 가리킨다.
StringView IndexedText(char (&buffer)[KeyBufferBytes], StringView prefix, usize index) noexcept {
    usize length = 0;
    for (; length < prefix.ByteCount() && length < KeyBufferBytes; ++length) {
        buffer[length] = prefix.Data()[length];
    }

    char digits[KeyBufferBytes];
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + index % DecimalBase);
        index /= DecimalBase;
    } while (index != 0 && count < KeyBufferBytes);

    while (count > 0 && length < KeyBufferBytes) {
        buffer[length++] = digits[--count];
    }

    return StringView(buffer, length);
}

// prefix 뒤에 index 10진을 붙인 키다("Input0").
Result<Name> IndexedKey(NamePool& names, StringView prefix, usize index) noexcept {
    char buffer[KeyBufferBytes];
    return names.TryIntern(IndexedText(buffer, prefix, index));
}

// prefix 뒤에 Guid 32자 hex(high, low 순 — 소문자)를 붙인 키다("P_…", NodeGraph GD2).
Result<Name> GuidKey(NamePool& names, StringView prefix, Guid id) noexcept {
    constexpr char hex[] = "0123456789abcdef";
    char buffer[KeyBufferBytes];
    usize length = 0;
    for (; length < prefix.ByteCount(); ++length) {
        buffer[length] = prefix.Data()[length];
    }

    const uint64 halves[] = {id.high, id.low};
    for (uint64 half : halves) {
        for (int digit = HalfHexDigits - 1; digit >= 0; --digit) {
            buffer[length++] = hex[(half >> (digit * HexDigitBits)) & HexDigitMask];
        }
    }

    static_assert(sizeof("P_") - 1 + GuidHexDigits <= KeyBufferBytes, "key buffer too small");
    return names.TryIntern(StringView(buffer, length));
}

// 동적 입력 핀이다(값 편집 숨김 — 연결이 없을 때의 값은 속성이 준다).
Result<void> AddInput(PinList& out, Name key, MaterialPinType type, StringView display) noexcept {
    PinDescriptor pin = nodegraph::InputPin(key, ToPinTypeId(type), display);
    pin.valuePolicy = nodegraph::PinValuePolicy::Hidden;
    return out.Add(pin);
}

Result<void> AddOutput(PinList& out, Name key, MaterialPinType type, StringView display) noexcept {
    return out.Add(nodegraph::OutputPin(key, ToPinTypeId(type), display));
}

Result<void> AddNamedInput(const DynamicPinsInput& input, PinList& out, StringView key,
                           MaterialPinType type, StringView display) noexcept {
    auto name = Intern(input.names, key);
    if (!name) {
        return Result<void>::Failure(name.Error());
    }

    return AddInput(out, name.Value(), type, display);
}

Result<void> AddNamedOutput(const DynamicPinsInput& input, PinList& out, StringView key,
                            MaterialPinType type, StringView display) noexcept {
    auto name = Intern(input.names, key);
    if (!name) {
        return Result<void>::Failure(name.Error());
    }

    return AddOutput(out, name.Value(), type, display);
}

// 이름 있는 핀은 그 이름, 빈 이름은 prefix<index> 키(표시 이름 없음)다.
Result<Name> UserKey(const DynamicPinsInput& input, StringView text, StringView prefix,
                     usize index) noexcept {
    return text.IsEmpty() ? IndexedKey(input.names, prefix, index) : Intern(input.names, text);
}

// 출력 키다. keyText인 핀(입력 포함)이 이미 있으면 뒤에 Out을 붙인다(핀 키는 방향과 상관없이
// 유일 — 19.13.3절). 붙인 키도 있으면 Add가 InvalidArgument를 낸다.
Result<Name> OutputKey(const DynamicPinsInput& input, const PinList& out,
                       StringView keyText) noexcept {
    auto key = Intern(input.names, keyText);
    if (!key || out.Find(key.Value()) == nullptr) {
        return key;
    }

    SystemAllocator scratch;
    String text(scratch);
    if (auto assigned = text.TryAssign(keyText); !assigned) {
        return Result<Name>::Failure(assigned.Error());
    }

    if (auto appended = text.TryAppend("Out"); !appended) {
        return Result<Name>::Failure(appended.Error());
    }

    return Intern(input.names, text.View());
}

// 사용자 이름 출력 핀이다. 이름이 있으면 그 이름, 비면 prefix<index>를 키로 하고 OutputKey 규칙을
// 따른다.
Result<void> AddUserOutput(const DynamicPinsInput& input, PinList& out, StringView text,
                           StringView prefix, usize index, MaterialPinType type) noexcept {
    char buffer[KeyBufferBytes];
    const StringView keyText = text.IsEmpty() ? IndexedText(buffer, prefix, index) : text;
    auto key = OutputKey(input, out, keyText);
    if (!key) {
        return Result<void>::Failure(key.Error());
    }

    return AddOutput(out, key.Value(), type, text);
}

Result<NodePropertyReader> Open(Allocator& scratch, const DynamicPinsInput& input,
                                const TypeSchema& schema) noexcept {
    return NodePropertyReader::Open(scratch, input.node.properties.bytes.View(), schema);
}

// 함수 입력 값 종류의 핀 타입이다(UE
// UMaterialExpressionFunctionInput::GetMaterialTypeFromInputType).
MaterialPinType FunctionInputPinType(int64 value) noexcept {
    switch (static_cast<FunctionInputType>(value)) {
    case FunctionInputType::Scalar:
        return MaterialPinType::Float1;
    case FunctionInputType::Vector2:
        return MaterialPinType::Float2;
    case FunctionInputType::Vector3:
        return MaterialPinType::Float3;
    case FunctionInputType::Vector4:
        return MaterialPinType::Float4;
    case FunctionInputType::Texture2D:
        return MaterialPinType::Texture2D;
    case FunctionInputType::TextureCube:
        return MaterialPinType::TextureCube;
    case FunctionInputType::Texture2DArray:
        return MaterialPinType::Texture2DArray;
    case FunctionInputType::Texture3D:
        return MaterialPinType::Texture3D;
    case FunctionInputType::StaticBool:
        return MaterialPinType::StaticBool;
    case FunctionInputType::MaterialAttributes:
        return MaterialPinType::MaterialAttributes;
    case FunctionInputType::TextureExternal:
        return MaterialPinType::TextureExternal;
    case FunctionInputType::Bool:
        return MaterialPinType::Bool;
    case FunctionInputType::Surface:
        return MaterialPinType::Surface;
    }

    return MaterialPinType::Any;
}

bool IsKnownFunctionInputType(int64 value) noexcept {
    return value >= static_cast<int64>(FunctionInputType::Scalar) &&
           value <= static_cast<int64>(FunctionInputType::Surface);
}

// Custom 출력 종류의 핀 타입이다(UE ECustomMaterialOutputType).
Result<MaterialPinType> CustomOutputPinType(int64 value) noexcept {
    switch (static_cast<CustomOutputType>(value)) {
    case CustomOutputType::Float1:
        return Result<MaterialPinType>::Success(MaterialPinType::Float1);
    case CustomOutputType::Float2:
        return Result<MaterialPinType>::Success(MaterialPinType::Float2);
    case CustomOutputType::Float3:
        return Result<MaterialPinType>::Success(MaterialPinType::Float3);
    case CustomOutputType::Float4:
        return Result<MaterialPinType>::Success(MaterialPinType::Float4);
    case CustomOutputType::MaterialAttributes:
        return Result<MaterialPinType>::Success(MaterialPinType::MaterialAttributes);
    }

    return Result<MaterialPinType>::Failure(ErrorCode::InvalidArgument);
}

// Convert 값 종류의 핀 타입이다(UE MaterialExpressionConvertType::ToMaterialValueType).
Result<MaterialPinType> ConvertPinType(int64 value) noexcept {
    switch (static_cast<ConvertValueType>(value)) {
    case ConvertValueType::Scalar:
        return Result<MaterialPinType>::Success(MaterialPinType::Float1);
    case ConvertValueType::Vector2:
        return Result<MaterialPinType>::Success(MaterialPinType::Float2);
    case ConvertValueType::Vector3:
        return Result<MaterialPinType>::Success(MaterialPinType::Float3);
    case ConvertValueType::Vector4:
        return Result<MaterialPinType>::Success(MaterialPinType::Float4);
    }

    return Result<MaterialPinType>::Failure(ErrorCode::InvalidArgument);
}

// 내장 속성 하나(id 또는 이름으로 찾는다). 없으면 nullptr이다(custom 속성은 등록 표에만 있다).
const MaterialAttributeDefinition* FindBuiltinAttribute(Guid id) noexcept {
    for (const MaterialAttributeDefinition& definition : BuiltinMaterialAttributes()) {
        if (definition.id == id) {
            return &definition;
        }
    }

    return nullptr;
}

const MaterialAttributeDefinition* FindBuiltinAttribute(StringView name) noexcept {
    for (const MaterialAttributeDefinition& definition : BuiltinMaterialAttributes()) {
        if (definition.name == name) {
            return &definition;
        }
    }

    return nullptr;
}

// 속성 id 배열마다 핀 하나(키 A_<id>, 표시·타입은 내장 표 — 없으면 표시 없음·Any)다.
Result<void> AddAttributePins(const DynamicPinsInput& input, PinList& out, const TypeSchema& schema,
                              StringView field, nodegraph::PinDirection direction) noexcept {
    SystemAllocator scratch;
    auto reader = Open(scratch, input, schema);
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    Array<Guid> ids(scratch);
    if (auto read = reader.Value().ReadGuidArray(field, ids); !read) {
        return read;
    }

    for (Guid id : ids) {
        auto key = GuidKey(input.names, "A_", id);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        const MaterialAttributeDefinition* definition = FindBuiltinAttribute(id);
        const MaterialPinType type = definition ? definition->type : MaterialPinType::Any;
        const StringView display = definition ? definition->name : StringView();
        auto added = direction == nodegraph::PinDirection::Input
                         ? AddInput(out, key.Value(), type, display)
                         : AddOutput(out, key.Value(), type, display);
        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> AddTextureSampleMipPins(const DynamicPinsInput& input, PinList& out,
                                     const TypeSchema& schema) noexcept {
    SystemAllocator scratch;
    auto reader = Open(scratch, input, schema);
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto mode = reader.Value().ReadInteger("mipValueMode", 0);
    if (!mode) {
        return Result<void>::Failure(mode.Error());
    }

    switch (static_cast<TextureMipValueMode>(mode.Value())) {
    case TextureMipValueMode::None:
        return Result<void>::Success();
    case TextureMipValueMode::MipLevel:
        return AddNamedInput(input, out, "MipValue", MaterialPinType::Numeric, "MipLevel");
    case TextureMipValueMode::MipBias:
        return AddNamedInput(input, out, "MipValue", MaterialPinType::Numeric, "MipBias");
    case TextureMipValueMode::Derivative:
        if (auto dx =
                AddNamedInput(input, out, "CoordinatesDX", MaterialPinType::Numeric, "DDX(UVs)");
            !dx) {
            return dx;
        }

        return AddNamedInput(input, out, "CoordinatesDY", MaterialPinType::Numeric, "DDY(UVs)");
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> AddSparseVolumeTextureMipPins(const DynamicPinsInput& input, PinList& out,
                                           const TypeSchema& schema) noexcept {
    // UE UMaterialExpressionSparseVolumeTextureSample::GetInputName의 이름도 텍스처 샘플과 같다.
    return AddTextureSampleMipPins(input, out, schema);
}

Result<void> AddRuntimeVirtualTextureMipPins(const DynamicPinsInput& input, PinList& out,
                                             const TypeSchema& schema) noexcept {
    SystemAllocator scratch;
    auto reader = Open(scratch, input, schema);
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto mode = reader.Value().ReadInteger("mipValueMode", 0);
    if (!mode) {
        return Result<void>::Failure(mode.Error());
    }

    switch (static_cast<RuntimeVirtualTextureMipValueMode>(mode.Value())) {
    case RuntimeVirtualTextureMipValueMode::None:
        return Result<void>::Success();
    case RuntimeVirtualTextureMipValueMode::MipLevel:
        return AddNamedInput(input, out, "MipValue", MaterialPinType::Numeric, "Mip Level");
    case RuntimeVirtualTextureMipValueMode::MipBias:
        return AddNamedInput(input, out, "MipValue", MaterialPinType::Numeric, "Mip Bias");
    case RuntimeVirtualTextureMipValueMode::DerivativeUV:
        if (auto dx = AddNamedInput(input, out, "DDX", MaterialPinType::Numeric, "DDX (UV)"); !dx) {
            return dx;
        }

        return AddNamedInput(input, out, "DDY", MaterialPinType::Numeric, "DDY (UV)");
    case RuntimeVirtualTextureMipValueMode::DerivativeWorld:
        if (auto dx = AddNamedInput(input, out, "DDX", MaterialPinType::Numeric, "DDX (World)");
            !dx) {
            return dx;
        }

        return AddNamedInput(input, out, "DDY", MaterialPinType::Numeric, "DDY (World)");
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> AddTextureObjectOutputPin(const DynamicPinsInput& input, PinList& out,
                                       const TypeSchema& schema) noexcept {
    SystemAllocator scratch;
    auto reader = Open(scratch, input, schema);
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto texture =
        reader.Value().OpenStruct("texture", GeneratedType<MaterialAssetReference>::Schema());
    if (!texture) {
        return Result<void>::Failure(texture.Error());
    }

    auto expected = texture.Value().ReadText("expectedClass");
    if (!expected) {
        return Result<void>::Failure(expected.Error());
    }

    MaterialPinType type = MaterialPinType::Texture2D;
    const StringView name = expected.Value();
    if (name == StringView("TextureCube")) {
        type = MaterialPinType::TextureCube;
    } else if (name == StringView("Texture2DArray")) {
        type = MaterialPinType::Texture2DArray;
    } else if (name == StringView("TextureCubeArray")) {
        type = MaterialPinType::TextureCubeArray;
    } else if (name == StringView("Texture3D") || name == StringView("VolumeTexture")) {
        type = MaterialPinType::Texture3D;
    }

    return AddNamedOutput(input, out, "Result", type, "");
}

Result<void> CustomDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionCustom::GetInput·GetInputName·GetOutputValueType.
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<CustomHlslProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto inputs = reader.Value().OpenStructArray("inputs", GeneratedType<CustomInput>::Schema());
    if (!inputs) {
        return Result<void>::Failure(inputs.Error());
    }

    for (usize index = 0; index < inputs.Value().Count(); ++index) {
        auto element = inputs.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto text = element.Value().ReadText("inputName");
        if (!text) {
            return Result<void>::Failure(text.Error());
        }

        auto key = UserKey(input, text.Value(), "Input", index);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        // UE Custom 입력은 텍스처 객체도 받는다(HLSL 인자로 넘긴다) — 타입은 번역이 정한다.
        if (auto added = AddInput(out, key.Value(), MaterialPinType::Any, text.Value()); !added) {
            return added;
        }
    }

    auto mainType =
        reader.Value().ReadInteger("outputType", static_cast<int64>(CustomOutputType::Float3));
    if (!mainType) {
        return Result<void>::Failure(mainType.Error());
    }

    auto resultType = CustomOutputPinType(mainType.Value());
    if (!resultType) {
        return Result<void>::Failure(resultType.Error());
    }

    auto outputs =
        reader.Value().OpenStructArray("additionalOutputs", GeneratedType<CustomOutput>::Schema());
    if (!outputs) {
        return Result<void>::Failure(outputs.Error());
    }

    // UE RebuildOutputs: 추가 출력이 있으면 주 출력 이름이 "return"이고 이름을 보인다. 키는 늘
    // Result다(연결이 추가 출력 수에 따라 바뀌지 않는다).
    const StringView mainDisplay =
        outputs.Value().Count() == 0 ? StringView() : StringView("return");
    auto mainKey = OutputKey(input, out, "Result");
    if (!mainKey) {
        return Result<void>::Failure(mainKey.Error());
    }

    if (auto added = AddOutput(out, mainKey.Value(), resultType.Value(), mainDisplay); !added) {
        return added;
    }

    for (usize index = 0; index < outputs.Value().Count(); ++index) {
        auto element = outputs.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto text = element.Value().ReadText("outputName");
        auto kind =
            element.Value().ReadInteger("outputType", static_cast<int64>(CustomOutputType::Float1));
        if (!text || !kind) {
            return Result<void>::Failure(!text ? text.Error() : kind.Error());
        }

        // UE RebuildOutputs는 이름 없는 추가 출력을 핀으로 만들지 않는다.
        if (text.Value().IsEmpty()) {
            continue;
        }

        auto type = CustomOutputPinType(kind.Value());
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        if (auto added = AddUserOutput(input, out, text.Value(), "Output", index, type.Value());
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> SwitchDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionSwitch::GetInput·GetInputName(2번부터 Inputs[i].InputName).
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<SwitchProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto inputs = reader.Value().OpenStructArray("inputs", GeneratedType<SwitchInput>::Schema());
    if (!inputs) {
        return Result<void>::Failure(inputs.Error());
    }

    for (usize index = 0; index < inputs.Value().Count(); ++index) {
        auto element = inputs.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto text = element.Value().ReadText("inputName");
        if (!text) {
            return Result<void>::Failure(text.Error());
        }

        auto key = UserKey(input, text.Value(), "Input", index);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        if (auto added = AddInput(out, key.Value(), MaterialPinType::Numeric, text.Value());
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> ConvertDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionConvert::GetInput·GetOutputs·Get*ValueType(성분 폭).
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<ConvertProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    struct Side {
        StringView field;
        const TypeSchema& schema;
        StringView prefix;
        nodegraph::PinDirection direction;
    };

    const Side sides[] = {
        {"convertInputs", GeneratedType<ConvertInput>::Schema(), "Input",
         nodegraph::PinDirection::Input},
        {"convertOutputs", GeneratedType<ConvertOutput>::Schema(), "Output",
         nodegraph::PinDirection::Output},
    };
    for (const Side& side : sides) {
        auto list = reader.Value().OpenStructArray(side.field, side.schema);
        if (!list) {
            return Result<void>::Failure(list.Error());
        }

        for (usize index = 0; index < list.Value().Count(); ++index) {
            auto element = list.Value().Element(index);
            if (!element) {
                return Result<void>::Failure(element.Error());
            }

            auto kind = element.Value().ReadInteger("type", 0);
            if (!kind) {
                return Result<void>::Failure(kind.Error());
            }

            auto type = ConvertPinType(kind.Value());
            auto key = IndexedKey(input.names, side.prefix, index);
            if (!type || !key) {
                return Result<void>::Failure(!type ? type.Error() : key.Error());
            }

            auto added = side.direction == nodegraph::PinDirection::Input
                             ? AddInput(out, key.Value(), type.Value(), StringView())
                             : AddOutput(out, key.Value(), type.Value(), StringView());
            if (!added) {
                return added;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> OperatorDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionOperator::GetInputName: A–Z, 그 뒤는 "Input <n>". Rocks는 27번째부터
    // 키 Input<n>(1부터)이고 표시 이름은 비운다(동적 표시 이름은 정적 data나 속성 바이트만
    // 가리킨다).
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<OperatorProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto inputs =
        reader.Value().OpenStructArray("dynamicInputs", GeneratedType<OperatorInput>::Schema());
    if (!inputs) {
        return Result<void>::Failure(inputs.Error());
    }

    for (usize index = 0; index < inputs.Value().Count(); ++index) {
        const bool lettered = index < OperatorLetterInputs;
        auto key = lettered ? Intern(input.names, OperatorLetters[index])
                            : IndexedKey(input.names, "Input", index + 1);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        const StringView display = lettered ? OperatorLetters[index] : StringView();
        if (auto added = AddInput(out, key.Value(), MaterialPinType::Numeric, display); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> TransformPositionDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionTransformPosition::GetInput(bUsesPeriodicWorldPosition·
    // bUsesFirstPersonInterpolationAlpha — PostEditChangeProperty가 원본·대상 공간으로 정한다).
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<TransformPositionProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto source = reader.Value().ReadInteger("transformSourceType", 0);
    auto destination = reader.Value().ReadInteger("transformType", 0);
    if (!source || !destination) {
        return Result<void>::Failure(!source ? source.Error() : destination.Error());
    }

    const auto uses = [&](PositionTransformSpace space) noexcept {
        return source.Value() == static_cast<int64>(space) ||
               destination.Value() == static_cast<int64>(space);
    };
    if (uses(PositionTransformSpace::PeriodicWorld)) {
        if (auto added = AddNamedInput(input, out, "PeriodicWorldTileSize",
                                       MaterialPinType::Numeric, "Periodic World Tile Size");
            !added) {
            return added;
        }
    }

    if (uses(PositionTransformSpace::FirstPersonTranslatedWorld)) {
        return AddNamedInput(input, out, "FirstPersonInterpolationAlpha", MaterialPinType::Numeric,
                             "First Person Interpolation Alpha");
    }

    return Result<void>::Success();
}

Result<void> TextureObjectFromCollectionDynamicPins(const DynamicPinsInput& input,
                                                    PinList& out) noexcept {
    // UE MaterialValueTypeFromTextureCollectionMemberType.
    SystemAllocator scratch;
    auto reader =
        Open(scratch, input, GeneratedType<TextureObjectFromCollectionProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto kind = reader.Value().ReadInteger("textureType", 0);
    if (!kind) {
        return Result<void>::Failure(kind.Error());
    }

    MaterialPinType type = MaterialPinType::Texture2D;
    switch (static_cast<TextureCollectionMemberType>(kind.Value())) {
    case TextureCollectionMemberType::Texture2D:
        type = MaterialPinType::Texture2D;
        break;
    case TextureCollectionMemberType::TextureCube:
        type = MaterialPinType::TextureCube;
        break;
    case TextureCollectionMemberType::Texture2DArray:
        type = MaterialPinType::Texture2DArray;
        break;
    case TextureCollectionMemberType::TextureCubeArray:
        type = MaterialPinType::TextureCubeArray;
        break;
    case TextureCollectionMemberType::Texture3D:
        type = MaterialPinType::Texture3D;
        break;
    default:
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return AddNamedOutput(input, out, "Result", type, "");
}

Result<void> DynamicParameterOutputPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionDynamicParameter::GetOutputs(출력 0–3의 이름이 ParamNames).
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<DynamicParameterProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    Array<StringView> names(scratch);
    if (auto read = reader.Value().ReadTextArray("paramNames", names); !read) {
        return read;
    }

    for (usize index = 0; index < DynamicParameterOutputs; ++index) {
        const StringView display =
            index < names.Size() ? names[index] : DynamicParameterDefaultNames[index];
        if (auto added = AddNamedOutput(input, out, DynamicParameterDefaultNames[index],
                                        MaterialPinType::Float1, display);
            !added) {
            return added;
        }
    }

    if (auto rgb = AddNamedOutput(input, out, "RGB", MaterialPinType::Float3, "RGB"); !rgb) {
        return rgb;
    }

    return AddNamedOutput(input, out, "RGBA", MaterialPinType::Float4, "RGBA");
}

Result<void> FunctionCallDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionMaterialFunctionCall::GetInput·GetInputName·GetOutputs.
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<FunctionCallProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto inputs =
        reader.Value().OpenStructArray("inputs", GeneratedType<FunctionCallInput>::Schema());
    if (!inputs) {
        return Result<void>::Failure(inputs.Error());
    }

    for (usize index = 0; index < inputs.Value().Count(); ++index) {
        auto element = inputs.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto id = element.Value().ReadGuid("id", Guid{});
        auto text = element.Value().ReadText("inputName");
        auto kind = element.Value().ReadInteger("inputType",
                                                static_cast<int64>(FunctionInputType::Vector3));
        if (!id || !text || !kind) {
            return Result<void>::Failure(!id ? id.Error() : !text ? text.Error() : kind.Error());
        }

        if (!IsKnownFunctionInputType(kind.Value())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto key = GuidKey(input.names, "P_", id.Value());
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        if (auto added =
                AddInput(out, key.Value(), FunctionInputPinType(kind.Value()), text.Value());
            !added) {
            return added;
        }
    }

    auto outputs =
        reader.Value().OpenStructArray("outputs", GeneratedType<FunctionCallOutput>::Schema());
    if (!outputs) {
        return Result<void>::Failure(outputs.Error());
    }

    for (usize index = 0; index < outputs.Value().Count(); ++index) {
        auto element = outputs.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto id = element.Value().ReadGuid("id", Guid{});
        auto text = element.Value().ReadText("outputName");
        if (!id || !text) {
            return Result<void>::Failure(!id ? id.Error() : text.Error());
        }

        auto key = GuidKey(input.names, "P_", id.Value());
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        // 출력 타입은 함수 출력 노드에 연결된 값이 정한다(번역 — UE GetRealInputValueType).
        if (auto added = AddOutput(out, key.Value(), MaterialPinType::Any, text.Value()); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> FunctionInputDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionFunctionInput::GetInputValueType·GetOutputValueType(InputType).
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<FunctionInputProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto kind =
        reader.Value().ReadInteger("inputType", static_cast<int64>(FunctionInputType::Vector3));
    if (!kind) {
        return Result<void>::Failure(kind.Error());
    }

    if (!IsKnownFunctionInputType(kind.Value())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const MaterialPinType type = FunctionInputPinType(kind.Value());
    if (auto added = AddNamedInput(input, out, "Preview", type, "Preview"); !added) {
        return added;
    }

    return AddNamedOutput(input, out, "Result", type, "");
}

Result<void> AggregateDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionAggregate::GetInput·GetInputName·GetPinValueType: 속성마다 입력(설정)과
    // 출력(읽기) 한 쌍이다.
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<AggregateProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto kind = reader.Value().ReadInteger("kind", 0);
    if (!kind) {
        return Result<void>::Failure(kind.Error());
    }

    Array<StringView> names(scratch);
    if (auto read = reader.Value().ReadTextArray("attributeNames", names); !read) {
        return read;
    }

    const bool builtin = kind.Value() == static_cast<int64>(AggregateKind::MaterialAttributes);
    for (StringView name : names) {
        // 이름 없는 속성은 핀을 만들지 않는다(편집기가 진단한다).
        if (name.IsEmpty()) {
            continue;
        }

        const MaterialAttributeDefinition* definition =
            builtin ? FindBuiltinAttribute(name) : nullptr;
        // TODO(MPQ-MaterialAggregate): 사용자 aggregate 에셋이 생기면 그 속성 타입을 쓴다.
        const MaterialPinType type = definition ? definition->type : MaterialPinType::Any;
        auto key = Intern(input.names, name);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        if (auto added = AddInput(out, key.Value(), type, name); !added) {
            return added;
        }

        // 같은 이름의 출력은 키에 Out을 붙인다(핀 키는 방향과 상관없이 유일 — 19.13.3절).
        String outputKey(scratch);
        if (auto assigned = outputKey.TryAssign(name); !assigned) {
            return assigned;
        }

        if (auto appended = outputKey.TryAppend("Out"); !appended) {
            return appended;
        }

        auto output = Intern(input.names, outputKey.View());
        if (!output) {
            return Result<void>::Failure(output.Error());
        }

        if (auto added = AddOutput(out, output.Value(), type, name); !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> GetAttributesDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionGetMaterialAttributes(AttributeGetTypes마다 출력).
    return AddAttributePins(input, out, GeneratedType<GetAttributesProperties>::Schema(),
                            "attributeGetTypes", nodegraph::PinDirection::Output);
}

Result<void> SetAttributesDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionSetMaterialAttributes(AttributeSetTypes마다 입력).
    return AddAttributePins(input, out, GeneratedType<SetAttributesProperties>::Schema(),
                            "attributeSetTypes", nodegraph::PinDirection::Input);
}

Result<void> LayerStackDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionLayerStack::GetInput·GetInputName(LayerInputs).
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<LayerStackProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto inputs =
        reader.Value().OpenStructArray("layerInputs", GeneratedType<LayerStackInput>::Schema());
    if (!inputs) {
        return Result<void>::Failure(inputs.Error());
    }

    for (usize index = 0; index < inputs.Value().Count(); ++index) {
        auto element = inputs.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto text = element.Value().ReadText("inputName");
        auto kind = element.Value().ReadInteger("inputType",
                                                static_cast<int64>(FunctionInputType::Vector3));
        if (!text || !kind) {
            return Result<void>::Failure(!text ? text.Error() : kind.Error());
        }

        if (!IsKnownFunctionInputType(kind.Value())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto key = UserKey(input, text.Value(), "Input", index);
        if (!key) {
            return Result<void>::Failure(key.Error());
        }

        if (auto added =
                AddInput(out, key.Value(), FunctionInputPinType(kind.Value()), text.Value());
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> MaterialCacheDynamicPins(const DynamicPinsInput& input, PinList& out) noexcept {
    // UE UMaterialExpressionMaterialCache::GetInput·GetInputName·GetOutputs.
    SystemAllocator scratch;
    auto reader = Open(scratch, input, GeneratedType<MaterialCacheOutputProperties>::Schema());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto sample = reader.Value().ReadBool("isSample", false);
    if (!sample) {
        return Result<void>::Failure(sample.Error());
    }

    if (sample.Value()) {
        if (auto added =
                AddNamedInput(input, out, "Primitive", MaterialPinType::UInt1, "Primitive");
            !added) {
            return added;
        }

        if (auto added = AddNamedInput(input, out, "UV", MaterialPinType::Float2, "UV"); !added) {
            return added;
        }
    }

    auto attributes = reader.Value().OpenStructArray(
        "attributes", GeneratedType<MaterialCacheAttribute>::Schema());
    if (!attributes) {
        return Result<void>::Failure(attributes.Error());
    }

    for (usize index = 0; index < attributes.Value().Count(); ++index) {
        auto element = attributes.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto text = element.Value().ReadText("decoration");
        auto kind = element.Value().ReadInteger("valueType", 0);
        if (!text || !kind) {
            return Result<void>::Failure(!text ? text.Error() : kind.Error());
        }

        if (kind.Value() < 0 || kind.Value() >= static_cast<int64>(MaterialPinTypeCount)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const MaterialPinType type = static_cast<MaterialPinType>(kind.Value());
        if (!sample.Value()) {
            auto key = UserKey(input, text.Value(), "Attribute", index);
            if (!key) {
                return Result<void>::Failure(key.Error());
            }

            if (auto added = AddInput(out, key.Value(), type, text.Value()); !added) {
                return added;
            }
        }
    }

    // UE ConstructOutputs: 출력은 견본 여부와 상관없이 속성마다 하나다. 같은 키의 입력이 있으면
    // 출력 키에 Out을 붙인다.
    for (usize index = 0; index < attributes.Value().Count(); ++index) {
        auto element = attributes.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto text = element.Value().ReadText("decoration");
        auto kind = element.Value().ReadInteger("valueType", 0);
        if (!text || !kind) {
            return Result<void>::Failure(!text ? text.Error() : kind.Error());
        }

        if (auto added = AddUserOutput(input, out, text.Value(), "Attribute", index,
                                       static_cast<MaterialPinType>(kind.Value()));
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::material::detail
