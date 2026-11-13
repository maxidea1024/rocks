using Rocks.Build.Rules;

// Object·Registry·GC·Reflection 실행(UE CoreUObject 대응).
public sealed class ObjectRuntime : CoreModuleRules
{
    protected override string ModuleName => "ObjectRuntime";

    // Serialization: 필드 codec(FieldCodec.h·GeneratedFieldCodec.h)이 Archive·Record 형식을 쓴다(Docs/Design/ReflectionFoundations.ko.md 6.4절).
    protected override string[] RequiredModules => ["MetaSchema", "Task", "Serialization"];
}
