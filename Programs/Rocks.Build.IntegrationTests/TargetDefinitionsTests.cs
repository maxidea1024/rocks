using Rocks.Build.Cli;
using Rocks.Build.Rules;

namespace Rocks.Build.IntegrationTests;

// 타깃 정의 디렉터리 찾기(Docs/Design/ProjectFiles.ko.md 6.2절): 규칙 타입의 class 선언이 정확히 한 *.Target.cs에 있어야 하고,
// Programs/Rocks.* 아래는 보지 않는다.
internal static class TargetDefinitionsTests
{
    public static void Run()
    {
        string root = Directory.CreateTempSubdirectory("rocks-target-definitions-").FullName;
        try
        {
            void Write(string relative, string text)
            {
                string path = Path.Combine(root, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, text);
            }

            Write("Programs/Probe/Probe.Target.cs", "public sealed class ProbeDefinitionTarget : TargetRules { }\n");
            Write("Programs/Rocks.Tool/Ignored.Target.cs", "public sealed class ProbeDefinitionTarget : TargetRules { }\n");
            var found = TargetDefinitions.Find(root, [new ProbeDefinitionTarget()]);
            Require(found["Probe"] == (Path.Combine(root, "Programs", "Probe"), "Probe"), "definition directory and file name");

            Write("Engine/Source/Copy.Target.cs", "public sealed class ProbeDefinitionTarget : TargetRules { }\n");
            Expect("found 2", () => TargetDefinitions.Find(root, [new ProbeDefinitionTarget()]));
            File.Delete(Path.Combine(root, "Engine/Source/Copy.Target.cs"));
            File.Delete(Path.Combine(root, "Programs/Probe/Probe.Target.cs"));
            Expect("found 0", () => TargetDefinitions.Find(root, [new ProbeDefinitionTarget()]));
            Console.WriteLine("PASS target definitions: one declaring file, Programs/Rocks.* ignored, zero or two files rejected");
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private sealed class ProbeDefinitionTarget : TargetRules
    {
        public override string Name => "Probe";

        public override IReadOnlyList<string> Modules => ["Probe"];
    }

    private static void Expect(string fragment, Action action)
    {
        try
        {
            action();
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
