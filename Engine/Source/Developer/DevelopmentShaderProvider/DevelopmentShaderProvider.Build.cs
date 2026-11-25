using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Development shader provider (Docs/Design/RenderBase section 9.12 item 4, ShaderSystem section 12.3.1 SSD16): implements the RenderBase shader
/// provider interface with the Developer shader compiler (CompileShaderLibrary, the shader compile worker and the shader nugget cache) and builds
/// each requested format's engine library in memory. Its module lifetime object attaches the provider to the host's EngineShaderHost service at
/// Startup and detaches it at Shutdown, so an executable gets on-demand shader compiling by linking this module outside Release (Runtime modules
/// never depend on it — RenderBase SX14). HostConfig: the engine folder that holds Shaders and Config/ShaderFormats.
/// </summary>
public sealed class DevelopmentShaderProvider : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "DevelopmentShaderProvider";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["RenderBase", "Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ShaderCompiler", "ShaderCompilerCommon", "ShaderCore", "AssetNugget", "ObjectRuntime", "Task", "HostConfig",
        "ModuleLoader", "PlatformBase"];
}
