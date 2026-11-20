using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 묶음 크기 실측(<c>unity-profile</c>, Docs/Design/UnityBuildPch.ko.md 12절)의 부품을 검증한다: 조합 목록과 모듈 재정의, 출력 디렉터리
/// 확인과 삭제 경로 확인, 프로세스 계측(<see cref="ProcessSpec.MeasureUsage"/> → <see cref="NativeActionResult"/>), 빌드 결과에서 행
/// 뽑기, 표 출력. 계측 값의 전달은 주입한 프로세스로 정확히 단언하고(실제 CPU 시간에 기대지 않는다), 실제 프로세스에서는 값이 있는지만
/// 본다.
/// </summary>
internal static class UnityProfileTests
{
    private const long CpuMilliseconds = 1234;

    private const long PeakBytes = 5678;

    private const long CommitBytes = 9012;

    private const int Kibibyte = 1024;

    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        CasesAndOverrides();
        pass("unity-profile: cases follow the spec order and override one module");
        OutputsAndDeletedPaths(root);
        pass("unity-profile: only a claimed output directory is accepted and only module directories under it are deleted");
        await ProcessUsageAsync(request, root);
        pass("unity-profile: process usage reaches the action result only when requested");
        await MeasuringKeepsTheCacheAsync(request, root);
        pass("unity-profile: measuring processes changes no action key, compile database or tool build ID");
        await RowsAsync(request, root);
        pass("unity-profile: a clean compile of one module yields a row, a cached compile is rejected, failed cases stay in the table");
    }

    private static void CasesAndOverrides()
    {
        IReadOnlyList<UnityProfileCase> cases = UnityProfile.Cases(UnityProfile.DefaultAnchorFiles, UnityProfile.DefaultBatchKibibytes);
        Check(cases.Count == 13 && cases[0] == new UnityProfileCase("unity off", false, 0, 0) &&
            cases[1] == new UnityProfileCase("anchor 8, 256 KiB", true, 8, 256 * Kibibyte) &&
            cases[3] == new UnityProfileCase("anchor 8, 512 KiB", true, 8, 512 * Kibibyte) &&
            cases[^1] == new UnityProfileCase("anchor 32, 512 KiB", true, 32, 512 * Kibibyte) &&
            cases.Select(item => item.Name).Distinct().Count() == cases.Count,
            "The default cases must be 'unity off' and anchor files 8, 16, 24, 32 times 256, 384, 512 KiB.");
        Check(UnityProfile.Cases([16], [384]).Count == 2, "A custom list must give 'unity off' and one case per pair.");
        foreach ((int[] files, int[] sizes) in new[] { (Array.Empty<int>(), new[] { 256 }), (new[] { 8 }, Array.Empty<int>()), (new[] { 0 }, new[] { 256 }),
            (new[] { 8 }, new[] { -1 }), (new[] { 8 }, new[] { int.MaxValue }) })
        {
            Rejects<ArgumentException>(() => UnityProfile.Cases(files, sizes), "positive", "An empty or non-positive case list must be rejected.");
        }

        // 재정의는 그 모듈의 유니티 설정만 바꾼다. 끄는 조합은 리플렉션 생성 소스 묶기도 끄고 구간·상한은 건드리지 않는다. 켜는 조합은
        // 생성 소스를 묶는지를 모듈 설정 그대로 둔다(두 값은 두 묶음 구성이 함께 쓴다).
        NativeModuleInput module = new() { Name = "Probe", Unity = UnityMode.Default, UnityExclude = ["keep.cpp"] };
        NativeModuleInput on = UnityProfile.Apply(module, cases[1]);
        NativeModuleInput off = UnityProfile.Apply(module, cases[0]);
        Check(module.ReflectionUnity && on.Unity == UnityMode.On && on.ReflectionUnity && on.UnityAnchorFiles == 8 && on.UnityBatchBytes == 256 * Kibibyte &&
            on.UnityExclude.SequenceEqual(["keep.cpp"]) && !UnityProfile.Apply(module with { ReflectionUnity = false }, cases[1]).ReflectionUnity &&
            off.Unity == UnityMode.Off && !off.ReflectionUnity && off.UnityAnchorFiles == module.UnityAnchorFiles &&
            off.UnityBatchBytes == module.UnityBatchBytes && off.UnityExclude.SequenceEqual(["keep.cpp"]),
            "A case must override only the unity settings of the module, and 'unity off' must also unbatch its reflection sources.");
    }

    private static void OutputsAndDeletedPaths(string root)
    {
        string outputs = Directory.CreateDirectory(Path.Combine(root, "unity-profile-outputs", "out")).FullName;
        // 이름이 접두로 시작하지 않는 출력(평소 빌드 출력의 이름)과 접두뿐인 이름은 거부한다. 디렉터리도 만들지 않는다.
        foreach (string name in new[] { "dotnet-windows-foundation-debug", "profile", UnityProfile.OutputPrefix.TrimEnd('-'), UnityProfile.OutputPrefix })
        {
            string wrong = Path.Combine(outputs, name);
            Rejects<ArgumentException>(() => UnityProfile.ClaimOutput(wrong), "must start with", "A wrongly named output must be rejected: " + name);
            Rejects<ArgumentException>(() => UnityProfile.RequireClaimedOutput(wrong), "must start with", "A wrongly named output must fail the check: " + name);
            Check(!Directory.Exists(wrong), "A rejected output must not be created: " + name);
        }

        // 새 디렉터리는 표지와 함께 만든다. 표지가 있기 전에는 build 흐름의 확인이 실패한다. 다시 불러도 된다.
        string fresh = Path.Combine(outputs, UnityProfile.OutputPrefix + "foundation-debug");
        Rejects<ArgumentException>(() => UnityProfile.RequireClaimedOutput(fresh), "marker", "An unclaimed output must fail the check.");
        UnityProfile.ClaimOutput(fresh);
        UnityProfile.ClaimOutput(fresh + Path.DirectorySeparatorChar);
        UnityProfile.RequireClaimedOutput(fresh);
        Check(File.Exists(Path.Combine(fresh, UnityProfile.MarkerFileName)), "Claiming an output must leave the marker file.");
        // 표지가 있으면 증분 기록이 생긴 뒤에도 받는다(이 명령이 빌드한 출력이다).
        File.WriteAllText(Path.Combine(fresh, "native-cache.json"), "{}");
        UnityProfile.ClaimOutput(fresh);
        // 표지 없이 증분 기록이 있는 디렉터리는 다른 빌드의 출력이다. 이름이 맞아도 거부하고 표지를 만들지 않는다.
        string foreign = Directory.CreateDirectory(Path.Combine(outputs, UnityProfile.OutputPrefix + "foreign")).FullName;
        File.WriteAllText(Path.Combine(foreign, "native-cache.json"), "{}");
        Rejects<ArgumentException>(() => UnityProfile.ClaimOutput(foreign), "another build", "An output of another build must be rejected.");
        Check(!File.Exists(Path.Combine(foreign, UnityProfile.MarkerFileName)), "A rejected output must not be marked.");
        // 비어 있는(증분 기록이 없는) 기존 디렉터리는 받는다.
        string empty = Directory.CreateDirectory(Path.Combine(outputs, UnityProfile.OutputPrefix + "empty")).FullName;
        UnityProfile.ClaimOutput(empty);
        UnityProfile.RequireClaimedOutput(empty);

        // 지우는 경로: 그 모듈의 object·묶음 디렉터리와 PCH 디렉터리이고, 모두 출력 디렉터리 아래다.
        IReadOnlyList<string> directories = UnityProfile.ModuleOutputDirectories(fresh, "Core");
        Check(directories.SequenceEqual([Path.Combine(fresh, "obj", "Core"), UnityBuild.DirectoryOf(fresh, "Core"), Path.Combine(fresh, PchBuild.DirectoryName)]) &&
            directories.All(directory => DirectoryPath.IsStrictlyUnder(directory, fresh)), "The deleted directories: " + string.Join(", ", directories));
        // 경로 조각 하나가 아닌 모듈 이름은 출력 밖이나 출력 전체를 가리킬 수 있으므로 거부한다.
        foreach (string name in new[] { "", " ", ".", "..", "../Other", "..\\Other", "Core/..", "a/b", "a\\b", Path.Combine(outputs, "abs") })
        {
            Rejects<ArgumentException>(() => UnityProfile.ModuleOutputDirectories(fresh, name), "module name", "A module name that is not one path segment: '" + name + "'");
        }
    }

    private static async Task ProcessUsageAsync(NativeBuildRequest request, string root)
    {
        string outputRoot = Directory.CreateDirectory(Path.Combine(root, "unity-profile-usage")).FullName;
        string input = Path.Combine(outputRoot, "input.cpp");
        File.WriteAllText(input, "input");
        NativeAction Action(string name) => new(name, "Fixture", "Compile", "fake", ["{OUTPUT0}"], [Path.Combine(outputRoot, name + ".obj")], [input],
            [], [], [], []);
        var store = new IncrementalStore(outputRoot);
        bool? asked = null;
        Task<ProcessResult> Measured(ProcessSpec spec, CancellationToken _)
        {
            asked = spec.MeasureUsage;
            File.WriteAllText(spec.Arguments[0], "object");
            return Task.FromResult(new ProcessResult(0, "", "")
            {
                Usage = spec.MeasureUsage ? new(TimeSpan.FromMilliseconds(CpuMilliseconds), PeakBytes, CommitBytes) : null
            });
        }

        // 요청하지 않으면 실행기는 계측을 청하지 않고 결과도 비어 있다. 보고서(JSON)에도 필드가 실리지 않는다.
        NativeActionResult plain = await Executor.RunAsync(request, Action("plain"), outputRoot, "fixture", store, CancellationToken.None, Measured);
        Check(asked == false && plain.Executed && !plain.ProcessMeasured && plain.CpuMilliseconds == 0 && plain.PeakMemoryBytes == 0 && plain.PeakCommitBytes == 0,
            "Process usage must not be requested or recorded by default.");
        string plainJson = System.Text.Json.JsonSerializer.Serialize(plain);
        Check(plainJson.Contains("DurationMilliseconds", StringComparison.Ordinal) && !plainJson.Contains("CpuMilliseconds", StringComparison.Ordinal) &&
            !plainJson.Contains("PeakMemoryBytes", StringComparison.Ordinal) && !plainJson.Contains("PeakCommitBytes", StringComparison.Ordinal) &&
            !plainJson.Contains("ProcessMeasured", StringComparison.Ordinal), "An unmeasured action must not write usage fields to the report: " + plainJson);
        NativeBuildRequest measuring = request with { MeasureProcesses = true };
        NativeAction measuredAction = Action("measured");
        NativeActionResult measured = await Executor.RunAsync(measuring, measuredAction, outputRoot, "fixture", store, CancellationToken.None, Measured);
        Check(asked == true && measured.Executed && measured.ProcessMeasured && measured.CpuMilliseconds == CpuMilliseconds &&
            measured.PeakMemoryBytes == PeakBytes && measured.PeakCommitBytes == CommitBytes,
            $"A measured action must carry the usage of its process: {measured.CpuMilliseconds} ms, {measured.PeakMemoryBytes}, {measured.PeakCommitBytes} bytes");
        string measuredJson = System.Text.Json.JsonSerializer.Serialize(measured);
        Check(measuredJson.Contains("\"CpuMilliseconds\":1234", StringComparison.Ordinal) && measuredJson.Contains("\"PeakCommitBytes\":9012", StringComparison.Ordinal) &&
            measuredJson.Contains("\"ProcessMeasured\":true", StringComparison.Ordinal), "A measured action must write its usage to the report: " + measuredJson);
        // cache hit은 계측이 없다(측정을 꺼도 켜도 같은 기록에 맞는다 — 아래 MeasuringKeepsTheCacheAsync가 실제 빌드로 본다).
        NativeActionResult cached = await Executor.RunAsync(request, measuredAction, outputRoot, "fixture", store, CancellationToken.None, Measured);
        Check(!cached.Executed && !cached.ProcessMeasured && cached.CpuMilliseconds == 0 && cached.PeakMemoryBytes == 0, "A cached action carries no usage.");

        // 실제 프로세스: Windows 호스트는 CPU 시간과 최대 working set·commit을 준다. 다른 호스트는 값이 없다(null).
        ProcessResult real = await ProcessRunner.RunAsync(new(request.Toolchain.CompilerPath, ["--version"], root, request.Toolchain.Environment,
            MeasureUsage: true));
        ProcessResult unmeasured = await ProcessRunner.RunAsync(new(request.Toolchain.CompilerPath, ["--version"], root, request.Toolchain.Environment));
        Check(real.ExitCode == 0 && unmeasured.Usage is null &&
            (OperatingSystem.IsWindows()
                ? real.Usage is { PeakWorkingSetBytes: > 0, PeakCommitBytes: > 0 } usage && usage.CpuTime >= TimeSpan.Zero
                : real.Usage is null),
            "A real process must report its usage on Windows hosts only, and only when asked.");
    }

    // 측정은 빌드의 어떤 입력도 바꾸지 않는다: 측정 없이 빌드한 출력을 측정을 켜고 다시 빌드하면 action이 하나도 실행되지 않고(key가
    // 같다), compile_commands.json과 도구 빌드 ID manifest가 byte까지 같다. 반대 순서도 같다.
    private static async Task MeasuringKeepsTheCacheAsync(NativeBuildRequest request, string root)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "unity-profile-cache")).FullName;
        BuildInfoTests.WriteFixture(fixture);
        NativeBuildRequest plain = BuildInfoTests.FixtureRequest(request, fixture, Path.Combine(fixture, "out"));
        var engine = new NativeBuildEngine();
        NativeBuildResult cold = await engine.BuildAsync(plain);
        Check(cold.Succeeded && cold.ExecutedActions != 0 && cold.Actions.All(action => !action.ProcessMeasured), "the unmeasured fixture build: " + cold.Failure);
        string manifestPath = Path.Combine(plain.OutputDirectory, "BuildInfo", "Game.BuildInfo.txt");
        byte[] database = File.ReadAllBytes(cold.CompilationDatabasePath);
        byte[] manifest = File.ReadAllBytes(manifestPath);
        foreach (bool measure in new[] { true, false })
        {
            NativeBuildResult again = await engine.BuildAsync(plain with { MeasureProcesses = measure });
            Check(again.Succeeded && again.ExecutedActions == 0 && again.Actions.All(action => !action.Executed && !action.ProcessMeasured),
                $"Turning MeasureProcesses {(measure ? "on" : "off")} must not run any action: " +
                string.Join(", ", again.Actions.Where(action => action.Executed).Select(action => action.Id)));
            Check(File.ReadAllBytes(again.CompilationDatabasePath).AsSpan().SequenceEqual(database) && File.ReadAllBytes(manifestPath).AsSpan().SequenceEqual(manifest),
                "MeasureProcesses must not change compile_commands.json or the tool build ID manifest.");
        }
    }

    private static async Task RowsAsync(NativeBuildRequest request, string root)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "unity profile fixture")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
            return path;
        }

        string[] sources = Enumerable.Range(0, 4).Select(index => Write($"lib/part{index}.cpp", $"int Part{index}() noexcept {{ return {index}; }}\n")).ToArray();
        string other = Write("base/base.cpp", "int Base() noexcept { return 7; }\n");
        NativeModuleInput baseModule = new() { Name = "ProfileBase", Sources = [other] };
        NativeModuleInput library = new() { Name = "ProfileLib", Sources = sources, PublicDependencies = ["ProfileBase"], UnityAnchorFiles = int.MaxValue };
        IReadOnlyList<UnityProfileCase> cases = UnityProfile.Cases([int.MaxValue], [UnityBuild.DefaultBatchBytes / Kibibyte]);
        var rows = new List<UnityProfileRow>();
        var engine = new NativeBuildEngine();
        string output = Path.Combine(fixture, "out", UnityProfile.OutputPrefix + "fixture");
        UnityProfile.ClaimOutput(output);
        foreach (UnityProfileCase profile in cases)
        {
            NativeBuildRequest build = request with
            {
                RootDirectory = fixture,
                OutputDirectory = output,
                Modules = [baseModule, UnityProfile.Apply(library, profile)],
                Targets = ["ProfileLib"],
                MeasureProcesses = true
            };
            // 명령줄 도구와 같은 순서다: 출력을 확인하고, 그 모듈의 출력을 지우고 빌드한다. 의존 모듈은 두 번째부터 cache hit이다.
            UnityProfile.RequireClaimedOutput(build.OutputDirectory);
            foreach (string directory in UnityProfile.ModuleOutputDirectories(build.OutputDirectory, "ProfileLib"))
            {
                if (Directory.Exists(directory))
                {
                    Directory.Delete(directory, recursive: true);
                }
            }

            NativeBuildResult result = await engine.BuildAsync(build);
            Check(result.Succeeded, "unity profile fixture build failed: " + result.Failure + "\n" +
                string.Join("\n", result.Actions.Where(action => !action.Succeeded).Select(action => action.StdOut + action.StdErr)));
            Check(File.Exists(Path.Combine(output, UnityProfile.MarkerFileName)), "A build must leave the output marker in place.");
            rows.Add(UnityProfile.Row(profile, result, "profilelib", TimeSpan.FromMilliseconds(CpuMilliseconds)));
            if (rows.Count == 2)
            {
                Check(result.Actions.Single(action => action.Module == "ProfileBase" && action.Kind == "Compile").Executed == false,
                    "The dependency must stay cached between cases.");
                // 지우지 않고 다시 빌드하면 cache hit이다. 처음부터 컴파일한 측정이 아니므로 행을 만들지 않는다.
                NativeBuildResult again = await engine.BuildAsync(build);
                Rejects<InvalidOperationException>(() => UnityProfile.Row(profile, again, "ProfileLib", TimeSpan.Zero), "not a clean compile",
                    "A cached compile must not be accepted as a measurement.");
                Rejects<InvalidOperationException>(() => UnityProfile.Row(profile, again, "NoSuchModule", TimeSpan.Zero), "no compile action",
                    "A module without compile actions must be rejected.");
            }
        }

        // 유니티를 끄면 소스마다 번역 단위 하나, 켜면(구간 하나) 묶음 하나다. 벽시계는 넘긴 값 그대로다.
        bool windows = OperatingSystem.IsWindows();
        Check(rows.Count == 2 && rows[0].TranslationUnits == sources.Length && rows[1].TranslationUnits == 1 &&
            rows.All(row => row.Succeeded && row.ExitCode == 0 && row.Error is null && row.WallMilliseconds == CpuMilliseconds && row.ProcessMeasured == windows &&
                (windows ? row.PeakMemoryBytes > 0 && row.PeakCommitBytes > 0 && row.CpuMilliseconds >= 0
                    : row.PeakMemoryBytes == 0 && row.PeakCommitBytes == 0 && row.CpuMilliseconds == 0)),
            "Rows must count the translation units of the module and carry process usage where the host provides it: " +
            string.Join("; ", rows.Select(row => $"{row.Case.Name}: {row.TranslationUnits} TUs, {row.PeakMemoryBytes} bytes")));

        // 재지 못한 조합은 표에 실패 행으로 남고(종료 코드), 표 아래에 첫 오류 한 줄이 붙는다. 잰 행은 그대로다.
        UnityProfileCase broken = new("anchor 8, 256 KiB", true, 8, 256 * Kibibyte);
        rows.Add(UnityProfileRow.Failed(broken, 1, "In file included from a.cpp:1:\n\n  a.h(3,1): error: unknown type name 'x'\nmore"));
        Check(!rows[2].Succeeded && rows[2].ExitCode == 1 && rows[2].Error == "In file included from a.cpp:1:" && rows[2].TranslationUnits == 0 &&
            UnityProfile.FirstErrorLine("In file included from a.cpp:1:\n  a.h(3,1): error: unknown type name 'x'\nmore") == "a.h(3,1): error: unknown type name 'x'" &&
            UnityProfile.FirstErrorLine("note only\nsecond") == "note only" && UnityProfile.FirstErrorLine(null) == "" && UnityProfile.FirstLine("\n\n") == "",
            "A failed row keeps the exit code and the first line of the error.");
        string table = UnityProfile.Table("ProfileLib", rows);
        string[] lines = table.TrimEnd('\n').Split('\n');
        Check(lines.Length == 6 && lines[0] == "Unity profile: module ProfileLib" && lines[1].StartsWith("case", StringComparison.Ordinal) &&
            lines[1].Contains("commit MiB", StringComparison.Ordinal) && lines[2].StartsWith("unity off", StringComparison.Ordinal) &&
            lines[2].Contains(" 1.2 ", StringComparison.Ordinal) && (windows || lines[2].Contains("n/a", StringComparison.Ordinal)) &&
            lines[4].StartsWith("anchor 8, 256 KiB", StringComparison.Ordinal) && lines[4].EndsWith("failed (exit 1)", StringComparison.Ordinal) &&
            lines[5] == "error: anchor 8, 256 KiB: In file included from a.cpp:1:",
            "The table must have a title, a header, one line per case and one error line per failed case:\n" + table);
    }

    // action이 TError(메시지에 expected가 든)를 던지는지 본다.
    private static void Rejects<TError>(Action action, string expected, string message)
        where TError : Exception
    {
        try
        {
            action();
        }
        catch (TError error) when (error.Message.Contains(expected, StringComparison.Ordinal))
        {
            return;
        }

        throw new InvalidOperationException(message);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
