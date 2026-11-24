#include "NodeGraphMutatorTestSupport.h"

#include <NodeGraph/GraphMigration.h>
#include <NodeGraph/GraphSemantic.h>
#include <NodeGraph/GraphValidation.h>

// 핀 재정리와 migration 시험이다(Docs/Design/NodeGraph 10.1절, 13절 GT7): 규칙마다 양성·음성,
// 버전 사슬·키 재지정 합성·1:1 교체·실패 노드 보존과 보고, migration 전 노드 명령 거부, 에디터
// (GraphMutator::Migrate)와 쿠커(MigrateGraphDocument) 결과 바이트·hash 동일, 이력 낡음.

using namespace rocks;

namespace node_graph_migration_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;
using Kind = GraphChangeKind;

constexpr Guid MigMain = Id(1);
constexpr Guid MigParamA = Id(10);
constexpr Guid MigParamB = Id(11);
constexpr Guid MigOld = Id(20);
constexpr Guid MigOther = Id(21);
constexpr Guid MigAddA = Id(30);
constexpr Guid MigAddB = Id(31);

// 시험용 Name 하나다(실패하면 None — 검사가 잡는다).
Name MigName(NamePool& names, StringView text) noexcept {
    auto name = names.TryIntern(text);
    return name ? name.Value() : Name();
}

Result<void> MigRename(NodeMigrationResult& out, NamePool& names, StringView from,
                       StringView to) noexcept {
    PinKeyRename rename;
    rename.from = MigName(names, from);
    rename.to = MigName(names, to);
    return out.renames.TryPushBack(rename);
}

// Test.Old: v1 → v2(A → X) → v3(X → Y).
Result<void> OldMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.schemaVersion = input.fromVersion + 1;
    return input.fromVersion == 1 ? MigRename(out, input.names, "A", "X")
                                  : MigRename(out, input.names, "X", "Y");
}

// Test.Legacy v1 → Test.Add v1(Lhs → A, 1:1 교체).
Result<void> LegacyMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.type = MigName(input.names, "Test.Add");
    out.schemaVersion = 1;
    return MigRename(out, input.names, "Lhs", "A");
}

Result<void> NoAdvanceMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.schemaVersion = input.fromVersion;
    return Result<void>::Success();
}

Result<void> FailMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    static_cast<void>(input);
    static_cast<void>(out);
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> StepMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.schemaVersion = input.fromVersion + 1;
    return Result<void>::Success();
}

// 이미 쓰인 B로 A를 재지정한다(항등 사상과 겹침).
Result<void> CollideMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.schemaVersion = 2;
    return MigRename(out, input.names, "A", "B");
}

Result<void> WrongPropsMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    static_cast<void>(input);
    out.schemaVersion = 2;
    return FillRecord(out.properties.bytes, TestValueStruct, 1, 1)
               ? Result<void>::Success()
               : Result<void>::Failure(ErrorCode::OutOfMemory);
}

Result<void> ToCommonMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.type = MigName(input.names, RerouteNodeTypeName);
    out.schemaVersion = 1;
    return Result<void>::Success();
}

// A ↔ B 교환.
Result<void> SwapMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.schemaVersion = 2;
    if (auto first = MigRename(out, input.names, "A", "B"); !first) {
        return first;
    }

    return MigRename(out, input.names, "B", "A");
}

// A → B, B → C 연쇄(밀기).
Result<void> PushMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.schemaVersion = 2;
    if (auto first = MigRename(out, input.names, "A", "B"); !first) {
        return first;
    }

    return MigRename(out, input.names, "B", "C");
}

// A → None(재지정 결과 키 없음).
Result<void> NoneKeyMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.schemaVersion = 2;
    PinKeyRename rename;
    rename.from = MigName(input.names, "A");
    return out.renames.TryPushBack(rename);
}

// Test.LegacyOld v1 → Test.Old v1(Lhs → A)로 교체한다. 그 뒤 Test.Old의 migrate가 이어받는다.
Result<void> LegacyOldMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.type = MigName(input.names, "Test.Old");
    out.schemaVersion = 1;
    return MigRename(out, input.names, "Lhs", "A");
}

Result<void> ToSingleMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.type = MigName(input.names, "Test.Unique");
    out.schemaVersion = 1;
    return Result<void>::Success();
}

// migration 시험 종류를 등록하는 환경이다.
struct MigEnv {
    explicit MigEnv(TestEnv& testEnv) noexcept : env(testEnv) {
        NamePool& names = env.names;
        pinX = MigName(names, "X");
        pinY = MigName(names, "Y");
        pinLhs = MigName(names, "Lhs");
        typeOld = MigName(names, "Test.Old");
        typeLegacy = MigName(names, "Test.Legacy");
        typeNoAdvance = MigName(names, "Test.NoAdvance");
        typeFail = MigName(names, "Test.Fail");
        typeEndless = MigName(names, "Test.Endless");
        typeCollide = MigName(names, "Test.Collide");
        typeWrongProps = MigName(names, "Test.WrongProps");
        typeToCommon = MigName(names, "Test.ToCommon");
        typeToSingle = MigName(names, "Test.ToSingle");
        typeUnique = MigName(names, "Test.Unique");
        typeSwap = MigName(names, "Test.Swap");
        typePush = MigName(names, "Test.Push");
        typeNoneKey = MigName(names, "Test.NoneKey");
        typeLegacyOld = MigName(names, "Test.LegacyOld");
        pinC = MigName(names, "C");

        PinDescriptor result = OutputPin(env.pinResult, TestFloatType, "Result");
        result.ordered = true;
        const PinDescriptor oldPins[] = {InputPin(pinY, TestFloatType, "Y"),
                                         InputPin(env.pinB, TestFloatType, "B"), result};
        NodeTypeDescriptor old;
        old.type = typeOld;
        old.schemaVersion = 3;
        old.staticPins = oldPins;
        old.migrate = &OldMigrate;

        const PinDescriptor abPins[] = {InputPin(env.pinA, TestFloatType, "A"),
                                        InputPin(env.pinB, TestFloatType, "B")};
        const auto versioned = [&abPins](Name type, uint32 version, MigrateFn migrate) noexcept {
            NodeTypeDescriptor descriptor;
            descriptor.type = type;
            descriptor.schemaVersion = version;
            descriptor.staticPins = abPins;
            descriptor.migrate = migrate;
            return descriptor;
        };

        const PinDescriptor bcPins[] = {InputPin(env.pinB, TestFloatType, "B"),
                                        InputPin(pinC, TestFloatType, "C"),
                                        OutputPin(env.pinResult, TestFloatType, "Result")};
        NodeTypeDescriptor push;
        push.type = typePush;
        push.schemaVersion = 2;
        push.staticPins = bcPins;
        push.migrate = &PushMigrate;

        NodeTypeDescriptor unique;
        unique.type = typeUnique;
        unique.capabilities = DefaultNodeCapabilities | NodeCapabilities::SinglePerDocument;

        ready = env.ready && Register(old) && Register(versioned(typeLegacy, 2, &LegacyMigrate)) &&
                Register(versioned(typeNoAdvance, 2, &NoAdvanceMigrate)) &&
                Register(versioned(typeFail, 2, &FailMigrate)) &&
                Register(versioned(typeEndless, MaxMigrationSteps + 10, &StepMigrate)) &&
                Register(versioned(typeCollide, 2, &CollideMigrate)) &&
                Register(versioned(typeWrongProps, 2, &WrongPropsMigrate)) &&
                Register(versioned(typeToCommon, 2, &ToCommonMigrate)) &&
                Register(versioned(typeToSingle, 2, &ToSingleMigrate)) && Register(unique) &&
                Register(versioned(typeSwap, 2, &SwapMigrate)) && Register(push) &&
                Register(versioned(typeNoneKey, 2, &NoneKeyMigrate)) &&
                Register(versioned(typeLegacyOld, 2, &LegacyOldMigrate));
    }

    bool Register(const NodeTypeDescriptor& descriptor) noexcept {
        return static_cast<bool>(
            env.registry.RegisterNodeType(env.domain, descriptor, ModuleLease()));
    }

    TestEnv& env;
    Name pinX;
    Name pinY;
    Name pinLhs;
    Name typeOld;
    Name typeLegacy;
    Name typeNoAdvance;
    Name typeFail;
    Name typeEndless;
    Name typeCollide;
    Name typeWrongProps;
    Name typeToCommon;
    Name typeToSingle;
    Name typeUnique;
    Name typeSwap;
    Name typePush;
    Name typeNoneKey;
    Name typeLegacyOld;
    Name pinC;
    bool ready = false;
};

// 주 그래프 하나짜리 문서다(배치는 노드마다 하나, 정규화까지).
struct MigDocument {
    explicit MigDocument(TestEnv& testEnv) noexcept
        : env(testEnv), document(testEnv.allocator), layout(testEnv.allocator),
          main(testEnv.allocator) {
        main.id = MigMain;
        main.role = GraphRole::Main;
    }

    bool Add(GraphNode&& node) noexcept {
        return static_cast<bool>(main.nodes.TryPushBack(Move(node)));
    }

    bool Connect(const GraphEdge& edge) noexcept {
        return static_cast<bool>(main.edges.TryPushBack(edge));
    }

    bool Finish() noexcept {
        document.domain = env.domain;
        document.mainGraph = MigMain;
        GraphLayout entry(env.allocator);
        entry.graph = MigMain;
        for (const GraphNode& node : main.nodes) {
            NodeLayout position(env.allocator);
            position.node = node.id;
            if (!entry.nodes.TryPushBack(Move(position))) {
                return false;
            }
        }

        return document.graphs.TryPushBack(Move(main)) && layout.graphs.TryPushBack(Move(entry)) &&
               CanonicalizeGraphDocument(document, &layout, env.names, env.allocator);
    }

    TestEnv& env;
    GraphDocument document;
    GraphDocumentLayout layout;
    Graph main;
};

const GraphNode* MigFind(const GraphDocument& document, Guid id) noexcept {
    for (const Graph& graph : document.graphs) {
        for (const GraphNode& node : graph.nodes) {
            if (node.id == id) {
                return &node;
            }
        }
    }

    return nullptr;
}

bool MigHasEdge(const GraphDocument& document, const GraphEdge& edge) noexcept {
    for (const GraphEdge& candidate : document.graphs[0].edges) {
        if (candidate.fromNode == edge.fromNode && candidate.fromPin == edge.fromPin &&
            candidate.toNode == edge.toNode && candidate.toPin == edge.toPin &&
            candidate.order == edge.order) {
            return true;
        }
    }

    return false;
}

// Test.Old v1 노드 하나와 그 주변(값·간선·순번 간선·사라지는 핀)을 만든다.
bool BuildOldDocument(MigEnv& mig, MigDocument& doc) noexcept {
    TestEnv& env = mig.env;
    GraphNode old = MakeNode(env, MigOld, mig.typeOld, 1);
    PinValueEntry value(env.allocator);
    value.pin = env.pinB;
    bool ok = FillRecord(value.value.bytes, TestValueStruct, 1, 9) &&
              old.inputDefaults.TryPushBack(Move(value));
    return ok && doc.Add(MakeParam(env, MigParamA, 1)) && doc.Add(MakeParam(env, MigParamB, 2)) &&
           doc.Add(Move(old)) && doc.Add(MakeNode(env, MigAddA, env.typeAdd)) &&
           doc.Add(MakeNode(env, MigAddB, env.typeAdd)) &&
           doc.Connect(MakeEdge(MigParamA, env.pinValue, MigOld, env.pinA)) &&
           doc.Connect(MakeEdge(MigParamB, env.pinValue, MigOld, env.pinGone)) &&
           doc.Connect(MakeEdge(MigOld, env.pinResult, MigAddA, env.pinA)) &&
           doc.Connect(MakeEdge(MigOld, env.pinResult, MigAddB, env.pinA)) && doc.Finish();
}

TEST(NodeGraph_MigrationChain) {
    TestEnv env;
    MigEnv mig(env);
    REQUIRE(mig.ready);
    MigDocument doc(env);
    REQUIRE(BuildOldDocument(mig, doc));

    SeededGraphIdSource ids(TestIdSeed, Id(0xA55E7));
    GraphMutator mutator(doc.document, doc.layout, env.registry, ids);
    REQUIRE(mutator.Initialize());
    mutator.SetFullValidation(true);
    ChangeRecorder recorder(env.allocator);
    REQUIRE(mutator.AddListener(recorder));

    // migration 전 노드: 핀이 필요한 명령은 TypeVersion, 배치·간선 끊기는 된다.
    const auto rejects = [&](const GraphCommandList& list, GraphCommandReason reason) noexcept {
        GraphCommandFailure failure;
        auto patch = mutator.Execute(TestSource, mutator.Revision(), list, &failure);
        return !patch && failure.reason == reason;
    };

    Array<uint8> bytes(env.allocator);
    REQUIRE(FillRecord(bytes, TestValueStruct, 1, 4));
    GraphCommandList connect(env.allocator);
    REQUIRE(connect.Connect(MigMain, MakeEdge(MigParamB, env.pinValue, MigOld, env.pinB)));
    CHECK(rejects(connect, GraphCommandReason::TypeVersion));
    GraphCommandList setDefault(env.allocator);
    REQUIRE(setDefault.SetInputDefault(MigMain, MigOld, env.pinB, bytes.View()));
    CHECK(rejects(setDefault, GraphCommandReason::TypeVersion));
    GraphCommandList properties(env.allocator);
    REQUIRE(properties.SetNodeProperties(MigMain, MigOld, Span<const uint8>()));
    CHECK(rejects(properties, GraphCommandReason::TypeVersion));
    GraphCommandList disable(env.allocator);
    REQUIRE(disable.SetEnableState(MigMain, MigOld, NodeEnableState::Disabled));
    CHECK(rejects(disable, GraphCommandReason::TypeVersion));

    GraphCommandList layoutOnly(env.allocator);
    REQUIRE(layoutOnly.SetLayout(MigMain, MigOld, MakeLayout(env, MigOld, 5.0f, 6.0f)));
    auto earlier = mutator.Execute(TestSource, mutator.Revision(), layoutOnly);
    REQUIRE(earlier);

    // 쿠커 경로(자유 함수)용 사본이다.
    GraphDocument cookDocument(env.allocator);
    GraphDocumentLayout cookLayout(env.allocator);
    REQUIRE(CloneGraphDocument(doc.document, cookDocument));
    REQUIRE(CloneGraphDocumentLayout(doc.layout, cookLayout));

    // bracket이 열려 있으면 Busy다.
    GraphMigrationReport report(env.allocator);
    REQUIRE(mutator.BeginInteraction(TestSource));
    auto busy = mutator.Migrate(TestSource, report);
    CHECK(!busy && busy.Error() == ErrorCode::Busy);
    REQUIRE(mutator.CancelInteraction(TestSource));

    const uint64 revision = mutator.Revision();
    REQUIRE(mutator.Migrate(TestSource, report));
    CHECK(mutator.Revision() == revision + 1);
    REQUIRE(report.entries.Size() == 1);
    const GraphMigrationEntry& entry = report.entries[0];
    CHECK(entry.node == MigOld && entry.fromType == mig.typeOld && entry.fromVersion == 1 &&
          entry.toType == mig.typeOld && entry.toVersion == 3 && entry.retainedCreated == 1 &&
          entry.failure == GraphCommandReason::None);

    // 키 재지정 A → X → Y, 사라진 Gone은 고아 핀(마지막 타입 모름), 값 B는 그대로, ordered 출력
    // Result는 정규 순서로 0·1.
    const GraphNode* migrated = MigFind(doc.document, MigOld);
    REQUIRE(migrated != nullptr);
    CHECK(migrated->typeSchemaVersion == 3);
    REQUIRE(migrated->retainedPins.Size() == 1);
    CHECK(migrated->retainedPins[0].key == env.pinGone &&
          migrated->retainedPins[0].direction == PinDirection::Input &&
          migrated->retainedPins[0].lastType == UnresolvedPinType &&
          migrated->retainedPins[0].value.bytes.IsEmpty());
    REQUIRE(migrated->inputDefaults.Size() == 1);
    CHECK(migrated->inputDefaults[0].pin == env.pinB);
    CHECK(MigHasEdge(doc.document, MakeEdge(MigParamA, env.pinValue, MigOld, mig.pinY)));
    CHECK(MigHasEdge(doc.document, MakeEdge(MigParamB, env.pinValue, MigOld, env.pinGone)));
    CHECK(MigHasEdge(doc.document, MakeEdge(MigOld, env.pinResult, MigAddA, env.pinA, 0)));
    CHECK(MigHasEdge(doc.document, MakeEdge(MigOld, env.pinResult, MigAddB, env.pinA, 1)));

    // 통지: 원인 Migration, 핀 목록 바뀜, 고아 핀 생김.
    const RecordedBatch& batch = recorder.Last();
    CHECK(batch.cause == GraphChangeCause::Migration && batch.semanticTouched &&
          !batch.interactive && batch.revision == revision + 1);
    bool pinsChanged = false;
    bool retainedChanged = false;
    for (const GraphChange& change : batch.changes) {
        pinsChanged = pinsChanged || change.kind == Kind::NodePinsChanged;
        retainedChanged = retainedChanged || change.kind == Kind::RetainedPinChanged;
    }

    CHECK(pinsChanged && retainedChanged);

    // undo 이력 밖이라 그 전 patch는 낡는다.
    auto stale = mutator.Undo(TestSource, earlier.Value());
    CHECK(!stale && stale.Error() == ErrorCode::StaleHandle);

    // 옮길 것이 없으면 아무것도 바꾸지 않는다.
    const usize batches = recorder.batches.Size();
    REQUIRE(mutator.Migrate(TestSource, report));
    CHECK(report.entries.IsEmpty() && mutator.Revision() == revision + 1 &&
          recorder.batches.Size() == batches);

    // 쿠커 경로: 같은 바이트·같은 의미 hash다(GT7). 배치 하나만 다르므로 맞춘다.
    GraphMigrationReport cookReport(env.allocator);
    REQUIRE(MigrateGraphDocument(cookDocument, cookLayout, env.registry, cookReport));
    REQUIRE(cookReport.entries.Size() == 1);
    CHECK(cookReport.entries[0].toVersion == 3);
    REQUIRE(CloneGraphDocumentLayout(doc.layout, cookLayout));
    CHECK(SameBytes(DocumentBytes(env, cookDocument, cookLayout),
                    DocumentBytes(env, doc.document, doc.layout)));
    auto editorHash = ComputeSemanticHash(doc.document, env.registry, env.allocator);
    auto cookHash = ComputeSemanticHash(cookDocument, env.registry, env.allocator);
    REQUIRE(editorHash && cookHash);
    CHECK(editorHash.Value() == cookHash.Value());
    CHECK(!recorder.failed);
}

TEST(NodeGraph_MigrationReplaceAndFailures) {
    TestEnv env;
    MigEnv mig(env);
    REQUIRE(mig.ready);
    MigDocument doc(env);
    const Name types[] = {mig.typeLegacy,   mig.typeNoAdvance, mig.typeFail,
                          mig.typeEndless,  mig.typeCollide,   mig.typeWrongProps,
                          mig.typeToCommon, mig.typeToSingle,  mig.typeToSingle};
    const GraphCommandReason expected[] = {GraphCommandReason::None,
                                           GraphCommandReason::MigrationInvalid,
                                           GraphCommandReason::MigrationInvalid,
                                           GraphCommandReason::MigrationInvalid,
                                           GraphCommandReason::MigrationInvalid,
                                           GraphCommandReason::PropertiesSchema,
                                           GraphCommandReason::MigrationInvalid,
                                           GraphCommandReason::None,
                                           GraphCommandReason::SinglePerDocument};
    constexpr usize count = sizeof(types) / sizeof(types[0]);
    REQUIRE(doc.Add(MakeParam(env, MigParamA, 1)));
    for (usize index = 0; index < count; ++index) {
        REQUIRE(doc.Add(MakeNode(env, Id(100 + index), types[index], 1)));
    }

    // 교체 노드 Lhs 간선, 겹치는 재지정 노드의 A·B 간선.
    REQUIRE(doc.Connect(MakeEdge(MigParamA, env.pinValue, Id(100), mig.pinLhs)));
    REQUIRE(doc.Connect(MakeEdge(MigParamA, env.pinValue, Id(104), env.pinA)));
    REQUIRE(doc.Connect(MakeEdge(MigParamA, env.pinValue, Id(104), env.pinB)));
    REQUIRE(doc.Finish());

    SeededGraphIdSource ids(TestIdSeed, Id(0xA55E7));
    GraphMutator mutator(doc.document, doc.layout, env.registry, ids);
    REQUIRE(mutator.Initialize());
    mutator.SetFullValidation(true);
    GraphDocument before(env.allocator);
    REQUIRE(CloneGraphDocument(doc.document, before));

    GraphMigrationReport report(env.allocator);
    REQUIRE(mutator.Migrate(TestSource, report));
    REQUIRE(report.entries.Size() == count);
    for (usize index = 0; index < count; ++index) {
        const GraphMigrationEntry& entry = report.entries[index];
        CHECK(entry.node == Id(100 + index) && entry.fromType == types[index]);
        CHECK(entry.failure == expected[index]);
        const GraphNode* now = MigFind(doc.document, Id(100 + index));
        const GraphNode* old = MigFind(before, Id(100 + index));
        REQUIRE(now != nullptr && old != nullptr);
        if (expected[index] != GraphCommandReason::None) {
            // 실패한 노드는 옛 버전 그대로다.
            CHECK(now->type == old->type && now->typeSchemaVersion == 1 && entry.toVersion == 0);
        }
    }

    CHECK(report.entries[4].pin == env.pinB);
    const GraphNode* replaced = MigFind(doc.document, Id(100));
    REQUIRE(replaced != nullptr);
    CHECK(replaced->type == env.typeAdd && replaced->typeSchemaVersion == 1 &&
          report.entries[0].toType == env.typeAdd);
    CHECK(MigHasEdge(doc.document, MakeEdge(MigParamA, env.pinValue, Id(100), env.pinA)));
    CHECK(MigFind(doc.document, Id(107))->type == mig.typeUnique);

    // 모두 실패하면 revision·문서가 그대로다(다시 부르면 실패 노드만 다시 보고).
    const uint64 revision = mutator.Revision();
    REQUIRE(mutator.Migrate(TestSource, report));
    CHECK(mutator.Revision() == revision && report.entries.Size() == count - 2);
}

// 키 재지정 교환·연쇄·None, 자기 간선, 양 끝이 모두 옮겨지는 간선, 1:1 교체 뒤 이어받기다.
TEST(NodeGraph_MigrationRenameShapes) {
    TestEnv env;
    MigEnv mig(env);
    REQUIRE(mig.ready);
    MigDocument doc(env);
    constexpr Guid swap = Id(40);
    constexpr Guid push = Id(41);
    constexpr Guid noneKey = Id(42);
    constexpr Guid legacy = Id(43);
    constexpr Guid oldFirst = Id(44);
    constexpr Guid oldSecond = Id(45);
    REQUIRE(doc.Add(MakeParam(env, MigParamA, 1)));
    REQUIRE(doc.Add(MakeNode(env, swap, mig.typeSwap, 1)));
    REQUIRE(doc.Add(MakeNode(env, push, mig.typePush, 1)));
    REQUIRE(doc.Add(MakeNode(env, noneKey, mig.typeNoneKey, 1)));
    REQUIRE(doc.Add(MakeNode(env, legacy, mig.typeLegacyOld, 1)));
    REQUIRE(doc.Add(MakeNode(env, oldFirst, mig.typeOld, 1)));
    REQUIRE(doc.Add(MakeNode(env, oldSecond, mig.typeOld, 1)));
    // 같은 출력 하나를 두 입력에: 교환·연쇄 재지정에서 간선이 사라지지 않아야 한다.
    REQUIRE(doc.Connect(MakeEdge(MigParamA, env.pinValue, swap, env.pinA)));
    REQUIRE(doc.Connect(MakeEdge(MigParamA, env.pinValue, swap, env.pinB)));
    REQUIRE(doc.Connect(MakeEdge(MigParamA, env.pinValue, push, env.pinA)));
    REQUIRE(doc.Connect(MakeEdge(MigParamA, env.pinValue, push, env.pinB)));
    REQUIRE(doc.Connect(MakeEdge(MigParamA, env.pinValue, noneKey, env.pinA)));
    // 교체 뒤 이어받기: Lhs → A(교체) → X → Y(Test.Old).
    REQUIRE(doc.Connect(MakeEdge(MigParamA, env.pinValue, legacy, mig.pinLhs)));
    // 자기 간선과 양 끝이 모두 옮겨지는 간선(Test.Old의 A → Y).
    REQUIRE(doc.Connect(MakeEdge(oldFirst, env.pinResult, oldFirst, env.pinA)));
    REQUIRE(doc.Connect(MakeEdge(oldFirst, env.pinResult, oldSecond, env.pinA)));
    REQUIRE(doc.Finish());

    GraphMigrationReport report(env.allocator);
    REQUIRE(MigrateGraphDocument(doc.document, doc.layout, env.registry, report));
    REQUIRE(report.entries.Size() == 6);
    CHECK(report.entries[0].node == swap && report.entries[0].failure == GraphCommandReason::None);
    CHECK(report.entries[1].node == push && report.entries[1].failure == GraphCommandReason::None);
    CHECK(report.entries[2].node == noneKey &&
          report.entries[2].failure == GraphCommandReason::MigrationInvalid);
    CHECK(report.entries[3].node == legacy && report.entries[3].toType == mig.typeOld &&
          report.entries[3].toVersion == 3);
    CHECK(report.entries[4].failure == GraphCommandReason::None &&
          report.entries[5].failure == GraphCommandReason::None);

    CHECK(MigHasEdge(doc.document, MakeEdge(MigParamA, env.pinValue, swap, env.pinA)));
    CHECK(MigHasEdge(doc.document, MakeEdge(MigParamA, env.pinValue, swap, env.pinB)));
    CHECK(MigHasEdge(doc.document, MakeEdge(MigParamA, env.pinValue, push, env.pinB)));
    CHECK(MigHasEdge(doc.document, MakeEdge(MigParamA, env.pinValue, push, mig.pinC)));
    CHECK(!MigHasEdge(doc.document, MakeEdge(MigParamA, env.pinValue, push, env.pinA)));
    CHECK(MigHasEdge(doc.document, MakeEdge(MigParamA, env.pinValue, legacy, mig.pinY)));
    CHECK(MigFind(doc.document, legacy)->type == mig.typeOld &&
          MigFind(doc.document, legacy)->typeSchemaVersion == 3);
    CHECK(MigHasEdge(doc.document, MakeEdge(oldFirst, env.pinResult, oldFirst, mig.pinY, 0)));
    CHECK(MigHasEdge(doc.document, MakeEdge(oldFirst, env.pinResult, oldSecond, mig.pinY, 1)));
    CHECK(MigFind(doc.document, noneKey)->typeSchemaVersion == 1);

    usize edges = 0;
    for (const GraphEdge& edge : doc.document.graphs[0].edges) {
        edges += edge.fromNode == MigParamA ? 1 : 0;
    }

    CHECK(edges == 6);
    GraphViolationList violations(env.allocator);
    CHECK(
        ValidateGraphDocument(doc.document, &doc.layout, env.registry, env.allocator, violations));
}

TEST(NodeGraph_MigrationDisabledAndRegistration) {
    TestEnv env;
    MigEnv mig(env);
    REQUIRE(mig.ready);

    // 버전이 1보다 큰데 migrate가 없으면 등록을 거부한다.
    NodeTypeDescriptor missing;
    missing.type = MigName(env.names, "Test.NoMigrate");
    missing.schemaVersion = 2;
    auto registered = env.registry.RegisterNodeType(env.domain, missing, ModuleLease());
    CHECK(!registered && registered.Error() == ErrorCode::InvalidArgument);

    // 비활성 노드가 비활성 허용 없는 종류로 가면 EnableState다.
    MigDocument doc(env);
    GraphNode disabled = MakeNode(env, MigOther, mig.typeLegacy, 1);
    disabled.enableState = NodeEnableState::Disabled;
    REQUIRE(doc.Add(Move(disabled)));
    REQUIRE(doc.Finish());
    GraphMigrationReport report(env.allocator);
    REQUIRE(MigrateGraphDocument(doc.document, doc.layout, env.registry, report));
    REQUIRE(report.entries.Size() == 1);
    CHECK(report.entries[0].failure == GraphCommandReason::EnableState);
    CHECK(MigFind(doc.document, MigOther)->type == mig.typeLegacy);
}

// SetNodeProperties의 핀 재정리다(고아 핀 생성·재결합·값 이동·방향 충돌·Hidden·간선 상한).
TEST(NodeGraph_ReconcileSetProperties) {
    TestEnv env;
    REQUIRE(env.ready);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    const auto setVersion = [&](uint32 version, GraphCommandFailure* failure) noexcept {
        Array<uint8> bytes(env.allocator);
        GraphCommandList list(env.allocator);
        return FillRecord(bytes, TestDynamicStruct, version, 0) &&
               list.SetNodeProperties(SampleMain, SampleDynamic, bytes.View()) &&
               static_cast<bool>(m.Run(list, failure));
    };
    const auto dynamicNode = [&]() noexcept -> const GraphNode& {
        return m.sample.Node(SampleDynamic);
    };

    // 간선 있는 Item_1이 사라지면 고아 핀(마지막 타입 Float)이 되고 간선은 그대로다.
    REQUIRE(setVersion(1, nullptr));
    REQUIRE(dynamicNode().retainedPins.Size() == 1);
    CHECK(dynamicNode().retainedPins[0].key == env.pinItem1 &&
          dynamicNode().retainedPins[0].lastType == TestFloatType);
    CHECK(MigHasEdge(m.sample.document,
                     MakeEdge(SampleParamA, env.pinValue, SampleDynamic, env.pinItem1)));

    // 반대 방향(출력 Item_1)으로 다시 생기면 RetainedShadowed, Hidden 입력이어도 간선은 남는다.
    GraphCommandFailure failure;
    CHECK(!setVersion(TestDynamicOutputVersion, &failure));
    CHECK(failure.reason == GraphCommandReason::RetainedShadowed && failure.pin == env.pinItem1);

    // 같은 방향으로 다시 생기면 재결합한다(고아 핀 제거, 간선 유지).
    REQUIRE(setVersion(3, nullptr));
    CHECK(dynamicNode().retainedPins.IsEmpty());
    CHECK(MigHasEdge(m.sample.document,
                     MakeEdge(SampleParamA, env.pinValue, SampleDynamic, env.pinItem1)));

    // 값만 있는 핀: 값이 고아 핀으로 갔다가 재결합 때 입력 기본값으로 돌아온다.
    Array<uint8> value(env.allocator);
    REQUIRE(FillRecord(value, TestValueStruct, 1, 5));
    GraphCommandList setValue(env.allocator);
    REQUIRE(setValue.Disconnect(SampleMain,
                                MakeEdge(SampleParamA, env.pinValue, SampleDynamic, env.pinItem1)));
    REQUIRE(setValue.SetInputDefault(SampleMain, SampleDynamic, env.pinItem1, value.View()));
    REQUIRE(m.Run(setValue));
    REQUIRE(setVersion(1, nullptr));
    REQUIRE(dynamicNode().retainedPins.Size() == 1);
    CHECK(SameBytes(dynamicNode().retainedPins[0].value.bytes, value));
    CHECK(dynamicNode().inputDefaults.IsEmpty());

    // Hidden 입력으로 재결합하면 값이 있어 ValueHidden이다.
    CHECK(!setVersion(TestDynamicHiddenVersion, &failure));
    CHECK(failure.reason == GraphCommandReason::ValueHidden);
    REQUIRE(setVersion(2, nullptr));
    REQUIRE(dynamicNode().inputDefaults.Size() == 1);
    CHECK(dynamicNode().inputDefaults[0].pin == env.pinItem1 &&
          SameBytes(dynamicNode().inputDefaults[0].value.bytes, value));
    CHECK(dynamicNode().retainedPins.IsEmpty());

    // 현재 입력 핀에 값이 있는데 그 핀이 Hidden이 되면 ValueHidden이다.
    CHECK(!setVersion(TestDynamicHiddenVersion, &failure));
    CHECK(failure.reason == GraphCommandReason::ValueHidden);

    // 고아 핀에 간선 둘(붙여넣기 InsertEdge)을 두면 재결합 때 상한(1)을 넘어 EdgeLimit다.
    GraphCommandList reset(env.allocator);
    REQUIRE(reset.ResetInputDefault(SampleMain, SampleDynamic, env.pinItem1));
    REQUIRE(reset.Connect(SampleMain,
                          MakeEdge(SampleParamA, env.pinValue, SampleDynamic, env.pinItem1)));
    REQUIRE(m.Run(reset));
    REQUIRE(setVersion(1, nullptr));
    GraphCommandList second(env.allocator);
    REQUIRE(second.InsertEdge(SampleMain,
                              MakeEdge(SampleParamB, env.pinValue, SampleDynamic, env.pinItem1)));
    REQUIRE(m.Run(second));
    CHECK(!setVersion(2, &failure));
    CHECK(failure.reason == GraphCommandReason::EdgeLimit && failure.pin == env.pinItem1);

    // 간선 하나를 끊으면 다시 된다. 간선이 모두 없어진 고아 핀은 GI7로 지운다.
    GraphCommandList cut(env.allocator);
    REQUIRE(cut.Disconnect(SampleMain,
                           MakeEdge(SampleParamB, env.pinValue, SampleDynamic, env.pinItem1)));
    REQUIRE(m.Run(cut));
    REQUIRE(setVersion(2, nullptr));
    CHECK(dynamicNode().retainedPins.IsEmpty());
    CHECK(!m.recorder.failed);
}

} // namespace node_graph_migration_tests_file
