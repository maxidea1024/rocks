using Rocks.Build;
using Rocks.Build.Rules;

// Trace capture control (Docs/Design/TraceSystem.ko.md sections 5 and 8, TR5): channel presets as config data (Engine branch section
// TracePresets), the --trace=<channel|preset>,... command line, the trace.* console commands and the trace.* session CVars.
// TraceCapture knows only Core, so the connection to the config layer lives here. Reflection headers: TraceControl/TraceSettings.h,
// identity spec TraceControl.rocksids.json.
public sealed class TraceControl : CoreModuleRules
{
    protected override string ModuleName => "TraceControl";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Config", "TraceCapture"];
}
