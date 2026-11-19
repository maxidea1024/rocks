using Rocks.Build;
using Rocks.Build.Rules;

// Archive contract and versioning rules (no Object dependency).
public sealed class Serialization : CoreModuleRules
{
    protected override string ModuleName => "Serialization";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] RequiredModules => ["Core"];
}
