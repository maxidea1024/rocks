using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

// self 시험 중 엔진 플러그인 정책 검사다(Docs/Design/BuildTargets.ko.md 3.9절 BD37·BD42): 플러그인 모듈 규칙의 기반 클래스·위치, 플러그인
// 시험 폴더가 시험 소스로 분류되는지(CRT 실수 변환 검사로 확인).
internal static partial class SelfTests
{
    private const string PluginDirectory = "Engine/Plugins/Importers/Probe/";

    private static void RegisterPluginPolicy(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryPluginModuleRules", () => NoFailures(PolicyChecks.PluginModuleRules(options.Root)));
        suite.Add("Policy.PluginModuleRulesControls", () =>
        {
            using TemporaryDirectory temporary = new();
            const string rules = PluginDirectory + "Source/Probe/Probe.Build.cs";
            temporary.Write(PluginDirectory + "Probe.rocksplugin", "{}\n");
            // 정상: 기술자 옆 Source/<모듈>/<모듈>.Build.cs, PluginModuleRules 상속. 주석·문자열 속 다른 기반 이름은 통과한다.
            temporary.Write(rules, string.Join('\n',
                "using Rocks.Build.Rules;",
                "// Not 'class Probe : EngineModuleRules'.",
                "public sealed class Probe : PluginModuleRules",
                "{",
                "    protected override string ModuleName => \"Probe\";",
                "}",
                ""));
            NoFailures(PolicyChecks.PluginModuleRules(temporary.Root));

            void ExpectOne(string label, string file, string content)
            {
                string? original = File.Exists(temporary.FilePath(file)) ? File.ReadAllText(temporary.FilePath(file)) : null;
                temporary.Write(file, content);
                var failures = PolicyChecks.PluginModuleRules(temporary.Root);
                Verify.That(failures.Count == 1 && failures[0].Rule == "plugin-module-rules-base" && failures[0].Path == file,
                    $"plugin module rules defect escaped audit: {label}\n{string.Join('\n', failures)}");
                if (original is null)
                {
                    File.Delete(temporary.FilePath(file));
                }
                else
                {
                    temporary.Write(file, original);
                }
            }

            ExpectOne("engine rules base under a plugin", rules, "public sealed class Probe : EngineModuleRules { }\n");
            ExpectOne("plain ModuleRules under a plugin", rules, "public sealed class Probe : ModuleRules { }\n");
            ExpectOne("class name differs from the module", rules, "public sealed class Other : PluginModuleRules { }\n");
            ExpectOne("rules outside Source/<Module>", PluginDirectory + "Probe2.Build.cs", "public sealed class Probe2 : PluginModuleRules { }\n");
            ExpectOne("rules folder differs from the module", PluginDirectory + "Source/Wrong/Probe3.Build.cs",
                "public sealed class Probe3 : PluginModuleRules { }\n");
            ExpectOne("plugin without a descriptor", "Engine/Plugins/Loose/Source/Loose/Loose.Build.cs",
                "public sealed class Loose : PluginModuleRules { }\n");
        });
        suite.Add("Policy.PluginSourcesCrtNumberConversions", () =>
        {
            using TemporaryDirectory temporary = new();
            // 플러그인 구현은 crt-number-conversion 대상이고, 플러그인 모듈의 Tests/는 시험 소스라 대상이 아니다.
            temporary.Write(PluginDirectory + "Source/Probe/Private/Reader.cpp", "double a = strtod(text, &end);\n");
            temporary.Write(PluginDirectory + "Source/Probe/Tests/ReaderTests.cpp", "double a = strtod(text, &end);\n");
            var failures = PolicyChecks.CrtNumberConversions(temporary.Root);
            Verify.That(failures.Count == 1 && failures[0].Path == PluginDirectory + "Source/Probe/Private/Reader.cpp" && failures[0].Line == 1,
                "plugin CRT number conversion classification: " + string.Join("; ", failures));
        });
    }
}
