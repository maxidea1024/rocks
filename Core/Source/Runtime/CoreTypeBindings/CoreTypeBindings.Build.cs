using Rocks.Build.Rules;

// Reflection binding module for Core value types (vectors, quaternions, rotators, colors, integer vectors, Transform, Transform3f, Transform3d
// and FrameRate — Docs/Design/MeshAnimationAssets.ko.md section 7.5)
// (Docs/Design/ReflectionFoundations.ko.md section 4.5) and for InterpCurve, InterpCurvePoint and InterpCurveMode (Docs/Design/CurveData.ko.md
// section 12.1, CV9). Core does not depend on MetaSchema or ObjectRuntime, so Core headers cannot carry annotations; this module's mirror
// declarations (ROCKS_STRUCT(Binds = "...") and ROCKS_ENUM(Binds = "...")) reflect the same members, and the generated code emits
// GeneratedType for the real Core types. The InterpCurve mirrors list private members, which the generated code reaches through the
// GeneratedType friend that InterpCurve declares. Binds may be used only in this module. Modules that reflect Core value type fields
// depend on this module. Header only.
public sealed class CoreTypeBindings : CoreModuleRules
{
    protected override string ModuleName => "CoreTypeBindings";

    protected override string[] PublicDependencies => ["ObjectRuntime"];

    protected override bool HasSources => false;
}
