using Rocks.Build.Rules;

// Module loader (Docs/Design/ModuleLoader.ko.md, UE FModuleManager counterpart): starts modules from the build-generated module table in dependency
// order, shuts module groups down in reverse start order, issues per-module code leases, and holds the host service table. The module lifetime base
// class and the entry point macro live in Core (Core/Module/ModuleEntry.h) because every module, Core included, implements an entry point.
// The loader does not know the type system (MD3); it depends only on Core and PlatformBase.
public sealed class ModuleLoader : CoreModuleRules
{
    protected override string ModuleName => "ModuleLoader";

    protected override string[] RequiredModules => ["Core", "PlatformBase"];
}
