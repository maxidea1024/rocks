using Rocks.Build.Rules;

// Task DAG and worker scheduler (UE TaskGraph counterpart). Records task lifetime events to TraceCapture (channel task,
// Docs/Design/TraceSystem.ko.md section 7). TraceCapture does not depend on Task.
public sealed class TaskRuntime : CoreModuleRules
{
    protected override string ModuleName => "Task";

    protected override string[] RequiredModules => ["Core", "TraceCapture"];
}
