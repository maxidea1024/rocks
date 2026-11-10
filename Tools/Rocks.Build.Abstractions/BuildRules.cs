using Rocks.Build;

namespace Rocks.Build.Rules;

/// <summary>규칙은 신뢰하는 저장소 코드다. 파일 목록은 실행마다 다시 평가하며 숨은 환경 입력을 금지한다.</summary>
public sealed record BuildContext(string RootDirectory, string OutputDirectory,
    NativePlatform Platform, NativeConfiguration Configuration, string? SdkDirectory = null)
{
    public string PlatformDirectory => Platform == NativePlatform.Windows ? "Win64" : "Mac";
    public string PathInRoot(string path) => Path.GetFullPath(path, RootDirectory);

    public IReadOnlyList<string> Sources(params string[] directories) => directories
        .SelectMany(directory => Directory.GetFiles(PathInRoot(directory), "*.cpp", SearchOption.AllDirectories))
        .Order(StringComparer.Ordinal).ToArray();
}

public abstract class ModuleRules
{
    public abstract NativeModuleInput Configure(BuildContext context);
}

public sealed record MetadataUnit(string Module, string Input, string Include, string Output);

public abstract class TargetRules
{
    public abstract string Name { get; }
    public abstract IReadOnlyList<string> Modules { get; }
    public virtual IReadOnlyList<MetadataUnit> Metadata => [];
    // 모든 메타 입력이 공유하는 소스 관리 identity 명세다. 일반 빌드는 읽기만 한다.
    public virtual string? IdentityManifest => null;
    public virtual bool CheckPublicHeaders => false;
    public virtual string? TestExecutable => null;
}
