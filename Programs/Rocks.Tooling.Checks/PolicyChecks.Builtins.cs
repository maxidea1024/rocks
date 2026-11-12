using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 컴파일러 builtin 함수와 원자 builtin·순서 상수다. __has_builtin 같은 기능 검사 매크로는 대상이 아니다.
    private static readonly Regex CompilerBuiltin = Pattern(
        "\\b__(?:builtin|atomic|sync|c11_atomic)_[A-Za-z0-9_]*|\\b__ATOMIC_[A-Z_]+\\b");

    // builtin을 감싸는 지정 헤더와 그 헤더가 맡은 계열이다. 같은 줄의 다른 계열은 여전히 위반이다.
    private static readonly Dictionary<string, Regex> BuiltinHelperAllowances = new(StringComparer.Ordinal)
    {
        ["Core/Source/Runtime/Base/Public/Base/Compiler.h"] = Pattern(
            "\\b__builtin_(?:debugtrap|unreachable|offsetof)\\b"),
        ["Core/Source/Runtime/Base/Public/Base/Checked.h"] = Pattern(
            "\\b__builtin_(?:add|sub|mul)_overflow\\b"),
        ["Core/Source/Runtime/Base/Public/Base/Endian.h"] = Pattern(
            "\\b__builtin_bswap(?:16|32|64)\\b"),
        ["Core/Source/Runtime/PlatformBase/Public/Platform/Memory.h"] = Pattern(
            "\\b__builtin_(?:memcpy|memmove|memset|memcmp|strlen|strcmp|bit_cast|launder|addressof)\\b"),
        ["Core/Source/Runtime/PlatformBase/Public/Platform/Atomics.h"] = Pattern(
            "\\b__atomic_[A-Za-z0-9_]*|\\b__ATOMIC_[A-Z_]+\\b"),
        // 수치 intrinsic(비트 계수·특수 값)은 PlatformMath 계층이 컴파일러별로 감싼다.
        ["Core/Source/Runtime/PlatformBase/Public/Platform/Math/ClangPlatformMath.h"] = Pattern(
            "\\b__builtin_[A-Za-z0-9_]*"),
        ["Core/Source/Runtime/PlatformBase/Public/Platform/Math/GenericPlatformMath.h"] = Pattern(
            "\\b__builtin_[A-Za-z0-9_]*"),
    };

    /// <summary>
    /// 컴파일러 builtin(<c>__builtin_*</c>, <c>__atomic_*</c>, <c>__sync_*</c>, <c>__ATOMIC_*</c>)은 계열별 지정 헤더만
    /// 직접 쓴다. 나머지 소스(테스트 포함)는 그 헤더의 함수를 쓴다(rule id compiler-builtin).
    /// <list type="bullet">
    /// <item>byte 범위·C 문자열·객체 표현(memcpy·strlen·bit_cast·launder·addressof): Platform/Memory.h</item>
    /// <item>원자 연산·메모리 순서: Platform/Atomics.h</item>
    /// <item>overflow 검사 산술: Base/Checked.h. byte 순서: Base/Endian.h</item>
    /// <item>중단점·도달 불가·offsetof: Base/Compiler.h</item>
    /// <item>비트 계수·NaN·무한대: Platform/Math/{Clang,Generic}PlatformMath.h</item>
    /// </list>
    /// 외부 라이브러리 원본은 <see cref="NativeSources"/>가 제외한다. 주석의 언급도 lexical하게 검사한다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> CompilerBuiltins(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            BuiltinHelperAllowances.TryGetValue(relative, out Regex? allowance);
            int line = 0;
            foreach (string text in File.ReadLines(path, StrictUtf8))
            {
                ++line;
                string checkedText = allowance is null ? text : allowance.Replace(text, " ");
                if (CompilerBuiltin.IsMatch(checkedText))
                    failures.Add(new(relative, line, "compiler-builtin",
                        "compiler builtin outside its helper header (Platform/Memory.h, Platform/Atomics.h, " +
                        "Base/Checked.h, Base/Endian.h, Base/Compiler.h, PlatformMath)"));
            }
        }
        return failures;
    }
}
