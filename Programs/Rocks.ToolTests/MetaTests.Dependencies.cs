using System.Text.Json.Nodes;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static partial class MetaTests
{
    // MetaCompiler 의존 기록·단계 보고(Docs/Design/BuildReuse 2.4·2.8절, 시험 T6).
    // T6-1: clang-cl·clang++는 -fsyntax-only·-ast-dump=json·표준 입력에서도 depfile을 쓰고 표준 입력은 적지 않는다(ClangInvocation).
    // T6-2·T6-4·T6-5: 실제 모듈 생성 한 번의 의존 기록에서 clang 실행 수와 depfile 수가 같고, 연 헤더·존재 확인이 있고, 자기 출력·검증
    // 임시 디렉터리·표준 입력은 없다. 단계 보고에 단계 시간과 clang 실행 수가 있다.
    // T6-3: depfile을 쓰지 않는 "컴파일러"는 RMC3200으로 거부된다.
    private static void RegisterDependencyRecords(MetaContext context)
    {
        var (suite, options, environment, generator, compiler) = context;
        suite.Add("Meta.ClangInvocationReadsDepfileFromStandardInput", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("include/Included.h", "struct Included { int value; };\n");
            string resolved = NativeEnvironment.FindExecutable(compiler, environment);
            bool clangCl = Path.GetFileNameWithoutExtension(resolved).Contains("clang-cl", StringComparison.OrdinalIgnoreCase);
            string include = temporary.FilePath("include");
            List<string> arguments = clangCl
                ? ["--no-default-config", "/nologo", "/TP", "/clang:-fsyntax-only", "/clang:-Xclang", "/clang:-ast-dump=json", "/I" + include, "-"]
                : ["-x", "c++", "-fsyntax-only", "-Xclang", "-ast-dump=json", "-I", include, "-"];
            MetaRunRecorder recorder = MetaRunRecorder.Begin();
            ProcessResult result = await ClangInvocation.RunAsync(new(resolved, arguments, temporary.Root, environment,
                "#include \"Included.h\"\nIncluded value;\n", ProbeTimeout), clangCl);
            Verify.Equal(0, result.ExitCode, result.StdErr);
            string record = temporary.FilePath("record.json");
            recorder.WriteDependencyRecord(record);
            JsonNode document = JsonNode.Parse(File.ReadAllText(record))!;
            string[] files = [.. document["files"]!.AsArray().Select(node => node!.GetValue<string>())];
            Verify.That(document["clangRuns"]!.GetValue<int>() == 1 && document["depfiles"]!.GetValue<int>() == 1, "one clang run must read one depfile");
            Verify.That(files.Any(file => file.EndsWith("Included.h", StringComparison.OrdinalIgnoreCase)), "the opened header must be recorded: " + string.Join(", ", files));
            Verify.That(!files.Any(file => Path.GetFileName(file) is "-" or "<stdin>"), "standard input must not be recorded");
        });
        suite.Add("Meta.ClangInvocationRejectsMissingDepfile", async () =>
        {
            using TemporaryDirectory temporary = new();
            // 아무것도 쓰지 않고 성공하는 "컴파일러"다.
            ProcessSpec silent = OperatingSystem.IsWindows()
                ? new(Path.Combine(Environment.SystemDirectory, "cmd.exe"), ["/c", "exit", "0"], temporary.Root)
                : new("/bin/sh", ["-c", "exit 0"], temporary.Root);
            MetaRunRecorder.Begin();
            try
            {
                await ClangInvocation.RunAsync(silent, OperatingSystem.IsWindows());
                Verify.That(false, "a clang run without a dependency file was accepted");
            }
            catch (MetadataException error)
            {
                Verify.Equal(ClangInvocation.MissingDependenciesCode, error.Code, error.Message);
            }
        });
        // 치명 오류로 멈춘 실패 실행은 depfile을 쓰지 않는다(탐침이 출력 루트를 뺀 순서로 컴파일할 때 — 실측). 탐침 판정을 깨지 않고 결과를 그대로
        // 돌려주되, 그 실행의 의존 기록은 쓸 수 없다고 남긴다(빌드가 그 모듈을 기록하지 않는다).
        suite.Add("Meta.ClangInvocationMarksFailedRunWithoutDepfileUnrecordable", async () =>
        {
            using TemporaryDirectory temporary = new();
            ProcessSpec failing = OperatingSystem.IsWindows()
                ? new(Path.Combine(Environment.SystemDirectory, "cmd.exe"), ["/c", "exit", "1"], temporary.Root)
                : new("/bin/sh", ["-c", "exit 1"], temporary.Root);
            MetaRunRecorder recorder = MetaRunRecorder.Begin();
            ProcessResult result = await ClangInvocation.RunAsync(failing, OperatingSystem.IsWindows());
            Verify.Equal(1, result.ExitCode, "the failing run's exit code must be returned unchanged");
            string record = temporary.FilePath("record.json");
            recorder.WriteDependencyRecord(record);
            JsonNode document = JsonNode.Parse(File.ReadAllText(record))!;
            Verify.That(document["unrecordable"]?.GetValue<string>() is string reason && reason.Contains("no dependency file", StringComparison.Ordinal),
                "a failing run without a dependency file must make the record unusable");
            Verify.That(document["clangRuns"]!.GetValue<int>() == 1 && document["depfiles"]!.GetValue<int>() == 0, "the run must count without a depfile");
        });
        suite.Add("Meta.DependencyRecordCoversEveryClangRun", async () =>
        {
            using Probe probe = new(options, environment, generator, compiler);
            probe.WriteHeader("Extra.h", "#pragma once\nnamespace probe { struct Extra { int value = 0; }; }\n");
            string record = probe.HeaderPath("dependencies.json");
            string phases = probe.HeaderPath("phases.json");
            string source = Preamble.Replace("namespace probe {\n", "#include \"Extra.h\"\nnamespace probe {\n", StringComparison.Ordinal) +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n";
            await probe.GenerateAsync(source, extra: ["--dependency-record", record, "--phase-report", phases]);
            JsonNode document = JsonNode.Parse(File.ReadAllText(record))!;
            int runs = document["clangRuns"]!.GetValue<int>();
            Verify.That(runs >= 3 && runs == document["depfiles"]!.GetValue<int>(), $"clang runs ({runs}) and depfiles must match and cover AST, variant and probes");
            Verify.That(document["unrecordable"] is null, "a build-style run must be recordable");
            string[] files = [.. document["files"]!.AsArray().Select(node => node!.GetValue<string>())];
            Verify.That(files.Any(file => file.EndsWith("Extra.h", StringComparison.OrdinalIgnoreCase)), "a header included by the input must be recorded");
            string outputRoot = MetaCompilerArguments.OutputRoot(probe.HeaderPath(""));
            Verify.That(!files.Any(file => DirectoryPath.IsAtOrUnder(file, outputRoot)), "the run's own outputs must not be recorded");
            Verify.That(!files.Any(file => file.Contains(".rocks-meta-validation-", StringComparison.OrdinalIgnoreCase)), "validation staging must not be recorded");
            Verify.That(!files.Any(file => Path.GetFileName(file) is "-" or "<stdin>"), "standard input must not be recorded");
            Verify.That(document["probes"]!.AsArray().Count != 0, "the include binding and header resolution probes must be recorded");
            // 모듈 public 헤더 열거는 불완전한 NATIVE 대상이 있을 때만 돈다(CompilerDriver.ModulePublicHeaders — 이 원본에는 없다). 있으면 결과까지
            // 적혀야 한다.
            Verify.That(document["enumerations"]!.AsArray().All(node => node!["results"] is JsonArray), "every enumeration must carry its results");
            JsonNode report = JsonNode.Parse(File.ReadAllText(phases))!;
            JsonObject phaseTimes = report["phases"]!.AsObject();
            foreach (string phase in new[] { "environment", "ast", "game-ast", "probes", "format", "validate", "write" })
            {
                Verify.That(phaseTimes.ContainsKey(phase), "phase report misses " + phase);
            }

            Verify.Equal(runs, report["clangRuns"]!.GetValue<int>(), "phase report clang runs");
        });
    }
}
