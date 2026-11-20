using Rocks.Build;
using Rocks.Build.Rules;

// Object, Registry, GC and reflection runtime (UE CoreUObject counterpart).
public sealed class ObjectRuntime : CoreModuleRules
{
    protected override string ModuleName => "ObjectRuntime";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Shared PCH: modules that reach ObjectRuntime (object layer modules and the test executables) use this header instead of Core's
    // (Docs/Design/UnityBuildPch.ko.md section 8.4). It includes Core/CoreSharedPch.h and the stable headers the object layer shares.
    protected override string? SharedPchHeader => "Object/ObjectSharedPch.h";

    // Serialization: field codecs (FieldCodec.h and GeneratedFieldCodec.h) use the Archive and Record formats
    // (Docs/Design/ReflectionFoundations.ko.md section 6.4).
    // Localization: rocks::Text field values (Docs/Design/ReflectionExtensions.ko.md section 13).
    // ModuleLoader: ModuleTypeRegistrations implements the module loader's type registrar (Docs/Design/ModuleLoader.ko.md section 3.10).
    protected override string[] PublicDependencies => ["MetaSchema", "Task", "Serialization", "Localization", "ModuleLoader"];
}
