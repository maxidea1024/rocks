using Rocks.Build;
using Rocks.Build.Rules;

public sealed class CommandletHost : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "RocksCommandlet",
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
        // Launch: the common entry point (command line capture, crash reporting and the tool log, Docs/Design/CrashHandlingAndLaunch.ko.md).
        PublicDependencies = ["Core", "Launch", "TraceAnalysis", "Crypto", "Compression", "HostConfig", "AssetRuntime"]
    };
}
