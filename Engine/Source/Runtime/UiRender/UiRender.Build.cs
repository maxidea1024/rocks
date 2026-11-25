using Rocks.Build;
using Rocks.Build.Rules;

// Turns UI draw data into Graphics commands (Docs/Design/UiToolkit/02-LayersAndModules.ko.md, UiRender row). The UI shaders are engine shader
// types under Engine/Shaders/Private/Ui (UiToolkit section 9.1 UW1a); the module lifetime object registers them in the host shader type
// registry, the same way RenderBase registers its engine shader types.
public sealed class UiRender : EngineModuleRules
{
    protected override string ModuleName => "UiRender";

    protected override string[] PublicDependencies => ["UiCore", "Graphics", "TextLayout", "RenderBase", "ShaderCore"];

    // The module lifetime object reads the host services (ModuleLoader start context).
    protected override string[] PrivateDependencies => ["ModuleLoader"];
}
