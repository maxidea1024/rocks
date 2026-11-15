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

    /// <summary>
    /// 엔진 소유 C++ 소스(Core·Engine·Programs·Templates, 시험 포함)는 형식 추론이 <c>std::initializer_list</c>를 요구하는
    /// 문법을 쓰지 않는다(rule id initializer-list-deduction, Docs/CodingStyle.md).
    /// <list type="bullet">
    /// <item>range-for의 중괄호 목록 범위: <c>for (bool x : {true, false})</c>. 지역 배열
    /// (<c>const bool cases[] = {true, false}; for (bool x : cases)</c>)로 쓴다.</item>
    /// <item>auto 변수의 중괄호 목록 복사 초기화: <c>auto values = {1, 2};</c>. 배열이나 <c>Array</c>로 쓴다.</item>
    /// </list>
    /// 엔진은 STL 헤더를 쓰지 않으므로 Android(NDK) 빌드에는 <c>&lt;initializer_list&gt;</c>가 없어 이 문법이 컴파일되지
    /// 않는다. Windows에서는 시스템 헤더가 간접으로 들여와 통과하므로 플랫폼 빌드 전에 이 검사가 잡는다. 주석·문자열 안의
    /// 철자는 보지 않는다. 외부 라이브러리 원본은 <see cref="NativeSources"/>가 제외한다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> InitializerListDeductions(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            string[] lines = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8)).Split('\n');
            for (int index = 0; index < lines.Length; ++index)
            {
                if (BracedRangeFor.IsMatch(lines[index]) || AutoBracedCopyInit.IsMatch(lines[index]))
                {
                    failures.Add(new(relative, index + 1, "initializer-list-deduction",
                        "braced list needs std::initializer_list, which engine builds do not have (use a local array)"));
                }
            }
        }

        return failures;
    }
}
