using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Development shader provider (Docs/Design/ShaderSystem/16-LaterStages.ko.md, SSD19 and its 2026-11-25 sharing note). Hosts without a cooked
/// shader library (Sandbox, UiDemo) hand it to a shader set: it mounts /Engine plus the host's own source roots, builds each device format's
/// library once with CompileShaderLibrary (shader nugget cache, development types included) and returns the bytes. Release Game targets must not
/// reach Developer modules, so hosts link this module outside Release only.
/// </summary>
public sealed class DevelopmentShaderProvider : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "DevelopmentShaderProvider";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["RenderBase", "Graphics", "Core"];

    // Not used by the public header, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ShaderCompiler", "ShaderCompilerCommon", "ShaderCore", "AssetNugget", "ObjectRuntime", "Task", "HostConfig",
        "PlatformBase"];
}
