using Rocks.Build.Rules;

// 객체 Archive bridge(ObjectRuntime 역참조 금지).
public sealed class ObjectSerialization : CoreModuleRules
{
    protected override string ModuleName => "ObjectSerialization";
    protected override string[] RequiredModules => ["ObjectRuntime", "Serialization"];
}
