#include <NodeGraph/GraphClone.h>
#include <NodeGraph/GraphCommand.h>

// 그래프 공통 명령 목록이다(Docs/Design/NodeGraph.ko.md 8.1·8.6절).
namespace rocks::nodegraph {

Result<GraphCommand*> GraphCommandList::Push(GraphCommandKind kind, Guid graph) noexcept {
    auto added = commands_.TryEmplaceBack(commands_.GetAllocator());
    if (!added) {
        return Result<GraphCommand*>::Failure(added.Error());
    }

    added.Value()->kind = kind;
    added.Value()->graph = graph;
    return Result<GraphCommand*>::Success(added.Value());
}

namespace {

// 마지막 명령을 지우고 error를 돌려준다(인자 복사가 실패했을 때 목록을 그대로 둔다).
Result<void> CommandListDropLast(Array<GraphCommand>& commands, ErrorCode error) noexcept {
    static_cast<void>(commands.TryRemoveAt(commands.Size() - 1));
    return Result<void>::Failure(error);
}

} // namespace

Result<void> GraphCommandList::AddNode(Guid graph, const GraphNode& node,
                                       const NodeLayout& layout) noexcept {
    auto command = Push(GraphCommandKind::AddNode, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = node.id;
    if (auto cloned = CloneGraphNode(node, command.Value()->nodeData); !cloned) {
        return CommandListDropLast(commands_, cloned.Error());
    }

    if (auto cloned = CloneNodeLayout(layout, command.Value()->layout); !cloned) {
        return CommandListDropLast(commands_, cloned.Error());
    }

    command.Value()->layout.node = node.id;
    return Result<void>::Success();
}

Result<void> GraphCommandList::RemoveNodes(Guid graph, Span<const Guid> nodes) noexcept {
    auto command = Push(GraphCommandKind::RemoveNodes, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    if (auto copied = command.Value()->nodes.TryAppend(nodes); !copied) {
        return CommandListDropLast(commands_, copied.Error());
    }

    return Result<void>::Success();
}

Result<void> GraphCommandList::Connect(Guid graph, const GraphEdge& edge,
                                       bool requireMake) noexcept {
    auto command = Push(GraphCommandKind::Connect, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->edge = edge;
    command.Value()->edge.order = 0;
    command.Value()->requireMake = requireMake;
    return Result<void>::Success();
}

Result<void> GraphCommandList::Disconnect(Guid graph, const GraphEdge& edge) noexcept {
    auto command = Push(GraphCommandKind::Disconnect, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->edge = edge;
    return Result<void>::Success();
}

Result<void> GraphCommandList::ReorderEdges(Guid graph, Guid fromNode, Name fromPin,
                                            Span<const GraphEdge> order) noexcept {
    auto command = Push(GraphCommandKind::ReorderEdges, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = fromNode;
    command.Value()->pin = fromPin;
    if (auto copied = command.Value()->edges.TryAppend(order); !copied) {
        return CommandListDropLast(commands_, copied.Error());
    }

    return Result<void>::Success();
}

Result<void> GraphCommandList::SetNodeProperties(Guid graph, Guid node,
                                                 Span<const uint8> properties) noexcept {
    auto command = Push(GraphCommandKind::SetNodeProperties, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = node;
    if (auto copied = command.Value()->bytes.TryAppend(properties); !copied) {
        return CommandListDropLast(commands_, copied.Error());
    }

    return Result<void>::Success();
}

Result<void> GraphCommandList::SetInputDefault(Guid graph, Guid node, Name pin,
                                               Span<const uint8> value) noexcept {
    auto command = Push(GraphCommandKind::SetInputDefault, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = node;
    command.Value()->pin = pin;
    if (auto copied = command.Value()->bytes.TryAppend(value); !copied) {
        return CommandListDropLast(commands_, copied.Error());
    }

    return Result<void>::Success();
}

Result<void> GraphCommandList::ResetInputDefault(Guid graph, Guid node, Name pin) noexcept {
    auto command = Push(GraphCommandKind::ResetInputDefault, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = node;
    command.Value()->pin = pin;
    return Result<void>::Success();
}

Result<void> GraphCommandList::SetLayout(Guid graph, Guid node, const NodeLayout& layout) noexcept {
    auto command = Push(GraphCommandKind::SetLayout, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = node;
    if (auto cloned = CloneNodeLayout(layout, command.Value()->layout); !cloned) {
        return CommandListDropLast(commands_, cloned.Error());
    }

    command.Value()->layout.node = node;
    return Result<void>::Success();
}

Result<void> GraphCommandList::AddComment(Guid graph, const CommentBox& comment) noexcept {
    auto command = Push(GraphCommandKind::AddComment, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = comment.id;
    if (auto cloned = CloneCommentBox(comment, command.Value()->comment); !cloned) {
        return CommandListDropLast(commands_, cloned.Error());
    }

    return Result<void>::Success();
}

Result<void> GraphCommandList::RemoveComment(Guid graph, Guid comment) noexcept {
    auto command = Push(GraphCommandKind::RemoveComment, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = comment;
    return Result<void>::Success();
}

Result<void> GraphCommandList::SetComment(Guid graph, const CommentBox& comment) noexcept {
    auto command = Push(GraphCommandKind::SetComment, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = comment.id;
    if (auto cloned = CloneCommentBox(comment, command.Value()->comment); !cloned) {
        return CommandListDropLast(commands_, cloned.Error());
    }

    return Result<void>::Success();
}

Result<void> GraphCommandList::SetBookmark(Guid graph, const Bookmark& bookmark) noexcept {
    auto command = Push(GraphCommandKind::SetBookmark, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->bookmark = bookmark;
    command.Value()->hasBookmark = true;
    return Result<void>::Success();
}

Result<void> GraphCommandList::ClearBookmark(Guid graph, uint32 number) noexcept {
    auto command = Push(GraphCommandKind::SetBookmark, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->bookmark.number = number;
    command.Value()->hasBookmark = false;
    return Result<void>::Success();
}

Result<void> GraphCommandList::SetEnableState(Guid graph, Guid node,
                                              NodeEnableState state) noexcept {
    auto command = Push(GraphCommandKind::SetEnableState, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = node;
    command.Value()->enableState = state;
    return Result<void>::Success();
}

Result<void> GraphCommandList::AddGraph(Guid graph, GraphRole role, uint32 domainRole,
                                        StringView title) noexcept {
    auto command = Push(GraphCommandKind::AddGraph, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->role = role;
    command.Value()->domainRole = domainRole;
    if (auto copied = command.Value()->title.TryAssign(title); !copied) {
        return CommandListDropLast(commands_, copied.Error());
    }

    return Result<void>::Success();
}

Result<void> GraphCommandList::RemoveGraph(Guid graph) noexcept {
    auto command = Push(GraphCommandKind::RemoveGraph, graph);
    return command ? Result<void>::Success() : Result<void>::Failure(command.Error());
}

Result<void> GraphCommandList::RenameGraph(Guid graph, StringView title) noexcept {
    auto command = Push(GraphCommandKind::RenameGraph, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    if (auto copied = command.Value()->title.TryAssign(title); !copied) {
        return CommandListDropLast(commands_, copied.Error());
    }

    return Result<void>::Success();
}

Result<void> GraphCommandList::MoveNodesToGraph(Guid source, Span<const Guid> nodes,
                                                Guid target) noexcept {
    auto command = Push(GraphCommandKind::MoveNodesToGraph, source);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->target = target;
    if (auto copied = command.Value()->nodes.TryAppend(nodes); !copied) {
        return CommandListDropLast(commands_, copied.Error());
    }

    return Result<void>::Success();
}

Result<void> GraphCommandList::RemoveRetainedPin(Guid graph, Guid node, Name key) noexcept {
    auto command = Push(GraphCommandKind::RemoveRetainedPin, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = node;
    command.Value()->pin = key;
    return Result<void>::Success();
}

Result<void> GraphCommandList::InsertNode(Guid graph, const GraphNode& node,
                                          const NodeLayout& layout) noexcept {
    auto command = Push(GraphCommandKind::InsertNode, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->node = node.id;
    if (auto cloned = CloneGraphNode(node, command.Value()->nodeData); !cloned) {
        return CommandListDropLast(commands_, cloned.Error());
    }

    if (auto cloned = CloneNodeLayout(layout, command.Value()->layout); !cloned) {
        return CommandListDropLast(commands_, cloned.Error());
    }

    command.Value()->layout.node = node.id;
    return Result<void>::Success();
}

Result<void> GraphCommandList::InsertEdge(Guid graph, const GraphEdge& edge) noexcept {
    auto command = Push(GraphCommandKind::InsertEdge, graph);
    if (!command) {
        return Result<void>::Failure(command.Error());
    }

    command.Value()->edge = edge;
    return Result<void>::Success();
}

const char* GraphCommandReasonName(GraphCommandReason reason) noexcept {
    switch (reason) {
    case GraphCommandReason::None:
        return "None";
    case GraphCommandReason::GraphMissing:
        return "GraphMissing";
    case GraphCommandReason::GraphExists:
        return "GraphExists";
    case GraphCommandReason::GraphRole:
        return "GraphRole";
    case GraphCommandReason::MainGraph:
        return "MainGraph";
    case GraphCommandReason::SameGraph:
        return "SameGraph";
    case GraphCommandReason::IdZero:
        return "IdZero";
    case GraphCommandReason::NodeMissing:
        return "NodeMissing";
    case GraphCommandReason::NodeExists:
        return "NodeExists";
    case GraphCommandReason::TypeUnknown:
        return "TypeUnknown";
    case GraphCommandReason::TypeVersion:
        return "TypeVersion";
    case GraphCommandReason::PropertiesSchema:
        return "PropertiesSchema";
    case GraphCommandReason::NodeValues:
        return "NodeValues";
    case GraphCommandReason::SinglePerDocument:
        return "SinglePerDocument";
    case GraphCommandReason::CannotDelete:
        return "CannotDelete";
    case GraphCommandReason::PinMissing:
        return "PinMissing";
    case GraphCommandReason::PinDirection:
        return "PinDirection";
    case GraphCommandReason::RetainedPin:
        return "RetainedPin";
    case GraphCommandReason::EdgeExists:
        return "EdgeExists";
    case GraphCommandReason::EdgeMissing:
        return "EdgeMissing";
    case GraphCommandReason::EdgeLimit:
        return "EdgeLimit";
    case GraphCommandReason::ConnectDenied:
        return "ConnectDenied";
    case GraphCommandReason::ConversionInvalid:
        return "ConversionInvalid";
    case GraphCommandReason::NotOrdered:
        return "NotOrdered";
    case GraphCommandReason::OrderMismatch:
        return "OrderMismatch";
    case GraphCommandReason::PinsDropped:
        return "PinsDropped";
    case GraphCommandReason::ValueHidden:
        return "ValueHidden";
    case GraphCommandReason::ValueEmpty:
        return "ValueEmpty";
    case GraphCommandReason::DefaultMissing:
        return "DefaultMissing";
    case GraphCommandReason::CommentExists:
        return "CommentExists";
    case GraphCommandReason::CommentMissing:
        return "CommentMissing";
    case GraphCommandReason::EnableState:
        return "EnableState";
    case GraphCommandReason::CrossingEdge:
        return "CrossingEdge";
    case GraphCommandReason::RetainedMissing:
        return "RetainedMissing";
    case GraphCommandReason::DuplicateNode:
        return "DuplicateNode";
    case GraphCommandReason::RetainedShadowed:
        return "RetainedShadowed";
    case GraphCommandReason::RerouteCycle:
        return "RerouteCycle";
    case GraphCommandReason::ValueTooLarge:
        return "ValueTooLarge";
    case GraphCommandReason::GraphFull:
        return "GraphFull";
    case GraphCommandReason::InvariantBroken:
        return "InvariantBroken";
    case GraphCommandReason::NodeDataInvalid:
        return "NodeDataInvalid";
    }

    return "Unknown";
}

} // namespace rocks::nodegraph
