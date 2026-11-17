using Rocks.Build.Rules;

// Trace capture control (Docs/Design/TraceSystem.ko.md sections 5 and 8, TR5): channel presets as config data (Engine branch section
// TracePresets), the --trace=<channel|preset>,... command line, the trace.* console commands and the session settings section Trace.
// TraceCapture knows only Core, so the connection to the config layer lives here. Reflection headers: TraceControl/TraceSettings.h,
// identity spec TraceControl.rocksids.json.
public sealed class TraceControl : CoreModuleRules
{
    protected override string ModuleName => "TraceControl";

    protected override string[] RequiredModules => ["Config", "TraceCapture"];

    protected override IReadOnlyList<string> Metadata => ["TraceControl/TraceSettings.h"];
}
