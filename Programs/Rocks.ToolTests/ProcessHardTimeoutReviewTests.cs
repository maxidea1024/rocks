using System.Diagnostics;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class ProcessHardTimeoutReviewTests
{
    public static void Register(TestSuite suite, Options options)
    {
        suite.Add("Process.CleanupCapacityHasExternalWatchdog", async () =>
        {
            using TemporaryDirectory temporary = new();
            string marker = temporary.FilePath("result.txt");
            await RunWorkerWithWatchdog(options, "--review-budget", marker);
            Verify.Equal("bounded", File.ReadAllText(marker), "cleanup capacity did not recover correctly");
        });
        suite.Add("Process.BrokenPipePreservesFailureHasExternalWatchdog", async () =>
        {
            using TemporaryDirectory temporary = new();
            string marker = temporary.FilePath("result.txt");
            await RunWorkerWithWatchdog(options, "--review-broken-pipe", marker);
            Verify.Equal("broken-pipe", File.ReadAllText(marker), "broken pipe lost its original failure");
        });
        suite.Add("Process.CleanupDeadlineHasExternalWatchdog", async () =>
        {
            using TemporaryDirectory temporary = new();
            await RunWorkerWithWatchdog(options, "--review-blocking-dispose", temporary.FilePath("result.txt"));
            Verify.Equal("timeout-then-clean", File.ReadAllText(temporary.FilePath("result.txt")),
                "blocked dispose did not return on deadline and eventually clean resources");
        });
        suite.Add("Process.BlockedStandardInputHasExternalWatchdog", async () =>
        {
            using TemporaryDirectory temporary = new();
            string marker = temporary.FilePath("result.txt");
            string childId = temporary.FilePath("child.pid");
            try
            {
                await RunWorkerWithWatchdog(options, "--review-blocked-input", marker, childId);
                Verify.Equal("rejected", File.ReadAllText(marker), "blocked stdin reported as success");
                Verify.That(File.Exists(childId), "stdin fixture did not start");
                int pid = int.Parse(File.ReadAllText(childId), System.Globalization.CultureInfo.InvariantCulture);
                try
                {
                    using var process = Process.GetProcessById(pid);
                    Verify.That(process.HasExited, "Timed-out stdin child remains active");
                }
                catch (ArgumentException) { }
            }
            finally
            {
                if (File.Exists(childId) && int.TryParse(File.ReadAllText(childId), out int pid))
                {
                    try
                    {
                        using var process = Process.GetProcessById(pid);
                        if (!process.HasExited) process.Kill(entireProcessTree: true);
                        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                    }
                    catch (ArgumentException) { }
                }
            }
        });
        suite.Add("Process.DetachedPipeDeadlineHasExternalWatchdog", async () =>
        {
            using TemporaryDirectory temporary = new();
            string marker = temporary.FilePath("result.txt");
            string descendant = temporary.FilePath("descendant.pid");
            try
            {
                await RunWorkerWithWatchdog(options, "--review-detached-pipe", marker, descendant);
                Verify.Equal("rejected", File.ReadAllText(marker), "open inherited pipe was reported as success");
            }
            finally
            {
                // fixture 소유의 분리된 후손은 wrapper의 격리 범위 밖이므로 테스트가 회수한다.
                if (File.Exists(descendant) && int.TryParse(File.ReadAllText(descendant), out int pid))
                {
                    try
                    {
                        using var process = Process.GetProcessById(pid);
                        if (!process.HasExited) process.Kill(entireProcessTree: true);
                        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                    }
                    catch (ArgumentException) { }
                }
            }
        });
    }

    private static async Task RunWorkerWithWatchdog(Options options, params string[] arguments)
    {
        ProcessSpec command = SelfProcess.Command(["child", .. arguments], options.Root);
        var start = new ProcessStartInfo(command.FileName)
        {
            WorkingDirectory = command.WorkingDirectory,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (string argument in command.Arguments) start.ArgumentList.Add(argument);
        // watchdog 자체는 ProcessRunner와 redirected pipe를 사용하지 않는다. 대상이 교착돼도
        // 별도 프로세스를 강제 종료하고 이 테스트의 실패를 보고할 수 있어야 한다.
        using Process process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start cleanup review worker");
        try { await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(12)); }
        catch (TimeoutException)
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            throw new InvalidOperationException("Cleanup worker exceeded the external watchdog deadline");
        }
        Verify.Equal(0, process.ExitCode, "Cleanup review worker failed");
    }

    public static async Task<int?> TryRunChildAsync(string[] arguments)
    {
        if (arguments.Length == 2 && arguments[0] == "--review-budget")
        {
            var budget = new ProcessCleanupBudget(2);
            var a = budget.TryReserve() ?? throw new InvalidOperationException("First reservation missing");
            var b = budget.TryReserve() ?? throw new InvalidOperationException("Second reservation missing");
            var pendingA = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var pendingB = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            a.ReleaseAfter(pendingA.Task);
            b.ReleaseAfter(pendingB.Task);
            for (int i = 0; i < 100; i++)
                if (budget.TryReserve() is not null) return 10;
            pendingA.SetResult();
            ProcessCleanupBudget.Reservation? recovered = null;
            for (int i = 0; i < 100 && recovered is null; i++)
            {
                recovered = budget.TryReserve();
                if (recovered is null) await Task.Delay(10);
            }
            if (recovered is null || budget.TryReserve() is not null) return 11;
            recovered.ReleaseAfter(Task.CompletedTask);
            pendingB.SetResult();
            await File.WriteAllTextAsync(arguments[1], "bounded");
            return 0;
        }
        if (arguments.Length == 1 && arguments[0] == "--review-close-input") return 0;
        if (arguments.Length == 2 && arguments[0] == "--review-broken-pipe")
        {
            try
            {
                await ProcessRunner.RunAsync(SelfProcess.Command(
                    ["child", "--review-close-input"], Directory.GetCurrentDirectory(),
                    TimeSpan.FromSeconds(5), new string('x', 4 * 1024 * 1024)));
                return 12;
            }
            catch (ProcessCleanupException error) when (error.InnerException is IOException) { }
            catch (IOException error) when (error is not ProcessCleanupException) { }
            await File.WriteAllTextAsync(arguments[1], "broken-pipe");
            return 0;
        }
        if (arguments.Length == 2 && arguments[0] == "--review-blocking-dispose")
        {
            using var blocked = new BlockingResource();
            var owner = new TrackingResource();
            Task cleanup = ProcessRunner.StartBackgroundCleanup([blocked], owner);
            if (!blocked.Entered.Wait(TimeSpan.FromSeconds(3))) return 1;
            bool completed = await ProcessRunner.CompletesWithinAsync(cleanup, TimeSpan.FromMilliseconds(200));
            if (completed || owner.Disposed) return 2;
            blocked.Release.Set();
            await cleanup.WaitAsync(TimeSpan.FromSeconds(3));
            if (!owner.Disposed || !blocked.Disposed) return 3;
            // 종료 작업이 남아 있으면 stream이 닫혀도 Process owner를 먼저 파괴하지 않는다.
            var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var stream = new TrackingResource();
            var delayedOwner = new TrackingResource();
            Task delayed = ProcessRunner.StartBackgroundCleanup([stream], delayedOwner, completion.Task);
            if (await ProcessRunner.CompletesWithinAsync(delayed, TimeSpan.FromMilliseconds(200)) ||
                !stream.Disposed || delayedOwner.Disposed) return 5;
            completion.SetResult();
            await delayed.WaitAsync(TimeSpan.FromSeconds(3));
            if (!delayedOwner.Disposed) return 6;
            await File.WriteAllTextAsync(arguments[1], "timeout-then-clean");
            return 0;
        }
        if (arguments.Length == 2 && arguments[0] == "--review-ignore-input")
        {
            await File.WriteAllTextAsync(arguments[1], Environment.ProcessId.ToString(System.Globalization.CultureInfo.InvariantCulture));
            await Task.Delay(TimeSpan.FromSeconds(30));
            return 0;
        }
        if (arguments.Length == 3 && arguments[0] == "--review-blocked-input")
        {
            bool rejected = false;
            try
            {
                await ProcessRunner.RunAsync(SelfProcess.Command(
                    ["child", "--review-ignore-input", arguments[2]], Directory.GetCurrentDirectory(),
                    TimeSpan.FromSeconds(1), new string('x', 4 * 1024 * 1024)));
            }
            catch (TimeoutException) { rejected = true; }
            catch (IOException error) when (error.Message.Contains("Process cleanup incomplete", StringComparison.Ordinal)) { rejected = true; }
            if (!rejected) return 7;
            await File.WriteAllTextAsync(arguments[1], "rejected");
            return 0;
        }
        if (arguments.Length == 3 && arguments[0] == "--review-detached-pipe")
        {
            bool rejected = false;
            try
            {
                await ProcessRunner.RunAsync(SelfProcess.Command(
                    ["child", "--review-fork-pipe", arguments[2]], Directory.GetCurrentDirectory(), TimeSpan.FromSeconds(1)));
            }
            catch (TimeoutException) { rejected = true; }
            catch (IOException error) when (error.Message.Contains("Process cleanup incomplete", StringComparison.Ordinal)) { rejected = true; }
            if (!rejected) return 4;
            await File.WriteAllTextAsync(arguments[1], "rejected");
            return 0;
        }
        return null;
    }

    private sealed class BlockingResource : IDisposable
    {
        public ManualResetEventSlim Entered { get; } = new();
        public ManualResetEventSlim Release { get; } = new();
        public bool Disposed { get; private set; }

        public void Dispose()
        {
            // 테스트 종료 시 재호출되어도 안전해야 한다.
            if (Disposed) return;
            Entered.Set();
            Release.Wait();
            Disposed = true;
        }
    }

    private sealed class TrackingResource : IDisposable
    {
        public bool Disposed { get; private set; }
        public void Dispose() => Disposed = true;
    }
}
