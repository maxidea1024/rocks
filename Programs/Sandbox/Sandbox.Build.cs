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
// Texture and WorldIntegration: the screen path converts cooked TexturePlatformData (engine procedural textures, memory scenes and the cooked
// test content of Programs/Sandbox/TestContent) into RenderBase cooked texture uploads with WorldIntegration PlatformTextureSource
// (Docs/Design/RenderBase section 18.5 items 7 and 8 — SM7, SM8).
// The OpenGL ES backend is the GLES31_PC preview on Windows and is kept out of Release Windows executables (ShaderSystem section 9, UE keeps the
// ES preview out of shipping Windows builds as well); Android always links it (GLES31_ANDROID).
// --audio-tone plays a test tone through Audio's output device; it links AudioMiniaudio, whose module lifetime object registers the
// miniaudio backend factory in the AudioOutputBackendRegistry host service (Docs/Design/AudioSystem section 3.34.5 — the host code never calls it).
// --engine runs AudioEngineSubsystem and AudioWorldSubsystem from EngineAudio (keys 1-3 audio check, Docs/Design/AudioSystem section 3.35.11);
// AudioOpus registers its decoder factory in the AudioDecoderRegistry host service for the engine decoder table (section 3.37.1).
// UiCore, UiFramework and UiRender: the --engine window draws a UI status panel over the scene on the same swap chain, and the window swap
// chain is a UI window surface (Docs/Design/RenderBase section 18.5 item 9); UiRender's module lifetime object registers the UI shader types.
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
            PublicDependencies = ["Launch", "Engine", "EngineAssets", "Texture", "WorldIntegration", "Collision", "Physics", "Gameplay", "HostConfig", "PlatformWindow", "SplashScreen", "RenderBase", "RenderGraph", "ShaderCore", "UiCore", "UiFramework", "UiRender", "D3D12Graphics",
                "D3D11Graphics", "VulkanGraphics", "Audio", "AudioMiniaudio", "AudioOpus", "EngineAudio",
                .. (openGl ? ["OpenGLGraphics"] : Array.Empty<string>()),
                .. (shaderProvider ? ["DevelopmentShaderProvider"] : Array.Empty<string>()),
                .. (traceAnalysis ? ["TraceAnalysis"] : Array.Empty<string>())]
        };
    }
}
