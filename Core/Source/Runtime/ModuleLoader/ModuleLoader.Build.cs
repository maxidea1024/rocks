using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Module loader (Docs/Design/ModuleLoader.ko.md, UE FModuleManager counterpart): starts modules from the build-generated module table in dependency
// order, shuts module groups down in reverse start order, issues per-module code leases, and holds the host service table. The module lifetime base
// class and the entry point macro live in Core (Core/Module/ModuleEntry.h) because every module, Core included, implements an entry point.
// The loader does not know the type system (MD3); it depends only on Core and PlatformBase.
public sealed class ModuleLoader : CoreModuleRules
{
    protected override string ModuleName => "ModuleLoader";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] RequiredModules => ["Core", "PlatformBase"];

    // Boot module (in the dependency closure of Launch): started by the module loader right after it is created.
    protected override LoadingPhase LoadingPhase => LoadingPhase.EarliestPossible;
}
