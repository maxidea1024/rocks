#include <Core/Text/String.h>
#include <Core/Text/Json.h>
#include <Core/FileRead.h>
#include <Core/Error.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <MaterialSchema/MaterialAttributeTable.h>
#include <MaterialSchema/MaterialGraphSchema.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <MaterialSchema/MaterialSchemaModule.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <Meta/TypeSchema.h>
#include <NodeGraph/GraphCommand.h>
#include <NodeGraph/GraphIdSource.h>
#include <NodeGraph/GraphReadView.h>
#include <NodeGraph/GraphSemantic.h>
#include <NodeGraph/NodeTypeRegistry.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// MaterialSchema 기반 시험이다(Docs/Design/MaterialSystem 5·19장, 단계 MS1a): 핀 타입 표와 연결
// 규칙, 노드 정의 검사와 등록·해제·되돌림, 그래프 종류 거르기, 기본 출력 노드, 파라미터 기본값을 뺀
// 의미 정규화, 순환 연결 거부.
using namespace rocks;
using namespace rocks::material;
namespace material_schema_tests_file {

namespace {

using nodegraph::PinTypeId;

constexpr PinTypeId Pin(MaterialPinType type) noexcept {
    return ToPinTypeId(type);
}

// 시험 노드 정의다(정적 수명 — 등록 동안 살아 있어야 한다).
constexpr MaterialPinDefinition AddPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(1.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, "Result"),
};

constexpr MaterialPinDefinition SamplePins[] = {
    MaterialInput("Texture", MaterialPinType::TextureAny, "Texture"),
    MaterialOutput("Rgb", MaterialPinType::Float3, "RGB"),
};

constexpr MaterialPinDefinition FunctionOnlyPins[] = {
    MaterialOutput("Value", MaterialPinType::Numeric, "Value"),
};

// 파라미터 흉내 속성 스키마다: 필드 "name"(의미)과 "defaultValue"(의미 밖).
constexpr Guid ParamStructId{0x4d41545445535430ull, 1};
constexpr Guid ParamNameField{0x4d41545445535430ull, 2};
constexpr Guid ParamDefaultField{0x4d41545445535430ull, 3};
constexpr char ParamNameText[] = {'n', 'a', 'm', 'e'};
constexpr char ParamDefaultText[] = {'d', 'e', 'f', 'a', 'u', 'l', 't', 'V', 'a', 'l', 'u', 'e'};
constexpr char ParamStructText[] = {'P', 'a', 'r', 'a', 'm'};

FieldSchema MakeField(Guid id, Span<const char> name) noexcept {
    FieldSchema field;
    field.id = id;
    field.name = name;
    return field;
}

TypeSchema MakeParamSchema() noexcept;

const TypeSchema& ParamSchema() noexcept {
    static const TypeSchema schema = MakeParamSchema();
    return schema;
}

TypeSchema MakeParamSchema() noexcept {
    static const FieldSchema fields[] = {
        MakeField(ParamNameField, Span<const char>(ParamNameText)),
        MakeField(ParamDefaultField, Span<const char>(ParamDefaultText)),
    };
    TypeSchema schema;
    schema.id = ParamStructId;
    schema.version = 1;
    schema.fields = fields;
    schema.name = Span<const char>(ParamStructText);
    return schema;
}

constexpr StringView ParamNonSemantic[] = {"defaultValue"};
constexpr StringView ParamUnknownField[] = {"missing"};

constexpr MaterialPinDefinition ParamPins[] = {
    MaterialOutput("Value", MaterialPinType::Float1, "Value"),
};

constexpr MaterialNodeDefinition TestDefinitions[] = {
    MaterialNode("Material.TestAdd").Pins(AddPins).Menu("Add", "Math", "+", "Adds."),
    MaterialNode("Material.TestSample").Pins(SamplePins),
    MaterialNode("Material.TestFunctionOnly")
        .Pins(FunctionOnlyPins)
        .GraphKinds(ToMask(MaterialGraphKind::Function)),
    MaterialNode("Material.TestParam")
        .Properties(&ParamSchema, ParamNonSemantic)
        .Pins(ParamPins)
        .Flags(MaterialNodeFlags::Parameter),
};

constexpr MaterialPinDefinition OutputWithDefault() noexcept {
    MaterialPinDefinition pin = MaterialOutput("Out", MaterialPinType::Numeric, "Out");
    pin.defaultValue = MaterialPinDefault::Scalar(1.0f);
    return pin;
}

struct SchemaEnv {
    // schema가 생성 때 이름을 intern하므로 pool을 먼저 초기화한다(멤버 선언 순서).
    SchemaEnv() noexcept
        : names(allocator), namesReady(static_cast<bool>(names.Initialize())),
          registry(allocator, names), catalog(allocator),
          material(MaterialGraphKind::Material, catalog, names, allocator),
          function(MaterialGraphKind::Function, catalog, names, allocator) {
        ready = namesReady && registry.Initialize() && material.Ready() && function.Ready() &&
                registry.RegisterSchema(material, ModuleLease()) &&
                registry.RegisterSchema(function, ModuleLease());
    }

    Name Intern(StringView text) noexcept {
        auto name = names.TryIntern(text);
        return name ? name.Value() : Name();
    }

    SystemAllocator allocator;
    NamePool names;
    bool namesReady = false;
    nodegraph::NodeTypeRegistry registry;
    MaterialNodeCatalog catalog;
    MaterialGraphSchema material;
    MaterialGraphSchema function;
    bool ready = false;
};

Array<uint8> ParamRecord(Allocator& allocator, uint32 nameValue, uint32 defaultValue) noexcept {
    RecordBuilder builder(allocator, ParamStructId, 1);
    const uint8 name[] = {static_cast<uint8>(nameValue), 0, 0, 0};
    const uint8 value[] = {static_cast<uint8>(defaultValue), 0, 0, 0};
    (void)builder.AddField(ParamNameField, WireType::U32, name);
    (void)builder.AddField(ParamDefaultField, WireType::U32, value);
    auto built = builder.Build();
    return built ? Move(built.Value()) : Array<uint8>(allocator);
}

bool SameBytes(const Array<uint8>& left, const Array<uint8>& right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 노드 하나짜리 머티리얼 문서다.
nodegraph::GraphDocument OneNodeDocument(SchemaEnv& env, Name type,
                                         Array<uint8> properties) noexcept {
    nodegraph::GraphDocument document(env.allocator);
    document.domain = env.material.Domain();
    nodegraph::Graph main(env.allocator);
    main.id = Guid{1, 1};
    nodegraph::GraphNode node(env.allocator);
    node.id = Guid{2, 2};
    node.type = type;
    node.typeSchemaVersion = 1;
    node.properties.bytes = Move(properties);
    (void)main.nodes.TryPushBack(Move(node));
    document.mainGraph = main.id;
    (void)document.graphs.TryPushBack(Move(main));
    return document;
}

} // namespace

// 핀 타입 표: 이름·군이 값 순서와 맞고, 범위 밖은 Invalid·Pending이다.
TEST(MaterialSchema_PinTypeTable) {
    CHECK(MaterialPinTypes().Count() == MaterialPinTypeCount);
    CHECK(MaterialPinTypeName(MaterialPinType::Numeric) == StringView("Numeric"));
    CHECK(MaterialPinTypeName(MaterialPinType::ShadingModel) == StringView("ShadingModel"));
    CHECK(MaterialPinTypeName(static_cast<MaterialPinType>(MaterialPinTypeCount)) ==
          StringView("Invalid"));
    CHECK(ClassifyMaterialPinType(MaterialPinType::Double3) == MaterialPinTypeClass::Numeric);
    CHECK(ClassifyMaterialPinType(MaterialPinType::Texture3D) == MaterialPinTypeClass::Texture);
    CHECK(ClassifyMaterialPinType(MaterialPinType::TextureVirtual) ==
          MaterialPinTypeClass::Pending);
    for (uint32 index = 0; index < MaterialPinTypeCount; ++index) {
        CHECK(MaterialPinTypes().types[index].name ==
              MaterialPinTypeName(static_cast<MaterialPinType>(index)));
    }
}

// 연결 규칙(5.3절): 수 군끼리·static, 같은 텍스처, TextureAny ← 텍스처, wildcard는 모두.
TEST(MaterialSchema_PinTypeConnectRules) {
    CHECK(CanConnectMaterialPinTypes(Pin(MaterialPinType::Float4), Pin(MaterialPinType::Numeric)));
    CHECK(CanConnectMaterialPinTypes(Pin(MaterialPinType::Float1), Pin(MaterialPinType::Float3)));
    CHECK(CanConnectMaterialPinTypes(Pin(MaterialPinType::StaticBool), Pin(MaterialPinType::Bool)));
    CHECK(
        CanConnectMaterialPinTypes(Pin(MaterialPinType::Float1), Pin(MaterialPinType::StaticBool)));
    CHECK(CanConnectMaterialPinTypes(Pin(MaterialPinType::Texture2D),
                                     Pin(MaterialPinType::Texture2D)));
    CHECK(!CanConnectMaterialPinTypes(Pin(MaterialPinType::Texture2D),
                                      Pin(MaterialPinType::TextureCube)));
    CHECK(CanConnectMaterialPinTypes(Pin(MaterialPinType::TextureCube),
                                     Pin(MaterialPinType::TextureAny)));
    CHECK(!CanConnectMaterialPinTypes(Pin(MaterialPinType::Float4),
                                      Pin(MaterialPinType::TextureAny)));
    CHECK(!CanConnectMaterialPinTypes(Pin(MaterialPinType::Texture2D),
                                      Pin(MaterialPinType::Numeric)));
    CHECK(!CanConnectMaterialPinTypes(Pin(MaterialPinType::Float3), Pin(MaterialPinType::Surface)));
    CHECK(CanConnectMaterialPinTypes(Pin(MaterialPinType::Surface), Pin(MaterialPinType::Surface)));
    CHECK(!CanConnectMaterialPinTypes(Pin(MaterialPinType::MaterialAttributes),
                                      Pin(MaterialPinType::Surface)));
    CHECK(!CanConnectMaterialPinTypes(Pin(MaterialPinType::SparseVolumeTexture),
                                      Pin(MaterialPinType::TextureAny)));
    CHECK(
        CanConnectMaterialPinTypes(nodegraph::UnresolvedPinType, Pin(MaterialPinType::Texture2D)));
    CHECK(CanConnectMaterialPinTypes(Pin(MaterialPinType::Surface), nodegraph::UnresolvedPinType));
    CHECK(!CanConnectMaterialPinTypes(MaterialPinTypeCount, Pin(MaterialPinType::Numeric)));

    // 통과 핀(Any — UE MCT_Unknown)은 어느 쪽이든 무엇과도 잇는다(5.3절).
    const MaterialPinType every[] = {
        MaterialPinType::Float3,       MaterialPinType::Texture2D,
        MaterialPinType::TextureAny,   MaterialPinType::Surface,
        MaterialPinType::StaticBool,   MaterialPinType::MaterialAttributes,
        MaterialPinType::ShadingModel, MaterialPinType::TextureExternal,
        MaterialPinType::Any,
    };
    for (MaterialPinType type : every) {
        CHECK(CanConnectMaterialPinTypes(Pin(type), Pin(MaterialPinType::Any)));
        CHECK(CanConnectMaterialPinTypes(Pin(MaterialPinType::Any), Pin(type)));
    }

    CHECK(MaterialPinTypeName(MaterialPinType::Any) == StringView("Any"));
    CHECK(ClassifyMaterialPinType(MaterialPinType::Any) == MaterialPinTypeClass::Any);
}

// 엔진 노드 묶음: 두 도메인에 등록되고, 출력 노드는 머티리얼 도메인에만 있다.
TEST(MaterialSchema_BuiltinGroupsRegister) {
    SchemaEnv env;
    REQUIRE(env.ready);
    auto materialGroup = env.catalog.RegisterGroups(env.registry, env.material,
                                                    BuiltinMaterialNodeGroups(), ModuleLease());
    REQUIRE(materialGroup);
    auto functionGroup = env.catalog.RegisterGroups(env.registry, env.function,
                                                    BuiltinMaterialNodeGroups(), ModuleLease());
    REQUIRE(functionGroup);

    const Name output = env.Intern(MaterialOutputNodeTypeName);
    CHECK(env.catalog.Find(output) != nullptr);
    CHECK(env.registry.FindNodeType(env.material.Domain(), output) != nullptr);
    CHECK(env.registry.FindNodeType(env.function.Domain(), output) == nullptr);

    CHECK(env.catalog.Unregister(env.registry, functionGroup.Value()));
    CHECK(env.catalog.Unregister(env.registry, materialGroup.Value()));
    CHECK(env.catalog.Find(output) == nullptr);
    CHECK(env.catalog.Unregister(env.registry, materialGroup.Value()).Error() ==
          ErrorCode::NotFound);
}

// 정의 등록: 핀·종류가 NodeGraph 서술로 옮겨지고 graphKinds가 도메인을 거른다. 해제하면 사라진다.
TEST(MaterialSchema_CatalogRegistersDefinitions) {
    SchemaEnv env;
    REQUIRE(env.ready);
    auto group = env.catalog.Register(env.registry, env.material, TestDefinitions, ModuleLease());
    REQUIRE(group);

    const Name add = env.Intern("Material.TestAdd");
    const nodegraph::RegisteredNodeType* registered =
        env.registry.FindNodeType(env.material.Domain(), add);
    REQUIRE(registered != nullptr);
    REQUIRE(registered->descriptor.staticPins.Size() == 3);
    CHECK(registered->descriptor.staticPins[0].key == env.Intern("A"));
    CHECK(registered->descriptor.staticPins[2].direction == nodegraph::PinDirection::Output);
    CHECK(registered->descriptor.menu.displayName == StringView("Add"));
    CHECK(env.catalog.Find(add) == &TestDefinitions[0]);

    // 함수 전용 정의는 머티리얼 도메인에 등록되지 않는다.
    CHECK(env.registry.FindNodeType(env.material.Domain(),
                                    env.Intern("Material.TestFunctionOnly")) == nullptr);
    CHECK(env.catalog.RegistrationCount() == 3);

    // 속성 스키마 ID가 NodeGraph 속성 struct가 된다.
    const nodegraph::RegisteredNodeType* param =
        env.registry.FindNodeType(env.material.Domain(), env.Intern("Material.TestParam"));
    REQUIRE(param != nullptr);
    CHECK(param->descriptor.propertiesStruct == ParamStructId);

    CHECK(env.catalog.Unregister(env.registry, group.Value()));
    CHECK(env.registry.FindNodeType(env.material.Domain(), add) == nullptr);
    CHECK(env.catalog.RegistrationCount() == 0);
}

// 정의 검사 음성: 규칙마다 InvalidArgument이고 등록표는 그대로다.
TEST(MaterialSchema_CatalogRejectsInvalidDefinitions) {
    SchemaEnv env;
    REQUIRE(env.ready);

    static constexpr MaterialPinDefinition duplicatePins[] = {
        MaterialInput("A", MaterialPinType::Numeric, "A"),
        MaterialInput("A", MaterialPinType::Numeric, "A again"),
    };
    static constexpr MaterialPinDefinition outputWithDefault[] = {OutputWithDefault()};
    static constexpr MaterialPinDefinition textureAnyOutput[] = {
        MaterialOutput("Out", MaterialPinType::TextureAny, "Out"),
    };
    static constexpr MaterialPinDefinition emptyKey[] = {
        MaterialInput("", MaterialPinType::Numeric, "Empty"),
    };
    static constexpr MaterialPinDefinition badType[] = {
        MaterialInput("A", static_cast<MaterialPinType>(MaterialPinTypeCount), "A"),
    };

    static constexpr MaterialNodeDefinition invalid[] = {
        MaterialNode(""),
        MaterialNode("Material.Dup").Pins(duplicatePins),
        MaterialNode("Material.OutDefault").Pins(outputWithDefault),
        MaterialNode("Material.TexOut").Pins(textureAnyOutput),
        MaterialNode("Material.EmptyKey").Pins(emptyKey),
        MaterialNode("Material.BadType").Pins(badType),
        MaterialNode("Material.Stage").Stages(0),
        MaterialNode("Material.StageBits").Stages(0x4),
        MaterialNode("Material.KindBits").GraphKinds(0x4),
        MaterialNode("Material.Missing")
            .RawStatus(MaterialNodeStatus::PrerequisiteMissing, MaterialPrerequisite::None),
        MaterialNode("Material.Extra")
            .RawStatus(MaterialNodeStatus::Implemented, MaterialPrerequisite::Font),
        MaterialNode("Material.NoSchema").Properties(nullptr, ParamNonSemantic),
        MaterialNode("Material.UnknownField").Properties(&ParamSchema, ParamUnknownField),
        MaterialNode("Material.Flags").Flags(static_cast<MaterialNodeFlags>(0x80)),
    };

    for (const MaterialNodeDefinition& definition : invalid) {
        auto group =
            env.catalog.Register(env.registry, env.material,
                                 Span<const MaterialNodeDefinition>(&definition, 1), ModuleLease());
        CHECK(!group && group.Error() == ErrorCode::InvalidArgument);
    }

    // 같은 묶음 안 종류 이름 중복.
    static constexpr MaterialNodeDefinition twice[] = {
        MaterialNode("Material.Twice"),
        MaterialNode("Material.Twice"),
    };
    CHECK(env.catalog.Register(env.registry, env.material, twice, ModuleLease()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(env.catalog.RegistrationCount() == 0);

    // 상태 TODO와 선행 ID가 짝을 이루면 받는다.
    static constexpr MaterialNodeDefinition missing[] = {
        MaterialNode("Material.Todo").Missing(MaterialPrerequisite::Font),
    };
    CHECK(env.catalog.Register(env.registry, env.material, missing, ModuleLease()));
}

// 등록 중간 실패(이미 있는 종류)는 묶음 전체를 되돌린다.
TEST(MaterialSchema_CatalogRollsBackOnFailure) {
    SchemaEnv env;
    REQUIRE(env.ready);
    static constexpr MaterialNodeDefinition first[] = {MaterialNode("Material.Taken")};
    REQUIRE(env.catalog.Register(env.registry, env.material, first, ModuleLease()));

    static constexpr MaterialNodeDefinition second[] = {
        MaterialNode("Material.Fresh"),
        MaterialNode("Material.Taken"),
    };
    auto group = env.catalog.Register(env.registry, env.material, second, ModuleLease());
    CHECK(!group && group.Error() == ErrorCode::AlreadyExists);
    CHECK(env.registry.FindNodeType(env.material.Domain(), env.Intern("Material.Fresh")) ==
          nullptr);
    CHECK(env.catalog.Find(env.Intern("Material.Fresh")) == nullptr);
    CHECK(env.catalog.RegistrationCount() == 1);
}

// 등록되지 않은 schema·준비되지 않은 schema로는 등록할 수 없다.
TEST(MaterialSchema_CatalogRequiresRegisteredSchema) {
    SchemaEnv env;
    REQUIRE(env.ready);
    MaterialNodeCatalog other(env.allocator);
    MaterialGraphSchema unregistered(MaterialGraphKind::Material, other, env.names, env.allocator);
    REQUIRE(unregistered.Ready());
    CHECK(
        env.catalog.Register(env.registry, unregistered, TestDefinitions, ModuleLease()).Error() ==
        ErrorCode::InvalidArgument);
}

// 기본 노드: 머티리얼 주 그래프에만 출력 노드 하나, 함수·하위 그래프에는 없다.
TEST(MaterialSchema_DefaultOutputNode) {
    SchemaEnv env;
    REQUIRE(env.ready);
    REQUIRE(env.catalog.RegisterGroups(env.registry, env.material, BuiltinMaterialNodeGroups(),
                                       ModuleLease()));
    nodegraph::SeededGraphIdSource ids(7, Guid{3, 3});

    nodegraph::GraphCommandList commands(env.allocator);
    REQUIRE(env.material.CreateDefaultNodes(nodegraph::GraphRole::Main, Guid{1, 1}, ids, commands));
    REQUIRE(commands.Size() == 1);
    const nodegraph::GraphCommand& added = commands.View()[0];
    CHECK(added.kind == nodegraph::GraphCommandKind::AddNode);
    CHECK(added.nodeData.type == env.Intern(MaterialOutputNodeTypeName));
    CHECK(added.nodeData.typeSchemaVersion == 1);
    CHECK(added.layout.node == added.nodeData.id);

    nodegraph::GraphCommandList collapsed(env.allocator);
    REQUIRE(env.material.CreateDefaultNodes(nodegraph::GraphRole::Collapsed, Guid{1, 1}, ids,
                                            collapsed));
    CHECK(collapsed.Size() == 0);

    nodegraph::GraphCommandList function(env.allocator);
    REQUIRE(env.function.CreateDefaultNodes(nodegraph::GraphRole::Main, Guid{1, 1}, ids, function));
    CHECK(function.Size() == 0);
}

// 출력 노드 정의가 없으면 기본 노드를 만들 수 없다(NotFound).
TEST(MaterialSchema_DefaultOutputNeedsDefinition) {
    SchemaEnv env;
    REQUIRE(env.ready);
    nodegraph::SeededGraphIdSource ids(7, Guid{3, 3});
    nodegraph::GraphCommandList commands(env.allocator);
    CHECK(env.material.CreateDefaultNodes(nodegraph::GraphRole::Main, Guid{1, 1}, ids, commands)
              .Error() == ErrorCode::NotFound);
}

// 의미 정규화(5.5절): 파라미터 기본값만 다른 두 문서는 의미 바이트가 같고, 의미 필드가 다르면
// 다르다.
TEST(MaterialSchema_SemanticIgnoresParameterDefaults) {
    SchemaEnv env;
    REQUIRE(env.ready);
    REQUIRE(env.catalog.Register(env.registry, env.material, TestDefinitions, ModuleLease()));
    const Name param = env.Intern("Material.TestParam");

    auto base = OneNodeDocument(env, param, ParamRecord(env.allocator, 5, 1));
    auto otherDefault = OneNodeDocument(env, param, ParamRecord(env.allocator, 5, 9));
    auto otherName = OneNodeDocument(env, param, ParamRecord(env.allocator, 6, 1));

    Array<uint8> baseBytes(env.allocator);
    Array<uint8> defaultBytes(env.allocator);
    Array<uint8> nameBytes(env.allocator);
    REQUIRE(nodegraph::WriteSemanticBytes(base, env.registry, env.allocator, baseBytes));
    REQUIRE(nodegraph::WriteSemanticBytes(otherDefault, env.registry, env.allocator, defaultBytes));
    REQUIRE(nodegraph::WriteSemanticBytes(otherName, env.registry, env.allocator, nameBytes));
    CHECK(SameBytes(baseBytes, defaultBytes));
    CHECK(!SameBytes(baseBytes, nameBytes));

    // 걸러 낼 필드가 없는 노드(모르는 종류)는 속성 바이트 전체가 의미다.
    const Name unknown = env.Intern("Material.NotRegistered");
    auto unknownA = OneNodeDocument(env, unknown, ParamRecord(env.allocator, 5, 1));
    auto unknownB = OneNodeDocument(env, unknown, ParamRecord(env.allocator, 5, 9));
    Array<uint8> unknownBytesA(env.allocator);
    Array<uint8> unknownBytesB(env.allocator);
    REQUIRE(nodegraph::WriteSemanticBytes(unknownA, env.registry, env.allocator, unknownBytesA));
    REQUIRE(nodegraph::WriteSemanticBytes(unknownB, env.registry, env.allocator, unknownBytesB));
    CHECK(!SameBytes(unknownBytesA, unknownBytesB));
}

// 연결 판정: 타입 거부, 순환 거부, 입력이 차면 교체.
TEST(MaterialSchema_ConnectVerdicts) {
    SchemaEnv env;
    REQUIRE(env.ready);
    REQUIRE(env.catalog.Register(env.registry, env.material, TestDefinitions, ModuleLease()));
    const Name add = env.Intern("Material.TestAdd");
    const Name sample = env.Intern("Material.TestSample");

    nodegraph::GraphDocument document(env.allocator);
    document.domain = env.material.Domain();
    nodegraph::Graph main(env.allocator);
    main.id = Guid{1, 1};
    for (uint64 index = 1; index <= 2; ++index) {
        nodegraph::GraphNode node(env.allocator);
        node.id = Guid{index, 0};
        node.type = index == 1 ? add : sample;
        node.typeSchemaVersion = 1;
        REQUIRE(main.nodes.TryPushBack(Move(node)));
    }

    // 노드 1의 Result → 노드 1의 A(자기 자신 순환).
    nodegraph::GraphDocument* view = &document;
    document.mainGraph = main.id;
    REQUIRE(document.graphs.TryPushBack(Move(main)));
    nodegraph::GraphReadView reader(*view, env.registry);
    const nodegraph::Graph& graph = document.graphs[0];

    const nodegraph::RegisteredNodeType* addType = env.registry.FindNodeType(document.domain, add);
    const nodegraph::RegisteredNodeType* sampleType =
        env.registry.FindNodeType(document.domain, sample);
    REQUIRE(addType != nullptr && sampleType != nullptr);
    const nodegraph::PinDescriptor& addA = addType->descriptor.staticPins[0];
    const nodegraph::PinDescriptor& addResult = addType->descriptor.staticPins[2];
    const nodegraph::PinDescriptor& sampleTexture = sampleType->descriptor.staticPins[0];
    const nodegraph::PinDescriptor& sampleRgb = sampleType->descriptor.staticPins[1];

    nodegraph::ConnectQuery self;
    self.graph = &graph;
    self.from = {Guid{1, 0}, addResult.key};
    self.to = {Guid{1, 0}, addA.key};
    self.fromPin = &addResult;
    self.toPin = &addA;
    auto cycle = env.material.CanConnect(reader, self);
    CHECK(cycle.action == nodegraph::ConnectAction::Deny);
    CHECK(cycle.reasonCode == static_cast<uint32>(MaterialConnectReason::Cycle));

    // 노드 1의 Result(Numeric) → 노드 2의 Texture(TextureAny): 타입 거부.
    nodegraph::ConnectQuery texture;
    texture.graph = &graph;
    texture.from = {Guid{1, 0}, addResult.key};
    texture.to = {Guid{2, 0}, sampleTexture.key};
    texture.fromPin = &addResult;
    texture.toPin = &sampleTexture;
    auto mismatch = env.material.CanConnect(reader, texture);
    CHECK(mismatch.action == nodegraph::ConnectAction::Deny);
    CHECK(mismatch.reasonCode == static_cast<uint32>(MaterialConnectReason::TypeMismatch));

    // 노드 2의 RGB(Float3) → 노드 1의 A(Numeric): 잇는다. 입력이 차 있으면 교체한다.
    nodegraph::ConnectQuery numeric;
    numeric.graph = &graph;
    numeric.from = {Guid{2, 0}, sampleRgb.key};
    numeric.to = {Guid{1, 0}, addA.key};
    numeric.fromPin = &sampleRgb;
    numeric.toPin = &addA;
    CHECK(env.material.CanConnect(reader, numeric).action == nodegraph::ConnectAction::Make);
    numeric.toEdgeCount = 1;
    CHECK(env.material.CanConnect(reader, numeric).action ==
          nodegraph::ConnectAction::MakeReplacingInput);
}

// 이름 표: 문맥 기능·선행 ID.
TEST(MaterialSchema_VocabularyNames) {
    CHECK(MaterialContextFeatureDefineName(MaterialContextFeature::DistanceField) ==
          StringView("DISTANCE_FIELD"));
    CHECK(MaterialContextFeatureKey(MaterialContextFeature::UserInterface) == StringView("ui"));
    CHECK(
        MaterialContextFeatureKey(static_cast<MaterialContextFeature>(MaterialContextFeatureCount))
            .IsEmpty());
    CHECK(MaterialPrerequisiteId(MaterialPrerequisite::Font) == StringView("MPQ-Font"));
    CHECK(MaterialPrerequisiteId(MaterialPrerequisite::None).IsEmpty());
    CHECK(MaterialPrerequisiteId(static_cast<MaterialPrerequisite>(MaterialPrerequisiteCount))
              .IsEmpty());
    CHECK(HasFeature(ToMask(MaterialContextFeature::Scene), MaterialContextFeature::Scene));
    CHECK(!HasFeature(ToMask(MaterialContextFeature::Scene), MaterialContextFeature::View));
}

// 선행 ID 전수: enum 값 순서가 19.4.1절 표 순서와 같고, 모든 ID가 "MPQ-" 접두이며 서로 다르다.
// 값을 빼거나 순서를 밀면 여기서 걸린다.
TEST(MaterialSchema_PrerequisiteIdsMatchTable) {
    // 19.4.1절 표의 행 순서다.
    const StringView table[] = {
        "MPQ-TextureCollection",
        "MPQ-SparseVolumeTexture",
        "MPQ-VirtualTexture",
        "MPQ-ExternalTexture",
        "MPQ-Font",
        "MPQ-CurveAsset",
        "MPQ-MaterialAggregate",
        "MPQ-SubsurfaceProfile",
        "MPQ-SpecularProfile",
        "MPQ-ToonShading",
        "MPQ-NeuralProfile",
        "MPQ-PhysicalMaterial",
        "MPQ-VirtualizedGeometry",
        "MPQ-ColoredTransmittance",
        "MPQ-AlphaHoldout",
        "MPQ-LightBake",
    };
    static_assert(sizeof(table) / sizeof(table[0]) + 1 == MaterialPrerequisiteCount,
                  "prerequisite table must list every value except None");

    for (uint32 value = 1; value < MaterialPrerequisiteCount; ++value) {
        const StringView id = MaterialPrerequisiteId(static_cast<MaterialPrerequisite>(value));
        CHECK(id == table[value - 1]);
        CHECK(id.StartsWith("MPQ-", StringComparison::CaseSensitive));
        for (uint32 other = 1; other < value; ++other) {
            CHECK(id != MaterialPrerequisiteId(static_cast<MaterialPrerequisite>(other)));
        }
    }
}

namespace {

// 계수 lease다(등록이 쥐고 해제가 놓는지 본다).
struct CountedLease {
    int count = 0;

    static void Retain(void* context) noexcept {
        ++static_cast<CountedLease*>(context)->count;
    }

    static void Release(void* context) noexcept {
        --static_cast<CountedLease*>(context)->count;
    }

    ModuleLease Acquire() noexcept {
        auto lease = ModuleLease::Acquire(this, &Retain, &Release);
        return lease ? Move(lease.Value()) : ModuleLease();
    }
};

constexpr MaterialPinDefinition TogglePins[] = {
    MaterialInput("In", MaterialPinType::Numeric, "In"),
    MaterialOutput("Out", MaterialPinType::Numeric, "Out"),
};

// 키를 대소문자만 다르게 적어도 정적 핀과 맞는다(NamePool 규칙).
constexpr MaterialPassThrough ToggleThrough[] = {{"in", "Out"}};
constexpr MaterialPassThrough ToggleBadThrough[] = {{"Out", "In"}};

constexpr MaterialNodeDefinition PluginDefinitions[] = {
    MaterialNode("Plugin.Toggle")
        .Pins(TogglePins)
        .PassThrough(ToggleThrough)
        .Capabilities(nodegraph::DefaultNodeCapabilities |
                      nodegraph::NodeCapabilities::AllowDisable),
};

nodegraph::GraphNode MakeNode(SchemaEnv& env, Guid id, Name type) noexcept {
    nodegraph::GraphNode node(env.allocator);
    node.id = id;
    node.type = type;
    node.typeSchemaVersion = 1;
    return node;
}

nodegraph::GraphEdge MakeEdge(Guid from, Name fromPin, Guid to, Name toPin) noexcept {
    nodegraph::GraphEdge edge;
    edge.fromNode = from;
    edge.fromPin = fromPin;
    edge.toNode = to;
    edge.toPin = toPin;
    return edge;
}

Array<uint8> ForeignRecord(Allocator& allocator, uint8 value) noexcept {
    constexpr Guid foreign{0x464f524549474e00ull, 9};
    RecordBuilder builder(allocator, foreign, 1);
    const uint8 payload[] = {value, 0, 0, 0};
    (void)builder.AddField(ParamDefaultField, WireType::U32, payload);
    auto built = builder.Build();
    return built ? Move(built.Value()) : Array<uint8>(allocator);
}

} // namespace

// 가짜 플러그인 묶음: 자기 lease로 등록하고 해제하면 lease를 놓는다. 통과 쌍 키는 intern된다.
TEST(MaterialSchema_PluginGroupLeaseAndPassThrough) {
    SchemaEnv env;
    REQUIRE(env.ready);
    CountedLease plugin;
    auto group =
        env.catalog.Register(env.registry, env.material, PluginDefinitions, plugin.Acquire());
    REQUIRE(group);
    CHECK(plugin.count > 0);
    const nodegraph::RegisteredNodeType* toggle =
        env.registry.FindNodeType(env.material.Domain(), env.Intern("Plugin.Toggle"));
    REQUIRE(toggle != nullptr);
    REQUIRE(toggle->descriptor.passThrough.Size() == 1);
    CHECK(toggle->descriptor.passThrough[0].input == env.Intern("In"));
    CHECK(toggle->descriptor.passThrough[0].output == env.Intern("Out"));
    CHECK(env.catalog.Unregister(env.registry, group.Value()));
    CHECK(plugin.count == 0);

    // 통과 쌍이 정적 입력 → 출력이 아니면 거부한다.
    static constexpr MaterialNodeDefinition bad[] = {
        MaterialNode("Plugin.BadToggle").Pins(TogglePins).PassThrough(ToggleBadThrough),
    };
    CHECK(env.catalog.Register(env.registry, env.material, bad, ModuleLease()).Error() ==
          ErrorCode::InvalidArgument);
}

// 같은 종류 이름을 다른 정의로 다른 도메인에 등록하면 AlreadyExists다(Find는 도메인 없이 찾는다).
// 한 도메인 묶음만 해제해도 다른 도메인에 등록한 같은 정의는 찾을 수 있다.
TEST(MaterialSchema_CatalogTypeNameIsDomainIndependent) {
    SchemaEnv env;
    REQUIRE(env.ready);
    static constexpr MaterialNodeDefinition first[] = {MaterialNode("Material.Shared")};
    static constexpr MaterialNodeDefinition second[] = {MaterialNode("Material.Shared")};
    auto inMaterial = env.catalog.Register(env.registry, env.material, first, ModuleLease());
    REQUIRE(inMaterial);
    CHECK(env.catalog.Register(env.registry, env.function, second, ModuleLease()).Error() ==
          ErrorCode::AlreadyExists);

    auto inFunction = env.catalog.Register(env.registry, env.function, first, ModuleLease());
    REQUIRE(inFunction);
    CHECK(env.catalog.Unregister(env.registry, inMaterial.Value()));
    CHECK(env.catalog.Find(env.Intern("Material.Shared")) == &first[0]);
    CHECK(env.catalog.Unregister(env.registry, inFunction.Value()));
    CHECK(env.catalog.Find(env.Intern("Material.Shared")) == nullptr);
}

// 종류 이름 중복은 NamePool 규칙(ASCII 대소문자 무시)으로 본다.
TEST(MaterialSchema_CatalogRejectsCaseVariants) {
    SchemaEnv env;
    REQUIRE(env.ready);
    static constexpr MaterialNodeDefinition variants[] = {
        MaterialNode("Material.Case"),
        MaterialNode("material.case"),
    };
    CHECK(env.catalog.Register(env.registry, env.material, variants, ModuleLease()).Error() ==
          ErrorCode::InvalidArgument);
}

// registry에서 이미 빠진 종류가 있어도 묶음 해제는 끝나고 lease를 놓는다.
TEST(MaterialSchema_CatalogUnregisterToleratesRemovedTypes) {
    SchemaEnv env;
    REQUIRE(env.ready);
    CountedLease lease;
    static constexpr MaterialNodeDefinition single[] = {MaterialNode("Material.Removed")};
    auto group = env.catalog.Register(env.registry, env.material, single, lease.Acquire());
    REQUIRE(group);
    // 다른 소유자가 등록표에서 먼저 지운 경우를 흉내 낸다: 등록 id를 모르므로 해제를 두 번 부른다.
    CHECK(env.catalog.Unregister(env.registry, group.Value()));
    CHECK(lease.count == 0);
    CHECK(env.catalog.Unregister(env.registry, group.Value()).Error() == ErrorCode::NotFound);
}

// 이름 pool이 준비되지 않았으면 schema는 준비되지 않는다.
TEST(MaterialSchema_SchemaNotReadyWithoutNames) {
    SystemAllocator allocator;
    NamePool names(allocator);
    MaterialNodeCatalog catalog(allocator);
    MaterialGraphSchema schema(MaterialGraphKind::Material, catalog, names, allocator);
    CHECK(!schema.Ready());
}

// 순환: 노드 여럿을 거치는 순환과 재경유를 낀 순환을 거부한다(V2).
TEST(MaterialSchema_ConnectRejectsIndirectCycles) {
    SchemaEnv env;
    REQUIRE(env.ready);
    REQUIRE(env.catalog.Register(env.registry, env.material, TestDefinitions, ModuleLease()));
    const Name add = env.Intern("Material.TestAdd");
    const Name a = env.Intern("A");
    const Name result = env.Intern("Result");
    const Name rerouteIn = env.Intern(nodegraph::ReroutePinIn);
    const Name rerouteOut = env.Intern(nodegraph::ReroutePinOut);

    nodegraph::GraphDocument document(env.allocator);
    document.domain = env.material.Domain();
    nodegraph::Graph main(env.allocator);
    main.id = Guid{1, 1};
    // 1 → 2 → 재경유 3 → 4. 4의 Result를 1의 B에 이으면 순환이다.
    REQUIRE(main.nodes.TryPushBack(MakeNode(env, Guid{1, 0}, add)));
    REQUIRE(main.nodes.TryPushBack(MakeNode(env, Guid{2, 0}, add)));
    REQUIRE(main.nodes.TryPushBack(MakeNode(env, Guid{3, 0}, env.registry.RerouteType())));
    REQUIRE(main.nodes.TryPushBack(MakeNode(env, Guid{4, 0}, add)));
    REQUIRE(main.edges.TryPushBack(MakeEdge(Guid{1, 0}, result, Guid{2, 0}, a)));
    REQUIRE(main.edges.TryPushBack(MakeEdge(Guid{2, 0}, result, Guid{3, 0}, rerouteIn)));
    REQUIRE(main.edges.TryPushBack(MakeEdge(Guid{3, 0}, rerouteOut, Guid{4, 0}, a)));
    document.mainGraph = main.id;
    REQUIRE(document.graphs.TryPushBack(Move(main)));
    nodegraph::GraphReadView reader(document, env.registry);

    const nodegraph::RegisteredNodeType* addType = env.registry.FindNodeType(document.domain, add);
    REQUIRE(addType != nullptr);
    const nodegraph::PinDescriptor& addB = addType->descriptor.staticPins[1];
    const nodegraph::PinDescriptor& addResult = addType->descriptor.staticPins[2];

    nodegraph::ConnectQuery query;
    query.graph = &document.graphs[0];
    query.from = {Guid{4, 0}, result};
    query.to = {Guid{1, 0}, addB.key};
    query.fromPin = &addResult;
    query.toPin = &addB;
    auto verdict = env.material.CanConnect(reader, query);
    CHECK(verdict.action == nodegraph::ConnectAction::Deny);
    CHECK(verdict.reasonCode == static_cast<uint32>(MaterialConnectReason::Cycle));

    // 반대 방향(1의 Result → 4의 B)은 순환이 아니다.
    query.from = {Guid{1, 0}, result};
    query.to = {Guid{4, 0}, addB.key};
    CHECK(env.material.CanConnect(reader, query).action == nodegraph::ConnectAction::Make);

    // 빈 질의는 InvalidQuery다.
    nodegraph::ConnectQuery empty;
    CHECK(env.material.CanConnect(reader, empty).reasonCode ==
          static_cast<uint32>(MaterialConnectReason::InvalidQuery));
}

// 속성 schema가 정의와 다른 바이트는 거르지 않고 그대로 의미에 든다(문서 검증이 따로 보고).
TEST(MaterialSchema_SemanticKeepsForeignSchemaBytes) {
    SchemaEnv env;
    REQUIRE(env.ready);
    REQUIRE(env.catalog.Register(env.registry, env.material, TestDefinitions, ModuleLease()));
    const Name param = env.Intern("Material.TestParam");
    auto left = OneNodeDocument(env, param, ForeignRecord(env.allocator, 1));
    auto right = OneNodeDocument(env, param, ForeignRecord(env.allocator, 2));
    Array<uint8> leftBytes(env.allocator);
    Array<uint8> rightBytes(env.allocator);
    REQUIRE(nodegraph::WriteSemanticBytes(left, env.registry, env.allocator, leftBytes));
    REQUIRE(nodegraph::WriteSemanticBytes(right, env.registry, env.allocator, rightBytes));
    CHECK(!SameBytes(leftBytes, rightBytes));
}

// 속성 표(19.12절): 내장 속성 등록·조회, UE Guid·기본값·빈도, 중복·잘못된 정의, lease 해제.
TEST(MaterialSchema_AttributeTable) {
    SystemAllocator allocator;
    MaterialAttributeTable table(allocator);
    CountedLease builtinLease;
    auto builtins = table.Register(BuiltinMaterialAttributes(), builtinLease.Acquire());
    REQUIRE(builtins);
    CHECK(table.Size() == BuiltinMaterialAttributes().Size());

    // UE Roughness: FGuid(0xD1DD967C, 0x4CAD47D3, 0x9E6346FB, 0x08ECF210), 기본값 0.5.
    const MaterialAttributeDefinition* roughness =
        table.Find(Guid{0xD1DD967C4CAD47D3ull, 0x9E6346FB08ECF210ull});
    REQUIRE(roughness != nullptr);
    CHECK(roughness->name == StringView("Roughness"));
    CHECK(roughness->type == MaterialPinType::Float1);
    CHECK(roughness->defaultValue.values[0] == 0.5f);

    // WPO는 정점 단계, Normal 기본값은 (0, 0, 1)이다.
    const MaterialAttributeDefinition* wpo =
        table.Find(Guid{0xF905F895D5814314ull, 0x916D24348C40CE9Eull});
    REQUIRE(wpo != nullptr);
    CHECK(wpo->frequency == MaterialVertexStage);
    const MaterialAttributeDefinition* normal =
        table.Find(Guid{0x0FA2821A200F4A4Aull, 0xB719B789C1259C64ull});
    REQUIRE(normal != nullptr);
    CHECK(normal->defaultValue.values[2] == 1.0f);

    // 이미 있는 id는 AlreadyExists, 잘못된 정의는 InvalidArgument다.
    CHECK(table.Register(Span<const MaterialAttributeDefinition>(roughness, 1), ModuleLease())
              .Error() == ErrorCode::AlreadyExists);
    static constexpr MaterialAttributeDefinition invalid[] = {
        {Guid{}, "Zero", MaterialPinType::Float1, {}, MaterialPixelStage, false, {}, 0},
        {Guid{7, 7}, "", MaterialPinType::Float1, {}, MaterialPixelStage, false, {}, 0},
        {Guid{7, 8}, "Both", MaterialPinType::Float1, {}, AllMaterialStages, false, {}, 0},
    };
    for (const MaterialAttributeDefinition& definition : invalid) {
        CHECK(table.Register(Span<const MaterialAttributeDefinition>(&definition, 1), ModuleLease())
                  .Error() == ErrorCode::InvalidArgument);
    }

    // custom 속성을 더하고 해제한다.
    CountedLease customLease;
    static constexpr MaterialAttributeDefinition custom[] = {
        {Guid{0x435553544f4d0001ull, 1},
         "Wetness",
         MaterialPinType::Float1,
         MaterialPinDefault::Scalar(0.0f),
         MaterialPixelStage,
         false,
         {},
         0},
    };
    auto customGroup = table.Register(custom, customLease.Acquire());
    REQUIRE(customGroup);
    CHECK(customLease.count > 0);
    CHECK(table.Unregister(customGroup.Value()));
    CHECK(customLease.count == 0);
    CHECK(table.Find(custom[0].id) == nullptr);
    CHECK(table.Unregister(customGroup.Value()).Error() == ErrorCode::NotFound);
    CHECK(table.Unregister(builtins.Value()));
    CHECK(builtinLease.count == 0);
}

namespace {

// 속성 읽기 시험 schema다: 정수·bool·float·문자열 필드 넷.
constexpr Guid ReaderStructId{0x4d41545245414445ull, 1};
constexpr Guid ReaderCountField{0x4d41545245414445ull, 2};
constexpr Guid ReaderFlagField{0x4d41545245414445ull, 3};
constexpr Guid ReaderScaleField{0x4d41545245414445ull, 4};
constexpr Guid ReaderLabelField{0x4d41545245414445ull, 5};
constexpr char ReaderCountText[] = {'c', 'o', 'u', 'n', 't'};
constexpr char ReaderFlagText[] = {'f', 'l', 'a', 'g'};
constexpr char ReaderScaleText[] = {'s', 'c', 'a', 'l', 'e'};
constexpr char ReaderLabelText[] = {'l', 'a', 'b', 'e', 'l'};
constexpr char ReaderStructText[] = {'R', 'e', 'a', 'd', 'e', 'r'};

FieldSchema MakeTypedField(Guid id, Span<const char> name, ValueKind kind) noexcept {
    FieldSchema field = MakeField(id, name);
    field.value.kind = kind;
    return field;
}

TypeSchema MakeReaderSchema() noexcept {
    static const FieldSchema fields[] = {
        MakeTypedField(ReaderCountField, Span<const char>(ReaderCountText), ValueKind::Int32),
        MakeTypedField(ReaderFlagField, Span<const char>(ReaderFlagText), ValueKind::Bool),
        MakeTypedField(ReaderScaleField, Span<const char>(ReaderScaleText), ValueKind::Float32),
        MakeTypedField(ReaderLabelField, Span<const char>(ReaderLabelText), ValueKind::Utf8String),
    };
    TypeSchema schema;
    schema.id = ReaderStructId;
    schema.version = 2;
    schema.fields = fields;
    schema.name = Span<const char>(ReaderStructText);
    return schema;
}

const TypeSchema& ReaderSchema() noexcept {
    static const TypeSchema schema = MakeReaderSchema();
    return schema;
}

// count(I64)·flag·scale·label을 쓴 Record다. withLabel이 거짓이면 label을 쓰지 않는다.
Array<uint8> ReaderRecord(Allocator& allocator, Guid schema, bool withLabel) noexcept {
    ArchiveWriter writer(allocator);
    auto record = RecordWriter::Begin(writer, schema, 1);
    if (!record) {
        return Array<uint8>(allocator);
    }

    RecordWriter& out = record.Value();
    const bool counted =
        out.BeginField(ReaderCountField) && writer.WriteI64(-7) && out.EndField(WireType::I64);
    const bool flagged =
        out.BeginField(ReaderFlagField) && writer.WriteBool(true) && out.EndField(WireType::Bool);
    const bool scaled =
        out.BeginField(ReaderScaleField) && writer.WriteF32(2.5f) && out.EndField(WireType::F32);
    const bool labeled =
        !withLabel || (out.BeginField(ReaderLabelField) && writer.WriteString("Moss") &&
                       out.EndField(WireType::String));
    if (!counted || !flagged || !scaled || !labeled || !out.End()) {
        return Array<uint8>(allocator);
    }

    return writer.TakeBytes();
}

} // namespace

// 속성 읽기(19.13.3절 — 동적 핀 함수): 기록된 필드는 wire대로 읽고, 기록되지 않은 필드와 빈 속성은
// 기본값이며, schema에 없는 이름·다른 schema·wire 불일치는 오류다.
TEST(MaterialSchema_NodePropertyReaderReadsFields) {
    SystemAllocator allocator;
    const Array<uint8> full = ReaderRecord(allocator, ReaderStructId, true);
    REQUIRE(!full.IsEmpty());
    auto reader = NodePropertyReader::Open(allocator, full.View(), ReaderSchema());
    REQUIRE(reader);
    NodePropertyReader& value = reader.Value();
    auto count = value.ReadInteger("count", 0);
    REQUIRE(count);
    CHECK(count.Value() == -7);
    auto flag = value.ReadBool("flag", false);
    REQUIRE(flag);
    CHECK(flag.Value());
    auto scale = value.ReadFloat("scale", 0.0f);
    REQUIRE(scale);
    CHECK(scale.Value() == 2.5f);
    auto label = value.ReadText("label");
    REQUIRE(label);
    CHECK(label.Value() == StringView("Moss"));
    CHECK(value.ReadInteger("missing", 0).Error() == ErrorCode::NotFound);
    // wire 불일치: 정수 필드를 bool로, bool 필드를 정수로, float 필드를 문자열로 읽는다.
    CHECK(value.ReadBool("count", false).Error() == ErrorCode::TypeMismatch);
    CHECK(value.ReadInteger("flag", 0).Error() == ErrorCode::TypeMismatch);
    CHECK(value.ReadText("scale").Error() == ErrorCode::TypeMismatch);

    // 기록되지 않은 필드는 기본값이다.
    const Array<uint8> partial = ReaderRecord(allocator, ReaderStructId, false);
    REQUIRE(!partial.IsEmpty());
    auto partialReader = NodePropertyReader::Open(allocator, partial.View(), ReaderSchema());
    REQUIRE(partialReader);
    auto missingLabel = partialReader.Value().ReadText("label");
    REQUIRE(missingLabel);
    CHECK(missingLabel.Value().IsEmpty());

    // 빈 속성은 모든 필드가 기본값이다.
    auto empty = NodePropertyReader::Open(allocator, Span<const uint8>(), ReaderSchema());
    REQUIRE(empty);
    auto fallback = empty.Value().ReadInteger("count", 42);
    REQUIRE(fallback);
    CHECK(fallback.Value() == 42);
    auto list = empty.Value().OpenStructArray("label", ReaderSchema());
    REQUIRE(list);
    CHECK(list.Value().Count() == 0);
    CHECK(list.Value().Element(0).Error() == ErrorCode::InvalidArgument);

    // 다른 schema의 Record는 TypeMismatch다.
    const Array<uint8> foreign = ReaderRecord(allocator, Guid{0x4f54484552ull, 9}, true);
    REQUIRE(!foreign.IsEmpty());
    CHECK(NodePropertyReader::Open(allocator, foreign.View(), ReaderSchema()).Error() ==
          ErrorCode::TypeMismatch);
}

// 새 노드 문자열 기본값(19.13.2절): 대상은 속성 schema의 최상위 String·Name 필드여야 한다.
TEST(MaterialSchema_CatalogChecksTextDefaults) {
    SchemaEnv env;
    REQUIRE(env.ready);
    static constexpr MaterialPinDefinition pins[] = {
        MaterialOutput("Value", MaterialPinType::Float1, "Value"),
    };
    static constexpr MaterialPropertyTextDefault label[] = {{"label", "Moss"}};
    static constexpr MaterialPropertyTextDefault notText[] = {{"count", "1"}};
    static constexpr MaterialPropertyTextDefault unknown[] = {{"missing", "x"}};
    static constexpr MaterialPropertyTextDefault twice[] = {{"label", "Moss"}, {"label", "Fern"}};
    static constexpr MaterialNodeDefinition valid[] = {
        MaterialNode("Material.TestText").Properties(&ReaderSchema).TextDefaults(label).Pins(pins),
    };
    static constexpr MaterialNodeDefinition invalid[] = {
        MaterialNode("Material.TestTextInt")
            .Properties(&ReaderSchema)
            .TextDefaults(notText)
            .Pins(pins),
        MaterialNode("Material.TestTextUnknown")
            .Properties(&ReaderSchema)
            .TextDefaults(unknown)
            .Pins(pins),
        MaterialNode("Material.TestTextNoSchema").TextDefaults(label).Pins(pins),
        MaterialNode("Material.TestTextTwice")
            .Properties(&ReaderSchema)
            .TextDefaults(twice)
            .Pins(pins),
    };
    for (const MaterialNodeDefinition& definition : invalid) {
        CHECK(env.catalog
                  .Register(env.registry, env.material,
                            Span<const MaterialNodeDefinition>(&definition, 1), ModuleLease())
                  .Error() == ErrorCode::InvalidArgument);
    }

    auto group = env.catalog.Register(env.registry, env.material, valid, ModuleLease());
    REQUIRE(group);
    const MaterialNodeDefinition* found = env.catalog.Find(env.Intern("Material.TestText"));
    REQUIRE(found != nullptr);
    REQUIRE(found->textDefaults.Size() == 1);
    CHECK(found->textDefaults[0].text == StringView("Moss"));
    CHECK(env.catalog.Unregister(env.registry, group.Value()));
}

// 등록 실패 보고 문구 버퍼와 그 안에 담는 노드 타입 이름의 최대 byte다(뒤에 오류 이름이 붙는다).
constexpr usize FailureMessageBytes = 256;
constexpr usize FailureTypeBytes = 160;

// 엔진 노드 정의마다 따로 등록해 본다(19.13.6절): 묶음 등록이 실패해도 어느 정의가 무슨 오류로
// 거부되는지 이름으로 보고한다.
TEST(MaterialSchema_EveryBuiltinDefinitionRegisters) {
    SchemaEnv env;
    REQUIRE(env.ready);
    usize count = 0;
    for (Span<const MaterialNodeDefinition> group : BuiltinMaterialNodeGroups()) {
        for (const MaterialNodeDefinition& definition : group) {
            const MaterialGraphSchema& schema =
                (definition.graphKinds & ToMask(MaterialGraphKind::Material)) != 0 ? env.material
                                                                                   : env.function;
            auto registered = env.catalog.Register(
                env.registry, schema, Span<const MaterialNodeDefinition>(&definition, 1),
                ModuleLease());
            if (!registered) {
                char message[FailureMessageBytes];
                const usize length = definition.type.ByteCount() < FailureTypeBytes
                                         ? definition.type.ByteCount()
                                         : FailureTypeBytes;
                usize at = 0;
                for (; at < length; ++at) {
                    message[at] = definition.type.Data()[at];
                }

                const char* error = ErrorCodeName(registered.Error());
                message[at++] = ':';
                message[at++] = ' ';
                for (usize i = 0; error[i] != '\0' && at + 1 < sizeof(message); ++i) {
                    message[at++] = error[i];
                }

                message[at] = '\0';
                test::RecordFailure(message, __FILE__, __LINE__);
                continue;
            }

            ++count;
            CHECK(env.catalog.Unregister(env.registry, registered.Value()));
        }
    }

    CHECK(count > 0);
}

namespace {

// 목록 파일(MaterialNodeCatalog.json)의 최대 크기다(지금 약 100 KiB).
constexpr uint64 MaxCatalogFileBytes = 4u * 1024u * 1024u;

// 목록 파일의 범주 이름을 메뉴 범주 표시로 바꾼다(19.13.4절 — 생성 규칙과 같다).
StringView CategoryTitle(StringView category) noexcept {
    struct Rename {
        StringView from;
        StringView to;
    };

    static constexpr Rename renames[] = {
        {"MaterialAttributes", "Material Attributes"},
        {"PostProcess", "Post Process"},
        {"DistanceFields", "Distance Fields"},
        {"VirtualTexture", "Virtual Texture"},
        {"MeshPaint", "Mesh Paint"},
        {"PhysicsField", "Physics Field"},
    };
    for (const Rename& rename : renames) {
        if (rename.from == category) {
            return rename.to;
        }
    }

    return category;
}

// 이 소스 파일이 있는 모듈 폴더의 목록 파일 경로다(시험은 소스 트리에서 돈다 — __FILE__ 기준).
bool CatalogPath(String& out) noexcept {
    const StringView source(__FILE__);
    usize cut = source.ByteCount();
    usize separators = 0;
    while (cut > 0 && separators < 2) {
        --cut;
        const char c = source.Data()[cut];
        if (c == '/' || c == '\\') {
            ++separators;
        }
    }

    return separators == 2 && out.TryAssign(StringView(source.Data(), cut + 1)) &&
           out.TryAppend("MaterialNodeCatalog.json");
}

} // namespace

// MS-T12 (a)(19.2.2·19.13.6절): 등록되는 엔진 노드 정의의 (id, 상태, 선행, 범주)가 단일 원본
// MaterialNodeCatalog.json의 "구현"·"TODO" 행과 같고, 그 행이 모두 정의되어 있다.
TEST(MaterialSchema_DefinitionsMatchCatalogFile) {
    SystemAllocator allocator;
    String path(allocator);
    REQUIRE(CatalogPath(path));
    Array<uint8> bytes(allocator);
    bool existed = false;
    REQUIRE(ReadWholeFile(platform::PhysicalFileSystem(), path.View(), MaxCatalogFileBytes, bytes,
                          existed));
    REQUIRE(existed);
    JsonDocument document(allocator);
    REQUIRE(document.Parse(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()))
                .code == JsonErrorCode::None);
    const JsonValue entries = document.Root().Find("entries");
    REQUIRE(entries.IsArray());

    usize rows = 0;
    for (usize index = 0; index < entries.Size(); ++index) {
        const JsonValue entry = entries.At(index);
        const StringView status = entry.Find("status").GetString();
        const bool implemented = status == StringView("Implemented");
        if (!implemented && status != StringView("PrerequisiteMissing")) {
            continue;
        }

        ++rows;
        const StringView node = entry.Find("node").GetString();
        const MaterialNodeDefinition* found = nullptr;
        usize matches = 0;
        for (Span<const MaterialNodeDefinition> group : BuiltinMaterialNodeGroups()) {
            for (const MaterialNodeDefinition& definition : group) {
                if (definition.type == node) {
                    found = &definition;
                    ++matches;
                }
            }
        }

        CHECK(matches == 1);
        if (found == nullptr) {
            test::RecordFailure("catalog row without definition", __FILE__, __LINE__);
            continue;
        }

        CHECK((found->status == MaterialNodeStatus::Implemented) == implemented);
        CHECK(MaterialPrerequisiteId(found->prerequisite) ==
              entry.Find("prerequisite").GetString());
        CHECK(found->menu.category == CategoryTitle(entry.Find("category").GetString()));
    }

    // 정의는 목록 행과 출력 노드(Material.Output — 19.8절, 목록 밖)뿐이다.
    usize definitions = 0;
    for (Span<const MaterialNodeDefinition> group : BuiltinMaterialNodeGroups()) {
        definitions += group.Size();
    }

    CHECK(rows > 0);
    CHECK(definitions == rows + 1);
}

} // namespace material_schema_tests_file
