#include <Core/Error.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <MaterialSchema/MaterialAttributeTable.h>
#include <MaterialSchema/MaterialSchemaModule.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.generated.h>
#include <MaterialSchema/Nodes/Custom/CustomHlslProperties.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.h>
#include <MaterialSchema/Nodes/Functions/FunctionInputProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionInputProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AggregateProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AggregateProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/GetAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/GetAttributesProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/SetAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/SetAttributesProperties.h>
#include <MaterialSchema/Nodes/Math/ConvertProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ConvertProperties.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.generated.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.h>
#include <MaterialSchema/Nodes/Math/SwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Math/SwitchProperties.h>
#include <MaterialSchema/Nodes/Outputs/MaterialCacheOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Outputs/MaterialCacheOutputProperties.h>
#include <MaterialSchema/Nodes/Particles/DynamicParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Particles/DynamicParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/SparseVolumeTextureSampleProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectFromCollectionProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectFromCollectionProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureObjectProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/TextureSampleProperties.h>
#include <MaterialSchema/Nodes/Vectors/TransformPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/TransformPositionProperties.h>
#include <MaterialSchema/Nodes/VirtualTexture/RuntimeVirtualTextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/VirtualTexture/RuntimeVirtualTextureSampleProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/NodeTypeDescriptor.h>
#include <NodeGraph/NodeTypeRegistry.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <ObjectSerialization/StructRecord.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// 엔진 노드 동적 핀 시험이다(Docs/Design/MaterialSystem 19.13.3·19.13.6절, 단계 MS1b2): 속성
// struct를 리플렉션 codec으로 Record에 쓰고(SaveStructRecord — 실제 wire), 동적 핀 함수마다 속성
// 값 둘 이상에서 핀 키·타입·표시 이름이 UE 규칙과 같은지 본다. NodePropertyReader의 wire 가정
// (String·Name·enum·struct·배열)도 여기서 실제 codec과 맞춰진다.
using namespace rocks;
using namespace rocks::material;
namespace material_dynamic_pins_tests_file {

namespace {

constexpr ContextId PinsRegistryContext{0x4d41545044594eull, 1};

// MaterialSchema 타입을 등록한 Registry 하나와 NamePool이다(멤버 선언 순서가 수명 순서).
struct PinEnv {
    PinEnv() noexcept
        : types(allocator), modules(types, allocator), names(allocator),
          namesReady(names.Initialize().IsOk()),
          objects(allocator, types, PinsRegistryContext, Options()) {
        ready = namesReady && RegisterMaterialSchemaTypes(modules).IsOk();
    }

    ~PinEnv() {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(modules.Unregister().IsOk());
    }

    RegistryOptions Options() noexcept {
        RegistryOptions options;
        options.names = &names;
        return options;
    }

    Name Intern(StringView text) noexcept {
        auto name = names.TryIntern(text);
        return name ? name.Value() : Name();
    }

    template <class S>
    Array<uint8> Save(const S& value) noexcept {
        auto lease = types.AcquireValueType(GeneratedType<S>::Id(), TypeKind::Struct);
        if (!lease) {
            return Array<uint8>(allocator);
        }

        auto saved =
            SaveStructRecord(allocator, objects, objects.OwnerAccess(), lease.Value(), &value);
        return saved ? Move(saved.Value()) : Array<uint8>(allocator);
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    NamePool names;
    bool namesReady = false;
    ObjectRegistry objects;
    bool ready = false;
};

const MaterialNodeDefinition* FindDefinition(StringView type) noexcept {
    for (Span<const MaterialNodeDefinition> group : BuiltinMaterialNodeGroups()) {
        for (const MaterialNodeDefinition& definition : group) {
            if (definition.type == type) {
                return &definition;
            }
        }
    }

    return nullptr;
}

// 동적 핀 입력 문서다. 동적 핀 표시 이름은 노드 속성 바이트를 가리킬 수 있으므로(NodeGraph 6.2절)
// 핀 목록을 보는 동안 살아 있어야 한다.
struct NodeFixture {
    explicit NodeFixture(Allocator& allocator) noexcept
        : document(allocator), graph(allocator), node(allocator) {
    }

    nodegraph::GraphDocument document;
    nodegraph::Graph graph;
    nodegraph::GraphNode node;
};

// type 노드의 동적 핀을 bytes 속성으로 계산한다. fixture는 out을 보는 동안 살아 있어야 한다.
Result<void> CollectDynamicPins(PinEnv& env, StringView type, const Array<uint8>& bytes,
                                NodeFixture& fixture, nodegraph::PinList& out) noexcept {
    const MaterialNodeDefinition* definition = FindDefinition(type);
    if (definition == nullptr || definition->dynamicPins == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    fixture.node.properties.bytes.Clear();
    if (auto copied = fixture.node.properties.bytes.TryAppend(bytes.View()); !copied) {
        return copied;
    }

    const nodegraph::DynamicPinsInput input{fixture.document, fixture.graph, fixture.node,
                                            env.names};
    return definition->dynamicPins(input, out);
}

// 핀 하나가 (키, 방향, 타입, 표시 이름)인가.
bool IsPin(PinEnv& env, const nodegraph::PinDescriptor& pin, StringView key,
           nodegraph::PinDirection direction, MaterialPinType type, StringView display) noexcept {
    return pin.key == env.Intern(key) && pin.direction == direction &&
           pin.type == ToPinTypeId(type) && pin.displayName == display;
}

constexpr nodegraph::PinDirection In = nodegraph::PinDirection::Input;
constexpr nodegraph::PinDirection Out = nodegraph::PinDirection::Output;

// 어떤 노드 enum에도 없는 값이다(uint8 enum의 마지막 값 — 동적 핀 함수가 거부해야 한다).
constexpr int64 UnknownEnumValue = 0xFF;

// schema의 정수 필드 하나만 value로 쓴 Record다. 리플렉션 codec은 enum에 없는 값을 저장하지
// 않으므로 (저장 검사) 손상·다른 판의 속성 바이트를 이렇게 만든다. 필드가 없거나 쓰기가 실패하면 빈
// 배열이다.
Array<uint8> IntegerFieldRecord(Allocator& allocator, const TypeSchema& schema, StringView field,
                                int64 value) noexcept {
    Guid id{};
    for (const FieldSchema& candidate : schema.fields) {
        if (StringView(candidate.name.Data(), candidate.name.Size()) == field) {
            id = candidate.id;
        }
    }

    ArchiveWriter writer(allocator);
    auto record = RecordWriter::Begin(writer, schema.id, schema.version);
    if (id == Guid{} || !record) {
        return Array<uint8>(allocator);
    }

    RecordWriter& out = record.Value();
    if (!out.BeginField(id) || !writer.WriteI64(value) || !out.EndField(WireType::I64) ||
        !out.End()) {
        return Array<uint8>(allocator);
    }

    return writer.TakeBytes();
}

// Operator 시험 입력 수다(문자 A–Z 26개를 넘겨 Input<n> 키까지 본다).
constexpr usize OperatorTestInputs = 28;

} // namespace

// 텍스처 샘플: MipValueMode에 따라 MipValue 또는 DX·DY가 생기고 None이면 없다(UE GetInput).
TEST(MaterialDynamicPins_TextureSampleMipInputs) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    struct Case {
        TextureMipValueMode mode;
        usize count;
    };

    const Case cases[] = {{TextureMipValueMode::None, 0},
                          {TextureMipValueMode::MipLevel, 1},
                          {TextureMipValueMode::MipBias, 1},
                          {TextureMipValueMode::Derivative, 2}};
    for (const Case& testCase : cases) {
        TextureSampleProperties properties(env.allocator);
        properties.mipValueMode = testCase.mode;
        nodegraph::PinList pins(env.allocator);
        REQUIRE(
            CollectDynamicPins(env, "Material.TextureSample", env.Save(properties), fixture, pins));
        REQUIRE(pins.Size() == testCase.count);
        if (testCase.mode == TextureMipValueMode::MipLevel) {
            CHECK(IsPin(env, pins.View()[0], "MipValue", In, MaterialPinType::Numeric, "MipLevel"));
        } else if (testCase.mode == TextureMipValueMode::MipBias) {
            CHECK(IsPin(env, pins.View()[0], "MipValue", In, MaterialPinType::Numeric, "MipBias"));
        } else if (testCase.mode == TextureMipValueMode::Derivative) {
            CHECK(IsPin(env, pins.View()[0], "CoordinatesDX", In, MaterialPinType::Numeric,
                        "DDX(UVs)"));
            CHECK(IsPin(env, pins.View()[1], "CoordinatesDY", In, MaterialPinType::Numeric,
                        "DDY(UVs)"));
        }
    }

    // 속성을 쓰지 않은 노드는 기본값(None)이다.
    nodegraph::PinList empty(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.TextureSample", Array<uint8>(env.allocator), fixture,
                               empty));
    CHECK(empty.Size() == 0);
}

// SVT 샘플: 텍스처 샘플과 같은 mip 입력 규칙이다(UE UMaterialExpressionSparseVolumeTextureSample).
TEST(MaterialDynamicPins_SparseVolumeTextureMipInputs) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    SparseVolumeTextureSampleProperties properties;
    nodegraph::PinList none(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.SparseVolumeTextureSample", env.Save(properties),
                               fixture, none));
    CHECK(none.Size() == 0);

    properties.mipValueMode = TextureMipValueMode::Derivative;
    nodegraph::PinList derivative(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.SparseVolumeTextureSample", env.Save(properties),
                               fixture, derivative));
    REQUIRE(derivative.Size() == 2);
    CHECK(IsPin(env, derivative.View()[0], "CoordinatesDX", In, MaterialPinType::Numeric,
                "DDX(UVs)"));
    CHECK(IsPin(env, derivative.View()[1], "CoordinatesDY", In, MaterialPinType::Numeric,
                "DDY(UVs)"));

    // 모르는 mip 모드는 거부한다.
    const Array<uint8> bytes = IntegerFieldRecord(
        env.allocator, GeneratedType<SparseVolumeTextureSampleProperties>::Schema(), "mipValueMode",
        UnknownEnumValue);
    REQUIRE(!bytes.IsEmpty());
    nodegraph::PinList bad(env.allocator);
    CHECK(CollectDynamicPins(env, "Material.SparseVolumeTextureSample", bytes, fixture, bad)
              .Error() == ErrorCode::InvalidArgument);
}

// RVT 샘플: 미분 모드는 UV·World 이름이 다르다.
TEST(MaterialDynamicPins_RuntimeVirtualTextureMipInputs) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    RuntimeVirtualTextureSampleProperties properties;
    properties.mipValueMode = RuntimeVirtualTextureMipValueMode::DerivativeWorld;
    nodegraph::PinList pins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.RuntimeVirtualTextureSample", env.Save(properties),
                               fixture, pins));
    REQUIRE(pins.Size() == 2);
    CHECK(IsPin(env, pins.View()[0], "DDX", In, MaterialPinType::Numeric, "DDX (World)"));
    CHECK(IsPin(env, pins.View()[1], "DDY", In, MaterialPinType::Numeric, "DDY (World)"));

    properties.mipValueMode = RuntimeVirtualTextureMipValueMode::MipBias;
    nodegraph::PinList bias(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.RuntimeVirtualTextureSample", env.Save(properties),
                               fixture, bias));
    REQUIRE(bias.Size() == 1);
    CHECK(IsPin(env, bias.View()[0], "MipValue", In, MaterialPinType::Numeric, "Mip Bias"));
}

// 텍스처 객체: 출력 타입이 참조의 기대 클래스를 따른다(비면 Texture2D).
TEST(MaterialDynamicPins_TextureObjectOutputType) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    TextureObjectProperties properties;
    nodegraph::PinList plain(env.allocator);
    REQUIRE(
        CollectDynamicPins(env, "Material.TextureObject", env.Save(properties), fixture, plain));
    REQUIRE(plain.Size() == 1);
    CHECK(IsPin(env, plain.View()[0], "Result", Out, MaterialPinType::Texture2D, ""));

    properties.texture.expectedClass = env.Intern("TextureCube");
    nodegraph::PinList cube(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.TextureObject", env.Save(properties), fixture, cube));
    REQUIRE(cube.Size() == 1);
    CHECK(IsPin(env, cube.View()[0], "Result", Out, MaterialPinType::TextureCube, ""));

    // 파라미터 노드도 같은 규칙이다(VolumeTexture는 Texture3D).
    TextureObjectParameterProperties parameter(env.allocator);
    parameter.texture.expectedClass = env.Intern("VolumeTexture");
    nodegraph::PinList parameterPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.TextureObjectParameter", env.Save(parameter), fixture,
                               parameterPins));
    REQUIRE(parameterPins.Size() == 1);
    CHECK(IsPin(env, parameterPins.View()[0], "Result", Out, MaterialPinType::Texture3D, ""));

    parameter.texture.expectedClass = env.Intern("Texture2DArray");
    nodegraph::PinList arrayPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.TextureObjectParameter", env.Save(parameter), fixture,
                               arrayPins));
    REQUIRE(arrayPins.Size() == 1);
    CHECK(IsPin(env, arrayPins.View()[0], "Result", Out, MaterialPinType::Texture2DArray, ""));

    TextureObjectFromCollectionProperties collection;
    collection.textureType = TextureCollectionMemberType::Texture3D;
    nodegraph::PinList volume(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.TextureObjectFromCollection", env.Save(collection),
                               fixture, volume));
    REQUIRE(volume.Size() == 1);
    CHECK(IsPin(env, volume.View()[0], "Result", Out, MaterialPinType::Texture3D, ""));

    collection.textureType = TextureCollectionMemberType::TextureCubeArray;
    nodegraph::PinList cubeArray(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.TextureObjectFromCollection", env.Save(collection),
                               fixture, cubeArray));
    REQUIRE(cubeArray.Size() == 1);
    CHECK(IsPin(env, cubeArray.View()[0], "Result", Out, MaterialPinType::TextureCubeArray, ""));

    // 모르는 멤버 종류는 거부한다.
    const Array<uint8> bytes = IntegerFieldRecord(
        env.allocator, GeneratedType<TextureObjectFromCollectionProperties>::Schema(),
        "textureType", UnknownEnumValue);
    REQUIRE(!bytes.IsEmpty());
    nodegraph::PinList bad(env.allocator);
    CHECK(CollectDynamicPins(env, "Material.TextureObjectFromCollection", bytes, fixture, bad)
              .Error() == ErrorCode::InvalidArgument);
}

// Custom: 이름 있는 입력은 그 이름, 빈 이름은 Input<n>이고, 출력 Result 타입은 outputType,
// 추가 출력은 이름·타입대로다. 추가 출력이 있으면 주 출력 표시가 "return"이고 이름 없는 추가 출력은
// 핀이 없다(UE RebuildOutputs). 입력과 같은 출력 키는 Out을 붙이고, 출력끼리 겹치면 실패한다.
TEST(MaterialDynamicPins_CustomInputsAndOutputs) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    CustomHlslProperties properties(env.allocator);
    properties.outputType = CustomOutputType::Float2;
    CustomInput named;
    named.inputName = env.Intern("Tint");
    CustomInput unnamed;
    CustomOutput extra;
    extra.outputName = env.Intern("Mask");
    extra.outputType = CustomOutputType::Float4;
    CustomOutput anonymous;
    CustomOutput sameAsInput;
    sameAsInput.outputName = env.Intern("Tint");
    sameAsInput.outputType = CustomOutputType::Float1;
    CustomOutput sameAsResult;
    sameAsResult.outputName = env.Intern("Result");
    sameAsResult.outputType = CustomOutputType::Float3;
    REQUIRE(properties.inputs.TryPushBack(named));
    REQUIRE(properties.inputs.TryPushBack(unnamed));
    REQUIRE(properties.additionalOutputs.TryPushBack(extra));
    REQUIRE(properties.additionalOutputs.TryPushBack(anonymous));
    REQUIRE(properties.additionalOutputs.TryPushBack(sameAsInput));
    nodegraph::PinList pins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.CustomHlsl", env.Save(properties), fixture, pins));
    REQUIRE(pins.Size() == 5);
    CHECK(IsPin(env, pins.View()[0], "Tint", In, MaterialPinType::Any, "Tint"));
    CHECK(IsPin(env, pins.View()[1], "Input1", In, MaterialPinType::Any, ""));
    CHECK(IsPin(env, pins.View()[2], "Result", Out, MaterialPinType::Float2, "return"));
    CHECK(IsPin(env, pins.View()[3], "Mask", Out, MaterialPinType::Float4, "Mask"));
    CHECK(IsPin(env, pins.View()[4], "TintOut", Out, MaterialPinType::Float1, "Tint"));
    CHECK(pins.View()[0].valuePolicy == nodegraph::PinValuePolicy::Hidden);

    // 추가 출력 이름이 주 출력 키 Result와 같으면 출력끼리 겹쳐 실패한다.
    CustomHlslProperties resultOutput(env.allocator);
    REQUIRE(resultOutput.additionalOutputs.TryPushBack(sameAsResult));
    nodegraph::PinList resultClash(env.allocator);
    CHECK(
        CollectDynamicPins(env, "Material.CustomHlsl", env.Save(resultOutput), fixture, resultClash)
            .Error() == ErrorCode::InvalidArgument);

    // 이름 있는 입력 "Result"면 주 출력 키가 ResultOut이다.
    CustomHlslProperties resultInput(env.allocator);
    CustomInput result;
    result.inputName = env.Intern("Result");
    REQUIRE(resultInput.inputs.TryPushBack(result));
    nodegraph::PinList renamed(env.allocator);
    REQUIRE(
        CollectDynamicPins(env, "Material.CustomHlsl", env.Save(resultInput), fixture, renamed));
    REQUIRE(renamed.Size() == 2);
    CHECK(IsPin(env, renamed.View()[0], "Result", In, MaterialPinType::Any, "Result"));
    CHECK(IsPin(env, renamed.View()[1], "ResultOut", Out, MaterialPinType::Float3, ""));

    // 모르는 출력 종류는 거부한다.
    const Array<uint8> badBytes =
        IntegerFieldRecord(env.allocator, GeneratedType<CustomHlslProperties>::Schema(),
                           "outputType", UnknownEnumValue);
    REQUIRE(!badBytes.IsEmpty());
    nodegraph::PinList bad(env.allocator);
    CHECK(CollectDynamicPins(env, "Material.CustomHlsl", badBytes, fixture, bad).Error() ==
          ErrorCode::InvalidArgument);

    // 기본 속성: 입력 없음, 출력 Result는 UE 기본 Float3.
    nodegraph::PinList empty(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.CustomHlsl", Array<uint8>(env.allocator), fixture,
                               empty));
    REQUIRE(empty.Size() == 1);
    CHECK(IsPin(env, empty.View()[0], "Result", Out, MaterialPinType::Float3, ""));

    // 같은 이름의 입력 둘은 핀 키가 겹쳐 동적 핀 계산이 실패한다(NodeGraph GI13 — 진단 대상).
    CustomHlslProperties duplicate(env.allocator);
    REQUIRE(duplicate.inputs.TryPushBack(named));
    REQUIRE(duplicate.inputs.TryPushBack(named));
    nodegraph::PinList clash(env.allocator);
    CHECK(CollectDynamicPins(env, "Material.CustomHlsl", env.Save(duplicate), fixture, clash)
              .Error() == ErrorCode::InvalidArgument);

    // 같은 이름의 추가 출력 둘도 실패한다(순서에 따라 키가 바뀌지 않게 피하지 않는다 — UE도 실패).
    CustomHlslProperties twoMasks(env.allocator);
    REQUIRE(twoMasks.additionalOutputs.TryPushBack(extra));
    REQUIRE(twoMasks.additionalOutputs.TryPushBack(extra));
    nodegraph::PinList outputClash(env.allocator);
    CHECK(CollectDynamicPins(env, "Material.CustomHlsl", env.Save(twoMasks), fixture, outputClash)
              .Error() == ErrorCode::InvalidArgument);

    // 이름 없는 추가 출력만 있어도 주 출력 표시는 "return"이다(UE는 추가 출력 배열 크기로 정한다).
    CustomHlslProperties onlyAnonymous(env.allocator);
    REQUIRE(onlyAnonymous.additionalOutputs.TryPushBack(anonymous));
    nodegraph::PinList anonymousPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.CustomHlsl", env.Save(onlyAnonymous), fixture,
                               anonymousPins));
    REQUIRE(anonymousPins.Size() == 1);
    CHECK(IsPin(env, anonymousPins.View()[0], "Result", Out, MaterialPinType::Float3, "return"));
}

// Switch·Operator·Convert: 개수와 이름·폭이 속성대로다.
TEST(MaterialDynamicPins_SwitchOperatorConvert) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    SwitchProperties switchProperties(env.allocator);
    SwitchInput first;
    first.inputName = env.Intern("Grass");
    REQUIRE(switchProperties.inputs.TryPushBack(first));
    REQUIRE(switchProperties.inputs.TryPushBack(SwitchInput{}));
    nodegraph::PinList switchPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.Switch", env.Save(switchProperties), fixture,
                               switchPins));
    REQUIRE(switchPins.Size() == 2);
    CHECK(IsPin(env, switchPins.View()[0], "Grass", In, MaterialPinType::Numeric, "Grass"));
    CHECK(IsPin(env, switchPins.View()[1], "Input1", In, MaterialPinType::Numeric, ""));

    OperatorProperties operatorProperties(env.allocator);
    for (usize index = 0; index < OperatorTestInputs; ++index) {
        REQUIRE(operatorProperties.dynamicInputs.TryPushBack(OperatorInput{}));
    }

    nodegraph::PinList operatorPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.Operator", env.Save(operatorProperties), fixture,
                               operatorPins));
    REQUIRE(operatorPins.Size() == OperatorTestInputs);
    CHECK(IsPin(env, operatorPins.View()[0], "A", In, MaterialPinType::Numeric, "A"));
    CHECK(IsPin(env, operatorPins.View()[25], "Z", In, MaterialPinType::Numeric, "Z"));
    CHECK(IsPin(env, operatorPins.View()[26], "Input27", In, MaterialPinType::Numeric, ""));
    CHECK(IsPin(env, operatorPins.View()[27], "Input28", In, MaterialPinType::Numeric, ""));

    ConvertProperties convert(env.allocator);
    ConvertInput vector2;
    vector2.type = ConvertValueType::Vector2;
    ConvertOutput scalar;
    ConvertOutput vector3;
    vector3.type = ConvertValueType::Vector3;
    REQUIRE(convert.convertInputs.TryPushBack(vector2));
    REQUIRE(convert.convertOutputs.TryPushBack(scalar));
    REQUIRE(convert.convertOutputs.TryPushBack(vector3));
    nodegraph::PinList convertPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.Convert", env.Save(convert), fixture, convertPins));
    REQUIRE(convertPins.Size() == 3);
    CHECK(IsPin(env, convertPins.View()[0], "Input0", In, MaterialPinType::Float2, ""));
    CHECK(IsPin(env, convertPins.View()[1], "Output0", Out, MaterialPinType::Float1, ""));
    CHECK(IsPin(env, convertPins.View()[2], "Output1", Out, MaterialPinType::Float3, ""));
}

// TransformPosition: 주기 world·1인칭 공간을 쓸 때만 보조 입력이 있다.
TEST(MaterialDynamicPins_TransformPositionInputs) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    TransformPositionProperties properties;
    nodegraph::PinList none(env.allocator);
    REQUIRE(
        CollectDynamicPins(env, "Material.TransformPosition", env.Save(properties), fixture, none));
    CHECK(none.Size() == 0);

    properties.transformSourceType = PositionTransformSpace::PeriodicWorld;
    properties.transformType = PositionTransformSpace::FirstPersonTranslatedWorld;
    nodegraph::PinList both(env.allocator);
    REQUIRE(
        CollectDynamicPins(env, "Material.TransformPosition", env.Save(properties), fixture, both));
    REQUIRE(both.Size() == 2);
    CHECK(IsPin(env, both.View()[0], "PeriodicWorldTileSize", In, MaterialPinType::Numeric,
                "Periodic World Tile Size"));
    CHECK(IsPin(env, both.View()[1], "FirstPersonInterpolationAlpha", In, MaterialPinType::Numeric,
                "First Person Interpolation Alpha"));
}

// DynamicParameter: 출력 키는 Param1–Param4로 고정이고 표시 이름은 paramNames(없으면 기본 이름)다.
TEST(MaterialDynamicPins_DynamicParameterOutputs) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    DynamicParameterProperties properties(env.allocator);
    nodegraph::PinList defaults(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.DynamicParameter", env.Save(properties), fixture,
                               defaults));
    REQUIRE(defaults.Size() == 6);
    CHECK(IsPin(env, defaults.View()[0], "Param1", Out, MaterialPinType::Float1, "Param1"));
    CHECK(IsPin(env, defaults.View()[5], "RGBA", Out, MaterialPinType::Float4, "RGBA"));

    String speed(env.allocator);
    REQUIRE(speed.TryAssign("Speed"));
    REQUIRE(properties.paramNames.TryPushBack(Move(speed)));
    const Array<uint8> bytes = env.Save(properties);
    nodegraph::PinList named(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.DynamicParameter", bytes, fixture, named));
    REQUIRE(named.Size() == 6);
    CHECK(named.View()[0].key == env.Intern("Param1"));
    CHECK(named.View()[0].displayName == StringView("Speed"));
    CHECK(named.View()[1].displayName == StringView("Param2"));
}

// 함수 호출·함수 입력: 키는 P_<id 32자 hex>, 타입은 inputType이다.
TEST(MaterialDynamicPins_FunctionCallAndInput) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    FunctionCallProperties call(env.allocator);
    FunctionCallInput base;
    base.id = Guid{0x0123456789abcdefull, 0x00000000000000ffull};
    base.inputName = env.Intern("Base");
    base.inputType = FunctionInputType::Scalar;
    FunctionCallInput mask;
    mask.id = Guid{3, 4};
    mask.inputName = env.Intern("Mask");
    mask.inputType = FunctionInputType::Texture2D;
    FunctionCallOutput result;
    result.id = Guid{1, 2};
    result.outputName = env.Intern("Out");
    FunctionCallOutput alpha;
    alpha.id = Guid{0xfedcba9876543210ull, 0};
    alpha.outputName = env.Intern("Alpha");
    REQUIRE(call.inputs.TryPushBack(base));
    REQUIRE(call.inputs.TryPushBack(mask));
    REQUIRE(call.outputs.TryPushBack(result));
    REQUIRE(call.outputs.TryPushBack(alpha));
    nodegraph::PinList callPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.FunctionCall", env.Save(call), fixture, callPins));
    REQUIRE(callPins.Size() == 4);
    CHECK(IsPin(env, callPins.View()[0], "P_0123456789abcdef00000000000000ff", In,
                MaterialPinType::Float1, "Base"));
    CHECK(IsPin(env, callPins.View()[1], "P_00000000000000030000000000000004", In,
                MaterialPinType::Texture2D, "Mask"));
    CHECK(IsPin(env, callPins.View()[2], "P_00000000000000010000000000000002", Out,
                MaterialPinType::Any, "Out"));
    CHECK(IsPin(env, callPins.View()[3], "P_fedcba98765432100000000000000000", Out,
                MaterialPinType::Any, "Alpha"));

    FunctionInputProperties input(env.allocator);
    nodegraph::PinList vector3(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.FunctionInput", env.Save(input), fixture, vector3));
    REQUIRE(vector3.Size() == 2);
    CHECK(IsPin(env, vector3.View()[0], "Preview", In, MaterialPinType::Float3, "Preview"));
    CHECK(IsPin(env, vector3.View()[1], "Result", Out, MaterialPinType::Float3, ""));

    // 모르는 입력 종류는 거부한다(함수 호출·레이어 입력 원소도 같은 검사다).
    const Array<uint8> badInput =
        IntegerFieldRecord(env.allocator, GeneratedType<FunctionInputProperties>::Schema(),
                           "inputType", UnknownEnumValue);
    REQUIRE(!badInput.IsEmpty());
    nodegraph::PinList bad(env.allocator);
    CHECK(CollectDynamicPins(env, "Material.FunctionInput", badInput, fixture, bad).Error() ==
          ErrorCode::InvalidArgument);

    input.inputType = FunctionInputType::Surface;
    nodegraph::PinList surface(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.FunctionInput", env.Save(input), fixture, surface));
    REQUIRE(surface.Size() == 2);
    CHECK(surface.View()[1].type == ToPinTypeId(MaterialPinType::Surface));
}

// 속성 노드: Get·Set은 A_<id>와 내장 속성 이름·타입, 모르는 id는 Any이고, Aggregate는 속성
// 이름마다 입력·출력 한 쌍이다.
TEST(MaterialDynamicPins_AttributeNodes) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    const MaterialAttributeDefinition& baseColor = BuiltinMaterialAttributes()[0];
    REQUIRE(baseColor.name == StringView("BaseColor"));
    GetAttributesProperties get(env.allocator);
    REQUIRE(get.attributeGetTypes.TryPushBack(baseColor.id));
    REQUIRE(get.attributeGetTypes.TryPushBack(Guid{7, 7}));
    nodegraph::PinList getPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.GetAttributes", env.Save(get), fixture, getPins));
    REQUIRE(getPins.Size() == 2);
    CHECK(IsPin(env, getPins.View()[0], "A_69b8d33616ed4d499aa497292f050f7a", Out,
                MaterialPinType::Float3, "BaseColor"));
    CHECK(IsPin(env, getPins.View()[1], "A_00000000000000070000000000000007", Out,
                MaterialPinType::Any, ""));

    const MaterialAttributeDefinition* metallic = nullptr;
    for (const MaterialAttributeDefinition& definition : BuiltinMaterialAttributes()) {
        if (definition.name == StringView("Metallic")) {
            metallic = &definition;
        }
    }

    REQUIRE(metallic != nullptr);
    SetAttributesProperties set(env.allocator);
    REQUIRE(set.attributeSetTypes.TryPushBack(baseColor.id));
    REQUIRE(set.attributeSetTypes.TryPushBack(metallic->id));
    nodegraph::PinList setPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.SetAttributes", env.Save(set), fixture, setPins));
    REQUIRE(setPins.Size() == 2);
    CHECK(IsPin(env, setPins.View()[0], "A_69b8d33616ed4d499aa497292f050f7a", In,
                MaterialPinType::Float3, "BaseColor"));
    CHECK(setPins.View()[1].direction == In);
    CHECK(setPins.View()[1].type == ToPinTypeId(MaterialPinType::Float1));
    CHECK(setPins.View()[1].displayName == StringView("Metallic"));

    // 같은 속성 id 둘은 핀 키가 겹쳐 거부된다.
    REQUIRE(set.attributeSetTypes.TryPushBack(baseColor.id));
    nodegraph::PinList twice(env.allocator);
    CHECK(
        CollectDynamicPins(env, "Material.SetAttributes", env.Save(set), fixture, twice).Error() ==
        ErrorCode::InvalidArgument);

    // Aggregate: 이름마다 입력·출력(키 + Out), 빈 이름은 핀이 없다.
    AggregateProperties aggregate(env.allocator);
    REQUIRE(aggregate.attributeNames.TryPushBack(env.Intern("BaseColor")));
    REQUIRE(aggregate.attributeNames.TryPushBack(Name()));
    REQUIRE(aggregate.attributeNames.TryPushBack(env.Intern("Metallic")));
    nodegraph::PinList aggregatePins(env.allocator);
    REQUIRE(
        CollectDynamicPins(env, "Material.Aggregate", env.Save(aggregate), fixture, aggregatePins));
    REQUIRE(aggregatePins.Size() == 4);
    CHECK(
        IsPin(env, aggregatePins.View()[0], "BaseColor", In, MaterialPinType::Float3, "BaseColor"));
    CHECK(IsPin(env, aggregatePins.View()[1], "BaseColorOut", Out, MaterialPinType::Float3,
                "BaseColor"));
    CHECK(IsPin(env, aggregatePins.View()[2], "Metallic", In, MaterialPinType::Float1, "Metallic"));
    CHECK(IsPin(env, aggregatePins.View()[3], "MetallicOut", Out, MaterialPinType::Float1,
                "Metallic"));

    // 사용자 aggregate(TODO MPQ-MaterialAggregate)의 속성은 타입을 모른다(Any).
    aggregate.kind = AggregateKind::UserDefined;
    nodegraph::PinList user(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.Aggregate", env.Save(aggregate), fixture, user));
    REQUIRE(user.Size() == 4);
    CHECK(user.View()[0].type == ToPinTypeId(MaterialPinType::Any));

    // 같은 속성 이름 둘은 거부한다.
    AggregateProperties repeated(env.allocator);
    REQUIRE(repeated.attributeNames.TryPushBack(env.Intern("BaseColor")));
    REQUIRE(repeated.attributeNames.TryPushBack(env.Intern("BaseColor")));
    nodegraph::PinList twicePins(env.allocator);
    CHECK(CollectDynamicPins(env, "Material.Aggregate", env.Save(repeated), fixture, twicePins)
              .Error() == ErrorCode::InvalidArgument);
}

// LayerStack·MaterialCache: 목록대로다. MaterialCache 출력은 늘 속성마다 있고, 입력은 견본이
// 아니면 속성마다, 견본이면 Primitive·UV다(UE GetInput·ConstructOutputs).
TEST(MaterialDynamicPins_LayerStackAndMaterialCache) {
    PinEnv env;
    REQUIRE(env.ready);
    NodeFixture fixture(env.allocator);
    LayerStackProperties stack(env.allocator);
    LayerStackInput wet;
    wet.inputName = env.Intern("Wetness");
    wet.inputType = FunctionInputType::Scalar;
    LayerStackInput unnamedLayer;
    unnamedLayer.inputType = FunctionInputType::MaterialAttributes;
    REQUIRE(stack.layerInputs.TryPushBack(wet));
    REQUIRE(stack.layerInputs.TryPushBack(unnamedLayer));
    nodegraph::PinList stackPins(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.LayerStack", env.Save(stack), fixture, stackPins));
    REQUIRE(stackPins.Size() == 2);
    CHECK(IsPin(env, stackPins.View()[0], "Wetness", In, MaterialPinType::Float1, "Wetness"));
    CHECK(IsPin(env, stackPins.View()[1], "Input1", In, MaterialPinType::MaterialAttributes, ""));

    MaterialCacheOutputProperties cache(env.allocator);
    MaterialCacheAttribute color(env.allocator);
    REQUIRE(color.decoration.TryAssign("Color"));
    color.valueType = static_cast<uint32>(MaterialPinType::Float3);
    MaterialCacheAttribute roughness(env.allocator);
    REQUIRE(roughness.decoration.TryAssign("Roughness"));
    roughness.valueType = static_cast<uint32>(MaterialPinType::Float1);
    REQUIRE(cache.attributes.TryPushBack(Move(color)));
    REQUIRE(cache.attributes.TryPushBack(Move(roughness)));
    nodegraph::PinList write(env.allocator);
    REQUIRE(
        CollectDynamicPins(env, "Material.MaterialCacheOutput", env.Save(cache), fixture, write));
    REQUIRE(write.Size() == 4);
    CHECK(IsPin(env, write.View()[0], "Color", In, MaterialPinType::Float3, "Color"));
    CHECK(IsPin(env, write.View()[1], "Roughness", In, MaterialPinType::Float1, "Roughness"));
    CHECK(IsPin(env, write.View()[2], "ColorOut", Out, MaterialPinType::Float3, "Color"));
    CHECK(IsPin(env, write.View()[3], "RoughnessOut", Out, MaterialPinType::Float1, "Roughness"));

    cache.isSample = true;
    nodegraph::PinList sample(env.allocator);
    REQUIRE(
        CollectDynamicPins(env, "Material.MaterialCacheOutput", env.Save(cache), fixture, sample));
    REQUIRE(sample.Size() == 4);
    CHECK(IsPin(env, sample.View()[0], "Primitive", In, MaterialPinType::UInt1, "Primitive"));
    CHECK(IsPin(env, sample.View()[1], "UV", In, MaterialPinType::Float2, "UV"));
    // 출력 키는 쓰기 모드와 같다(isSample을 바꿔도 출력 연결이 그대로다).
    CHECK(IsPin(env, sample.View()[2], "ColorOut", Out, MaterialPinType::Float3, "Color"));
    CHECK(IsPin(env, sample.View()[3], "RoughnessOut", Out, MaterialPinType::Float1, "Roughness"));

    // 이름 없는 속성은 Attribute<n>·Attribute<n>Out이고, 견본 모드의 UV 속성은 입력 UV와 겹치지
    // 않는다.
    MaterialCacheOutputProperties unnamedCache(env.allocator);
    MaterialCacheAttribute blank(env.allocator);
    blank.valueType = static_cast<uint32>(MaterialPinType::Float2);
    MaterialCacheAttribute uv(env.allocator);
    REQUIRE(uv.decoration.TryAssign("UV"));
    uv.valueType = static_cast<uint32>(MaterialPinType::Float2);
    REQUIRE(unnamedCache.attributes.TryPushBack(Move(blank)));
    REQUIRE(unnamedCache.attributes.TryPushBack(Move(uv)));
    nodegraph::PinList unnamedWrite(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.MaterialCacheOutput", env.Save(unnamedCache), fixture,
                               unnamedWrite));
    REQUIRE(unnamedWrite.Size() == 4);
    CHECK(IsPin(env, unnamedWrite.View()[0], "Attribute0", In, MaterialPinType::Float2, ""));
    CHECK(IsPin(env, unnamedWrite.View()[2], "Attribute0Out", Out, MaterialPinType::Float2, ""));
    unnamedCache.isSample = true;
    nodegraph::PinList unnamedSample(env.allocator);
    REQUIRE(CollectDynamicPins(env, "Material.MaterialCacheOutput", env.Save(unnamedCache), fixture,
                               unnamedSample));
    REQUIRE(unnamedSample.Size() == 4);
    CHECK(IsPin(env, unnamedSample.View()[1], "UV", In, MaterialPinType::Float2, "UV"));
    CHECK(IsPin(env, unnamedSample.View()[3], "UVOut", Out, MaterialPinType::Float2, "UV"));

    // 범위 밖 핀 타입 번호는 거부한다.
    cache.attributes[0].valueType = MaterialPinTypeCount;
    nodegraph::PinList bad(env.allocator);
    CHECK(CollectDynamicPins(env, "Material.MaterialCacheOutput", env.Save(cache), fixture, bad)
              .Error() == ErrorCode::InvalidArgument);
}

// 정적 핀과의 충돌: 등록표의 CollectPins(정적 + 동적)를 거쳐 사용자 이름이 정적 핀 키와 같으면
// 실패하는지 본다(19.13.3절 — Switch 입력 "Default", Aggregate 속성 "Result").
TEST(MaterialDynamicPins_StaticKeyCollisions) {
    PinEnv env;
    REQUIRE(env.ready);
    nodegraph::NodeTypeRegistry registry(env.allocator, env.names);
    MaterialNodeCatalog catalog(env.allocator);
    MaterialGraphSchema schema(MaterialGraphKind::Material, catalog, env.names, env.allocator);
    REQUIRE(registry.Initialize());
    REQUIRE(schema.Ready());
    REQUIRE(registry.RegisterSchema(schema, ModuleLease()));
    REQUIRE(catalog.RegisterGroups(registry, schema, BuiltinMaterialNodeGroups(), ModuleLease()));

    NodeFixture fixture(env.allocator);
    fixture.document.domain = schema.Domain();
    fixture.node.typeSchemaVersion = 1;
    const auto collect = [&](StringView type, const Array<uint8>& bytes,
                             nodegraph::PinList& out) noexcept {
        fixture.node.type = env.Intern(type);
        fixture.node.properties.bytes.Clear();
        if (auto copied = fixture.node.properties.bytes.TryAppend(bytes.View()); !copied) {
            return copied;
        }

        return registry.CollectPins(fixture.document, fixture.graph, fixture.node, out);
    };

    SwitchProperties fine(env.allocator);
    SwitchInput grass;
    grass.inputName = env.Intern("Grass");
    REQUIRE(fine.inputs.TryPushBack(grass));
    nodegraph::PinList finePins(env.allocator);
    REQUIRE(collect("Material.Switch", env.Save(fine), finePins));
    CHECK(finePins.Size() == 4);

    const StringView staticKeys[] = {"Default", "SwitchValue", "Result"};
    for (StringView name : staticKeys) {
        SwitchProperties clash(env.allocator);
        SwitchInput input;
        input.inputName = env.Intern(name);
        REQUIRE(clash.inputs.TryPushBack(input));
        nodegraph::PinList pins(env.allocator);
        CHECK(collect("Material.Switch", env.Save(clash), pins).Error() ==
              ErrorCode::InvalidArgument);
    }

    AggregateProperties aggregate(env.allocator);
    REQUIRE(aggregate.attributeNames.TryPushBack(env.Intern("Result")));
    nodegraph::PinList aggregatePins(env.allocator);
    CHECK(collect("Material.Aggregate", env.Save(aggregate), aggregatePins).Error() ==
          ErrorCode::InvalidArgument);
}

} // namespace material_dynamic_pins_tests_file
