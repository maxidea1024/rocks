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

/// <summary>
/// 타깃 종류다(UE 5.8 TargetType의 Game·Program 대응, Editor·Client·Server는 아직 없다).
/// <list type="bullet">
/// <item>Game: 출시하는 실행 파일과 그것이 링크하는 엔진 모듈 묶음이다. Release 구성에서 셰이더 시스템 Release 배제 정책
/// (Docs/Design/ShaderSystem.ko.md 9절)을 받는다: 모듈 도달 집합에 Developer 모듈이 없고 셰이더 컴파일러(d3dcompiler·dxcompiler)를
/// 링크하지 않으며, Windows에서는 OpenGLGraphics를 링크하지 않는다.</item>
/// <item>Program: 개발 도구·시험·벤치마크 실행 파일이다(셰이더 워커, 시험 runner, Commandlet). UE처럼 Release에서도 Developer
/// 모듈을 쓸 수 있다(UE <c>bBuildDeveloperTools</c>는 Program 타깃이면 구성과 관계없이 켜진다).</item>
/// </list>
/// </summary>
public enum TargetType { Game, Program }

// 리플렉션 메타데이터는 모듈이 소유한다(NativeModuleInput.Metadata·ModuleDirectory). 타깃은 모듈만 고른다.
public abstract class TargetRules
{
    public abstract string Name { get; }

    public abstract IReadOnlyList<string> Modules { get; }

    /// <summary>
    /// 타깃 종류다. 기본은 UE TargetRules.Type처럼 Game이며, 도구·시험·벤치마크 타깃은 Program으로 재정의한다. 사용 예:
    /// <code>
    /// public override TargetType Type => TargetType.Program;
    /// </code>
    /// </summary>
    public virtual TargetType Type => TargetType.Game;

    /// <summary>
    /// 로그 컴파일 하한의 타깃 재정의다(UE <c>COMPILED_IN_MINIMUM_VERBOSITY</c> 대응). null(기본)이면 구성 기본값(Debug: Trace,
    /// Development: Debug, Release: Info)이다. 이 레벨보다 낮은 로그 호출은 템플릿 문자열까지 바이너리에서 빠진다. 값은 타깃의 모든
    /// 모듈에 <c>ROCKS_LOG_COMPILED_MINIMUM</c>으로 같게 정의된다(모듈별 재정의는 ODR 위반이라 빌드 검사가 거부한다). 설치 SDK의
    /// 엔진 라이브러리는 구성 기본값으로 미리 빌드되므로 SDK export 타깃과 SDK 게임 타깃에서는 재정의할 수 없다(ArgumentException).
    /// 사용 예:
    /// <code>
    /// public override LogLevel? LogCompiledMinimum => LogLevel.Warning;
    /// </code>
    /// </summary>
    public virtual LogLevel? LogCompiledMinimum => null;

    public virtual bool CheckPublicHeaders => false;

    public virtual string? TestExecutable => null;
}
