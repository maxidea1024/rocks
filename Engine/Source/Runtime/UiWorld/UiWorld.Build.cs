using Rocks.Build;
using Rocks.Build.Rules;

// UI in the world (Docs/Design/UiToolkit section 2): world-space widget components, their virtual pointer interaction
// and the presenter that draws them into the game viewport (UE UWidgetComponent and the UMG game layer). Kept apart from
// WorldIntegration (asset components above World) so server builds do not link the UI and its render path.
public sealed class UiWorld : EngineModuleRules
{
    protected override string ModuleName => "UiWorld";

    protected override string[] PublicDependencies => ["World", "UiWidget", "UiRender"];
}
