using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 컴파일러가 정하는 대상 CPU·OS 원시 매크로를 전처리기 분기에 쓰는 줄이다. ROCKS_ARCH_*·ROCKS_PLATFORM_*와
    // __ARM_FEATURE_* 같은 기능 매크로는 대상이 아니다.
    private static readonly Regex RawTargetMacro = Pattern(
        "#\\s*(?:if|ifdef|ifndef|elif)\\b[^\\n]*\\b(?:__x86_64__|__amd64__|_M_X64|_M_AMD64|__aarch64__|__arm64__|_M_ARM64|" +
        "_WIN32|_WIN64|__APPLE__|__ANDROID__|__linux__)\\b");

    // 원시 매크로를 해석해 ROCKS_ARCH_*·ROCKS_PLATFORM_*를 정의·검증하는 유일한 헤더다.
    private const string TargetMacroDefinitionHeader = "Core/Source/Runtime/Base/Public/Base/Compiler.h";

    /// <summary>
    /// 대상 CPU·OS 판별은 <c>Base/Compiler.h</c>가 한 번만 한다(rule id target-macro). 다른 소스(테스트·native
    /// backend 포함)는 전처리기 분기에 <c>ROCKS_ARCH_X64</c>·<c>ROCKS_ARCH_ARM64</c>·<c>ROCKS_PLATFORM_*</c>를, 함수
    /// 안에서는 <c>rocks::IsX64</c>·<c>IsArm64</c>·<c>IsWindows</c>·<c>IsMac</c>·<c>IsAndroid</c>를 쓴다. OS API 분기를
    /// native backend 안에만 두는 규칙은 <see cref="PlatformBoundary"/>가 따로 검사한다. 외부 라이브러리 원본은
    /// <see cref="NativeSources"/>가 제외한다.
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

            ScanFile(path, relative, RawTargetMacro, "target-macro",
                "raw compiler target macro outside Base/Compiler.h (use ROCKS_ARCH_*/ROCKS_PLATFORM_* or rocks::Is*)", failures);
        }

        return failures;
    }
}
