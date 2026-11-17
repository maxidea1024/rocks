using Rocks.Build;
using Rocks.Build.Rules;

// Executable for checking the engine vertical path. Wires windows, input, the graphics device (D3D12/OpenGL), Engine and World.
// --self-test checks offscreen render results without a window. The sample renderer used for test drawing (SampleRenderer) lives in this
// executable (it is not the engine renderer).
public sealed class SandboxProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "Sandbox",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/Sandbox/Private"),
        PrivateIncludeDirectories = [context.PathInRoot("Programs/Sandbox/Private")],
        Dependencies = ["Engine", "PlatformWindow", "RenderBase", "RenderGraph", "D3D12Graphics", "OpenGLGraphics"]
    };
}
