using Rocks.Build;
using Rocks.Build.Rules;

// Sparks VM (Docs/Design/SparksLanguage.ko.md sections 11 and 18.1, Docs/Design/SparksBytecode.ko.md section 4): the bytecode format
// (opcodes, instruction words, the per-module function, constant and reference tables) that the compiler writes and, from milestone
// LC5, the interpreter, value buffers and execution queues that run it. Engine layer module that depends on Core only.
public sealed class SparksVm : EngineModuleRules
{
    protected override string ModuleName => "SparksVm";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core"];
}
