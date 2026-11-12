using Rocks.Build;
using Rocks.Build.Descriptors;

// .rocksproject·.rocksplugin 읽기·검증과 플러그인 활성화 해석을 시험한다.
internal static class DescriptorTests
{
    public static void Run(string root, string? repository)
    {
        string directory = Path.Combine(root, "descriptors");
        Directory.CreateDirectory(directory);

        ReadsValidDescriptors(directory);
        RejectsMalformedDescriptors(directory);
        ResolvesEnabledPlugins(directory);
        DiscoversPluginFolders(directory);
        SelectsModulesForBuild(directory);

        // 저장소의 게임 템플릿 기술자도 형식에 맞아야 한다.
        if (repository is not null)
        {
            ProjectDescriptor template = DescriptorReader.ReadProject(
                Path.Combine(repository, "Templates", "Game", "Foundation", "Game.rocksproject"));
            Check(template.Modules.Count == 1 && template.Modules[0].Name == "SampleGame",
                "template project must declare the SampleGame module");
        }
    }

    private static void ReadsValidDescriptors(string directory)
    {
        string project = Write(directory, "MyGame/MyGame.rocksproject", """
            {
              "fileVersion": 1,
              "engineVersion": "0.1",
              "description": "예제",
              "modules": [
                { "name": "MyGame", "type": "Runtime" },
                { "name": "MyGameEditor", "type": "Editor", "loadingPhase": "PostDefault", "platforms": ["Windows", "MacOS"] }
              ],
              "plugins": [ { "name": "Water", "enabled": true }, { "name": "Debug", "enabled": false } ],
              "targetPlatforms": ["Windows", "Android"]
            }
            """);
        ProjectDescriptor read = DescriptorReader.ReadProject(project);
        Check(read.Name == "MyGame" && read.FileVersion == 1 && read.EngineVersion == "0.1", "project header");
        Check(read.Modules.Count == 2 && read.Modules[0].LoadingPhase == LoadingPhase.Default &&
              read.Modules[0].Platforms is null, "default module fields");
        Check(read.Modules[1].Type == ModuleType.Editor && read.Modules[1].LoadingPhase == LoadingPhase.PostDefault &&
              read.Modules[1].SupportsPlatform(NativePlatform.MacOS) &&
              !read.Modules[1].SupportsPlatform(NativePlatform.Android), "module platforms");
        Check(read.Plugins.Count == 2 && !read.Plugins[1].Enabled, "project plugin references");
        Check(read.TargetPlatforms.SequenceEqual([NativePlatform.Windows, NativePlatform.Android]), "target platforms");
        Check(read.RootDirectory == Path.GetDirectoryName(Path.GetFullPath(project)), "project root directory");

        // 생략한 targetPlatforms는 모든 플랫폼이다.
        string minimal = Write(directory, "Minimal/Minimal.rocksproject", """{ "fileVersion": 1, "engineVersion": "1.2" }""");
        Check(DescriptorReader.ReadProject(minimal).TargetPlatforms.Count == Enum.GetValues<NativePlatform>().Length,
            "omitted target platforms must mean every platform");

        string plugin = Write(directory, "Plugins/Water/Water.rocksplugin", """
            {
              "fileVersion": 1, "version": 3, "versionName": "1.2", "friendlyName": "Water",
              "category": "Rendering", "enabledByDefault": true,
              "modules": [ { "name": "Water", "type": "Runtime" } ],
              "plugins": [ { "name": "Physics", "enabled": true } ]
            }
            """);
        PluginDescriptor water = DescriptorReader.ReadPlugin(plugin);
        Check(water.Name == "Water" && water.Version == 3 && water.EnabledByDefault && water.Plugins.Count == 1,
            "plugin fields");
        Check(water.RootDirectory.EndsWith("Water", StringComparison.Ordinal), "plugin root directory");
    }

    private static void RejectsMalformedDescriptors(string directory)
    {
        // 프로젝트 오류: 모르는 필드, 버전, 형식, 이름, 중복, JSON 문법.
        string[] badProjects =
        [
            """{ "fileVersion": 1, "engineVersion": "0.1", "Modules": [] }""",
            """{ "fileVersion": 2, "engineVersion": "0.1" }""",
            """{ "fileVersion": "1", "engineVersion": "0.1" }""",
            """{ "fileVersion": 1, "engineVersion": "latest" }""",
            """{ "fileVersion": 1 }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "modules": [ { "name": "9Bad", "type": "Runtime" } ] }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "modules": [ { "name": "A", "type": "runtime" } ] }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "modules": [ { "name": "A", "type": "Runtime" }, { "name": "a", "type": "Runtime" } ] }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "modules": [ { "name": "A", "type": "Runtime", "loadingPhase": "1" } ] }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "targetPlatforms": ["Linux"] }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "targetPlatforms": [] }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "targetPlatforms": ["Windows", "Windows"] }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "plugins": [ { "name": "X" } ] }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "plugins": [ { "name": "X", "enabled": true }, { "name": "X", "enabled": false } ] }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", }""",
            """{ "fileVersion": 1, "engineVersion": "0.1" /* 주석 */ }""",
            """{ "fileVersion": 1, "engineVersion": "0.1", "fileVersion": 1 }""",
            """[ ]""",
        ];
        for (int index = 0; index < badProjects.Length; ++index)
            ExpectFailure(() => DescriptorReader.ReadProject(Write(directory, $"Bad{index}/Bad{index}.rocksproject",
                badProjects[index])), "bad project " + index);

        ExpectFailure(() => DescriptorReader.ReadProject(Write(directory, "Bad-Name/Bad-Name.rocksproject",
            """{ "fileVersion": 1, "engineVersion": "0.1" }""")), "non-identifier project name");
        ExpectFailure(() => DescriptorReader.ReadProject(Path.Combine(directory, "Missing.rocksproject")), "missing file");
        ExpectFailure(() => DescriptorReader.ReadProject(Write(directory, "Wrong/Wrong.json",
            """{ "fileVersion": 1, "engineVersion": "0.1" }""")), "wrong extension");

        // 플러그인 오류: 폴더 이름 불일치, 필수 필드, 의존을 끄는 표기, version 0.
        ExpectFailure(() => DescriptorReader.ReadPlugin(Write(directory, "Plugins/Folder/Other.rocksplugin",
            """{ "fileVersion": 1, "version": 1, "versionName": "1", "friendlyName": "X" }""")), "plugin folder mismatch");
        ExpectFailure(() => DescriptorReader.ReadPlugin(Write(directory, "Plugins/NoName/NoName.rocksplugin",
            """{ "fileVersion": 1, "version": 1, "versionName": "1" }""")), "plugin without friendlyName");
        ExpectFailure(() => DescriptorReader.ReadPlugin(Write(directory, "Plugins/Off/Off.rocksplugin",
            """{ "fileVersion": 1, "version": 1, "versionName": "1", "friendlyName": "Off", "plugins": [ { "name": "A", "enabled": false } ] }""")),
            "disabled plugin dependency");
        ExpectFailure(() => DescriptorReader.ReadPlugin(Write(directory, "Plugins/Zero/Zero.rocksplugin",
            """{ "fileVersion": 1, "version": 0, "versionName": "0", "friendlyName": "Zero" }""")), "plugin version 0");
    }

    private static void ResolvesEnabledPlugins(string directory)
    {
        string root = Path.Combine(directory, "resolve");
        PluginDescriptor Plugin(string name, bool byDefault, params string[] dependencies)
        {
            string references = string.Join(", ", dependencies.Select(d => $"{{ \"name\": \"{d}\", \"enabled\": true }}"));
            string path = Write(root, $"{name}/{name}.rocksplugin",
                $"{{ \"fileVersion\": 1, \"version\": 1, \"versionName\": \"1\", \"friendlyName\": \"{name}\", " +
                $"\"enabledByDefault\": {(byDefault ? "true" : "false")}, \"plugins\": [ {references} ] }}");
            return DescriptorReader.ReadPlugin(path);
        }

        ProjectDescriptor Project(string name, string plugins)
        {
            string path = Write(root, $"Projects/{name}/{name}.rocksproject",
                $"{{ \"fileVersion\": 1, \"engineVersion\": \"0.1\", \"plugins\": [ {plugins} ] }}");
            return DescriptorReader.ReadProject(path);
        }

        PluginDescriptor core = Plugin("CoreTools", true);
        PluginDescriptor physics = Plugin("Physics", false);
        PluginDescriptor water = Plugin("Water", false, "Physics");
        PluginDescriptor ocean = Plugin("Ocean", false, "Water");
        PluginDescriptor debug = Plugin("DebugDraw", true);
        PluginDescriptor[] all = [core, physics, water, ocean, debug];

        // 기본 활성 + 명시 활성 + 의존 전이. 의존이 앞에 온다.
        var enabled = PluginCatalog.ResolveEnabled(Project("Game1",
            "{ \"name\": \"Ocean\", \"enabled\": true }, { \"name\": \"DebugDraw\", \"enabled\": false }"), all);
        string[] names = enabled.Select(plugin => plugin.Name).ToArray();
        Check(names.SequenceEqual(["CoreTools", "Physics", "Water", "Ocean"]), "resolve order: " + string.Join(",", names));

        // 프로젝트가 끈 플러그인을 켠 플러그인이 요구하면 오류다.
        ExpectFailure(() => PluginCatalog.ResolveEnabled(Project("Game2",
            "{ \"name\": \"Water\", \"enabled\": true }, { \"name\": \"Physics\", \"enabled\": false }"), all),
            "disabled dependency");

        // 없는 플러그인 참조(프로젝트·플러그인), 순환, 중복 이름은 오류다.
        ExpectFailure(() => PluginCatalog.ResolveEnabled(Project("Game3", "{ \"name\": \"Nope\", \"enabled\": true }"), all),
            "unknown project plugin");
        PluginDescriptor broken = Plugin("Broken", true, "Ghost");
        ExpectFailure(() => PluginCatalog.ResolveEnabled(Project("Game4", ""), [.. all, broken]), "unknown dependency");
        PluginDescriptor left = Plugin("Left", true, "Right");
        PluginDescriptor right = Plugin("Right", false, "Left");
        ExpectFailure(() => PluginCatalog.ResolveEnabled(Project("Game5", ""), [.. all, left, right]), "cycle");
        ExpectFailure(() => PluginCatalog.ResolveEnabled(Project("Game6", ""), [.. all, core]), "duplicate plugin");
    }

    private static void DiscoversPluginFolders(string directory)
    {
        string engine = Path.Combine(directory, "discover", "Engine", "Plugins");
        string project = Path.Combine(directory, "discover", "Game", "Plugins");
        string Descriptor(string name) =>
            $"{{ \"fileVersion\": 1, \"version\": 1, \"versionName\": \"1\", \"friendlyName\": \"{name}\" }}";

        Write(engine, "Rendering/Water/Water.rocksplugin", Descriptor("Water"));
        Write(engine, "Rendering/Water/Source/Water/Water.Build.cs", "// 플러그인 안의 다른 파일");
        Write(engine, "Audio/Mixer/Mixer.rocksplugin", Descriptor("Mixer"));
        Write(project, "Gameplay/Abilities/Abilities.rocksplugin", Descriptor("Abilities"));
        var found = PluginCatalog.Discover([engine, project, Path.Combine(directory, "discover", "Missing")]);
        Check(found.Select(plugin => plugin.Name).SequenceEqual(["Abilities", "Mixer", "Water"]), "discovered plugins");

        // 엔진과 프로젝트에 같은 이름의 플러그인이 있으면 오류다.
        Write(project, "Rendering/Water/Water.rocksplugin", Descriptor("Water"));
        ExpectFailure(() => PluginCatalog.Discover([engine, project]), "duplicate plugin across roots");
    }

    // 기술자로 빌드할 모듈을 고르고 규칙과 대조한다(DescriptorModuleSet).
    private static void SelectsModulesForBuild(string directory)
    {
        string root = Path.Combine(directory, "select");
        string projectFile = Write(root, "Game/Game.rocksproject", """
            {
              "fileVersion": 1, "engineVersion": "0.1",
              "modules": [
                { "name": "Game", "type": "Runtime" },
                { "name": "GameTools", "type": "Developer" },
                { "name": "GameEditor", "type": "Editor" },
                { "name": "GameMobile", "type": "Runtime", "platforms": ["Android"] }
              ],
              "plugins": [ { "name": "Water", "enabled": true } ],
              "targetPlatforms": ["Windows", "MacOS", "Android"]
            }
            """);
        string plugins = Path.Combine(root, "Game", "Plugins");
        Write(plugins, "Water/Water.rocksplugin", """
            { "fileVersion": 1, "version": 1, "versionName": "1.0", "friendlyName": "Water",
              "modules": [ { "name": "Water", "type": "Runtime" } ] }
            """);
        Write(plugins, "Weather/Weather.rocksplugin", """
            { "fileVersion": 1, "version": 1, "versionName": "1.0", "friendlyName": "Weather",
              "modules": [ { "name": "Weather", "type": "Runtime" } ] }
            """);
        ProjectDescriptor project = DescriptorReader.ReadProject(projectFile);
        IReadOnlyList<PluginDescriptor> available = PluginCatalog.Discover([plugins]);

        NativeModuleInput Rule(string name, params string[] dependencies) => new() { Name = name, Dependencies = dependencies };
        NativeModuleInput[] rules =
        [
            Rule("Game", "Water", "ObjectRuntime"), Rule("GameTools", "Game"), Rule("GameEditor", "GameTools"),
            Rule("GameMobile", "Game"), Rule("Water"), Rule("Weather")
        ];
        string[] Names(IEnumerable<NativeModuleInput> modules) => modules.Select(module => module.Name).ToArray();

        // Windows Development: Developer는 들어가고 Editor·Android 전용·켜지지 않은 플러그인 모듈은 빠진다.
        var development = DescriptorModuleSet.Resolve(project, available, NativePlatform.Windows, NativeConfiguration.Development);
        Check(Names(development.Apply(rules, ["Game", "GameTools"])).SequenceEqual(["Game", "GameTools", "Water"]),
            "Windows Development module selection");
        Check(development.EnabledPlugins.Select(plugin => plugin.Name).SequenceEqual(["Water"]), "enabled plugins");
        // 켠 플러그인의 모듈은 타깃이 참조하지 않아도 빌드 대상에 더한다. 프로젝트 모듈은 더하지 않는다.
        Check(development.IncludedPluginModules.SequenceEqual(["Water"]), "included plugin modules");
        Check(development.Modules.Single(module => module.Name == "Weather").ExcludedReason!.Contains("not enabled", StringComparison.Ordinal),
            "disabled plugin reason");

        // Release는 Developer 모듈을 뺀다. 타깃이 쓰면 이유와 함께 거부한다.
        var release = DescriptorModuleSet.Resolve(project, available, NativePlatform.Windows, NativeConfiguration.Release);
        Check(Names(release.Apply(rules, ["Game"])).SequenceEqual(["Game", "Water"]), "Release excludes Developer modules");
        ExpectFailure(() => release.Apply(rules, ["GameTools"]), "Release target used a Developer module",
            "Developer modules are not built in Release");

        // Android는 Android 전용 모듈을 넣는다.
        var android = DescriptorModuleSet.Resolve(project, available, NativePlatform.Android, NativeConfiguration.Debug);
        Check(Names(android.Apply(rules, ["GameMobile"])).SequenceEqual(["Game", "GameTools", "GameMobile", "Water"]),
            "Android includes platform-specific module");

        // 빠진 모듈 참조: 타깃(Editor·플랫폼 미지원·켜지지 않은 플러그인)과 남은 모듈의 의존.
        ExpectFailure(() => development.Apply(rules, ["GameEditor"]), "target used an Editor module", "Editor modules are not built");
        ExpectFailure(() => development.Apply(rules, ["GameMobile"]), "target used a module unsupported on Windows",
            "does not support Windows");
        ExpectFailure(() => development.Apply(rules, ["Weather"]), "target used a disabled plugin module", "plugin 'Weather' is not enabled");
        NativeModuleInput[] usesWeather = [.. rules.Where(rule => rule.Name != "Game"), Rule("Game", "Weather")];
        ExpectFailure(() => development.Apply(usesWeather, ["Game"]), "included module depended on a disabled plugin module",
            "module 'Game' uses module 'Weather'");

        // 규칙과 기술자 불일치: 선언 없는 규칙, 규칙 없는 선언, 대소문자 차이.
        ExpectFailure(() => development.Apply([.. rules, Rule("Stray")], ["Game"]), "rules without a descriptor",
            "has rules but no descriptor declares it");
        ExpectFailure(() => development.Apply(rules.Where(rule => rule.Name != "Weather").ToArray(), ["Game"]),
            "descriptor module without rules", "has no rules");
        ExpectFailure(() => development.Apply([.. rules.Where(rule => rule.Name != "Water"), Rule("water")], ["Game"]),
            "module name differs in case", "differs in case");

        // 프로젝트가 대상으로 하지 않는 플랫폼, 여러 기술자에 같은 모듈 이름.
        string windowsOnly = Write(root, "WindowsOnly/WindowsOnly.rocksproject",
            """{ "fileVersion": 1, "engineVersion": "0.1", "targetPlatforms": ["Windows"] }""");
        ExpectFailure(() => DescriptorModuleSet.Resolve(DescriptorReader.ReadProject(windowsOnly), [], NativePlatform.MacOS,
            NativeConfiguration.Debug), "platform outside targetPlatforms", "does not target MacOS");
        Write(plugins, "Ocean/Ocean.rocksplugin", """
            { "fileVersion": 1, "version": 1, "versionName": "1.0", "friendlyName": "Ocean",
              "modules": [ { "name": "Water", "type": "Runtime" } ] }
            """);
        ExpectFailure(() => DescriptorModuleSet.Resolve(project, PluginCatalog.Discover([plugins]), NativePlatform.Windows,
            NativeConfiguration.Debug), "module declared by two descriptors", "is also declared by");
    }

    private static string Write(string directory, string relative, string text)
    {
        string path = Path.Combine(directory, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
        return path;
    }

    // diagnostic을 주면 오류 메시지에 그 문구가 있어야 한다(다른 이유로 실패한 것을 통과로 보지 않는다).
    private static void ExpectFailure(Action action, string what, string? diagnostic = null)
    {
        try
        {
            action();
        }
        catch (DescriptorException error)
        {
            if (diagnostic != null && !error.Message.Contains(diagnostic, StringComparison.Ordinal))
                throw new InvalidOperationException($"descriptor error for '{what}' had an unexpected message: {error.Message}");

            return;
        }

        throw new InvalidOperationException("descriptor error was accepted: " + what);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }
}
