using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// Shader tool spike (SH0, Docs/Design/ShaderSystem section 16). Builds the current built-in Renderer shaders as DXIL, SPIR-V, DXBC, MSL, ESSL and HLSL 5.0
/// and runs validation (spirv-val, DXIL signature hash, reflection dump) and measurements (S2, S3, S8, S12). As a measurement program that opens tools directly
/// before the worker (Programs/ShaderCompileWorker) exists, it is an allowed location of the shader tool reference policy. It does not go into the product runtime.
/// OS-dependent code (DXC COM, FXC, D3D12, WGL) lives in Private/Win64; on other platforms Private/Unsupported reports it as unsupported.
/// </summary>
public sealed class ShaderToolSpikeProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context)
    {
        const string root = "Programs/ShaderToolSpike/Private";
        bool windows = context.Platform == NativePlatform.Windows;
        return new()
        {
            Name = "ShaderToolSpike",
            ModuleDirectory = "Programs/ShaderToolSpike",
            Kind = NativeModuleKind.Executable,
            Sources = context.Sources($"{root}/Common", windows ? $"{root}/Win64" : $"{root}/Unsupported"),
            PrivateIncludeDirectories = [context.PathInRoot($"{root}/Common")],
            PublicDependencies = ["Launch", "ShaderToolLoader", "SpirvReflect"],
            // Public tool headers this program includes (Docs/Design/BuildTargets section 3.5 BS3a).
            PrivateDependencies = ["DirectXShaderCompiler", "SpirvCross", "SpirvTools"],
            // Used for the D3D12 shader model query (S2) and the hidden-window GL context (S12). Tool DLLs are not linked but opened at run time.
            SystemLibraries = windows ? ["d3d12", "dxgi", "opengl32", "gdi32", "user32"] : []
        };
    }
}
