using Rocks.Build.Rules;

// Shader compile worker (Docs/Design/ShaderSystem section 11.1 SC14). Program targets that start workers build it as a companion target
// (TargetRules.CompanionTargets) so it lands next to their executable with the same configuration. Game targets (Sandbox) build it first as a
// prerequisite target with these rules and its own output (TargetRules.PrerequisiteTargets, Docs/Design/BuildTargets section 3.11).
public sealed class ShaderCompileWorkerTarget : TargetRules
{
    public override string Name => "ShaderCompileWorker";

    // Development tool (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    // Built as a companion of editor-data targets (EngineTests, Commandlet) with their target macros (Docs/Design/BuildTargets section 3.1 —
    // companions share the host's macro combination), so it carries editor data too, like UE builds the worker for the editor.
    public override bool WithEditorData => true;

    public override IReadOnlyList<string> Modules => ["ShaderCompileWorker"];
}
