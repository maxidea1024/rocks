using Rocks.Build;
using Rocks.Build.Rules;

public sealed class CommandletHost : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "RocksCommandlet",
        Kind = NativeModuleKind.Executable,
        // Commandlet is a desktop tool. Android has no host backend, so only the common sources are listed (it is not built unless
        // a target includes it).
        Sources = context.Platform == NativePlatform.Android
            ? context.Sources("Programs/Commandlet/Private/Common")
            : context.Sources("Programs/Commandlet/Private/Common", $"Programs/Commandlet/Private/{context.PlatformDirectory}"),
        PrivateIncludeDirectories = [context.PathInRoot("Programs/Commandlet/Private/Common")],
        // TraceAnalysis (Developer): the trace-report command. Commandlet is a tool (Program) target, so it may link Developer modules.
        Dependencies = ["Core", "TraceAnalysis"]
    };
}
