using System.Runtime.CompilerServices;
using System.Xml.Linq;
using Microsoft.Build.Evaluation;
using Microsoft.Build.Locator;
using Microsoft.VisualStudio.SolutionPersistence.Model;
using Microsoft.VisualStudio.SolutionPersistence.Serializer;
using Rocks.Build.Cli;

namespace Rocks.Build.IntegrationTests;

// Visual Studio 프로젝트 파일 쓰기(Docs/Design/ProjectFiles.ko.md 16절 V2–V5)를 Visual Studio 없이 검사한다. 손으로 만든 모델을 쓴다
// (도구 모음·계획과 무관하게 결정적이다). Windows 호스트에서만 돈다(Visual Studio 프로젝트는 Windows 전용이다).
// - V2: Microsoft.Build로 .vcxproj를 구성마다 평가해(Visual Studio의 VC props가 없어도 되게 없는 import를 무시) Makefile 합성 규칙
//   (항목 메타데이터가 ItemDefinitionGroup 기본값을 바꾸고 IncludePath는 더한다)으로 얻은 파일별 값이 모델과 같다.
// - V3: .slnx를 같은 패키지로 다시 읽어 프로젝트 경로·구성 대응·C# 빌드 끔을 본다.
// - V4: 같은 입력 두 번 byte 같음, 두 루트에서 도구 모음 props 말고 같음, 두 번째 쓰기는 아무것도 쓰지 않음, 정리는 상태 목록의 파일만.
// - V5: golden(ProjectFiles/Golden/*.golden). ROCKS_UPDATE_GOLDEN=1이면 golden을 다시 쓴다.
internal static class ProjectFilesWriterTests
{
    private const string GoldenUpdateVariable = "ROCKS_UPDATE_GOLDEN";

    public static async Task RunAsync(string repository)
    {
        if (!OperatingSystem.IsWindows())
        {
            Console.WriteLine("SKIP project files writer: Windows hosts only");
            return;
        }

        if (!MSBuildLocator.IsRegistered)
        {
            MSBuildLocator.RegisterDefaults();
        }

        string first = Directory.CreateTempSubdirectory("rocks-projectfiles-a-").FullName;
        string second = Directory.CreateTempSubdirectory("rocks-projectfiles-b-").FullName;
        try
        {
            ProjectFilesInput a = Input(first, "C:/LLVM-A");
            ProjectFilesInput b = Input(second, "D:/LLVM-B");
            SortedDictionary<string, byte[]> rendered = VisualStudioProjects.Render(a);
            Require(rendered.SequenceEqual(VisualStudioProjects.Render(a), new FileComparer()), "rendering is not deterministic");
            Golden(repository, a, rendered);
            RootIndependence(a, rendered, b);
            await WriteAndCleanAsync(a, rendered);
            Evaluate(a);
            await SolutionRoundTripAsync(a);
            Console.WriteLine("PASS project files writer: golden, determinism, root independence, write-if-changed and cleanup, MSBuild evaluation, solution round trip");
        }
        finally
        {
            Directory.Delete(first, recursive: true);
            Directory.Delete(second, recursive: true);
        }
    }

    // 손으로 만든 모델이다. 값에 MSBuild 특수 문자(%·$·'·@)와 괄호·따옴표가 든다(;는 Visual Studio도 목록 구분자로 쪼개므로 경로에 둘 수 없다).
    private static ProjectFilesInput Input(string root, string llvm)
    {
        string Path(string relative) => System.IO.Path.GetFullPath(System.IO.Path.Combine(root, relative));
        foreach (string file in new[] { "Engine/Lib/Lib.cpp", "Engine/Lib/Raw.c", "Engine/Lib/Unused.cpp", "Engine/Lib/Lib.h", "Engine/Lib/Pch.h",
            "Programs/Tool/Main.cpp" })
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path(file))!);
            File.WriteAllText(Path(file), "// fixture\n");
        }

        string projectFiles = Path("out/ProjectFiles");
        const string copy = "0123456789abcdef.h";
        IntelliSenseValues Cpp(string configuration) => new(
            [Path("Engine/Lib"), Path("out/dotnet-windows-engine-" + configuration.ToLowerInvariant() + "/Generated/Lib/Public"), Path("Odd %$'@ Dir")],
            ["ROCKS_BUILD_" + configuration.ToUpperInvariant() + "=1", "ROCKS_LIB_API=", "VISIBLE=__attribute__((visibility(\"default\")))", "PERCENT=100%"],
            [System.IO.Path.Combine(projectFiles, "Definitions", copy), Path("Engine/Lib/Pch.h")],
            ["/utf-8", "/std:c++23preview", configuration == "Debug" ? "/MDd" : "/MD", "/Yu\"" + Path("Engine/Lib/Pch.h") + "\""]);
        IntelliSenseValues C(string configuration) => new([Path("Engine/Lib")], ["ROCKS_BUILD_" + configuration.ToUpperInvariant() + "=1"], [],
            ["/std:c17", configuration == "Debug" ? "/MDd" : "/MD"]);
        Dictionary<string, IntelliSenseValues?> Values(Func<string, IntelliSenseValues> make, string target) =>
            new(StringComparer.Ordinal) { [target + "|Debug"] = make("Debug"), [target + "|Release"] = make("Release") };
        var none = new Dictionary<string, IdeChoice?>(StringComparer.Ordinal);
        IdeProjectConfiguration[] Configurations(string target) => [new(target, "Debug"), new(target, "Release")];
        var engine = new IdeProject(IntelliSenseModel.EngineProjectName, true, Configurations("Engine"),
            [new(Path("Engine/Lib/Lib.cpp"), 0, none), new(Path("Engine/Lib/Raw.c"), 1, none), new(Path("Engine/Lib/Unused.cpp"), null, none)],
            [Path("Engine/Lib/Lib.h"), Path("Engine/Lib/Pch.h")], [new(0, Values(Cpp, "Engine")), new(1, Values(C, "Engine"))], 0);
        var tool = new IdeProject("Tool", false, Configurations("Tool"), [new(Path("Programs/Tool/Main.cpp"), 0, none)], [],
            [new(0, Values(Cpp, "Tool"))], 0);
        var model = new IntelliSenseModel([engine, tool], new SortedDictionary<string, string>(StringComparer.Ordinal) { [copy] = "// Generated by Rocks.Build. Do not edit.\n#define ROCKS_LIB_API \n" }, []);
        var targets = new Dictionary<string, ProjectFilesTarget>(StringComparer.Ordinal)
        {
            ["Engine"] = new("Engine", "Game", new Dictionary<string, string?> { ["Debug"] = null, ["Release"] = null }),
            ["Tool"] = new("Tool", "Program", new Dictionary<string, string?>
            {
                ["Debug"] = Path("out/dotnet-windows-tool-debug/bin/Tool.exe"),
                ["Release"] = Path("out/dotnet-windows-tool-release/bin/Tool.exe")
            })
        };
        var toolchain = new ProjectFilesToolchain(llvm + "/bin/clang-cl.exe", llvm, "22", [llvm + "/msvc/include", llvm + "/sdk/ucrt"], llvm + "/asan");
        return new(root, projectFiles, Path("Rocks.slnx"), model, targets, ["Debug", "Release"], true, toolchain,
            ["Build/Rules/Rocks.Engine.Rules.csproj", "Programs/Rocks.Build/Rocks.Build.csproj"]);
    }

    private static void Golden(string repository, ProjectFilesInput input, SortedDictionary<string, byte[]> rendered)
    {
        string golden = Path.Combine(repository, "Programs", "Rocks.Build.IntegrationTests", "ProjectFiles", "Golden");
        bool update = Environment.GetEnvironmentVariable(GoldenUpdateVariable) == "1";
        foreach ((string path, byte[] content) in rendered.Where(pair => !IsMachineSpecific(pair.Key)))
        {
            string name = Path.GetRelativePath(input.Root, path).Replace('\\', '/').Replace("/", "__", StringComparison.Ordinal) + ".golden";
            string file = Path.Combine(golden, name);
            if (update)
            {
                Directory.CreateDirectory(golden);
                File.WriteAllBytes(file, content);
                continue;
            }

            Require(File.Exists(file) && File.ReadAllBytes(file).AsSpan().SequenceEqual(content),
                $"rendered {name} differs from its golden file (set {GoldenUpdateVariable}=1 to update after review)");
        }
    }

    private static void RootIndependence(ProjectFilesInput a, SortedDictionary<string, byte[]> rendered, ProjectFilesInput b)
    {
        SortedDictionary<string, byte[]> other = VisualStudioProjects.Render(b);
        string Key(string root, string path) => Path.GetRelativePath(root, path).Replace('\\', '/');
        var left = rendered.Where(pair => !IsMachineSpecific(pair.Key)).ToDictionary(pair => Key(a.Root, pair.Key), pair => pair.Value);
        var right = other.Where(pair => !IsMachineSpecific(pair.Key)).ToDictionary(pair => Key(b.Root, pair.Key), pair => pair.Value);
        Require(left.Count == right.Count && left.All(pair => right.TryGetValue(pair.Key, out byte[]? value) && value.AsSpan().SequenceEqual(pair.Value)),
            "rendered files depend on the working copy location (only the toolchain props may)");
    }

    private static async Task WriteAndCleanAsync(ProjectFilesInput input, SortedDictionary<string, byte[]> rendered)
    {
        var fingerprints = ProjectFilesOutput.Fingerprints(input, rendered);
        ProjectFilesWriteResult once = await ProjectFilesOutput.WriteAsync(input.Root, input.ProjectFilesDirectory, rendered, fingerprints, CancellationToken.None);
        Require(once.Written == rendered.Count && once.Removed == 0, "first write");
        var times = rendered.Keys.ToDictionary(path => path, File.GetLastWriteTimeUtc);
        ProjectFilesWriteResult twice = await ProjectFilesOutput.WriteAsync(input.Root, input.ProjectFilesDirectory, rendered, fingerprints, CancellationToken.None);
        Require(twice.Written == 0 && rendered.Keys.All(path => File.GetLastWriteTimeUtc(path) == times[path]), "second write rewrote unchanged files");

        // Visual Studio가 만든 파일은 상태 목록에 없으므로 남는다. 없어진 프로젝트의 파일만 지운다.
        string user = Path.Combine(input.ProjectFilesDirectory, "Programs", "Tool.vcxproj.user");
        string intermediate = Path.Combine(input.ProjectFilesDirectory, "Intermediate", "Tool", "Debug_x64", "vs.tlog");
        Directory.CreateDirectory(Path.GetDirectoryName(intermediate)!);
        File.WriteAllText(user, "<Project />\n");
        File.WriteAllText(intermediate, "log\n");
        ProjectFilesInput withoutTool = input with
        {
            Model = input.Model with { Projects = [.. input.Model.Projects.Where(project => project.Name != "Tool")] },
            Targets = input.Targets.Where(pair => pair.Key != "Tool").ToDictionary(pair => pair.Key, pair => pair.Value)
        };
        SortedDictionary<string, byte[]> smaller = VisualStudioProjects.Render(withoutTool);
        ProjectFilesWriteResult cleaned = await ProjectFilesOutput.WriteAsync(input.Root, input.ProjectFilesDirectory, smaller,
            ProjectFilesOutput.Fingerprints(withoutTool, smaller), CancellationToken.None);
        Require(cleaned.Removed == 2 && !File.Exists(Path.Combine(input.ProjectFilesDirectory, "Programs", "Tool.vcxproj")) && File.Exists(user) &&
            File.Exists(intermediate), "cleanup must remove only the files the previous run wrote");
        await ProjectFilesOutput.WriteAsync(input.Root, input.ProjectFilesDirectory, rendered, fingerprints, CancellationToken.None);
    }

    // V2: 평가한 값이 모델 값과 같다. Microsoft.Build 형식은 이 메서드 안에서만 쓴다(MSBuildLocator 등록 뒤에 올라가야 한다).
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Evaluate(ProjectFilesInput input)
    {
        using var collection = new ProjectCollection();
        foreach (IdeProject project in input.Model.Projects)
        {
            string path = VisualStudioProjects.ProjectPath(input, project);
            foreach (IdeProjectConfiguration configuration in project.Configurations)
            {
                foreach (string name in new[] { configuration.Configuration, "Android_arm64_" + configuration.Configuration })
                {
                    var evaluated = new Project(path, new Dictionary<string, string> { ["Configuration"] = name, ["Platform"] = "x64" }, null, collection,
                        ProjectLoadSettings.IgnoreMissingImports | ProjectLoadSettings.IgnoreInvalidImports);
                    string[] system = [.. Split(evaluated.GetPropertyValue("IncludePath")).Select(Path.GetFullPath)];
                    Require(system.SequenceEqual(input.Toolchain.SystemIncludes.Select(Path.GetFullPath)), $"{project.Name} {name}: system include path");
                    string? executable = input.Targets[configuration.Target].Executables[configuration.Configuration];
                    bool android = name.StartsWith("Android_", StringComparison.Ordinal);
                    Require(evaluated.GetPropertyValue("NMakeOutput") == (android || executable is null ? "" : executable), $"{project.Name} {name}: NMakeOutput");
                    Require(evaluated.GetPropertyValue("NMakeBuildCommandLine").Contains("-Platform " + (android ? "Android" : "Windows"), StringComparison.Ordinal) &&
                        evaluated.GetPropertyValue("NMakeBuildCommandLine").Contains("-Target " + configuration.Target, StringComparison.Ordinal),
                        $"{project.Name} {name}: NMake command line");
                    foreach (IdeSourceFile file in project.Sources)
                    {
                        ProjectItem? item = evaluated.Items.SingleOrDefault(value => Path.GetFullPath(value.EvaluatedInclude) == file.Path);
                        Require(item is not null && item.ItemType == (file.Bundle is null ? "None" : "ClCompile"), $"{project.Name} {name}: item {file.Path}");
                        if (file.Bundle is not int bundle)
                        {
                            continue;
                        }

                        IntelliSenseValues expected = project.Bundles[bundle].Values[configuration.Key]!;
                        // 합성: 항목 include에 IncludePath를 더한다. 강제 include·정의는 항목 값 그대로다(Microsoft.Cpp.DesignTime.targets).
                        string[] includes = [.. Split(item!.GetMetadataValue("AdditionalIncludeDirectories")), .. system];
                        Require(includes.SequenceEqual([.. expected.Includes.Select(Path.GetFullPath), .. system]), $"{project.Name} {name}: includes of {file.Path}");
                        Require(Split(item.GetMetadataValue("PreprocessorDefinitions")).SequenceEqual(expected.Defines), $"{project.Name} {name}: definitions of {file.Path}");
                        Require(Split(item.GetMetadataValue("ForcedIncludeFiles")).Select(Path.GetFullPath).SequenceEqual(expected.ForcedIncludes.Select(Path.GetFullPath)),
                            $"{project.Name} {name}: forced includes of {file.Path}");
                        Require(item.GetMetadataValue("AdditionalOptions") == string.Join(" ", expected.Options), $"{project.Name} {name}: options of {file.Path}");
                    }

                    Require(project.Headers.All(header => evaluated.GetItems("ClInclude").Any(value => Path.GetFullPath(value.EvaluatedInclude) == header)),
                        $"{project.Name} {name}: headers");
                    collection.UnloadProject(evaluated);
                }
            }
        }
    }

    // V3: 솔루션을 다시 읽어 프로젝트 경로·구성 대응·C# 빌드 끔을 본다.
    private static async Task SolutionRoundTripAsync(ProjectFilesInput input)
    {
        SolutionModel solution = await SolutionSerializers.SlnXml.OpenAsync(input.SolutionPath, CancellationToken.None);
        string directory = Path.GetDirectoryName(input.SolutionPath)!;
        foreach (SolutionProjectModel project in solution.SolutionProjects)
        {
            string path = Path.GetFullPath(project.FilePath, directory);
            IReadOnlyList<ConfigurationRule> rules = project.ProjectConfigurationRules ?? [];
            if (path.EndsWith(".csproj", StringComparison.Ordinal))
            {
                Require(rules.Any(rule => rule.Dimension == BuildDimension.Build && rule.ProjectValue == "false"), "C# project builds with the solution: " + path);
                continue;
            }

            Require(File.Exists(path), "solution project is missing: " + path);
            string[] configurations = [.. XDocument.Load(path).Descendants().Where(element => element.Name.LocalName == "ProjectConfiguration")
                .Select(element => element.Attribute("Include")!.Value)];
            foreach (string buildType in solution.BuildTypes)
            {
                foreach (string platform in solution.Platforms)
                {
                    if (rules.Any(rule => rule.Dimension == BuildDimension.Build && rule.SolutionBuildType == buildType && rule.SolutionPlatform == platform &&
                        rule.ProjectValue == "false"))
                    {
                        continue;
                    }

                    ConfigurationRule[] mapped = [.. rules.Where(rule => rule.Dimension == BuildDimension.BuildType && rule.SolutionBuildType == buildType &&
                        rule.SolutionPlatform == platform)];
                    string value = mapped.Length == 1 ? mapped[0].ProjectValue : buildType;
                    Require(configurations.Contains(value + "|x64"), $"{path}: {buildType}|{platform} maps to missing configuration {value}");
                }
            }
        }

        Require(solution.SolutionProjects.Count == input.Model.Projects.Count + input.CSharpProjects.Count, "solution project count");
    }

    private static bool IsMachineSpecific(string path) =>
        path.EndsWith(VisualStudioProjects.ToolchainProps, StringComparison.Ordinal) || path.EndsWith(VisualStudioProjects.ToolchainPathsProps, StringComparison.Ordinal);

    private static string[] Split(string value) => value.Split(';', StringSplitOptions.RemoveEmptyEntries);

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private sealed class FileComparer : IEqualityComparer<KeyValuePair<string, byte[]>>
    {
        public bool Equals(KeyValuePair<string, byte[]> left, KeyValuePair<string, byte[]> right) =>
            left.Key == right.Key && left.Value.AsSpan().SequenceEqual(right.Value);

        public int GetHashCode(KeyValuePair<string, byte[]> pair) => pair.Key.GetHashCode(StringComparison.Ordinal);
    }
}
