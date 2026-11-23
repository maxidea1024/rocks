using Rocks.Build.Rules;

// Shader compile worker (Docs/Design/ShaderSystem section 11.1 SC14). Targets that start workers build it as a companion target
// (TargetRules.CompanionTargets) so it lands next to their executable with the same configuration.
public sealed class ShaderCompileWorkerTarget : TargetRules
{
    public override string Name => "ShaderCompileWorker";

    // Development tool (not subject to the Release exclusion policy).
    public override TargetType Type => TargetType.Program;

    public override IReadOnlyList<string> Modules => ["ShaderCompileWorker"];
}
