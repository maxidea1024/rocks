using Rocks.Build;
using Rocks.Build.Rules;

// Asset runtime (Docs/Design/AssetSystem sections 5, 6, 10 and 11, stage A1): package format v1 (PackageWriter and PackageReader),
// Package and Asset reflection classes, bulk data references, the cooked manifest reader and the runtime loader with AssetManager.
// Knows nothing about Engine, World or graphics (EngineAssets connects AssetManager to the engine frame). Package, Asset and
// BulkDataRef are reflection types; generated headers are <AssetRuntime/<Type>.generated.h>, module registration is
// rocks::generated::AssetRuntime::RegisterModuleTypes, declared in <AssetRuntime/AssetRuntime.module.generated.h> and defined in
// the module generated source that the build compiles into this module (called by RegisterAssetRuntimeTypes), and the identity
// spec is AssetRuntime.rocksids.json. Crypto: the signed cooked manifest envelope (Docs/Design/ContentSigning.ko.md, stage A1f).
public sealed class AssetRuntime : EngineModuleRules
{
    protected override string ModuleName => "AssetRuntime";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ObjectSerialization", "ObjectRuntime", "Serialization", "Task", "Compression", "Crypto"];
}
