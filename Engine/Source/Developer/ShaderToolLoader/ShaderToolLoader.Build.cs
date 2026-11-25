using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Shader tool opening module (Docs/Design/ShaderSystem section 9). Matches tool files in the user store against the tool table (ShaderToolTable)
/// and SHA-256, then opens them with P10 DynamicLibrary::Open and obtains the function table. Only workers and ShaderFormat modules open tools; until
/// the worker (Programs/ShaderCompileWorker) exists, this module stands in for it (policy check shader-tool-binary-load).
/// Platform implementations are Private/Win64 (repository root; DXC version query that uses dxcapi.h together with Windows headers), Private/Mac and Private/Unsupported
/// (Android).
/// </summary>
public sealed class ShaderToolLoader : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderToolLoader";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ShaderToolTable", "PlatformBase", "Core"];

    // Tool headers seen only by this module's sources (BS3a): DXC dxcapi.h (Windows implementation), and the SPIRV-Tools and SPIRV-Cross C APIs
    // used by the shared format tool calls (ShaderToolCalls.h, Docs/Design/ShaderSystem section 11.1 SH1e E2).
    protected override string[] PrivateDependencies => ["DirectXShaderCompiler", "SpirvTools", "SpirvCross"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        string platform = context.Platform switch
        {
            NativePlatform.Windows => "Win64",
            NativePlatform.MacOS => "Mac",
            _ => "Unsupported"
        };
        return base.Configure(context) with
        {
            Sources = [.. Directory.GetFiles(context.PathInRoot(backend), "*.cpp").Order(StringComparer.Ordinal),
                .. context.Sources($"{backend}/{platform}")]
        };
    }
}
