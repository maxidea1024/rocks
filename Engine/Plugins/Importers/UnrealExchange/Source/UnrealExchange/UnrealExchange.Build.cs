using Rocks.Build.Rules;

// Unreal exchange importer (Docs/Design/UnrealExchange section 10.1): reads exchange bundles written by the RocksExport Unreal Engine
// plugin and imports them as Rocks assets. The plugin descriptor (UnrealExchange.rocksplugin) gives the module type (Developer: never in
// Release game targets) and folder. It is disabled by default; the command-line tool (Commandlet) and EngineTests enable it.
// Stage UX1a adds the bundle reader, path mapping and import order (section 10.4); stage UX1c adds the import plan, the existing asset
// lookup, the kind importer table and the asset-import-unreal command (section 10.5). Stage UX2b adds the common kind import frame
// (section 10.6: the reflected import options struct, asset document re-reading, settings translation) and the texture import (section
// 4.6). Stage UX3b adds the static mesh record loading, the conversion to the static mesh import spec and the unreal_static_mesh
// importer (section 5.5); stage UX7b adds the skeleton and skeletal mesh conversion and importers (section 6.4); stage UX8b adds the animation clip conversion and importer (section 7.2). Plugin modules may depend on engine modules, never the reverse (Docs/Design/BuildTargets BD39). Generated headers are
// <UnrealExchange/<Header>.generated.h>, module registration is rocks::generated::UnrealExchange::RegisterModuleTypes (called by
// RegisterUnrealExchangeTypes), and the identity spec is UnrealExchange.rocksids.json.
public sealed class UnrealExchange : PluginModuleRules
{
    protected override string ModuleName => "UnrealExchange";

    // AssetRuntime: AssetDiagnosticCode. MeshSource: variable blob layout limits (MeshMaxSourceInfluences, MaxNameFieldBytes).
    // AssetDatabase: the AssetIndex tag lookup and AssetService (section 10.5). AssetCommandlets: the asset command session.
    // ObjectRuntime: the reflected import options struct. ReflectionText: ImportJson for the translated settings (section 10.6 item 8).
    // Texture: the texture output classes and TextureSource (section 4.6). Mesh, MeshTools: the static mesh settings structs, collision
    // and the static mesh import service spec (section 5.5). Animation, AnimationTools: the skeleton and clip types and the skeleton and clip
    // import service specs (sections 6.4, 7.2). Curves: TangentCurve for clip curves (section 7.2).
    protected override string[] PublicDependencies =>
        ["Core", "ObjectRuntime", "ReflectionText", "AssetRuntime", "MeshSource", "AssetDatabase", "AssetCommandlets", "Texture", "Mesh",
         "MeshTools", "Animation", "AnimationTools", "Curves"];

    // ImageCodec: the source size limits shared with file imports (ImageDecodeLimits defaults — section 4.6 item 2).
    protected override string[] PrivateDependencies => ["ImageCodec"];
}
