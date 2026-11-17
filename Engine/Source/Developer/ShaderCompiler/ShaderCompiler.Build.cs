using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Shader compile management module (main process) (Docs/Design/ShaderSystem.ko.md sections 3 and 12.1). For now it only has P6a format definition
/// (<c>Engine/Config/ShaderFormats/&lt;id&gt;.json</c>) and engine compile profile (<c>Engine-&lt;host&gt;.json</c>) reading, validation and SHA-256.
/// The preprocessor (decision 6), job keys (P5) and worker communication (SH1) are added to this module. It checks the tool names of format definitions
/// against the tool table (ShaderToolTable) but never opens tool binaries (policy check shader-tool-binary-load).
/// </summary>
public sealed class ShaderCompiler : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderCompiler";

    protected override string[] RequiredModules => ["ShaderToolTable", "PlatformBase", "Core"];
}
