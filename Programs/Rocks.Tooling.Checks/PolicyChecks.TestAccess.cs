using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 모듈 내부 시험 접근점 헤더를 include하는 지시문이다(예: <Crypto/Private/AeadSequenceTestAccess.h>, #include_next·Objective-C
    // #import 포함). 경로가 리터럴이므로 주석만 지운 전체 텍스트에 맞추고, # 자체가 리터럴 밖인지 따로 확인한다.
    private static readonly Regex TestAccessInclude = Pattern(
        "(?m:^)[ \\t]*#\\s*(?:include|include_next|import)\\s*[<\"][^>\"\\n]*TestAccess\\.h[>\"]");

    // 시험 실행 파일 규칙(FoundationTests.Build.cs)이 자기 소스에만 정의하는 매크로다.
    private static readonly Regex TestingMacro = Pattern("\\bROCKS_TESTING\\b");

    // 프로세스 전역 상태를 바꿔 두는 libsodium 설정 함수다(난수 구현·misuse handler). Crypto 초기화만 부른다.
    private static readonly Regex SodiumGlobalHook = Pattern("\\b(?:randombytes_set_implementation|sodium_set_misuse_handler)\\b");

    // libsodium 전역 설정을 하는 유일한 곳이다(난수를 OS 난수로, misuse를 Fatal로 연결한다).
    private const string SodiumRuntimeSource = "Core/Source/Runtime/Crypto/Private/Sodium/SodiumRuntime.cpp";

    // 시험 실행 파일에 소스로 들어가는 시험 지원 코드다(ROCKS_TESTING이 정의된다).
    private const string TestSupportRoot = "Core/Source/Developer/TestSupport/";

    // 경로에서 처음 나오는 Public·Private 디렉터리가 Private인가(Public/Crypto/Private/처럼 공개 include 경로 안의 Private는
    // 아니다).
    private static bool IsUnderPrivateDirectory(string relative)
    {
        foreach (string part in relative.Split('/'))
        {
            if (part == "Public")
            {
                return false;
            }

            if (part == "Private")
            {
                return true;
            }
        }

        return false;
    }

    /// <summary>
    /// 시험 전용 길이 출하 코드에 남지 않게 한다(Docs/Design/Crypto.ko.md 11절 결정성). 모듈 오브젝트는 시험 실행 파일과 제품이
    /// 같이 쓰므로(모듈을 시험용으로 따로 컴파일하지 않는다) 모듈 안에 둔 hook은 그대로 출하된다. 그래서 바꿔 둘 수 있는 전역 hook을
    /// 두지 않는다. 고정 입력(salt·nonce·CPU 기능)은 값을 인자로 받는 내부 함수(모듈 구현과 시험이 함께 include하는 일반 Private
    /// 내부 헤더)로, 공개 계약이 막은 상태 이동(순번 이어 가기 등)은 모듈 Private/의 header-only friend 접근점 <c>*TestAccess.h</c>로
    /// 시험한다. 규칙 id는 넷이다.
    /// <list type="bullet">
    /// <item>test-access-include: 시험 소스(<see cref="IsTestSource"/>: 모듈 Tests/·Programs/FoundationTests/) 밖의 소스가
    /// <c>*TestAccess.h</c>를 include한다(접근점끼리는 예외). 모듈 Private/Tests/는 모듈에 컴파일되므로 시험 소스가 아니다.</item>
    /// <item>test-access-location: <c>*TestAccess.h</c>가 모듈 Private/ 밖(Public/ 등 공개 include 경로)에 있다.</item>
    /// <item>testing-macro: <c>ROCKS_TESTING</c>을 시험 소스·TestSupport 밖에서 쓴다. 모듈 .cpp에서는 늘 정의되지 않아 그 매크로로
    /// 감싼 hook은 시험에서도 꺼지고 반대 분기는 출하된다. 모듈 헤더에서는 시험 번역 단위만 정의되어 클래스 배치·inline 정의가
    /// 번역 단위마다 달라진다(ODR 위반).</item>
    /// <item>global-hook: libsodium 전역 설정 함수(<c>randombytes_set_implementation</c>·<c>sodium_set_misuse_handler</c>)를
    /// Crypto 초기화(Private/Sodium/SodiumRuntime.cpp) 밖에서 부른다. 시험이 난수 구현을 바꿔 두면 다른 시험·스레드의 키·nonce가
    /// 결정적이 된다.</item>
    /// </list>
    /// lexical 검사의 한계: 매크로로 만든 include 경로(<c>#include MACRO</c>), 공개 헤더가 friend로 선언한 접근점 이름을 제품 코드가
    /// 직접 정의하는 형태, 일반 내부 헤더에 둔 가변 전역은 잡지 않는다(의도적 우회이거나 설계 규칙이므로 리뷰가 맡는다).
    /// </summary>
    public static IReadOnlyList<CheckFailure> TestAccess(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            bool testSource = IsTestSource(relative);
            bool accessHeader = relative.EndsWith("TestAccess.h", StringComparison.Ordinal);
            string text = File.ReadAllText(path, StrictUtf8);
            string code = WithoutCommentsAndLiterals(text);
            if (accessHeader && !IsUnderPrivateDirectory(relative))
            {
                failures.Add(new(relative, 0, "test-access-location",
                    "*TestAccess.h test access point must live in a module Private/ directory"));
            }

            if (!testSource && !accessHeader)
            {
                foreach (Match match in TestAccessInclude.Matches(WithoutComments(text)))
                {
                    int hash = match.Index + match.Value.IndexOf('#');
                    if (code[hash] == '#')
                    {
                        failures.Add(new(relative, LineAt(code, hash), "test-access-include",
                            "*TestAccess.h is a test-only access point; include it only from test sources (module Tests/)"));
                    }
                }
            }

            string[] lines = code.Split('\n');
            for (int index = 0; index < lines.Length; ++index)
            {
                if (!testSource && !relative.StartsWith(TestSupportRoot, StringComparison.Ordinal) &&
                    TestingMacro.IsMatch(lines[index]))
                {
                    failures.Add(new(relative, index + 1, "testing-macro",
                        "ROCKS_TESTING is defined only for test executable sources; module code is shared with shipping " +
                        "builds (take the value as an internal function argument or use a *TestAccess.h access point)"));
                }

                if (relative != SodiumRuntimeSource && SodiumGlobalHook.IsMatch(lines[index]))
                {
                    failures.Add(new(relative, index + 1, "global-hook",
                        "libsodium process-global setting outside Crypto/Private/Sodium/SodiumRuntime.cpp"));
                }
            }
        }

        return failures;
    }
}
