using Rocks.Build;
using Rocks.Build.Rules;

// Engine connection of the asset runtime (Docs/Design/AssetSystem.ko.md section 11.8, stage A1e): EngineAssetsSubsystem owns the game's
// AssetManager, pumps it before the World tick, steps the asset object registry GC at the end of the frame, injects the engine frame
// predicate for LoadBlocking and shuts the manager down in the engine shutdown order. Settings are asset.* console variables registered
// by RegisterEngineAssetsSettings.
public sealed class EngineAssets : EngineModuleRules
{
    protected override string ModuleName => "EngineAssets";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Engine", "AssetRuntime"];
}
