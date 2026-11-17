using Rocks.Build;
using Rocks.Build.Rules;

// Executable for checking the engine vertical path. Wires windows, input, the graphics devices (D3D12/D3D11/OpenGL/Vulkan), Engine and World.
// --self-test checks offscreen render results without a window. The sample renderer used for test drawing (SampleRenderer) lives in this
// executable (it is not the engine renderer).
// Vulkan has no run-time shader compiler: outside Release the self-test hands the device the development shader compiler hook
// (Developer/ShaderFormatVulkan, DXC from the user tool store). Release Game targets must not reach Developer modules, so Release Sandbox
// leaves it out and its Vulkan run reports NOT RUN until cooked shaders exist (ShaderSystem SH2).
public sealed class SandboxProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context)
    {
        bool shaderCompiler = context.Configuration != NativeConfiguration.Release;
        return new()
        {
            Name = "Sandbox",
            Kind = NativeModuleKind.Executable,
            Sources = context.Sources("Programs/Sandbox/Private"),
            PrivateIncludeDirectories = [context.PathInRoot("Programs/Sandbox/Private")],
            PrivateDefines = [$"SANDBOX_SHADER_SOURCE_COMPILER={(shaderCompiler ? 1 : 0)}"],
            Dependencies = ["Engine", "PlatformWindow", "RenderBase", "D3D12Graphics", "D3D11Graphics", "OpenGLGraphics", "VulkanGraphics",
                .. (shaderCompiler ? ["ShaderFormatVulkan"] : Array.Empty<string>())]
        };
    }
}
