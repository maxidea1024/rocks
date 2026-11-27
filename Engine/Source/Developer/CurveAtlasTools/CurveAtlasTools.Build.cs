using Rocks.Build;
using Rocks.Build.Rules;

// Color curve atlas tools (Docs/Design/CurveAssets 4.3, CR10 CA6): BakeColorCurveAtlas (UE UCurveLinearColorAtlas::UpdateTextures
// and UCurveLinearColor::PushToSourceData) and the ColorCurveAtlas type hooks: source rebuild (asset-set, asset-new and package
// open — UE PostEditChangeProperty and PostLoad), Build dependencies on the curves, and settings normalization. Cooking is the
// ordinary texture processor.
public sealed class CurveAtlasTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "CurveAtlasTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // CurveAtlas and CurveAssets: the atlas and its curves. AssetRuntime: the type hook table.
    protected override string[] PublicDependencies => ["Core", "CurveAtlas", "CurveAssets", "AssetRuntime"];

    // Texture: the source format and the Texture base fields the hooks write. ObjectRuntime: Registry access in the hooks.
    protected override string[] PrivateDependencies => ["Texture", "ObjectRuntime"];
}
