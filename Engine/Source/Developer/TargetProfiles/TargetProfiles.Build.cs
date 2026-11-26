using Rocks.Build.Rules;

// Target profiles (Docs/Design/AssetSystem sections 10.2, 10.3 item 1 and 10.3.1 A4f1): the TargetProfile reflected struct (shader backends
// and formats, bulk settings, texture LOD groups), the engine and project JSON layering, the structural checks and the host platform profile.
// The cooker, the shader tool and the Development shader provider read profiles through this module without linking AssetCooker or the asset
// database. Developer module: editor and tools only.
public sealed class TargetProfiles : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "TargetProfiles";

    // AssetRuntime: diagnostic codes and the cooked manifest limits the shader checks share. ReflectionText: the JSON layering.
    // Texture: the LOD group enums. HostConfig: the host platform name.
    protected override string[] PublicDependencies => ["Core", "ObjectRuntime", "ReflectionText", "AssetRuntime", "Texture"];

    protected override string[] PrivateDependencies => ["HostConfig"];
}
