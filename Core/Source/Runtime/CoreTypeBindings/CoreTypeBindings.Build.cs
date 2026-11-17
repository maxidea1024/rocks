using Rocks.Build.Rules;

// Reflection binding module for Core value types (vectors, quaternions, rotators, colors, integer vectors, Transform, Transform3f and Transform3d)
// (Docs/Design/ReflectionFoundations.ko.md section 4.5). Core does not depend on MetaSchema or ObjectRuntime, so Core headers cannot carry
// annotations; this module's mirror declarations (ROCKS_STRUCT(Binds = "...")) reflect the same public members, and the generated
// code emits GeneratedType for the real Core types. Binds may be used only in this module. Modules that reflect Core value type fields
// depend on this module. Header only.
public sealed class CoreTypeBindings : CoreModuleRules
{
    protected override string ModuleName => "CoreTypeBindings";

    protected override string[] RequiredModules => ["ObjectRuntime"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata => ["CoreTypeBindings/CoreValueTypes.h"];
}
