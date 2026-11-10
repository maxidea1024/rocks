using System.Diagnostics;
using System.Text;

namespace Rocks.Tooling.Common;

public sealed record ProcessSpec(
    string FileName,
    IReadOnlyList<string> Arguments,
    string WorkingDirectory,
    IReadOnlyDictionary<string, string?>? Environment = null,
    string? StandardInput = null,
    TimeSpan? Timeout = null,
    Encoding? StandardOutputEncoding = null,
    Encoding? StandardErrorEncoding = null,
    Encoding? StandardInputEncoding = null);

public sealed record ProcessResult(int ExitCode, string StdOut, string StdErr);

public static class ProcessRunner
{
    public static async Task<ProcessResult> RunAsync(ProcessSpec spec, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(spec);
        ArgumentException.ThrowIfNullOrWhiteSpace(spec.FileName);
        ArgumentException.ThrowIfNullOrWhiteSpace(spec.WorkingDirectory);
        ArgumentNullException.ThrowIfNull(spec.Arguments);
        TimeSpan limit = spec.Timeout ?? TimeSpan.FromMinutes(5);
        // 예외가 날 수 있는 설정 검사는 child를 시작하기 전에 끝낸다.
        if (limit <= TimeSpan.Zero || limit.TotalMilliseconds > uint.MaxValue - 1)
            throw new ArgumentOutOfRangeException(nameof(spec), "Timeout must be finite, positive and within the timer range.");
        if (spec.FileName.Contains('\0') || spec.WorkingDirectory.Contains('\0') ||
            spec.Arguments.Any(value => value is null || value.Contains('\0')))
            throw new ArgumentException("Process path/arguments cannot contain null characters.", nameof(spec));
        if (!Directory.Exists(spec.WorkingDirectory)) throw new DirectoryNotFoundException(spec.WorkingDirectory);
        using var timeout = new CancellationTokenSource(limit);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, timeout.Token);
        var start = new ProcessStartInfo(spec.FileName)
        {
            WorkingDirectory = spec.WorkingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = spec.StandardInput is not null,
            StandardInputEncoding = spec.StandardInput is null ? null : spec.StandardInputEncoding ?? new UTF8Encoding(false),
            StandardOutputEncoding = spec.StandardOutputEncoding ?? new UTF8Encoding(false),
            StandardErrorEncoding = spec.StandardErrorEncoding ?? new UTF8Encoding(false)
        };
        foreach (string argument in spec.Arguments) start.ArgumentList.Add(argument);
        if (spec.Environment is not null)
        {
            start.Environment.Clear();
            foreach (var pair in spec.Environment) start.Environment[pair.Key] = pair.Value;
        }
        linked.Token.ThrowIfCancellationRequested();
        using var process = new Process { StartInfo = start };
        Task<string> stdout = Task.FromResult("");
        Task<string> stderr = Task.FromResult("");
        bool started = false;
        try
        {
            process.Start();
            started = true;
            // 양쪽 pipe를 동시에 비운다. parent 종료뿐 아니라 EOF 수신에도 같은 deadline을 적용한다.
            stdout = process.StandardOutput.ReadToEndAsync();
            stderr = process.StandardError.ReadToEndAsync();
            if (spec.StandardInput is not null)
            {
                await process.StandardInput.WriteAsync(spec.StandardInput.AsMemory(), linked.Token);
                process.StandardInput.Close();
            }
            await process.WaitForExitAsync(linked.Token);
            await Task.WhenAll(stdout, stderr).WaitAsync(linked.Token);
            return new ProcessResult(process.ExitCode, stdout.Result, stderr.Result);
        }
        catch (Exception error)
        {
            string cleanup = started ? await CleanupAsync(process, stdout, stderr, start.RedirectStandardInput) : "";
            if (cleanup.Length != 0)
                throw new IOException("Process cleanup incomplete: " + cleanup, error);
            if (timeout.IsCancellationRequested && !cancellationToken.IsCancellationRequested)
                throw new TimeoutException($"Process/pipe deadline expired: {Path.GetFileName(spec.FileName)}", error);
            if (cancellationToken.IsCancellationRequested)
                throw new OperationCanceledException("Process execution cancelled.", error, cancellationToken);
            throw;
        }
    }

    private static async Task<string> CleanupAsync(Process process, Task<string> stdout, Task<string> stderr, bool hasInput)
    {
        List<string> failures = [];
        try
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3));
        }
        catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception or TimeoutException)
        { failures.Add("native termination: " + error.Message); }
        // 이미 종료한 parent의 분리된 descendants를 OS job/process-group 없이 모두 찾는다고 주장하지
        // 않는다. 남은 pipe는 닫고 bounded 복귀한다. 완전한 subtree 격리는 별도 Host 기능이다.
        if (hasInput) process.StandardInput.Dispose();
        process.StandardOutput.Dispose();
        process.StandardError.Dispose();
        try { await Task.WhenAll(stdout, stderr).WaitAsync(TimeSpan.FromSeconds(3)); }
        catch (Exception error) when (error is IOException or ObjectDisposedException or OperationCanceledException) { }
        catch (TimeoutException error) { failures.Add("pipe closure: " + error.Message); }
        // 플랫폼이 close를 지연하더라도 나중에 fault한 task의 예외를 유실하지 않는다.
        foreach (Task task in new Task[] { stdout, stderr })
            _ = task.ContinueWith(completed => { _ = completed.Exception; }, CancellationToken.None,
                TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        return string.Join("; ", failures);
    }
}
