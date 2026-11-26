using Rocks.Build;
using Rocks.Build.Rules;

// Material compiler (Docs/Design/MaterialSystem sections 3.2, 6 and 7, stages MS2a-MS2f): graph validation, function and layer expansion,
// the typed IR (values, hash-consing builder, constant folding, dead value removal, IR hash), HLSL generation of the material source file
// /Engine/Generated/Material.rsh, constant programs, layout and diagnostics with the source location map. It never compiles shaders and
// never touches devices or processes (job submission is MaterialCook and the editor compile coordinator).
// Developer module: editor, cooker and tool programs link it, shipping runtime targets do not.
public sealed class MaterialCompiler : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "MaterialCompiler";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // MaterialSchema for the graph schema, node definitions and value types; Core for containers, text and SHA-256.
    protected override string[] PublicDependencies => ["Core", "MaterialSchema", "Texture"];

    // DeterministicMath gives constant folding the same bytes on every host (floor, truncation and rounding — section 6.5).
    protected override string[] PrivateDependencies => ["DeterministicMath"];
}
