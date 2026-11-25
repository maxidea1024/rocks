#include <NodeGraph/GraphDiagnostic.h>
#include <NodeGraph/GraphValidation.h>

// 그래프 진단(Docs/Design/NodeGraph GD19)과 불변식 위반의 진단 변환이다.
namespace rocks::nodegraph {

namespace {

// 불변식 위반의 영어 설명이다(GI 번호 순).
StringView DiagnosticRuleMessage(GraphInvariant rule) noexcept {
    switch (rule) {
    case GraphInvariant::EdgeNodesInGraph:
        return "GI1: edge endpoint node is not in the graph";
    case GraphInvariant::EdgePinExists:
        return "GI2: edge pin is missing or has the wrong direction";
    case GraphInvariant::InputEdgeLimit:
        return "GI3: input pin has more edges than allowed";
    case GraphInvariant::DuplicateEdge:
        return "GI4: duplicate edge";
    case GraphInvariant::CanonicalOrder:
        return "GI5: element is out of canonical order";
    case GraphInvariant::LayoutMatchesDocument:
        return "GI6: layout does not match the document";
    case GraphInvariant::RetainedPinUnused:
        return "GI7: retained pin has neither edges nor a value";
    case GraphInvariant::RetainedPinShadowed:
        return "GI8: retained pin key is also a current pin";
    case GraphInvariant::DuplicateId:
        return "GI9: duplicate id in the document";
    case GraphInvariant::RerouteCycle:
        return "GI10: reroute chain forms a cycle";
    case GraphInvariant::EmptyIdentity:
        return "GI11: zero id or None name";
    case GraphInvariant::DocumentHeader:
        return "GI12: invalid document header or graph role";
    case GraphInvariant::NodeTypeData:
        return "GI13: node data does not match its type";
    case GraphInvariant::InputDefaultPin:
        return "GI14: input default is not on an editable input pin";
    case GraphInvariant::BoundaryInterface:
        return "GI15: invalid subgraph boundary interface or boundary node";
    case GraphInvariant::CollapsedOwner:
        return "GI16: subgraph and collapsed node do not match one to one";
    }

    return "unknown graph invariant";
}

} // namespace

Result<void> GraphDiagnosticList::Add(GraphDiagnosticSeverity severity, uint32 code, Guid asset,
                                      Guid graph, Guid node, Name pin,
                                      StringView message) noexcept {
    auto added = items.TryEmplaceBack(items.GetAllocator());
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    GraphDiagnostic& diagnostic = *added.Value();
    diagnostic.severity = severity;
    diagnostic.code = code;
    diagnostic.asset = asset;
    diagnostic.graph = graph;
    diagnostic.node = node;
    diagnostic.pin = pin;
    if (auto copied = diagnostic.message.TryAssign(message); !copied) {
        static_cast<void>(items.TryRemoveAt(items.Size() - 1));
        return copied;
    }

    return Result<void>::Success();
}

Result<void> AppendViolationDiagnostics(const GraphViolationList& violations, Guid asset,
                                        GraphDiagnosticList& out) noexcept {
    for (const GraphViolation& violation : violations.items) {
        auto added = out.Add(GraphDiagnosticSeverity::Error, static_cast<uint32>(violation.rule),
                             asset, violation.graph, violation.node, violation.pin,
                             DiagnosticRuleMessage(violation.rule));
        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::nodegraph
