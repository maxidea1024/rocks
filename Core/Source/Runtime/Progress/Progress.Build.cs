using Rocks.Build;
using Rocks.Build.Rules;

// Task progress reporting and cancellation (Docs/Design/ProgressReporting): the progress hub, tasks, thread bindings, nested weighted scopes,
// counters, pull snapshots with a per-consumer event cursor and the UI pump hook. UE counterparts are FSlowTask, FScopedSlowTask,
// FFeedbackContext and FAsyncTaskNotification (UE keeps them in Core; Rocks keeps them in this Core layer module).
public sealed class ProgressRuntime : CoreModuleRules
{
    protected override string ModuleName => "Progress";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Localization: Text titles and messages in the public API. Task: ProgressParallelFor in the public header.
    protected override string[] PublicDependencies => ["Core", "Localization", "Task"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["TraceCapture"];
}
