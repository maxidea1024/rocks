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

    // 엔진 소유 모듈의 공개 헤더인가: <루트>/Source/<그룹>/<모듈>/Public/… (그룹이 ThirdParty면 아니다) 또는 엔진 플러그인 모듈의 Public/ 아래.
    private static bool IsPublicModuleHeader(string relative)
    {
        string[] parts = relative.Split('/');
        return (parts.Length > 5 && parts[1] == "Source" && parts[2] != "ThirdParty" && parts[4] == "Public") ||
               IsPluginModuleSubfolder(parts, "Public");
    }
}
