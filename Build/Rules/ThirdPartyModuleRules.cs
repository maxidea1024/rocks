using Rocks.Build;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

/// <summary>
/// 고정 버전 외부 라이브러리 모듈 규칙이다(UE ThirdParty 모듈 대응). 소스·정의·include는 라이브러리 폴더의
/// <c>ThirdParty.json</c>(단일 출처)에서 읽는다. 폴더 구조:
/// <code>
/// Core/Source/ThirdParty/&lt;Module&gt;/
///   &lt;Module&gt;.Build.cs   이 클래스를 상속한 규칙(이름만 적는다)
///   ThirdParty.json      버전·출처·라이선스·파일별 SHA-256·번역 단위·정의(정책 검사 third-party가 확인)
///   &lt;lib&gt;-&lt;version&gt;/      릴리스 원본을 byte 그대로 둔 디렉터리(수정 금지, 서식·lexical 검사 제외)
///   *.c                  필요할 때만 두는 Rocks 글루(정책 검사 대상)
/// </code>
/// 규칙 계약:
/// <list type="bullet">
/// <item>외부 헤더는 전파하지 않는다. PublicIncludeDirectories가 비어 있고, 소비 모듈은 <see cref="ConsumerIncludeDirectories"/>와
/// <see cref="ConsumerDefines"/>를 private으로 받는다. 엔진 공개 헤더는 외부 헤더를 포함하지 않는다.</item>
/// <item>항상 최적화한다(<see cref="CodeOptimization.Always"/>). Debug 빌드의 에셋 해제가 크게 느려지는 것을 막는다.
/// CRT·구성 매크로·NDEBUG는 구성을 따른다.</item>
/// <item>규칙 로더는 모든 규칙의 Configure를 매 빌드 실행하므로 manifest 오류는 즉시 빌드 오류가 된다.</item>
/// </list>
/// 사용 예(Core/Source/ThirdParty/Zstd/Zstd.Build.cs):
/// <code>
/// public sealed class Zstd : ThirdPartyModuleRules
/// {
///     protected override string ModuleName => "Zstd";
/// }
/// </code>
/// </summary>
public abstract class ThirdPartyModuleRules : ModuleRules
{
    /// <summary>모듈 이름이다. 라이브러리 폴더 이름과 같아야 한다.</summary>
    protected abstract string ModuleName { get; }

    /// <summary>저장소 루트 기준 라이브러리 폴더다. 예: Core/Source/ThirdParty/Zstd.</summary>
    public static string LibraryDirectory(string module) => $"{ThirdPartyManifest.ThirdPartyRoot}/{module}";

    public override NativeModuleInput Configure(BuildContext context)
    {
        string library = LibraryDirectory(ModuleName);
        ThirdPartyManifest manifest = ThirdPartyManifest.Read(context.PathInRoot(library));
        return new()
        {
            Name = ModuleName,
            Sources = manifest.TranslationUnits.Select(unit => context.PathInRoot($"{library}/{unit}")).ToArray(),
            PrivateIncludeDirectories = manifest.IncludeDirectories.Select(directory => context.PathInRoot($"{library}/{directory}")).ToArray(),
            PrivateDefines = manifest.DefinesFor(context.Platform.ToString()),
            Optimization = CodeOptimization.Always
        };
    }

    /// <summary>
    /// adapter(소비 모듈의 Private 소스)가 외부 헤더를 include할 경로다. 여러 라이브러리를 순서대로 합친다.
    /// </summary>
    public static IReadOnlyList<string> ConsumerIncludeDirectories(BuildContext context, params string[] modules) =>
        modules.SelectMany(module => ThirdPartyManifest.Read(context.PathInRoot(LibraryDirectory(module))).IncludeDirectories
            .Select(directory => context.PathInRoot($"{LibraryDirectory(module)}/{directory}"))).ToArray();

    /// <summary>
    /// adapter가 외부 헤더를 include할 때 라이브러리 빌드와 같아야 하는 정의다(예: zlib Z_PREFIX). 같은 이름이 다른 값으로
    /// 겹치면 빌드 도구가 오류로 거부한다.
    /// </summary>
    public static IReadOnlyList<string> ConsumerDefines(BuildContext context, params string[] modules) =>
        modules.SelectMany(module => ThirdPartyManifest.Read(context.PathInRoot(LibraryDirectory(module))).ConsumerDefines)
            .Distinct(StringComparer.Ordinal).ToArray();
}
