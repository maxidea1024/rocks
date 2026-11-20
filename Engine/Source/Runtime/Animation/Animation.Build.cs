using Rocks.Build;
using Rocks.Build.Rules;

// Animation assets (Docs/Design/MeshAnimationAssets.ko.md sections 6 and 7, stage MA4a): the bone hierarchy value types BoneInfo and
// BoneHierarchy with ValidateBoneHierarchy and NormalizeBoneReferencePose (section 6.1), the Skeleton asset class with its retarget modes,
// virtual bones, sockets, curve info, slot groups, retarget sources and compatible skeletons plus ValidateSkeleton (section 6.2), and the
// pure merge function MergeBoneHierarchy that checks parent chains and returns the merged hierarchy and the mesh-to-skeleton bone map
// (SkeletonMismatch). Saving merged skeletons, import services and nugget invalidation come with stage MA4b; animation clips with MA6.
// Engine Runtime module: AssetRuntime for Asset and diagnostics, CoreTypeBindings for the Transform, Rotator<double> and Vec3d mirrors.
// Generated headers are <Animation/BoneHierarchy.generated.h> and <Animation/Skeleton.generated.h>, module registration is
// rocks::generated::Animation::RegisterModuleTypes (called by RegisterAnimationTypes), and the identity spec is Animation.rocksids.json.
public sealed class Animation : EngineModuleRules
{
    protected override string ModuleName => "Animation";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["AssetRuntime", "CoreTypeBindings"];

    protected override IReadOnlyList<string> Metadata => ["Animation/BoneHierarchy.h", "Animation/Skeleton.h"];
}
