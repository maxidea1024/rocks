using Rocks.Build;
using Rocks.Build.Rules;

// 메모리·파일·스레드·시계 등 OS 경계 모듈이다. 공개 인터페이스는 플랫폼 독립이고 구현은
// Private/<플랫폼>/에 둔다. POSIX 플랫폼은 공통 Posix 구현과 OS hook(Apple·Android)을 함께 빌드한다.
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
            // Mac 소스가 sibling Apple 공통 헤더를 쓰므로 증분 빌드의 입력에도 포함한다.
            PrivateIncludeDirectories = context.Platform == NativePlatform.MacOS
                ? [context.PathInRoot($"{backend}/Apple")]
                : [],
            Dependencies = RequiredModules,
            // macOS 디렉터리 감시(FSEvents)는 CoreServices framework를 쓴다.
            AdditionalLinkArguments = context.Platform == NativePlatform.MacOS
                ? ["-framework", "CoreServices"]
                : []
        };
    }
}
