using Rocks.Build;
using Rocks.Build.Rules;

// Mesh asset types shared by static and skinned meshes (Docs/Design/MeshAnimationAssets.ko.md section 4 and stage MA2a). For now only the
// build settings value struct MeshBuildSettings (section 4.1: implemented and preserved fields, UE FMeshBuildSettings) and the field
// identifiers that build diagnostics name (MeshBuildSettingField), plus SkinnedMeshBuildSettings (section 6.3, stage MA5a — the
// reflected derived struct adding the six skinned build fields of UE FSkeletalMeshBuildSettings), and the simple collision value
// types (section 4.6/4.8, stage MA3a — CollisionSphere/Box/Capsule/TaperedCapsule/Cylinder/Convex, CollisionEnabled, CollisionComplexity,
// SimpleCollision) with validation and convex bulk store/load. StaticMesh, material slots and section info come with stage MA2b.
// Engine Runtime module: ObjectRuntime for reflection, CoreTypeBindings for the Vec3d, Rotator<double> and Transform mirrors and
// AssetRuntime for BulkDataRef, Package and diagnostics. Generated headers are <Mesh/MeshBuildSettings.generated.h> and
// <Mesh/SimpleCollision.generated.h>, module registration is rocks::generated::Mesh::RegisterModuleTypes (called by RegisterMeshTypes),
// and the identity spec is Mesh.rocksids.json.
public sealed class Mesh : EngineModuleRules
{
    protected override string ModuleName => "Mesh";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ObjectRuntime", "CoreTypeBindings", "AssetRuntime"];
}
