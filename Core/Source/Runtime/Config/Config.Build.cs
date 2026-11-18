using Rocks.Build.Rules;

// Config layers and console settings (Docs/Design/ConfigSystem.ko.md, config CF1 to CF3). Merges values from the engine, project, platform, user,
// session and console layers into one priority order and keeps a source stack per value. Settings structs
// (ROCKS_STRUCT(Settings(...))) are read through ReflectionText JSON import and assignment batches.
// CF3 adds user layer saving (difference against the lower layers, atomic replace), lenient reading of old user files, restart-pending
// values and development hot reload of config files at the sync point.
// Wiring to the render lane (RenderBase RB1) is outside this module; it only holds the settings snapshot data structures and generation numbers.
public sealed class Config : CoreModuleRules
{
    protected override string ModuleName => "Config";

    protected override string[] RequiredModules => ["ReflectionText", "ObjectRuntime", "Core"];
}
