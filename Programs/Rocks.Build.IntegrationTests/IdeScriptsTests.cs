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
        await LockOrderAsync(repository);
        await ToolInputsAsync(repository);
        await NMakeCommandsAsync(repository);
        Console.WriteLine("PASS IDE scripts: clean boundaries, IDE build lock order, tool input coverage, NMake build/clean/rebuild and terminal no-op");
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
            Environment = { ["ROCKS_IDE_BUILD_PROBE"] = probe }
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

    // V6: 생성 프로젝트의 NMake 명령을 cmd로 돌린다.
    private static async Task NMakeCommandsAsync(string repository)
    {
        string project = Path.Combine(repository, "out", "ProjectFiles", "Programs", SmallTarget + ".vcxproj");
        if (!File.Exists(project))
        {
            Console.WriteLine($"SKIP IDE scripts V6: {project} does not exist; run projectfiles first");
            return;
        }

        (string build, string clean, string rebuild, string output) = NMakeProperties(project);
        string outputDirectory = Path.GetDirectoryName(Path.GetDirectoryName(output)!)!;
        await Run(build, "NMake build");
        Require(File.Exists(output), "NMakeOutput was not built: " + output);
        var terminal = await ProcessRunner.RunAsync(new("dotnet", ["msbuild", Path.Combine(repository, "Rocks.proj"), "-t:Build", "-nologo",
            "-p:RocksTarget=" + SmallTarget, "-p:Configuration=Debug", "-p:Compiler=" + Compiler(build)], repository));
        Require(terminal.ExitCode == 0 && terminal.StdOut.Contains("executed=0;", StringComparison.Ordinal),
            "the terminal build after the IDE build was not a no-op: " + terminal.StdOut);
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
            using var process = Process.Start(start)!;
            Task<string> output = process.StandardOutput.ReadToEndAsync();
            Task<string> errors = process.StandardError.ReadToEndAsync();
            await process.WaitForExitAsync();
            Require(process.ExitCode == 0, what + " failed: " + await output + await errors);
        }
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
        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(Path.TrimEndingDirectorySeparator(Path.GetFullPath(repository)).ToLowerInvariant());
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
