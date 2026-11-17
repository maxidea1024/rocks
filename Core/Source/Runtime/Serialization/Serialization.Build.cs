using Rocks.Build.Rules;

// Archive contract and versioning rules (no Object dependency).
public sealed class Serialization : CoreModuleRules
{
    protected override string ModuleName => "Serialization";

    protected override string[] RequiredModules => ["Core"];
}
