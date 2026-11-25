#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <MaterialSchema/MaterialGraphSchema.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <MaterialSchema/MaterialSchemaModule.h>
#include <Meta/TypeSchema.h>
#include <NodeGraph/GraphCommand.h>
#include <NodeGraph/GraphIdSource.h>
#include <NodeGraph/GraphReadView.h>
#include <NodeGraph/GraphSemantic.h>
#include <NodeGraph/NodeTypeRegistry.h>
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
    CHECK(commands.Size() == 1);

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

} // namespace material_schema_tests_file
