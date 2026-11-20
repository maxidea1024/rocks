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

    /// <summary>고정한 clang-format 버전이다(예: <c>21.1.6</c>). 서식 검사와 MetaCompiler가 이 버전만 받는다.</summary>
    public static string ClangFormatVersion => Document.Value.RootElement.GetProperty("clangFormat").GetProperty("version").GetString()!;

    /// <summary>
    /// 플랫폼(<c>Windows</c>·<c>MacOS</c>·<c>Android</c>)의 최소 Clang major다(Apple clang은 Apple 버전 번호). 표에 없으면 null이다.
    /// </summary>
    public static int? MinimumClangMajor(string platform) =>
        Document.Value.RootElement.GetProperty("clang").GetProperty("minimum").TryGetProperty(platform, out JsonElement value) ? value.GetInt32() : null;

    /// <summary>플랫폼의 검증한 Clang 버전 목록이다(<c>major.minor.patch</c>). CI가 없는 플랫폼은 비어 있다.</summary>
    public static IReadOnlyList<string> VerifiedClangVersions(string platform) =>
        Document.Value.RootElement.GetProperty("clang").GetProperty("verified").TryGetProperty(platform, out JsonElement list)
            ? list.EnumerateArray().Select(entry => entry.GetString()!).ToArray()
            : [];

    private static readonly Regex VersionPattern = new(@"clang version (?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// 정규화한 컴파일러 버전 줄(<c>clang version 22.1.8 (…)</c>, <c>Apple clang version 17.0.0 (…)</c>, NDK의
    /// <c>Android (…) clang version 20.0.0 (…)</c>)에서 <c>major.minor.patch</c>와 major를 읽는다. 버전이 없으면 null이다.
    /// </summary>
    public static (string Version, int Major)? ParseClangVersion(string compilerVersion)
    {
        Match match = VersionPattern.Match(compilerVersion);
        return match.Success
            ? ($"{match.Groups["major"].Value}.{match.Groups["minor"].Value}.{match.Groups["patch"].Value}",
                int.Parse(match.Groups["major"].Value, CultureInfo.InvariantCulture))
            : null;
    }

    private static readonly Regex StlClangMinimum = new(@"defined\(__clang__\)\s*\r?\n\s*#if __clang_major__ < (?<major>\d+)",
        RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    /// <summary>
    /// MSVC STL <c>yvals_core.h</c> 본문에서 그 STL이 요구하는 최소 Clang major를 읽는다(<c>STL1000</c> 조건 <c>#if __clang_major__ &lt; N</c>).
    /// 그 줄을 찾지 못하면 null이다(형식이 바뀐 STL — 호출자는 확인을 건너뛰고 첫 컴파일의 <c>STL1000</c>이 알린다).
    /// </summary>
    public static int? ParseStlClangMinimum(string yvalsCore)
    {
        Match match = StlClangMinimum.Match(yvalsCore);
        return match.Success ? int.Parse(match.Groups["major"].Value, CultureInfo.InvariantCulture) : null;
    }
}
