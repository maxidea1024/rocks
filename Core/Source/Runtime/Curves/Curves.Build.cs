using Rocks.Build;
using Rocks.Build.Rules;

// Curve value types (Docs/Design/CurveData.ko.md sections 3 and 4, CV5 and CV6): ScalarCurveBase, TangentCurve and PlainCurve with their
// keys, evaluation, automatic tangents, key editing, redundant key removal and value range, the step curves IntegerStepCurve,
// NameStepCurve and StringStepCurve, and PackedTangentCurve (section 11). The evaluation formulas live in Core
// (Core/Math/KeyCurveMath.h, Core/Time/FrameRate.h). This module holds the reflected value types. Reflection headers: Curves/CurveTypes.h,
// Curves/TangentCurve.h, Curves/PlainCurve.h, Curves/StepCurves.h (CV6 step curves), Curves/PackedTangentCurve.h (CV8 compressed
// tangent curve), identity spec Curves.rocksids.json.
public sealed class Curves : CoreModuleRules
{
    protected override string ModuleName => "Curves";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ObjectRuntime"];
}
