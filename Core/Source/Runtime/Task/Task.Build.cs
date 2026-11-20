using Rocks.Build;
using Rocks.Build.Rules;

// Task DAG and worker scheduler (UE TaskGraph counterpart). Records task lifetime events to TraceCapture (channel task,
// Docs/Design/TraceSystem.ko.md section 7). TraceCapture does not depend on Task.
public sealed class TaskRuntime : CoreModuleRules
{
    protected override string ModuleName => "Task";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "TraceCapture"];
}
