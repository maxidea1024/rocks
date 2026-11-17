using Rocks.Build.Rules;

// Device-bound render base (Docs/Design/RenderBase.ko.md, part of UE RenderCore): render lane, frame
// slots and frame packet transfer, render-transferable type registration, completion notices, and the
// RenderHandle reservation table. Sits on the Graphics device contract and does not know ObjectRuntime,
// World or Engine (the executable and WorldIntegration connect the game side).
// MetaSchema is used only for the Development check of Object reference fields in transferred types.
// TraceCapture is used by GPU instrumentation (RB7, GpuTrace).
public sealed class RenderBase : EngineModuleRules
{
    protected override string ModuleName => "RenderBase";

    protected override string[] RequiredModules => ["Graphics", "Task", "MetaSchema", "TraceCapture"];
}
