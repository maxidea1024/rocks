using Rocks.Build;
using Rocks.Build.Rules;

public sealed class CommandletHost : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "RocksCommandlet",
        Kind = NativeModuleKind.Executable,
        // Commandlet은 desktop 도구다. Android에는 host backend가 없으므로 공통 소스만 나열한다(대상에
        // 포함하지 않는 한 빌드되지 않는다).
        Sources = context.Platform == NativePlatform.Android
            ? context.Sources("Programs/Commandlet/Private/Common")
            : context.Sources("Programs/Commandlet/Private/Common", $"Programs/Commandlet/Private/{context.PlatformDirectory}"),
        PrivateIncludeDirectories = [context.PathInRoot("Programs/Commandlet/Private/Common")],
        Dependencies = ["Core"]
    };
}
