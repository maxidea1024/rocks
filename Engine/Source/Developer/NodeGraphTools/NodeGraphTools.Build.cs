using Rocks.Build;
using Rocks.Build.Rules;

// Node graph tools (Docs/Design/NodeGraph.ko.md sections 9.2-9.4, 11.1): the graph text codec (fragment and document JSON, record values as
// reflection text), copy and paste command building, document diff and the graph-dump, graph-validate and graph-diff commandlets. Developer
// module: editor, cooker and tool programs link it, shipping runtime targets do not.
public sealed class NodeGraphTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "NodeGraphTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["NodeGraph"];

    // The standalone struct record codec and reflection text are used by the private sources only.
    protected override string[] PrivateDependencies => ["ObjectSerialization", "ReflectionText"];
}
