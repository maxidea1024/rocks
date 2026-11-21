using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

/// <summary>
/// 켠 엔진 플러그인 모듈의 링크와 모듈 표 행(<see cref="NativeModuleInput.PluginModules"/> — Docs/Design/BuildTargets.ko.md 3.9절 BD36)을
/// 실제 기술자 해석·컴파일·링크로 검사한다. 픽스처: 가짜 Core(모듈 표 제공)·Engine, 플러그인 Importer(켬, Developer, PostDefault)와
/// Viewer(끔), 주 실행 파일 Tool과 부 실행 파일 Probe(둘 다 플러그인에 의존하지 않는다). 확인하는 것:
/// <list type="bullet">
/// <item>Tool이 Importer를 링크하고 모듈 표에 기술자 단계의 보통 행(요청 시 적재 아님)으로 싣는다. 행은 링크 폐포 뒤, 실행 파일 앞이다.</item>
/// <item>Probe에는 Importer가 없고, 끈 Viewer 모듈은 빌드되지 않는다.</item>
/// <item>주 모듈이 라이브러리인 타깃은 플러그인 모듈을 빌드만 한다.</item>
/// <item>거부: 라이브러리의 PluginModules, 모르는 이름, 중복, Modular 타깃(ML4 전).</item>
/// </list>
/// </summary>
internal static class PluginModuleLinkTests
{
    // 표 행을 "이름|의존,의존|단계"(요청 시 적재 행이면 "|request"를 덧붙임)로 출력한다.
    private const string MainSource = """
        #include <Core/Module/ModuleEntry.h>
        #include <stdio.h>
        #if __has_include(<Importer/Importer.h>)
        #error "an executable received the include path of a plugin module it does not depend on"
        #endif

        ROCKS_IMPLEMENT_MODULE(Unused, MODULE_NAME)

        int main() {
            const rocks::ModuleTable table = rocks::GeneratedModuleTable();
            for (unsigned long long index = 0; index < table.count; ++index) {
                const rocks::ModuleTableRow& row = table.rows[index];
                printf("%s|", row.name);
                for (unsigned long long dependency = 0; dependency < row.dependencyCount; ++dependency) {
                    printf("%s%s", dependency == 0 ? "" : ",", row.dependencies[dependency]);
                }

                printf("|%d%s\n", static_cast<int>(row.phase), row.onRequest ? "|request" : "");
            }

            return 0;
        }

        """;

    private const string ImporterHeader = """
        #pragma once
        int ImporterValue() noexcept;

        """;

    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "plugin-modules")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content.Replace("\r\n", "\n", StringComparison.Ordinal));
            return path;
        }

        string coreInclude = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(
            Write("Core/include/Core/Module/ModuleEntry.h", ModuleTableTests.EntryHeader)))!)!;
        string Simple(string directory, string name) => Write($"{directory}/{name}.cpp",
            $"#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, {name})\n");
        const string importerDirectory = "Engine/Plugins/Tools/Importer/Source/Importer";
        const string viewerDirectory = "Engine/Plugins/Tools/Viewer/Source/Viewer";
        Write(importerDirectory + "/Public/Importer/Importer.h", ImporterHeader);
        string importerSource = Write(importerDirectory + "/Private/Importer.cpp",
            "#include <Core/Module/ModuleEntry.h>\n#include <Importer/Importer.h>\nROCKS_IMPLEMENT_MODULE(Unused, Importer)\n" +
            "int ImporterValue() noexcept {\n    return 1;\n}\n");
        Write("Engine/Plugins/Tools/Importer/Importer.rocksplugin", """
            { "fileVersion": 1, "version": 1, "versionName": "1", "friendlyName": "Importer",
              "modules": [ { "name": "Importer", "type": "Developer", "loadingPhase": "PostDefault" } ] }
            """);
        Write("Engine/Plugins/Tools/Viewer/Viewer.rocksplugin", """
            { "fileVersion": 1, "version": 1, "versionName": "1", "friendlyName": "Viewer",
              "modules": [ { "name": "Viewer", "type": "Runtime" } ] }
            """);
        string MainFor(string name) => Write($"Programs/{name}/main.cpp", MainSource.Replace("MODULE_NAME", name, StringComparison.Ordinal));

        NativeModuleInput[] rules =
        [
            new() { Name = "Core", Sources = [Simple("Core/src", "Core")], PublicIncludeDirectories = [coreInclude], ProvidesModuleTable = true,
                ModuleDirectory = "Core/Source/Runtime/Core" },
            new() { Name = "Engine", Sources = [Simple("Engine/src", "Engine")], PublicDependencies = ["Core"],
                ModuleDirectory = "Engine/Source/Runtime/Engine" },
            new()
            {
                Name = "Importer", Sources = [importerSource], PublicIncludeDirectories = [Path.Combine(fixture, importerDirectory, "Public")],
                PublicDependencies = ["Engine"], ModuleDirectory = importerDirectory
            },
            new() { Name = "Viewer", Sources = [Simple(viewerDirectory + "/Private", "Viewer")], PublicDependencies = ["Core"], ModuleDirectory = viewerDirectory },
            new() { Name = "Tool", Kind = NativeModuleKind.Executable, Sources = [MainFor("Tool")], PublicDependencies = ["Core"] },
            new() { Name = "Probe", Kind = NativeModuleKind.Executable, Sources = [MainFor("Probe")], PublicDependencies = ["Core"] }
        ];

        // 기술자 해석(엔진 저장소 타깃, Program Release — Developer 플러그인 모듈이 들어간다)과 주 실행 파일 표시.
        IReadOnlyList<PluginDescriptor> available = PluginCatalog.Discover([Path.Combine(fixture, "Engine", "Plugins")]);
        string[] targetModules = ["Tool", "Probe"];
        var set = DescriptorModuleSet.Resolve(DescriptorOwner.ForEngine("Tool", [new("Importer", true)], fixture), available,
            request.Toolchain.Platform, NativeConfiguration.Release, TargetType.Program);
        IReadOnlyList<NativeModuleInput> modules = set.LinkIntoPrimary(set.Apply(rules, targetModules), targetModules);
        Check(modules.Single(module => module.Name == "Tool").PluginModules.SequenceEqual(["Importer"]) &&
            modules.Single(module => module.Name == "Probe").PluginModules.Count == 0, "only the primary executable links plugin modules");
        Check(modules.All(module => module.Name != "Viewer"), "a disabled plugin module must not be built");
        Check(modules.Single(module => module.Name == "Importer").LoadingPhase == LoadingPhase.PostDefault, "descriptor loading phase");

        var byName = modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        List<string> linkOrder = LinkClosure.Of("Tool", byName, out HashSet<string> onRequest);
        Check(linkOrder.SequenceEqual(["Core", "Engine", "Importer"]) && onRequest.Count == 0,
            "plugin modules follow the link closure and are linked, not loaded on request: " + string.Join(", ", linkOrder));
        Check(LinkClosure.Reachable(["Tool"], byName).SetEquals(["Tool", "Core", "Engine", "Importer"]), "reachable modules follow PluginModules");

        string[] roots = [.. targetModules, .. set.IncludedPluginModules];
        NativeBuildRequest build = request with { OutputDirectory = Path.Combine(fixture, "out"), Modules = [.. modules], Targets = roots };
        var builder = new NativeBuildEngine();
        NativeBuildResult cold = await builder.BuildAsync(build);
        Check(cold.Succeeded, "plugin module fixture build failed: " + Failure(cold));
        Check(cold.Actions.Any(action => action.Module == "Importer" && action.Kind == "Archive") &&
            !cold.Actions.Any(action => action.Module == "Viewer"), "the enabled plugin module is built and the disabled one is not");
        using (var database = System.Text.Json.JsonDocument.Parse(File.ReadAllText(cold.CompilationDatabasePath)))
        {
            Check(!database.RootElement.EnumerateArray().Any(item =>
                    item.GetProperty("file").GetString()!.EndsWith("main.cpp", StringComparison.Ordinal) &&
                    item.GetProperty("arguments").EnumerateArray().Any(argument => argument.GetString()!.Contains("Importer", StringComparison.Ordinal))),
                "an executable must not receive the include path of a plugin module it only links");
        }

        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            int postDefault = (int)LoadingPhase.PostDefault;
            int none = (int)LoadingPhase.None;
            int core = (int)LoadingPhase.Default;
            ProcessResult tool = await ProcessRunner.RunAsync(new(cold.ModuleOutputs["Tool"], [], fixture, request.Toolchain.Environment));
            Check(tool.ExitCode == 0 && tool.StdOut.Replace("\r", "", StringComparison.Ordinal) ==
                $"Core||{core}\nEngine|Core|{core}\nImporter|Engine|{postDefault}\nTool|Core|{none}\n",
                "module table rows of the primary executable:\n" + tool.StdOut + tool.StdErr);
            ProcessResult probe = await ProcessRunner.RunAsync(new(cold.ModuleOutputs["Probe"], [], fixture, request.Toolchain.Environment));
            Check(probe.ExitCode == 0 && probe.StdOut.Replace("\r", "", StringComparison.Ordinal) == $"Core||{core}\nProbe|Core|{none}\n",
                "a secondary executable must not link plugin modules:\n" + probe.StdOut + probe.StdErr);
        }

        pass("enabled plugin modules link into the primary executable with descriptor phase rows; disabled plugin modules are not built");

        // 주 모듈이 라이브러리인 타깃(엔진 라이브러리 묶음)은 플러그인 모듈을 빌드 대상에만 더한다.
        string[] libraryTarget = ["Engine"];
        IReadOnlyList<NativeModuleInput> library = set.LinkIntoPrimary(set.Apply(rules, libraryTarget), libraryTarget);
        Check(library.All(module => module.PluginModules.Count == 0), "a library target links no plugin module");
        NativeBuildResult libraryBuild = await builder.BuildAsync(request with
        {
            OutputDirectory = Path.Combine(fixture, "out-library"),
            Modules = [.. library],
            Targets = [.. libraryTarget, .. set.IncludedPluginModules]
        });
        Check(libraryBuild.Succeeded && libraryBuild.ModuleOutputs.ContainsKey("Importer") && !libraryBuild.ModuleOutputs.ContainsKey("Tool"),
            "a library target builds the enabled plugin modules only: " + Failure(libraryBuild));
        pass("a target without a primary executable builds enabled plugin modules without linking them");

        // 거부.
        NativeModuleInput[] Replace(NativeModuleInput replacement) =>
            [.. modules.Where(module => module.Name != replacement.Name), replacement];
        await Rejects(builder, build with { Modules = Replace(byName["Engine"] with { PluginModules = ["Importer"] }) },
            "only an executable links plugin modules", "plugin modules on a library");
        await Rejects(builder, build with { Modules = Replace(byName["Tool"] with { PluginModules = ["Missing"] }) },
            "unknown module Missing", "an unknown plugin module");
        await Rejects(builder, build with { Modules = Replace(byName["Tool"] with { PluginModules = ["Importer", "Importer"] }) },
            "listed twice", "a plugin module listed twice");
        // Modular은 모듈 표 제공 모듈이 없는 작은 묶음으로 본다(표 제공 모듈의 Modular 거부보다 앞서 플러그인 거부가 드러나게).
        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            NativeModuleInput[] small =
            [
                new() { Name = "Lib", Sources = [Write("small/lib.cpp", "int LibValue() noexcept { return 1; }\n")] },
                new() { Name = "Exe", Kind = NativeModuleKind.Executable, Sources = [Write("small/main.cpp", "int main() { return 0; }\n")], PluginModules = ["Lib"] }
            ];
            await Rejects(builder, request with
            {
                OutputDirectory = Path.Combine(fixture, "out-modular"),
                Modules = small,
                Targets = ["Exe", "Lib"],
                LinkType = NativeLinkType.Modular,
                Configuration = NativeConfiguration.Debug,
                TargetName = "Exe"
            }, "Modular linking of a target that enables plugins", "a Modular target with plugin modules");
        }

        pass("plugin module links are validated (executables only, known names, once, Monolithic only)");
    }

    private static async Task Rejects(NativeBuildEngine builder, NativeBuildRequest request, string expected, string name)
    {
        try
        {
            await builder.BuildAsync(request with { OutputDirectory = request.OutputDirectory + "-rejected" });
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains(expected, StringComparison.Ordinal), $"{name}: unexpected message: {error.Message}");
            return;
        }

        throw new InvalidOperationException($"{name} was accepted");
    }

    private static string Failure(NativeBuildResult result) =>
        result.Failure + "\n" + string.Join("\n", result.Actions.Where(action => !action.Succeeded).Select(action => action.StdOut + action.StdErr));

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
