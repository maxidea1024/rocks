using System.Text.Json;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// self 시험 중 모듈 scaffold(ScaffoldService)와 빌드 증거(BuildEvidence) 시험이다.
internal static partial class SelfTests
{
    // scaffold 시험(Scaffold.*)과 증거 시험(Evidence.*)을 원래 순서로 등록한다.
    private static void RegisterScaffold(TestSuite suite, Options options)
    {
        suite.Add("Scaffold.PlanApplyValidatePreservesPlanAndRejectsOverwrite", () =>
        {
            using TemporaryDirectory temporary = new();
            string plan = temporary.FilePath("plan.json");
            var created = ScaffoldService.PlanModule(options.Root, "Example", temporary.Root, plan);
            byte[] original = File.ReadAllBytes(plan);
            ScaffoldService.Apply(plan);
            ScaffoldService.ValidateOutput(plan);
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
            temporary.Write("Programs/Tool/bin/ignored.dll", "generated\n");
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
                        JsonSerializer.Serialize(new
                        {
                            failures = mode == "failed" ? 1 : 0,
                            tests = new[] { new { name = "case", status = mode is "failed" or "inconsistent" ? "FAIL" : "PASS" } }
                        }));
                    if (mode == "stale")
                    {
                        File.SetLastWriteTimeUtc(reportPath, DateTime.UnixEpoch.AddYears(30));
                    }
                }

                evidence.RecordStep("tests", ["native"], 0, TimeSpan.Zero, tests: true);
                var report = await evidence.FinishAsync();
                Verify.Equal("FAIL", report.Status, mode + " evidence status");
                if (mode is "missing" or "stale" or "malformed")
                {
                    Verify.That(!report.RunnerReportCurrent, "invalid report current");
                }
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
            {
                await Verify.ThrowsAsync<IOException>(async () => { await BuildEvidence.StartAsync(temporary.Root, "current"); });
            }

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
            {
                Verify.Equal("RUNNING", running.RootElement.GetProperty("status").GetString(), "previous PASS retained on start");
            }

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
            {
                Verify.Equal("PASS", completed.RootElement.GetProperty("status").GetString(), "final evidence was not published");
            }

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
}
