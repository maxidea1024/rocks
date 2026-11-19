using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

// self 시험 중 다른 엔진 경계 정책 검사다(Docs/Design/UnrealExchange.ko.md 11.1절 — foreign-engine-boundary). UE 플러그인은
// Extras/에, Rocks 모듈은 Rocks 규칙 경로에 있으면 통과하고, 서로의 서술자·빌드 네임스페이스가 상대 자리에 있으면 거부한다.
internal static partial class SelfTests
{
    private const string UnrealPluginDirectory = "Extras/UnrealExport/RocksExport/";

    private static void RegisterForeignEngineBoundary(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryForeignEngineBoundary", () => NoFailures(PolicyChecks.ForeignEngineBoundary(options.Root)));
        suite.Add("Policy.ForeignEngineBoundaryControls", () =>
        {
            using TemporaryDirectory temporary = new();

            // 정상 배치: UE 플러그인(서술자·UE 규칙)은 Extras/, Rocks 모듈은 Engine/Source. 주석·문자열에서 상대 엔진 이름을
            // 말하는 것과, 규칙 경로에서 빠지는 C# 도구 Programs/Rocks.*의 UE 이름은 통과한다.
            temporary.Write(UnrealPluginDirectory + "RocksExport.uplugin", "{ \"FileVersion\": 3 }\n");
            temporary.Write(UnrealPluginDirectory + "Source/RocksExport/RocksExport.Build.cs", string.Join('\n',
                "// Not a Rocks.Build module: built by the Unreal toolchain.",
                "using UnrealBuildTool;",
                "public class RocksExport : ModuleRules",
                "{",
                "    public RocksExport(ReadOnlyTargetRules Target) : base(Target) { string note = \"Rocks.Build\"; }",
                "}",
                ""));
            temporary.Write("Engine/Source/Runtime/Probe/Probe.Build.cs", string.Join('\n',
                "using Rocks.Build.Rules;",
                "// Mirrors UnrealBuildTool ModuleRules.",
                "public sealed class Probe : EngineModuleRules { }",
                ""));
            temporary.Write("Programs/Rocks.Fake/Fake.Target.cs", "using UnrealBuildTool;\n");
            temporary.Write(UnrealPluginDirectory + "README.md", "Copy into an Unreal project; see Rocks.Build docs.\n");
            NoFailures(PolicyChecks.ForeignEngineBoundary(temporary.Root));

            void ExpectOne(string label, string file, string content, int line)
            {
                temporary.Write(file, content);
                var failures = PolicyChecks.ForeignEngineBoundary(temporary.Root);
                Verify.That(failures.Count == 1 && failures[0].Rule == "foreign-engine-boundary" && failures[0].Line == line &&
                    failures[0].Path == file, $"foreign engine boundary defect escaped audit: {label}\n{string.Join('\n', failures)}");
                File.Delete(temporary.FilePath(file));
            }

            ExpectOne("Rocks plugin descriptor under Extras", "Extras/Probe/Probe.rocksplugin", "{}\n", 0);
            ExpectOne("Rocks project descriptor under Extras (upper case)", "Extras/Probe/Probe.ROCKSPROJECT", "{}\n", 0);
            ExpectOne("Rocks rules under Extras", UnrealPluginDirectory + "Source/Bad/Bad.Build.cs",
                "using Rocks.Build.Rules;\npublic sealed class Bad : EngineModuleRules { }\n", 1);
            ExpectOne("qualified Rocks rules name under Extras", "Extras/Probe/Probe.Target.cs",
                "// target\n\npublic sealed class ProbeTarget : Rocks . Build.Rules.TargetRules { }\n", 3);
            ExpectOne("Unreal plugin descriptor under Engine/Plugins", "Engine/Plugins/Foo/Foo.uplugin", "{}\n", 0);
            ExpectOne("Unreal project descriptor under Templates", "Templates/Game/Game.uproject", "{}\n", 0);
            ExpectOne("Unreal rules under Core/Source", "Core/Source/Runtime/Bad/Bad.Build.cs",
                "// rules\nusing UnrealBuildTool;\npublic class Bad : ModuleRules { }\n", 2);
            ExpectOne("Unreal target rules under Programs", "Programs/Sandbox/Sandbox.Target.cs",
                "public class SandboxTarget : UnrealBuildTool.TargetRules { }\n", 1);
        });
    }
}
