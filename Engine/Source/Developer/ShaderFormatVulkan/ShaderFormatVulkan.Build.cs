using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Development Vulkan shader format (Docs/Design/VulkanGraphics.ko.md section 13). Until cooked shaders exist (ShaderSystem SH2) it implements the
/// Graphics shader source compiler hook: HLSL source to Vulkan 1.1 SPIR-V with DXC (VULKAN_SM5 options), then a SPIR-V rewrite (ATTRIBUTEn input
/// locations, draw constants as push constants, set/binding per ShaderSystem 7.2 item 4, SPV_GOOGLE reflection data removed), spirv-val and
/// SPIRV-Reflect reflection. Tool binaries are opened from the user store through ShaderToolLoader (a ShaderFormat module may load shader tools).
/// Only development programs link it; Release Game targets must not reach it (Release exclusion policy).
/// The DXC call uses COM types and lives in Private/Win64; other platforms report Unsupported (Private/Unsupported).
/// Inside the shader compile worker the VULKAN_SM5 processor compiles with -fspv-reflect, rewrites inputs (ATTRIBUTEn), inter-stage locations
/// (TEXCOORDn) and set/binding, validates again with spirv-val and turns the SPIRV-Reflect reflection into the binding layout value through the
/// shared binding rules (ShaderSystem section 16.1 SSD29-SSD33).
/// </summary>
public sealed class ShaderFormatVulkan : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderFormatVulkan";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Graphics", "ShaderCompilerCommon", "SpirvReflect"];

    // Tool headers included only by this module's sources (Docs/Design/BuildTargets section 3.5 BS3a).
    protected override string[] PrivateDependencies => ["DirectXShaderCompiler", "SpirvTools", "ShaderToolLoader"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        string platform = context.Platform == NativePlatform.Windows ? "Win64" : "Unsupported";
        return base.Configure(context) with
        {
            Sources = [.. Directory.GetFiles(context.PathInRoot(backend), "*.cpp").Order(StringComparer.Ordinal),
                .. context.Sources($"{backend}/{platform}")],
            PrivateIncludeDirectories = [context.PathInRoot(backend)]
        };
    }
}
