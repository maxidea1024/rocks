#include <NodeGraph/GraphClone.h>

// 문서·배치 값의 깊은 복사다(Docs/Design/NodeGraph GD24·8.6절).
namespace rocks::nodegraph {

namespace {

Result<void> CloneText(const String& source, String& out) noexcept {
    return out.TryAssign(source.View());
}

template <class T, class CloneOne>
Result<void> CloneArray(const Array<T>& source, Array<T>& out, CloneOne&& cloneOne) noexcept {
    out.Clear();
    if (auto reserved = out.TryReserve(source.Size()); !reserved) {
        return reserved;
    }

    for (const T& item : source) {
        auto added = out.TryEmplaceBack(out.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        if (auto cloned = cloneOne(item, *added.Value()); !cloned) {
            return cloned;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> CloneBytes(const Array<uint8>& source, Array<uint8>& out) noexcept {
    out.Clear();
    return out.TryAppend(source.View());
}

Result<void> CloneGraphNode(const GraphNode& source, GraphNode& out) noexcept {
    out.id = source.id;
    out.type = source.type;
    out.typeSchemaVersion = source.typeSchemaVersion;
    out.enableState = source.enableState;
    if (auto cloned = CloneBytes(source.properties.bytes, out.properties.bytes); !cloned) {
        return cloned;
    }

    auto defaults = CloneArray(source.inputDefaults, out.inputDefaults,
                               [](const PinValueEntry& item, PinValueEntry& copy) noexcept {
                                   copy.pin = item.pin;
                                   return CloneBytes(item.value.bytes, copy.value.bytes);
                               });
    if (!defaults) {
        return defaults;
    }

    return CloneArray(source.retainedPins, out.retainedPins,
                      [](const RetainedPin& item, RetainedPin& copy) noexcept {
                          copy.key = item.key;
                          copy.direction = item.direction;
                          copy.lastType = item.lastType;
                          return CloneBytes(item.value.bytes, copy.value.bytes);
                      });
}

Result<void> CloneGraph(const Graph& source, Graph& out) noexcept {
    out.id = source.id;
    out.role = source.role;
    out.domainRole = source.domainRole;
    if (auto nodes = CloneArray(source.nodes, out.nodes,
                                [](const GraphNode& item, GraphNode& copy) noexcept {
                                    return CloneGraphNode(item, copy);
                                });
        !nodes) {
        return nodes;
    }

    out.edges.Clear();
    return out.edges.TryAppend(source.edges.View());
}

Result<void> CloneGraphDocument(const GraphDocument& source, GraphDocument& out) noexcept {
    out.domain = source.domain;
    out.documentFormatVersion = source.documentFormatVersion;
    out.mainGraph = source.mainGraph;
    return CloneArray(source.graphs, out.graphs, [](const Graph& item, Graph& copy) noexcept {
        return CloneGraph(item, copy);
    });
}

Result<void> CloneNodeLayout(const NodeLayout& source, NodeLayout& out) noexcept {
    out.node = source.node;
    out.position = source.position;
    out.size = source.size;
    out.collapsed = source.collapsed;
    out.showAdvanced = source.showAdvanced;
    out.bubbleVisible = source.bubbleVisible;
    return CloneText(source.bubble, out.bubble);
}

Result<void> CloneCommentBox(const CommentBox& source, CommentBox& out) noexcept {
    out.id = source.id;
    out.position = source.position;
    out.size = source.size;
    out.color = source.color;
    out.fontSize = source.fontSize;
    return CloneText(source.text, out.text);
}

Result<void> CloneGraphLayout(const GraphLayout& source, GraphLayout& out) noexcept {
    out.graph = source.graph;
    if (auto title = CloneText(source.title, out.title); !title) {
        return title;
    }

    if (auto nodes = CloneArray(source.nodes, out.nodes,
                                [](const NodeLayout& item, NodeLayout& copy) noexcept {
                                    return CloneNodeLayout(item, copy);
                                });
        !nodes) {
        return nodes;
    }

    if (auto comments = CloneArray(source.comments, out.comments,
                                   [](const CommentBox& item, CommentBox& copy) noexcept {
                                       return CloneCommentBox(item, copy);
                                   });
        !comments) {
        return comments;
    }

    out.bookmarks.Clear();
    return out.bookmarks.TryAppend(source.bookmarks.View());
}

Result<void> CloneGraphDocumentLayout(const GraphDocumentLayout& source,
                                      GraphDocumentLayout& out) noexcept {
    return CloneArray(source.graphs, out.graphs,
                      [](const GraphLayout& item, GraphLayout& copy) noexcept {
                          return CloneGraphLayout(item, copy);
                      });
}

} // namespace rocks::nodegraph
