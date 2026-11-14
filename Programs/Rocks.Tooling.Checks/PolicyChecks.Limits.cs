using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 정수 형식 이름(엔진 별칭·C++ 표준)이다. 외부 API 형식(DWORD·z_size_t 등)은 넣지 않는다. 그 API가 문서로 정한
    // 값((DWORD)-1 등)은 API 철자 그대로 쓴다.
    private const string UnsignedTypeName =
        "(?:(?:rocks\\s*::\\s*)?(?:usize|uptr|uint8|uint16|uint32|uint64)|size_t|uintptr_t|uint(?:8|16|32|64)_t" +
        "|unsigned(?:\\s+(?:char|short|int|long(?:\\s+long)?))?)";

    private const string IntegerSuffix = "(?:[uU](?:ll|LL|l|L)?|(?:ll|LL|l|L)[uU]?)?";

    // 어떤 문맥이든 형식 한계를 날 철자로 쓴 것이다: C 한계 매크로(<stdint.h>·<limits.h>·<float.h>), 부호 없는 형식의 -1
    // 변환, 0의 비트 반전. 모두 NumericLimits<T>·MaxSize·IndexNone·영역의 이름 있는 상수로 바꾼다.
    private static readonly Regex RawLimitSpelling = Pattern(
        "\\b(?:U?INT(?:8|16|32|64|PTR|MAX)_(?:MAX|MIN)|S?SIZE_MAX|PTRDIFF_(?:MAX|MIN)|WCHAR_(?:MAX|MIN)" +
        "|[SU]?CHAR_(?:MAX|MIN)|U?SHRT_(?:MAX|MIN)|U?INT_(?:MAX|MIN)|U?LONG_(?:MAX|MIN)|U?LLONG_(?:MAX|MIN)" +
        "|(?:FLT|DBL|LDBL)_(?:MAX|MIN|TRUE_MIN))\\b" +
        "|\\bstatic_cast\\s*<\\s*" + UnsignedTypeName + "\\s*>\\s*\\(\\s*-\\s*1\\s*\\)" +
        "|\\b(?:usize|uptr|uint8|uint16|uint32|uint64|size_t)\\s*[({]\\s*-\\s*1\\s*[)}]" +
        // C 스타일 변환이다. 이름 뒤의 괄호(sizeof(uint32) - 1·alignof(usize) - 1)는 변환이 아니다.
        "|(?<![A-Za-z0-9_]\\s*)\\(\\s*" + UnsignedTypeName + "\\s*\\)\\s*-\\s*1\\b" +
        "|~\\s*(?:0" + IntegerSuffix + "\\b(?![.xX])" +
        "|(?:usize|uptr|uint8|uint16|uint32|uint64|size_t|unsigned)\\s*[({]\\s*0?\\s*[)}]" +
        "|static_cast\\s*<\\s*" + UnsignedTypeName + "\\s*>\\s*\\(\\s*0\\s*\\))");

    // 형식 최대값과 같은 정수 리터럴이다(32·64-bit 모든 bit, 부호 있는 최대값, 10진 철자). 비트 마스크로 쓰는 일이
    // 흔하므로 &·|·^ 연산자의 피연산자이면 허용한다. 실수 리터럴(2147483647.0 등)은 대상이 아니다.
    private static readonly Regex RawLimitValue = Pattern(
        "\\b(?:0[xX](?:[fF]{16}|[fF]{8}|7[fF]{15}|7[fF]{7})" +
        "|(?:18446744073709551615|9223372036854775807|4294967295|2147483647))" + IntegerSuffix + "\\b(?![.eEpP])");

    // 값 자체가 의미인 상수(비트 마스크·형식 정의값)가 연산자 피연산자가 아닌 자리에 오는 지점이다. 파일과 철자를 함께
    // 지정한다. 같은 파일의 다른 날 한계 철자는 여전히 위반이다.
    private static readonly Dictionary<string, Regex> RawLimitAllowances = new(StringComparer.Ordinal)
    {
        // float 부호 bit를 지우는 SIMD 마스크다(lane마다 0x7fffffff).
        ["Core/Source/Runtime/PlatformBase/Public/Platform/Math/VectorRegisterSse.h"] = Pattern(
            "\\b_mm_set1_epi32\\s*\\(\\s*0x7fffffff\\s*\\)"),
    };

    /// <summary>
    /// 런타임·도구 C++ 소스(Core·Engine·Programs·Templates, 모듈 <c>Tests/</c>와 테스트 runner 제외)는 형식 한계를 날
    /// 숫자로 쓰지 않는다(rule id raw-limit-literal, Docs/CodingStyle.md). 최대·최소 검사는 Base/NumericLimits.h의
    /// <c>NumericLimits&lt;T&gt;::Max()</c>·<c>Min()</c>(usize는 <c>MaxSize</c>), "없음" 표지는 <c>IndexNone</c>이나 영역의
    /// 이름 있는 상수로 쓴다.
    /// <list type="bullet">
    /// <item>항상 위반: C 한계 매크로(UINT32_MAX·SIZE_MAX·INT_MAX·FLT_MAX 등), 부호 없는 형식의 -1 변환
    /// (<c>static_cast&lt;usize&gt;(-1)</c>·<c>uint32(-1)</c>), 0의 비트 반전(<c>~0u</c>·<c>~usize(0)</c>).</item>
    /// <item>연산자 피연산자가 아니면 위반: 0xffffffff·0xffffffffffffffff·0x7fffffff·0x7fffffffffffffff와 같은 값의 10진
    /// 철자. <c>&amp;</c>·<c>|</c>·<c>^</c>(대입 포함)의 피연산자(비트 마스크)는 허용한다.</item>
    /// <item>그 밖의 값 자체가 의미인 지점(연산자 밖의 마스크 등)은 <see cref="RawLimitAllowances"/>에 파일과 철자로
    /// 등록한다. 외부 API 형식의 문서화된 값((DWORD)-1 등)·해시 초기값·작은 폭 마스크(0xffff·0xff)는 대상이 아니다.</item>
    /// </list>
    /// 주석·문자열 안의 철자는 보지 않는다. 외부 라이브러리 원본은 <see cref="NativeSources"/>가 제외한다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> RawLimitLiterals(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (IsTestSource(relative))
            {
                continue;
            }

            RawLimitAllowances.TryGetValue(relative, out Regex? allowance);
            string[] lines = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8)).Split('\n');
            for (int index = 0; index < lines.Length; ++index)
            {
                string line = allowance is null ? lines[index] : allowance.Replace(lines[index], " ");
                if (RawLimitSpelling.IsMatch(line) ||
                    RawLimitValue.Matches(line).Any(match => !IsBitOperand(line, match)))
                {
                    failures.Add(new(relative, index + 1, "raw-limit-literal",
                        "raw type-limit literal (use NumericLimits<T>::Max()/Min(), MaxSize, IndexNone or a named sentinel)"));
                }
            }
        }

        return failures;
    }

    // 리터럴이 &·|·^(복합 대입 포함)의 피연산자인지다. &&·||는 논리 연산자이므로 아니다. 괄호는 건너뛰지 않는다.
    private static bool IsBitOperand(string line, Match match)
    {
        int before = match.Index - 1;
        while (before >= 0 && char.IsWhiteSpace(line[before]))
        {
            --before;
        }

        if (before >= 0 && line[before] == '=' && before > 0 && line[before - 1] is '&' or '|' or '^')
        {
            return true;
        }

        if (before >= 0 && line[before] is '&' or '|' or '^' &&
            !(before > 0 && line[before - 1] == line[before] && line[before] != '^'))
        {
            return true;
        }

        int after = match.Index + match.Length;
        while (after < line.Length && char.IsWhiteSpace(line[after]))
        {
            ++after;
        }

        return after < line.Length && line[after] is '&' or '|' or '^' &&
               !(after + 1 < line.Length && line[after + 1] == line[after] && line[after] != '^');
    }
}
