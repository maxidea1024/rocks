using Rocks.Build.Rules;

// Long-running GC smoke simulator (tooling only, Docs/Design/GarbageCollectionScale.ko.md section 9). A deterministic
// seed mixes object creation, edge edits, roots, destruction, hold counts and incremental and stop-the-world collection
// per tick and compares the registry against an independent model. FoundationTests (ObjectRuntime tests) run it briefly;
// GcBench --soak runs it for long durations. It is not part of the game runtime.
public sealed class GcSoak : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "GcSoak";

    protected override string[] RequiredModules => ["ObjectRuntime"];
}
