using Rocks.Build;
using Rocks.Build.Rules;

// Device-bound render base (Docs/Design/RenderBase.ko.md, part of UE RenderCore): render lane, frame
// slots and frame packet transfer, render-transferable type registration, completion notices, and the
// RenderHandle reservation table. Sits on the Graphics device contract and does not know ObjectRuntime,
// World or Engine (the executable and WorldIntegration connect the game side).
// MetaSchema is used only for the Development check of Object reference fields in transferred types.
// TraceCapture is used by GPU instrumentation (RB7, GpuTrace). ModuleLoader gives the module lifetime object the host trace session
// service, so Startup registers the GPU trace channel and types (Docs/Design/ModuleLoader section 3.12).
public sealed class RenderBase : EngineModuleRules
{
    protected override string ModuleName => "RenderBase";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Graphics", "Task", "MetaSchema", "TraceCapture"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ModuleLoader"];
}
