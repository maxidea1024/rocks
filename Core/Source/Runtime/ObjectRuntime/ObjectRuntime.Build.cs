using Rocks.Build;
using Rocks.Build.Rules;

// Object, Registry, GC and reflection runtime (UE CoreUObject counterpart).
public sealed class ObjectRuntime : CoreModuleRules
{
    protected override string ModuleName => "ObjectRuntime";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Serialization: field codecs (FieldCodec.h and GeneratedFieldCodec.h) use the Archive and Record formats
    // (Docs/Design/ReflectionFoundations.ko.md section 6.4).
    // Localization: rocks::Text field values (Docs/Design/ReflectionExtensions.ko.md section 13).
    protected override string[] RequiredModules => ["MetaSchema", "Task", "Serialization", "Localization"];
}
