using Rocks.Build.Rules;

// Curve value type test fixture module (Docs/Design/CurveData.ko.md section 16, CV5). The Foundation and Engine test executables depend on it.
// This module owns the reflection header that places curves in every value position (class field, array element, Optional, map value,
// InstancedStruct content) and its identity spec (CurvesTestFixtures.rocksids.json). Header only. It is kept separate from
// ReflectionTestFixtures so the reflection fixtures do not depend on the Curves module.
public sealed class CurvesTestFixtures : CoreModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "CurvesTestFixtures";

    protected override string[] RequiredModules => ["Curves"];

    protected override bool HasSources => false;

    protected override IReadOnlyList<string> Metadata => ["CurvesTestFixtures/CurveFixture.h"];
}
