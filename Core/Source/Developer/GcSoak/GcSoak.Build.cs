using Rocks.Build;
using Rocks.Build.Rules;

// Long-running GC smoke simulator (tooling only, Docs/Design/GarbageCollectionScale.ko.md section 9). A deterministic
// seed mixes object creation, edge edits, roots, destruction, hold counts and incremental and stop-the-world collection
// per tick and compares the registry against an independent model. FoundationTests (ObjectRuntime tests) run it briefly;
// GcBench --soak runs it for long durations. It is not part of the game runtime.
public sealed class GcSoak : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "GcSoak";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ObjectRuntime"];
}
