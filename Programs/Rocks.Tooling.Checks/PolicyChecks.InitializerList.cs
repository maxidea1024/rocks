using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // range-for의 범위가 중괄호 목록인 자리다(for (bool x : {true, false})). 콜론 앞에 중괄호·세미콜론이 없어야 하므로
    // 일반 for 문과 같은 줄의 switch case 라벨은 걸리지 않는다.
    private static readonly Regex BracedRangeFor = Pattern("\\bfor\\s*\\([^;{}]*?:\\s*\\{");

    // auto 변수를 중괄호 목록으로 복사 초기화하는 자리다(auto x = {1, 2}). 직접 초기화 auto x{1}은 원소 하나의 형식으로
    // 추론되므로 대상이 아니다.
    private static readonly Regex AutoBracedCopyInit = Pattern("\\bauto\\s*[&*]{0,2}\\s*[A-Za-z_]\\w*\\s*=\\s*\\{");

    // std::initializer_list를 여는 래퍼 헤더다. <initializer_list>를 직접 include할 수 있는 유일한 파일이다.
    private const string InitializerListHeader = "Core/Source/Runtime/Base/Public/Base/InitializerList.h";

    // 래퍼를 include하는 철자와 표준 헤더 철자다.
    private const string InitializerListWrapperSpelling = "Base/InitializerList.h";

    private const string InitializerListStandardSpelling = "initializer_list";

    /// <summary>
    /// 엔진 소유 C++ 소스(Core·Engine·Programs·Templates, 시험 포함)의 중괄호 목록 추론 규칙이다(Docs/CodingStyle.md "중괄호 목록 추론").
    /// <list type="bullet">
    /// <item>rule id <c>initializer-list-deduction</c>: 형식 추론이 <c>std::initializer_list</c>를 요구하는 문법(range-for의 중괄호 목록
    /// 범위 <c>for (bool x : {true, false})</c>, auto 변수의 중괄호 목록 복사 초기화 <c>auto values = {1, 2};</c>)은
    /// <c>&lt;Base/InitializerList.h&gt;</c>를 직접 include한 파일에서만 쓸 수 있다. 엔진은 STL 헤더를 쓰지 않으므로 래퍼 없이는
    /// Android(NDK) 빌드에 <c>&lt;initializer_list&gt;</c>가 보이지 않아 컴파일되지 않는다. Windows에서는 시스템 헤더가 간접으로 들여와
    /// 통과하므로 플랫폼 빌드 전에 이 검사가 잡는다. 간접 include는 인정하지 않는다.</item>
    /// <item>rule id <c>initializer-list-header</c>: <c>&lt;initializer_list&gt;</c>는 래퍼만 include한다.</item>
    /// </list>
    /// 주석·문자열 안의 철자는 보지 않는다. 외부 라이브러리 원본은 <see cref="NativeSources"/>가 제외한다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> InitializerListDeductions(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            string text = File.ReadAllText(path, StrictUtf8);
            bool includesWrapper = false;
            foreach (IncludeLine include in IncludeLines(text))
            {
                if (include.Kind != IncludeKind.Angle && include.Kind != IncludeKind.Quote)
                {
                    continue;
                }

                if (string.Equals(include.Spelling, InitializerListWrapperSpelling, StringComparison.Ordinal))
                {
                    includesWrapper = true;
                }
                else if (relative != InitializerListHeader &&
                    string.Equals(include.Spelling, InitializerListStandardSpelling, StringComparison.OrdinalIgnoreCase))
                {
                    failures.Add(new(relative, include.Line, "initializer-list-header",
                        "include <Base/InitializerList.h> instead of <initializer_list>: the standard header is opened only by that wrapper"));
                }
            }

            if (includesWrapper)
            {
                continue;
            }

            string[] lines = WithoutCommentsAndLiterals(text).Split('\n');
            for (int index = 0; index < lines.Length; ++index)
            {
                if (BracedRangeFor.IsMatch(lines[index]) || AutoBracedCopyInit.IsMatch(lines[index]))
                {
                    failures.Add(new(relative, index + 1, "initializer-list-deduction",
                        "braced list needs std::initializer_list, which engine builds do not have without the wrapper " +
                        "(include <Base/InitializerList.h> in this file, or use a local array)"));
                }
            }
        }

        return failures;
    }
}
