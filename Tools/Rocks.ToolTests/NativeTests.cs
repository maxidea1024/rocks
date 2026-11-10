using System.Text.Json;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class NativeTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        string executable = Path.GetFullPath(options.Required("native-executable"));
        if (!File.Exists(executable)) throw new FileNotFoundException("Native test executable missing", executable);
        Task<ProcessResult> Run(params string[] args) => ProcessRunner.RunAsync(new(executable, args, options.Root,
            environment, Timeout: TimeSpan.FromSeconds(20)));
        foreach (string control in new[] { "check", "require" })
            suite.Add("Native.Negative." + control, async () =>
            {
                var result = await Run("--negative-control", control);
                Verify.Equal(1, result.ExitCode, "negative control exit");
                Verify.Contains("CHECK FAILED: false", result.StdErr, "negative control diagnostic");
            });
        suite.Add("Native.InvalidFilterAndRepeat", async () =>
        {
            Verify.Equal(2, (await Run("--filter", "ThisTestDoesNotExist")).ExitCode, "unmatched test filter");
            Verify.Equal(2, (await Run("--repeat", "0")).ExitCode, "zero repeat");
        });
        suite.Add("Native.SortedUniqueInventory", async () =>
        {
            var result = await Run("--list");
            Verify.Equal(0, result.ExitCode, "list exit");
            string[] names = result.StdOut.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries);
            Verify.That(names.Length != 0 && names.SequenceEqual(names.Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal)), "list must be sorted/unique/nonempty");
        });
        suite.Add("Native.SeedRepeatJsonAndReportFailure", async () =>
        {
            using TemporaryDirectory temporary = new();
            string reportPath = temporary.FilePath("report.json");
            var result = await Run("--filter", "Harness_SeedIsAccessible", "--seed", "914", "--repeat", "3", "--json", reportPath);
            Verify.Equal(0, result.ExitCode, result.StdErr);
            using JsonDocument report = JsonDocument.Parse(File.ReadAllText(reportPath));
            Verify.Equal(914, report.RootElement.GetProperty("seed").GetInt32(), "seed");
            Verify.Equal(3, report.RootElement.GetProperty("repeat").GetInt32(), "repeat");
            Verify.Equal(0, report.RootElement.GetProperty("failures").GetInt32(), "failures");
            var tests = report.RootElement.GetProperty("tests");
            Verify.Equal(1, tests.GetArrayLength(), "filter case count");
            Verify.Equal("PASS", tests[0].GetProperty("status").GetString(), "case status");
            var unavailable = await Run("--filter", "Harness_SeedIsAccessible", "--json", temporary.Root);
            Verify.Equal(2, unavailable.ExitCode, "unwritable report exit");
            Verify.Contains("Cannot open JSON report", unavailable.StdErr, "unwritable report diagnostic");
            Verify.That(!unavailable.StdOut.Contains("[PASS]", StringComparison.Ordinal), "report failure must precede test pass output");
        });
        suite.Add("Native.ConcurrentFailureCounting", async () =>
        {
            var result = await Run("--negative-control", "parallel");
            Verify.Equal(1, result.ExitCode, "parallel negative exit");
            Verify.Equal(200, result.StdErr.Split("CHECK FAILED: parallel-negative-control", StringSplitOptions.None).Length - 1, "parallel failure count");
        });
        foreach ((string control, string diagnostic) in new[] { ("assert-stderr-lock", "Rocks invariant failed:"),
            ("thread-unjoined", "Thread"), ("name-live-lease", "NamePool destroyed with active operations or leases") })
            suite.Add("Native.Fatal." + control, async () =>
            {
                var result = await Run("--negative-control", control);
                Verify.That(result.ExitCode != 0, "fatal negative control succeeded");
                Verify.Contains(diagnostic, result.StdErr, "fatal diagnostic");
                if (control == "thread-unjoined") Verify.Contains("join", result.StdErr.ToLowerInvariant(), "unjoined diagnostic");
            });
        if (options.Asan)
            suite.Add("Native.AsanDetectsActualUseAfterFree", async () =>
            {
                var result = await Run("--negative-control", "asan-uaf");
                Verify.That(result.ExitCode != 0, "ASan negative control succeeded");
                Verify.Contains("AddressSanitizer: heap-use-after-free", result.StdErr, "actual ASan report");
            });
        if (options.Optional("compiler") is string compiler)
            suite.Add("Native.CompilerRejectsIntentionalWarning", () => WarningProbe.VerifyAsync(compiler, options.Root, environment));
        if (options.Optional("compile-commands") is string database)
            suite.Add("Native.CompileCommandsWarningPolicy", () =>
            {
                var failures = PolicyChecks.CompilationDatabase(database);
                Verify.That(failures.Count == 0, string.Join('\n', failures));
            });
    }
}
