namespace Rocks.Tooling.Common;

/// <summary>
/// 디렉터리 포함 판정이다. 저장소 도구가 "이 경로가 그 루트 안인가"를 물을 때 쓴다.
/// <para>
/// <c>Path.TrimEndingDirectorySeparator(root) + Path.DirectorySeparatorChar</c>로 접두를 만들면 드라이브 루트(<c>W:\</c> —
/// subst 작업 사본)·POSIX <c>/</c>·장치 경로 루트(<c>\\?\W:\</c>)는 구분자가 지워지지 않아 <c>W:\\</c>가 되고, 그 아래 어떤 경로와도
/// 맞지 않는다. 이 클래스는 전체 경로가 이미 구분자로 끝나면 그대로 두고, 아니면 구분자 하나를 붙여 접두를 만든다.
/// </para>
/// <para>
/// 두 인자 모두 <see cref="Path.GetFullPath(string)"/>로 정규화한다(상대 경로는 현재 디렉터리 기준, Windows의 <c>/</c>는 <c>\</c>).
/// symlink·junction·8.3 짧은 이름은 풀지 않는다 — 철자 비교다(<c>\\?\W:\a</c>와 <c>W:\</c>처럼 철자 형식이 섞이면 거짓). 비교는
/// Windows에서 대소문자 무시, 그 밖에서 구분한다(macOS의 <c>Path.GetRelativePath</c>는 대소문자를 무시하므로 철자가 섞이면 둘의
/// 판정이 다를 수 있다).
/// </para>
/// 예: <c>DirectoryPath.IsAtOrUnder(@"W:\Core\A.h", @"W:\")</c>는 참, <c>DirectoryPath.IsAtOrUnder(@"C:\work\rocks2", @"C:\work\rocks")</c>는 거짓.
/// </summary>
public static class DirectoryPath
{
    /// <summary>경로 철자 비교 방식이다(Windows는 대소문자 무시, 그 밖은 구분).</summary>
    public static StringComparison Comparison => OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

    /// <summary>
    /// directory의 전체 경로를 구분자로 끝나게 돌려준다. 루트(<c>W:\</c>·<c>\\server\share\</c>·<c>/</c>)처럼 이미 구분자로 끝나면
    /// 그대로다. 예: <c>C:\work\rocks</c> → <c>C:\work\rocks\</c>, <c>W:\</c> → <c>W:\</c>. directory가 비었거나 잘못된 경로면
    /// <see cref="Path.GetFullPath(string)"/>의 예외(ArgumentException 등)다.
    /// </summary>
    public static string WithTrailingSeparator(string directory)
    {
        string full = Path.GetFullPath(directory);
        return Path.EndsInDirectorySeparator(full) ? full : full + Path.DirectorySeparatorChar;
    }

    /// <summary>path가 root 자신이거나 그 아래면 참이다. 끝 구분자 유무는 무시한다(<c>W:\a\</c>와 <c>W:\a</c>는 같은 디렉터리).</summary>
    public static bool IsAtOrUnder(string path, string root) =>
        WithTrailingSeparator(path).StartsWith(WithTrailingSeparator(root), Comparison);

    /// <summary>path가 root 아래이고 root 자신은 아니면 참이다. 루트 안의 파일·하위 디렉터리만 받는 검사에 쓴다.</summary>
    public static bool IsStrictlyUnder(string path, string root)
    {
        string candidate = WithTrailingSeparator(path);
        string prefix = WithTrailingSeparator(root);
        return candidate.Length > prefix.Length && candidate.StartsWith(prefix, Comparison);
    }
}
