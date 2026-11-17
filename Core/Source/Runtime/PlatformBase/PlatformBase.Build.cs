using Rocks.Build;
using Rocks.Build.Rules;

// OS boundary module for memory, files, threads, clocks and the like. The public interface is platform independent and implementations
// live in Private/<Platform>/. POSIX platforms build the shared Posix implementation together with OS hooks (Apple and Android).
public sealed class PlatformBase : CoreModuleRules
{
    protected override string ModuleName => "PlatformBase";

    protected override string[] RequiredModules => ["Base"];

    public override NativeModuleInput Configure(BuildContext context)
    {
        string backend = $"{ModuleDirectory}/Private";
        return new()
        {
            Name = ModuleName,
            Sources = context.Platform switch
            {
                NativePlatform.MacOS => context.Sources($"{backend}/Mac", $"{backend}/Apple", $"{backend}/Posix",
                    $"{backend}/Common"),
                NativePlatform.Android => context.Sources($"{backend}/Android", $"{backend}/Posix", $"{backend}/Common"),
                _ => context.Sources($"{backend}/{context.PlatformDirectory}", $"{backend}/Common")
            },
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            // Mac sources use the sibling Apple shared headers, so they are also inputs of incremental builds.
            PrivateIncludeDirectories = context.Platform == NativePlatform.MacOS
                ? [context.PathInRoot($"{backend}/Apple")]
                : [],
            Dependencies = RequiredModules,
            // macOS directory watching (FSEvents) uses the CoreServices framework.
            Frameworks = context.Platform == NativePlatform.MacOS ? ["CoreServices"] : []
        };
    }
}
