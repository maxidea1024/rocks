using System.Text.RegularExpressions;

namespace Rocks.Tooling.Common;

// 소스 정책과 MetaCompiler의 실제 출력 경계가 같은 내장 형질 목록을 쓴다. 현재 사용 중인 이름만
// 열거하지 않고 형질 계열을 검사하여 새 내장을 호출부에 직접 추가하는 것도 막는다.
public static class CompilerTypeTraits
{
    private const string TypeTraitPattern =
        "\\b__(?!is_identifier\\b)(?:(?:is|remove|add|make|reference)_[A-Za-z0-9_]+|" +
        "has_(?:(?:trivial|nothrow)_[A-Za-z0-9_]+|virtual_destructor|unique_object_representations)|" +
        "array_(?:rank|extent)|underlying_type|decay|type_pack_element)\\b";

    private static readonly Regex TypeTrait = new(TypeTraitPattern,
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    // 출력에는 사용자가 지정한 표시 이름·진단 문자열도 있다. 그 문자열이나 주석의 철자를
    // 내장 호출로 오인하지 않도록 C++ 문자열(원시 문자열 포함)·문자 리터럴·주석을 먼저 소비한다.
    private static readonly Regex GeneratedToken = new(
        "(?<ignored>^[\\t ]*#[\\t ]*include(?:_next)?[\\t ]*<[^>\\r\\n]+>|" +
        "//[^\\r\\n]*|/\\*[\\s\\S]*?\\*/|\\b[0-9](?:[A-Za-z0-9_.]|'[A-Za-z0-9_])*|" +
        "(?:u8|u|U|L)?R\"(?<delimiter>[^\\s()\\\\]{0,16})\\([\\s\\S]*?\\)\\k<delimiter>\"|" +
        "\"(?:\\\\[\\s\\S]|[^\"\\\\])*\"|'(?:\\\\[\\s\\S]|[^'\\\\])*')|" +
        "(?<builtin>" + TypeTraitPattern +
        "|\\b__(?:builtin|atomic|sync|c11_atomic)_[A-Za-z0-9_]+\\b|\\b__ATOMIC_[A-Z_]+\\b)",
        RegexOptions.CultureInvariant | RegexOptions.Multiline, TimeSpan.FromSeconds(1));

    // 주석과 문자열도 검사한다. C#에서는 생성 코드 문자열이 바로 검사 대상이므로 지우지 않는다.
    public static string? FirstUse(string source)
    {
        Match match = TypeTrait.Match(source);
        return match.Success ? match.Value : null;
    }

    // 생성 C++에는 내장 형질뿐 아니라 메모리·원자 연산 등도 목적별 공개 helper로만 쓴다.
    // 기능 검사 매크로는 함수/형질 내장이 아니므로 두 검사 모두 허용한다.
    public static string? FirstGeneratedBuiltinUse(string source)
    {
        foreach (Match match in GeneratedToken.Matches(source))
        {
            if (match.Groups["builtin"].Success)
            {
                return match.Value;
            }
        }

        return null;
    }
}
