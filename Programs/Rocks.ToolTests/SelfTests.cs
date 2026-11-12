using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;
using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

internal static class SelfTests
{
    private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    public static void Register(TestSuite suite, Options options)
    {
        BoundedFileTests.Register(suite);
        ApplePropertyLocationTests.Register(suite);
        IdentityInventoryTests.Register(suite);
        StableIdManifestTests.Register(suite);
        StableIdMergeTests.Register(suite);
        IdentityGitMergeTests.Register(suite, options);
        ProgramsPolicyReviewTests.Register(suite);
        ProcessReviewTests.Register(suite, options);
        ProcessHardTimeoutReviewTests.Register(suite, options);
        suite.Add("MetaPrefix.CommentLineSpliceIsRejectedDirectly", () =>
        {
            void CheckGap(string gap, bool allowed)
            {
                foreach (string name in new[] { "ROCKS_CLASS" })
                {
                    string source = name + "()" + gap + "class Object {};";
                    byte[] bytes = Encoding.UTF8.GetBytes(source);
                    int end = Encoding.UTF8.GetByteCount(source[..source.IndexOf("class Object", StringComparison.Ordinal)]);
                    if (allowed) PrefixSourceVerifier.Verify(bytes, 0, end);
                    else Verify.Throws<MetadataException>(() => PrefixSourceVerifier.Verify(bytes, 0, end));
                }
            }
            CheckGap("\n// ordinary \\ path #if\n", true);
            CheckGap("\n/* ordinary \\ path #include */\n", true);
            CheckGap("\n// line splice \\\n", false);
            CheckGap("\n// line splice \\\r\n", false);
            CheckGap("\n/* block splice \\\n continuation */\n", false);
            CheckGap("\n/* block splice \\\r\n continuation */\n", false);
            CheckGap("\n/* split terminator *\\\n/ #if 0\n class hidden {};\n#endif\n/* */\n", false);
        });
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
        ThirdPartyPolicyTests.Register(suite, options);
        suite.Add("Policy.RepositoryPlatform", () => NoFailures(PolicyChecks.PlatformBoundary(options.Root)));
        suite.Add("Policy.RepositoryWarnings", () => NoFailures(PolicyChecks.SourceWarnings(options.Root)));
        suite.Add("Policy.RepositoryOptimizationOverrides", () => NoFailures(PolicyChecks.OptimizationOverrides(options.Root)));
        suite.Add("Policy.OptimizationOverrideControls", () =>
        {
            using TemporaryDirectory temporary = new();
            // 정의 헤더와 테스트 소스는 허용한다.
            temporary.Write("Core/Source/Runtime/Base/Public/Base/Compiler.h", "#define ROCKS_DISABLE_OPTIMIZATION _Pragma(\"clang optimize off\")\n");
            temporary.Write("Core/Source/Runtime/Core/Tests/ProbeTests.cpp", "ROCKS_DISABLE_OPTIMIZATION\nvoid F() {}\nROCKS_ENABLE_OPTIMIZATION\n");
            temporary.Write("Engine/Source/Runtime/Probe/Private/Clean.cpp", "// ROCKS_DISABLE_OPTIMIZATIONS is a different name\nvoid G() {}\n");
            NoFailures(PolicyChecks.OptimizationOverrides(temporary.Root));
            temporary.Write("Engine/Source/Runtime/Probe/Probe.Build.cs", "// CodeOptimization.Always\nOptimization => CodeOptimization.Always;\n");
            NoFailures(PolicyChecks.OptimizationOverrides(temporary.Root));
            foreach (string source in new[] { "ROCKS_DISABLE_OPTIMIZATION", "ROCKS_ENABLE_OPTIMIZATION", "#pragma clang optimize off",
                "#  pragma  clang optimize on", "_Pragma(\"clang optimize off\")", "#pragma optimize(\"\", off)",
                "[[clang::optnone]] void F();", "__attribute__((optnone)) void F();" })
            {
                temporary.Write("Engine/Source/Runtime/Probe/Private/Probe.cpp", source + "\n");
                Verify.Equal(1, PolicyChecks.OptimizationOverrides(temporary.Root).Count, "optimization override escaped audit: " + source);
            }
            temporary.Write("Engine/Source/Runtime/Probe/Private/Probe.cpp", "void F();\n");
            temporary.Write("Engine/Source/Runtime/Probe/Probe.Build.cs", "protected override CodeOptimization Optimization => CodeOptimization.Never;\n");
            Verify.Equal(1, PolicyChecks.OptimizationOverrides(temporary.Root).Count, "committed Never optimization rule escaped audit");
            temporary.Write("Engine/Source/Runtime/Probe/Probe.Build.cs", "// rules\n");

            // 강제 인라인은 테스트 소스에서도 거부한다. 단어만 언급한 주석은 허용한다.
            temporary.Write("Engine/Source/Runtime/Probe/Private/Probe.cpp", "// UE FORCEINLINE과 always_inline은 쓰지 않는다\nvoid F();\n");
            NoFailures(PolicyChecks.OptimizationOverrides(temporary.Root));
            foreach (string source in new[] { "inline __attribute__((always_inline)) void F();", "__attribute__ ((__always_inline__)) void F();",
                "[[clang::always_inline]] void F();", "[[gnu::always_inline]] void F();", "__forceinline void F();" })
            {
                foreach (string file in new[] { "Engine/Source/Runtime/Probe/Private/Probe.cpp", "Core/Source/Runtime/Core/Tests/ProbeTests.cpp" })
                {
                    temporary.Write("Engine/Source/Runtime/Probe/Private/Probe.cpp", "void F();\n");
                    temporary.Write("Core/Source/Runtime/Core/Tests/ProbeTests.cpp", "void G();\n");
                    temporary.Write(file, source + "\n");
                    var failures = PolicyChecks.OptimizationOverrides(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].ToString().Contains("forced-inline", StringComparison.Ordinal),
                        $"forced inline escaped audit in {file}: {source}");
                }
            }
        });
        suite.Add("Policy.RepositoryCompilerBuiltins", () => NoFailures(PolicyChecks.CompilerBuiltins(options.Root)));
        suite.Add("Policy.CompilerBuiltinControls", () =>
        {
            using TemporaryDirectory temporary = new();
            // 지정 헤더는 자기 계열만 쓴다. 기능 검사 매크로와 builtin이 아닌 이름은 허용한다.
            const string memory = "Core/Source/Runtime/PlatformBase/Public/Platform/Memory.h";
            const string atomics = "Core/Source/Runtime/PlatformBase/Public/Platform/Atomics.h";
            temporary.Write(memory, "inline void C(void* d, const void* s, usize n) { __builtin_memcpy(d, s, n); }\n" +
                "constexpr usize L(const char* t) { return __builtin_strlen(t); }\n");
            temporary.Write(atomics, "enum class MemoryOrder { Relaxed = __ATOMIC_RELAXED };\n" +
                "template <class T> T Load(const T* a) { T r; __atomic_load(a, &r, __ATOMIC_ACQUIRE); return r; }\n");
            temporary.Write("Core/Source/Runtime/Base/Public/Base/Compiler.h", "#define ROCKS_UNREACHABLE() __builtin_unreachable()\n");
            temporary.Write("Engine/Source/Runtime/Probe/Private/Clean.cpp",
                "#if __has_builtin(x)\n#endif\nvoid builtin_memcpy(); int my__builtin_x;\n");
            NoFailures(PolicyChecks.CompilerBuiltins(temporary.Root));

            // 지정 헤더라도 다른 계열은 위반이다.
            foreach (string source in new[] { "__atomic_fetch_add(&x, 1, 0);", "__builtin_bswap32(x);" })
            {
                temporary.Write(memory, source + "\n");
                Verify.Equal(1, PolicyChecks.CompilerBuiltins(temporary.Root).Count, "helper allowance too wide: " + source);
            }
            temporary.Write(memory, "#pragma once\n");

            // 그 밖의 소스는 테스트까지 모두 위반이다.
            foreach (string source in new[] { "__builtin_memcpy(d, s, n);", "x = __builtin_bit_cast(float, y);",
                "__atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);", "__sync_synchronize();",
                "__c11_atomic_load(&x, 0);", "int order = __ATOMIC_SEQ_CST;", "auto o = __builtin_offsetof(T, m);" })
            {
                foreach (string file in new[] { "Engine/Source/Runtime/Probe/Private/Probe.cpp",
                    "Core/Source/Runtime/Core/Tests/ProbeTests.cpp", "Programs/Sandbox/Private/Probe.h" })
                {
                    temporary.Write(file, "// first line\n" + source + "\n");
                    var failures = PolicyChecks.CompilerBuiltins(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].Line == 2 &&
                        failures[0].ToString().Contains("compiler-builtin", StringComparison.Ordinal),
                        $"builtin escaped audit in {file}: {source}");
                    temporary.Write(file, "// clean\n");
                }
            }
        });
        suite.Add("Policy.RepositoryPythonFreeMainPath", () => NoFailures(PolicyChecks.PythonFreeMainPath(options.Root)));
        suite.Add("Policy.RuntimePositiveAndNegativeControls", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Core/Source/Runtime/Core/Public/Probe.h", "#pragma once\nnamespace rocks { class Array; }\n");
            NoFailures(PolicyChecks.Runtime(temporary.Root));
            foreach (string banned in new[] { "#include <vector>", "# include \"string\"", "std :: vector<int> value;", "#include <filesystem>" })
            {
                temporary.Write("Core/Source/Runtime/Core/Public/Probe.h", "// first line\n" + banned + "\n");
                var failures = PolicyChecks.Runtime(temporary.Root);
                Verify.Equal(1, failures.Count, "runtime negative count");
                Verify.Equal(2, failures[0].Line, "diagnostic line");
            }
        });
        suite.Add("Policy.RuntimeExcludesModuleTestsButCoversProgramsAndPlugins", () =>
        {
            // 모듈 Tests/와 테스트 runner는 STL 정책 밖이다(STL 호환 테스트가 STL을 쓴다).
            using TemporaryDirectory temporary = new();
            temporary.Write("Core/Source/Runtime/Core/Tests/StlProbe.cpp", "#include <vector>\n");
            temporary.Write("Programs/FoundationTests/Private/StlProbe.cpp", "#include <vector>\n");
            NoFailures(PolicyChecks.Runtime(temporary.Root));
            // 플러그인과 프로그램의 런타임 소스는 검사한다.
            temporary.Write("Engine/Plugins/Sample/Example/Source/Example/Private/Probe.cpp", "#include <vector>\n");
            temporary.Write("Programs/Commandlet/Private/Common/Probe.cpp", "#include <map>\n");
            Verify.Equal(2, PolicyChecks.Runtime(temporary.Root).Count, "plugin or program STL escaped runtime policy");
        });
        suite.Add("Policy.RuntimeTupleProtocolAllowanceIsNarrow", () =>
        {
            using TemporaryDirectory temporary = new();
            const string tuple = "Core/Source/Runtime/Core/Public/Core/Tuple.h";
            temporary.Write(tuple, "#pragma once\n#include <utility>\n" +
                "template <typename... T> struct std::tuple_size<rocks::Tuple<T...>> {};\n" +
                "template <decltype(sizeof(0)) I, typename... T> struct std :: tuple_element<I, rocks::Tuple<T...>> {};\n");
            NoFailures(PolicyChecks.Runtime(temporary.Root));
            // 같은 파일이라도 다른 std 사용·다른 STL 헤더는 위반이다.
            foreach (string banned in new[] { "std::vector<int> value;", "#include <tuple>",
                "struct std::tuple_size<X> : std::integral_constant<int, 1> {};" })
            {
                temporary.Write(tuple, "#pragma once\n" + banned + "\n");
                Verify.Equal(1, PolicyChecks.Runtime(temporary.Root).Count, "tuple allowance too wide: " + banned);
            }
            // 다른 파일에서는 tuple protocol 이름과 <utility>도 위반이다.
            temporary.Write(tuple, "#pragma once\n");
            temporary.Write("Core/Source/Runtime/Core/Public/Core/Other.h", "#include <utility>\nstruct std::tuple_size<int>;\n");
            Verify.Equal(2, PolicyChecks.Runtime(temporary.Root).Count, "tuple allowance leaked to other file");
        });
        suite.Add("Policy.PlatformPreciseBackendBoundary", () =>
        {
            using TemporaryDirectory temporary = new();
            foreach (string source in new[] { "#if defined(_WIN32)\n#endif", "#include <Windows.h>", "#include <pthread.h>", "void f(){CreateThread();}", "void f(){nanosleep();}" })
            {
                temporary.Write("Core/Source/Runtime/Core/Tests/Probe.cpp", source);
                Verify.That(PolicyChecks.PlatformBoundary(temporary.Root).Count != 0, "public platform regression accepted");
            }
            temporary.Write("Core/Source/Runtime/Core/Tests/Probe.cpp", "#pragma once\n");
            temporary.Write("Core/Source/Runtime/PlatformBase/Private/Win64/Probe.cpp", "#include <Windows.h>\n");
            temporary.Write("Core/Source/Developer/TestSupport/Private/Mac/Probe.mm", "#include <pthread.h>\n");
            NoFailures(PolicyChecks.PlatformBoundary(temporary.Root));
            temporary.Write("Core/Source/Runtime/PlatformBase/Private/Win64Wrong/Probe.cpp", "#include <Windows.h>\n");
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
                "#pragma clang diagnostic warning \"-Wunused-variable\"", "#pragma GCC diagnostic warning \"-Wunused-variable\"",
                "#define QUIET _Pragma(\"clang diagnostic ignored \\\"-Wunused-variable\\\"\")",
                "#define QUIET __pragma(warning(disable: 4101))" })
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
            temporary.Write("compile_commands.json", JsonSerializer.Serialize(new[] { new { file = "a.cpp", arguments = new[] { "clang-cl", "/W4", "/WX", "/clang:-std=c++23", "/c", "/TP", "a.cpp" } } }));
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
            temporary.Write(".github/workflows/build.yml", "run: dotnet run --project Programs/Rocks.Cli\n");
            temporary.Write("Programs/Rocks.Cli/Probe.cs", "var spec = new ProcessSpec(\"python.exe\", args, root);\n");
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
