using Rocks.Build;
using Rocks.Build.Rules;

// Sparks tools (Docs/Design/SparksTools.ko.md, LC8): workspace loading, the commands of the `sparks` CLI (check, dump, build, ids,
// fmt, test) and the language server core. Development module: the Sparks programs and the tests link it; game targets do not.
public sealed class SparksTools : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "SparksTools";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["SparksCompiler", "SparksVm", "SparksTestHost"];
}
