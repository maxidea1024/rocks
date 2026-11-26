using Rocks.Build;
using Rocks.Build.Rules;

// Connects world components and UI input/render targets to the game viewport.
public sealed class WorldIntegration : EngineModuleRules
{
    protected override string ModuleName => "WorldIntegration";
    protected override string[] PublicDependencies => ["World", "UiWidget", "UiRender"];
}
