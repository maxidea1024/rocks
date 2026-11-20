using Rocks.Build;
using Rocks.Build.Rules;

// Sparks compiler library (Docs/Design/SparksLanguage.ko.md section 18.1): source text management, diagnostics and, in later milestones,
// the lexer, parser, checker and bytecode generator. Engine layer module that depends on Core only (no World, RHI or Object
// dependency), so the CLI, the language server and runtime eval can link it without the rest of the engine.
public sealed class SparksCompiler : EngineModuleRules
{
    protected override string ModuleName => "SparksCompiler";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] RequiredModules => ["Core"];
}
