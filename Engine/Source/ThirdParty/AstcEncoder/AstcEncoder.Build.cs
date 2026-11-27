using Rocks.Build.Rules;

// ARM astc-encoder 5.1.0 (Apache-2.0): ASTC encoder of the Android texture cook (Docs/Design/AssetSystem section 12.17.8). The
// ThirdParty.json in the same folder is the single source of sources, licenses and file hashes.
public sealed class AstcEncoder : ThirdPartyModuleRules
{
    protected override string ModuleName => "AstcEncoder";
}
