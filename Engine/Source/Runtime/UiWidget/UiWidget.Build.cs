using Rocks.Build;
using Rocks.Build.Rules;

// Synchronizes lifetime and properties between game UI objects and the non-object Element tree.
public sealed class UiWidget : EngineModuleRules
{
    protected override string ModuleName => "UiWidget";
    protected override string[] PrivateDependencies => ["ObjectSerialization", "PlatformBase"];
    protected override string[] PublicDependencies => ["ObjectRuntime", "CoreTypeBindings", "Curves", "Localization", "UiFramework", "AssetRuntime", "World"];
}
