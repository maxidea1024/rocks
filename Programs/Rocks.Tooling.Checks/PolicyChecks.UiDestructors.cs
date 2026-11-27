using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 소멸자 본문의 시작이다: `X::~X() noexcept {`(밖 정의)와 `~X() noexcept override {`(안 정의).
    private static readonly Regex UiDestructorStart = Pattern(
        @"(?:\b\w+\s*::\s*)?~\s*\w+\s*\(\s*\)\s*(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?\{");

    // 소멸자에서 부르면 안 되는 사용자 hook·사용자 콜백 호출이다(Docs/Design/UiToolkit 21.4절).
    private static readonly Regex UiDestructorUserCall = Pattern(
        @"\b(?:OnDestruct|OnDeactivated|ReleaseRow|ExecuteIfBound|Broadcast)\s*\(|(?:\.|->)\s*Execute\s*\(");

    // 대기열에 넣는 lambda다(CreateLambda·TryCreateLambda).
    private static readonly Regex UiDeferredLambda = Pattern(@"\b(?:Try)?CreateLambda\s*\(");

    private const string UiDestructorRule = "ui-destructor-user-code";

    /// <summary>
    /// UI 모듈 Element·Widget 소멸자에서 사용자 코드를 부르는 줄을 막는다(rule id ui-destructor-user-code, UiToolkit 21.4절).
    /// 소멸은 SharedPtr 마지막 참조가 사라지는 아무 곳에서나 일어나므로 사용자 hook·delegate는 명시 해제 경로나
    /// 해체 대기열(UiHostServices::DeferTeardown)에서 부른다. 대기열에 넣는 lambda 안의 호출은 소멸자에서 돌지
    /// 않으므로 CreateLambda 인자 안은 보지 않는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> UiDestructorUserCode(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in Files(root, UiElementCastRoots).Where(path =>
            path.EndsWith(".h", StringComparison.Ordinal) || path.EndsWith(".cpp", StringComparison.Ordinal)))
        {
            string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            foreach (Match start in UiDestructorStart.Matches(code))
            {
                int open = start.Index + start.Length - 1;
                int close = MatchingBrace(code, open);
                if (close < 0)
                {
                    continue;
                }

                string body = WithoutDeferredLambdas(code[(open + 1)..close]);
                foreach (Match call in UiDestructorUserCall.Matches(body))
                {
                    int line = 1 + code.Take(open + 1).Count(c => c == '\n') + body.Take(call.Index).Count(c => c == '\n');
                    failures.Add(new(Relative(root, path), line, UiDestructorRule,
                        $"destructor calls user code '{call.Value.TrimEnd('(').Trim()}'; call it on the explicit release path " +
                        "or queue it with DeferTeardown (Docs/Design/UiToolkit 21.4)"));
                }
            }
        }

        return failures;
    }

    private static int MatchingBrace(string code, int open)
    {
        int depth = 0;
        for (int i = open; i < code.Length; ++i)
        {
            if (code[i] == '{')
            {
                ++depth;
            }
            else if (code[i] == '}' && --depth == 0)
            {
                return i;
            }
        }

        return -1;
    }

    // CreateLambda(...) 인자를 같은 길이의 공백으로 바꾼다(줄 번호 유지). 그 안은 나중에 대기열에서 돈다.
    private static string WithoutDeferredLambdas(string body)
    {
        char[] text = body.ToCharArray();
        foreach (Match lambda in UiDeferredLambda.Matches(body))
        {
            int depth = 0;
            for (int i = lambda.Index + lambda.Length - 1; i < text.Length; ++i)
            {
                char c = body[i];
                if (c == '(')
                {
                    ++depth;
                }
                else if (c == ')' && --depth == 0)
                {
                    break;
                }

                if (c != '\n')
                {
                    text[i] = ' ';
                }
            }
        }

        return new string(text);
    }
}
