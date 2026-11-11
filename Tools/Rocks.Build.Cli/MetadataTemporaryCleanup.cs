namespace Rocks.Build.Cli;

// 실행이 이미 실패했다면 임시 파일 삭제 실패를 별도로 보고하되 기존 실패·취소를 바꾸지 않는다.
// 실행이 성공한 경우에는 정리 오류도 빌드 실패로 반환한다.
internal static class MetadataTemporaryCleanup
{
    public static void Finish(IEnumerable<string> paths, bool priorFailure,
        Action<string>? report = null, Action<string>? delete = null)
    {
        ArgumentNullException.ThrowIfNull(paths);
        report ??= Console.Error.WriteLine;
        delete ??= File.Delete;
        List<string> failures = [];
        foreach (string path in paths)
        {
            try { delete(path); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            { failures.Add(path + ": " + error.Message); }
        }
        if (failures.Count == 0) return;
        string diagnostic = "Metadata temporary cleanup failed: " + string.Join("; ", failures);
        if (!priorFailure) throw new IOException(diagnostic);
        // 오류 출력 자체가 닫힌 pipe 등으로 실패해도 기존 컴파일 오류/취소를 덮지 않는다.
        try { report(diagnostic); }
        catch (Exception error) when (error is not OutOfMemoryException) { }
    }
}
