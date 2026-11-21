using System.Text.Json.Nodes;
using Rocks.Build.Cli;
using Rocks.Build.Descriptors;

namespace Rocks.Build.IntegrationTests;

// 게임 프로젝트 선행 작업(Docs/Design/ProjectFiles.ko.md 10.1·10.2·10.5·10.6절, 16절 V8·V9)을 도구 모음 없이 검사한다.
// - V8: SdkAssociation.Resolve(경로·ID·호환 최고 패치·동률·없음·사라진 SDK), associate 쓰기, JSON·Windows 레지스트리 등록부.
// - V9: 게임 입력 snapshot과 규칙 탐색 제외(점 디렉터리, 최상위 out/·Saved/, 최상위 *.slnx). 쓰기 공유 없이 잠긴 .vs 파일도 읽지 않는다.
// - 게임 clean: 기본 출력(<project>/out/<platform>-<target>-<configuration>)과 경계(프로젝트 out의 엄격한 하위, 엔진 옵션 거부).
internal static class GameProjectTests
{
    private const string Incompatible = "incompatible-by-test";

    public static async Task RunAsync()
    {
        string root = Directory.CreateTempSubdirectory("rocks-game-project-").FullName;
        try
        {
            Association(Path.Combine(root, "association"));
            Registries(Path.Combine(root, "registry"));
            Snapshot(Path.Combine(root, "snapshot"));
            await CleanAsync(Path.Combine(root, "clean"));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }

        Console.WriteLine("PASS game project: SDK association and registries, input snapshot and rule exclusions, game clean boundaries");
    }

    private static void Association(string root)
    {
        string sdks = Path.Combine(root, "Sdks");
        string low = Sdk(sdks, "Rocks-0.1.1", 0, 1, 1);
        string high = Sdk(sdks, "Rocks-0.1.2", 0, 1, 2);
        string incompatible = Sdk(sdks, "Rocks-0.1.5-" + Incompatible, 0, 1, 5);
        string otherMinor = Sdk(sdks, "Rocks-0.2.9", 0, 2, 9);
        string missing = Path.Combine(sdks, "Rocks-0.1.7-removed");
        var registry = new MemoryRegistry();
        foreach (string sdk in new[] { low, high, incompatible, otherMinor, missing })
        {
            registry.Register(sdk);
        }

        string gameDirectory = Path.Combine(root, "Game");
        string projectFile = Path.Combine(gameDirectory, "Game.rocksproject");
        Directory.CreateDirectory(gameDirectory);
        File.WriteAllText(projectFile, "{\n  \"fileVersion\": 1,\n  \"engineVersion\": \"0.1\",\n  \"description\": \"게임\"\n}\n");
        static string? Check(string path) => path.Contains(Incompatible, StringComparison.Ordinal) ? "test toolchain mismatch" : null;
        string Resolve(MemoryRegistry entries) => SdkAssociation.Resolve(DescriptorReader.ReadProject(projectFile), entries, Check);

        // 연결이 없으면 호환 후보 중 주·부가 같은 최고 패치다: 호환되지 않는 0.1.5, 주·부가 다른 0.2.9, 사라진 0.1.7은 빠진다.
        Require(Same(Resolve(registry), high), "resolve must pick the highest compatible patch");
        string none = Error(() => Resolve(new MemoryRegistry()));
        Require(none.Contains("no SDK is registered", StringComparison.Ordinal), "an empty registry must say so: " + none);
        var onlyBad = new MemoryRegistry();
        onlyBad.Register(incompatible);
        onlyBad.Register(otherMinor);
        onlyBad.Register(missing);
        string reasons = Error(() => Resolve(onlyBad));
        Require(reasons.Contains("test toolchain mismatch", StringComparison.Ordinal) && reasons.Contains("engine 0.2.9", StringComparison.Ordinal) &&
            reasons.Contains("missing (unregister it", StringComparison.Ordinal), "no candidate must list every reason: " + reasons);
        string twin = Sdk(Path.Combine(root, "Twins"), "Rocks-0.1.2", 0, 1, 2);
        registry.Register(twin);
        string tie = Error(() => Resolve(registry));
        Require(tie.Contains("all fit engineVersion", StringComparison.Ordinal) && tie.Contains("associate", StringComparison.Ordinal),
            "two SDKs at the highest patch must be ambiguous: " + tie);

        // associate: 경로는 프로젝트 기준 상대 경로로 쓰고, 다른 필드와 순서는 그대로 둔다.
        Require(SdkCommands.Run(SdkCommands.Associate, new() { ["project"] = gameDirectory, ["sdk"] = low }, registry) == 0, "associate --sdk");
        JsonObject written = JsonNode.Parse(File.ReadAllText(projectFile))!.AsObject();
        Require(written["engineAssociation"]!.GetValue<string>() == "../Sdks/Rocks-0.1.1", "associate must write a project-relative path");
        Require(written.Select(pair => pair.Key).SequenceEqual(["fileVersion", "engineVersion", "description", "engineAssociation"]) &&
            written["description"]!.GetValue<string>() == "게임", "associate must keep the other fields and their order");
        Require(Same(Resolve(registry), low), "a path association wins over the registry choice");

        // 프로젝트 디렉터리 안의 SDK는 구분자가 없으므로 ./를 붙여 경로로 읽히게 한다.
        string inside = Sdk(gameDirectory, "Sdk", 0, 1, 0);
        SdkCommands.Run(SdkCommands.Associate, new() { ["project"] = projectFile, ["sdk"] = inside }, registry);
        Require(DescriptorReader.ReadProject(projectFile).EngineAssociation == "./Sdk" && Same(Resolve(registry), inside), "associate --sdk inside the project");

        string id = registry.List().Single(entry => Same(entry.Path, twin)).Id;
        SdkCommands.Run(SdkCommands.Associate, new() { ["project"] = projectFile, ["id"] = id.ToLowerInvariant() }, registry);
        Require(Same(Resolve(registry), twin), "an ID association resolves through the registry (case-insensitive)");
        registry.Unregister(id);
        string gone = Error(() => Resolve(registry));
        Require(gone.Contains("is not a registered SDK", StringComparison.Ordinal) && gone.Contains("Registered:", StringComparison.Ordinal),
            "an unknown ID must list the registered SDKs: " + gone);
        Error(() => SdkCommands.Run(SdkCommands.Associate, new() { ["project"] = projectFile, ["id"] = id }, registry));
        Error(() => SdkCommands.Run(SdkCommands.Associate, new() { ["project"] = projectFile, ["sdk"] = low, ["id"] = id }, registry));
        Error(() => SdkCommands.Run(SdkCommands.Associate, new() { ["project"] = projectFile, ["version"] = "off" }, registry));
        Error(() => SdkCommands.Run(SdkCommands.Associate, new() { ["project"] = projectFile, ["sdk"] = missing }, registry));

        SdkCommands.Run(SdkCommands.Associate, new() { ["project"] = projectFile, ["version"] = "on" }, registry);
        Require(DescriptorReader.ReadProject(projectFile).EngineAssociation is null && !File.ReadAllText(projectFile).Contains("engineAssociation", StringComparison.Ordinal),
            "associate --version on must remove the field");

        // 경로 연결이 SDK가 아닌 곳을 가리키면 오류다.
        SdkAssociation.Write(projectFile, "../Sdks/Rocks-0.1.7-removed");
        Require(Error(() => Resolve(registry)).Contains("is not an installed SDK", StringComparison.Ordinal), "a path association without a manifest");
    }

    private static void Registries(string root)
    {
        string first = Sdk(root, "First", 0, 1, 0);
        string second = Sdk(root, "Second", 0, 1, 1);
        string file = Path.Combine(root, "config", "Sdks.json");
        Exercise(new JsonSdkRegistry(file), first, second, "JSON");
        JsonNode saved = JsonNode.Parse(File.ReadAllText(file))!;
        Require(saved["fileVersion"]!.GetValue<int>() == 1 && saved["sdks"]!.AsArray().Count == 1, "JSON registry file format");
        File.WriteAllText(file, "{ broken");
        Require(new JsonSdkRegistry(file).List().Count == 0, "a broken JSON registry reads as empty");

        if (OperatingSystem.IsWindows())
        {
            string subKey = @"Software\Rocks\Tests\" + Guid.NewGuid().ToString("B");
            try
            {
                Exercise(new WindowsSdkRegistry(subKey), first, second, "registry");
            }
            finally
            {
                Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(subKey, throwOnMissingSubKey: false);
            }
        }

        // 환경 변수 재정의는 호스트와 무관하게 JSON 파일이다(시험·CI용).
        string overrideFile = Path.Combine(root, "override", "Sdks.json");
        string? previous = Environment.GetEnvironmentVariable(SdkRegistries.OverrideVariable);
        Environment.SetEnvironmentVariable(SdkRegistries.OverrideVariable, overrideFile);
        try
        {
            Require(SdkRegistries.Default() is JsonSdkRegistry, "the override variable must select a JSON registry");
            Require(SdkCommands.Run(SdkCommands.Register, new() { ["sdk"] = first }, SdkRegistries.Default()) == 0 && File.Exists(overrideFile),
                "sdk-register must write the override registry");
            Require(SdkCommands.Run(SdkCommands.Unregister, new() { ["sdk"] = first }, SdkRegistries.Default()) == 0 &&
                SdkRegistries.Default().List().Count == 0, "sdk-unregister --sdk");
            Error(() => SdkCommands.Run(SdkCommands.Register, new() { ["sdk"] = root }, SdkRegistries.Default()));
        }
        finally
        {
            Environment.SetEnvironmentVariable(SdkRegistries.OverrideVariable, previous);
        }
    }

    // 등록은 같은 경로면 같은 ID이고, ID(대소문자 무관)·경로로 지운다.
    private static void Exercise(ISdkRegistry registry, string first, string second, string what)
    {
        Require(registry.List().Count == 0, what + ": a new registry is empty");
        string id = registry.Register(first);
        Require(Guid.TryParse(id, out _) && id.StartsWith('{') && registry.Register(first + Path.DirectorySeparatorChar) == id,
            what + ": registering the same path again keeps its ID");
        string other = registry.Register(second);
        Require(other != id && registry.List().Count == 2, what + ": two SDKs");
        Require(registry.Unregister(id.ToLowerInvariant()) && registry.List().Single().Id == other, what + ": unregister by ID");
        Require(!registry.Unregister(id), what + ": unregistering twice");
        Require(registry.Unregister(second) && registry.List().Count == 0, what + ": unregister by path");
        registry.Register(first);
    }

    private static void Snapshot(string root)
    {
        string project = Path.Combine(root, "Game");
        string sdk = Path.Combine(root, "Sdk");
        Directory.CreateDirectory(sdk);
        string output = GameProjectLayout.DefaultOutput(project, "Game", NativeConfiguration.Debug);
        Write(project, "Game.Build.cs", "// rules");
        Write(project, "Game.Target.cs", "// target");
        Write(project, "Source/Main.cpp", "int main() { return 0; }");
        Write(project, "Source/Tools/Extra.Build.cs", "// nested rules");
        Write(project, "Source/Saved/Kept.txt", "nested Saved is an input");
        Write(project, "Source/out/Kept.txt", "nested out is an input");
        Write(project, "Source/Game.slnx", "nested solution is an input");
        Write(project, ".editorconfig", "dot files are inputs");
        string Capture() => GameInputSnapshot.Capture(project, output, sdk, []);
        string baseline = Capture();

        // 빼는 곳은 만들고 바꿔도 snapshot이 같다. .vs 파일은 쓰기 공유 없이 연 채로 둔다(Visual Studio가 Browse.VC.db를 그렇게 연다).
        Write(project, ".vs/Game/v18/Browse.VC.db", "database");
        Write(project, "Source/.cache/index", "nested dot directory");
        Write(project, "Saved/Crashes/crash.dmp", "dump");
        Write(project, "out/windows-game-release/action-report.json", "{}");
        Write(project, "out/ProjectFiles/Game.vcxproj", "<Project />");
        Write(project, "out/windows-game-release/Copy.Build.cs", "// a copy is not a rule");
        Write(project, "Saved/Copy.Target.cs", "// a copy is not a rule");
        Write(project, ".vs/Copy.Build.cs", "// a copy is not a rule");
        Write(project, "Game.slnx", "<Solution />");
        Write(output, "obj/a.obj", "object");
        using (new FileStream(Path.Combine(project, ".vs/Game/v18/Browse.VC.db"), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        {
            Require(Capture() == baseline, "excluded paths must not change the game input snapshot");
        }

        Require(GameProjectLayout.RuleFiles(project, output).Select(file => Path.GetRelativePath(project, file).Replace('\\', '/'))
            .SequenceEqual(["Game.Build.cs", "Game.Target.cs", "Source/Tools/Extra.Build.cs"]), "rule discovery must skip excluded directories: " +
            string.Join(", ", GameProjectLayout.RuleFiles(project, output)));

        // 명시 입력은 제외 규칙과 무관하다.
        Require(GameInputSnapshot.Capture(project, output, sdk, [Path.Combine(project, "Saved/Crashes/crash.dmp")]) != baseline,
            "an explicit input under an excluded directory must be captured");

        // 입력은 지금처럼 잡는다: 소스, 중첩된 Saved/·out/, 점 파일.
        foreach (string input in new[] { "Source/Main.cpp", "Source/Saved/Kept.txt", "Source/out/Kept.txt", "Source/Game.slnx", ".editorconfig" })
        {
            string path = Path.Combine(project, input);
            string text = File.ReadAllText(path);
            File.WriteAllText(path, text + " changed");
            Require(Capture() != baseline, input + " must stay a game input");
            File.WriteAllText(path, text);
        }

        Require(Capture() == baseline, "restoring the inputs must restore the snapshot");
        Require(GameProjectLayout.IsExcluded(project, Path.Combine(project, "out")) && GameProjectLayout.IsExcluded(project, Path.Combine(project, "SAVED", "x")) ==
            OperatingSystem.IsWindows() && !GameProjectLayout.IsExcluded(project, project) &&
            !GameProjectLayout.IsExcluded(project, Path.Combine(root, ".outside")), "exclusion edge cases");
    }

    private static async Task CleanAsync(string root)
    {
        string project = Path.Combine(root, "Game");
        Directory.CreateDirectory(project);
        Dictionary<string, string> Options(params (string Key, string Value)[] extra)
        {
            var options = new Dictionary<string, string> { ["project"] = project, ["target"] = "Game", ["configuration"] = "Debug" };
            foreach ((string key, string value) in extra)
            {
                options[key] = value;
            }

            return options;
        }

        string output = GameProjectLayout.DefaultOutput(project, "Game", NativeConfiguration.Debug);
        Require(Path.GetFileName(output) == $"{GameProjectLayout.HostPlatformName}-game-debug", "game output name: " + output);
        Require(await CleanCommand.RunAsync(Options(), CancellationToken.None) == 0, "a missing game output is nothing to clean");
        Write(output, "bin/Game.exe", "x");
        await Expect(() => CleanCommand.RunAsync(Options(), CancellationToken.None), "has no .native-build.lock");
        Write(output, CleanCommand.OutputMarker, "");
        string other = GameProjectLayout.DefaultOutput(project, "Game", NativeConfiguration.Release);
        Write(other, "bin/Game.exe", "release");
        Require(await CleanCommand.RunAsync(Options(), CancellationToken.None) == 0, "game clean");
        Require(Directory.EnumerateFileSystemEntries(output).Select(Path.GetFileName).Order(StringComparer.Ordinal)
            .SequenceEqual([CleanCommand.OutputMarker, ".rocks-invocation.lock"], StringComparer.Ordinal), "game clean must keep only the lock files");
        Require(File.Exists(Path.Combine(other, "bin/Game.exe")), "game clean must not touch another configuration");

        await Expect(() => CleanCommand.RunAsync(Options(("output", Path.Combine(project, "out"))), CancellationToken.None), "only build outputs under");
        await Expect(() => CleanCommand.RunAsync(Options(("output", Path.Combine(project, "Source"))), CancellationToken.None), "only build outputs under");
        await Expect(() => CleanCommand.RunAsync(Options(("output", Path.Combine(root, "out", "x"))), CancellationToken.None), "only build outputs under");
        await Expect(() => CleanCommand.RunAsync(Options(("root", root)), CancellationToken.None), "belong to engine targets");
        await Expect(() => CleanCommand.RunAsync(Options(("platform", "android")), CancellationToken.None), "belong to engine targets");
    }

    private sealed class MemoryRegistry : ISdkRegistry
    {
        private readonly List<SdkRegistration> entries = [];

        public IReadOnlyList<SdkRegistration> List() => [.. entries.OrderBy(entry => entry.Id, StringComparer.Ordinal)];

        public string Register(string path)
        {
            string full = Path.GetFullPath(path);
            SdkRegistration? existing = entries.FirstOrDefault(entry => Same(entry.Path, full));
            if (existing is not null)
            {
                return existing.Id;
            }

            string id = Guid.NewGuid().ToString("B").ToUpperInvariant();
            entries.Add(new(id, full));
            return id;
        }

        public bool Unregister(string idOrPath) =>
            entries.RemoveAll(entry => string.Equals(entry.Id, idOrPath, StringComparison.OrdinalIgnoreCase) || Same(entry.Path, idOrPath)) != 0;
    }

    // SDK 해석이 보는 manifest 칸(EngineVersion)만 가진 가짜 SDK다.
    private static string Sdk(string parent, string name, int major, int minor, int patch)
    {
        string path = Path.Combine(parent, name);
        Write(path, SdkAssociation.ManifestName, $"{{ \"EngineVersion\": {{ \"Major\": {major}, \"Minor\": {minor}, \"Patch\": {patch} }} }}");
        return path;
    }

    private static void Write(string directory, string relative, string text)
    {
        string path = Path.Combine(directory, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
    }

    private static bool Same(string left, string right) =>
        string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(left)), Path.TrimEndingDirectorySeparator(Path.GetFullPath(right)),
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);

    private static string Error(Action action)
    {
        try
        {
            action();
        }
        catch (ArgumentException error)
        {
            return error.Message;
        }

        throw new InvalidOperationException("Expected an ArgumentException");
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
