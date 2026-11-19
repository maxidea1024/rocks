using Rocks.Build.Rules;

// General spatial query structures (Docs/Design/SpatialIndex.ko.md section 6 and Docs/Design/FlatBounds.ko.md): LooseOctree, a
// per-consumer AABB index with generation handles, in-place bounds updates, deferred maintenance and read scopes that enforce the
// mutation/read phase split, and FlatBoundsSet, the flat tile-relative SoA bounds set for per-view visibility bit sets. Engine
// layer module (Core stays a light foundation layer): depends on Core and Task only (Task for ParallelFor in view queries; no
// World, RHI or Object dependency), so DedicatedServer builds may link it.
public sealed class SpatialIndex : EngineModuleRules
{
    protected override string ModuleName => "SpatialIndex";

    protected override string[] RequiredModules => ["Core", "Task"];
}
