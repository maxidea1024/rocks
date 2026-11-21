using System.Diagnostics;
using System.Runtime.CompilerServices;
using Microsoft.Build.Evaluation;
using Microsoft.VisualStudio.SolutionPersistence.Serializer;
using Rocks.Build.Cli;
using Rocks.Tooling.Common;

namespace Rocks.Build.IntegrationTests;

// 게임 모드 프로젝트 파일(Docs/Design/ProjectFiles.ko.md 10.4절, 16절 V2–V4·V6)을 실제 설치 SDK와 템플릿 게임으로 검사한다. Windows 호스트만.
// - V4: 두 번째 생성은 아무 파일도 쓰지 않는다.
// - V3: 솔루션을 다시 읽는다. 게임 프로젝트는 폴더 밖, RocksSdk는 /Rocks/ 아래이고 솔루션 빌드에서 빠진다. 모든 프로젝트 파일이 있다.
// - V2: 게임 프로젝트를 MSBuild로 평가한다. NMake 명령이 SDK의 IdeBuild를 게임 형태로 부르고, NMakeOutput이 게임 출력의 실행 파일이고,
//   ClCompile 항목이 게임 소스이며 모두 존재한다. RocksSdk는 SDK 헤더만 ClInclude로 담는다.
// - V6: 평가한 NMake Build·Clean·Rebuild 명령을 cmd로 돌린다(실행 파일 생성, Clean 뒤 잠금 파일만, Rebuild 뒤 다시 생성).
// - associate: 생성 상태가 있으면 연결을 바꾼 뒤 프로젝트 파일을 다시 만든다.
internal static class GameProjectFilesTests
{
    public static async Task RunAsync(string cli, string sdk, string game, string compiler, IReadOnlyDictionary<string, string?> environment)
    {
        if (!OperatingSystem.IsWindows())
        {
            Console.WriteLine("SKIP game project files: Windows hosts only");
            return;
        }

        async Task<ProcessResult> Cli(params string[] arguments)
        {
            var result = await ProcessRunner.RunAsync(new("dotnet", [cli, .. arguments], game, environment, Timeout: TimeSpan.FromMinutes(10)));
            Require(result.ExitCode == 0, "Rocks.Build.Cli " + string.Join(' ', arguments) + " failed: " + result.StdOut + result.StdErr);
            return result;
        }

        await Cli("projectfiles", "--project", game, "--sdk", sdk, "--compiler", compiler);
        // 게임 이름은 .rocksproject 이름이고 기본 타깃도 그 이름이다(템플릿에서 만든 프로젝트).
        string name = GameProjectLayout.DefaultTarget(game);
        string descriptorFile = Path.Combine(game, name + ".rocksproject");
        string projectFiles = Path.Combine(game, "out", "ProjectFiles");
        string solution = Path.Combine(game, name + ".slnx");
        string gameProject = Path.Combine(projectFiles, name + ".vcxproj");
        string sdkProject = Path.Combine(projectFiles, GameProjectFiles.HeaderProjectName + ".vcxproj");
        Require(File.Exists(solution) && File.Exists(gameProject) && File.Exists(sdkProject), "game projectfiles did not write the solution and projects");

        // model-only: 종료 0이고 게임 디렉터리에 아무것도 쓰지 않는다(규칙은 임시 디렉터리에 컴파일한다).
        var snapshot = Directory.GetFiles(game, "*", SearchOption.AllDirectories).ToDictionary(file => file, File.GetLastWriteTimeUtc, StringComparer.OrdinalIgnoreCase);
        ProcessResult modelOnly = await ProcessRunner.RunAsync(new("dotnet", [cli, "projectfiles", "--project", game, "--sdk", sdk, "--compiler", compiler,
            "--model-only", "on"], game, environment, Timeout: TimeSpan.FromMinutes(10)));
        var after = Directory.GetFiles(game, "*", SearchOption.AllDirectories).ToDictionary(file => file, File.GetLastWriteTimeUtc, StringComparer.OrdinalIgnoreCase);
        Require(modelOnly.ExitCode == 0 && after.Count == snapshot.Count && after.All(pair => snapshot.TryGetValue(pair.Key, out DateTime written) && written == pair.Value),
            "model-only must succeed without writing into the game: " + modelOnly.ExitCode + " " + modelOnly.StdOut + modelOnly.StdErr);

        // 동시 생성: 생성 영역 잠금으로 차례로 돌아 둘 다 성공한다.
        ProcessResult[] concurrent = await Task.WhenAll(Enumerable.Range(0, 2).Select(_ => ProcessRunner.RunAsync(new("dotnet",
            [cli, "projectfiles", "--project", game, "--sdk", sdk, "--compiler", compiler], game, environment, Timeout: TimeSpan.FromMinutes(10)))));
        Require(concurrent.All(result => result.ExitCode == 0), "concurrent game generations failed: " + string.Join("\n", concurrent.Select(result => result.StdOut + result.StdErr)));

        // V4: 같은 입력이면 다시 쓰지 않는다.
        var again = await Cli("projectfiles", "--project", descriptorFile, "--sdk", sdk, "--compiler", compiler);
        Require(again.StdOut.Contains(": 0 written,", StringComparison.Ordinal), "a second game generation wrote files: " + again.StdOut);

        // V3: 솔루션 왕복.
        var model = await SolutionSerializers.SlnXml.OpenAsync(solution, CancellationToken.None);
        var entries = model.SolutionProjects.ToArray();
        Require(entries.Length == 2 && entries.All(entry => File.Exists(Path.GetFullPath(entry.FilePath, game))), "game solution projects: " +
            string.Join(", ", entries.Select(entry => entry.FilePath)));
        Require(entries.Single(entry => Path.GetFileName(entry.FilePath) == name + ".vcxproj").Parent is null &&
            entries.Single(entry => entry.FilePath.EndsWith(GameProjectFiles.HeaderProjectName + ".vcxproj", StringComparison.Ordinal)).Parent?.Path == "/Rocks/",
            "game solution folders");
        string solutionText = File.ReadAllText(solution);
        int sdkEntry = solutionText.IndexOf(GameProjectFiles.HeaderProjectName + ".vcxproj", StringComparison.Ordinal);
        Require(sdkEntry >= 0 && solutionText.IndexOf("<Build Project=\"false\" />", sdkEntry, StringComparison.Ordinal) is int excluded && excluded > sdkEntry &&
            solutionText.IndexOf("</Project>", sdkEntry, StringComparison.Ordinal) > excluded, "RocksSdk must be left out of the solution build:\n" + solutionText);

        // V2: MSBuild 평가.
        GameProjectValues values = Evaluate(gameProject, sdkProject);
        Require(values.Build.Contains(Path.Combine(sdk, "Build", "IdeBuild.bat"), StringComparison.OrdinalIgnoreCase) &&
            values.Build.Contains("-Project \"" + descriptorFile + "\"", StringComparison.OrdinalIgnoreCase) &&
            values.Build.Contains($"-Target {name} -Configuration Debug", StringComparison.Ordinal), "game NMake build command: " + values.Build);
        Require(string.Equals(values.Output, Path.Combine(GameProjectLayout.DefaultOutput(game, name, NativeConfiguration.Debug), "bin", name + ".exe"),
            StringComparison.OrdinalIgnoreCase), "game NMakeOutput: " + values.Output);
        string[] expected = [.. Directory.GetFiles(Path.Combine(game, "Source"), "*.cpp").Select(Path.GetFullPath).Order(StringComparer.OrdinalIgnoreCase)];
        Require(values.Sources.Order(StringComparer.OrdinalIgnoreCase).SequenceEqual(expected, StringComparer.OrdinalIgnoreCase) && values.Sources.All(File.Exists),
            "game ClCompile items: " + string.Join(", ", values.Sources));
        Require(values.SdkSources.Count == 0 && values.SdkHeaders.Count != 0 && values.SdkHeaders.All(header => DirectoryPath.IsAtOrUnder(header, sdk) && File.Exists(header)) &&
            values.SdkBuild == "@rem Nothing to build.", "RocksSdk must hold only existing SDK headers and build nothing");

        // V6: NMake 명령 실행.
        string output = GameProjectLayout.DefaultOutput(game, name, NativeConfiguration.Debug);
        await Run(values.Build, "game NMake build", game);
        Require(File.Exists(values.Output), "game NMake build did not build " + values.Output);
        await Run(values.Clean, "game NMake clean", game);
        string[] left = [.. Directory.EnumerateFileSystemEntries(output).Select(Path.GetFileName).Order(StringComparer.Ordinal)!];
        Require(left.SequenceEqual([CleanCommand.OutputMarker, ".rocks-invocation.lock"], StringComparer.Ordinal), "game NMake clean left " + string.Join(", ", left));
        await Run(values.Rebuild, "game NMake rebuild", game);
        Require(File.Exists(values.Output), "game NMake rebuild did not build " + values.Output);

        // 파일 하나 컴파일(Ctrl+F7, 7.4절): 게임 소스 하나를 SDK의 IdeBuild로 논리 명령 그대로 컴파일한다.
        await IdeScriptsTests.CompileSelectedAsync(gameProject, "Debug", output, Path.Combine(game, "Source", "Main.cpp"),
            Path.Combine(game, "Source", "GameObject.h"), game, environment);

        // associate: 생성 상태가 있으면 다시 만든다.
        var associated = await Cli("associate", "--project", game, "--sdk", sdk);
        Require(associated.StdOut.Contains("Regenerating the game project files", StringComparison.Ordinal) &&
            associated.StdOut.Contains(": 0 written,", StringComparison.Ordinal), "associate did not regenerate: " + associated.StdOut);

        // 다시 만들기가 실패하면 연결은 바뀐 채 종료 코드 2이고, 옛 프로젝트 파일의 IDE 빌드는 낡았다고 멈춘다(옛 SDK로 조용히 빌드하지 않는다).
        string other = Path.Combine(Path.GetDirectoryName(game)!, "other-sdk");
        Directory.CreateDirectory(other);
        File.WriteAllText(Path.Combine(other, SdkAssociation.ManifestName), "{}");
        try
        {
            ProcessResult moved = await ProcessRunner.RunAsync(new("dotnet", [cli, "associate", "--project", game, "--sdk", other], game, environment,
                Timeout: TimeSpan.FromMinutes(10)));
            Require(moved.ExitCode == GameProjectFiles.AssociationKeptExitCode && moved.StdErr.Contains("were not regenerated", StringComparison.Ordinal) &&
                File.ReadAllText(descriptorFile).Contains("other-sdk", StringComparison.Ordinal),
                "a failed regeneration after associate: " + moved.ExitCode + " " + moved.StdOut + moved.StdErr);
            string staleOutput = await RunFailing(values.Build, game);
            Require(staleOutput.Contains("project files are stale", StringComparison.Ordinal), "a stale game project built with the old SDK: " + staleOutput);
        }
        finally
        {
            // 연결을 지워 원래대로 둔다. 시험 등록부가 비어 있어 다시 만들기는 실패하고(2) 연결은 지워진다.
            ProcessResult removed = await ProcessRunner.RunAsync(new("dotnet", [cli, "associate", "--project", game, "--version", "on"], game, environment,
                Timeout: TimeSpan.FromMinutes(10)));
            Require(removed.ExitCode == GameProjectFiles.AssociationKeptExitCode &&
                !File.ReadAllText(descriptorFile).Contains("engineAssociation", StringComparison.Ordinal),
                "associate --version on without a registered SDK: " + removed.ExitCode + " " + removed.StdOut + removed.StdErr);
            Directory.Delete(other, recursive: true);
        }

        Console.WriteLine("PASS game project files: generation, no-op regeneration, solution round trip, MSBuild evaluation, NMake build/clean/rebuild, associate");
    }

    private sealed record GameProjectValues(string Build, string Clean, string Rebuild, string Output, IReadOnlyList<string> Sources,
        string SdkBuild, IReadOnlyList<string> SdkSources, IReadOnlyList<string> SdkHeaders);

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static GameProjectValues Evaluate(string gameProject, string sdkProject)
    {
        using var collection = new ProjectCollection();
        var settings = ProjectLoadSettings.IgnoreMissingImports | ProjectLoadSettings.IgnoreInvalidImports;
        var properties = new Dictionary<string, string> { ["Configuration"] = "Debug", ["Platform"] = "x64" };
        var game = new Project(gameProject, properties, null, collection, settings);
        var sdk = new Project(sdkProject, properties, null, collection, settings);
        string Full(Project project, ProjectItem item) => Path.GetFullPath(item.EvaluatedInclude, Path.GetDirectoryName(project.FullPath)!);
        return new(game.GetPropertyValue("NMakeBuildCommandLine"), game.GetPropertyValue("NMakeCleanCommandLine"), game.GetPropertyValue("NMakeReBuildCommandLine"),
            game.GetPropertyValue("NMakeOutput"), [.. game.GetItems("ClCompile").Select(item => Full(game, item))], sdk.GetPropertyValue("NMakeBuildCommandLine"),
            [.. sdk.GetItems("ClCompile").Select(item => Full(sdk, item))], [.. sdk.GetItems("ClInclude").Select(item => Full(sdk, item))]);
    }

    // 실패해야 하는 NMake 명령을 돌리고 출력을 돌려준다.
    private static async Task<string> RunFailing(string command, string directory)
    {
        var start = new ProcessStartInfo("cmd.exe", "/d /s /c \"" + command + "\"")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = directory
        };
        using var process = Process.Start(start)!;
        Task<string> output = process.StandardOutput.ReadToEndAsync();
        Task<string> errors = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        string text = await output + await errors;
        Require(process.ExitCode != 0, "the command was expected to fail: " + text);
        return text;
    }

    // NMake처럼 명령줄 문자열을 cmd에 그대로 넘긴다(인자 목록으로 넘기면 따옴표가 이스케이프된다).
    private static async Task Run(string command, string what, string directory)
    {
        var start = new ProcessStartInfo("cmd.exe", "/d /s /c \"" + command + "\"")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = directory
        };
        using var process = Process.Start(start)!;
        Task<string> output = process.StandardOutput.ReadToEndAsync();
        Task<string> errors = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        Require(process.ExitCode == 0, what + " failed: " + await output + await errors);
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
