using Rocks.Build;
using Rocks.Build.Rules;

// Editor-side asset database (Docs/Design/AssetSystem section 7.1, stage A2): project layout and logical asset paths
// (/Game/... mapped to Content/*.rasset, NFC and caseless keys), the project write lock (Intermediate/AssetDatabase.lock),
// the asset index over editor packages, editor package save and open, asset operations (move, duplicate, delete, aliases),
// the asset reference text resolver (ReflectionText) and the import framework. Developer module: editors and tools link
// it; games read cooked content only (AssetRuntime).
// HostConfig: project discovery (FindProjectFrom) is shared with the engine hosts.
public sealed class AssetDatabase : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AssetDatabase";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["AssetRuntime", "HostConfig", "ReflectionText"];

    // Class default revert (asset-set --reset=) calls ObjectSerialization directly.
    protected override string[] PrivateDependencies => ["ObjectSerialization"];
}
