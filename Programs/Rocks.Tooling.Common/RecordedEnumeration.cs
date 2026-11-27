namespace Rocks.Tooling.Common;

/// <summary>
/// 의존 기록에 남기는 디렉터리 열거다(Docs/Design/BuildReuse 2.4절 — clang 밖의 열거). 기록한 쪽(MetaCompiler)과 다시 확인하는 쪽(빌드의
/// 메타 단계 판정)이 같은 규칙으로 열거해야 결과 집합을 비교할 수 있으므로 열거 규칙을 여기 한 곳에 둔다.
/// <list type="bullet">
/// <item>reparse point(symlink·junction)는 건너뛴다. 읽을 수 없는 항목은 건너뛴다.</item>
/// <item>결과는 정규 절대 경로이고 ordinal 순이다(파일 시스템 열거 순서와 무관하다).</item>
/// <item>디렉터리가 없으면 빈 목록이다(없음도 결과다 — 다시 생기면 결과가 달라진다).</item>
/// </list>
/// 사용 예: <c>RecordedEnumeration.Files(includeRoot, "*.h", recursive: true)</c>.
/// </summary>
public static class RecordedEnumeration
{
    /// <summary>
    /// <paramref name="directory"/> 아래에서 <paramref name="pattern"/>(파일 시스템 와일드카드)에 맞는 파일을 돌려준다.
    /// </summary>
    /// <param name="directory">열거할 디렉터리. 상대 경로면 현재 디렉터리 기준이다.</param>
    /// <param name="pattern">파일 이름 패턴(예: <c>*.h</c>).</param>
    /// <param name="recursive">하위 디렉터리까지 보는가.</param>
    public static IReadOnlyList<string> Files(string directory, string pattern, bool recursive)
    {
        string full = Path.GetFullPath(directory);
        if (!Directory.Exists(full))
        {
            return [];
        }

        EnumerationOptions options = new()
        {
            RecurseSubdirectories = recursive,
            AttributesToSkip = FileAttributes.ReparsePoint,
            IgnoreInaccessible = true
        };
        return [.. Directory.EnumerateFiles(full, pattern, options).Select(Path.GetFullPath).Order(StringComparer.Ordinal)];
    }
}
