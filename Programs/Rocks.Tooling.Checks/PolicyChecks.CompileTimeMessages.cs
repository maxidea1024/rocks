using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 줄 시작의 #error 지시문이다. 줄 이음(\)으로 이어진 다음 줄까지 문구에 넣는다.
    private static readonly Regex ErrorDirective = new(
        "^[ \\t]*#[ \\t]*error\\b(?<text>(?:[^\\n\\\\]|\\\\\\r?\\n|\\\\)*)",
        RegexOptions.Multiline | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private static readonly Regex StaticAssertStart = Pattern("\\bstatic_assert\\s*\\(");

    // 한글 자모·호환 자모·완성형 음절이다.
    private static readonly Regex Hangul = Pattern("[\\u1100-\\u11FF\\u3130-\\u318F\\uAC00-\\uD7A3]");

    /// <summary>
    /// 엔진 소유 C++ 소스(Core·Engine·Programs·Templates, 시험 포함)의 컴파일 시점 오류 메시지는 영어로 쓴다(rule id
    /// compile-time-message-english, Docs/CodingStyle.md). 대상은 <c>static_assert</c>의 메시지 문자열과 <c>#error</c>
    /// 지시문의 문구다. 주석은 보지 않으므로 한국어 주석은 그대로 쓴다. 외부 라이브러리 원본은
    /// <see cref="NativeSources"/>가 제외한다. 생성 코드의 static_assert는 MetaCompiler가 영어로 내고 MetaTests가 컴파일한다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> CompileTimeMessages(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            string code = WithoutComments(File.ReadAllText(path, StrictUtf8));
            foreach (Match match in ErrorDirective.Matches(code))
            {
                if (Hangul.IsMatch(match.Groups["text"].Value))
                {
                    failures.Add(CompileTimeMessageFailure(relative, code, match.Index, "#error"));
                }
            }

            foreach (Match match in StaticAssertStart.Matches(code))
            {
                int end = MatchingParenthesis(code, match.Index + match.Length - 1);
                string statement = code[match.Index..end];
                if (StringLiteral.Matches(statement).Any(literal => Hangul.IsMatch(literal.Groups[1].Value)))
                {
                    failures.Add(CompileTimeMessageFailure(relative, code, match.Index, "static_assert"));
                }
            }
        }

        return failures;
    }

    private static CheckFailure CompileTimeMessageFailure(string relative, string code, int index, string kind) =>
        new(relative, code.AsSpan(0, index).Count('\n') + 1, "compile-time-message-english",
            $"{kind} message must be English (comments may stay Korean)");

    // open은 '('의 위치다. 짝이 되는 ')' 다음 위치를 돌려주고, 짝이 없으면 끝까지다. 문자열·문자 리터럴 안의 괄호는 건너뛴다.
    private static int MatchingParenthesis(string code, int open)
    {
        int depth = 0;
        for (int index = open; index < code.Length; ++index)
        {
            char current = code[index];
            if (current is '"' or '\'')
            {
                for (++index; index < code.Length && code[index] != current && code[index] != '\n'; ++index)
                {
                    if (code[index] == '\\')
                    {
                        ++index;
                    }
                }
            }
            else if (current == '(')
            {
                ++depth;
            }
            else if (current == ')' && --depth == 0)
            {
                return index + 1;
            }
        }

        return code.Length;
    }
}
