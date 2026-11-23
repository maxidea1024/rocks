using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Shader compiler code shared by the main process and the shader compile worker (Docs/Design/ShaderSystem sections 3 and 11.1 SC1): P6a format
/// definition (<c>Engine/Config/ShaderFormats/&lt;id&gt;.json</c>) and engine compile profile reading, validation and SHA-256. Worker message formats,
/// exit codes and the format compile interface (ShaderFormatBackend) are added in SH1c. It checks the tool names of format definitions against the
/// tool table (ShaderToolTable) but never opens tool binaries (policy check shader-tool-binary-load).
/// </summary>
public sealed class ShaderCompilerCommon : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderCompilerCommon";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ShaderToolTable", "PlatformBase", "Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["DeterministicMath", "Serialization"];
}
