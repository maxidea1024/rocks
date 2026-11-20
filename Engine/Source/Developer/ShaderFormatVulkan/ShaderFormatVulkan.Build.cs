using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Development Vulkan shader format (Docs/Design/VulkanGraphics.ko.md section 13). Until cooked shaders exist (ShaderSystem SH2) it implements the
/// Graphics shader source compiler hook: HLSL source to Vulkan 1.1 SPIR-V with DXC (VULKAN_SM5 options), then a SPIR-V rewrite (ATTRIBUTEn input
/// locations, draw constants as push constants, set/binding per ShaderSystem 7.2 item 4, SPV_GOOGLE reflection data removed), spirv-val and
/// SPIRV-Reflect reflection. Tool binaries are opened from the user store through ShaderToolLoader (a ShaderFormat module may load shader tools).
/// Only development programs link it; Release Game targets must not reach it (Release exclusion policy).
/// The DXC call uses COM types and lives in Private/Win64; other platforms report Unsupported (Private/Unsupported).
/// </summary>
public sealed class ShaderFormatVulkan : EngineModuleRules
{
    // Tool and library headers included only by this module's sources (adapter style).
    private static readonly string[] ToolHeaders = ["DirectXShaderCompiler", "SpirvTools", "SpirvReflect"];

    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderFormatVulkan";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Graphics", "ShaderToolLoader", "SpirvReflect"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        string platform = context.Platform == NativePlatform.Windows ? "Win64" : "Unsupported";
        return base.Configure(context) with
        {
            Sources = [.. Directory.GetFiles(context.PathInRoot(backend), "*.cpp").Order(StringComparer.Ordinal),
                .. context.Sources($"{backend}/{platform}")],
            PrivateIncludeDirectories = [context.PathInRoot(backend), .. ThirdPartyModuleRules.ConsumerIncludeDirectories(context, ToolHeaders)],
            PrivateDefines = ThirdPartyModuleRules.ConsumerDefines(context, ToolHeaders)
        };
    }
}
