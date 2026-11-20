using Rocks.Build;
using Rocks.Build.Rules;

// Sparks test host (Docs/Design/SparksLanguage.ko.md sections 13.1 and 18.1): the conformance runner that compiles each
// Conformance/<area>/<case>.spark and compares the result with <case>.expect, and, from milestone LC5, the fake engine host the VM
// runs against. Development module: tests and the Sparks CLI link it; game targets do not.
public sealed class SparksTestHost : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "SparksTestHost";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] RequiredModules => ["Core", "SparksCompiler"];
}
