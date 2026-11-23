#pragma once

#include "NodeGraphTestSupport.h"

#include <NodeGraph/GraphClone.h>
#include <NodeGraph/GraphMutator.h>

// GraphMutator 시험 공용 도우미다(Docs/Design/NodeGraph 13절 GT1–GT4·GT6): 문서 전체(의미 +
// 배치) 바이트로 같음 판정, 통지 기록기, 표본 문서 위 mutator 환경.
namespace rocks::nodegraph_test {

// 시험 명령 출처 번호다.
inline constexpr uint64 TestSource = 1;
inline constexpr uint64 OtherSource = 2;

// seed 발급기의 시험 seed다.
inline constexpr uint64 TestIdSeed = 0x4e47314d55544154ull;

// Name 토큰을 쓴다(같은 pool 안 비교용 — 저장 형식이 아니다).
inline bool WriteNameToken(ArchiveWriter& writer, Name name) noexcept {
    return writer.WriteU8(static_cast<uint8>(name.kind)) &&
           writer.WriteU64(name.kind == NameKind::Normal ? name.id : 0) &&
           writer.WriteU32(name.number);
}

inline bool WriteRecordBytes(ArchiveWriter& writer, const PropertyRecord& record) noexcept {
    return static_cast<bool>(writer.WriteBytes(record.bytes.View()));
}

// 문서와 배치의 모든 필드를 배열 순서대로 쓴다. 두 문서가 같은 바이트면 내용·순서가 같다(undo 일치
// 판정 — 13절).
inline Array<uint8> DocumentBytes(TestEnv& env, const GraphDocument& document,
                                  const GraphDocumentLayout& layout) noexcept {
    ArchiveWriter writer(env.allocator,
                         ArchiveLimits{256u * 1024u * 1024u, 4u * 1024u * 1024u,
                                       64ull * 1024ull * 1024ull, 1024ull * 1024ull * 1024ull});
    bool ok = WriteNameToken(writer, document.domain) &&
              writer.WriteU32(document.documentFormatVersion) &&
              writer.WriteGuid(document.mainGraph) &&
              writer.WriteU32(static_cast<uint32>(document.graphs.Size()));
    for (const Graph& graph : document.graphs) {
        ok = ok && writer.WriteGuid(graph.id) && writer.WriteU8(static_cast<uint8>(graph.role)) &&
             writer.WriteU32(graph.domainRole) &&
             writer.WriteU32(static_cast<uint32>(graph.nodes.Size()));
        for (const GraphNode& node : graph.nodes) {
            ok = ok && writer.WriteGuid(node.id) && WriteNameToken(writer, node.type) &&
                 writer.WriteU32(node.typeSchemaVersion) &&
                 WriteRecordBytes(writer, node.properties) &&
                 writer.WriteU8(static_cast<uint8>(node.enableState)) &&
                 writer.WriteU32(static_cast<uint32>(node.inputDefaults.Size()));
            for (const PinValueEntry& entry : node.inputDefaults) {
                ok = ok && WriteNameToken(writer, entry.pin) &&
                     WriteRecordBytes(writer, entry.value);
            }

            ok = ok && writer.WriteU32(static_cast<uint32>(node.retainedPins.Size()));
            for (const RetainedPin& pin : node.retainedPins) {
                ok = ok && WriteNameToken(writer, pin.key) &&
                     writer.WriteU8(static_cast<uint8>(pin.direction)) &&
                     writer.WriteU32(pin.lastType) && WriteRecordBytes(writer, pin.value);
            }
        }

        ok = ok && writer.WriteU32(static_cast<uint32>(graph.edges.Size()));
        for (const GraphEdge& edge : graph.edges) {
            ok = ok && writer.WriteGuid(edge.fromNode) && WriteNameToken(writer, edge.fromPin) &&
                 writer.WriteGuid(edge.toNode) && WriteNameToken(writer, edge.toPin) &&
                 writer.WriteU32(edge.order);
        }
    }

    ok = ok && writer.WriteU32(static_cast<uint32>(layout.graphs.Size()));
    for (const GraphLayout& graph : layout.graphs) {
        ok = ok && writer.WriteGuid(graph.graph) && writer.WriteString(graph.title.View()) &&
             writer.WriteU32(static_cast<uint32>(graph.nodes.Size()));
        for (const NodeLayout& node : graph.nodes) {
            ok = ok && writer.WriteGuid(node.node) && writer.WriteF32(node.position.x) &&
                 writer.WriteF32(node.position.y) && writer.WriteF32(node.size.x) &&
                 writer.WriteF32(node.size.y) && writer.WriteBool(node.collapsed) &&
                 writer.WriteBool(node.showAdvanced) && writer.WriteString(node.bubble.View()) &&
                 writer.WriteBool(node.bubbleVisible);
        }

        ok = ok && writer.WriteU32(static_cast<uint32>(graph.comments.Size()));
        for (const CommentBox& comment : graph.comments) {
            ok = ok && writer.WriteGuid(comment.id) && writer.WriteF32(comment.position.x) &&
                 writer.WriteF32(comment.size.x) && writer.WriteString(comment.text.View()) &&
                 writer.WriteF32(comment.color.r) && writer.WriteU32(comment.fontSize);
        }

        ok = ok && writer.WriteU32(static_cast<uint32>(graph.bookmarks.Size()));
        for (const Bookmark& bookmark : graph.bookmarks) {
            ok = ok && writer.WriteU32(bookmark.number) && writer.WriteF32(bookmark.viewCenter.x) &&
                 writer.WriteF32(bookmark.zoom);
        }
    }

    Array<uint8> bytes(env.allocator);
    if (ok) {
        bytes = writer.TakeBytes();
    }

    return bytes;
}

// 통지 묶음 하나의 기록이다.
struct RecordedBatch {
    explicit RecordedBatch(Allocator& allocator) noexcept : changes(allocator) {
    }

    uint64 revision = 0;
    bool semanticTouched = false;
    GraphChangeCause cause = GraphChangeCause::Edit;
    bool interactive = false;
    Array<GraphChange> changes;
};

// 통지를 모두 기록한다.
class ChangeRecorder final : public GraphChangeListener {
public:
    explicit ChangeRecorder(Allocator& allocator) noexcept : batches(allocator) {
    }

    void OnGraphChanged(const GraphChangeBatch& batch) noexcept override {
        auto added = batches.TryEmplaceBack(batches.GetAllocator());
        if (!added) {
            failed = true;
            return;
        }

        RecordedBatch& recorded = *added.Value();
        recorded.revision = batch.revision;
        recorded.semanticTouched = batch.semanticTouched;
        recorded.cause = batch.cause;
        recorded.interactive = batch.interactive;
        failed = failed || !recorded.changes.TryAppend(batch.changes.View());
    }

    // 마지막 묶음의 통지 종류가 kinds(순서대로)와 같은가.
    bool LastKinds(Span<const GraphChangeKind> kinds) const noexcept {
        if (batches.IsEmpty()) {
            return false;
        }

        const RecordedBatch& last = batches[batches.Size() - 1];
        if (last.changes.Size() != kinds.Size()) {
            return false;
        }

        for (usize index = 0; index < kinds.Size(); ++index) {
            if (last.changes[index].kind != kinds[index]) {
                return false;
            }
        }

        return true;
    }

    const RecordedBatch& Last() const noexcept {
        return batches[batches.Size() - 1];
    }

    Array<RecordedBatch> batches;
    bool failed = false;
};

// 표본 문서 위의 mutator 환경이다(전체 불변식 검사를 켠다).
struct MutatorEnv {
    // prepare는 mutator를 만들기 전에 표본을 고친다(정규 순서로 다시 맞춘다).
    using Prepare = bool (*)(TestEnv& env, Sample& sample) noexcept;

    explicit MutatorEnv(TestEnv& testEnv, Prepare prepare = nullptr) noexcept
        : env(testEnv), sample(testEnv), ids(TestIdSeed, Id(0xA55E7)),
          mutator(sample.document, sample.layout, testEnv.registry, ids),
          recorder(testEnv.allocator) {
        const bool prepared =
            prepare == nullptr || (prepare(testEnv, sample) &&
                                   CanonicalizeGraphDocument(sample.document, &sample.layout,
                                                             testEnv.names, testEnv.allocator));
        ready = sample.ok && prepared && mutator.Initialize() && mutator.AddListener(recorder);
        mutator.SetFullValidation(true);
    }

    Array<uint8> Bytes() noexcept {
        return DocumentBytes(env, sample.document, sample.layout);
    }

    // 명령 목록을 지금 revision으로 실행한다.
    Result<GraphPatch> Run(const GraphCommandList& list,
                           GraphCommandFailure* failure = nullptr) noexcept {
        return mutator.Execute(TestSource, mutator.Revision(), list, failure);
    }

    TestEnv& env;
    Sample sample;
    SeededGraphIdSource ids;
    GraphMutator mutator;
    ChangeRecorder recorder;
    bool ready = false;
};

// 노드 배치 하나를 만든다.
inline NodeLayout MakeLayout(TestEnv& env, Guid node, float x, float y) noexcept {
    NodeLayout layout(env.allocator);
    layout.node = node;
    layout.position = Vec2(x, y);
    return layout;
}

} // namespace rocks::nodegraph_test
