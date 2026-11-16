using Rocks.Build;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

/// <summary>
/// 고정 버전 외부 라이브러리 모듈 규칙이다(UE ThirdParty 모듈 대응). 소스·정의·include는 라이브러리 폴더의
/// <c>ThirdParty.json</c>(단일 출처)에서 읽는다. 라이브러리 폴더는 Core가 쓰면 <c>Core/Source/ThirdParty</c>, 엔진·도구 전용이면
/// <c>Engine/Source/ThirdParty</c> 아래에 둔다(<see cref="ThirdPartyManifest.ThirdPartyRoots"/>). 폴더 구조:
/// <code>
/// &lt;루트&gt;/&lt;Module&gt;/
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
/// CRT·구성 매크로·NDEBUG는 구성을 따른다. manifest <c>linkTimeOptimization: false</c>이면 Release에서도 ThinLTO에서 빼고,
/// <c>sanitizerInstrumentation: false</c>이면 Asan에서도 계측하지 않는다.</item>
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

    /// <summary>
    /// 저장소 루트 기준 라이브러리 폴더다. 예: Core/Source/ThirdParty/Zstd. 루트 목록에서 찾으며 없거나 두 루트에 모두 있으면
    /// <see cref="FormatException"/>이다(빌드 오류).
    /// </summary>
    public static string LibraryDirectory(BuildContext context, string module) => ThirdPartyManifest.FindLibrary(context.RootDirectory, module);

    public override NativeModuleInput Configure(BuildContext context)
    {
        string library = LibraryDirectory(context, ModuleName);
        ThirdPartyManifest manifest = ThirdPartyManifest.Read(context.PathInRoot(library));
        if (manifest.Kind != ThirdPartyKind.Code)
        {
            throw new FormatException($"{library}: only a code library has a module rule (kind is {manifest.Kind})");
        }

        return new()
        {
            Name = ModuleName,
            Sources = manifest.TranslationUnits.Select(unit => context.PathInRoot($"{library}/{unit}")).ToArray(),
            PrivateIncludeDirectories = manifest.IncludeDirectories.Select(directory => context.PathInRoot($"{library}/{directory}")).ToArray(),
            PrivateDefines = manifest.DefinesFor(context.Platform.ToString(), context.Architecture.ToString()),
            // manifest compileArguments는 GCC형 철자이고 clang-cl도 그대로 받는다(ThirdPartyManifest.AllowedCompileArguments).
            // /clang: 우회 경로는 빌드 입력 검증이 막으므로 쓰지 않는다.
            AdditionalCompileArguments = manifest.CompileArguments,
            Optimization = CodeOptimization.Always,
            LinkTimeOptimization = manifest.LinkTimeOptimization,
            SanitizerInstrumentation = manifest.SanitizerInstrumentation
        };
    }

    /// <summary>
    /// adapter(소비 모듈의 Private 소스)가 외부 헤더를 include할 경로다. 여러 라이브러리를 순서대로 합친다. code 라이브러리뿐 아니라
    /// 헤더 전용(<c>kind: headers</c>, 모듈 의존 없이 include만)과 binary의 커밋한 공개 헤더(<c>kind: binary</c>)에도 쓴다.
    /// 사용 예: <c>ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "VulkanHeaders", "SpirvReflect")</c>.
    /// </summary>
    public static IReadOnlyList<string> ConsumerIncludeDirectories(BuildContext context, params string[] modules) =>
        modules.SelectMany(module =>
        {
            string library = LibraryDirectory(context, module);
            return ThirdPartyManifest.Read(context.PathInRoot(library)).IncludeDirectories
                .Select(directory => context.PathInRoot($"{library}/{directory}"));
        }).ToArray();

    /// <summary>
    /// adapter가 외부 헤더를 include할 때 라이브러리 빌드와 같아야 하는 정의다(예: zlib Z_PREFIX). 같은 이름이 다른 값으로
    /// 겹치면 빌드 도구가 오류로 거부한다.
    /// </summary>
    public static IReadOnlyList<string> ConsumerDefines(BuildContext context, params string[] modules) =>
        modules.SelectMany(module => ThirdPartyManifest.Read(context.PathInRoot(LibraryDirectory(context, module))).ConsumerDefines)
            .Distinct(StringComparer.Ordinal).ToArray();
}
