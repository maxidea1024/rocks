using Rocks.Build.Rules;

// General region query structures (Docs/Design/SpatialIndex.ko.md section 6): LooseOctree, a per-consumer AABB index with generation
// handles, in-place bounds updates, deferred maintenance and read scopes that enforce the mutation/read phase split. Core layer
// module: depends on Core only (no World, RHI or Object dependency), so DedicatedServer builds may link it.
public sealed class SpatialIndex : CoreModuleRules
{
    protected override string ModuleName => "SpatialIndex";

    protected override string[] RequiredModules => ["Core"];
}
