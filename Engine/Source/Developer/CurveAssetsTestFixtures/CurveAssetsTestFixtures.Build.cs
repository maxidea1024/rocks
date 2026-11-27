using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Curve assets reflection test fixture module (Docs/Design/CurveAssets section 3.1, CA1). Owns the header of a reflected asset class that
// holds the three curve sources as direct fields (reference structs live only in owner objects), and the identity spec
// (CurveAssetsTestFixtures.rocksids.json). Only the EngineTests executable depends on it. Header only.
public sealed class CurveAssetsTestFixtures : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "CurveAssetsTestFixtures";

    // Test fixture: not started by phase (Docs/Design/ModuleLoader section 3.11). Tests register its types into their own registries;
    // the engine group of a test executable must not register them into every engine.
    protected override LoadingPhase LoadingPhase => LoadingPhase.None;

    protected override string[] PublicDependencies => ["CurveAssets"];

    protected override bool HasSources => false;
}
