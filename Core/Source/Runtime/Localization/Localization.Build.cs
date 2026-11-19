using Rocks.Build;
using Rocks.Build.Rules;

// Localized Text, catalogs and culture snapshots.
public sealed class Localization : CoreModuleRules
{
    protected override string ModuleName => "Localization";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] RequiredModules => ["Core"];
}
