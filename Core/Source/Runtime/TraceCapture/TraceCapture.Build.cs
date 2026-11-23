using Rocks.Build;
using Rocks.Build.Rules;

// Multithreaded trace capture (UE TraceLog counterpart, Docs/Design/TraceSystem.ko.md): sessions, channels, event types, thread blocks and the pool,
// definition buffers, storage, writers and output. The writer is a platform thread, not a Task (it must drain even after the general scheduler stops).
public sealed class TraceCapture : CoreModuleRules
{
    protected override string ModuleName => "TraceCapture";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["Compression"];
}
