using Rocks.Build;
using Rocks.Build.Rules;

// Shared node graph model (Docs/Design/NodeGraph sections 6 to 10, stage NG0): the reflected document and layout types (GraphDocument,
// GraphDocumentLayout), node type and pin descriptors, the NG0 part of GraphSchema, the NodeTypeRegistry with the common Graph.Reroute type
// and pin list computation, the canonical Name order, canonicalization and the full invariant check (GI1 to GI14), and the semantic bytes and
// hash (section 8.4). Engine Runtime module: ObjectRuntime for the generated reflection codec and CoreTypeBindings for the Vec2 and
// LinearColor mirrors. The module never creates or holds objects, ObjectHandle, TypeLease or InstancedStruct (principle 1, policy
// nodegraph-boundary); node properties stay Record bytes (GD23). Generated header <NodeGraph/GraphDocument.generated.h>, module registration
// rocks::generated::NodeGraph::RegisterModuleTypes (called by RegisterNodeGraphTypes), identity spec NodeGraph.rocksids.json.
public sealed class NodeGraph : EngineModuleRules
{
    protected override string ModuleName => "NodeGraph";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ObjectRuntime", "CoreTypeBindings"];
}
