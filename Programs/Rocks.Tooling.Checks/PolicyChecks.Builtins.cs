using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

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
        // RemoveAtomicT는 평가되지 않는 load 식의 결과 형식으로 Clang의 원자 한정을 벗긴다.
        ["Core/Source/Runtime/Base/Public/Base/TypeTraits.h"] = Pattern(
            "\\b__c11_atomic_load\\b"),
        ["Core/Source/Runtime/PlatformBase/Public/Platform/Memory.h"] = Pattern(
            "\\b__builtin_(?:memcpy|memmove|memset|memcmp|strlen|strcmp|bit_cast|launder|addressof|prefetch)\\b"),
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
    /// <item>byte 범위·C 문자열·객체 표현·캐시 힌트(memcpy·strlen·bit_cast·launder·addressof·prefetch): Platform/Memory.h</item>
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
                {
                    failures.Add(new(relative, line, "compiler-builtin",
                        "compiler builtin outside its helper header (Platform/Memory.h, Platform/Atomics.h, " +
                        "Base/Checked.h, Base/Endian.h, Base/Compiler.h, PlatformMath)"));
                }
            }
        }

        failures.AddRange(CompilerTraits(root));
        return failures;
    }

    // 형질은 Base의 단일 구현 헤더만 직접 쓴다. C# 생성 문자열·시험의 C++ 조각도 같은 규칙이며
    // 테스트 파일 전체를 면제하지 않는다. 기존 NativeSources 소비자의 검사 범위는 바꾸지 않는다.
    private static IReadOnlyList<CheckFailure> CompilerTraits(string root)
    {
        const string helper = "Core/Source/Runtime/Base/Public/Base/TypeTraits.h";
        IReadOnlyList<string> upstream = UpstreamDirectories(root);
        List<CheckFailure> failures = [];
        foreach (string path in Files(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (relative == helper || upstream.Any(directory => relative.StartsWith(directory, StringComparison.Ordinal)))
            {
                continue;
            }

            if (Path.GetExtension(path).ToLowerInvariant() is not
                (".h" or ".hh" or ".hpp" or ".hxx" or ".c" or ".cc" or ".cpp" or ".cxx" or ".mm" or
                ".inl" or ".inc" or ".in" or ".cs"))
            {
                continue;
            }

            int line = 0;
            foreach (string text in File.ReadLines(path, StrictUtf8))
            {
                ++line;
                if (CompilerTypeTraits.FirstUse(text) is string intrinsic)
                {
                    failures.Add(new(relative, line, "compiler-type-trait",
                        $"{intrinsic} outside Base/TypeTraits.h; use the public rocks type traits"));
                }
            }
        }

        return failures;
    }
}
