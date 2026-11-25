using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// D3D shader formats inside the shader compile worker (Docs/Design/ShaderSystem sections 2 and 11.1 SC1). D3D12_SM6 is DXC to DXIL with the
/// built-in validator signature check; D3D11_SM5 is FXC with the DXC, SPIR-V, HLSL 5.0, FXC fallback path (SH1e). Tool binaries are opened by
/// the worker through ShaderToolLoader (a ShaderFormat module may use shader tools). It must not depend on the main process shader compiler or the
/// asset nugget (policy check shader-worker-isolation). Shader reflection (DXC IDxcUtils for DXIL, FXC D3DReflect for DXBC) turns the compiled code into
/// the binding layout value through the shared binding rules (ShaderSystem section 16.1 SSD33); it uses COM types and lives in Private/Win64, other
/// platforms report UnsupportedHost (Private/Unsupported).
/// </summary>
public sealed class ShaderFormatD3D : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderFormatD3D";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ShaderCompilerCommon", "Core"];

    // Not used by the public headers, so dependents do not see them (Docs/Design/BuildTargets section 3.5 BS3b).
    protected override string[] PrivateDependencies => ["ShaderToolLoader", "DirectXShaderCompiler"];

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
