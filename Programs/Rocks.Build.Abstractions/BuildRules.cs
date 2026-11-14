using Rocks.Build;

namespace Rocks.Build.Rules;

/// <summary>
/// 규칙은 신뢰하는 저장소 코드다. 파일 목록은 실행마다 다시 평가하며 숨은 환경 입력을 금지한다.
/// <see cref="Architecture"/>는 컴파일러가 보고한 대상 triple에서 정한 값이다(Android는 ABI마다 다르다).
/// </summary>
public sealed record BuildContext(string RootDirectory, string OutputDirectory,
    NativePlatform Platform, NativeArchitecture Architecture, NativeConfiguration Configuration, string? SdkDirectory = null)
{
    // 새로운 타깃은 명시적으로 추가해야 하며 알 수 없는 값을 다른 플랫폼으로 취급하지 않는다.
    public string PlatformDirectory => Platform switch
    {
        NativePlatform.Windows => "Win64",
        NativePlatform.MacOS => "Mac",
        NativePlatform.Android => "Android",
        _ => throw new ArgumentOutOfRangeException(nameof(Platform), Platform, "Unsupported native platform.")
    };

    public string PathInRoot(string path) => Path.GetFullPath(path, RootDirectory);

    public IReadOnlyList<string> Sources(params string[] directories) => directories
        .SelectMany(directory => Directory.GetFiles(PathInRoot(directory), "*.cpp", SearchOption.AllDirectories))
        .Order(StringComparer.Ordinal).ToArray();
}

public abstract class ModuleRules
{
    public abstract NativeModuleInput Configure(BuildContext context);
}

// 리플렉션 메타데이터는 모듈이 소유한다(NativeModuleInput.Metadata·ModuleDirectory). 타깃은 모듈만 고른다.
public abstract class TargetRules
{
    public abstract string Name { get; }

    public abstract IReadOnlyList<string> Modules { get; }

    public virtual bool CheckPublicHeaders => false;

    public virtual string? TestExecutable => null;
}
