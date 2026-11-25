using Rocks.Build;
using Rocks.Build.Rules;

// Mesh build tools (Docs/Design/MeshAnimationAssets section 5.2, stage MA2a): BuildStaticMeshLod turns one LOD source mesh (MeshSource)
// with its build settings (Mesh MeshBuildSettings) and the material slot names into an in-memory render result (welded render vertices with
// position, normal, tangent, binormal sign, UV channels and linear colors, uint32 indices, sections and bounds) plus warnings. Stage MA2b2 (section
// 5.5.2) adds PackStaticMeshLod (cooked LOD chunk layout) and StaticMeshCookProcessor (nugget bucket "mesh").
// BuildSkinnedMeshLod (section 6.5, stage MA5a) extends the same stages for skinned meshes: reduced bone influences with the
// LOD bone map, required bones, inverse bind matrices from the mesh BoneHierarchy (Animation) and sparse morph target deltas.
// Tangents use MikkTSpace (Engine/Source/ThirdParty/MikkTSpace); this module defines the hook functions of its Rocks.patch (deterministic
// cos/acos and allocator hooks), so MikkTSpace headers stay private to this module (adapter approach).
public sealed class MeshTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "MeshTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["MeshSource", "Mesh", "Animation", "AssetRuntime", "AssetCooker", "AssetNugget"];

    protected override string[] PrivateDependencies => ["DeterministicMath", "MikkTSpace"];
}
