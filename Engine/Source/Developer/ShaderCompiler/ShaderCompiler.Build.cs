using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Shader compile management module (main process) (Docs/Design/ShaderSystem sections 3, 10 and 11.1). It holds the shader source tree and the
/// preprocessor (SC2, SC3), preprocess define assembly (SC10), compile options (SC9), job keys and the shader nugget record values and location (P5),
/// and grows the compile manager and worker pool (SH1). Format definitions come from ShaderCompilerCommon. AssetNugget: key writer and the nugget
/// cache. AssetCooker: the shader library cook artifact stage (ShaderSystem section 16.1 SSD44 — this module registers it at Startup).
/// ModuleLoader: the cook stage starts the engine module set to fill the shader type registry (BuildTargets section 3.10 item 4).
/// HostConfig: the engine folder used as the nugget root without a project. It checks tool names against the tool table (ShaderToolTable)
/// but never opens tool binaries (policy check shader-tool-binary-load). Progress: the compile control (cancel token and progress sinks into a
/// ProgressCounter) and the shader-compile command progress output (CommandProgress — Docs/Design/ProgressReporting sections 9.5 and 12).
/// </summary>
public sealed class ShaderCompiler : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderCompiler";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ShaderCore", "ShaderCompilerCommon", "AssetNugget", "AssetCooker", "Progress", "PlatformBase", "Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["HostConfig", "ModuleLoader", "TargetProfiles", "Task"];
}
