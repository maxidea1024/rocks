using System.Diagnostics;
using System.Runtime.CompilerServices;
using Microsoft.Build.Evaluation;
using Microsoft.Build.Locator;
using Rocks.Build.Cli;
using Rocks.Tooling.Common;

namespace Rocks.Build.IntegrationTests;

// Visual Studio NMake 진입점·도구 준비·정리(Docs/Design/ProjectFiles.ko.md 16절 V6·V7·V13·V14)를 Visual Studio 없이 검사한다. Windows
// 호스트에서 pwsh가 있을 때만 돈다.
// - V14: clean 경계(out의 엄격한 하위, Rocks 출력 표지, 잠금 파일 보존, 없는 출력).
// - V7: IDE 빌드 잠금을 이 스레드가 쥔 동안 IdeBuild.ps1은 "Waiting for the IDE build lock"을 내고 멈추며, 놓은 뒤에야 시험 표지를
//   쓴다(시간 대기 없이 잠금과 출력 줄로만 순서를 만든다).
// - V13: Build/Tools.ps1의 도구 지문 입력이 MSBuild가 평가한 도구 프로젝트 입력(Compile·EmbeddedResource·복사되는 None·import)을 덮는다.
// - V6: 저장소의 생성 프로젝트(MathBench)를 평가한 NMake Build·Clean·Rebuild 명령을 cmd로 돌리고, IDE 빌드 뒤 터미널 Rocks.proj 빌드는
//   0 action이다(같은 컴파일러를 쓴다).
internal static class IdeScriptsTests
{
    // V6에 쓰는 작은 엔진 저장소 타깃이다.
    private const string SmallTarget = "MathBench";

    public static async Task RunAsync(string repository)
    {
        if (!OperatingSystem.IsWindows() || !Tool("pwsh"))
        {
            Console.WriteLine("SKIP IDE scripts: Windows hosts with pwsh only");
            return;
        }

        if (!MSBuildLocator.IsRegistered)
        {
            MSBuildLocator.RegisterDefaults();
        }

        await CleanBoundariesAsync(repository);
        await SetupSelfTestAsync(repository);
        await SdkScriptsAsync(repository);
        await LockOrderAsync(repository);
        await ToolInputsAsync(repository);
        await NMakeCommandsAsync(repository);
        await StalenessAsync(repository);
        Console.WriteLine("PASS IDE scripts: clean boundaries, setup self test (-Command and -File, pwsh and 5.1), SDK scripts, IDE build lock order, tool inputs and rebuild decisions, Visual Studio or NMake build/clean/rebuild, " +
            "terminal no-op, failure exit codes, RPF0001, compile one file");
    }

    private static async Task CleanBoundariesAsync(string repository)
    {
        string root = Directory.CreateTempSubdirectory("rocks-clean-").FullName;
        try
        {
            string output = Path.Combine(root, "out", "dotnet-windows-probe-debug");
            Require(await CleanCommand.CleanAsync(root, output, CancellationToken.None) == -1, "a missing output is nothing to clean");
            Directory.CreateDirectory(Path.Combine(output, "obj", "Probe"));
            File.WriteAllText(Path.Combine(output, "obj", "Probe", "a.obj"), "x");
            await Expect(() => CleanCommand.CleanAsync(root, output, CancellationToken.None), "has no .native-build.lock");
            File.WriteAllText(Path.Combine(output, CleanCommand.OutputMarker), "");
            File.WriteAllText(Path.Combine(output, "compile_commands.json"), "[]");
            Require(await CleanCommand.CleanAsync(root, output, CancellationToken.None) == 2, "clean removed a different number of entries");
            string[] left = [.. Directory.EnumerateFileSystemEntries(output).Select(Path.GetFileName).Order(StringComparer.Ordinal)!];
            Require(left.SequenceEqual([CleanCommand.OutputMarker, ".rocks-invocation.lock"], StringComparer.Ordinal), "clean must keep only the lock files: " +
                string.Join(", ", left));
            await Expect(() => CleanCommand.CleanAsync(root, Path.Combine(root, "out"), CancellationToken.None), "only build outputs under");
            await Expect(() => CleanCommand.CleanAsync(root, Path.Combine(root, "elsewhere"), CancellationToken.None), "only build outputs under");
            await Expect(() => CleanCommand.CleanAsync(root, repository, CancellationToken.None), "only build outputs under");
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    // V7: 잠금 순서. mutex는 이 스레드에서 잡고 같은 스레드에서 놓는다.
    private static async Task LockOrderAsync(string repository)
    {
        string probe = Path.Combine(Directory.CreateTempSubdirectory("rocks-ide-lock-").FullName, "started.txt");
        using var mutex = new Mutex(false, LockName(repository));
        Require(mutex.WaitOne(0), "the IDE build lock is held by another process; close other IDE builds of this working copy");
        using var waiting = new ManualResetEventSlim(false);
        var start = new ProcessStartInfo("pwsh")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            ArgumentList = { "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Path.Combine(repository, "Build", "IdeBuild.ps1"),
                "-Action", "Build", "-Target", SmallTarget, "-Configuration", "Debug", "-Platform", "Windows" },
            Environment = { ["ROCKS_IDE_BUILD_PROBE"] = probe, ["ROCKS_IDE_BUILD_TEST"] = "1" }
        };
        using var process = Process.Start(start)!;
        process.OutputDataReceived += (_, line) =>
        {
            if (line.Data?.Contains("Waiting for the IDE build lock", StringComparison.Ordinal) == true)
            {
                waiting.Set();
            }
        };
        process.BeginOutputReadLine();
        Task<string> errors = process.StandardError.ReadToEndAsync();
        bool released = false;
        try
        {
            // 스크립트가 잠금을 기다린다고 알린 뒤에도 표지가 없어야 한다(잠금 없이 진행하지 않는다).
            waiting.Wait();
            Require(!File.Exists(probe), "IdeBuild.ps1 went on without the IDE build lock");
            mutex.ReleaseMutex();
            released = true;
            await process.WaitForExitAsync();
        }
        finally
        {
            if (!released)
            {
                mutex.ReleaseMutex();
            }
        }

        Require(process.ExitCode == 0 && File.Exists(probe), "IdeBuild.ps1 did not run after the lock was released: " + await errors);
        Directory.Delete(Path.GetDirectoryName(probe)!, recursive: true);
    }

    // V13: 도구 지문 입력이 MSBuild 평가 입력을 덮는다.
    private static async Task ToolInputsAsync(string repository)
    {
        var listing = await ProcessRunner.RunAsync(new("pwsh", ["-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            Path.Combine(repository, "Build", "Tools.ps1"), "-ListInputs"], repository));
        Require(listing.ExitCode == 0, "Tools.ps1 -ListInputs failed: " + listing.StdErr);
        var inputs = new HashSet<string>(listing.StdOut.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries),
            StringComparer.OrdinalIgnoreCase);
        string[] missing = [.. EvaluatedToolInputs(repository).Where(path => !inputs.Contains(path))];
        Require(missing.Length == 0, "Build/Tools.ps1 does not fingerprint the tool inputs " + string.Join(", ", missing));
        Require(!inputs.Any(path => path.StartsWith("Programs/Rocks.Build.Tests/", StringComparison.Ordinal) ||
            path.StartsWith("Programs/Rocks.Build.IntegrationTests/", StringComparison.Ordinal)), "test projects must not be tool inputs");

        // 지문이 같으면 dotnet build를 부르지 않고, 도구 원본(도구 프로젝트 디렉터리의 파일)이 바뀌면 부른다. 시험 프로젝트 파일은 입력이
        // 아니다. 끝에 원래대로 되돌리면 다시 한 번 빌드한다.
        async Task<string> Prepare()
        {
            var run = await ProcessRunner.RunAsync(new("pwsh", ["-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                Path.Combine(repository, "Build", "Tools.ps1")], repository));
            Require(run.ExitCode == 0, "Tools.ps1 failed: " + run.StdOut + run.StdErr);
            return run.StdOut;
        }

        const string upToDate = "Rocks tools are up to date.";
        const string building = "Building the Rocks tools";
        await Prepare();
        Require((await Prepare()).Contains(upToDate, StringComparison.Ordinal), "unchanged tool inputs rebuilt the tools");
        string testProbe = Path.Combine(repository, "Programs", "Rocks.Build.Tests", "rocks-tools-fingerprint-probe.txt");
        string toolProbe = Path.Combine(repository, "Programs", "Rocks.Build.Cli", "rocks-tools-fingerprint-probe.txt");
        try
        {
            File.WriteAllText(testProbe, "probe\n");
            Require((await Prepare()).Contains(upToDate, StringComparison.Ordinal), "a test project change rebuilt the tools");
            File.WriteAllText(toolProbe, "probe\n");
            Require((await Prepare()).Contains(building, StringComparison.Ordinal), "a tool input change did not rebuild the tools");
            Require((await Prepare()).Contains(upToDate, StringComparison.Ordinal), "the rebuilt tools are not up to date");
        }
        finally
        {
            File.Delete(testProbe);
            File.Delete(toolProbe);
        }

        Require((await Prepare()).Contains(building, StringComparison.Ordinal), "removing a tool input did not rebuild the tools");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static SortedSet<string> EvaluatedToolInputs(string repository)
    {
        var result = new SortedSet<string>(StringComparer.Ordinal);
        var pending = new Stack<string>([Path.Combine(repository, "Programs", "Rocks.Tools.csproj")]);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using var collection = new ProjectCollection();
        while (pending.Count != 0)
        {
            string path = Path.GetFullPath(pending.Pop());
            if (!seen.Add(path))
            {
                continue;
            }

            var project = new Project(path, new Dictionary<string, string> { ["Configuration"] = "Debug" }, null, collection);
            void Add(string file)
            {
                string full = Path.GetFullPath(file, project.DirectoryPath);
                if (DirectoryPath.IsAtOrUnder(full, repository) && !DirectoryPath.IsAtOrUnder(full, Path.Combine(repository, "out")))
                {
                    result.Add(Path.GetRelativePath(repository, full).Replace('\\', '/'));
                }
            }

            Add(path);
            foreach (var imported in project.Imports)
            {
                Add(imported.ImportedProject.FullPath);
            }

            foreach (ProjectItem item in project.Items.Where(item => item.ItemType is "Compile" or "EmbeddedResource" ||
                (item.ItemType is "None" or "Content" && item.GetMetadataValue("CopyToOutputDirectory") is "PreserveNewest" or "Always")))
            {
                Add(item.EvaluatedInclude);
            }

            foreach (ProjectItem reference in project.GetItems("ProjectReference"))
            {
                pending.Push(Path.GetFullPath(reference.EvaluatedInclude, project.DirectoryPath));
            }

            collection.UnloadProject(project);
        }

        return result;
    }

    // V14: RPF0001. 프로젝트 파일을 만든 뒤 기본 빌드와 --unity off·--adaptive off·--pch off 빌드는 경고가 없고, 소스를 더하면 경고가 나며,
    // 다시 만들면 사라진다(Android·PublicHeaderChecks는 조건 코드와 Rocks.Build.Tests의 지문 시험이 덮는다).
    private static async Task StalenessAsync(string repository)
    {
        string cli = Path.Combine(repository, "out", "dotnet", "bin", "Rocks.Build.Cli", "Debug", "net10.0", "Rocks.Build.Cli.dll");
        async Task<string> Cli(params string[] arguments)
        {
            var result = await ProcessRunner.RunAsync(new("dotnet", [cli, .. arguments], repository));
            Require(result.ExitCode == 0, "Rocks.Build.Cli " + string.Join(' ', arguments) + " failed: " + result.StdOut + result.StdErr);
            return result.StdOut;
        }

        string[] Build(params string[] extra) => ["build", "--root", repository, "--target", SmallTarget, "--configuration", "Debug", .. extra];
        bool Stale(string output) => output.Contains("warning RPF0001", StringComparison.Ordinal);
        await Cli("projectfiles", "--root", repository);
        Require(!Stale(await Cli(Build())), "a fresh build reported stale project files");
        foreach (string[] mode in new[] { new[] { "--unity", "off" }, ["--adaptive", "off"], ["--pch", "off"] })
        {
            Require(!Stale(await Cli(Build(mode))), "a build with " + string.Join(' ', mode) + " reported stale project files");
        }

        string probe = Path.Combine(repository, "Programs", SmallTarget, "Private", "RocksStalenessProbe.cpp");
        try
        {
            File.WriteAllText(probe, "int RocksStalenessProbe() noexcept;\nint RocksStalenessProbe() noexcept { return 1; }\n");
            Require(Stale(await Cli(Build())), "adding a source did not report stale project files");
            await Cli("projectfiles", "--root", repository);
            Require(!Stale(await Cli(Build())), "regenerating did not clear the stale project files warning");
        }
        finally
        {
            File.Delete(probe);
        }

        await Cli("projectfiles", "--root", repository);
        Require(!Stale(await Cli(Build())), "project files are stale after removing the probe and regenerating");
    }

    // 설치 마법사 셀프 테스트를 CI와 같은 -Command 호출과 -File 호출로, PowerShell 7과 Windows PowerShell 5.1 모두에서 돌린다. -Command는 스크립트를
    // 세션 안에서 불러 GetNewClosure 클로저가 스크립트 함수를 보지 못하는 문제가 -File에서는 드러나지 않았다(CI만 실패).
    private static async Task SetupSelfTestAsync(string repository)
    {
        string setup = Path.Combine(repository, "Build", "Setup.ps1");
        foreach (string shell in new[] { "pwsh", "powershell" })
        {
            foreach (string[] form in new[] { new[] { "-Command", "& '" + setup.Replace("'", "''", StringComparison.Ordinal) + "' -SelfTest" }, ["-File", setup, "-SelfTest"] })
            {
                var result = await ProcessRunner.RunAsync(new(shell, ["-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", .. form], repository,
                    Timeout: TimeSpan.FromMinutes(5)));
                Require(result.ExitCode == 0 && result.StdOut.Contains("Setup self test: PASS", StringComparison.Ordinal),
                    $"Setup self test failed with {shell} {form[0]}: " + result.StdOut + result.StdErr);
            }
        }
    }

    // 설치 SDK에 실린 스크립트: Tools.ps1은 도구를 빌드하지 않고(-ListInputs는 해당 없음), IdeBuild.ps1·GenerateProjectFiles.ps1은 게임 형태만
    // 받는다. 엔진 작업 사본의 IdeBuild.ps1은 게임 형태를 거부한다.
    private static async Task SdkScriptsAsync(string repository)
    {
        string sdk = Directory.CreateTempSubdirectory("rocks-sdk-scripts-").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(sdk, "Build"));
            foreach (string script in new[] { "Tools.ps1", "IdeBuild.ps1", "GenerateProjectFiles.ps1" })
            {
                File.Copy(Path.Combine(repository, "Build", script), Path.Combine(sdk, "Build", script));
            }

            File.WriteAllText(Path.Combine(sdk, "rocks-sdk.json"), "{}");
            Directory.CreateDirectory(Path.Combine(sdk, "Tools", "Rocks.Build.Cli"));
            File.WriteAllText(Path.Combine(sdk, "Tools", "Rocks.Build.Cli", "Rocks.Build.Cli.dll"), "");
            Task<ProcessResult> Script(string name, params string[] arguments) => ProcessRunner.RunAsync(new("pwsh",
                ["-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Path.Combine(sdk, "Build", name), .. arguments], sdk));
            var tools = await Script("Tools.ps1");
            Require(tools.ExitCode == 0 && tools.StdOut.Contains("ship with the installed SDK", StringComparison.Ordinal), "SDK Tools.ps1: " + tools.StdOut + tools.StdErr);
            var inputs = await Script("Tools.ps1", "-ListInputs");
            Require(inputs.ExitCode == 0 && inputs.StdOut.Contains("not applicable", StringComparison.Ordinal), "SDK Tools.ps1 -ListInputs: " + inputs.StdOut + inputs.StdErr);
            // 설치 SDK는 게임 형태(-Project·-Sdk)만, 엔진 작업 사본은 엔진 형태만 받는다(10.4절). 실제 게임 형태 실행은 본 흐름이 시험한다.
            var engineForm = await Script("IdeBuild.ps1", "-Action", "Build", "-Target", "Game", "-Configuration", "Debug", "-Platform", "Windows");
            Require(engineForm.ExitCode == 1 && engineForm.StdOut.Contains("error RB0002: an installed SDK builds game projects only", StringComparison.Ordinal),
                "SDK IdeBuild.ps1 engine form: " + engineForm.StdOut + engineForm.StdErr);
            var halfGame = await Script("IdeBuild.ps1", "-Action", "Build", "-Project", Path.Combine(sdk, "Game.rocksproject"), "-Target", "Game",
                "-Configuration", "Debug");
            Require(halfGame.ExitCode == 1 && halfGame.StdOut.Contains("error RB0002", StringComparison.Ordinal), "SDK IdeBuild.ps1 without -Sdk: " +
                halfGame.StdOut + halfGame.StdErr);
            var engineGame = await ProcessRunner.RunAsync(new("pwsh", ["-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                Path.Combine(repository, "Build", "IdeBuild.ps1"), "-Action", "Build", "-Project", Path.Combine(sdk, "Game.rocksproject"), "-Sdk", sdk,
                "-Target", "Game", "-Configuration", "Debug"], repository));
            Require(engineGame.ExitCode == 1 && engineGame.StdOut.Contains("error RB0002: game projects build with the IdeBuild.ps1 of their installed SDK",
                StringComparison.Ordinal), "engine IdeBuild.ps1 game form: " + engineGame.StdOut + engineGame.StdErr);
            var generate = await Script("GenerateProjectFiles.ps1");
            Require(generate.ExitCode == 1 && generate.StdOut.Contains("error RB0002: an installed SDK generates game project files only", StringComparison.Ordinal),
                "SDK GenerateProjectFiles.ps1 without -Project: " + generate.StdOut + generate.StdErr);
        }
        finally
        {
            Directory.Delete(sdk, recursive: true);
        }
    }

    // V6: 생성 프로젝트의 NMake 명령을 cmd로 돌린다.
    private static async Task NMakeCommandsAsync(string repository)
    {
        // 깨끗한 트리에서도 돌게 프로젝트 파일을 먼저 만든다(V14보다 먼저 돈다).
        string cli = Path.Combine(repository, "out", "dotnet", "bin", "Rocks.Build.Cli", "Debug", "net10.0", "Rocks.Build.Cli.dll");
        var generated = await ProcessRunner.RunAsync(new("dotnet", [cli, "projectfiles", "--root", repository], repository));
        Require(generated.ExitCode == 0, "projectfiles --root failed: " + generated.StdOut + generated.StdErr);
        string project = Path.Combine(repository, "out", "ProjectFiles", "Programs", SmallTarget + ".vcxproj");
        Require(File.Exists(project), "projectfiles did not write " + project);

        (string build, string clean, string rebuild, string output) = NMakeProperties(project);
        string outputDirectory = Path.GetDirectoryName(Path.GetDirectoryName(output)!)!;
        // Visual Studio 18이 있으면 Visual Studio MSBuild로 빌드한다 — Makefile 프로젝트도 INCLUDE·LIB 등을 넣는 실제 경로다(7.1절 환경
        // 정리). 없으면 NMake 명령줄을 cmd로 그대로 돌린다.
        string? msbuild = await ProjectFilesWriterTests.VisualStudioMSBuildAsync();
        if (msbuild is not null)
        {
            var visualStudio = await ProcessRunner.RunAsync(new(msbuild, [project, "-nologo", "-t:Build", "-p:Configuration=Debug", "-p:Platform=x64"],
                Path.GetDirectoryName(project)!, ProjectFilesWriterTests.VisualStudioEnvironment()));
            Require(visualStudio.ExitCode == 0, "Visual Studio MSBuild build failed: " + visualStudio.StdOut + visualStudio.StdErr);
            Require(!visualStudio.StdOut.Contains("RPF0001", StringComparison.Ordinal), "the Visual Studio build reported stale project files: " + visualStudio.StdOut);
        }
        else
        {
            Console.WriteLine("SKIP IDE scripts V6 Visual Studio MSBuild: no Visual Studio 18 with the ClangCL toolset; running the NMake command line");
            await Run(build, "NMake build");
        }

        Require(File.Exists(output), "NMakeOutput was not built: " + output);
        var terminal = await ProcessRunner.RunAsync(new("dotnet", ["msbuild", Path.Combine(repository, "Rocks.proj"), "-t:Build", "-nologo",
            "-p:RocksTarget=" + SmallTarget, "-p:Configuration=Debug", "-p:Compiler=" + Compiler(build)], repository));
        Require(terminal.ExitCode == 0 && terminal.StdOut.Contains("executed=0;", StringComparison.Ordinal),
            "the terminal build after the IDE build was not a no-op: " + terminal.StdOut);
        Require(!terminal.StdOut.Contains("RPF0001", StringComparison.Ordinal), "the terminal build reported stale project files: " + terminal.StdOut);

        // 파일 하나 컴파일(Ctrl+F7): 논리 명령·별도 object·증분 기록 불변, -WorkingDir 상대 경로. 그 뒤 터미널 빌드는 여전히 0 action이다.
        using (var database = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(outputDirectory, "compile_commands.json"))))
        {
            string source = database.RootElement.EnumerateArray().Select(entry => Path.GetFullPath(entry.GetProperty("file").GetString()!))
                .First(file => file.StartsWith(Path.Combine(repository, "Programs", SmallTarget) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) &&
                    file.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase));
            // 헤더는 확장자로 거부하므로 있을 필요가 없다(MathBench에는 헤더가 없다).
            string header = Path.Combine(repository, "Programs", SmallTarget, "Private", "Selected.h");
            await CompileSelectedAsync(project, "Debug", outputDirectory, source, header, repository);
        }

        var afterCompile = await ProcessRunner.RunAsync(new("dotnet", ["msbuild", Path.Combine(repository, "Rocks.proj"), "-t:Build", "-nologo",
            "-p:RocksTarget=" + SmallTarget, "-p:Configuration=Debug", "-p:Compiler=" + Compiler(build)], repository));
        Require(afterCompile.ExitCode == 0 && afterCompile.StdOut.Contains("executed=0;", StringComparison.Ordinal),
            "the terminal build after compiling one file was not a no-op: " + afterCompile.StdOut);

        // 도구를 다시 빌드한 회차에도 실패는 실패다(도구 빌드 출력이 종료 코드에 섞이지 않는다).
        File.Delete(Path.Combine(repository, "out", "dotnet", ".rocks-tools.json"));
        var failing = await ProcessRunner.RunAsync(new("pwsh", ["-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            Path.Combine(repository, "Build", "IdeBuild.ps1"), "-Action", "Build", "-Target", "NoSuchTarget", "-Configuration", "Debug", "-Platform", "Windows"],
            repository));
        Require(failing.ExitCode != 0 && failing.StdOut.Contains("Building the Rocks tools", StringComparison.Ordinal) &&
            failing.StdOut.Contains("error RB0001", StringComparison.Ordinal), "a failing IDE build after a tool rebuild must fail with error RB0001: " +
            failing.ExitCode + " " + failing.StdOut);
        await Run(clean, "NMake clean");
        string[] left = [.. Directory.EnumerateFileSystemEntries(outputDirectory).Select(Path.GetFileName).Order(StringComparer.Ordinal)!];
        Require(left.SequenceEqual([CleanCommand.OutputMarker, ".rocks-invocation.lock"], StringComparer.Ordinal), "NMake clean left " + string.Join(", ", left));
        await Run(rebuild, "NMake rebuild");
        Require(File.Exists(output), "NMake rebuild did not build " + output);

        async Task Run(string command, string what)
        {
            // NMake처럼 명령줄 문자열을 cmd에 그대로 넘긴다(인자 목록으로 넘기면 따옴표가 이스케이프된다).
            var start = new ProcessStartInfo("cmd.exe", "/d /s /c \"" + command + "\"")
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WorkingDirectory = repository
            };
            // Visual Studio가 Makefile 프로젝트에 넣는 변수를 일부러 다른 값으로 둔다. IdeBuild.ps1이 지우지 않으면 도구 모음 key가 달라져
            // 뒤의 터미널 빌드가 0 action이 아니다(7.1절 환경 정리).
            foreach (string name in new[] { "INCLUDE", "EXTERNAL_INCLUDE", "LIB", "LIBPATH" })
            {
                start.Environment[name] = Path.Combine(repository, "out", "rocks-ide-environment-probe", name);
            }

            using var process = Process.Start(start)!;
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> errors = process.StandardError.ReadToEndAsync();
            await process.WaitForExitAsync();
            Require(process.ExitCode == 0, what + " failed: " + await output + await errors);
        }
    }

    /// <summary>
    /// 파일 하나 컴파일(Ctrl+F7, Docs/Design/ProjectFiles.ko.md 7.4절) 시험: SelectedFiles를 프로젝트 디렉터리 상대 경로로 주어 평가한
    /// NMakeCompileFileCommandLine을 cmd로 돌린다. object가 생기고, 기록한 인자가 빌드한 compile_commands.json의 그 소스 명령(/Fo 제외)과 같고,
    /// native-cache.json·action-report.json이 byte 그대로다. 헤더를 고르면 실패한다.
    /// </summary>
    internal static async Task CompileSelectedAsync(string projectPath, string configuration, string output, string source, string header,
        string workingDirectory, IReadOnlyDictionary<string, string?>? environment = null)
    {
        byte[] Bytes(string name) => File.ReadAllBytes(Path.Combine(output, name));
        byte[] cache = Bytes("native-cache.json");
        byte[] report = Bytes("action-report.json");
        string relative = Path.GetRelativePath(Path.GetDirectoryName(projectPath)!, source);
        string command = CompileFileCommand(projectPath, configuration, relative);
        Require(command.Contains("-Action Compile", StringComparison.Ordinal) && command.Contains("-Files \"" + relative + "\"", StringComparison.Ordinal),
            "NMakeCompileFileCommandLine: " + command);
        await RunCommand(command, "compile one file", workingDirectory, environment, expectSuccess: true);
        string[] records = Directory.GetFiles(Path.Combine(output, "SingleFile"), "*.json");
        using var record = System.Text.Json.JsonDocument.Parse(File.ReadAllText(records.Single(path =>
            string.Equals(System.Text.Json.JsonDocument.Parse(File.ReadAllText(path)).RootElement.GetProperty("source").GetString(), source,
                StringComparison.OrdinalIgnoreCase))));
        string objectPath = record.RootElement.GetProperty("object").GetString()!;
        Require(File.Exists(objectPath) && Path.GetDirectoryName(objectPath)!.EndsWith("SingleFile", StringComparison.Ordinal), "single-file object " + objectPath);
        static string[] WithoutObject(IEnumerable<string> arguments) => [.. arguments.Where(argument => !argument.StartsWith("/Fo", StringComparison.Ordinal))];
        string[] compiled = WithoutObject(record.RootElement.GetProperty("arguments").EnumerateArray().Select(item => item.GetString()!));
        using var database = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(output, "compile_commands.json")));
        bool matched = database.RootElement.EnumerateArray().Where(entry => string.Equals(Path.GetFullPath(entry.GetProperty("file").GetString()!), source,
            StringComparison.OrdinalIgnoreCase)).Any(entry => WithoutObject(entry.GetProperty("arguments").EnumerateArray().Skip(1).Select(item => item.GetString()!))
            .SequenceEqual(compiled, StringComparer.Ordinal));
        Require(matched, "the single-file command is not the logical command of the build: " + string.Join(' ', compiled));
        Require(Bytes("native-cache.json").AsSpan().SequenceEqual(cache) && Bytes("action-report.json").AsSpan().SequenceEqual(report),
            "compiling one file changed the incremental record or the action report");
        await RunCommand(CompileFileCommand(projectPath, configuration, Path.GetRelativePath(Path.GetDirectoryName(projectPath)!, header)), "compile a header",
            workingDirectory, environment, expectSuccess: false);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static string CompileFileCommand(string path, string configuration, string selected)
    {
        using var collection = new ProjectCollection();
        var project = new Project(path, new Dictionary<string, string> { ["Configuration"] = configuration, ["Platform"] = "x64", ["SelectedFiles"] = selected },
            null, collection, ProjectLoadSettings.IgnoreMissingImports | ProjectLoadSettings.IgnoreInvalidImports);
        return project.GetPropertyValue("NMakeCompileFileCommandLine");
    }

    // NMake처럼 명령줄 문자열을 cmd에 그대로 넘긴다.
    private static async Task RunCommand(string command, string what, string directory, IReadOnlyDictionary<string, string?>? environment, bool expectSuccess)
    {
        var start = new ProcessStartInfo("cmd.exe", "/d /s /c \"" + command + "\"")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = directory
        };
        if (environment is not null)
        {
            start.Environment.Clear();
            foreach ((string key, string? value) in environment)
            {
                start.Environment[key] = value;
            }
        }

        using var process = Process.Start(start)!;
        Task<string> output = process.StandardOutput.ReadToEndAsync();
        Task<string> errors = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        Require((process.ExitCode == 0) == expectSuccess, $"{what}: exit code {process.ExitCode}: " + await output + await errors);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (string Build, string Clean, string Rebuild, string Output) NMakeProperties(string path)
    {
        using var collection = new ProjectCollection();
        var project = new Project(path, new Dictionary<string, string> { ["Configuration"] = "Debug", ["Platform"] = "x64" }, null, collection,
            ProjectLoadSettings.IgnoreMissingImports | ProjectLoadSettings.IgnoreInvalidImports);
        return (project.GetPropertyValue("NMakeBuildCommandLine"), project.GetPropertyValue("NMakeCleanCommandLine"),
            project.GetPropertyValue("NMakeReBuildCommandLine"), project.GetPropertyValue("NMakeOutput"));
    }

    // NMake 명령의 -Compiler 값이다(터미널 빌드가 같은 컴파일러를 쓰게 한다).
    private static string Compiler(string command)
    {
        const string marker = "-Compiler \"";
        int start = command.IndexOf(marker, StringComparison.Ordinal) + marker.Length;
        return command[start..command.IndexOf('"', start)];
    }

    private static string LockName(string repository)
    {
        // Build/Tools.ps1과 같다: 디스크 철자로 맞춘 루트(끝 구분자 없음)의 소문자 UTF-8.
        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(Path.TrimEndingDirectorySeparator(PathSpelling.OnDisk(Path.GetFullPath(repository))).ToLowerInvariant());
        return @"Local\Rocks.IdeBuild." + Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(bytes))[..16];
    }

    private static bool Tool(string name)
    {
        string? path = Environment.GetEnvironmentVariable("PATH");
        return path is not null && path.Split(Path.PathSeparator).Any(directory =>
            File.Exists(Path.Combine(directory, name + ".exe")) || File.Exists(Path.Combine(directory, name)));
    }

    private static async Task Expect(Func<Task> action, string fragment)
    {
        try
        {
            await action();
        }
        catch (ArgumentException error) when (error.Message.Contains(fragment, StringComparison.Ordinal))
        {
            return;
        }

        throw new InvalidOperationException("Expected an error containing: " + fragment);
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
