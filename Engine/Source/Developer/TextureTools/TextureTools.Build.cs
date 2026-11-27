using Rocks.Build;
using Rocks.Build.Rules;

// Texture import tools (Docs/Design/AssetSystem section 12.12, stage A5h): the nine texture importers (2D, cube, array, cube array
// and volume, from several files or one file), their new-import defaults (UE 5.8 UTextureFactory and
// ApplyDefaultsForNewlyImportedTextures), the settings normalization hook (UE UTexture::ValidateSettingsAfterImportOrEdit), the
// texture import request (TextureImportSpec) and the asset-import argument translator (--cube, --array, --cube-array, --volume,
// --cube-layout, --volume-tile and the DDS dimension choice). It also holds the texture build (A5i) and the texture cook
// processor (A5j).
// Developer module: editor, cooker and tool programs link it, shipping runtime targets do not.
public sealed class TextureTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "TextureTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Texture for the asset classes and the import option types, AssetDatabase for the importer interface and the argument
    // translator types, AssetRuntime for the type hook table.
    // AssetCooker for the cook processor interface and the target profile LOD group table, AssetNugget for the texture nugget
    // records (section 12.14).
    protected override string[] PublicDependencies => ["Core", "Texture", "AssetRuntime", "AssetDatabase", "AssetCooker", "AssetNugget"];

    // ImageCodec decodes the source files (sections 12.9-12.11). DeterministicMath gives the build the same bytes on every host
    // (sRGB and gamma curves — section 12.13). AstcEncoder encodes ASTC for the Android cook (section 12.17.8).
    protected override string[] PrivateDependencies => ["ImageCodec", "DeterministicMath", "AstcEncoder"];

    // IspcTextureCompressor encodes BC6H and BC7 (section 12.17.6). ISPC sources are desktop-only (Docs/Design/BuildTargets section 3.12,
    // BD46), so Android targets build without it and report BC6H and BC7 encoding as Unsupported (format selection never picks them there).
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput module = base.Configure(context);
        return context.Platform == NativePlatform.Android
            ? module
            : module with { PrivateDependencies = [.. module.PrivateDependencies, "IspcTextureCompressor"] };
    }
}
