using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    private const string PublicHeaderStaticStorageRule = "public-header-static-storage";

    // 주석·리터럴을 지운 텍스트의 thread_local 토큰이다.
    private static readonly Regex ThreadLocalToken = new(@"\bthread_local\b", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10));

    /// <summary>
    /// 엔진 소유 모듈의 공개 헤더(<c>&lt;루트&gt;/Source/&lt;그룹&gt;/&lt;모듈&gt;/Public/</c>와 엔진 플러그인 모듈의 <c>Public/</c>, 서드파티 제외)에서
    /// <c>thread_local</c>을 찾는다(Docs/Design/BuildTargets.ko.md 3.8절 BD29·BS8c, 규칙 <c>public-header-static-storage</c>). 스레드 지역
    /// 변수는 공유 라이브러리 경계로 import할 수 없으므로 정의(<c>inline thread_local</c>)뿐 아니라 선언(<c>extern thread_local</c>,
    /// <c>static thread_local</c> 데이터 멤버)도 공개 헤더에 두지 않는다 — 소유 모듈 <c>.cpp</c>에 두고 접근 함수로 노출한다. 쓰기 가능한 inline
    /// 변수·inline 함수 정적 지역 변수는 공개 헤더 검사의 Clang 진단(<c>-Wunique-object-duplication</c>)이 본다. 주석·문자열·문자 리터럴은 보지
    /// 않는다. 사용 예: <c>PolicyChecks.PublicHeaderStaticStorage(root)</c>.
    /// </summary>
    public static IReadOnlyList<CheckFailure> PublicHeaderStaticStorage(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (!relative.EndsWith(".h", StringComparison.Ordinal) || !IsPublicModuleHeader(relative))
            {
                continue;
            }

            string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            foreach (Match match in ThreadLocalToken.Matches(code))
            {
                int line = 1 + code.AsSpan(0, match.Index).Count('\n');
                failures.Add(new(relative, line, PublicHeaderStaticStorageRule,
                    "thread_local is not allowed in a public header: a thread-local variable cannot be imported across a shared library " +
                    "boundary, so each module would get its own copy. Define it in the owning module's .cpp and expose an accessor function"));
            }
        }

        return failures;
    }

    private const string PublicHeaderTypeTagRule = "public-header-type-tag";

    // 타입 키를 두는 한 곳이다(Core/TypeKey.h — 선언한 export 키가 없는 타입의 inline 표지).
    private const string TypeKeyHeader = "Core/Source/Runtime/Core/Public/Core/TypeKey.h";

    // 변수 템플릿 선언이다: template <...> inline [constexpr|constinit] [const] 타입 이름 = | { | ;
    private static readonly Regex VariableTemplateDeclaration = new(
        @"\btemplate\s*<[^;{}()]*>\s*inline\s+(?:constexpr\s+|constinit\s+)?(?:const\s+)?[\w:]+\s+(?<name>\w+)\s*(?:=|\{|;)",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10));

    /// <summary>
    /// 엔진 소유 모듈의 공개 헤더에서 변수 템플릿의 주소를 타입 표지로 쓰는 곳을 찾는다(Docs/Design/BuildTargets.ko.md 3.8절 BD29·BS8e, 규칙
    /// <c>public-header-type-tag</c>). 같은 헤더에 <c>template &lt;class T&gt; inline … Name = …;</c> 선언과 그 주소 <c>&amp;Name&lt;</c>가 함께
    /// 있으면 선언 줄을 알린다. inline 변수는 Modular(모듈마다 공유 라이브러리)에서 이미지마다 따로 생겨 모듈 사이에 키가 갈라지고(조회가 조용히
    /// 실패한다), const면 Monolithic에서도 링커 동일 데이터 병합(ICF)이 서로 다른 타입의 주소를 합칠 수 있다. 타입 키는 Core/TypeKey.h의
    /// <c>TypeKeyOf&lt;T&gt;()</c>를 쓴다(그 헤더만 예외). BS8c의 공개 헤더 검사(<c>-Wunique-object-duplication</c>)는 템플릿 문맥을 보지 못해 이
    /// 어휘 검사가 그 자리를 맡는다. 주석·리터럴은 보지 않는다. 사용 예: <c>PolicyChecks.PublicHeaderTypeTags(root)</c>.
    /// </summary>
    public static IReadOnlyList<CheckFailure> PublicHeaderTypeTags(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (!relative.EndsWith(".h", StringComparison.Ordinal) || !IsPublicModuleHeader(relative) || relative == TypeKeyHeader)
            {
                continue;
            }

            string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            foreach (Match match in VariableTemplateDeclaration.Matches(code))
            {
                string name = match.Groups["name"].Value;
                if (!Regex.IsMatch(code, @"&\s*(?:[\w:]+::)?" + Regex.Escape(name) + @"\s*<", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10)))
                {
                    continue;
                }

                int line = 1 + code.AsSpan(0, match.Groups["name"].Index).Count('\n');
                failures.Add(new(relative, line, PublicHeaderTypeTagRule,
                    $"the address of variable template '{name}' is used as a type tag in a public header: each shared library gets its own " +
                    "copy (Modular) and a const tag can be merged by identical data folding. Use TypeKeyOf<T>() from <Core/TypeKey.h> and " +
                    "declare ROCKS_DECLARE_NATIVE_TYPE_KEY for a type used across modules"));
            }
        }

        return failures;
    }

    // 엔진 소유 모듈의 공개 헤더인가: <루트>/Source/<그룹>/<모듈>/Public/… (그룹이 ThirdParty면 아니다) 또는 엔진 플러그인 모듈의 Public/ 아래.
    private static bool IsPublicModuleHeader(string relative)
    {
        string[] parts = relative.Split('/');
        return (parts.Length > 5 && parts[1] == "Source" && parts[2] != "ThirdParty" && parts[4] == "Public") ||
               IsPluginModuleSubfolder(parts, "Public");
    }
}
