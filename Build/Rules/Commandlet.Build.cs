using Rocks.Build;
using Rocks.Build.Rules;

public sealed class CommandletHost : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "RocksCommandlet",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/Commandlet/Private/Common", $"Programs/Commandlet/Private/{context.PlatformDirectory}"),
        PrivateIncludeDirectories = [context.PathInRoot("Programs/Commandlet/Private/Common")],
        Dependencies = ["Core"]
    };
}
