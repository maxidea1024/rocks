using Rocks.Build.Rules;

// Device-bound render foundation (Docs/Design/RenderBase.ko.md, partial UE RenderCore counterpart): render lane and frame
// slots, frame packet handoff, render-transferable type registration, completion notification and the RenderHandle reservation table. Sits on the
// Graphics device contract and knows nothing about ObjectRuntime, World or Engine (executables and WorldIntegration wire the game side).
// MetaSchema is used only to check Object reference fields of transferred types (Development).
public sealed class RenderBase : EngineModuleRules
{
    protected override string ModuleName => "RenderBase";

    protected override string[] RequiredModules => ["Graphics", "Task", "MetaSchema"];
}
