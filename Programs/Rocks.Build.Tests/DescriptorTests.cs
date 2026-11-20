using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

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
        Check(read.Name == "MyGame" && read.FileVersion == 1 && read.EngineVersion == new ProjectEngineVersion(0, 1) &&
              read.EngineVersion.ToString() == "0.1", "project header");
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

        // engineVersion 경계: 0과 65535는 받는다.
        string largest = Write(directory, "Largest/Largest.rocksproject", """{ "fileVersion": 1, "engineVersion": "65535.0" }""");
        Check(DescriptorReader.ReadProject(largest).EngineVersion == new ProjectEngineVersion(ushort.MaxValue, 0), "engineVersion 65535.0");

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
        {
            ExpectFailure(() => DescriptorReader.ReadProject(Write(directory, $"Bad{index}/Bad{index}.rocksproject",
                badProjects[index])), "bad project " + index);
        }

        // engineVersion은 앞자리 0 없는 10진 "주.부"이고 각 값은 0–65535다(패치·공백·끝 줄바꿈·부호·전각 숫자·빈 칸은 받지 않는다).
        // 배열 값은 JSON 문자열 안에 그대로 넣으므로 "\\n"은 JSON escape 줄바꿈이다.
        string[] badEngineVersions = ["01.1", "0.01", "00.1", "65536.0", "0.65536", "0.1.0", "0", " 0.1", "0.1 ", "0.1\\n", "+0.1", "-0.1",
            "0.", ".1", "0x1.0", "1,0", "０.１", "1e1.0", ""];
        for (int index = 0; index < badEngineVersions.Length; ++index)
        {
            string text = badEngineVersions[index];
            ExpectFailure(() => DescriptorReader.ReadProject(Write(directory, $"BadEngine{index}/BadEngine{index}.rocksproject",
                $"{{ \"fileVersion\": 1, \"engineVersion\": \"{text}\" }}")), "bad engineVersion '" + text + "'",
                text.Length == 0 ? "'engineVersion' is required" : "must be '<major>.<minor>' in decimal without leading zeros");
        }

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
        var enabled = ResolveProject(Project("Game1",
            "{ \"name\": \"Ocean\", \"enabled\": true }, { \"name\": \"DebugDraw\", \"enabled\": false }"), all);
        string[] names = enabled.Select(plugin => plugin.Name).ToArray();
        Check(names.SequenceEqual(["CoreTools", "Physics", "Water", "Ocean"]), "resolve order: " + string.Join(",", names));

        // 프로젝트가 끈 플러그인을 켠 플러그인이 요구하면 오류다.
        ExpectFailure(() => ResolveProject(Project("Game2",
            "{ \"name\": \"Water\", \"enabled\": true }, { \"name\": \"Physics\", \"enabled\": false }"), all),
            "disabled dependency");

        // 없는 플러그인 참조(프로젝트·플러그인), 순환, 중복 이름은 오류다.
        ExpectFailure(() => ResolveProject(Project("Game3", "{ \"name\": \"Nope\", \"enabled\": true }"), all),
            "unknown project plugin");
        PluginDescriptor broken = Plugin("Broken", true, "Ghost");
        ExpectFailure(() => ResolveProject(Project("Game4", ""), [.. all, broken]), "unknown dependency");
        PluginDescriptor left = Plugin("Left", true, "Right");
        PluginDescriptor right = Plugin("Right", false, "Left");
        ExpectFailure(() => ResolveProject(Project("Game5", ""), [.. all, left, right]), "cycle");
        ExpectFailure(() => ResolveProject(Project("Game6", ""), [.. all, core]), "duplicate plugin");

        // 타깃 규칙 참조(엔진 저장소 타깃, Docs/Design/BuildTargets.ko.md 3.9절). Program은 enabledByDefault를 적용하지 않고 명시한 것과
        // 그 의존만 켠다(BD34). Game은 기본 활성도 켠다.
        string[] Names(IEnumerable<PluginDescriptor> plugins) => plugins.Select(plugin => plugin.Name).ToArray();
        PluginReferrer commandlet = PluginReferrer.OfTarget("Commandlet");
        Check(Names(PluginCatalog.ResolveEnabled([new("Water", true)], commandlet, all, applyDefaults: false))
            .SequenceEqual(["Physics", "Water"]), "a Program target enables only the referenced plugin and its dependencies");
        Check(PluginCatalog.ResolveEnabled([], commandlet, all, applyDefaults: false).Count == 0,
            "a Program target without references enables no plugin");
        Check(Names(PluginCatalog.ResolveEnabled([new("CoreTools", false)], commandlet, all, applyDefaults: true))
            .SequenceEqual(["DebugDraw"]), "a Game target enables default plugins unless it disables them");
        ExpectFailure(() => PluginCatalog.ResolveEnabled([new("Nope", true)], commandlet, all, applyDefaults: false),
            "unknown target plugin", "target 'Commandlet' references unknown plugin 'Nope'");
        ExpectFailure(() => PluginCatalog.ResolveEnabled([new("Water", true), new("Physics", false)], commandlet, all, applyDefaults: false),
            "target disabled a required plugin", "requires 'Physics', but the target 'Commandlet' disables it");
        ExpectFailure(() => PluginCatalog.ResolveEnabled([new("Water", true), new("Water", false)], commandlet, all, applyDefaults: false),
            "duplicate reference", "more than once");
        ExpectFailure(() => TargetPluginReferences.Validate("Commandlet", [new("Water", true), new("water", true)]),
            "target reference differing in case", "more than once");
        ExpectFailure(() => TargetPluginReferences.Validate("Commandlet", [new("Bad-Name", true)]), "target reference name",
            "is not a valid name");

        // 타깃 참조가 같은 이름의 프로젝트 참조를 이긴다(BD33). 순서는 타깃 참조, 그 뒤 남은 프로젝트 참조다.
        IReadOnlyList<PluginReference> combined = TargetPluginReferences.Combine([new("Water", false)],
            [new("Water", true), new("Ocean", true)]);
        Check(combined.SequenceEqual([new PluginReference("Water", false), new PluginReference("Ocean", true)]),
            "target references win over project references");

        IReadOnlyList<PluginDescriptor> ResolveProject(ProjectDescriptor project, IReadOnlyList<PluginDescriptor> available) =>
            PluginCatalog.ResolveEnabled(project.Plugins, PluginReferrer.Of(project), available, applyDefaults: true);
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
              "modules": [ { "name": "Water", "type": "Runtime", "loadingPhase": "PostConfigInit" } ] }
            """);
        Write(plugins, "Weather/Weather.rocksplugin", """
            { "fileVersion": 1, "version": 1, "versionName": "1.0", "friendlyName": "Weather",
              "modules": [ { "name": "Weather", "type": "Runtime" } ] }
            """);
        ProjectDescriptor project = DescriptorReader.ReadProject(projectFile);
        IReadOnlyList<PluginDescriptor> available = PluginCatalog.Discover([plugins]);

        NativeModuleInput Rule(string name, params string[] dependencies) => new() { Name = name, PublicDependencies = dependencies };
        NativeModuleInput[] rules =
        [
            Rule("Game", "Water", "ObjectRuntime"), Rule("GameTools", "Game"), Rule("GameEditor", "GameTools"),
            Rule("GameMobile", "Game"), Rule("Water"), Rule("Weather")
        ];
        string[] Names(IEnumerable<NativeModuleInput> modules) => modules.Select(module => module.Name).ToArray();

        // Windows Development: Developer는 들어가고 Editor·Android 전용·켜지지 않은 플러그인 모듈은 빠진다.
        var development = GameSet(project, available, NativePlatform.Windows, NativeConfiguration.Development);
        Check(Names(development.Apply(rules, ["Game", "GameTools"])).SequenceEqual(["Game", "GameTools", "Water"]),
            "Windows Development module selection");
        Check(development.EnabledPlugins.Select(plugin => plugin.Name).SequenceEqual(["Water"]), "enabled plugins");
        // 기술자의 loadingPhase가 모듈 표 행의 단계로 넘어간다(Docs/Design/ModuleLoader.ko.md 3.2절). 적지 않으면 Default다.
        Check(development.Apply(rules, ["Game"]).Select(module => module.LoadingPhase)
            .SequenceEqual([LoadingPhase.Default, LoadingPhase.Default, LoadingPhase.PostConfigInit]), "descriptor loading phases");
        // 켠 플러그인의 모듈은 타깃이 참조하지 않아도 빌드 대상에 더한다. 프로젝트 모듈은 더하지 않는다.
        Check(development.IncludedPluginModules.SequenceEqual(["Water"]), "included plugin modules");
        Check(development.Modules.Single(module => module.Name == "Weather").ExcludedReason!.Contains("not enabled", StringComparison.Ordinal),
            "disabled plugin reason");

        // Release는 Developer 모듈을 뺀다. 타깃이 쓰면 이유와 함께 거부한다.
        var release = GameSet(project, available, NativePlatform.Windows, NativeConfiguration.Release);
        Check(Names(release.Apply(rules, ["Game"])).SequenceEqual(["Game", "Water"]), "Release excludes Developer modules");
        ExpectFailure(() => release.Apply(rules, ["GameTools"]), "Release target used a Developer module",
            "Developer modules are not built in Release");

        // Android는 Android 전용 모듈을 넣는다.
        var android = GameSet(project, available, NativePlatform.Android, NativeConfiguration.Debug);
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
        // 실행 중 적재 관계(DynamicModules — Docs/Design/ModuleLoader.ko.md 3.8절)도 같은 type 검사를 받는다(Monolithic이 링크한다):
        // Runtime 모듈이 Editor 모듈을, Release에서 Developer 모듈을 적재 목록에 두면 거부한다. 들어가는 모듈을 두면 그대로 통과한다.
        NativeModuleInput[] WithGameDynamic(string dynamic) =>
            [.. rules.Where(rule => rule.Name != "Game"), Rule("Game", "Water", "ObjectRuntime") with { DynamicModules = [dynamic] }];
        ExpectFailure(() => development.Apply(WithGameDynamic("GameEditor"), ["Game"]), "a run-time loaded Editor module",
            "module 'Game' uses module 'GameEditor'");
        ExpectFailure(() => release.Apply(WithGameDynamic("GameTools"), ["Game"]), "a run-time loaded Developer module in Release",
            "Developer modules are not built in Release");
        Check(Names(development.Apply(WithGameDynamic("GameTools"), ["Game"])).Contains("GameTools"),
            "a run-time loaded module that the target may include is accepted");

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
        ExpectFailure(() => GameSet(DescriptorReader.ReadProject(windowsOnly), [], NativePlatform.MacOS,
            NativeConfiguration.Debug), "platform outside targetPlatforms", "does not target MacOS");
        Write(plugins, "Ocean/Ocean.rocksplugin", """
            { "fileVersion": 1, "version": 1, "versionName": "1.0", "friendlyName": "Ocean",
              "modules": [ { "name": "Water", "type": "Runtime" } ] }
            """);
        ExpectFailure(() => GameSet(project, PluginCatalog.Discover([plugins]), NativePlatform.Windows,
            NativeConfiguration.Debug), "module declared by two descriptors", "is also declared by");
        File.Delete(Path.Combine(plugins, "Ocean", "Ocean.rocksplugin"));

        // Program 타깃(명령행 도구·시험)은 Release에서도 Developer 모듈을 넣는다(BD35 — UE DeveloperTool). Game Release만 뺀다.
        var programRelease = DescriptorModuleSet.Resolve(DescriptorOwner.ForProject(project, "Tool", []), available, NativePlatform.Windows,
            NativeConfiguration.Release, TargetType.Program);
        Check(Names(programRelease.Apply(rules, ["GameTools"])).SequenceEqual(["Game", "GameTools", "Water"]),
            "a Program target keeps Developer modules in Release");

        // 타깃 규칙의 참조가 프로젝트 참조를 이긴다: 프로젝트가 켠 Water를 타깃이 끄면 Water 모듈이 빠진다.
        var targetDisables = DescriptorModuleSet.Resolve(DescriptorOwner.ForProject(project, "Game", [new("Water", false)]), available,
            NativePlatform.Windows, NativeConfiguration.Development, TargetType.Game);
        Check(targetDisables.EnabledPlugins.Count == 0, "a target reference overrides the project reference");

        // 엔진 저장소 타깃(프로젝트 없음): 기술자가 없는 엔진 모듈은 대조 없이 통과하고, 켠 플러그인 모듈만 더해진다. 플랫폼 검사는 없다.
        NativeModuleInput[] engineRules = [Rule("Core"), Rule("Engine", "Core"), Rule("Water", "Engine"), Rule("Weather", "Engine")];
        var engine = DescriptorModuleSet.Resolve(DescriptorOwner.ForEngine("EngineTests", [new("Water", true)]), available,
            NativePlatform.Android, NativeConfiguration.Release, TargetType.Program);
        Check(Names(engine.Apply(engineRules, ["Engine"])).SequenceEqual(["Core", "Engine", "Water"]),
            "an engine target keeps engine modules and adds enabled plugin modules");
        Check(engine.IncludedPluginModules.SequenceEqual(["Water"]), "engine target plugin modules");
        ExpectFailure(() => engine.Apply([.. engineRules, Rule("Tool", "Weather")], ["Tool"]),
            "an engine module used a disabled plugin module", "plugin 'Weather' is not enabled");
        var engineNone = DescriptorModuleSet.Resolve(DescriptorOwner.ForEngine("Foundation", []), available, NativePlatform.Windows,
            NativeConfiguration.Debug, TargetType.Program);
        Check(engineNone.EnabledPlugins.Count == 0 && Names(engineNone.Apply(engineRules, ["Engine"])).SequenceEqual(["Core", "Engine"]),
            "an engine target without plugin references builds no plugin module");
    }

    // 설치 SDK 게임 빌드의 Game 타깃(타깃 참조 없음)이다.
    private static DescriptorModuleSet GameSet(ProjectDescriptor project, IReadOnlyList<PluginDescriptor> available, NativePlatform platform,
        NativeConfiguration configuration) =>
        DescriptorModuleSet.Resolve(DescriptorOwner.ForProject(project, "Game", []), available, platform, configuration, TargetType.Game);

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
            {
                throw new InvalidOperationException($"descriptor error for '{what}' had an unexpected message: {error.Message}");
            }

            return;
        }

        throw new InvalidOperationException("descriptor error was accepted: " + what);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
