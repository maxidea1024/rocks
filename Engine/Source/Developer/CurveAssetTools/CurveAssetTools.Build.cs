using Rocks.Build;
using Rocks.Build.Rules;

// Curve asset tools (Docs/Design/CurveAssets section 5, CR10 CA3 and CA4): the A2f importers of the curve assets (curve_table_csv and
// curve_table_json for CurveTableAsset, float_curve_csv, vector_curve_csv and color_curve_csv for the single curve assets — UE
// UCSVImportFactory, CreateTableFromCSVString, CreateTableFromJSONString and UCurveBase::CreateCurveFromCSVString) and later the curve
// table cook processor. Developer module: editors, cookers and tool programs link it.
public sealed class CurveAssetTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "CurveAssetTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // CurveAssets: the curve asset classes and the import options. AssetDatabase: AssetImporter and AssetImportContext.
    protected override string[] PublicDependencies => ["Core", "CurveAssets", "AssetDatabase"];
}
