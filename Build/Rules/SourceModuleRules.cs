using Rocks.Build;
using Rocks.Build.Rules;

/// <summary>
/// 한 모듈 폴더를 기준으로 경로를 만드는 공통 규칙이다(UE ModuleRules의 ModuleDirectory 대응).
/// 모듈 폴더 구조:
/// <code>
/// &lt;Layer&gt;/Source/&lt;Group&gt;/&lt;Module&gt;/
///   &lt;Module&gt;.Build.cs   이 모듈의 규칙(CoreModuleRules·EngineModuleRules를 상속)
///   Public/              다른 모듈이 include하는 헤더
///   Private/             구현(.cpp)과 모듈 내부 헤더
///   Tests/               모듈 테스트(테스트 실행 파일 규칙이 모은다. 모듈 라이브러리에는 들어가지 않는다)
/// </code>
/// Layer는 Core(기반 라이브러리) 또는 Engine(게임 엔진 계층)이다. Core 모듈은 Engine 모듈을 참조하지 않는다.
/// </summary>
public abstract class SourceModuleRules : ModuleRules
{
    /// <summary>모듈이 속한 최상위 계층 폴더(Core·Engine)다.</summary>
    protected abstract string Layer { get; }

    /// <summary>모듈 이름이다. 폴더 이름과 같아야 한다.</summary>
    protected abstract string ModuleName { get; }

    /// <summary>직접 의존하는 모듈 이름이다. 같은 계층의 아래 모듈이나 아래 계층 모듈만 둔다.</summary>
    protected abstract string[] RequiredModules { get; }

    /// <summary>Source 아래 그룹(Runtime·Developer·Editor)이다.</summary>
    protected virtual string Group => "Runtime";

    /// <summary>Private/에 구현 소스가 있는지다. 헤더만 있는 모듈은 false로 둔다.</summary>
    protected virtual bool HasSources => true;

    /// <summary>
    /// 이 모듈의 최적화 재정의다(UE ModuleRules.OptimizeCode 대응). 기본값은 구성을 따른다.
    /// 예: Development 빌드에서 이 모듈만 단계 실행하려면 임시로 Never를 둔다.
    /// <code>
    /// protected override CodeOptimization Optimization => CodeOptimization.Never;
    /// </code>
    /// Never는 Release에서도 최적화와 ThinLTO를 끄므로 커밋하지 않는다. Always는 Debug에서도 최적화한다.
    /// </summary>
    protected virtual CodeOptimization Optimization => CodeOptimization.Default;

    /// <summary>저장소 루트 기준 모듈 폴더다. 예: Core/Source/Runtime/Core.</summary>
    protected string ModuleDirectory => $"{Layer}/Source/{Group}/{ModuleName}";

    /// <summary>
    /// 리플렉션 메타데이터 헤더(ROCKS_CLASS 선언)의 include 철자다. Public/ 또는 Private/ 기준이다. 비어 있지 않으면
    /// 이 모듈 폴더에 identity 명세 <c>&lt;ModuleName&gt;.rocksids.json</c>이 있어야 한다. 예:
    /// <code>
    /// protected override IReadOnlyList&lt;string&gt; Metadata =&gt; ["ReflectionTestFixtures/ReflectionFixture.h"];
    /// </code>
    /// 생성 헤더는 <c>&lt;ReflectionTestFixtures/ReflectionFixture.generated.h&gt;</c>로 include한다.
    /// </summary>
    protected virtual IReadOnlyList<string> Metadata => [];

    /// <summary>
    /// 이 모듈 소스에만 쓰는 include 경로다(의존 모듈로 전파하지 않는다). 외부 라이브러리 adapter가 쓴다.
    /// 예: <c>ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "Zstd")</c>.
    /// </summary>
    protected virtual IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) => [];

    /// <summary>이 모듈 소스에만 쓰는 정의다(의존 모듈로 전파하지 않는다).</summary>
    protected virtual IReadOnlyList<string> PrivateDefines(BuildContext context) => [];

    public override NativeModuleInput Configure(BuildContext context) => HasSources
        ? new()
        {
            Name = ModuleName,
            Sources = context.Sources($"{ModuleDirectory}/Private"),
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            PrivateIncludeDirectories = PrivateIncludeDirectories(context),
            PrivateDefines = PrivateDefines(context),
            Dependencies = RequiredModules,
            Optimization = Optimization,
            Metadata = Metadata,
            ModuleDirectory = ModuleDirectory
        }
        : new()
        {
            Name = ModuleName,
            PublicIncludeDirectories = [context.PathInRoot($"{ModuleDirectory}/Public")],
            Dependencies = RequiredModules,
            Metadata = Metadata,
            ModuleDirectory = ModuleDirectory
        };
}

/// <summary>
/// 기반 라이브러리(Core 계층) 모듈 규칙이다. 엔진이 아니라 엔진·도구·게임이 함께 쓰는 바탕이다.
/// 사용 예(Core/Source/Runtime/Core/Core.Build.cs):
/// <code>
/// public sealed class Core : CoreModuleRules
/// {
///     protected override string ModuleName => "Core";
///     protected override string[] RequiredModules => ["PlatformBase"];
/// }
/// </code>
/// </summary>
public abstract class CoreModuleRules : SourceModuleRules
{
    protected override string Layer => "Core";
}

/// <summary>게임 엔진 계층(Engine/Source) 모듈 규칙이다. Core 모듈에 의존할 수 있다.</summary>
public abstract class EngineModuleRules : SourceModuleRules
{
    protected override string Layer => "Engine";
}
