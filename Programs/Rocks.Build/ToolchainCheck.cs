using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// 도구 모음 버전 확인이다(Docs/Design/ClangVersionSupport.ko.md 6.2절). 도구 모음을 찾을 때 한 번 부른다: 제조사별 최소 Clang 미만이면
/// 거부하고, Windows에서는 고른 MSVC STL(<c>yvals_core.h</c>)이 요구하는 최소 Clang을 첫 컴파일 전에 확인한다. 검증 목록 밖이면 거부하지
/// 않고 <see cref="NativeToolchain.ToolchainVerified"/>를 거짓으로 둔다(빌드 CLI가 한 줄 알리고 증거에 남기며, CI는
/// <c>--require-verified-toolchain on</c>으로 실패시킨다 — <see cref="Enforce"/>).
/// </summary>
public static class ToolchainCheck
{
    /// <summary>빌드 CLI 옵션 이름이다(<c>--require-verified-toolchain on|off</c>, 기본 off).</summary>
    public const string RequireVerifiedOption = "require-verified-toolchain";

    /// <summary>
    /// compilerVersion은 정규화한 <c>--version</c> 첫 줄(<see cref="NativeToolchain.CompilerVersion"/>), stlHeader는 Windows에서 고른 도구
    /// 모음의 <c>include/yvals_core.h</c> 본문(없으면 null)이다. 첫 줄 전체가 검증 목록에 있으면 참이다. 제조사별 최소 미만이거나 STL이
    /// 요구하는 Clang에 못 미치면 InvalidOperationException이다.
    /// </summary>
    public static bool Verify(NativePlatform platform, string compilerVersion, string? stlHeader)
    {
        string name = platform.ToString();
        if (ToolchainTable.ParseClangVersion(compilerVersion) is not (string version, int major, bool appleClang))
        {
            throw new InvalidOperationException($"Cannot read the Clang version from '{compilerVersion}'.");
        }

        if (ToolchainTable.MinimumClangMajor(name, appleClang) is int minimum && major < minimum)
        {
            throw new InvalidOperationException($"{(appleClang ? "Apple clang" : "Clang")} {version} is older than the minimum {(appleClang ? "Apple clang" : "Clang")} " +
                $"{minimum} for {name} (Build/Toolchain.json); install a newer toolchain (Windows: pwsh Build/InstallLlvm.ps1).");
        }

        if (stlHeader is not null && ToolchainTable.ParseStlClangMinimum(stlHeader) is int stl && major < stl)
        {
            throw new InvalidOperationException($"The selected MSVC STL requires Clang {stl} or newer (STL1000), but the compiler is Clang {version}; " +
                $"pick an older MSVC toolset with {NativeEnvironment.MsvcToolsetVariable} (for example 14.44) or install a newer LLVM.");
        }

        return ToolchainTable.VerifiedCompilers(name).Contains(compilerVersion.Trim(), StringComparer.Ordinal);
    }

    /// <summary>
    /// build-evidence <c>stlClangCheck</c>에 남길 STL 최소 Clang 확인 결과다. stlHeader가 있으면 요구한 최소(<c>requires Clang 22</c>)나
    /// 조건을 찾지 못한 사실을, 없으면 readProblem(읽지 못한 이유)을 돌려준다.
    /// </summary>
    public static string StlStatus(string? stlHeader, string? readProblem) =>
        stlHeader is null ? "not checked: " + (readProblem ?? "no STL header")
            : ToolchainTable.ParseStlClangMinimum(stlHeader) is int stl ? $"requires Clang {stl}"
            : "not checked: yvals_core.h has no STL1000 Clang condition";

    /// <summary>
    /// <c>--require-verified-toolchain</c> 값을 읽는다(없으면 off). on·off가 아니면 ArgumentException이다 — 빌드 CLI는 출력을 만들기 전에
    /// 부른다.
    /// </summary>
    public static bool RequireVerified(string? value) =>
        (value ?? "off") switch
        {
            "on" => true,
            "off" => false,
            _ => throw new ArgumentException($"{RequireVerifiedOption} must be on or off.")
        };

    /// <summary>
    /// 검증 요구를 적용한다: required가 참인데 검증 목록 밖이면 실패 문구를, 아니면 null을 돌려준다. 빌드 CLI는 build-evidence를 시작하고
    /// <c>toolchainVerified</c>를 적은 뒤 불러, 실패를 FAIL evidence로 남긴다.
    /// </summary>
    public static string? Enforce(NativeToolchain toolchain, bool required) =>
        required && Notice(toolchain) is string notice ? notice + $"; --{RequireVerifiedOption} on accepts only verified toolchains." : null;

    /// <summary>검증 목록 밖 도구 모음의 한 줄 알림이다(검증한 도구 모음이면 null).</summary>
    public static string? Notice(NativeToolchain toolchain) =>
        toolchain.ToolchainVerified ? null
            : $"Toolchain: {toolchain.CompilerVersion} is not a verified {toolchain.Platform} toolchain " +
              $"(verified: {string.Join("; ", ToolchainTable.VerifiedCompilers(toolchain.Platform.ToString()).DefaultIfEmpty("none"))}; Build/Toolchain.json)";
}
