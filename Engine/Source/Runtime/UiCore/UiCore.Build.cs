using Rocks.Build;
using Rocks.Build.Rules;

// UI coordinate and layout foundation with no dependency on windows or Graphics.
public sealed class UiCore : EngineModuleRules
{
    protected override string ModuleName => "UiCore";

    protected override string[] PublicDependencies => ["Core", "Input", "MetaSchema"];
}
