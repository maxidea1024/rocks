using Rocks.Build;
using Rocks.Build.Rules;

// Render graph (Docs/Design/RenderBase.ko.md sections 7 and 8, UE RenderCore RDG counterpart). Collects passes with
// their resource access lists, culls unused passes, computes resource lifetimes and state transitions, executes
// passes in declaration order and keeps a frame-to-frame resource pool. Sits on RenderBase (render lane, frame
// arena, RenderHandle table and leases, GpuTrace) and the Graphics device contract (GX5 transitions).
public sealed class RenderGraph : EngineModuleRules
{
    protected override string ModuleName => "RenderGraph";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] RequiredModules => ["RenderBase", "Graphics", "TraceCapture"];
}
