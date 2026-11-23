using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Shader compile worker body (Docs/Design/ShaderSystem sections 11 and 11.1 SC1, SC4-SC7): control lines on the standard streams, input and
/// output message files, version checks and exit codes, tool queries, dispatch to the ShaderFormat* modules and the test hooks. The worker program
/// (Programs/ShaderCompileWorker) calls it. It may open tool binaries (policy check shader-tool-binary-load) and must not depend on the main
/// process shader compiler or the asset nugget (policy check shader-worker-isolation).
/// </summary>
public sealed class ShaderWorker : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderWorker";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["PlatformBase", "Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["DeterministicMath", "ShaderCompilerCommon", "ShaderFormatD3D", "ShaderFormatVulkan", "ShaderToolLoader"];
}
