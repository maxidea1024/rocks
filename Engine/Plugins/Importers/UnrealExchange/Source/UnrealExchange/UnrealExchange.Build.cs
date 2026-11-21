using Rocks.Build.Rules;

// Unreal exchange importer (Docs/Design/UnrealExchange.ko.md section 10.1): reads exchange bundles written by the RocksExport Unreal Engine
// plugin and imports them as Rocks assets. The plugin descriptor (UnrealExchange.rocksplugin) gives the module type (Developer: never in
// Release game targets) and folder. It is disabled by default; the command-line tool (Commandlet) and EngineTests enable it.
// Stage UX1a adds the bundle reader, path mapping and import order (section 10.4); asset mapping and the import connection come with
// stage UX1b and later. Plugin modules may depend on engine modules, never the reverse (Docs/Design/BuildTargets.ko.md BD39).
public sealed class UnrealExchange : PluginModuleRules
{
    protected override string ModuleName => "UnrealExchange";

    // AssetRuntime: AssetDiagnosticCode. MeshSource: variable blob layout limits (MeshMaxSourceInfluences, MaxNameFieldBytes).
    protected override string[] PublicDependencies => ["Core", "AssetRuntime", "MeshSource"];
}
