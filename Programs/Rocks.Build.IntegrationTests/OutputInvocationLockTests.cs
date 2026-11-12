using System.Reflection;
using Rocks.Build.Cli;
using Rocks.Tooling.Common;

internal static class OutputInvocationLockTests
{
    public static async Task<int> ChildAsync(string[] args)
    {
        if (args.Length != 4)
        {
            throw new ArgumentException("Invalid output lock child arguments.");
        }

        string output = args[1], waiting = args[2], acquired = args[3];
        File.WriteAllText(waiting, "waiting");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(1));
        try
        {
            await using var held = await OutputInvocationLock.AcquireAsync(output, timeout.Token);
            File.WriteAllText(acquired, "acquired");
            return 0;
        }
        catch (OperationCanceledException)
        {
            return 41;
        }
    }

    public static async Task RunAsync()
    {
        string root = Path.Combine(Path.GetTempPath(), "Rocks OutputLock " + Guid.NewGuid().ToString("N"));
        string output = Path.Combine(root, "out");
        Directory.CreateDirectory(root);
        bool cleanupSafe = true;
        try
        {
            await using (var held = await OutputInvocationLock.AcquireAsync(output))
            {
                string waiting = Path.Combine(root, "child-waiting");
                string acquired = Path.Combine(root, "child-acquired");
                ProcessResult child = await Child(root, output, waiting, acquired);
                Require(File.Exists(waiting), "child did not start lock attempt");
                Require(child.ExitCode == 41 && !File.Exists(acquired), "cross-process lock failed to block and cancel: " + child.StdErr);
                try
                {
                    await using var unexpected = await OutputInvocationLock.AcquireAsync(output, maximumWait: TimeSpan.FromMilliseconds(150));
                    throw new InvalidOperationException("contended output lock ignored maximum wait");
                }
                catch (TimeoutException error)
                {
                    Require(error.InnerException is IOException, "timeout omitted the underlying I/O cause");
                }

                // 동일 프로세스의 게임 호출도 대기 중에는 RUNNING/FAIL 보고서를 덮지 않는다.
                using var cancel = new CancellationTokenSource(TimeSpan.FromMilliseconds(300));
                var options = new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["sdk"] = Path.Combine(root, "sdk"),
                    ["project"] = Path.Combine(root, "project"),
                    ["output"] = output
                };
                try
                {
                    await InstalledSdk.BuildGameAsync(options, cancel.Token);
                    throw new InvalidOperationException("game call did not cancel while waiting for output ownership");
                }
                catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }

                Require(!File.Exists(Path.Combine(output, "game-build-evidence.json")), "waiting game call published evidence");
            }

            string nextWaiting = Path.Combine(root, "next-waiting");
            string nextAcquired = Path.Combine(root, "next-acquired");
            var next = await Child(root, output, nextWaiting, nextAcquired);
            Require(next.ExitCode == 0 && File.Exists(nextAcquired), "released lock was not reusable: " + next.StdErr);
            string invalid = Path.Combine(root, "invalid");
            Directory.CreateDirectory(Path.Combine(invalid, ".rocks-invocation.lock"));
            try
            {
                await using var unexpected = await OutputInvocationLock.AcquireAsync(invalid, maximumWait: TimeSpan.FromSeconds(1));
                throw new InvalidOperationException("lock path directory was accepted");
            }
            catch (IOException error)
            {
                Require(error.Message.Contains("directory", StringComparison.Ordinal), "invalid lock path diagnostic omitted cause");
            }

            Console.WriteLine("PASS output invocation lock: cross-process exclusion, cancellation, game evidence, release");
        }
        catch (ProcessCleanupException)
        {
            cleanupSafe = false;
            throw;
        }
        finally
        {
            if (cleanupSafe)
            {
                Directory.Delete(root, true);
            }
        }
    }

    private static async Task<ProcessResult> Child(string root, string output, string waiting, string acquired)
    {
        string assembly = Assembly.GetExecutingAssembly().Location;
        return await ProcessRunner.RunAsync(new("dotnet", [assembly, "--output-lock-child", output, waiting, acquired],
            root, Timeout: TimeSpan.FromSeconds(15)));
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
