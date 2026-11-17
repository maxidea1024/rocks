using Rocks.Build.Rules;

// Lowest-level header module holding only primitive types, the Guid POD and ABI definitions (no allocation).
public sealed class Base : CoreModuleRules
{
    protected override string ModuleName => "Base";

    protected override string[] RequiredModules => [];

    protected override bool HasSources => false;
}
