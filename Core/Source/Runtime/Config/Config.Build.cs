using Rocks.Build;
using Rocks.Build.Rules;

// Config layers and console settings (Docs/Design/ConfigSystem.ko.md, config CF1 to CF4). Merges values from the engine, project, platform, user,
// session and console layers into one priority order and keeps a source stack per value. Settings structs
// (ROCKS_STRUCT(Settings(...))) are read through ReflectionText JSON import and assignment batches.
// CF3 adds user layer saving (difference against the lower layers, atomic replace), lenient reading of old user files, restart-pending
// values and development hot reload of config files at the sync point. CF4 adds device profiles (selection order, parent chain, memory
// buckets) and the quality combination check as a pure function (quality groups themselves are CVarGroup, Docs/Design/CVarSystem.ko.md
// section 11). Device traits are plain name/value input filled by the host; this module does not depend on Graphics.
// Wiring to the render lane (RenderBase RB1) is outside this module; it only holds the settings snapshot data structures and generation numbers.
public sealed class Config : CoreModuleRules
{
    protected override string ModuleName => "Config";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ReflectionText", "ObjectRuntime", "Core"];

    // CF4 device profiles are a settings struct of this module (identity spec Config.rocksids.json).
    protected override IReadOnlyList<string> Metadata => ["Config/DeviceProfileSettings.h"];
}
