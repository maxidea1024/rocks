using Rocks.Build.Rules;

// Host-side config boot (Docs/Design/ConfigSystem.ko.md section 5.3): the one config context of a process (game, editor, Sandbox,
// RocksCommandlet). It owns the type registry, module registration and object registry the context needs, registers the host-level
// settings (log.* from LogHost, trace.* and TracePresets from TraceControl), and runs the boot order: engine and project files, an
// optional extra file, --set assignments, CompleteStartup. The engine borrows the context (EngineConfig::settings).
public sealed class HostConfig : CoreModuleRules
{
    protected override string ModuleName => "HostConfig";

    // ModuleLoader: settings registration walks the module table (Docs/Design/ModuleLoader.ko.md section 3.10).
    protected override string[] RequiredModules => ["LogHost", "TraceControl", "Config", "ModuleLoader"];
}
