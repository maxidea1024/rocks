using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 컴파일러·플랫폼 SDK가 정하는 대상 CPU·OS 원시 매크로다(변형 철자·32 bit·Apple·빌드가 넣는 WIN32·_WINDOWS 포함).
    // Compiler.h 밖에서는 정당한 용도가 없으므로 전처리기 분기만이 아니라 주석·리터럴을 지운 코드의 어느 토큰이든 거부한다.
    // 그래서 줄 이음(\)으로 이어진 #if, C++23 #elifdef·#elifndef, #define 별칭(#define IS_APPLE __APPLE__)도 잡힌다.
    private static readonly Regex RawTargetMacro = Pattern(
        "\\b(?:__x86_64__|__x86_64|__amd64__|__amd64|_M_X64|_M_AMD64|__aarch64__|__arm64__|__arm64|_M_ARM64|" +
        "__i386__|__i386|_M_IX86|__arm__|_M_ARM|_WIN32|_WIN64|WIN32|_WINDOWS|__APPLE__|__MACH__|TARGET_OS_[A-Z0-9_]+|" +
        "__ANDROID__|__linux__|__linux|__unix__|__unix)\\b");

    // 빌드·Compiler.h가 항상 0 또는 1로 정의하는 매크로를 정의 여부로 묻는 형태다. 언제나 참이므로 값으로 분기해야 한다
    // (#ifdef ROCKS_PLATFORM_MAC은 모든 플랫폼에서 참이다).
    private static readonly Regex DefinedRocksTargetMacro = Pattern(
        "\\bdefined\\s*\\(?\\s*ROCKS_(?:BUILD|PLATFORM|ARCH)_[A-Z0-9_]*|" +
        "#\\s*(?:ifdef|ifndef|elifdef|elifndef)\\s+ROCKS_(?:BUILD|PLATFORM|ARCH)_[A-Z0-9_]*");

    // Compiler.h가 정의하는 ROCKS_ARCH_*·ROCKS_PLATFORM_*를 쓰는 전처리기 줄이다(줄 이음을 합친 논리 줄 기준).
    private static readonly Regex RocksTargetMacroUse = Pattern("\\bROCKS_(?:ARCH|PLATFORM)_[A-Z0-9_]+\\b");

    private static readonly Regex CompilerHeaderInclude = Pattern("^\\s*#\\s*include\\s*<Base/Compiler\\.h>");

    // 원시 매크로를 해석해 ROCKS_ARCH_*·ROCKS_PLATFORM_*를 정의·검증하는 유일한 헤더다.
    private const string TargetMacroDefinitionHeader = "Core/Source/Runtime/Base/Public/Base/Compiler.h";

    /// <summary>
    /// 대상 CPU·OS 판별은 <c>Base/Compiler.h</c>가 한 번만 한다. 다른 소스(테스트·native backend 포함)는 전처리기 분기에
    /// <c>ROCKS_ARCH_X64</c>·<c>ROCKS_ARCH_ARM64</c>·<c>ROCKS_PLATFORM_*</c>를, 함수 안에서는 <c>rocks::IsX64</c>·<c>IsArm64</c>·
    /// <c>IsWindows</c>·<c>IsMac</c>·<c>IsAndroid</c>를 쓴다. 규칙 id는 셋이다.
    /// <list type="bullet">
    /// <item>target-macro: 원시 대상 매크로 토큰(주석·리터럴 제외)이 Compiler.h 밖에 있다.</item>
    /// <item>target-macro-defined: ROCKS_BUILD_*·ROCKS_PLATFORM_*·ROCKS_ARCH_*를 정의 여부(<c>#ifdef</c>·<c>defined()</c>)로 묻는다.
    /// 이 매크로는 항상 0 또는 1로 정의되므로 값으로 분기한다.</item>
    /// <item>target-macro-order: ROCKS_ARCH_*·ROCKS_PLATFORM_*를 쓰는 전처리기 줄보다 앞에 <c>#include &lt;Base/Compiler.h&gt;</c>가
    /// 없다. 정의 전에 쓰면 경고 없이 0이 되기 때문이다(-Wundef를 켜지 않는다). 다른 헤더를 거친 간접 포함은 인정하지 않는다.</item>
    /// </list>
    /// OS API 분기를 native backend 안에만 두는 규칙은 <see cref="PlatformBoundary"/>가 따로 검사한다. 외부 라이브러리 원본은
    /// <see cref="NativeSources"/>가 제외한다. 소유 C 파일(.c, 예: Lz4Unity.c)은 C++ 전용 Compiler.h를 쓸 수 없으므로 대상별
    /// 분기를 두지 않는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> TargetMacros(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            if (relative == TargetMacroDefinitionHeader)
            {
                continue;
            }

            string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            string[] lines = code.Split('\n');
            int? compilerInclude = null;
            bool reportedOrder = false;
            for (int index = 0; index < lines.Length; ++index)
            {
                if (RawTargetMacro.IsMatch(lines[index]))
                {
                    failures.Add(new(relative, index + 1, "target-macro",
                        "raw compiler target macro outside Base/Compiler.h (use ROCKS_ARCH_*/ROCKS_PLATFORM_* or rocks::Is*)"));
                }

                compilerInclude ??= CompilerHeaderInclude.IsMatch(lines[index]) ? index : null;
            }

            // 전처리기 지시문은 줄 이음을 합친 논리 줄로 본다(보고는 시작 줄).
            for (int index = 0; index < lines.Length; ++index)
            {
                int start = index;
                string logical = lines[index];
                while (logical.EndsWith('\\') && index + 1 < lines.Length)
                {
                    logical = logical[..^1] + " " + lines[++index];
                }

                if (!logical.TrimStart().StartsWith('#'))
                {
                    continue;
                }

                if (DefinedRocksTargetMacro.IsMatch(logical))
                {
                    failures.Add(new(relative, start + 1, "target-macro-defined",
                        "ROCKS_BUILD_*/ROCKS_PLATFORM_*/ROCKS_ARCH_* are always defined to 0 or 1; branch on the value, not on defined()"));
                }

                if (!reportedOrder && RocksTargetMacroUse.IsMatch(logical) && (compilerInclude is null || compilerInclude > start))
                {
                    reportedOrder = true;
                    failures.Add(new(relative, start + 1, "target-macro-order",
                        "#include <Base/Compiler.h> must come before the first ROCKS_ARCH_*/ROCKS_PLATFORM_* directive (undefined macros silently read as 0)"));
                }
            }
        }

        return failures;
    }
}
