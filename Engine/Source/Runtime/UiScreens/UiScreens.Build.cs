using Rocks.Build.Rules;

// Manages local player screen lifetimes and input priority.
public sealed class UiScreens : EngineModuleRules
{
    protected override string ModuleName => "UiScreens";

    protected override string[] PublicDependencies => ["UiWidget", "Input"];
}
