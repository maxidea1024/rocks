using Rocks.Build;
using Rocks.Build.Rules;

// Standalone example that wires input, the UI runtime and Graphics into a window (Docs/Design/UiToolkit section 9.1). The UI shaders are engine
// shader types (UiRender UiShaderTypes); the demo has no cooked shader library, so outside Release it carries the development shader provider
// (Developer DevelopmentShaderProvider) that builds the device format's library in memory, like Sandbox (ShaderSystem section 16.1 SSD19).
// Release UiDemo has no provider and cannot open its shader set.
public sealed class UiDemoProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context)
    {
        bool shaderProvider = context.Configuration != NativeConfiguration.Release;
        return new()
        {
            Name = "UiDemo",
            ModuleDirectory = "Programs/UiDemo",
            Kind = NativeModuleKind.Executable,
            Sources = context.Sources("Programs/UiDemo/Private"),
            PrivateDefines = [$"UIDEMO_SHADER_PROVIDER={(shaderProvider ? 1 : 0)}"],
            PublicDependencies = ["Launch", "PlatformWindow", "D3D11Graphics", "UiFramework", "UiRender", "UiWidget", "RenderBase", "ShaderCore",
                .. (shaderProvider ? ["DevelopmentShaderProvider"] : Array.Empty<string>())],
            PrivateDependencies = ["AssetDatabase", "WorldIntegration"]
        };
    }
}
