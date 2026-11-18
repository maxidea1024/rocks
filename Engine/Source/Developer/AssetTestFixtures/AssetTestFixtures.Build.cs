using Rocks.Build.Rules;

// Asset runtime reflection test fixture module (Docs/Design/AssetSystem.ko.md section 14). Owns test asset classes that derive from AssetRuntime's
// Asset (hard, soft and EditorOnly references, bulk data references, subobjects, an unsaveable class, and value-kind assets from simple scalars to
// game-data-like composites — Docs/Design/AssetSystem.ko.md section 14.1) and the identity spec
// (AssetTestFixtures.rocksids.json). Only the EngineTests executable depends on it. Header only.
public sealed class AssetTestFixtures : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AssetTestFixtures";

    // CoreTypeBindings: Transform, vector, rotator and InterpCurve mirrors. Localization: Text fields. Curves: TangentCurve fields.
    protected override string[] RequiredModules => ["AssetRuntime", "CoreTypeBindings", "Localization", "Curves"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata => ["AssetTestFixtures/TestAssets.h", "AssetTestFixtures/ValueAssets.h", "AssetTestFixtures/CombinationAssets.h"];
}
