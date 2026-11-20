using System.Text.Json;
using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 실행 중 적재 모듈(<see cref="NativeModuleInput.DynamicModules"/> — Docs/Design/ModuleLoader.ko.md 3.8절, ML3)을 실제 컴파일·링크로
/// 검사한다. 픽스처: Engine이 Plugin을 DynamicModules로 두고 Plugin은 Engine에 링크 의존한다(UE처럼 동적 관계는 순환 검사의 참조 사슬이
/// 아니다). 확인하는 것:
/// <list type="bullet">
/// <item>Monolithic 실행 파일이 Plugin archive를 링크하고 모듈 표에 Plugin 행을 싣는다. 행은 실행 파일 행 앞이고 행의 의존은 링크 의존뿐이다.</item>
/// <item>Engine·Game 컴파일에 Plugin의 public include가 들어가지 않는다(Plugin 소스는 받는다).</item>
/// <item>타깃이 직접 닿지 않아도 Plugin을 빌드하고, 바꾸면 Plugin·실행 파일만 다시 돈다.</item>
/// <item>중첩 동적 모듈, 동적 모듈만 끌어오는 archive와 링크 순서, 링크 의존이기도 한 동적 모듈(행·archive 하나), 실행 파일 자신의 동적 모듈.</item>
/// <item>거부: Dependencies와 겹침, 자기 자신, 실행 파일, 모르는 이름, 대소문자 철자, 중복, 표에 행이 없는 모듈(header-only·서드파티), 링크 의존만의
/// 순환.</item>
/// </list>
/// </summary>
internal static class DynamicModuleTests
{
    private const string EngineSource = """
        #include <Core/Module/ModuleEntry.h>
        #if __has_include(<Plugin/Plugin.h>)
        #error "a module received the include path of a module it loads at run time"
        #endif
        ROCKS_IMPLEMENT_MODULE(Unused, Engine)

        """;

    private const string PluginHeader = """
        #pragma once
        int PluginValue() noexcept;

        """;

    private static string PluginSource(int value) => $$"""
        #include <Core/Module/ModuleEntry.h>
        #include <Plugin/Plugin.h>
        ROCKS_IMPLEMENT_MODULE(Unused, Plugin)
        int PluginValue() noexcept {
            return {{value}};
        }

        """;

    // 표 행을 "이름|의존,의존"으로 출력한다. 실행 파일은 Plugin 헤더를 보지 못하므로 Plugin 코드는 표의 진입 함수 참조로만 링크된다.
    private const string MainSource = """
        #include <Core/Module/ModuleEntry.h>
        #include <stdio.h>
        #include <string.h>
        #if __has_include(<Plugin/Plugin.h>)
        #error "an executable received the include path of a module it loads at run time"
        #endif

        ROCKS_IMPLEMENT_MODULE(Unused, Game)

        int main() {
            const rocks::ModuleTable table = rocks::GeneratedModuleTable();
            for (unsigned long long index = 0; index < table.count; ++index) {
                const rocks::ModuleTableRow& row = table.rows[index];
                if (strcmp(row.entry().name, row.name) != 0) {
                    return 3;
                }

                printf("%s|", row.name);
                for (unsigned long long dependency = 0; dependency < row.dependencyCount; ++dependency) {
                    printf("%s%s", dependency == 0 ? "" : ",", row.dependencies[dependency]);
                }

                printf("\n");
            }

            return 0;
        }

        """;

    public static async Task RunAsync(NativeBuildRequest request, string root)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "dynamic-modules")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content.Replace("\r\n", "\n", StringComparison.Ordinal));
            return path;
        }

        string coreInclude = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(
            Write("core/include/Core/Module/ModuleEntry.h", ModuleTableTests.EntryHeader)))!)!;
        string Simple(string name) => Write($"{name.ToLowerInvariant()}/{name}.cpp",
            $"#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, {name})\n");
        string coreSource = Simple("Core");
        string pluginInclude = Path.GetDirectoryName(Path.GetDirectoryName(Write("plugin/include/Plugin/Plugin.h", PluginHeader))!)!;
        string pluginSource = Write("plugin/src/plugin.cpp", PluginSource(1));
        string engineSource = Write("engine/engine.cpp", EngineSource);
        string mainSource = Write("game/main.cpp", MainSource);
        string headersInclude = Directory.CreateDirectory(Path.Combine(fixture, "headers", "include")).FullName;
        string thirdSource = Write("third/third.cpp", "int ThirdValue() noexcept { return 1; }\n");
        // Engine은 Plugin(링크 의존이 Engine으로 돌아가는 관계)과 Shared(실행 파일이 링크 의존으로도 쓴다)를 적재 목록에 둔다. Plugin은
        // PluginHelper(동적 모듈만 끌어오는 archive)에 링크하고 Nested를 다시 적재 목록에 둔다(중첩). 실행 파일은 Solo를 적재 목록에 둔다.
        NativeModuleInput[] modules =
        [
            new() { Name = "Core", Sources = [coreSource], PublicIncludeDirectories = [coreInclude], ProvidesModuleTable = true },
            new() { Name = "Engine", Sources = [engineSource], Dependencies = ["Core"], DynamicModules = ["Plugin", "Shared"] },
            new()
            {
                Name = "Plugin", Sources = [pluginSource], PublicIncludeDirectories = [pluginInclude], Dependencies = ["Engine", "PluginHelper"],
                DynamicModules = ["Nested"]
            },
            new() { Name = "PluginHelper", Sources = [Simple("PluginHelper")], Dependencies = ["Core"] },
            new() { Name = "Nested", Sources = [Simple("Nested")], Dependencies = ["Core"] },
            new() { Name = "Shared", Sources = [Simple("Shared")], Dependencies = ["Core"] },
            new() { Name = "Solo", Sources = [Simple("Solo")], Dependencies = ["Core"] },
            new() { Name = "Headers", PublicIncludeDirectories = [headersInclude], Dependencies = ["Core"] },
            new() { Name = "Third", Sources = [thirdSource], ThirdParty = true },
            new()
            {
                Name = "Game", Kind = NativeModuleKind.Executable, Sources = [mainSource], Dependencies = ["Engine", "Core", "Shared"],
                DynamicModules = ["Solo"]
            }
        ];
        NativeBuildRequest game = request with { OutputDirectory = Path.Combine(fixture, "out"), Modules = modules, Targets = ["Game"] };
        var builder = new NativeBuildEngine();

        // 링크 순서: 링크 폐포(Core·Engine·Shared) 다음 동적 모듈 쪽 — 실행 파일 자신의 Solo(index -1), Engine의 Plugin과 그 링크 의존
        // PluginHelper, 이미 링크된 Shared는 건너뜀, Plugin의 Nested. 링크로 닿지 않는 모듈은 요청 시 적재 행이다.
        var byName = modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        List<string> linkOrder = LinkClosure.Of("Game", byName, out HashSet<string> onRequest);
        Check(linkOrder.SequenceEqual(["Core", "Engine", "Shared", "Solo", "PluginHelper", "Plugin", "Nested"]),
            "link closure order: " + string.Join(", ", linkOrder));
        Check(onRequest.SetEquals(["Solo", "PluginHelper", "Plugin", "Nested"]), "on-request modules: " + string.Join(", ", onRequest));
        Check(LinkClosure.Reachable(["Game"], byName).SetEquals(["Game", "Engine", "Core", "Shared", "Solo", "PluginHelper", "Plugin", "Nested"]),
            "reachable modules follow DynamicModules");
        var unknownNames = new List<string>();
        LinkClosure.Reachable(["Game", "Missing"], byName, (name, from) => unknownNames.Add(name + "<" + (from ?? "root")));
        Check(unknownNames.SequenceEqual(["Missing<root"]), "reachable reports unknown names: " + string.Join(", ", unknownNames));

        // 실행 파일 링크 명령의 archive 순서: 의존하는 쪽이 앞이고 각 archive는 한 번이다(Shared가 링크 의존과 동적 관계로 두 번 닿아도).
        NativeAction linkAction = ActionGraph.Create(game).Actions.Single(action => action.Module == "Game" && action.Kind == "Link");
        string[] archives = linkAction.Arguments.Select(argument => Path.GetFileNameWithoutExtension(argument))
            .Where(name => byName.ContainsKey(name) && name != "Game").ToArray();
        Check(archives.SequenceEqual(["Nested", "Plugin", "PluginHelper", "Solo", "Shared", "Engine", "Core"]),
            "archive order on the link line: " + string.Join(", ", archives));

        NativeBuildResult cold = await builder.BuildAsync(game);
        Check(cold.Succeeded, "dynamic module fixture build failed: " + Failure(cold));
        foreach (string name in new[] { "Plugin", "PluginHelper", "Nested", "Solo" })
        {
            Check(cold.ModuleOutputs.ContainsKey(name) && cold.Actions.Any(action => action.Module == name && action.Kind == "Archive" && action.Executed),
                $"run-time loaded module {name} must be built: " + Failure(cold));
        }

        string table = File.ReadAllText(Path.Combine(game.OutputDirectory, GeneratedSources.DirectoryName, "Game", "Game" + ModuleTables.FileSuffix));
        Check(table.Split("RocksModuleEntry_Shared()", StringSplitOptions.None).Length == 2 &&
            table.Split("{\"Shared\"", StringSplitOptions.None).Length == 2, "a module both linked and loaded at run time must have one row:\n" + table);
        using (JsonDocument database = JsonDocument.Parse(File.ReadAllText(cold.CompilationDatabasePath)))
        {
            string ArgumentsOf(string source) => string.Join(" ", database.RootElement.EnumerateArray()
                .Single(item => NativeToolchains.PathComparer.Equals(item.GetProperty("file").GetString(), source))
                .GetProperty("arguments").EnumerateArray().Select(item => item.GetString()));
            Check(!ArgumentsOf(engineSource).Contains(pluginInclude, StringComparison.OrdinalIgnoreCase) &&
                !ArgumentsOf(mainSource).Contains(pluginInclude, StringComparison.OrdinalIgnoreCase) &&
                ArgumentsOf(pluginSource).Contains(pluginInclude, StringComparison.OrdinalIgnoreCase),
                "the include path of a run-time loaded module must not reach the modules that load it");
        }

        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            ProcessResult run = await ProcessRunner.RunAsync(new(cold.ModuleOutputs["Game"], [], fixture, request.Toolchain.Environment));
            Check(run.ExitCode == 0 && run.StdOut.Replace("\r", "", StringComparison.Ordinal) ==
                "Core|\nEngine|Core\nShared|Core\nSolo|Core\nPluginHelper|Core\nPlugin|Engine,PluginHelper\nNested|Core\nGame|Engine,Core,Shared\n",
                "module table rows of run-time loaded modules:\n" + run.StdOut + run.StdErr);
        }

        NativeBuildResult warm = await builder.BuildAsync(game);
        Check(warm.Succeeded && warm.ExecutedActions == 0, "an unchanged build with a run-time loaded module must be a no-op: " + Failure(warm));
        File.WriteAllText(pluginSource, PluginSource(2).Replace("\r\n", "\n", StringComparison.Ordinal));
        NativeBuildResult changed = await builder.BuildAsync(game);
        string[] executed = changed.Actions.Where(action => action.Executed).Select(action => action.Module + ":" + action.Kind)
            .Order(StringComparer.Ordinal).ToArray();
        Check(changed.Succeeded && executed.SequenceEqual(["Game:Link", "Plugin:Archive", "Plugin:Compile"]),
            "a changed run-time loaded module must rebuild only itself and the link: " + string.Join(", ", executed) + Failure(changed));

        // 거부.
        NativeModuleInput[] Replace(params NativeModuleInput[] replacements) =>
            modules.Select(module => replacements.FirstOrDefault(item => item.Name == module.Name) ?? module).ToArray();
        NativeModuleInput engine = byName["Engine"];
        NativeModuleInput plugin = byName["Plugin"];
        NativeModuleInput gameModule = byName["Game"];
        await Rejects(builder, game with { Modules = Replace(gameModule with { DynamicModules = ["Engine"] }) },
            "both in Dependencies and DynamicModules", "a module that is both linked and loaded at run time");
        await Rejects(builder, game with { Modules = Replace(plugin with { DynamicModules = ["Plugin"] }) },
            "cannot load itself", "a module that loads itself");
        await Rejects(builder, game with { Modules = Replace(engine with { DynamicModules = ["Game"] }) },
            "is an executable", "an executable loaded at run time");
        await Rejects(builder, game with { Modules = Replace(engine with { DynamicModules = ["Missing"] }) },
            "unknown module Missing", "an unknown run-time loaded module");
        await Rejects(builder, game with { Modules = Replace(engine with { DynamicModules = ["plugin"] }) },
            "differs in case from module 'Plugin'", "a run-time loaded module spelled with different case");
        await Rejects(builder, game with { Modules = Replace(engine with { DynamicModules = ["Plugin", "Plugin"] }) },
            "listed twice", "a run-time loaded module listed twice");
        // 모듈 표에 행이 없는 모듈(header-only·서드파티)은 Load(이름)로 찾을 수 없으므로 적재 목록에 둘 수 없다.
        await Rejects(builder, game with { Modules = Replace(engine with { DynamicModules = ["Headers"] }) },
            "cannot be loaded at run time", "a header-only module loaded at run time");
        await Rejects(builder, game with { Modules = Replace(engine with { DynamicModules = ["Third"] }) },
            "cannot be loaded at run time", "a third-party module loaded at run time");
        // 링크 의존만으로 된 순환은 지금처럼 거부한다(동적 관계를 링크 의존으로 바꾸면 Engine ↔ Plugin 순환이다).
        await Rejects(builder, game with { Modules = Replace(engine with { Dependencies = ["Core", "Plugin"], DynamicModules = [] }) },
            "Module dependency cycle", "a link dependency cycle");
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
