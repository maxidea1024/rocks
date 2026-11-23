using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// OpenGL ES shader formats inside the shader compile worker (Docs/Design/ShaderSystem sections 2 and 11.1 SH1e E5): GLES31_ANDROID and GLES31_PC
/// are DXC to SPIR-V, spirv-val, then SPIRV-Cross ESSL 320 (output before the SH2 GLES rewrite). Tool binaries are opened by the worker through
/// ShaderToolLoader (a ShaderFormat module may use shader tools). It must not depend on the main process shader compiler or the asset nugget
/// (policy check shader-worker-isolation).
/// </summary>
public sealed class ShaderFormatOpenGL : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderFormatOpenGL";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ShaderCompilerCommon", "Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ShaderToolLoader"];
}
