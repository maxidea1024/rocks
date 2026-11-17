using Rocks.Build.Rules;

// Object Archive bridge (must not reference ObjectRuntime back).
public sealed class ObjectSerialization : CoreModuleRules
{
    protected override string ModuleName => "ObjectSerialization";

    protected override string[] RequiredModules => ["ObjectRuntime", "Serialization"];
}
