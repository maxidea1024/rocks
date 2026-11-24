using Rocks.Build;
using Rocks.Build.Rules;

// Texture import tools (Docs/Design/AssetSystem section 12.12, stage A5h): the nine texture importers (2D, cube, array, cube array
// and volume, from several files or one file), their new-import defaults (UE 5.8 UTextureFactory and
// ApplyDefaultsForNewlyImportedTextures), the settings normalization hook (UE UTexture::ValidateSettingsAfterImportOrEdit), the
// texture import request (TextureImportSpec) and the asset-import argument translator (--cube, --array, --cube-array, --volume,
// --cube-layout, --volume-tile and the DDS dimension choice). The texture build (A5i) and cooking (A5j) are added here later.
// Developer module: editor, cooker and tool programs link it, shipping runtime targets do not.
public sealed class TextureTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "TextureTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Texture for the asset classes and the import option types, AssetDatabase for the importer interface and the argument
    // translator types, AssetRuntime for the type hook table.
    protected override string[] PublicDependencies => ["Core", "Texture", "AssetRuntime", "AssetDatabase"];

    // ImageCodec decodes the source files (sections 12.9-12.11). DeterministicMath gives the build the same bytes on every host
    // (sRGB and gamma curves — section 12.13).
    protected override string[] PrivateDependencies => ["ImageCodec", "DeterministicMath"];
}
