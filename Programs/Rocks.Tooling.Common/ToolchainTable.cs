using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.Tooling.Common;

/// <summary>
/// 검증한 도구 모음 표(<c>Build/Toolchain.json</c> — Docs/Design/ClangVersionSupport.ko.md 6.1절)다. 빌드 때 이 어셈블리의 포함 리소스로
/// 들어가므로 설치 SDK에 실린 도구도 그 엔진 버전의 표를 함께 갖는다. 설치 스크립트(<c>InstallLlvm.ps1</c>·<c>InstallFormatter.ps1</c>)는
/// 저장소의 같은 파일을 읽는다.
/// </summary>
public static class ToolchainTable
{
    private const string ResourceName = "Rocks.Toolchain.json";

    private static readonly Lazy<JsonDocument> Document = new(() =>
    {
        using Stream stream = typeof(ToolchainTable).Assembly.GetManifestResourceStream(ResourceName)
            ?? throw new InvalidOperationException($"Toolchain table resource {ResourceName} is missing from {typeof(ToolchainTable).Assembly.GetName().Name}.");
        return JsonDocument.Parse(stream);
    });

    /// <summary>고정한 clang-format 버전이다(예: <c>21.1.6</c>). 서식 검사와 MetaCompiler가 이 버전만 받는다(<see cref="IsClangFormatVersion"/>).</summary>
    public static string ClangFormatVersion => Document.Value.RootElement.GetProperty("clangFormat").GetProperty("version").GetString()!;

    /// <summary>
    /// 고정한 ISPC 컴파일러 버전이다(<c>ispc.version</c>, 예: <c>1.32.0</c> — Docs/Design/BuildTargets 3.12절 BD45). ISPC 원본을 빌드하는
    /// 모듈은 이 버전만 받는다(Rocks.Build IspcTools).
    /// </summary>
    public static string IspcVersion => Document.Value.RootElement.GetProperty("ispc").GetProperty("version").GetString()!;

    /// <summary>
    /// 받는 Visual Studio(Build Tools 포함)의 최소 버전이다(<c>visualStudio.minimumVersion</c>, 예: <c>17.8</c>). vswhere 조회에
    /// <c>-version [17.8,)</c>로 준다. UE 5.8 <c>Windows_SDK.json</c>의 최소값과 같다(Docs/Design/ClangVersionSupport.ko.md 6.2절).
    /// </summary>
    public static string MinimumVisualStudioVersion =>
        Document.Value.RootElement.GetProperty("visualStudio").GetProperty("minimumVersion").GetString()!;

    /// <summary>받는 MSVC 도구 모음의 최소 버전이다(<c>visualStudio.minimumMsvc</c>, 예: <c>14.38.33130</c>). 만든 환경의 <c>VCToolsVersion</c>과 비교한다.</summary>
    public static string MinimumMsvcToolset => Document.Value.RootElement.GetProperty("visualStudio").GetProperty("minimumMsvc").GetString()!;

    /// <summary>CI가 쓰는 Android NDK 버전이다(<c>android.ndk.version</c>, 예: <c>29.0.14206865</c> — <c>Build/SelectAndroidNdk.ps1</c>가 고른다).</summary>
    public static string AndroidNdkVersion => Document.Value.RootElement.GetProperty("android").GetProperty("ndk").GetProperty("version").GetString()!;

    /// <summary>
    /// clang-format <c>--version</c> 출력이 고정한 버전인지다. <c>version 21.1.6</c> 뒤가 공백·끝이어야 한다(<c>21.1.60</c>은 다른 버전이다).
    /// 서식 검사(FormatCheck)와 MetaCompiler(RMC3100)가 함께 쓴다.
    /// </summary>
    public static bool IsClangFormatVersion(string versionOutput) =>
        Regex.IsMatch(versionOutput, $@"version\s+{Regex.Escape(ClangFormatVersion)}(?:\s|$)", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// 최소 Clang major다. Apple clang(<c>Apple clang version</c>)은 <c>clang.minimum.appleClang</c>, 그 밖(업스트림 LLVM 계열 — 공식 배포·
    /// Homebrew·NDK·포크)은 플랫폼(<c>Windows</c>·<c>MacOS</c>·<c>Android</c>)의 <c>clang.minimum.llvm</c>이다. Apple clang 번호와 업스트림 번호는
    /// 다른 계열이라 표를 나눈다(Docs/Design/ClangVersionSupport.ko.md 6.1절). 표에 없으면 null이다.
    /// </summary>
    public static int? MinimumClangMajor(string platform, bool appleClang)
    {
        JsonElement minimum = Document.Value.RootElement.GetProperty("clang").GetProperty("minimum");
        if (appleClang)
        {
            return minimum.TryGetProperty("appleClang", out JsonElement apple) ? apple.GetInt32() : null;
        }

        return minimum.TryGetProperty("llvm", out JsonElement llvm) && llvm.TryGetProperty(platform, out JsonElement value) ? value.GetInt32() : null;
    }

    /// <summary>
    /// 플랫폼의 검증한 컴파일러 목록이다: 정규화한 <c>--version</c> 첫 줄 전체(<c>NativeToolchain.CompilerVersion</c> — 괄호의 빌드 표지·commit
    /// 포함). 번호만 보면 Xcode 빌드·포크가 같은 번호를 내므로 줄 전체로 대조한다. CI가 없는 플랫폼은 비어 있다.
    /// </summary>
    public static IReadOnlyList<string> VerifiedCompilers(string platform) =>
        Document.Value.RootElement.GetProperty("clang").GetProperty("verified").TryGetProperty(platform, out JsonElement list)
            ? list.EnumerateArray().Select(entry => entry.GetString()!).ToArray()
            : [];

    private static readonly Regex VersionPattern = new(@"clang version (?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// 정규화한 컴파일러 버전 줄(<c>clang version 22.1.8 (…)</c>, <c>Apple clang version 17.0.0 (…)</c>, NDK의
    /// <c>Android (…) clang version 20.0.0 (…)</c>)에서 <c>major.minor.patch</c>, major, Apple clang인지를 읽는다. 버전이 없으면 null이다.
    /// </summary>
    public static (string Version, int Major, bool AppleClang)? ParseClangVersion(string compilerVersion)
    {
        Match match = VersionPattern.Match(compilerVersion);
        return match.Success
            ? ($"{match.Groups["major"].Value}.{match.Groups["minor"].Value}.{match.Groups["patch"].Value}",
                int.Parse(match.Groups["major"].Value, CultureInfo.InvariantCulture), compilerVersion.TrimStart().StartsWith("Apple clang version", StringComparison.Ordinal))
            : null;
    }

    // STL1000 조건이다: `#if __clang_major__ < N` 바로 다음 줄이 STL1000 오류여야 한다(다른 `__clang_major__` 비교를 잡지 않게). 오류 줄은
    // 14.29의 `#error STL1000: ...`와 14.44·14.52의 `_EMIT_STL_ERROR(STL1000, "...");` 두 형식이다(Docs/Design/ClangVersionSupport.ko.md 6.2절).
    private static readonly Regex StlClangMinimum = new(@"#if __clang_major__ < (?<major>\d+)[ \t]*\r?\n[ \t]*(?:#error[ \t]+|_EMIT_STL_ERROR\([ \t]*)STL1000\b",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// MSVC STL <c>yvals_core.h</c> 본문에서 그 STL이 요구하는 최소 Clang major를 읽는다(<c>#if __clang_major__ &lt; N</c> 바로 다음이
    /// <c>#error STL1000</c> 또는 <c>_EMIT_STL_ERROR(STL1000, …)</c>). 그 조건을 찾지 못하면 null이다(형식이 바뀐 STL — 호출자는 evidence에 남기고 첫 컴파일의 <c>STL1000</c>이 알린다).
    /// </summary>
    public static int? ParseStlClangMinimum(string yvalsCore)
    {
        Match match = StlClangMinimum.Match(yvalsCore);
        return match.Success ? int.Parse(match.Groups["major"].Value, CultureInfo.InvariantCulture) : null;
    }
}
