using Rocks.Build;
using Rocks.Build.Rules;

// Device-bound render base (Docs/Design/RenderBase, part of UE RenderCore): render lane, frame
// slots and frame packet transfer, completion notices, and the
// RenderHandle reservation table. Sits on the Graphics device contract and does not know ObjectRuntime,
// World or Engine (the executable and WorldIntegration connect the game side).
// MetaSchema holds the lane transfer registration and its Development schema check (Meta/LaneTransfer.h, section 5.2) that
// the render lane tasks require of the values they take.
// TraceCapture is used by GPU instrumentation (RB7, GpuTrace). ModuleLoader gives the module lifetime object the host trace session
// service, so Startup registers the GPU trace channel and types (Docs/Design/ModuleLoader section 3.12).
// AssetRuntime: the cooked manifest v2 shader library source (CookedManifestShaderLibraries — artifact paths and file checks, Docs/Design/ShaderSystem
// section 16.1 SSD47).
public sealed class RenderBase : EngineModuleRules
{
    protected override string ModuleName => "RenderBase";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Graphics", "ShaderCore", "Task", "MetaSchema", "TraceCapture"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ModuleLoader", "AssetRuntime"];
}
