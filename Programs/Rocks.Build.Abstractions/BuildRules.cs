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
    public string PlatformDirectory => LinkTypes.PlatformDirectory(Platform, nameof(Platform));

    public string PathInRoot(string path) => Path.GetFullPath(path, RootDirectory);

    /// <summary>
    /// 폴더 아래 C++ 원본(<c>.cpp</c>)과 Objective-C++ 원본(<c>.mm</c> — Apple backend 폴더에만 둔다, Docs/Design/MacPlatform.ko.md 6절)을
    /// 이름 순으로 모은다. <c>.mm</c>을 Apple 밖 대상에 넘기면 입력 검증이 거부한다.
    /// </summary>
    public IReadOnlyList<string> Sources(params string[] directories) => directories
        .SelectMany(directory => Directory.GetFiles(PathInRoot(directory), "*.cpp", SearchOption.AllDirectories)
            .Concat(Directory.GetFiles(PathInRoot(directory), "*.mm", SearchOption.AllDirectories)))
        .Order(StringComparer.Ordinal).ToArray();

    /// <summary>
    /// 플러그인을 찾는 폴더다(Docs/Design/BuildTargets 3.9절 BD32). 엔진 저장소 빌드는 <c>Engine/Plugins</c>, 설치 SDK 게임
    /// 빌드(<see cref="SdkDirectory"/>가 있음)는 프로젝트의 <c>Plugins</c>다. 설치 SDK는 엔진 플러그인을 싣지 않는다.
    /// </summary>
    public string PluginRoot => PathInRoot(SdkDirectory is null ? "Engine/Plugins" : "Plugins");

    /// <summary>
    /// <see cref="PluginRoot"/>에서 찾은 모든 플러그인(켜지 않은 것 포함, 이름 순)이다. <see cref="RuleLoader"/>가 모듈 규칙의 Configure 전에
    /// 채운다. 플러그인 모듈 규칙(PluginModuleRules)이 자기 모듈 폴더·종류·단계를 기술자에서 찾을 때 쓴다.
    /// </summary>
    public IReadOnlyList<Descriptors.PluginDescriptor> Plugins { get; init; } = [];
}

public abstract class ModuleRules
{
    public abstract NativeModuleInput Configure(BuildContext context);
}

// 리플렉션 메타데이터는 모듈이 소유한다(ModuleDirectory — 리플렉션 헤더는 빌드가 표지로 찾는다, Docs/Design/ReflectionFoundations.ko.md 1.10절). 타깃은 모듈만 고른다.
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
    /// 이 타깃이 켜거나 끄는 플러그인이다(Docs/Design/BuildTargets 3.9절 BD33, UE TargetRules.EnablePlugins·DisablePlugins). 형식과 의미는
    /// 프로젝트 기술자의 <c>plugins[]</c>와 같고, 같은 이름을 프로젝트도 적으면 이 목록이 이긴다. 켠 플러그인의 의존 플러그인도 켜진다.
    /// Program 타깃은 여기(와 프로젝트)에 적은 플러그인만 켜고, Game 타깃은 enabledByDefault 플러그인도 켠다(BD34). 켠 플러그인의 모듈은
    /// <see cref="Modules"/>의 첫 항목(주 실행 파일)에 링크된다(BD36). 이름은 식별자여야 하고 중복은 오류다. 사용 예:
    /// <code>
    /// public override IReadOnlyList&lt;PluginReference&gt; Plugins =&gt; [new("UnrealExchange", true)];
    /// </code>
    /// </summary>
    public virtual IReadOnlyList<Descriptors.PluginReference> Plugins => [];

    /// <summary>
    /// 링크 방식이다(Docs/Design/BuildTargets 3.8절 BD17, UE TargetRules.LinkType). Monolithic은 모든 모듈을 실행 파일 하나에 링크하고,
    /// Modular는 모듈마다 공유 라이브러리를 만든다. Editor 타깃은 Modular가 기본이고(UE와 같다) 나머지 종류는 Monolithic이 기본이다.
    /// Android와 Asan·Coverage·Release 구성은 Modular를 거부한다.
    /// </summary>
    public virtual NativeLinkType LinkType => Type == TargetType.Editor ? NativeLinkType.Modular : NativeLinkType.Monolithic;

    /// <summary>
    /// Program만: 에디터 모듈을 링크하는 도구다(Docs/Design/BuildTargets 3.1절, UE <c>bCompileAgainstEditor</c>). 켜면
    /// <c>ROCKS_WITH_EDITOR=1</c>·<c>ROCKS_WITH_EDITOR_DATA=1</c>이다. 다른 종류가 켜면 규칙 로드 오류다. 사용 예:
    /// <code>
    /// public override bool CompileWithEditor => true;
    /// </code>
    /// </summary>
    public virtual bool CompileWithEditor => false;

    /// <summary>
    /// Program만: 에디터 전용 데이터를 읽는 에셋 도구다(Docs/Design/BuildTargets 3.1절 BD19). 켜면 <c>ROCKS_WITH_EDITOR_DATA=1</c>이다.
    /// 다른 종류가 켜면 규칙 로드 오류다.
    /// </summary>
    public virtual bool WithEditorData => false;

    /// <summary>이 타깃의 종류·Program 속성과 타깃 매크로다(<see cref="TargetKind"/>).</summary>
    public TargetKind Kind => new(Type, CompileWithEditor, WithEditorData);

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

    /// <summary>
    /// 시험 동반 타깃이다(Docs/Design/BuildTargets 3.8절 BS8d6). native 하니스가 쓰는 다른 실행 파일을 그 실행 파일 하나뿐인 타깃으로
    /// 적는다 — Modular 타깃은 실행 파일이 하나라(manifest·빌드 정보) Launch 시험 프로그램을 같은 타깃에 둘 수 없다. <c>test</c>가 native 시험
    /// 뒤 하니스 전에 같은 구성으로 따로(자기 출력에) 빌드하고 실행 파일 경로를 하니스에 넘긴다. 동반 타깃은 Program이고 시험 실행 파일이
    /// 없어야 한다. <see cref="CompanionTargets"/>(같은 빌드에 모듈을 합치는 Monolithic 동반)와 다르다. 사용 예:
    /// <code>
    /// public override IReadOnlyList&lt;string&gt; TestPrograms =&gt; ["LaunchProbeModular"];
    /// </code>
    /// </summary>
    public virtual IReadOnlyList<string> TestPrograms => [];

    /// <summary>
    /// 이 타깃과 함께 빌드해 같은 출력 폴더에 두는 프로그램 타깃의 이름이다(Docs/Design/ShaderSystem 11.1절 SC14, UE 에디터가
    /// ShaderCompileWorker를 함께 빌드하는 <c>bEditorDependsOnShaderCompileWorker</c> 대응). 빌드는 동반 타깃의 <see cref="Modules"/>를
    /// 이 타깃의 모듈 목록 뒤에 더해 같은 구성으로 한 번에 빌드한다. 동반 타깃은 Program·Monolithic이어야 하고 플러그인·동반 타깃을 가질 수
    /// 없으며, 이 타깃도 Monolithic이어야 한다(아니면 빌드가 거부한다). 사용 예:
    /// <code>
    /// public override IReadOnlyList&lt;string&gt; CompanionTargets =&gt; ["ShaderCompileWorker"];
    /// </code>
    /// </summary>
    public virtual IReadOnlyList<string> CompanionTargets => [];
}
