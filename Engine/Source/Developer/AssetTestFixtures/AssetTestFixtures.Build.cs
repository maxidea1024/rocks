using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Asset runtime reflection test fixture module (Docs/Design/AssetSystem.ko.md section 14). Owns test asset classes that derive from AssetRuntime's
// Asset (hard, soft and EditorOnly references, bulk data references, subobjects, an unsaveable class, and value-kind assets from simple scalars to
// game-data-like composites — Docs/Design/AssetSystem.ko.md section 14.1, and a source mesh asset for the MeshSource tests —
// Docs/Design/MeshAnimationAssets.ko.md section 12) and the identity spec
// (AssetTestFixtures.rocksids.json). Only the EngineTests executable depends on it. Header only.
public sealed class AssetTestFixtures : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AssetTestFixtures";

    // Test fixture: not started by phase (Docs/Design/ModuleLoader.ko.md section 3.11). Tests register its types into their own registries;
    // the engine group of a test executable must not register them into every engine.
    protected override LoadingPhase LoadingPhase => LoadingPhase.None;

    // CoreTypeBindings: Transform, vector, rotator and InterpCurve mirrors. Localization: Text fields. Curves: TangentCurve fields.
    // MeshSource: MeshSourceData fields.
    protected override string[] PublicDependencies => ["AssetRuntime", "CoreTypeBindings", "Localization", "Curves", "MeshSource"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata => ["AssetTestFixtures/TestAssets.h", "AssetTestFixtures/ValueAssets.h", "AssetTestFixtures/CombinationAssets.h", "AssetTestFixtures/MeshAssets.h"];
}
