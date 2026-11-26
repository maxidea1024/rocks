using Rocks.Build.Rules;

// Unreal exchange importer (Docs/Design/UnrealExchange section 10.1): reads exchange bundles written by the RocksExport Unreal Engine
// plugin and imports them as Rocks assets. The plugin descriptor (UnrealExchange.rocksplugin) gives the module type (Developer: never in
// Release game targets) and folder. It is disabled by default; the command-line tool (Commandlet) and EngineTests enable it.
// Stage UX1a adds the bundle reader, path mapping and import order (section 10.4); stage UX1c adds the import plan, the existing asset
// lookup, the kind importer table and the asset-import-unreal command (section 10.5). Stage UX2b adds the common kind import frame
// (section 10.6: the reflected import options struct, asset document re-reading, settings translation) and the texture import (section
// 4.6). Plugin modules may depend on engine modules, never the reverse (Docs/Design/BuildTargets BD39). Generated headers are
// <UnrealExchange/<Header>.generated.h>, module registration is rocks::generated::UnrealExchange::RegisterModuleTypes (called by
// RegisterUnrealExchangeTypes), and the identity spec is UnrealExchange.rocksids.json.
public sealed class UnrealExchange : PluginModuleRules
{
    protected override string ModuleName => "UnrealExchange";

    // AssetRuntime: AssetDiagnosticCode. MeshSource: variable blob layout limits (MeshMaxSourceInfluences, MaxNameFieldBytes).
    // AssetDatabase: the AssetIndex tag lookup and AssetService (section 10.5). AssetCommandlets: the asset command session.
    // ObjectRuntime: the reflected import options struct. ReflectionText: ImportJson for the translated settings (section 10.6 item 8).
    // Texture: the texture output classes and TextureSource (section 4.6).
    protected override string[] PublicDependencies =>
        ["Core", "ObjectRuntime", "ReflectionText", "AssetRuntime", "MeshSource", "AssetDatabase", "AssetCommandlets", "Texture"];

    // ImageCodec: the source size limits shared with file imports (ImageDecodeLimits defaults — section 4.6 item 2).
    protected override string[] PrivateDependencies => ["ImageCodec"];
}
