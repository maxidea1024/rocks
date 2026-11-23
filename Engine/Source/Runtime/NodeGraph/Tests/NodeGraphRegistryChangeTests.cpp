#include "NodeGraphMutatorTestSupport.h"

#include <NodeGraph/GraphClone.h>

// 등록표 변경 통지와 문서 다시 읽기 시험이다(Docs/Design/NodeGraph 10.1절 NG3b, 13절 GT8): 해제
// 순서 (조회에서 빼기 → 통지 → lease 놓기), 해제 뒤 자리 노드·바이트·revision 불변·이력 유지,
// 재등록 복원·이력 낡음, 버전 올린 재등록 → migration 전 노드, 핀이 바뀐 재등록 → TypeMismatch·
// DependencyFailed와 Reload 복구, Reload의 도메인·검사·Busy 오류.

using namespace rocks;

namespace node_graph_registry_change_tests_file {

using namespace rocks::nodegraph;
using namespace rocks::nodegraph_test;

constexpr Guid ExtraNode = Id(70);

// 통지를 기록하고, 통지 안에서 그 종류가 조회에서 빠졌는지·등록표를 바꿀 수 없는지 본다.
class RegistryRecorder final : public NodeTypeRegistryListener {
public:
    explicit RegistryRecorder(TestEnv& testEnv) noexcept : env(testEnv), keys(testEnv.allocator) {
    }

    void OnNodeTypesChanged(Span<const NodeTypeKey> changed) noexcept override {
        for (const NodeTypeKey& key : changed) {
            failed = failed || !keys.TryPushBack(key);
            foundDuringCallback = env.registry.FindNodeType(key.domain, key.type) != nullptr;
        }

        // 콜백 안의 등록·해제는 Busy다.
        NodeTypeDescriptor probe;
        probe.type = env.typeUnknown;
        auto nested = env.registry.RegisterNodeType(env.domain, probe, ModuleLease());
        nestedBusy = !nested && nested.Error() == ErrorCode::Busy;
        if (mutator != nullptr) {
            refreshed = mutator->RefreshNodeTypes(TestSource, changed);
        }
    }

    TestEnv& env;
    Array<NodeTypeKey> keys;
    GraphMutator* mutator = nullptr;
    Result<void> refreshed = Result<void>::Success();
    bool foundDuringCallback = false;
    bool nestedBusy = false;
    bool failed = false;
};

struct ExtraType {
    explicit ExtraType(TestEnv& env) noexcept {
        pins[0] = InputPin(env.pinIn, TestFloatType, "In");
        pins[1] = OutputPin(env.pinOut, TestFloatType, "Out");
        descriptor.type = MakeName(env, "Test.Extra");
        descriptor.staticPins = Span<const PinDescriptor>(pins, 2);
    }

    static Name MakeName(TestEnv& env, StringView text) noexcept {
        auto name = env.names.TryIntern(text);
        return name ? name.Value() : Name();
    }

    PinDescriptor pins[2];
    NodeTypeDescriptor descriptor;
};

Result<void> ExtraMigrate(const NodeMigrationInput& input, NodeMigrationResult& out) noexcept {
    out.schemaVersion = input.fromVersion + 1;
    return Result<void>::Success();
}

TEST(NodeGraph_RegistryListener) {
    TestEnv env;
    REQUIRE(env.ready);
    RegistryRecorder recorder(env);
    REQUIRE(env.registry.AddListener(recorder));
    auto again = env.registry.AddListener(recorder);
    CHECK(!again && again.Error() == ErrorCode::AlreadyExists);

    ExtraType extra(env);
    auto id = env.registry.RegisterNodeType(env.domain, extra.descriptor, ModuleLease());
    REQUIRE(id);
    REQUIRE(recorder.keys.Size() == 1);
    CHECK(recorder.keys[0].domain == env.domain && recorder.keys[0].type == extra.descriptor.type);
    CHECK(recorder.foundDuringCallback && recorder.nestedBusy);

    // 해제 통지 때 그 종류는 이미 조회에서 빠져 있다.
    REQUIRE(env.registry.UnregisterNodeType(id.Value()));
    REQUIRE(recorder.keys.Size() == 2);
    CHECK(!recorder.foundDuringCallback && recorder.nestedBusy);
    auto missing = env.registry.UnregisterNodeType(id.Value());
    CHECK(!missing && missing.Error() == ErrorCode::NotFound);

    // 구독을 풀면 더 받지 않는다.
    env.registry.RemoveListener(recorder);
    REQUIRE(env.registry.RegisterNodeType(env.domain, extra.descriptor, ModuleLease()));
    CHECK(recorder.keys.Size() == 2 && !recorder.failed);
}

TEST(NodeGraph_RegistryRefreshAndReload) {
    TestEnv env;
    REQUIRE(env.ready);
    ExtraType extra(env);
    auto id = env.registry.RegisterNodeType(env.domain, extra.descriptor, ModuleLease());
    REQUIRE(id);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    RegistryRecorder registryRecorder(env);
    registryRecorder.mutator = &m.mutator;
    REQUIRE(env.registry.AddListener(registryRecorder));

    // Extra 노드를 더하고 ParamA → Extra.In을 잇는다.
    GraphCommandList add(env.allocator);
    REQUIRE(add.AddNode(SampleMain, MakeNode(env, ExtraNode, extra.descriptor.type),
                        MakeLayout(env, ExtraNode, 0.0f, 0.0f)));
    REQUIRE(add.Connect(SampleMain, MakeEdge(SampleParamA, env.pinValue, ExtraNode, env.pinIn)));
    auto added = m.Run(add);
    REQUIRE(added);

    // 해제: 자리 노드가 되고 바이트·revision·이력은 그대로, 통지 원인 Registry.
    const Array<uint8> before = m.Bytes();
    const uint64 revision = m.mutator.Revision();
    REQUIRE(env.registry.UnregisterNodeType(id.Value()));
    CHECK(registryRecorder.refreshed);
    CHECK(SameBytes(m.Bytes(), before) && m.mutator.Revision() == revision);
    const RecordedBatch& unregistered = m.recorder.Last();
    REQUIRE(unregistered.changes.Size() == 1);
    CHECK(unregistered.cause == GraphChangeCause::Registry && !unregistered.semanticTouched &&
          unregistered.changes[0].kind == GraphChangeKind::NodeTypesChanged &&
          unregistered.changes[0].pin == extra.descriptor.type);
    GraphCommandList connect(env.allocator);
    REQUIRE(
        connect.Connect(SampleMain, MakeEdge(SampleParamB, env.pinValue, ExtraNode, env.pinIn)));
    GraphCommandFailure failure;
    CHECK(!m.Run(connect, &failure) && failure.reason == GraphCommandReason::TypeUnknown);

    // 해제만이면 이력이 그대로다(자리 노드로 undo·redo).
    CHECK(m.mutator.Undo(TestSource, added.Value()));
    CHECK(m.mutator.Redo(TestSource, added.Value()));

    // 다른 도메인 종류는 아무것도 하지 않는다.
    const usize batches = m.recorder.batches.Size();
    const NodeTypeKey foreign{ExtraType::MakeName(env, "Other"), extra.descriptor.type};
    REQUIRE(m.mutator.RefreshNodeTypes(TestSource, Span<const NodeTypeKey>(&foreign, 1)));
    CHECK(m.recorder.batches.Size() == batches);

    // 재등록: 원래 해석으로 돌아오고 이력은 낡는다.
    auto again = env.registry.RegisterNodeType(env.domain, extra.descriptor, ModuleLease());
    REQUIRE(again);
    CHECK(registryRecorder.refreshed);
    auto stale = m.mutator.Undo(TestSource, added.Value());
    CHECK(!stale && stale.Error() == ErrorCode::StaleHandle);

    // 버전 올린 재등록: migration 전 노드가 되고 Migrate로 옮겨진다.
    REQUIRE(env.registry.UnregisterNodeType(again.Value()));
    NodeTypeDescriptor newer = extra.descriptor;
    newer.schemaVersion = 2;
    newer.migrate = &ExtraMigrate;
    auto newerId = env.registry.RegisterNodeType(env.domain, newer, ModuleLease());
    REQUIRE(newerId);
    CHECK(registryRecorder.refreshed);
    CHECK(!m.Run(connect, &failure) && failure.reason == GraphCommandReason::TypeVersion);
    GraphMigrationReport report(env.allocator);
    REQUIRE(m.mutator.Migrate(TestSource, report));
    REQUIRE(report.entries.Size() == 1);
    CHECK(report.entries[0].toVersion == 2 && m.sample.Node(ExtraNode).typeSchemaVersion == 2);

    // 같은 버전인데 입력 핀이 사라진 재등록: 간선이 GI2를 어겨 TypeMismatch, 이후 DependencyFailed.
    REQUIRE(env.registry.UnregisterNodeType(newerId.Value()));
    NodeTypeDescriptor broken = newer;
    broken.staticPins = Span<const PinDescriptor>(extra.pins + 1, 1);
    REQUIRE(env.registry.RegisterNodeType(env.domain, broken, ModuleLease()));
    CHECK(!registryRecorder.refreshed &&
          registryRecorder.refreshed.Error() == ErrorCode::TypeMismatch);
    GraphCommandList layoutOnly(env.allocator);
    REQUIRE(layoutOnly.SetLayout(SampleMain, ExtraNode, MakeLayout(env, ExtraNode, 1.0f, 1.0f)));
    auto refused = m.Run(layoutOnly);
    CHECK(!refused && refused.Error() == ErrorCode::DependencyFailed);
    auto migrateRefused = m.mutator.Migrate(TestSource, report);
    CHECK(!migrateRefused && migrateRefused.Error() == ErrorCode::DependencyFailed);
    auto beginRefused = m.mutator.BeginInteraction(TestSource);
    CHECK(!beginRefused && beginRefused.Error() == ErrorCode::DependencyFailed);

    // Reload: 같은 내용(간선이 그대로라 검사 실패)은 거부, 그 간선을 뺀 문서는 받아들인다.
    GraphDocument same(env.allocator);
    GraphDocumentLayout sameLayout(env.allocator);
    REQUIRE(CloneGraphDocument(m.sample.document, same));
    REQUIRE(CloneGraphDocumentLayout(m.sample.layout, sameLayout));
    CHECK(!m.mutator.Reload(TestSource, Move(same), Move(sameLayout)));

    GraphDocument fixed(env.allocator);
    GraphDocumentLayout fixedLayout(env.allocator);
    REQUIRE(CloneGraphDocument(m.sample.document, fixed));
    REQUIRE(CloneGraphDocumentLayout(m.sample.layout, fixedLayout));
    Graph& main = fixed.graphs[0];
    for (usize index = main.edges.Size(); index > 0; --index) {
        if (main.edges[index - 1].toNode == ExtraNode) {
            REQUIRE(main.edges.TryRemoveAt(index - 1));
        }
    }

    GraphDocument wrongDomain(env.allocator);
    GraphDocumentLayout wrongLayout(env.allocator);
    REQUIRE(CloneGraphDocument(fixed, wrongDomain));
    REQUIRE(CloneGraphDocumentLayout(fixedLayout, wrongLayout));
    wrongDomain.domain = foreign.domain;
    auto mismatch = m.mutator.Reload(TestSource, Move(wrongDomain), Move(wrongLayout));
    CHECK(!mismatch && mismatch.Error() == ErrorCode::TypeMismatch);

    const Array<uint8> expected = DocumentBytes(env, fixed, fixedLayout);
    const uint64 reloadRevision = m.mutator.Revision();
    REQUIRE(m.mutator.Reload(TestSource, Move(fixed), Move(fixedLayout)));
    CHECK(SameBytes(m.Bytes(), expected) && m.mutator.Revision() == reloadRevision + 1);
    const RecordedBatch& reloaded = m.recorder.Last();
    REQUIRE(reloaded.changes.Size() == 1);
    CHECK(reloaded.cause == GraphChangeCause::Reload &&
          reloaded.changes[0].kind == GraphChangeKind::DocumentReloaded);
    REQUIRE(m.Run(layoutOnly));

    // bracket이 열려 있으면 Reload는 Busy, RefreshNodeTypes는 미뤘다가 bracket이 끝날 때 반영한다.
    REQUIRE(m.mutator.BeginInteraction(TestSource));
    GraphDocument during(env.allocator);
    GraphDocumentLayout duringLayout(env.allocator);
    REQUIRE(CloneGraphDocument(m.sample.document, during));
    REQUIRE(CloneGraphDocumentLayout(m.sample.layout, duringLayout));
    auto busy = m.mutator.Reload(TestSource, Move(during), Move(duringLayout));
    CHECK(!busy && busy.Error() == ErrorCode::Busy);
    const NodeTypeKey own{env.domain, broken.type};
    const usize beforeDeferred = m.recorder.batches.Size();
    REQUIRE(m.mutator.RefreshNodeTypes(TestSource, Span<const NodeTypeKey>(&own, 1)));
    CHECK(m.recorder.batches.Size() == beforeDeferred);
    REQUIRE(m.mutator.CancelInteraction(TestSource));
    CHECK(m.recorder.Last().cause == GraphChangeCause::Registry &&
          m.recorder.Last().changes[0].pin == broken.type);
    env.registry.RemoveListener(registryRecorder);
    CHECK(!m.recorder.failed && !registryRecorder.failed);
}

// 버전을 낮춘 재등록: 문서 노드 버전이 서술보다 높아 TypeMismatch, 이후 DependencyFailed.
TEST(NodeGraph_RegistryVersionLowered) {
    TestEnv env;
    REQUIRE(env.ready);
    ExtraType extra(env);
    NodeTypeDescriptor newer = extra.descriptor;
    newer.schemaVersion = 2;
    newer.migrate = &ExtraMigrate;
    auto id = env.registry.RegisterNodeType(env.domain, newer, ModuleLease());
    REQUIRE(id);
    MutatorEnv m(env);
    REQUIRE(m.ready);
    GraphCommandList add(env.allocator);
    REQUIRE(add.AddNode(SampleMain, MakeNode(env, ExtraNode, newer.type, 2),
                        MakeLayout(env, ExtraNode, 0.0f, 0.0f)));
    REQUIRE(m.Run(add));

    REQUIRE(env.registry.UnregisterNodeType(id.Value()));
    const NodeTypeKey key{env.domain, newer.type};
    REQUIRE(m.mutator.RefreshNodeTypes(TestSource, Span<const NodeTypeKey>(&key, 1)));
    REQUIRE(env.registry.RegisterNodeType(env.domain, extra.descriptor, ModuleLease()));
    auto lowered = m.mutator.RefreshNodeTypes(TestSource, Span<const NodeTypeKey>(&key, 1));
    CHECK(!lowered && lowered.Error() == ErrorCode::TypeMismatch);
    GraphCommandList layoutOnly(env.allocator);
    REQUIRE(layoutOnly.SetLayout(SampleMain, ExtraNode, MakeLayout(env, ExtraNode, 1.0f, 1.0f)));
    auto refused = m.Run(layoutOnly);
    CHECK(!refused && refused.Error() == ErrorCode::DependencyFailed);
}

} // namespace node_graph_registry_change_tests_file
