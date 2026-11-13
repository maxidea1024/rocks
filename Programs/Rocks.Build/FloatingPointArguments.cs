using System.Text.RegularExpressions;

namespace Rocks.Build;

/// <summary>
/// 부동소수점 결과를 바꾸는 컴파일러 인자(FP 모델 인자) 판정이다(Docs/Design/ReflectionFoundations.ko.md 8.1절). 툴체인 지문의
/// <c>fp:</c> 줄(<see cref="ToolchainFingerprint"/>)이 빌드 프로필에서 이 인자를 골라 쓰고, 모듈 인자 검사(InputValidation)가 모듈의
/// <c>AdditionalCompileArguments</c>·<c>AdditionalLinkArguments</c>에서 거부한다(결정 7 — FP 모델은 빌드 구성·모듈마다 달라지지
/// 않는다).
/// <para>
/// 사용 예: <c>FloatingPointArguments.IsModelArgument("-ffast-math")</c>는 true, <c>IsModelArgument("-fPIC")</c>는 false다.
/// </para>
/// </summary>
public static class FloatingPointArguments
{
    // -f[no-] 뒤에 오는 FP 의미 이름이다(정확히 같은 이름).
    private static readonly HashSet<string> NegatableNames = new(StringComparer.Ordinal)
    {
        "fast-math", "unsafe-math-optimizations", "associative-math", "reciprocal-math", "signed-zeros", "trapping-math",
        "finite-math-only", "math-errno", "approx-func", "rounding-math", "protect-parens", "strict-float-cast-overflow",
        "cx-limited-range", "cx-fortran-rules"
    };

    // -f 뒤의 접두로 판정하는 이름이다(-ffp-*, -fdenormal-fp-math*, -fexcess-precision* 등).
    private static readonly string[] PrefixNames =
    [
        "fp-", "honor-", "complex-arithmetic=", "denormal-fp-math", "excess-precision", "float16-excess-precision",
        "bfloat16-excess-precision", "veclib="
    ];

    // 배포 대상 인자(-mmacosx-version-min= 등)는 target triple에 들어가므로 모델 인자가 아니다.
    private static readonly Regex DeploymentTarget = new("^-m[a-z][a-z0-9-]*-version-min=",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// argument가 FP 모델 인자인지다. 판정은 대소문자를 구분한다.
    /// <list type="bullet">
    /// <item>CL 철자(<c>fp:</c>·<c>arch:</c>)는 <c>/</c>·<c>-</c> 두 접두 모두 받는다(<c>/fp:fast</c>·<c>-fp:fast</c>·<c>/arch:AVX2</c>).
    /// <c>/clang:</c>·<c>-clang:</c> 접두는 떼고 안의 인자를 판정한다(<c>-clang:-ffast-math</c>). GCC형 <c>-f</c>·<c>-m</c> 인자는 접두를 바꾸지
    /// 않는다.</item>
    /// <item>GCC형: <c>-ffp-*</c>, <c>-f[no-]fast-math</c>, <c>-f[no-]</c> + unsafe-math-optimizations·associative-math·reciprocal-math·
    /// signed-zeros·trapping-math·finite-math-only·honor-*·math-errno·approx-func·rounding-math·protect-parens·
    /// strict-float-cast-overflow·cx-limited-range·cx-fortran-rules, <c>-fcomplex-arithmetic=</c>, <c>-fdenormal-fp-math*</c>,
    /// <c>-fexcess-precision*</c>, <c>-f[b]float16-excess-precision*</c>, <c>-fveclib=</c>.</item>
    /// <item>ISA·FP 하드웨어 선택: <c>-m</c>로 시작하는 모든 인자(<c>-march=</c>·<c>-mcpu=</c>·<c>-mtune=</c>·<c>-mfpmath*</c>·<c>-mrecip*</c>·
    /// <c>-mdaz-ftz</c>·<c>-mlong-double-*</c>·<c>-mavx2</c>·<c>-mno-sse*</c> 등). 배포 대상 <c>-m&lt;os&gt;-version-min=</c>과 이름 뒤에
    /// ':'가 오는 링커·CL 철자(<c>-machine:x64</c>)는 제외한다.</item>
    /// </list>
    /// </summary>
    public static bool IsModelArgument(string argument)
    {
        ArgumentNullException.ThrowIfNull(argument);
        if (argument.Length > 1 && argument[0] is ('/' or '-'))
        {
            string body = argument[1..];
            if (body.StartsWith("fp:", StringComparison.Ordinal) || body.StartsWith("arch:", StringComparison.Ordinal))
            {
                return true;
            }

            if (body.StartsWith("clang:", StringComparison.Ordinal))
            {
                return IsDriverModelArgument(body["clang:".Length..]);
            }
        }

        return IsDriverModelArgument(argument);
    }

    private static bool IsDriverModelArgument(string argument)
    {
        if (argument.StartsWith("-m", StringComparison.Ordinal))
        {
            // 드라이버의 -m 옵션은 값을 '='로 받는다. 이름 뒤에 ':'가 먼저 오면 링커·CL 철자(lld-link -machine:x64)라 ISA 선택이 아니다.
            int colon = argument.IndexOf(':');
            int equals = argument.IndexOf('=');
            bool linkerSpelling = colon >= 0 && (equals < 0 || colon < equals);
            return !linkerSpelling && !DeploymentTarget.IsMatch(argument);
        }

        if (!argument.StartsWith("-f", StringComparison.Ordinal))
        {
            return false;
        }

        string name = argument[2..];
        string positive = name.StartsWith("no-", StringComparison.Ordinal) ? name[3..] : name;
        return NegatableNames.Contains(positive) ||
            PrefixNames.Any(prefix => name.StartsWith(prefix, StringComparison.Ordinal) || positive.StartsWith(prefix, StringComparison.Ordinal));
    }
}
