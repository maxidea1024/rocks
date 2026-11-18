using Rocks.Build.Rules;

// Scalar key curve value types (Docs/Design/CurveData.ko.md sections 3 and 4, CV5): ScalarCurveBase, TangentCurve and PlainCurve with their
// keys, evaluation, automatic tangents, key editing, redundant key removal and value range. The evaluation formulas live in Core
// (Core/Math/KeyCurveMath.h, Core/Time/FrameRate.h). This module holds the reflected value types. Reflection headers: Curves/CurveTypes.h,
// Curves/TangentCurve.h, Curves/PlainCurve.h, identity spec Curves.rocksids.json.
public sealed class Curves : CoreModuleRules
{
    protected override string ModuleName => "Curves";

    protected override string[] RequiredModules => ["ObjectRuntime"];

    protected override IReadOnlyList<string> Metadata => ["Curves/CurveTypes.h", "Curves/TangentCurve.h", "Curves/PlainCurve.h"];
}
