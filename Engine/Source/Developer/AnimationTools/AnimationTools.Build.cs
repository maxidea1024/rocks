using Rocks.Build;
using Rocks.Build.Rules;

// Animation asset tools (Docs/Design/MeshAnimationAssets section 8 and 6.6, stage MA4b): the skeleton import service (ImportSkeleton —
// a reimport merges bones into the existing skeleton) and the bone merge apply helper (MergeBonesIntoSkeleton) that the skinned mesh
// import also uses. MA6b1 (section 7.6.1) adds the AnimationAsset Build dependency hook (skeleton) and the clip cook processor
// (AnimationClipCookProcessor, AssetCooker); clip import joins in MA6b2. Developer module: editors, cookers and tool programs link it.
public sealed class AnimationTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AnimationTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Animation: Skeleton, BoneHierarchy, the merge decision and clip cooking functions. AssetDatabase: AssetImportContext (the import
    // service runs inside an importer's Import call). AssetCooker: CookProcessor and the chained cooked bulk source.
    protected override string[] PublicDependencies => ["Animation", "AssetDatabase", "AssetCooker"];
}
