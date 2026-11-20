using Rocks.Build.Rules;

// Unreal exchange importer (Docs/Design/UnrealExchange.ko.md section 10.1): reads exchange bundles written by the RocksExport Unreal Engine
// plugin and imports them as Rocks assets. The plugin descriptor (UnrealExchange.rocksplugin) gives the module type (Developer: never in
// Release game targets) and folder. It is disabled by default; the command-line tool (Commandlet) and EngineTests enable it.
// The bundle reader, asset mapping and import connection come with stages UX1a and UX1b.
public sealed class UnrealExchange : PluginModuleRules
{
    protected override string ModuleName => "UnrealExchange";

    protected override string[] PublicDependencies => ["Core"];
}
