using Rocks.Build;
using Rocks.Build.Rules;

// Source mesh data (Docs/Design/MeshAnimationAssets.ko.md section 3 and stage MA1): the persistent form MeshSourceData (reflection struct with
// element counts, a topology bulk and one bulk per attribute), the in-memory editing form MeshSource with Load and Store, attribute name
// constants, ValidateMeshSource and SanitizeMeshSource, and the MeshTopologyIndex adjacency queries used by mesh builds. Engine Runtime module
// that depends on AssetRuntime only (BulkDataRef, Package, MemoryBulkSource and asset diagnostics; Core, ObjectRuntime and Serialization come
// through it). Reflection types are MeshElement, MeshValueType, MeshAttributeRecord and MeshSourceData; generated headers are
// <MeshSource/MeshSourceData.generated.h>, module registration is rocks::generated::MeshSource::RegisterModuleTypes (called by
// RegisterMeshSourceTypes), and the identity spec is MeshSource.rocksids.json.
public sealed class MeshSource : EngineModuleRules
{
    protected override string ModuleName => "MeshSource";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["AssetRuntime"];

    protected override IReadOnlyList<string> Metadata => ["MeshSource/MeshSourceData.h"];
}
