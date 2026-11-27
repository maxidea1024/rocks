using Rocks.Build.Rules;

// ISPC Texture Compressor (GameTechDev/ISPCTextureCompressor commit 79ddbc90334f, MIT): BC6H and BC7 encoder of the texture cook
// (Docs/Design/AssetSystem section 12.17.6). kernel.ispc is compiled with the pinned ISPC (Docs/Design/BuildTargets section 3.12). The
// ThirdParty.json in the same folder is the single source of sources, licenses and file hashes.
public sealed class IspcTextureCompressor : ThirdPartyModuleRules
{
    protected override string ModuleName => "IspcTextureCompressor";
}
