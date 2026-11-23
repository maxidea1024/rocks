using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Shader compile management module (main process) (Docs/Design/ShaderSystem sections 3, 10 and 11.1). It holds the shader source tree and the
/// preprocessor (SC2, SC3), preprocess define assembly (SC10), compile options (SC9), job keys and the shader nugget record values and location (P5),
/// and grows the compile manager and worker pool (SH1). Format definitions come from ShaderCompilerCommon. AssetNugget: key writer and the nugget
/// cache. HostConfig: the engine folder used as the nugget root without a project. It checks tool names against the tool table (ShaderToolTable)
/// but never opens tool binaries (policy check shader-tool-binary-load).
/// </summary>
public sealed class ShaderCompiler : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderCompiler";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ShaderCompilerCommon", "AssetNugget", "HostConfig", "PlatformBase", "Core"];
}
