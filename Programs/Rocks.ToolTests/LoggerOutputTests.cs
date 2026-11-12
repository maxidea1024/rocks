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
                string expected = "[Warning][27] 한\0X" + (OperatingSystem.IsWindows() ? "\r\n" : "\n");
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
