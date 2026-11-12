using Rocks.Build.Rules;

// Object·Registry·GC·Reflection 실행(UE CoreUObject 대응).
public sealed class ObjectRuntime : CoreModuleRules
{
    protected override string ModuleName => "ObjectRuntime";

    protected override string[] RequiredModules => ["MetaSchema", "Task"];
}
