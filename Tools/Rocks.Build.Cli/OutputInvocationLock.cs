using System.Diagnostics;

namespace Rocks.Build.Cli;

// 같은 출력 경로를 사용하는 전체 빌드 호출의 단일 소유권이다. 메타 생성과 보고서도
// 보호하며 네이티브 엔진 내부의 .native-build.lock과는 다른 파일이다. lock 파일을
// 삭제하면 보유 중인 파일과 새 파일이 분리될 수 있어 잠금 해제 때 지우지 않는다.
public static class OutputInvocationLock
{
    public static readonly TimeSpan DefaultMaximumWait = TimeSpan.FromMinutes(30);

    // I/O 실패 재시도는 최대 대기 시간을 넘기지 않는다. OS 파일 열기 자체가 중단 없이
    // 오래 걸리는 경우는 제한할 수 없다. 취소와 시간 초과는 별도 예외로 구분한다.
    public static async Task<FileStream> AcquireAsync(string output, CancellationToken cancellationToken = default,
        TimeSpan? maximumWait = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(output);
        TimeSpan limit = maximumWait ?? DefaultMaximumWait;
        if (limit <= TimeSpan.Zero || limit > TimeSpan.FromDays(1))
            throw new ArgumentOutOfRangeException(nameof(maximumWait));
        cancellationToken.ThrowIfCancellationRequested();
        string directory = Path.GetFullPath(output);
        Directory.CreateDirectory(directory);
        string path = Path.Combine(directory, ".rocks-invocation.lock");
        long start = Stopwatch.GetTimestamp();
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (Directory.Exists(path)) throw new IOException("Output invocation lock path is a directory: " + path);
            try
            {
                var stream = new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
                if (cancellationToken.IsCancellationRequested)
                {
                    stream.Dispose();
                    cancellationToken.ThrowIfCancellationRequested();
                }
                return stream;
            }
            catch (IOException error)
            {
                if (Directory.Exists(path)) throw new IOException("Output invocation lock path is a directory: " + path, error);
                TimeSpan remaining = limit - Stopwatch.GetElapsedTime(start);
                if (remaining <= TimeSpan.Zero)
                    throw new TimeoutException($"Output invocation lock could not be acquired within {limit}: {path}", error);
                // 다른 호출이 소유한 동안 bounded polling한다. 취소는 다음 대기에서 즉시 반영한다.
                await Task.Delay(remaining < TimeSpan.FromMilliseconds(50) ? remaining : TimeSpan.FromMilliseconds(50), cancellationToken);
            }
        }
    }
}
