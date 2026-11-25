#include "GraphTextInternal.h"

#include <Core/Algorithms.h>
#include <Core/Math/MathUtility.h>
#include <Core/Text/NamePool.h>
#include <NodeGraph/GraphNameOrder.h>
#include <NodeGraphTools/GraphDiff.h>

// 문서 diff다(Docs/Design/NodeGraph 9.4절 "diff"). 그래프·노드·간선·배치·주석·북마크를 키로
// 정렬한 사본을 병합 한 번으로 맞춘다(O(N log N) — 입력이 정규 순서가 아니어도 된다). 간선 키의
// Name은 토큰 값(같은 pool)으로 비교한다 — 표시 순서가 아니라 짝 맞추기용이다.
namespace rocks::nodegraph {
namespace {

using namespace text_detail;

int32 CompareToken(Name left, Name right) noexcept {
    const uint64 leftKind = static_cast<uint64>(left.kind);
    const uint64 rightKind = static_cast<uint64>(right.kind);
    if (leftKind != rightKind) {
        return leftKind < rightKind ? -1 : 1;
    }

    const uint64 leftId = left.kind == NameKind::Normal ? left.id : 0;
    const uint64 rightId = right.kind == NameKind::Normal ? right.id : 0;
    if (leftId != rightId) {
        return leftId < rightId ? -1 : 1;
    }

    if (left.number != right.number) {
        return left.number < right.number ? -1 : 1;
    }

    return 0;
}

int32 CompareEdgeKey(const GraphEdge& left, const GraphEdge& right) noexcept {
    if (const int32 compared = CompareGuid(left.fromNode, right.fromNode); compared != 0) {
        return compared;
    }

    if (const int32 compared = CompareToken(left.fromPin, right.fromPin); compared != 0) {
        return compared;
    }

    if (const int32 compared = CompareGuid(left.toNode, right.toNode); compared != 0) {
        return compared;
    }

    return CompareToken(left.toPin, right.toPin);
}

bool SameBits(float left, float right) noexcept {
    return platform::BitCast<uint32>(left) == platform::BitCast<uint32>(right);
}

bool SameVec(Vec2 left, Vec2 right) noexcept {
    return SameBits(left.x, right.x) && SameBits(left.y, right.y);
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

bool SameLayout(const NodeLayout& left, const NodeLayout& right) noexcept {
    return SameVec(left.position, right.position) && SameVec(left.size, right.size) &&
           left.collapsed == right.collapsed && left.showAdvanced == right.showAdvanced &&
           left.bubble.View() == right.bubble.View() && left.bubbleVisible == right.bubbleVisible;
}

bool SameComment(const CommentBox& left, const CommentBox& right) noexcept {
    return SameVec(left.position, right.position) && SameVec(left.size, right.size) &&
           left.text.View() == right.text.View() && SameBits(left.color.r, right.color.r) &&
           SameBits(left.color.g, right.color.g) && SameBits(left.color.b, right.color.b) &&
           SameBits(left.color.a, right.color.a) && left.fontSize == right.fontSize;
}

bool SameBookmark(const Bookmark& left, const Bookmark& right) noexcept {
    return SameVec(left.viewCenter, right.viewCenter) && SameBits(left.zoom, right.zoom);
}

// 배열 원소를 가리키는 정렬 사본이다.
template <class T, class Less>
Result<Array<const T*>> SortedPointers(Allocator& allocator, const Array<T>& items,
                                       Less less) noexcept {
    Array<const T*> sorted(allocator);
    if (auto reserved = sorted.TryReserve(items.Size()); !reserved) {
        return Result<Array<const T*>>::Failure(reserved.Error());
    }

    for (const T& item : items) {
        if (auto added = sorted.TryPushBack(&item); !added) {
            return Result<Array<const T*>>::Failure(added.Error());
        }
    }

    algo::Sort(sorted.View(),
               [&less](const T* left, const T* right) noexcept { return less(*left, *right); });
    return Result<Array<const T*>>::Success(Move(sorted));
}

class Differ {
public:
    Differ(Allocator& allocator, NamePool& names, const GraphDiffOptions& options,
           Array<GraphDiffEntry>& out) noexcept
        : allocator_(allocator), names_(names), options_(options), out_(out) {
    }

    Result<void> Add(GraphDiffKind kind, Guid graph, Guid node = Guid{}, Guid other = Guid{},
                     Name pin = Name(), Name otherPin = Name(), uint32 number = 0) noexcept {
        GraphDiffEntry entry;
        entry.kind = kind;
        entry.graph = graph;
        entry.node = node;
        entry.other = other;
        entry.pin = pin;
        entry.otherPin = otherPin;
        entry.number = number;
        return out_.TryPushBack(entry);
    }

    Result<void> Documents(const GraphDocument& before, const GraphDocumentLayout* beforeLayout,
                           const GraphDocument& after,
                           const GraphDocumentLayout* afterLayout) noexcept {
        const bool domain = !(before.domain == after.domain);
        const bool main = !(before.mainGraph == after.mainGraph);
        const bool version = before.documentFormatVersion != after.documentFormatVersion;
        const bool header[] = {domain, main, version};
        for (uint32 index = 0; index < sizeof(header) / sizeof(header[0]); ++index) {
            if (header[index]) {
                if (auto added = Add(GraphDiffKind::DocumentHeader, Guid{}, Guid{}, Guid{}, Name(),
                                     Name(), index);
                    !added) {
                    return added;
                }
            }
        }

        const auto byId = [](const Graph& left, const Graph& right) noexcept {
            return CompareGuid(left.id, right.id) < 0;
        };
        auto left = SortedPointers(allocator_, before.graphs, byId);
        auto right = SortedPointers(allocator_, after.graphs, byId);
        if (!left || !right) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        usize i = 0;
        usize j = 0;
        while (i < left.Value().Size() || j < right.Value().Size()) {
            const Graph* b = i < left.Value().Size() ? left.Value()[i] : nullptr;
            const Graph* a = j < right.Value().Size() ? right.Value()[j] : nullptr;
            const int32 compared = b == nullptr ? 1 : a == nullptr ? -1 : CompareGuid(b->id, a->id);
            Result<void> step = Result<void>::Success();
            if (compared < 0) {
                step = Add(GraphDiffKind::GraphRemoved, b->id);
                ++i;
            } else if (compared > 0) {
                step = Add(GraphDiffKind::GraphAdded, a->id);
                ++j;
            } else {
                step =
                    Graphs(*b, FindLayout(beforeLayout, b->id), *a, FindLayout(afterLayout, a->id));
                ++i;
                ++j;
            }

            if (!step) {
                return step;
            }
        }

        return Result<void>::Success();
    }

private:
    const GraphLayout* FindLayout(const GraphDocumentLayout* layout, Guid graph) const noexcept {
        if (layout == nullptr || options_.semanticOnly) {
            return nullptr;
        }

        for (const GraphLayout& entry : layout->graphs) {
            if (entry.graph == graph) {
                return &entry;
            }
        }

        return nullptr;
    }

    static bool SameBoundaryList(const Array<GraphBoundaryPin>& left,
                                 const Array<GraphBoundaryPin>& right) noexcept {
        if (left.Size() != right.Size()) {
            return false;
        }

        for (usize index = 0; index < left.Size(); ++index) {
            const GraphBoundaryPin& a = left[index];
            const GraphBoundaryPin& b = right[index];
            if (!(a.key == b.key) || a.kind != b.kind || !(a.type == b.type) ||
                a.maxEdges != b.maxEdges) {
                return false;
            }
        }

        return true;
    }

    static bool SameBoundary(const Graph& before, const Graph& after) noexcept {
        return before.ownerNode == after.ownerNode &&
               SameBoundaryList(before.inputs, after.inputs) &&
               SameBoundaryList(before.outputs, after.outputs);
    }

    Result<void> Graphs(const Graph& before, const GraphLayout* beforeLayout, const Graph& after,
                        const GraphLayout* afterLayout) noexcept {
        if (before.role != after.role || before.domainRole != after.domainRole) {
            if (auto added = Add(GraphDiffKind::GraphRole, after.id); !added) {
                return added;
            }
        }

        if (!SameBoundary(before, after)) {
            if (auto added = Add(GraphDiffKind::GraphBoundary, after.id); !added) {
                return added;
            }
        }

        if (!options_.semanticOnly) {
            const StringView beforeTitle =
                beforeLayout != nullptr ? beforeLayout->title.View() : StringView();
            const StringView afterTitle =
                afterLayout != nullptr ? afterLayout->title.View() : StringView();
            if (!(beforeTitle == afterTitle)) {
                if (auto added = Add(GraphDiffKind::GraphTitle, after.id); !added) {
                    return added;
                }
            }
        }

        if (auto nodes = Nodes(before, after); !nodes) {
            return nodes;
        }

        if (auto edges = Edges(before, after); !edges) {
            return edges;
        }

        if (options_.semanticOnly) {
            return Result<void>::Success();
        }

        if (auto layouts = Layouts(after.id, beforeLayout, afterLayout); !layouts) {
            return layouts;
        }

        if (auto comments = Comments(after.id, beforeLayout, afterLayout); !comments) {
            return comments;
        }

        return Bookmarks(after.id, beforeLayout, afterLayout);
    }

    Result<void> Nodes(const Graph& before, const Graph& after) noexcept {
        const auto byId = [](const GraphNode& left, const GraphNode& right) noexcept {
            return CompareGuid(left.id, right.id) < 0;
        };
        auto left = SortedPointers(allocator_, before.nodes, byId);
        auto right = SortedPointers(allocator_, after.nodes, byId);
        if (!left || !right) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        usize i = 0;
        usize j = 0;
        while (i < left.Value().Size() || j < right.Value().Size()) {
            const GraphNode* b = i < left.Value().Size() ? left.Value()[i] : nullptr;
            const GraphNode* a = j < right.Value().Size() ? right.Value()[j] : nullptr;
            const int32 compared = b == nullptr ? 1 : a == nullptr ? -1 : CompareGuid(b->id, a->id);
            Result<void> step = Result<void>::Success();
            if (compared < 0) {
                step = Add(GraphDiffKind::NodeRemoved, after.id, b->id, Guid{}, b->type);
                ++i;
            } else if (compared > 0) {
                step = Add(GraphDiffKind::NodeAdded, after.id, a->id, Guid{}, a->type);
                ++j;
            } else {
                step = Node(after.id, *b, *a);
                ++i;
                ++j;
            }

            if (!step) {
                return step;
            }
        }

        return Result<void>::Success();
    }

    Result<void> Node(Guid graph, const GraphNode& before, const GraphNode& after) noexcept {
        const struct {
            bool differs;
            GraphDiffKind kind;
        } fields[] = {
            {!(before.type == after.type), GraphDiffKind::NodeType},
            {before.typeSchemaVersion != after.typeSchemaVersion, GraphDiffKind::NodeVersion},
            {!SameBytes(before.properties.bytes, after.properties.bytes),
             GraphDiffKind::NodeProperties},
            {before.enableState != after.enableState, GraphDiffKind::NodeEnableState},
        };
        for (const auto& field : fields) {
            if (field.differs) {
                if (auto added = Add(field.kind, graph, after.id); !added) {
                    return added;
                }
            }
        }

        // 입력 기본값: 한쪽에만 있거나 값이 다른 핀마다(목록은 작다).
        if (auto defaults = Keyed(
                graph, after.id, GraphDiffKind::InputDefault, before.inputDefaults,
                after.inputDefaults, [](const PinValueEntry& entry) noexcept { return entry.pin; },
                [](const PinValueEntry& left, const PinValueEntry& right) noexcept {
                    return SameBytes(left.value.bytes, right.value.bytes);
                });
            !defaults) {
            return defaults;
        }

        return Keyed(
            graph, after.id, GraphDiffKind::RetainedPin, before.retainedPins, after.retainedPins,
            [](const RetainedPin& pin) noexcept { return pin.key; },
            [](const RetainedPin& left, const RetainedPin& right) noexcept {
                return left.direction == right.direction && left.lastType == right.lastType &&
                       SameBytes(left.value.bytes, right.value.bytes);
            });
    }

    // 키가 Name인 작은 목록 둘을 맞춘다(뒤 목록의 키 순서 다음 앞 목록에만 있는 키).
    template <class T, class KeyOf, class Same>
    Result<void> Keyed(Guid graph, Guid node, GraphDiffKind kind, const Array<T>& before,
                       const Array<T>& after, KeyOf keyOf, Same same) noexcept {
        for (const T& item : after) {
            const T* match = nullptr;
            for (const T& candidate : before) {
                if (keyOf(candidate) == keyOf(item)) {
                    match = &candidate;
                    break;
                }
            }

            if (match == nullptr || !same(*match, item)) {
                if (auto added = Add(kind, graph, node, Guid{}, keyOf(item)); !added) {
                    return added;
                }
            }
        }

        for (const T& item : before) {
            bool present = false;
            for (const T& candidate : after) {
                present = present || keyOf(candidate) == keyOf(item);
            }

            if (!present) {
                if (auto added = Add(kind, graph, node, Guid{}, keyOf(item)); !added) {
                    return added;
                }
            }
        }

        return Result<void>::Success();
    }

    Result<void> Edges(const Graph& before, const Graph& after) noexcept {
        const usize first = out_.Size();
        if (auto matched = MatchEdges(before, after); !matched) {
            return matched;
        }

        return SortEdgeEntries(first);
    }

    // 간선 항목의 출력 순서는 문서의 정규 간선 순서(입력 노드, 입력 핀 원문, 출력 노드, 출력 핀
    // 원문)다 — 짝 맞추기의 토큰 순서는 pool의 발급 순서라 실행마다 다를 수 있어 출력에 쓰지
    // 않는다.
    Result<void> SortEdgeEntries(usize first) noexcept {
        if (out_.Size() - first < 2) {
            return Result<void>::Success();
        }

        bool failed = false;
        NamePool& names = names_;
        const auto compareName = [&names, &failed](Name left, Name right) noexcept {
            auto compared = CompareNames(names, left, right);
            failed = failed || !compared;
            return compared ? compared.Value() : 0;
        };

        algo::Sort(
            out_.View().Slice(first, out_.Size() - first),
            [&compareName](const GraphDiffEntry& left, const GraphDiffEntry& right) noexcept {
                if (const int32 c = CompareGuid(left.other, right.other); c != 0) {
                    return c < 0;
                }

                if (const int32 c = compareName(left.otherPin, right.otherPin); c != 0) {
                    return c < 0;
                }

                if (const int32 c = CompareGuid(left.node, right.node); c != 0) {
                    return c < 0;
                }

                if (const int32 c = compareName(left.pin, right.pin); c != 0) {
                    return c < 0;
                }

                return static_cast<uint32>(left.kind) < static_cast<uint32>(right.kind);
            });
        return failed ? Result<void>::Failure(ErrorCode::InvalidArgument) : Result<void>::Success();
    }

    Result<void> MatchEdges(const Graph& before, const Graph& after) noexcept {
        const auto byKey = [](const GraphEdge& left, const GraphEdge& right) noexcept {
            return CompareEdgeKey(left, right) < 0;
        };
        auto left = SortedPointers(allocator_, before.edges, byKey);
        auto right = SortedPointers(allocator_, after.edges, byKey);
        if (!left || !right) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        usize i = 0;
        usize j = 0;
        while (i < left.Value().Size() || j < right.Value().Size()) {
            const GraphEdge* b = i < left.Value().Size() ? left.Value()[i] : nullptr;
            const GraphEdge* a = j < right.Value().Size() ? right.Value()[j] : nullptr;
            const int32 compared = b == nullptr ? 1 : a == nullptr ? -1 : CompareEdgeKey(*b, *a);
            Result<void> step = Result<void>::Success();
            if (compared < 0) {
                step = Add(GraphDiffKind::EdgeRemoved, after.id, b->fromNode, b->toNode, b->fromPin,
                           b->toPin, b->order);
                ++i;
            } else if (compared > 0) {
                step = Add(GraphDiffKind::EdgeAdded, after.id, a->fromNode, a->toNode, a->fromPin,
                           a->toPin, a->order);
                ++j;
            } else {
                if (b->order != a->order) {
                    step = Add(GraphDiffKind::EdgeOrder, after.id, a->fromNode, a->toNode,
                               a->fromPin, a->toPin, a->order);
                }

                ++i;
                ++j;
            }

            if (!step) {
                return step;
            }
        }

        return Result<void>::Success();
    }

    Result<void> Layouts(Guid graph, const GraphLayout* before, const GraphLayout* after) noexcept {
        if (before == nullptr || after == nullptr) {
            return Result<void>::Success();
        }

        const auto byId = [](const NodeLayout& left, const NodeLayout& right) noexcept {
            return CompareGuid(left.node, right.node) < 0;
        };
        auto left = SortedPointers(allocator_, before->nodes, byId);
        auto right = SortedPointers(allocator_, after->nodes, byId);
        if (!left || !right) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        // 두 쪽에 다 있는 노드만 본다(추가·삭제는 노드 항목이 알린다).
        usize i = 0;
        usize j = 0;
        while (i < left.Value().Size() && j < right.Value().Size()) {
            const NodeLayout& b = *left.Value()[i];
            const NodeLayout& a = *right.Value()[j];
            const int32 compared = CompareGuid(b.node, a.node);
            if (compared < 0) {
                ++i;
                continue;
            }

            if (compared > 0) {
                ++j;
                continue;
            }

            if (!SameLayout(b, a)) {
                if (auto added = Add(GraphDiffKind::NodeLayout, graph, a.node); !added) {
                    return added;
                }
            }

            ++i;
            ++j;
        }

        return Result<void>::Success();
    }

    Result<void> Comments(Guid graph, const GraphLayout* before,
                          const GraphLayout* after) noexcept {
        const Array<CommentBox> none(allocator_);
        const Array<CommentBox>& left = before != nullptr ? before->comments : none;
        const Array<CommentBox>& right = after != nullptr ? after->comments : none;
        const auto byId = [](const CommentBox& l, const CommentBox& r) noexcept {
            return CompareGuid(l.id, r.id) < 0;
        };
        auto sortedLeft = SortedPointers(allocator_, left, byId);
        auto sortedRight = SortedPointers(allocator_, right, byId);
        if (!sortedLeft || !sortedRight) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        usize i = 0;
        usize j = 0;
        while (i < sortedLeft.Value().Size() || j < sortedRight.Value().Size()) {
            const CommentBox* b = i < sortedLeft.Value().Size() ? sortedLeft.Value()[i] : nullptr;
            const CommentBox* a = j < sortedRight.Value().Size() ? sortedRight.Value()[j] : nullptr;
            const int32 compared = b == nullptr ? 1 : a == nullptr ? -1 : CompareGuid(b->id, a->id);
            Result<void> step = Result<void>::Success();
            if (compared < 0) {
                step = Add(GraphDiffKind::CommentRemoved, graph, b->id);
                ++i;
            } else if (compared > 0) {
                step = Add(GraphDiffKind::CommentAdded, graph, a->id);
                ++j;
            } else {
                if (!SameComment(*b, *a)) {
                    step = Add(GraphDiffKind::CommentChanged, graph, a->id);
                }

                ++i;
                ++j;
            }

            if (!step) {
                return step;
            }
        }

        return Result<void>::Success();
    }

    Result<void> Bookmarks(Guid graph, const GraphLayout* before,
                           const GraphLayout* after) noexcept {
        const Array<Bookmark> none(allocator_);
        const Array<Bookmark>& left = before != nullptr ? before->bookmarks : none;
        const Array<Bookmark>& right = after != nullptr ? after->bookmarks : none;
        const auto byNumber = [](const Bookmark& l, const Bookmark& r) noexcept {
            return l.number < r.number;
        };
        auto sortedLeft = SortedPointers(allocator_, left, byNumber);
        auto sortedRight = SortedPointers(allocator_, right, byNumber);
        if (!sortedLeft || !sortedRight) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        usize i = 0;
        usize j = 0;
        while (i < sortedLeft.Value().Size() || j < sortedRight.Value().Size()) {
            const Bookmark* b = i < sortedLeft.Value().Size() ? sortedLeft.Value()[i] : nullptr;
            const Bookmark* a = j < sortedRight.Value().Size() ? sortedRight.Value()[j] : nullptr;
            const int32 compared = b == nullptr             ? 1
                                   : a == nullptr           ? -1
                                   : b->number == a->number ? 0
                                                            : (b->number < a->number ? -1 : 1);
            bool differs = compared != 0;
            uint32 number = 0;
            if (compared < 0) {
                number = b->number;
                ++i;
            } else if (compared > 0) {
                number = a->number;
                ++j;
            } else {
                differs = !SameBookmark(*b, *a);
                number = a->number;
                ++i;
                ++j;
            }

            if (differs) {
                if (auto added =
                        Add(GraphDiffKind::Bookmark, graph, Guid{}, Guid{}, Name(), Name(), number);
                    !added) {
                    return added;
                }
            }
        }

        return Result<void>::Success();
    }

    Allocator& allocator_;
    NamePool& names_;
    const GraphDiffOptions& options_;
    Array<GraphDiffEntry>& out_;
};

StringView KindText(GraphDiffKind kind) noexcept {
    switch (kind) {
    case GraphDiffKind::DocumentHeader:
        return "document";
    case GraphDiffKind::GraphAdded:
        return "graph-added";
    case GraphDiffKind::GraphRemoved:
        return "graph-removed";
    case GraphDiffKind::GraphRole:
        return "graph-role";
    case GraphDiffKind::GraphBoundary:
        return "graph-boundary";
    case GraphDiffKind::GraphTitle:
        return "graph-title";
    case GraphDiffKind::NodeAdded:
        return "node-added";
    case GraphDiffKind::NodeRemoved:
        return "node-removed";
    case GraphDiffKind::NodeType:
        return "node-type";
    case GraphDiffKind::NodeVersion:
        return "node-version";
    case GraphDiffKind::NodeProperties:
        return "node-properties";
    case GraphDiffKind::NodeEnableState:
        return "node-enable-state";
    case GraphDiffKind::InputDefault:
        return "input-default";
    case GraphDiffKind::RetainedPin:
        return "retained-pin";
    case GraphDiffKind::EdgeAdded:
        return "edge-added";
    case GraphDiffKind::EdgeRemoved:
        return "edge-removed";
    case GraphDiffKind::EdgeOrder:
        return "edge-order";
    case GraphDiffKind::NodeLayout:
        return "node-layout";
    case GraphDiffKind::CommentAdded:
        return "comment-added";
    case GraphDiffKind::CommentRemoved:
        return "comment-removed";
    case GraphDiffKind::CommentChanged:
        return "comment-changed";
    case GraphDiffKind::Bookmark:
        return "bookmark";
    }

    return "unknown";
}

constexpr StringView HeaderFields[] = {"domain", "main-graph", "format-version"};

Result<void> AppendName(NamePool& names, Name name, StringBuilderBase& out) noexcept {
    auto text = names.Acquire(name);
    if (!text) {
        return Result<void>::Failure(text.Error());
    }

    out << text.Value().View();
    return out.Status();
}

} // namespace

Result<void> ComputeGraphDiff(const GraphDocument& before, const GraphDocumentLayout* beforeLayout,
                              const GraphDocument& after, const GraphDocumentLayout* afterLayout,
                              NamePool& names, const GraphDiffOptions& options,
                              Array<GraphDiffEntry>& out) noexcept {
    out.Clear();
    Differ differ(out.GetAllocator(), names, options, out);
    return differ.Documents(before, beforeLayout, after, afterLayout);
}

Result<void> FormatGraphDiff(NamePool& names, Span<const GraphDiffEntry> entries,
                             StringBuilderBase& out) noexcept {
    for (const GraphDiffEntry& entry : entries) {
        out << KindText(entry.kind);
        if (entry.kind == GraphDiffKind::DocumentHeader) {
            out << " "
                << (entry.number < sizeof(HeaderFields) / sizeof(HeaderFields[0])
                        ? HeaderFields[entry.number]
                        : StringView("unknown"))
                << "\n";
            if (auto status = out.Status(); !status) {
                return status;
            }

            continue;
        }

        out << " " << FormatGuidText(entry.graph).View();
        switch (entry.kind) {
        case GraphDiffKind::EdgeAdded:
        case GraphDiffKind::EdgeRemoved:
        case GraphDiffKind::EdgeOrder:
            out << " " << FormatGuidText(entry.node).View() << ".";
            if (auto name = AppendName(names, entry.pin, out); !name) {
                return name;
            }

            out << " -> " << FormatGuidText(entry.other).View() << ".";
            if (auto name = AppendName(names, entry.otherPin, out); !name) {
                return name;
            }

            if (entry.kind == GraphDiffKind::EdgeOrder) {
                out << " order " << entry.number;
            }

            break;
        case GraphDiffKind::NodeAdded:
        case GraphDiffKind::NodeRemoved:
        case GraphDiffKind::InputDefault:
        case GraphDiffKind::RetainedPin:
            out << " " << FormatGuidText(entry.node).View() << " ";
            if (auto name = AppendName(names, entry.pin, out); !name) {
                return name;
            }

            break;
        case GraphDiffKind::Bookmark:
            out << " " << entry.number;
            break;
        case GraphDiffKind::GraphAdded:
        case GraphDiffKind::GraphRemoved:
        case GraphDiffKind::GraphRole:
        case GraphDiffKind::GraphBoundary:
        case GraphDiffKind::GraphTitle:
        case GraphDiffKind::DocumentHeader:
            break;
        default:
            out << " " << FormatGuidText(entry.node).View();
            break;
        }

        out << "\n";
        if (auto status = out.Status(); !status) {
            return status;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::nodegraph
