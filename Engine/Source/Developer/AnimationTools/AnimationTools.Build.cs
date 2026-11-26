using Rocks.Build;
using Rocks.Build.Rules;

// Animation asset tools (Docs/Design/MeshAnimationAssets section 8 and 6.6, stage MA4b): the skeleton import service (ImportSkeleton —
// a reimport merges bones into the existing skeleton) and the bone merge apply helper (MergeBonesIntoSkeleton) that the skinned mesh
// import also uses. Clip cooking and clip import join in MA6b. Developer module: editors, cookers and tool programs link it.
public sealed class AnimationTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AnimationTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Animation: Skeleton, BoneHierarchy and the merge decision. AssetDatabase: AssetImportContext (the import service runs inside an
    // importer's Import call).
    protected override string[] PublicDependencies => ["Animation", "AssetDatabase"];
}
