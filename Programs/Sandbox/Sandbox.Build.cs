using Rocks.Build;
using Rocks.Build.Rules;

// Executable for checking the engine vertical path. Wires windows, input, the graphics devices (D3D12/D3D11/OpenGL/Vulkan), Engine and World.
// --self-test checks offscreen render results without a window. The sample renderer used for test drawing (SampleRenderer) lives in this
// executable (it is not the engine renderer).
// Shaders are Programs/Sandbox/Shaders mounted as /Plugin/Sandbox (ShaderSystem section 16.1 SSD19). They are not cooked: the executable puts its
// shader types and source folder into the engine shader host (RenderBase section 9.12) and, outside Release, links the DevelopmentShaderProvider
// module, whose module lifetime object attaches the development provider (Developer ShaderCompiler, the shader compile worker and the user tool
// store) that builds each device format's library in memory. Release Game targets must not reach Developer modules, so Release Sandbox has no
// provider and its drawing self-tests report NOT RUN.
// With an engine-only cook (asset-cook --engine --profile=<host>, Engine/Saved/Cooked/<profile>) the --engine run adds the EngineAssets
// subsystem and opens the cooked engine shader libraries instead (ShaderSystem section 16.1 SSD48); the --engine run registers only the engine
// module shader types so that its type set matches the cooker.
// The OpenGL ES backend is the GLES31_PC preview on Windows and is kept out of Release Windows executables (ShaderSystem section 9, UE keeps the
// ES preview out of shipping Windows builds as well); Android always links it (GLES31_ANDROID).
public sealed class SandboxProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context)
    {
        bool release = context.Configuration == NativeConfiguration.Release;
        bool shaderProvider = !release;
        bool openGl = context.Platform != NativePlatform.Windows || !release;
        // The self-test reads the trace file of --trace-file with TraceAnalysis (Developer) outside Release only.
        bool traceAnalysis = !release;
        return new()
        {
            Name = "Sandbox",
            ModuleDirectory = "Programs/Sandbox",
            Kind = NativeModuleKind.Executable,
            // Android builds Sandbox as an APK program (lib Sandbox .so loaded by NativeActivity — Docs/Design/AndroidPlatform.ko.md).
            AndroidPackage = context.Platform == NativePlatform.Android
                ? new AndroidPackage("com.rocks.sandbox", context.PathInRoot("Programs/Sandbox/AndroidAssets"))
                : null,
            Sources = context.Sources("Programs/Sandbox/Private"),
            // The engine shader probe ids are shared by the probe shaders and the self-test (Docs/Design/EngineShaders section 7.6).
            PrivateIncludeDirectories = [context.PathInRoot("Programs/Sandbox/Private"), context.PathInRoot("Programs/Sandbox/EngineShaderTests/Shared")],
            PrivateDefines = [$"SANDBOX_OPENGL={(openGl ? 1 : 0)}",
                $"SANDBOX_TRACE_ANALYSIS={(traceAnalysis ? 1 : 0)}"],
            PublicDependencies = ["Launch", "Engine", "EngineAssets", "Collision", "Gameplay", "HostConfig", "PlatformWindow", "RenderBase", "RenderGraph", "ShaderCore", "D3D12Graphics",
                "D3D11Graphics", "VulkanGraphics",
                .. (openGl ? ["OpenGLGraphics"] : Array.Empty<string>()),
                .. (shaderProvider ? ["DevelopmentShaderProvider"] : Array.Empty<string>()),
                .. (traceAnalysis ? ["TraceAnalysis"] : Array.Empty<string>())]
        };
    }
}
