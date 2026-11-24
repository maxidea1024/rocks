using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Runtime shader core (Docs/Design/ShaderSystem sections 3 and 12.2.1, part of UE RenderCore): the shared shader format vocabulary (backend,
/// stage, feature and device requirement bits, limits), the virtual shader path grammar, shader type descriptors, the shader type registry and
/// the module registration helper, permutation domains, format facts, hand-written parameter layouts and their signature, the binding layout
/// and GLES binding header formats, and the shader library file format v1 (SH2a2). The Developer shader compiler modules depend on it; it never depends on them (Release exclusion, section 9).
/// ModuleLoader gives the registration helper the host service table (Docs/Design/ModuleLoader.ko.md section 3.12).
/// </summary>
public sealed class ShaderCore : EngineModuleRules
{
    protected override string ModuleName => "ShaderCore";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ModuleLoader"];
}
