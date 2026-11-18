using System.Diagnostics;
using System.Text;
namespace Rocks.ToolTests;

internal static class LoggerOutputTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        string executable = Path.GetFullPath(options.Required("native-executable"));
        ProcessStartInfo StartInfo()
        {
            var start = new ProcessStartInfo(executable)
            {
                WorkingDirectory = options.Root,
                UseShellExecute = false,
                RedirectStandardError = true,
                RedirectStandardInput = true,
                CreateNoWindow = true
            };
            start.Environment.Clear();
            foreach (var pair in environment)
            {
                start.Environment[pair.Key] = pair.Value;
            }

            return start;
        }

        suite.Add("Native.Logger.StderrUtf8AndEmbeddedNul", async () =>
        {
            var start = StartInfo();
            start.ArgumentList.Add("--filter");
            start.ArgumentList.Add("RuntimeLoggerStderrOutputProbe");
            using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start logger probe");
            using var bytes = new MemoryStream();
            try
            {
                process.StandardInput.Close();
                Task copy = process.StandardError.BaseStream.CopyToAsync(bytes);
                await Task.WhenAll(copy, process.WaitForExitAsync()).WaitAsync(TimeSpan.FromSeconds(20));
                Verify.Equal(0, process.ExitCode, "stderr probe exit");
                string expected = "[Warning][Tests.Logger] 한\0X" + (OperatingSystem.IsWindows() ? "\r\n" : "\n");
                Verify.That(bytes.ToArray().SequenceEqual(Encoding.UTF8.GetBytes(expected)),
                    "stderr byte contents differ: " + Convert.ToHexString(bytes.ToArray()));
            }
            finally
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                }
            }
        });
        // Log::Fatal·assert는 비상 출력(stderr 직접 쓰기)과 설치된 LogSystem의 sink 출력으로 같은 줄을 두 번
        // 남기고 비정상 종료한다(설계 11절). 재귀 Fatal은 바로 종료한다.
        foreach ((string control, string line, int occurrences) in new[] {
            ("log-fatal-sync", "[Fatal][Tests.Fatal] fatal probe 42", 2),
            ("log-fatal-async", "[Fatal][Tests.Fatal] fatal probe 42", 2),
            ("log-assert", "[Fatal][Assert] Rocks invariant failed: value == 2", 2),
            ("log-fatal-recursive", "Recursive fatal error while reporting a fatal log record", 1) })
        {
            suite.Add("Native.Logger.Fatal." + control, async () =>
            {
                var start = StartInfo();
                start.ArgumentList.Add("--negative-control");
                start.ArgumentList.Add(control);
                using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start fatal probe");
                using var bytes = new MemoryStream();
                try
                {
                    process.StandardInput.Close();
                    Task copy = process.StandardError.BaseStream.CopyToAsync(bytes);
                    await Task.WhenAll(copy, process.WaitForExitAsync()).WaitAsync(TimeSpan.FromSeconds(60));
                    string text = Encoding.UTF8.GetString(bytes.ToArray());
                    Verify.That(process.ExitCode != 0, control + " did not terminate abnormally");
                    int count = 0;
                    for (int index = text.IndexOf(line, StringComparison.Ordinal); index >= 0;
                        index = text.IndexOf(line, index + line.Length, StringComparison.Ordinal))
                    {
                        ++count;
                    }

                    Verify.Equal(occurrences, count, control + " output: " + text);
                }
                finally
                {
                    if (!process.HasExited)
                    {
                        process.Kill(entireProcessTree: true);
                        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                    }
                }
            });
        }

        // 비정상 종료(platform::Fatal·접근 위반·stack overflow) 직전의 Async 기록이 종료 hook으로 파일에
        // 남는다(설계 14.1.8, L24·L25). 판정은 비정상 종료, 파일 내용, stderr다. 멈춘 sink 안의 Fatal은
        // flush 제한 시간 뒤에 끝난다(파일 없음). 시험 쪽 제한 시간(60초)은 flush 상한(2초)보다 넉넉하다.
        foreach ((string control, int mode, bool expectsFile, string stderrText) in new[] {
            ("log-crash-platform-fatal", 0, true, "log crash probe: platform fatal"),
            ("log-crash-access-violation", 1, true, ""),
            ("log-crash-stack-overflow", 2, true, ""),
            ("log-crash-stuck-sink", 3, false, "log crash probe: fatal inside a stuck sink") })
        {
            suite.Add("Native.Logger.Crash." + control, async () =>
            {
                string directory = Path.Combine(Path.GetTempPath(), "rocks-log-crash-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(directory);
                var start = StartInfo();
                start.ArgumentList.Add("--negative-control");
                start.ArgumentList.Add(control);
                start.ArgumentList.Add(directory);
                using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start crash probe");
                try
                {
                    process.StandardInput.Close();
                    using var errors = new MemoryStream();
                    Task copy = process.StandardError.BaseStream.CopyToAsync(errors);
                    await Task.WhenAll(copy, process.WaitForExitAsync()).WaitAsync(TimeSpan.FromSeconds(60));
                    // 20–23은 probe 준비 실패·크래시 없음이다(LoggerProcessProbe.cpp RunLogCrashProbe).
                    Verify.That(process.ExitCode != 0 && (process.ExitCode < 20 || process.ExitCode > 23),
                        control + " did not crash as intended: " + process.ExitCode);
                    string expected = "last record before crash mode " + mode;
                    bool found = Directory.GetFiles(directory, "Probe-*.log")
                        .Any(path => File.ReadAllText(path).Contains(expected, StringComparison.Ordinal));
                    Verify.That(found == expectsFile, control + " log file record: " + found);
                    string errorText = Encoding.UTF8.GetString(errors.ToArray());
                    Verify.That(stderrText.Length == 0 || errorText.Contains(stderrText, StringComparison.Ordinal),
                        control + " stderr: " + errorText);
                }
                finally
                {
                    if (!process.HasExited)
                    {
                        process.Kill(entireProcessTree: true);
                        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                    }

                    Directory.Delete(directory, recursive: true);
                }
            });
        }

        suite.Add("Native.Logger.StderrFailure", async () =>
        {
            var start = StartInfo();
            start.ArgumentList.Add("--logger-failure-probe");
            using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start logger probe");
            try
            {
                process.StandardInput.Close();
                await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(20));
                Verify.Equal(0, process.ExitCode, "stderr failure result");
            }
            finally
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                }
            }
        });
    }
}
