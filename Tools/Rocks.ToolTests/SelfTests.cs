using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class SelfTests
{
    private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    public static void Register(TestSuite suite, Options options)
    {
        ProcessReviewTests.Register(suite, options);
        suite.Add("Process.LargeUnicodePipesDrainWithoutDeadlock", async () =>
        {
            var result = await ProcessRunner.RunAsync(SelfProcess.Command(["child", "--large-output"], options.Root));
            Verify.Equal(17, result.ExitCode, "child failure exit");
            Verify.That(result.StdOut.Length >= 256 * 1024 && result.StdErr.Length >= 256 * 1024, "pipe output was truncated");
            Verify.Contains("출력🎮", result.StdOut, "stdout Unicode");
            Verify.Contains("오류🎮", result.StdErr, "stderr Unicode");
        });
        suite.Add("Process.StandardInputIsNotShellInterpreted", async () =>
        {
            string input = "한글 🎮 $HOME & | ; \"quotes\"\nline two";
            var result = await ProcessRunner.RunAsync(SelfProcess.Command(["child", "--echo-input"], options.Root, input: input));
            Verify.Equal(0, result.ExitCode, "stdin child exit");
            Verify.Equal(input, result.StdOut, "stdin bytes/text changed");
        });
        suite.Add("Policy.RepositoryRuntime", () => NoFailures(PolicyChecks.Runtime(options.Root)));
        suite.Add("Policy.RepositoryPlatform", () => NoFailures(PolicyChecks.PlatformBoundary(options.Root)));
        suite.Add("Policy.RepositoryWarnings", () => NoFailures(PolicyChecks.SourceWarnings(options.Root)));
        suite.Add("Policy.RepositoryPythonFreeMainPath", () => NoFailures(PolicyChecks.PythonFreeMainPath(options.Root)));
        suite.Add("Policy.RuntimePositiveAndNegativeControls", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/Core/Public/Probe.h", "#pragma once\nnamespace rocks { class Array; }\n");
            NoFailures(PolicyChecks.Runtime(temporary.Root));
            foreach (string banned in new[] { "#include <vector>", "# include \"string\"", "std :: vector<int> value;", "#include <filesystem>" })
            {
                temporary.Write("Engine/Core/Public/Probe.h", "// first line\n" + banned + "\n");
                var failures = PolicyChecks.Runtime(temporary.Root);
                Verify.Equal(1, failures.Count, "runtime negative count");
                Verify.Equal(2, failures[0].Line, "diagnostic line");
            }
        });
        suite.Add("Policy.PlatformPreciseBackendBoundary", () =>
        {
            using TemporaryDirectory temporary = new();
            foreach (string source in new[] { "#if defined(_WIN32)\n#endif", "#include <Windows.h>", "#include <pthread.h>", "void f(){CreateThread();}", "void f(){nanosleep();}" })
            {
                temporary.Write("Tests/Common/Probe.cpp", source);
                Verify.That(PolicyChecks.PlatformBoundary(temporary.Root).Count != 0, "public platform regression accepted");
            }
            temporary.Write("Tests/Common/Probe.cpp", "#pragma once\n");
            temporary.Write("Engine/PlatformBase/Private/Win64/Probe.cpp", "#include <Windows.h>\n");
            temporary.Write("Tests/Support/Private/Mac/Probe.mm", "#include <pthread.h>\n");
            NoFailures(PolicyChecks.PlatformBoundary(temporary.Root));
            temporary.Write("Engine/PlatformBase/Private/Win64Wrong/Probe.cpp", "#include <Windows.h>\n");
            Verify.Equal(1, PolicyChecks.PlatformBoundary(temporary.Root).Count, "prefix lookalike must not be allowlisted");
        });
        suite.Add("Policy.CompilerFlagsAndResponseFileControls", () =>
        {
            foreach (string valid in new[] { "clang-cl /W4 /WX /c probe.cpp", "clang++ -Wall -Wextra -Wpedantic -Werror -c probe.cpp",
                "\"C:\\compiler tools\\clang-cl.exe\" \"/W4\" \"/WX\" /c probe.cpp" })
                NoFailures(PolicyChecks.CompilerCommand(valid));
            foreach (string invalid in new[] { "clang-cl /W4 /c probe.cpp", "clang++ -Wall -c probe.cpp",
                "clang-cl /W4 /WX /WX-", "clang-cl /W4 /WX /wd4101", "clang-cl /W4 /WX /W0",
                "clang++ -Wall -Wextra -Wpedantic -Werror -Wno-error=unused-variable", "clang++ -Wall -Wextra -Wpedantic -Werror -w",
                "clang-cl /W4 /WX /D_CRT_SECURE_NO_WARNINGS", "clang++ -Wall -Werror", "clang++ -Wall -Wextra -Werror",
                "clang++ -Wall -Wpedantic -Werror", "clang-cl /W4 /WX /clang:-Wno-error=unused-variable", "clang-cl /W4 /WX @hidden.rsp",
                "clang-cl \"/W4 /WX" })
                Verify.That(PolicyChecks.CompilerCommand(invalid).Count != 0, "invalid warning profile accepted: " + invalid);
        });
        suite.Add("Policy.SourceSuppressionControls", () =>
        {
            using TemporaryDirectory temporary = new();
            foreach (string source in new[] { "#pragma warning(disable: 4101)", "#pragma clang diagnostic ignored \"-Wunused-variable\"",
                "#pragma GCC diagnostic ignored \"-Wunused-variable\"", "#define _CRT_SECURE_NO_WARNINGS", "#define _SCL_SECURE_NO_WARNINGS",
                "#pragma clang diagnostic warning \"-Wunused-variable\"", "#pragma GCC diagnostic warning \"-Wunused-variable\"" })
            {
                temporary.Write("Engine/Probe.cpp", source);
                Verify.That(PolicyChecks.SourceWarnings(temporary.Root).Count != 0, "warning suppression escaped audit");
            }
        });
        suite.Add("Policy.CompilationDatabaseUsesStructuredArguments", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("compile_commands.json", "[]");
            Verify.That(PolicyChecks.CompilationDatabase(temporary.FilePath("compile_commands.json")).Count != 0, "empty compilation database accepted");
            temporary.Write("compile_commands.json", JsonSerializer.Serialize(new[] { new { file = "a.cpp", arguments = new[] { "clang-cl", "/W4", "/WX", "/c", "a.cpp" } } }));
            NoFailures(PolicyChecks.CompilationDatabase(temporary.FilePath("compile_commands.json")));
            temporary.Write("compile_commands.json", "[{\"file\":\"a.cpp\",\"command\":\"clang-cl /W4 /c a.cpp\"}]");
            Verify.That(PolicyChecks.CompilationDatabase(temporary.FilePath("compile_commands.json")).Count != 0, "missing WX in database accepted");
        });
        suite.Add("Policy.PythonEntryDetectionAllowsWheelDownloadReferences", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Build/InstallFormatter.ps1", "$uri = 'https://files.pythonhosted.org/clang_format-21.1.6-py2.py3-none-win_amd64.whl'\n");
            temporary.Write("Build/reference.py", "import subprocess\n");
            NoFailures(PolicyChecks.PythonFreeMainPath(temporary.Root));
            temporary.Write(".github/workflows/build.yml", "run: python Build/build.py\n");
            Verify.That(PolicyChecks.PythonFreeMainPath(temporary.Root).Count != 0, "Python workflow entry escaped audit");
            temporary.Write(".github/workflows/build.yml", "run: dotnet run --project Tools/Rocks.Cli\n");
            temporary.Write("Tools/Rocks.Cli/Probe.cs", "var spec = new ProcessSpec(\"python.exe\", args, root);\n");
            Verify.That(PolicyChecks.PythonFreeMainPath(temporary.Root).Count != 0, "Python process invocation escaped audit");
        });
        suite.Add("Scaffold.PlanApplyValidatePreservesPlanAndRejectsOverwrite", () =>
        {
            using TemporaryDirectory temporary = new();
            string plan = temporary.FilePath("plan.json");
            var created = ScaffoldService.PlanModule(options.Root, "Example", temporary.Root, plan);
            byte[] original = File.ReadAllBytes(plan);
            ScaffoldService.Apply(plan); ScaffoldService.ValidateOutput(plan);
            Verify.That(File.ReadAllBytes(plan).AsSpan().SequenceEqual(original), "apply/validate changed the reviewed plan");
            Verify.That(File.Exists(Path.Combine(created.Target, "Example.cpp")), "module source missing");
            Verify.Throws<IOException>(() => ScaffoldService.Apply(plan));
            Verify.Throws<IOException>(() => ScaffoldService.PlanModule(options.Root, "Other", temporary.Root, plan));
            Verify.That(!Directory.EnumerateDirectories(temporary.Root, "*.stage-*").Any(), "owned staging directory leaked");
        });
        suite.Add("Scaffold.TraversalCollisionsAndHashTamperAreRejected", () =>
        {
            using TemporaryDirectory temporary = new();
            var original = ScaffoldService.PlanModule(options.Root, "Safe", temporary.Root, temporary.FilePath("plan.json"));
            foreach (string relative in new[] { "../escape.txt", "/escape.txt", "a\\b.txt", "C:escape.txt", "./x", "a//x", "CON.txt", "a. ", "AUX", "a/../x" })
            {
                var modified = original with { Files = new(StringComparer.Ordinal) { [relative] = "escape" }, Hashes = new(StringComparer.Ordinal) { [relative] = Hash("escape") } };
                Verify.Throws<InvalidDataException>(() => ScaffoldService.ValidatePlan(modified));
            }
            var changed = original with { Files = new(original.Files, StringComparer.Ordinal) };
            changed.Files[changed.Files.Keys.First()] += "tamper";
            Verify.Throws<InvalidDataException>(() => ScaffoldService.ValidatePlan(changed));
            var collision = original with { Files = new() { ["a"] = "", ["a/b"] = "" }, Hashes = new() { ["a"] = Hash(""), ["a/b"] = Hash("") } };
            Verify.Throws<InvalidDataException>(() => ScaffoldService.ValidatePlan(collision));
            Verify.That(!Directory.Exists(original.Target), "invalid plan created output");
        });
        suite.Add("Scaffold.MalformedJsonAndOutputMismatchAreRejected", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("duplicate.json", "{\"schema\":1,\"schema\":1}");
            Verify.Throws<InvalidDataException>(() => ScaffoldService.ReadPlan(temporary.FilePath("duplicate.json")));
            var plan = ScaffoldService.PlanModule(options.Root, "Example", temporary.Root, temporary.FilePath("plan.json"));
            ScaffoldService.Apply(temporary.FilePath("plan.json"));
            File.AppendAllText(Path.Combine(plan.Target, "Example.cpp"), "modified\n");
            Verify.Throws<InvalidDataException>(() => ScaffoldService.ValidateOutput(temporary.FilePath("plan.json")));
        });
        suite.Add("Scaffold.FailedStagingDoesNotPublishOrLeak", () =>
        {
            using TemporaryDirectory temporary = new();
            var original = ScaffoldService.PlanModule(options.Root, "Example", temporary.Root, temporary.FilePath("original.json"));
            string oversizedName = new('x', 300);
            var failed = original with { Files = new() { [oversizedName] = "value" }, Hashes = new() { [oversizedName] = Hash("value") } };
            temporary.Write("failed.json", JsonSerializer.Serialize(failed, Json));
            Verify.Throws<IOException>(() => ScaffoldService.Apply(temporary.FilePath("failed.json")));
            Verify.That(!Directory.Exists(original.Target), "failed staging published target");
            Verify.That(!Directory.EnumerateDirectories(temporary.Root, "*.stage-*").Any(), "failed staging leaked directory");
        });
        suite.Add("Scaffold.CliUsesCSharpEntryPoint", async () =>
        {
            using TemporaryDirectory temporary = new();
            string plan = temporary.FilePath("cli.json");
            var created = await ProcessRunner.RunAsync(SelfProcess.Command(["scaffold", "plan", "--root", options.Root,
                "--name", "CliExample", "--output", temporary.Root, "--plan", plan], options.Root));
            Verify.Equal(0, created.ExitCode, created.StdErr);
            Verify.Equal(0, (await ProcessRunner.RunAsync(SelfProcess.Command(["scaffold", "apply", "--plan", plan], options.Root))).ExitCode, "CLI apply");
            Verify.Equal(0, (await ProcessRunner.RunAsync(SelfProcess.Command(["scaffold", "validate", "--plan", plan], options.Root))).ExitCode, "CLI validate");
        });
        suite.Add("Evidence.SnapshotTracksInputContentAndExcludesGeneratedFiles", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "before\n");
            temporary.Write("Templates/Game/Source/Main.cpp", "template\n");
            var initial = SourceSnapshot.Capture(temporary.Root);
            temporary.Write("out/ignored.json", "generated\n");
            temporary.Write("Tools/Tool/bin/ignored.dll", "generated\n");
            temporary.Write("Build/Cache/ignored", "cache\n");
            Verify.Equal(initial, SourceSnapshot.Capture(temporary.Root), "generated artifact altered input digest");
            temporary.Write("Engine/source.h", "after\n");
            Verify.That(initial != SourceSnapshot.Capture(temporary.Root), "changed source content escaped snapshot");
        });
        suite.Add("Evidence.Exit17AndUnknownGitAreNeverSuccess", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            var evidence = await BuildEvidence.StartAsync(temporary.Root, "negative/exit17");
            var child = await evidence.RunAsync(SelfProcess.Command(["child", "--exit-code", "17"], temporary.Root));
            Verify.Equal(17, child.ExitCode, "child exit");
            var report = await evidence.FinishAsync();
            Verify.Equal("FAIL", report.Status, "failed step status without caller error");
            Verify.Equal(17, report.Steps[0].ExitCode, "failure provenance");
            Verify.Equal("NOT RUN", report.TestsStatus, "tests were not executed");
            Verify.That(report.GitAfter?.Dirty is null && !report.RunnerReportCurrent, "unknown Git/stale report claimed clean/current");
            Verify.That(File.Exists(evidence.EvidencePath), "failure evidence missing");
        });
        suite.Add("Evidence.SourceMutationProducesPersistedFailure", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "before\n");
            var evidence = await BuildEvidence.StartAsync(temporary.Root, "mutation");
            temporary.Write("Engine/source.h", "after\n");
            var report = await evidence.FinishAsync();
            Verify.That(report.Status == "FAIL" && !report.SourceStableDuringRun, "source mutation passed evidence");
            using JsonDocument persisted = JsonDocument.Parse(File.ReadAllText(evidence.EvidencePath));
            Verify.Equal("FAIL", persisted.RootElement.GetProperty("status").GetString(), "persisted status");
        });
        suite.Add("Evidence.StaleMissingAndFailingReportsCannotPass", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            foreach (string mode in new[] { "missing", "stale", "malformed", "failed", "inconsistent" })
            {
                var evidence = await BuildEvidence.StartAsync(temporary.Root, mode);
                evidence.BeginTests();
                string reportPath = Path.Combine(evidence.OutputDirectory, "foundation-report.json");
                if (mode != "missing")
                {
                    Directory.CreateDirectory(evidence.OutputDirectory);
                    File.WriteAllText(reportPath, mode == "malformed" ? "{" :
                        JsonSerializer.Serialize(new { failures = mode == "failed" ? 1 : 0,
                            tests = new[] { new { name = "case", status = mode is "failed" or "inconsistent" ? "FAIL" : "PASS" } } }));
                    if (mode == "stale") File.SetLastWriteTimeUtc(reportPath, DateTime.UnixEpoch.AddYears(30));
                }
                evidence.RecordStep("tests", ["native"], 0, TimeSpan.Zero, tests: true);
                var report = await evidence.FinishAsync();
                Verify.Equal("FAIL", report.Status, mode + " evidence status");
                if (mode is "missing" or "stale" or "malformed") Verify.That(!report.RunnerReportCurrent, "invalid report current");
            }
        });
        suite.Add("Evidence.SnapshotFailureDoesNotKeepOldPass", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            string evidencePath = temporary.FilePath("out/current/build-evidence.json");
            Directory.CreateDirectory(Path.GetDirectoryName(evidencePath)!);
            File.WriteAllText(evidencePath, "{\"status\":\"PASS\"}");
            using (var locked = new FileStream(temporary.FilePath("Engine/source.h"), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
                await Verify.ThrowsAsync<IOException>(async () => { await BuildEvidence.StartAsync(temporary.Root, "current"); });
            using var evidence = JsonDocument.Parse(File.ReadAllText(evidencePath));
            Verify.Equal("RUNNING", evidence.RootElement.GetProperty("status").GetString(), "snapshot failure left old PASS");
        });
        suite.Add("Evidence.CurrentPassingReportAndLifecycleGuards", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            string evidencePath = temporary.FilePath("out/current/build-evidence.json");
            Directory.CreateDirectory(Path.GetDirectoryName(evidencePath)!);
            File.WriteAllText(evidencePath, "{\"status\":\"PASS\"}");
            var evidence = await BuildEvidence.StartAsync(temporary.Root, "current");
            using (var running = JsonDocument.Parse(File.ReadAllText(evidencePath)))
                Verify.Equal("RUNNING", running.RootElement.GetProperty("status").GetString(), "previous PASS retained on start");
            evidence.SetConfiguration("Debug", "FoundationTests", null, "test-compiler", "fixture-version");
            Directory.CreateDirectory(evidence.OutputDirectory);
            string reportPath = Path.Combine(evidence.OutputDirectory, "foundation-report.json");
            File.WriteAllText(reportPath, "old report");
            evidence.BeginTests(reportPath);
            Verify.That(!File.Exists(reportPath), "old report was not removed before test execution");
            File.WriteAllText(reportPath, "{\"failures\":0,\"tests\":[{\"name\":\"probe\",\"status\":\"PASS\"}]}");
            evidence.RecordStep("tests", ["native"], 0, TimeSpan.FromMilliseconds(1), tests: true);
            var report = await evidence.FinishAsync();
            Verify.That(report.Status == "PASS" && report.RunnerReportCurrent && report.RunnerReportSha256?.Length == 64, "valid current report did not pass");
            using (var completed = JsonDocument.Parse(File.ReadAllText(evidencePath)))
                Verify.Equal("PASS", completed.RootElement.GetProperty("status").GetString(), "final evidence was not published");
            Verify.Throws<InvalidOperationException>(() => evidence.RecordStep("late", [], 0, TimeSpan.Zero));
            Verify.Throws<InvalidOperationException>(() => evidence.BeginTests());
            await Verify.ThrowsAsync<InvalidOperationException>(async () => { await evidence.FinishAsync(); });
        });
        suite.Add("Evidence.UnownedOutputPathsAreRejected", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            await Verify.ThrowsAsync<ArgumentException>(async () => { await BuildEvidence.StartAsync(temporary.Root, "../escape"); });
            var evidence = await BuildEvidence.StartAsync(temporary.Root, "safe");
            Verify.Throws<ArgumentException>(() => evidence.BeginTests(temporary.FilePath("outside.json")));
            Verify.That(!File.Exists(temporary.FilePath("outside.json")), "outside report was created");
        });
    }

    private static void NoFailures(IReadOnlyList<CheckFailure> failures) => Verify.That(failures.Count == 0, string.Join('\n', failures));
    private static string Hash(string text) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
}
