using Rocks.Build;
using Rocks.Build.Rules;

// Curve asset layer (Docs/Design/CurveAssets, CR10): the curve asset classes over the Curves value types (FloatCurveAsset,
// VectorCurveAsset, ColorCurveAsset with the UE color adjustments, their abstract base CurveAsset), the inline-or-asset curve
// sources (FloatCurveSource, VectorCurveSource, ColorCurveSource), their flat value forms (VectorCurveValue, ColorCurveValue), the
// curve table (CurveTableAsset) and its row reference (CurveTableRowRef). Mirrors UE 5.8 UCurveBase, UCurveFloat, UCurveVector,
// UCurveLinearColor, UCurveTable, FRuntimeFloatCurve and FCurveTableRowHandle with Rocks names. Generated headers are
// <CurveAssets/<Header>.generated.h>, module registration is rocks::generated::CurveAssets::RegisterModuleTypes (called by
// RegisterCurveAssetsTypes), and the identity spec is CurveAssets.rocksids.json.
public sealed class CurveAssets : EngineModuleRules
{
    protected override string ModuleName => "CurveAssets";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Curves for the curve value types, ObjectRuntime for the reflected classes and object references, AssetRuntime for the Asset base.
    protected override string[] PublicDependencies => ["Core", "Curves", "ObjectRuntime", "AssetRuntime"];
}
