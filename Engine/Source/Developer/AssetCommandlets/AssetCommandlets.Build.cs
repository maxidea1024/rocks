using Rocks.Build;
using Rocks.Build.Rules;

// Asset commands of the command-line tool (Docs/Design/AssetSystem section 13, section 7.1 item 18): asset-list, asset-info,
// asset-new, asset-set, asset-move, asset-duplicate, asset-delete, asset-fix-duplicate-id, asset-index-*, asset-alias-*, asset-orphans-*,
// the import commands and asset-cook. Developer module: the RocksCommandlet program registers these commands, and EngineTests calls the command
// functions in process with test importers (the product executable registers real importers only).
// ModuleLoader: the commands start the asset type module. Task: the index scan worker scheduler. AssetCooker: asset-cook (cook flow,
// settings and the nugget cache).
public sealed class AssetCommandlets : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AssetCommandlets";

    protected override string[] PublicDependencies => ["AssetCooker", "AssetDatabase", "HostConfig", "ModuleLoader", "Task"];
}
