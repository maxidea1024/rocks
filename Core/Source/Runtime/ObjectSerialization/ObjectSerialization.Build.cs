using Rocks.Build;
using Rocks.Build.Rules;

// Object Archive bridge (must not reference ObjectRuntime back).
public sealed class ObjectSerialization : CoreModuleRules
{
    protected override string ModuleName => "ObjectSerialization";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] RequiredModules => ["ObjectRuntime", "Serialization"];
}
