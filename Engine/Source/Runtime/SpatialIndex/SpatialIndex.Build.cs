using Rocks.Build;
using Rocks.Build.Rules;

// General spatial query structures (Docs/Design/SpatialIndex.ko.md section 6 and Docs/Design/FlatBounds.ko.md): LooseOctree, a
// per-consumer AABB index with generation handles, in-place bounds updates, deferred maintenance and read scopes that enforce the
// mutation/read phase split, and FlatBoundsSet, the flat tile-relative SoA bounds set for per-view visibility bit sets. Engine
// layer module (Core stays a light foundation layer): depends on Core and Task only (Task for ParallelFor in view queries; no
// World, RHI or Object dependency), so DedicatedServer builds may link it.
public sealed class SpatialIndex : EngineModuleRules
{
    protected override string ModuleName => "SpatialIndex";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["Task"];
}
