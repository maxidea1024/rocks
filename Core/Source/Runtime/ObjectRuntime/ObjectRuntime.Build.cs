using Rocks.Build.Rules;

// Object, Registry, GC and reflection runtime (UE CoreUObject counterpart).
public sealed class ObjectRuntime : CoreModuleRules
{
    protected override string ModuleName => "ObjectRuntime";

    // Serialization: field codecs (FieldCodec.h and GeneratedFieldCodec.h) use the Archive and Record formats
    // (Docs/Design/ReflectionFoundations.ko.md section 6.4).
    protected override string[] RequiredModules => ["MetaSchema", "Task", "Serialization"];
}
