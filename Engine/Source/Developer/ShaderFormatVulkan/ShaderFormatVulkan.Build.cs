using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Vulkan shader format processor (Docs/Design/ShaderSystem sections 2 and 11.1 SC1, linked by the shader compile worker). Inside the worker the
/// VULKAN_SM5 processor compiles with -fspv-reflect, rewrites inputs (ATTRIBUTEn), inter-stage locations (TEXCOORDn) and set/binding, validates
/// again with spirv-val and turns the SPIRV-Reflect reflection into the binding layout value through the shared binding rules (ShaderSystem
/// section 16.1 SSD29-SSD33). Tool binaries are opened from the user store through ShaderToolLoader. The development Graphics compile hook it
/// used to carry is gone: devices take cooked SPIR-V (ShaderSystem section 16.1 SH2e2).
/// </summary>
public sealed class ShaderFormatVulkan : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderFormatVulkan";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ShaderCompilerCommon", "Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ShaderToolLoader"];
}
