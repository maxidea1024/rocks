using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 묶음 크기 실측(<c>unity-profile</c>, Docs/Design/UnityBuildPch.ko.md 12절)의 부품을 검증한다: 조합 목록과 모듈 재정의, 프로세스
/// 계측(<see cref="ProcessSpec.MeasureUsage"/> → <see cref="NativeActionResult"/>), 빌드 결과에서 행 뽑기, 표 출력. 계측 값의 전달은
/// 주입한 프로세스로 정확히 단언하고(실제 CPU 시간에 기대지 않는다), 실제 프로세스에서는 값이 있는지만 본다.
/// </summary>
internal static class UnityProfileTests
{
    private const long CpuMilliseconds = 1234;

    private const long PeakBytes = 5678;

    private const int Kibibyte = 1024;

    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        CasesAndOverrides();
        pass("unity-profile: cases follow the spec order and override one module");
        await ProcessUsageAsync(request, root);
        pass("unity-profile: process usage reaches the action result only when requested");
        await RowsAsync(request, root);
        pass("unity-profile: a clean compile of one module yields a row, a cached compile is rejected");
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
            try
            {
                UnityProfile.Cases(files, sizes);
                throw new InvalidOperationException("An empty or non-positive case list must be rejected.");
            }
            catch (ArgumentException)
            {
            }
        }

        // 재정의는 그 모듈의 유니티 설정만 바꾼다(끄는 조합은 구간·상한을 건드리지 않는다).
        NativeModuleInput module = new() { Name = "Probe", Unity = UnityMode.Default, UnityExclude = ["keep.cpp"] };
        NativeModuleInput on = UnityProfile.Apply(module, cases[1]);
        NativeModuleInput off = UnityProfile.Apply(module, cases[0]);
        Check(on.Unity == UnityMode.On && on.UnityAnchorFiles == 8 && on.UnityBatchBytes == 256 * Kibibyte && on.UnityExclude.SequenceEqual(["keep.cpp"]) &&
            off.Unity == UnityMode.Off && off.UnityAnchorFiles == module.UnityAnchorFiles && off.UnityBatchBytes == module.UnityBatchBytes,
            "A case must override only the unity settings of the module.");
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
            return Task.FromResult(new ProcessResult(0, "", "") { Usage = spec.MeasureUsage ? new(TimeSpan.FromMilliseconds(CpuMilliseconds), PeakBytes) : null });
        }

        // 요청하지 않으면 실행기는 계측을 청하지 않고 결과도 비어 있다.
        NativeActionResult plain = await Executor.RunAsync(request, Action("plain"), outputRoot, "fixture", store, CancellationToken.None, Measured);
        Check(asked == false && plain.Executed && !plain.ProcessMeasured && plain.CpuMilliseconds == 0 && plain.PeakMemoryBytes == 0,
            "Process usage must not be requested or recorded by default.");
        NativeAction measuredAction = Action("measured");
        string keyBefore = measuredAction.Key("fixture", outputRoot, request.RootDirectory);
        NativeBuildRequest measuring = request with { MeasureProcesses = true };
        NativeActionResult measured = await Executor.RunAsync(measuring, measuredAction, outputRoot, "fixture", store, CancellationToken.None, Measured);
        Check(asked == true && measured.Executed && measured.ProcessMeasured && measured.CpuMilliseconds == CpuMilliseconds &&
            measured.PeakMemoryBytes == PeakBytes, $"A measured action must carry the usage of its process: {measured.CpuMilliseconds} ms, {measured.PeakMemoryBytes} bytes");
        // 측정은 key에 들어가지 않는다(켜도 cache가 그대로 맞는다). cache hit은 계측이 없다.
        Check(measuredAction.Key("fixture", outputRoot, measuring.RootDirectory) == keyBefore, "Measuring processes must not change the action key.");
        NativeActionResult cached = await Executor.RunAsync(measuring, measuredAction, outputRoot, "fixture", store, CancellationToken.None, Measured);
        Check(!cached.Executed && !cached.ProcessMeasured && cached.CpuMilliseconds == 0 && cached.PeakMemoryBytes == 0, "A cached action carries no usage.");

        // 실제 프로세스: Windows 호스트는 CPU 시간과 최대 working set을 준다. 다른 호스트는 값이 없다(null).
        ProcessResult real = await ProcessRunner.RunAsync(new(request.Toolchain.CompilerPath, ["--version"], root, request.Toolchain.Environment,
            MeasureUsage: true));
        ProcessResult unmeasured = await ProcessRunner.RunAsync(new(request.Toolchain.CompilerPath, ["--version"], root, request.Toolchain.Environment));
        Check(real.ExitCode == 0 && unmeasured.Usage is null &&
            (OperatingSystem.IsWindows() ? real.Usage is { PeakWorkingSetBytes: > 0 } usage && usage.CpuTime >= TimeSpan.Zero : real.Usage is null),
            "A real process must report its usage on Windows hosts only, and only when asked.");
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
        foreach (UnityProfileCase profile in cases)
        {
            NativeBuildRequest build = request with
            {
                RootDirectory = fixture,
                OutputDirectory = Path.Combine(fixture, "out"),
                Modules = [baseModule, UnityProfile.Apply(library, profile)],
                Targets = ["ProfileLib"],
                MeasureProcesses = true
            };
            // 명령줄 도구와 같은 순서다: 그 모듈의 출력을 지우고 빌드한다. 의존 모듈은 두 번째부터 cache hit이다.
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
            rows.Add(UnityProfile.Row(profile, result, "ProfileLib", TimeSpan.FromMilliseconds(CpuMilliseconds)));
            if (rows.Count == 2)
            {
                Check(result.Actions.Single(action => action.Module == "ProfileBase" && action.Kind == "Compile").Executed == false,
                    "The dependency must stay cached between cases.");
                // 지우지 않고 다시 빌드하면 cache hit이다. 처음부터 컴파일한 측정이 아니므로 행을 만들지 않는다.
                NativeBuildResult again = await engine.BuildAsync(build);
                try
                {
                    UnityProfile.Row(profile, again, "ProfileLib", TimeSpan.Zero);
                    throw new InvalidOperationException("A cached compile must not be accepted as a measurement.");
                }
                catch (InvalidOperationException error) when (error.Message.Contains("not a clean compile", StringComparison.Ordinal))
                {
                }

                try
                {
                    UnityProfile.Row(profile, again, "NoSuchModule", TimeSpan.Zero);
                    throw new InvalidOperationException("A module without compile actions must be rejected.");
                }
                catch (InvalidOperationException error) when (error.Message.Contains("no compile action", StringComparison.Ordinal))
                {
                }
            }
        }

        // 유니티를 끄면 소스마다 번역 단위 하나, 켜면(구간 하나) 묶음 하나다. 벽시계는 넘긴 값 그대로다.
        bool windows = OperatingSystem.IsWindows();
        Check(rows.Count == 2 && rows[0].TranslationUnits == sources.Length && rows[1].TranslationUnits == 1 &&
            rows.All(row => row.WallMilliseconds == CpuMilliseconds && row.ProcessMeasured == windows &&
                (windows ? row.PeakMemoryBytes > 0 && row.CpuMilliseconds >= 0 : row.PeakMemoryBytes == 0 && row.CpuMilliseconds == 0)),
            "Rows must count the translation units of the module and carry process usage where the host provides it: " +
            string.Join("; ", rows.Select(row => $"{row.Case.Name}: {row.TranslationUnits} TUs, {row.PeakMemoryBytes} bytes")));
        string table = UnityProfile.Table("ProfileLib", rows);
        string[] lines = table.TrimEnd('\n').Split('\n');
        Check(lines.Length == 4 && lines[0] == "Unity profile: module ProfileLib" && lines[1].StartsWith("case", StringComparison.Ordinal) &&
            lines[2].StartsWith("unity off", StringComparison.Ordinal) && lines[2].Contains(" 1.2 ", StringComparison.Ordinal) &&
            (windows || lines[2].Contains("n/a", StringComparison.Ordinal)), "The table must have a title, a header and one line per case:\n" + table);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
