using Rocks.Build;
using Rocks.Build.Rules;

// UI elements and input routing on the Game lane. Platform and rendering are connected separately.
public sealed class UiFramework : EngineModuleRules
{
    protected override string ModuleName => "UiFramework";

    protected override string[] PrivateDependencies => ["PlatformBase"];

    protected override string[] PublicDependencies => ["UiCore", "TextLayout", "Localization"];
}
