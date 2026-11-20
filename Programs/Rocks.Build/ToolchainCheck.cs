using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// 도구 모음 버전 확인이다(Docs/Design/ClangVersionSupport.ko.md 6.2절). 도구 모음을 찾을 때 한 번 부른다: 플랫폼 최소 Clang 미만이면
/// 거부하고, Windows에서는 고른 MSVC STL(<c>yvals_core.h</c>)이 요구하는 최소 Clang을 첫 컴파일 전에 확인한다. 검증 목록 밖이면 거부하지
/// 않고 <see cref="NativeToolchain.ToolchainVerified"/>를 거짓으로 둔다(빌드 CLI가 한 줄 알리고 증거에 남기며, CI는
/// <c>--require-verified-toolchain on</c>으로 실패시킨다).
/// </summary>
public static class ToolchainCheck
{
    /// <summary>
    /// compilerVersion은 정규화한 <c>--version</c> 첫 줄, stlHeader는 Windows에서 고른 도구 모음의 <c>include/yvals_core.h</c> 본문(없으면
    /// null)이다. 검증 목록에 있으면 참이다. 최소 미만이거나 STL이 요구하는 Clang에 못 미치면 InvalidOperationException이다.
    /// </summary>
    public static bool Verify(NativePlatform platform, string compilerVersion, string? stlHeader)
    {
        string name = platform.ToString();
        if (ToolchainTable.ParseClangVersion(compilerVersion) is not (string version, int major))
        {
            throw new InvalidOperationException($"Cannot read the Clang version from '{compilerVersion}'.");
        }

        if (ToolchainTable.MinimumClangMajor(name) is int minimum && major < minimum)
        {
            throw new InvalidOperationException($"Clang {version} is older than the minimum Clang {minimum} for {name} " +
                "(Build/Toolchain.json); install a newer LLVM (Windows: pwsh Build/InstallLlvm.ps1).");
        }

        if (stlHeader is not null && ToolchainTable.ParseStlClangMinimum(stlHeader) is int stl && major < stl)
        {
            throw new InvalidOperationException($"The selected MSVC STL requires Clang {stl} or newer (STL1000), but the compiler is Clang {version}; " +
                $"pick an older MSVC toolset with {NativeEnvironment.MsvcToolsetVariable} (for example 14.44) or install a newer LLVM.");
        }

        return ToolchainTable.VerifiedClangVersions(name).Contains(version, StringComparer.Ordinal);
    }

    /// <summary>검증 목록 밖 도구 모음의 한 줄 알림이다(검증한 도구 모음이면 null).</summary>
    public static string? Notice(NativeToolchain toolchain) =>
        toolchain.ToolchainVerified ? null
            : $"Toolchain: {toolchain.CompilerVersion} is not a verified {toolchain.Platform} toolchain " +
              $"(verified: {string.Join(", ", ToolchainTable.VerifiedClangVersions(toolchain.Platform.ToString()).DefaultIfEmpty("none"))}; Build/Toolchain.json)";
}
