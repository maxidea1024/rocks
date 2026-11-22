using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Curve value type test fixture module (Docs/Design/CurveData.ko.md section 16, CV5). The Foundation and Engine test executables depend on it.
// This module owns the reflection header that places curves in every value position (class field, array element, Optional, map value,
// InstancedStruct content), the InterpCurve mirror holder (CV9 — fields of the CoreTypeBindings InterpCurve mirrors) and its identity spec
// (CurvesTestFixtures.rocksids.json). Header only. It is kept separate from ReflectionTestFixtures so the reflection fixtures do not depend on
// the Curves module.
public sealed class CurvesTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "CurvesTestFixtures";

    // Test fixture: not started by phase (Docs/Design/ModuleLoader.ko.md section 3.11). Tests register its types into their own registries;
    // the engine group of a test executable must not register them into every engine.
    protected override LoadingPhase LoadingPhase => LoadingPhase.None;

    protected override string[] PublicDependencies => ["Curves", "CoreTypeBindings"];

    protected override bool HasSources => false;
}
