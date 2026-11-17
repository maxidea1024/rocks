using Rocks.Build.Rules;

// Trace analysis (UE TraceAnalysis and TraceServices counterpart, Docs/Design/TraceSystem.ko.md sections 3 and 10, TR4): stream reading, thread merge,
// span, task, counter and marker models, and a deterministic JSON report. TraceCapture's TraceFormat.h is the single source of the format. Only tools
// (Commandlet) and tests link it; it does not go into game executables (Developer module).
public sealed class TraceAnalysis : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "TraceAnalysis";

    protected override string[] RequiredModules => ["Core", "Compression", "TraceCapture"];
}
