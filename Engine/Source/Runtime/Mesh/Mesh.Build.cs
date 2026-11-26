using Rocks.Build;
using Rocks.Build.Rules;

// Mesh asset types shared by static and skinned meshes (Docs/Design/MeshAnimationAssets section 4 and stage MA2a). For now only the
// build settings value struct MeshBuildSettings (section 4.1: implemented and preserved fields, UE FMeshBuildSettings) and the field
// identifiers that build diagnostics name (MeshBuildSettingField), plus SkinnedMeshBuildSettings (section 6.3, stage MA5a — the
// reflected derived struct adding the six skinned build fields of UE FSkeletalMeshBuildSettings), and the simple collision value
// types (section 4.6/4.8, stage MA3a — CollisionSphere/Box/Capsule/TaperedCapsule/Cylinder/Convex, CollisionEnabled, CollisionComplexity,
// SimpleCollision) with validation and convex bulk store/load; CollisionEnabled itself comes from the Collision module
// (Docs/Design/CollisionData.ko.md). Stage MA2b1 (section 5.5.1) adds the StaticMesh asset class with its parts under
// Mesh/Common (material slots, section info, sockets, reduction and virtual geometry settings, cooked render streams and sections) and
// Mesh/StaticMesh (source LOD, cooked render LOD and render data, load validation), depending on MeshSource, Material and GraphicsTypes.
// Stage MA5b1 (section 6.7.1) adds Mesh/SkeletalMesh (SkinnedAsset, SkeletalMesh, source LOD, skin weight profile info, cooked skinned
// render LOD with morph targets and profiles, render data, load validation), depending on Animation for Skeleton and BoneHierarchy.
// Engine Runtime module: ObjectRuntime for reflection, CoreTypeBindings for the Vec3d, Rotator<double> and Transform mirrors and
// AssetRuntime for BulkDataRef, Package and diagnostics. Generated headers are <Mesh/MeshBuildSettings.generated.h> and
// <Mesh/SimpleCollision.generated.h>, module registration is rocks::generated::Mesh::RegisterModuleTypes (called by RegisterMeshTypes),
// and the identity spec is Mesh.rocksids.json.
public sealed class Mesh : EngineModuleRules
{
    protected override string ModuleName => "Mesh";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ObjectRuntime", "CoreTypeBindings", "AssetRuntime", "Collision", "MeshSource", "Material", "GraphicsTypes", "Animation"];
}
