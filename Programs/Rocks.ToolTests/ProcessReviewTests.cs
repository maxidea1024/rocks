using System.Diagnostics;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class ProcessReviewTests
{
    public static void Register(TestSuite suite, Options options)
    {
        suite.Add("process-review-invalid-timeout-cannot-start-child", async () =>
        {
            using TemporaryDirectory temporary = new();
            string marker = temporary.FilePath("started.txt");
            foreach (TimeSpan invalid in new[] { TimeSpan.Zero, TimeSpan.FromMilliseconds(-2), TimeSpan.MaxValue })
            {
                await Verify.ThrowsAsync<ArgumentOutOfRangeException>(() => ProcessRunner.RunAsync(
                    SelfProcess.Command(["child", "--review-marker", marker], options.Root, invalid)));
            }

            await Task.Delay(300);
            Verify.That(!File.Exists(marker), "Rejected timeout launched a child");
        });
        suite.Add("process-review-cancelled-before-start-cannot-create-marker", async () =>
        {
            using TemporaryDirectory temporary = new();
            string marker = temporary.FilePath("started.txt");
            using CancellationTokenSource cancellation = new();
            cancellation.Cancel();
            await Verify.ThrowsAsync<OperationCanceledException>(() => ProcessRunner.RunAsync(
                SelfProcess.Command(["child", "--review-marker", marker], options.Root), cancellation.Token));
            Verify.That(!File.Exists(marker), "Pre-cancelled call launched a child");
        });
        suite.Add("process-review-running-cancellation-drains", async () =>
        {
            using CancellationTokenSource cancellation = new(TimeSpan.FromMilliseconds(300));
            long started = Stopwatch.GetTimestamp();
            await Verify.ThrowsAsync<OperationCanceledException>(() => ProcessRunner.RunAsync(
                SelfProcess.Command(["child", "--hang"], options.Root), cancellation.Token));
            Verify.That(Stopwatch.GetElapsedTime(started) < TimeSpan.FromSeconds(10), "Cancellation cleanup was not bounded");
        });
        suite.Add("process-review-exited-parent-open-child-pipe-has-deadline", async () =>
        {
            using TemporaryDirectory temporary = new();
            string childId = temporary.FilePath("child.pid");
            long started = Stopwatch.GetTimestamp();
            try
            {
                bool rejected = false;
                try
                {
                    await ProcessRunner.RunAsync(SelfProcess.Command(["child", "--review-fork-pipe", childId], options.Root, TimeSpan.FromSeconds(1)));
                }
                catch (TimeoutException)
                {
                    rejected = true;
                }
                catch (IOException error)
                {
                    Verify.Contains("Process cleanup incomplete:", error.Message, "Expected explicit incomplete pipe cleanup");
                    rejected = true;
                }

                Verify.That(rejected, "An open descendant pipe was reported as successful completion");
                Verify.That(Stopwatch.GetElapsedTime(started) < TimeSpan.FromSeconds(9), "Pipe drain ignored its deadline");
                Verify.That(File.Exists(childId), "Fixture never spawned the pipe-holding child");
            }
            finally
            {
                // 부모 종료 후 분리된 후손까지 Common이 격리한다고 주장하지 않는다. fixture owner가 회수한다.
                if (File.Exists(childId))
                {
                    try
                    {
                        using Process descendant = Process.GetProcessById(int.Parse(File.ReadAllText(childId), System.Globalization.CultureInfo.InvariantCulture));
                        if (!descendant.HasExited)
                        {
                            descendant.Kill(entireProcessTree: true);
                        }

                        await descendant.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                    }
                    catch (ArgumentException) { }
                }
            }
        });
        suite.Add("process-review-vs-environment-unicode-roundtrip", async () =>
        {
            // 빌드 환경은 허용 목록만 넘긴다. 허용 목록의 변수(SOURCE_DATE_EPOCH)로 Visual Studio 환경 출력의 Unicode·'=' 왕복을 보고,
            // 목록 밖 변수는 빠지는지 확인한다.
            const string key = "SOURCE_DATE_EPOCH";
            const string outside = "ROCKS_ENV_REVIEW_VALUE";
            const string expected = "한글 경로🎮=표시값";
            string? oldValue = Environment.GetEnvironmentVariable(key);
            string? oldOutside = Environment.GetEnvironmentVariable(outside);
            string? oldVc = Environment.GetEnvironmentVariable("VCToolsInstallDir");
            try
            {
                Environment.SetEnvironmentVariable(key, expected);
                Environment.SetEnvironmentVariable(outside, expected);
                if (OperatingSystem.IsWindows())
                {
                    Environment.SetEnvironmentVariable("VCToolsInstallDir", null);
                }

                Dictionary<string, string?> environment = await NativeEnvironment.CreateAsync();
                Verify.Equal(expected, environment[key], "Native environment corrupted Unicode or equals in value");
                Verify.That(!environment.ContainsKey(outside), "Native environment passed a variable outside the allowlist");
            }
            finally
            {
                Environment.SetEnvironmentVariable(key, oldValue);
                Environment.SetEnvironmentVariable(outside, oldOutside);
                Environment.SetEnvironmentVariable("VCToolsInstallDir", oldVc);
            }
        });
    }

    // Program.ChildAsync에서 기존 child mode보다 먼저 호출한다. shell/Python 없이 스스로를 실행한다.
    public static async Task<int?> TryRunChildAsync(string[] arguments)
    {
        if (arguments.Length == 2 && arguments[0] == "--review-marker")
        {
            await File.WriteAllTextAsync(arguments[1], "started");
            return 0;
        }

        if (arguments.SequenceEqual(new[] { "--review-hold-pipe" }))
        {
            await Task.Delay(TimeSpan.FromSeconds(30));
            return 0;
        }

        if (arguments.Length == 2 && arguments[0] == "--review-fork-pipe")
        {
            ProcessSpec command = SelfProcess.Command(["child", "--review-hold-pipe"], Directory.GetCurrentDirectory());
            ProcessStartInfo start = new(command.FileName) { UseShellExecute = false, CreateNoWindow = true };
            foreach (string argument in command.Arguments)
            {
                start.ArgumentList.Add(argument);
            }

            using Process child = Process.Start(start) ?? throw new InvalidOperationException("Could not start child fixture");
            await File.WriteAllTextAsync(arguments[1], child.Id.ToString(System.Globalization.CultureInfo.InvariantCulture));
            return 0;
        }

        return null;
    }
}
