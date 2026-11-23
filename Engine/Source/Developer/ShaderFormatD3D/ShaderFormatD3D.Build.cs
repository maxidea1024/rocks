using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// D3D shader formats inside the shader compile worker (Docs/Design/ShaderSystem.ko.md sections 2 and 11.1 SC1). SH1c implements D3D12_SM6
/// (DXC to DXIL with the built-in validator signature check); D3D11_SM5 (FXC and its fallback path) arrives in SH1e. Tool binaries are opened by
/// the worker through ShaderToolLoader (a ShaderFormat module may use shader tools). It must not depend on the main process shader compiler or the
/// asset nugget (policy check shader-worker-isolation).
/// </summary>
public sealed class ShaderFormatD3D : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ShaderFormatD3D";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["ShaderCompilerCommon", "ShaderToolLoader", "Core"];
}
