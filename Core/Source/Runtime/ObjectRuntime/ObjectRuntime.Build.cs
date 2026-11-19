using Rocks.Build.Rules;

// Object, Registry, GC and reflection runtime (UE CoreUObject counterpart).
public sealed class ObjectRuntime : CoreModuleRules
{
    protected override string ModuleName => "ObjectRuntime";

    // Serialization: field codecs (FieldCodec.h and GeneratedFieldCodec.h) use the Archive and Record formats
    // (Docs/Design/ReflectionFoundations.ko.md section 6.4).
    // Localization: rocks::Text field values (Docs/Design/ReflectionExtensions.ko.md section 13).
    // ModuleLoader: ModuleTypeRegistrations implements the module loader's type registrar (Docs/Design/ModuleLoader.ko.md section 3.10).
    protected override string[] RequiredModules => ["MetaSchema", "Task", "Serialization", "Localization", "ModuleLoader"];
}
