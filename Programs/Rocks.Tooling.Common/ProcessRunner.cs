using System.Diagnostics;
using System.Text;

[assembly: System.Runtime.CompilerServices.InternalsVisibleTo("Rocks.ToolTests")]

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

// 호출자가 정리 실패와 원래 프로세스 실패를 구분해 후속 리소스 삭제를 보류할 수 있다.
public sealed class ProcessCleanupException(string message, Exception? innerException = null)
    : IOException(message, innerException)
{
    public bool CleanupIncomplete => true;
}

public static class ProcessRunner
{
    // 완료되지 않은 cleanup이 무한 누적되지 않게 신규 프로세스 실행 전에 용량을 확보한다.
    public const int MaximumConcurrentProcesses = 64;
    private static readonly ProcessCleanupBudget Budget = new(MaximumConcurrentProcesses);

    public static async Task<ProcessResult> RunAsync(ProcessSpec spec, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(spec);
        ArgumentException.ThrowIfNullOrWhiteSpace(spec.FileName);
        ArgumentException.ThrowIfNullOrWhiteSpace(spec.WorkingDirectory);
        ArgumentNullException.ThrowIfNull(spec.Arguments);
        TimeSpan limit = spec.Timeout ?? TimeSpan.FromMinutes(5);
        // 예외가 날 수 있는 설정 검사는 child를 시작하기 전에 끝낸다.
        if (limit <= TimeSpan.Zero || limit.TotalMilliseconds > uint.MaxValue - 1)
        {
            throw new ArgumentOutOfRangeException(nameof(spec), "Timeout must be finite, positive and within the timer range.");
        }

        if (spec.FileName.Contains('\0') || spec.WorkingDirectory.Contains('\0') ||
            spec.Arguments.Any(value => value is null || value.Contains('\0')))
        {
            throw new ArgumentException("Process path/arguments cannot contain null characters.", nameof(spec));
        }

        if (!Directory.Exists(spec.WorkingDirectory))
        {
            throw new DirectoryNotFoundException(spec.WorkingDirectory);
        }

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
        foreach (string argument in spec.Arguments)
        {
            start.ArgumentList.Add(argument);
        }

        if (spec.Environment is not null)
        {
            start.Environment.Clear();
            foreach (var pair in spec.Environment)
            {
                start.Environment[pair.Key] = pair.Value;
            }
        }

        linked.Token.ThrowIfCancellationRequested();
        var process = new Process { StartInfo = start };
        var reservation = Budget.TryReserve();
        if (reservation is null)
        {
            process.Dispose();
            throw new InvalidOperationException("Process execution capacity exhausted: 64 running or not-yet-cleaned operations. Retry after cleanup completes.");
        }

        Task? cleanupTask = null;
        Task<string> stdout = Task.FromResult("");
        Task<string> stderr = Task.FromResult("");
        bool started = false;
        Task? inputClose = null;
        try
        {
            process.Start();
            started = true;
            // 양쪽 pipe를 동시에 비운다. parent 종료뿐 아니라 EOF 수신에도 같은 deadline을 적용한다.
            stdout = process.StandardOutput.ReadToEndAsync(linked.Token);
            stderr = process.StandardError.ReadToEndAsync(linked.Token);
            if (spec.StandardInput is not null)
            {
                await process.StandardInput.WriteAsync(spec.StandardInput.AsMemory(), linked.Token);
                await process.StandardInput.FlushAsync(linked.Token);
                // flush 후에도 OS handle close가 지연될 수 있으므로 호출자의 deadline에 포함한다.
                inputClose = Task.Run(process.StandardInput.Close);
                await inputClose.WaitAsync(linked.Token);
            }

            await process.WaitForExitAsync(linked.Token);
            await Task.WhenAll(stdout, stderr).WaitAsync(linked.Token);
            var result = new ProcessResult(process.ExitCode, stdout.Result, stderr.Result);
            // 정상적인 EOF 후에도 OS handle 정리가 예상 밖으로 지연될 수 있으므로 deadline을 둔다.
            cleanupTask = StartBackgroundCleanup([process.StandardOutput, process.StandardError], process);
            if (!await CompletesWithinAsync(cleanupTask, TimeSpan.FromSeconds(3)))
            {
                throw new ProcessCleanupException("Process cleanup incomplete: resource disposal exceeded deadline; cleanup remains pending");
            }

            if (cleanupTask.IsFaulted)
            {
                throw new ProcessCleanupException("Process cleanup incomplete: resource disposal failed", cleanupTask.Exception!.GetBaseException());
            }

            return result;
        }
        catch (Exception error)
        {
            // broken pipe 등 원래 실패가 취소로 바뀌지 않도록 상태를 취소 전에 보관한다.
            bool timedOut = timeout.IsCancellationRequested;
            bool cancelled = cancellationToken.IsCancellationRequested;
            linked.Cancel();
            if (cleanupTask is not null)
            {
                throw; // 성공 경로의 cleanup이 실패·시간 초과한 경우 이미 정리 작업을 소유한다.
            }

            if (started)
            {
                var outcome = await CleanupAsync(process, stdout, stderr, start.RedirectStandardInput, inputClose);
                cleanupTask = outcome.Completion;
                if (outcome.Details.Length != 0)
                {
                    Exception original = cancelled
                        ? new OperationCanceledException("Process execution cancelled.", error, cancellationToken)
                        : timedOut
                            ? new TimeoutException($"Process/pipe deadline expired: {Path.GetFileName(spec.FileName)}", error)
                            : error;
                    throw new ProcessCleanupException("Process cleanup incomplete: " + outcome.Details, original);
                }
            }
            else
            {
                process.Dispose();
            }

            if (timedOut && !cancelled)
            {
                throw new TimeoutException($"Process/pipe deadline expired: {Path.GetFileName(spec.FileName)}", error);
            }

            if (cancelled)
            {
                throw new OperationCanceledException("Process execution cancelled.", error, cancellationToken);
            }

            throw;
        }
        finally
        {
            // timeout 이후에도 실제 정리가 끝날 때까지 예약을 보유한다. 미완료 상태를 재사용하지 않는다.
            reservation.ReleaseAfter(cleanupTask ?? Task.CompletedTask);
        }
    }

    // 완료되지 않는 Dispose가 caller를 묶지 못하도록 전용 worker로 이전한다.
    // process는 모든 stream Dispose 작업이 끝난 뒤 정리한다. 각 worker는 백그라운드이므로
    // 프로세스가 종료한 후에도 상속 파이프를 붙잡는 자식은 별도 OS 격리 계층의 책임이다.
    internal static Task StartBackgroundCleanup(IReadOnlyList<IDisposable> resources, IDisposable owner, Task? termination = null)
    {
        var streams = resources.Select(resource => Task.Run(resource.Dispose)).ToArray();
        return FinishAsync();

        async Task FinishAsync()
        {
            try
            {
                await Task.WhenAll(streams);
            }
            finally
            {
                // 종료 작업이 아직 Process 객체를 사용하는 동안 owner를 파괴하지 않는다.
                try
                {
                    if (termination is not null)
                    {
                        await termination;
                    }
                }
                finally
                {
                    await Task.Run(owner.Dispose);
                }
            }
        }
    }

    internal static async Task<bool> CompletesWithinAsync(Task task, TimeSpan timeout)
    {
        try
        {
            await task.WaitAsync(timeout);
            return true;
        }
        catch (TimeoutException)
        {
            return false;
        }
        catch (Exception) when (task.IsFaulted)
        {
            return true;
        }
    }

    private sealed record CleanupOutcome(string Details, Task Completion);

    private static async Task<CleanupOutcome> CleanupAsync(Process process, Task<string> stdout, Task<string> stderr, bool hasInput, Task? inputClose)
    {
        List<string> failures = [];
        // Kill(entireProcessTree)도 OS 프로세스 열거·종료에 시간이 걸릴 수 있어 caller가 직접
        // 동기 호출하지 않는다. 제한 시간을 넘기면 정리 대기 중임을 명시하고 호출에서 복귀한다.
        Task termination = Task.Run(async () =>
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }

            await process.WaitForExitAsync();
        });
        if (!await CompletesWithinAsync(termination, TimeSpan.FromSeconds(3)))
        {
            failures.Add("native termination exceeded deadline; background cleanup remains pending");
        }
        else if (termination.IsFaulted)
        {
            failures.Add("native termination: " + termination.Exception!.GetBaseException().Message);
        }
        // 이미 종료한 parent의 분리된 descendants를 OS job/process-group 없이 모두 찾는다고 주장하지
        // 않는다. 남은 pipe는 닫고 bounded 복귀한다. 완전한 subtree 격리는 별도 Host 기능이다.
        // StreamReader.Dispose는 진행 중인 OS read를 동기적으로 기다릴 수 있다. 호출자 스레드에서
        // 직접 실행하지 않는다. 실제 Dispose 완료 전에는 Process도 파괴하지 않으며, 무기한 대기를
        // 호출자에게 성공으로 숨기지 않고 별도 실패로 보고한다.
        var resources = new List<IDisposable>();
        if (hasInput && inputClose is null)
        {
            resources.Add(process.StandardInput);
        }

        resources.Add(process.StandardOutput);
        resources.Add(process.StandardError);
        Task prerequisites = inputClose is null ? termination : Task.WhenAll(termination, inputClose);
        Task cleanup = StartBackgroundCleanup(resources, process, prerequisites);
        if (!await CompletesWithinAsync(cleanup, TimeSpan.FromSeconds(3)))
        {
            failures.Add("pipe closure exceeded deadline; background cleanup remains pending");
        }
        else if (cleanup.IsFaulted)
        {
            failures.Add("resource disposal: " + cleanup.Exception!.GetBaseException().Message);
        }
        // timeout 이후에도 나중에 fault한 pipe task의 예외는 관찰한다.
        foreach (Task task in new Task[] { stdout, stderr, termination, prerequisites, cleanup })
        {
            _ = task.ContinueWith(completed => { _ = completed.Exception; }, CancellationToken.None,
                TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }

        return new CleanupOutcome(string.Join("; ", failures), cleanup);
    }
}
