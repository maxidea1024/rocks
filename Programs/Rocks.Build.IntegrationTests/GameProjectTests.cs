using System.Text.Json.Nodes;
using Rocks.Build.Cli;
using Rocks.Build.Descriptors;
using Rocks.Tooling.Common;

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
            await Registries(Path.Combine(root, "registry"));
            Snapshot(Path.Combine(root, "snapshot"));
            await CleanAsync(Path.Combine(root, "clean"));
            Assembly(Path.Combine(root, "assembly"));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }

        Console.WriteLine("PASS game project: SDK association and registries, input snapshot and rule exclusions, game clean boundaries, " +
            "multi-configuration SDK assembly");
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
            reasons.Contains("registered but missing", StringComparison.Ordinal), "no candidate must list every reason: " + reasons);
        string twin = Sdk(Path.Combine(root, "Twins"), "Rocks-0.1.2", 0, 1, 2);
        registry.Register(twin);
        string tie = Error(() => Resolve(registry));
        Require(tie.Contains("all fit engineVersion", StringComparison.Ordinal) && tie.Contains("associate", StringComparison.Ordinal),
            "two SDKs at the highest patch must be ambiguous: " + tie);

        // 같은 SDK를 두 ID로 등록해도 동률이 아니다(경로로 중복을 지운다).
        var duplicated = new MemoryRegistry();
        duplicated.Add(SdkRegistries.NewId(), high);
        duplicated.Add(SdkRegistries.NewId(), high + Path.DirectorySeparatorChar);
        Require(Same(Resolve(duplicated), high), "one SDK registered under two IDs must not be ambiguous");

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
        // 등록됐지만 사라진 SDK는 "registered but missing"으로 알린다.
        var vanished = new MemoryRegistry();
        vanished.Add(id, missing);
        string vanishedError = Error(() => Resolve(vanished));
        Require(vanishedError.Contains("is registered but missing", StringComparison.Ordinal) && vanishedError.Contains("sdk-unregister --id", StringComparison.Ordinal),
            "a registered but missing SDK: " + vanishedError);
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

    private static async Task Registries(string root)
    {
        string first = Sdk(root, "First", 0, 1, 0);
        string second = Sdk(root, "Second", 0, 1, 1);
        string file = Path.Combine(root, "config", "Sdks.json");
        Exercise(new JsonSdkRegistry(file), first, second, "JSON");
        JsonNode saved = JsonNode.Parse(File.ReadAllText(file))!;
        Require(saved["fileVersion"]!.GetValue<int>() == 1 && saved["sdks"]!.AsArray().Count == 1, "JSON registry file format");
        JsonFileRobustness(Path.Combine(root, "robust"), first, second);
        Concurrency(Path.Combine(root, "concurrent", "Sdks.json"));
        await CrossProcessAsync(Path.Combine(root, "processes", "Sdks.json"));

        if (OperatingSystem.IsWindows())
        {
            const string testsKey = @"Software\Rocks\Tests";
            string subKey = testsKey + @"\" + Guid.NewGuid().ToString("B");
            try
            {
                Exercise(new WindowsSdkRegistry(subKey), first, second, "registry");
                using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(subKey))
                {
                    key.SetValue("{00000000-0000-0000-0000-000000000001}", 5, Microsoft.Win32.RegistryValueKind.DWord);
                    key.SetValue("{00000000-0000-0000-0000-000000000002}", "relative/sdk", Microsoft.Win32.RegistryValueKind.String);
                }

                var problems = new List<string>();
                Require(new WindowsSdkRegistry(subKey).List(problems).Count == 1 && problems.Count == 2 &&
                    problems.Any(problem => problem.Contains("not a string", StringComparison.Ordinal)) &&
                    problems.Any(problem => problem.Contains("not an absolute path", StringComparison.Ordinal)), "registry: unreadable values are reported");
                Require(new WindowsSdkRegistry(subKey).Unregister("{00000000-0000-0000-0000-000000000001}"), "registry: an unreadable value can be removed by ID");
            }
            finally
            {
                Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(subKey, throwOnMissingSubKey: false);
                // 시험 키의 부모가 비었으면 지운다(다른 시험이 쓰는 중이면 남긴다).
                bool empty;
                using (var tests = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(testsKey))
                {
                    empty = tests is not null && tests.SubKeyCount == 0 && tests.ValueCount == 0;
                }

                if (empty)
                {
                    Microsoft.Win32.Registry.CurrentUser.DeleteSubKey(testsKey, throwOnMissingSubKey: false);
                }
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

    // JSON 등록부: 읽기는 틀린 항목을 이유와 함께 빼고, 쓰기는 그 항목과 모르는 칸을 보존한다. 깨졌거나 판을 모르는 파일은 쓰지 않는다.
    private static void JsonFileRobustness(string root, string first, string second)
    {
        string file = Path.Combine(root, "Sdks.json");
        Directory.CreateDirectory(root);
        var initial = new JsonObject
        {
            ["fileVersion"] = 1,
            ["note"] = "kept",
            ["sdks"] = new JsonArray(
                new JsonObject { ["id"] = 5 },
                new JsonObject { ["id"] = "{00000000-0000-0000-0000-000000000003}", ["path"] = "relative/sdk" },
                new JsonObject { ["id"] = "{00000000-0000-0000-0000-000000000004}", ["path"] = Path.GetFullPath(first), ["extra"] = true })
        };
        File.WriteAllText(file, initial.ToJsonString());
        var registry = new JsonSdkRegistry(file);
        var problems = new List<string>();
        Require(registry.List(problems).Single().Id == "{00000000-0000-0000-0000-000000000004}" && problems.Count == 2 &&
            problems.Any(problem => problem.Contains("not an absolute path", StringComparison.Ordinal)), "JSON: unreadable entries are reported: " +
            string.Join("; ", problems));
        registry.Register(second);
        JsonObject saved = JsonNode.Parse(File.ReadAllText(file))!.AsObject();
        JsonArray sdks = saved["sdks"]!.AsArray();
        Require(saved["note"]!.GetValue<string>() == "kept" && sdks.Count == 4 && sdks[0]!["id"]!.GetValue<int>() == 5 &&
            sdks[2]!["extra"]!.GetValue<bool>() && registry.List().Count == 2, "JSON: registering must keep unreadable entries and unknown fields");
        Require(registry.Unregister("{00000000-0000-0000-0000-000000000004}") && JsonNode.Parse(File.ReadAllText(file))!["sdks"]!.AsArray().Count == 3,
            "JSON: unregistering removes only that entry");

        foreach (string text in new[] { "{ broken", "{ \"fileVersion\": 2, \"sdks\": [] }", "{ \"fileVersion\": 1, \"sdks\": {} }",
            "{ \"fileVersion\": 1, \"fileVersion\": 1, \"sdks\": [] }", "{ \"fileVersion\": 1, \"sdks\": [ { \"id\": \"a\", \"id\": \"b\", \"path\": \"x\" } ] }" })
        {
            File.WriteAllText(file, text);
            var unreadable = new List<string>();
            Require(registry.List(unreadable).Count == 0 && unreadable.Count == 1, "JSON: an unreadable file lists nothing and says why: " + text);
            try
            {
                registry.Register(first);
                throw new InvalidOperationException("JSON: registering into an unreadable file must fail: " + text);
            }
            catch (InvalidDataException error) when (error.Message.Contains("Next:", StringComparison.Ordinal))
            {
            }

            Require(File.ReadAllText(file) == text, "JSON: an unreadable registry file must stay unchanged: " + text);
        }
    }

    // 교차 프로세스 잠금: 자식 프로세스 둘이 같은 등록부에 동시에 등록해도 항목을 잃지 않는다(JSON은 잠금 파일, Windows 레지스트리는 사용자 SID
    // 전역 mutex). 같은 프로세스 안 시험(Concurrency)과 달리 운영체제 잠금이 프로세스 사이에서 배제하는지 본다.
    private static async Task CrossProcessAsync(string file)
    {
        const int perWriter = 12;
        string writer = typeof(GameProjectTests).Assembly.Location;
        async Task Run(string kind, string target)
        {
            ProcessResult[] results = await Task.WhenAll(new[] { "A", "B" }.Select(prefix => ProcessRunner.RunAsync(new("dotnet",
                [writer, "--registry-writer", kind, target, prefix, perWriter.ToString(System.Globalization.CultureInfo.InvariantCulture)],
                Path.GetDirectoryName(writer)!, Timeout: TimeSpan.FromMinutes(5)))));
            Require(results.All(result => result.ExitCode == 0), kind + " registry writers failed: " + string.Join("\n", results.Select(result => result.StdOut + result.StdErr)));
            ISdkRegistry registry = kind == "json" ? new JsonSdkRegistry(target) : OperatingSystem.IsWindows() ? new WindowsSdkRegistry(target) : throw new PlatformNotSupportedException();
            Require(registry.List().Count == 2 * perWriter, $"{kind}: concurrent registrations from two processes lost entries: {registry.List().Count}");
        }

        await Run("json", file);
        if (OperatingSystem.IsWindows())
        {
            string subKey = @"Software\Rocks\Tests\" + Guid.NewGuid().ToString("B");
            try
            {
                await Run("windows", subKey);
            }
            finally
            {
                Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(subKey, throwOnMissingSubKey: false);
            }
        }
    }

    /// <summary>교차 프로세스 시험의 자식 본문이다: 접두가 붙은 SDK 경로 count개를 등록한다.</summary>
    internal static void WriteRegistry(string kind, string target, string prefix, int count)
    {
        ISdkRegistry registry = kind == "json" ? new JsonSdkRegistry(target)
            : OperatingSystem.IsWindows() ? new WindowsSdkRegistry(target) : throw new PlatformNotSupportedException(kind);
        for (int index = 0; index < count; ++index)
        {
            registry.Register(Path.Combine(Path.GetTempPath(), "rocks-registry-writer", prefix + index.ToString(System.Globalization.CultureInfo.InvariantCulture)));
        }
    }

    // 등록·해제는 등록부 잠금 안에서 하므로 동시에 등록해도 항목을 잃지 않는다.
    private static void Concurrency(string file)
    {
        const int writers = 16;
        string[] paths = [.. Enumerable.Range(0, writers).Select(index => Path.Combine(Path.GetDirectoryName(file)!, "Sdk" + index))];
        Parallel.For(0, writers, new ParallelOptions { MaxDegreeOfParallelism = writers }, index => new JsonSdkRegistry(file).Register(paths[index]));
        IReadOnlyList<SdkRegistration> entries = new JsonSdkRegistry(file).List();
        Require(entries.Count == writers && paths.All(path => entries.Any(entry => Same(entry.Path, path))), "concurrent registrations lost entries: " + entries.Count);
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

        Require(GameProjectLayout.RuleFiles(project, output, sdk).Select(file => Path.GetRelativePath(project, file).Replace('\\', '/'))
            .SequenceEqual(["Game.Build.cs", "Game.Target.cs", "Source/Tools/Extra.Build.cs"]), "rule discovery must skip excluded directories: " +
            string.Join(", ", GameProjectLayout.RuleFiles(project, output, sdk)));

        // 명시 입력과 그 아래는 제외 규칙과 무관하다.
        Require(GameInputSnapshot.Capture(project, output, sdk, [Path.Combine(project, "Saved/Crashes/crash.dmp")]) != baseline,
            "an explicit input under an excluded directory must be captured");
        foreach ((string input, string changed) in new[] { ("Saved", "Saved/Crashes/crash.dmp"), ("Source", "Source/.cache/index"), (".vs", ".vs/Copy.Build.cs") })
        {
            string explicitBaseline = GameInputSnapshot.Capture(project, output, sdk, [Path.Combine(project, input)]);
            string path = Path.Combine(project, changed);
            string text = File.ReadAllText(path);
            File.WriteAllText(path, text + " changed");
            Require(GameInputSnapshot.Capture(project, output, sdk, [Path.Combine(project, input)]) != explicitBaseline,
                $"{changed} under the explicit input {input} must be captured");
            File.WriteAllText(path, text);
        }

        // 프로젝트 안에 둔 SDK는 규칙 탐색에서 빠진다.
        string innerSdk = Path.Combine(project, "Sdk");
        Write(innerSdk, "Copy.Build.cs", "// SDK files are not game rules");
        Require(!GameProjectLayout.RuleFiles(project, output, innerSdk).Any(file => file.StartsWith(innerSdk, StringComparison.OrdinalIgnoreCase)),
            "rule discovery must skip an SDK inside the project");
        Directory.Delete(innerSdk, recursive: true);

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
        // out·Saved·.slnx는 호스트와 무관하게 대소문자를 가리지 않는다(macOS 기본 볼륨도 가리지 않는다).
        Require(GameProjectLayout.IsExcluded(project, Path.Combine(project, "out")) && GameProjectLayout.IsExcluded(project, Path.Combine(project, "SAVED", "x")) &&
            GameProjectLayout.IsExcluded(project, Path.Combine(project, "OUT", "x")) && GameProjectLayout.IsExcluded(project, Path.Combine(project, "Game.SLNX")) &&
            !GameProjectLayout.IsExcluded(project, project) && !GameProjectLayout.IsExcluded(project, Path.Combine(root, ".outside")), "exclusion edge cases");

        // 규칙 탐색은 링크를 따라가지 않고 거부한다(입력 snapshot과 같다).
        string target = Path.Combine(root, "LinkTarget");
        Write(target, "Linked.Build.cs", "// behind a link");
        string link = Path.Combine(project, "Source", "Linked");
        if (OperatingSystem.IsWindows())
        {
            using var junction = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", ["/d", "/c", "mklink", "/J", link, target])
            {
                UseShellExecute = false,
                RedirectStandardOutput = true
            })!;
            junction.StandardOutput.ReadToEnd();
            junction.WaitForExit();
            Require(junction.ExitCode == 0, "mklink /J failed");
        }
        else
        {
            Directory.CreateSymbolicLink(link, target);
        }

        try
        {
            GameProjectLayout.RuleFiles(project, output, sdk);
            throw new InvalidOperationException("rule discovery followed a link");
        }
        catch (IOException error) when (error.Message.Contains("cannot follow links", StringComparison.Ordinal))
        {
        }
        finally
        {
            Directory.Delete(link);
        }
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

        // 네이티브 빌드 전에 실패한 출력(호출 잠금만 있다)도 Rocks 출력이라 비운다 — 실패한 빌드 뒤 Rebuild가 막히지 않는다.
        string failed = GameProjectLayout.DefaultOutput(project, "Game", NativeConfiguration.Development);
        Write(failed, ".rocks-invocation.lock", "");
        Write(failed, "game-build-evidence.json", "{}");
        Require(await CleanCommand.RunAsync(Options(("configuration", "Development")), CancellationToken.None) == 0 &&
            Directory.EnumerateFileSystemEntries(failed).Select(Path.GetFileName).SequenceEqual([".rocks-invocation.lock"]),
            "an output with only the invocation lock is a Rocks output");

        await Expect(() => CleanCommand.RunAsync(Options(("output", Path.Combine(project, "out"))), CancellationToken.None), "only build outputs under");
        await Expect(() => CleanCommand.RunAsync(Options(("output", Path.Combine(project, "Source"))), CancellationToken.None), "only build outputs under");
        await Expect(() => CleanCommand.RunAsync(Options(("output", Path.Combine(root, "out", "x"))), CancellationToken.None), "only build outputs under");
        await Expect(() => CleanCommand.RunAsync(Options(("root", root)), CancellationToken.None), "belong to engine targets");
        await Expect(() => CleanCommand.RunAsync(Options(("platform", "android")), CancellationToken.None), "belong to engine targets");
    }

    // 다중 구성 SDK 조립(Docs/Design/ProjectFiles.ko.md 10.3절, V10의 내보내기 오류): Libraries/·Metadata/는 구성 디렉터리로 가고, 한 벌 파일은
    // 구성마다 같아야 하며 한 구성에만 있어도 오류다. 실패하면 대상이 생기지 않고 stage도 남지 않는다.
    private static void Assembly(string root)
    {
        Directory.CreateDirectory(root);
        string Stage(string name, params (string Path, string Text)[] files)
        {
            string directory = Path.Combine(root, "stage-" + name);
            foreach ((string path, string text) in files)
            {
                Write(directory, path, text);
            }

            return directory;
        }

        string destination = Path.Combine(root, "Sdk");
        using (var assembly = new SdkAssembly(destination))
        {
            string[] debug = assembly.Merge(NativeConfiguration.Debug, Stage("debug", ("Headers/M/0/M.h", "header"), ("Libraries/M.lib", "debug"),
                ("Metadata/M/M.rocksmeta.json", "meta"), ("Tools/Tool.dll", "tool")));
            Require(debug.SequenceEqual(["Configurations/Debug/Libraries/M.lib", "Configurations/Debug/Metadata/M/M.rocksmeta.json"]),
                "per-configuration files go under Configurations/Debug: " + string.Join(", ", debug));
            assembly.Merge(NativeConfiguration.Release, Stage("release", ("Headers/M/0/M.h", "header"), ("Libraries/M.lib", "release"),
                ("Metadata/M/M.rocksmeta.json", "meta"), ("Tools/Tool.dll", "tool")));
            assembly.RequireSameSharedFiles();
            try
            {
                assembly.Merge(NativeConfiguration.Development, Stage("development", ("Headers/M/0/M.h", "other header")));
                throw new InvalidOperationException("a configuration-independent file that differs must fail the export");
            }
            catch (InvalidOperationException error) when (error.Message.Contains("SDK file Headers/M/0/M.h differs between the Debug and Development", StringComparison.Ordinal))
            {
            }
        }

        using (var assembly = new SdkAssembly(destination))
        {
            assembly.Merge(NativeConfiguration.Debug, Stage("debug-extra", ("Headers/M/0/M.h", "header"), ("Headers/M/0/Extra.h", "extra")));
            assembly.Merge(NativeConfiguration.Release, Stage("release-extra", ("Headers/M/0/M.h", "header")));
            try
            {
                assembly.RequireSameSharedFiles();
                throw new InvalidOperationException("a configuration-independent file in one configuration only must fail the export");
            }
            catch (InvalidOperationException error) when (error.Message.Contains("Headers/M/0/Extra.h is in the Debug configuration but not in Release", StringComparison.Ordinal))
            {
            }

            try
            {
                assembly.Publish();
                throw new InvalidOperationException("publishing an assembly without a built configuration must fail");
            }
            catch (InvalidOperationException error) when (error.Message.Contains("no configuration to publish", StringComparison.Ordinal))
            {
            }
        }

        Require(!Directory.Exists(destination) && !Directory.EnumerateDirectories(root, "Sdk.staging-*").Any(), "a failed assembly must leave no SDK or stage");
        Directory.CreateDirectory(destination);
        try
        {
            using var existing = new SdkAssembly(destination);
            throw new InvalidOperationException("an existing SDK destination must be rejected");
        }
        catch (IOException error) when (error.Message.Contains("already exists", StringComparison.Ordinal))
        {
        }

        // 구성 목록: 쉼표·세미콜론, 구성 순서로 정렬, 이름은 정확히, 중복·빈 칸 거부.
        Require(SdkExportCommand.Parse("Release;Debug").SequenceEqual([NativeConfiguration.Debug, NativeConfiguration.Release]) &&
            SdkExportCommand.Parse("Debug, Development").SequenceEqual([NativeConfiguration.Debug, NativeConfiguration.Development]), "configuration list parsing");
        foreach (string bad in new[] { "debug", "Debug,Debug", "", "Debug,", "Fast" })
        {
            Error(() => SdkExportCommand.Parse(bad));
        }
    }

    private sealed class MemoryRegistry : ISdkRegistry
    {
        private readonly List<SdkRegistration> entries = [];

        public IReadOnlyList<SdkRegistration> List(ICollection<string>? problems = null) => [.. entries.OrderBy(entry => entry.Id, StringComparer.Ordinal)];

        // 경로 중복 검사 없이 넣는다(다른 도구가 같은 경로를 두 ID로 등록한 등록부).
        public void Add(string id, string path) => entries.Add(new(id, Path.GetFullPath(path)));

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
