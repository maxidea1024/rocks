using Rocks.Build;
using Rocks.Build.Rules;

// Color curve atlas (Docs/Design/CurveAssets chapter 4, CR10 CA6): ColorCurveAtlas, a Texture2D whose source is baked from
// ColorCurveAsset rows. Mirrors UE 5.8 UCurveLinearColorAtlas with Rocks names. Kept apart from CurveAssets so curve consumers do
// not pull in the texture module (CAD2). The bake and the type hooks live in the Developer module CurveAtlasTools. Generated
// headers are <CurveAtlas/<Header>.generated.h>, module registration is rocks::generated::CurveAtlas::RegisterModuleTypes (called
// by RegisterCurveAtlasTypes), and the identity spec is CurveAtlas.rocksids.json.
public sealed class CurveAtlas : EngineModuleRules
{
    protected override string ModuleName => "CurveAtlas";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // CurveAssets for ColorCurveAsset, Texture for the Texture2D base, ObjectRuntime for the object references.
    protected override string[] PublicDependencies => ["Core", "ObjectRuntime", "AssetRuntime", "CurveAssets", "Texture"];
}
