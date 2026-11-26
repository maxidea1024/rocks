using Rocks.Build;
using Rocks.Build.Rules;

public sealed class CommandletHost : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "RocksCommandlet",
        ModuleDirectory = "Programs/Commandlet",
        Kind = NativeModuleKind.Executable,
        // Commandlet is a desktop tool. Android has no standard output backend, so only the common sources are listed (it is not built
        // unless a target includes it). The platform directory holds the standard output channel (StandardOutput.cpp).
        Sources = context.Platform == NativePlatform.Android
            ? context.Sources("Programs/Commandlet/Private/Common")
            : context.Sources("Programs/Commandlet/Private/Common", $"Programs/Commandlet/Private/{context.PlatformDirectory}"),
        PrivateIncludeDirectories = [context.PathInRoot("Programs/Commandlet/Private/Common")],
        // TraceAnalysis (Developer): the trace-report command. Commandlet is a tool (Program) target, so it may link Developer modules.
        // Crypto: the log keygen/decode/seal commands (encrypted log files).
        // Compression: log decode/view read compressed log files (independent zstd frames per batch).
        // HostConfig: the config-dump command boots the same config context as the engine hosts (engine files, log.*, trace.*).
        // AssetRuntime: the content-sign command (signed cooked manifest envelope, Docs/Design/ContentSigning.ko.md).
        // AssetCommandlets (Developer): the asset-* commands (editor package index and AssetService, Docs/Design/AssetSystem
        // section 13 — the command functions live in that module so EngineTests can call them in process).
        // Animation, Texture: the asset type modules the asset commands start (Skeleton, AnimationClip; Texture2D and the other texture
        // classes — AssetRuntime is their dependency, Docs/Design/AssetSystem section 12.8).
        // Launch: the common entry point (command line capture, crash reporting and the tool log, Docs/Design/CrashHandlingAndLaunch.ko.md).
        // UnrealExchange (plugin enabled by the Commandlet target): the asset-import-unreal command (Docs/Design/UnrealExchange section
        // 10.5; a Program may depend on the modules of plugins its target enables, Docs/Design/BuildTargets BD39).
        // NodeGraphTools (Developer): the graph-dump, graph-validate and graph-diff commands (Docs/Design/NodeGraph section 9.4).
        // ShaderCompiler (Developer): the shader-compile command (Docs/Design/ShaderSystem section 11.1 SC13).
        // TextureTools (Developer): the texture importers, the settings normalization hook and the asset-import argument translator
        // (Docs/Design/AssetSystem section 12.12).
        // MeshTools and AnimationTools (Developer): the static mesh, skeletal mesh and animation clip cook processors and the animation
        // Build dependency hook (Docs/Design/MeshAnimationAssets section 13.1 item 1).
        // Collision: owns the Engine.json Collision section (Docs/Design/CollisionData.ko.md section 3); a program that reads the engine config
        // must register every section's settings struct (an unknown section is a config error), as Sandbox does.
        // Cooker module set (Docs/Design/BuildTargets section 3.10, BS10 item 1 and 4): the engine Runtime and ClientOnly modules that the engine
        // repository shipping target (Sandbox, Release) links, so their module lifetime objects register the same shader types in the cooker as in
        // the shipping executable (the shader library cook stage starts the engine group, policy check cooker-module-set). OpenGLGraphics is linked
        // on every platform: Windows shipping targets do not have it, but the rule is a superset and Android and macOS shipping targets do.
        // Audio, AudioMiniaudio and EngineAudio come from the Sandbox audio modes (Docs/Design/AudioSystem sections 3.34.5 and 3.35.11); without an
        // AudioOutputBackendRegistry host service the AudioMiniaudio module lifetime object stays idle in the cooker.
        PublicDependencies = ["Core", "Launch", "TraceAnalysis", "Crypto", "Compression", "HostConfig", "AssetRuntime", "AssetCommandlets", "Animation",
            "Texture", "UnrealExchange", "NodeGraphTools", "ShaderCompiler", "TextureTools", "MeshTools", "AnimationTools", "Collision",
            "Engine", "Gameplay", "Physics", "EngineAssets", "PlatformWindow", "RenderBase", "RenderGraph", "ShaderCore", "D3D12Graphics", "D3D11Graphics",
            "VulkanGraphics", "OpenGLGraphics", "Audio", "AudioMiniaudio", "EngineAudio"]
    };
}
