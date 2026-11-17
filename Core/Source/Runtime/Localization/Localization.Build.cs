using Rocks.Build.Rules;

// Localized Text, catalogs and culture snapshots.
public sealed class Localization : CoreModuleRules
{
    protected override string ModuleName => "Localization";

    protected override string[] RequiredModules => ["Core"];
}
