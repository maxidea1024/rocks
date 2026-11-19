using System.Text.Json;
using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 리플렉션 생성 소스(<see cref="NativeModuleInput.ReflectionSources"/> — Docs/Design/ReflectionGeneratedSources.ko.md 6.3절)를 실제
/// 컴파일·링크로 검사한다. 픽스처는 MetaCompiler 출력과 같은 모양이다: 모듈 생성 헤더는 등록 함수의 선언만, 모듈 생성 소스는 그 정의를
/// 담는다. 확인하는 것:
/// <list type="bullet">
/// <item>손으로 쓴 소스가 없는 리플렉션 모듈이 archive(ModuleOutputs)가 되고, 실행 파일이 선언만 보고 부른 등록 함수가 링크·실행된다.</item>
/// <item>그 모듈은 모듈 진입점 면제 그대로다: 모듈 표에 들지 않고 ROCKS_MODULE_NAME을 받지 않는다. 설치 SDK의 미리 빌드한 라이브러리
/// (<see cref="NativeModuleInput.ReflectionOnlyLibrary"/>)가 되어도 같다.</item>
/// <item>손으로 쓴 소스가 있는 모듈의 생성 소스는 그 모듈 소스와 같은 include·정의로 컴파일된다(compile_commands.json에도 실린다).</item>
/// <item>증분: no-op 0 action, 생성 소스 디렉터리는 header 디렉터리가 아님, 내용이 바뀌면 그 번역 단위·archive·link만 다시 돈다.</item>
/// <item>잘못된 입력(.cpp가 아닌 경로, 없는 파일, Sources와 겹침, 생성 소스뿐인 실행 파일, 자기 소스가 있는 모듈의 SDK 표지) 거부.</item>
/// </list>
/// </summary>
internal static class ReflectionSourceTests
{
    private const int RegisteredValue = 7;

    private const int ChangedValue = 9;

    private const string ModuleHeader = """
        // Module generated header: declaration only.
        #pragma once
        namespace fixture {
        int RegisterModuleTypes() noexcept;
        }

        """;

    // 진입점 면제 모듈의 생성 소스다. 면제 모듈은 ROCKS_MODULE_NAME을 받지 않는다.
    private static string ModuleSource(int value) => $$"""
        #ifdef ROCKS_MODULE_NAME
        #error "a module with only reflection generated sources received ROCKS_MODULE_NAME"
        #endif
        #include <Fixtures/Fixtures.module.generated.h>
        namespace fixture {
        int RegisterModuleTypes() noexcept {
            return {{value}};
        }
        }

        """;

    private const string MainSource = """
        #include <Core/Module/ModuleEntry.h>
        #include <Fixtures/Fixtures.module.generated.h>
        #include <stdio.h>

        ROCKS_IMPLEMENT_MODULE(Unused, Game)

        const char* EngineReflectionModuleName() noexcept;

        // 모듈 표의 행 이름을 출력하고, 선언만 보고 부른 등록 함수의 값을 종료 코드로 돌려준다.
        int main() {
            const rocks::ModuleTable table = rocks::GeneratedModuleTable();
            for (unsigned long long index = 0; index < table.count; ++index) {
                printf("%s\n", table.rows[index].name);
            }

            printf("reflection=%s\n", EngineReflectionModuleName());
            return fixture::RegisterModuleTypes();
        }

        """;

    public static async Task RunAsync(NativeBuildRequest request, string root)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "reflection-source")).FullName;
        string Write(string relative, string content)
        {
            // 구분자를 정규화한다(compile_commands.json의 경로와 그대로 비교한다).
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content.Replace("\r\n", "\n", StringComparison.Ordinal));
            return path;
        }

        string output = Path.Combine(fixture, "out");
        string coreInclude = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(
            Write("core/include/Core/Module/ModuleEntry.h", ModuleTableTests.EntryHeader)))!)!;
        string coreSource = Write("core/src/core.cpp", "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Core)\n");
        // 생성물은 실제 배치처럼 출력 아래 Generated/<모듈>/Public(include 루트)과 Generated/<모듈>/Source(include 루트 아님)에 둔다.
        string generatedPublic = Path.GetDirectoryName(Path.GetDirectoryName(
            Write("out/Generated/Fixtures/Public/Fixtures/Fixtures.module.generated.h", ModuleHeader))!)!;
        string moduleSource = Write("out/Generated/Fixtures/Source/Fixtures.module.generated.cpp", ModuleSource(RegisteredValue));
        string engineSource = Write("engine/engine.cpp", "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Engine)\n");
        // 손으로 쓴 소스가 있는 모듈의 생성 소스는 모듈 소스와 같은 정의(ROCKS_MODULE_NAME)와 의존 include를 받는다.
        string engineReflection = Write("out/Generated/Engine/Source/Engine.module.generated.cpp",
            "#include <Core/Module/ModuleEntry.h>\nconst char* EngineReflectionModuleName() noexcept;\n" +
            "const char* EngineReflectionModuleName() noexcept { return ROCKS_FIXTURE_TEXT(ROCKS_MODULE_NAME); }\n");
        string mainSource = Write("game/main.cpp", MainSource);
        NativeModuleInput fixtures = new()
        {
            Name = "Fixtures",
            PublicIncludeDirectories = [generatedPublic],
            Dependencies = ["Core"],
            ReflectionSources = [moduleSource]
        };
        NativeModuleInput[] modules =
        [
            new() { Name = "Core", Sources = [coreSource], PublicIncludeDirectories = [coreInclude], ProvidesModuleTable = true },
            fixtures,
            new() { Name = "Engine", Sources = [engineSource], Dependencies = ["Core"], ReflectionSources = [engineReflection] },
            new() { Name = "Game", Kind = NativeModuleKind.Executable, Sources = [mainSource], Dependencies = ["Engine", "Fixtures", "Core"] }
        ];
        NativeBuildRequest game = request with { OutputDirectory = output, Modules = modules, Targets = ["Game"] };
        var builder = new NativeBuildEngine();

        // 진입점 판정: 생성 소스만 있는 모듈은 면제, 자기 소스가 있는 모듈은 대상이다. 미리 빌드한 라이브러리는 SDK 표지가 있을 때만 면제다.
        var byName = modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        HashSet<string> entries = ModuleTables.EntryModules(byName);
        Check(entries.SetEquals(["Core", "Engine", "Game"]), "entry modules must not count reflection generated sources: " + string.Join(", ", entries.Order()));
        string prebuilt = Path.Combine(fixture, "prebuilt.lib");
        NativeModuleInput imported = new() { Name = "Fixtures", Dependencies = ["Core"], PrebuiltLibraries = [prebuilt], ReflectionOnlyLibrary = true };
        byName["Fixtures"] = imported;
        Check(!ModuleTables.EntryModules(byName).Contains("Fixtures"), "an installed SDK reflection-only library must stay exempt from the module entry point");
        byName["Fixtures"] = imported with { ReflectionOnlyLibrary = false };
        Check(ModuleTables.EntryModules(byName).Contains("Fixtures"), "a prebuilt library without the marker is module code");

        NativeBuildResult cold = await builder.BuildAsync(game);
        Check(cold.Succeeded, "reflection source fixture build failed: " + Failure(cold));
        // 손으로 쓴 소스가 없는 리플렉션 모듈이 archive가 된다.
        Check(cold.ModuleOutputs.TryGetValue("Fixtures", out string? archive) && File.Exists(archive) &&
            cold.Actions.Any(action => action.Module == "Fixtures" && action.Kind == "Archive" && action.Executed) &&
            cold.Actions.Count(action => action.Module == "Fixtures" && action.Kind == "Compile" && action.Executed) == 1,
            "a module with only reflection generated sources must be compiled into an archive: " + Failure(cold));
        string table = File.ReadAllText(Path.Combine(output, GeneratedSources.DirectoryName, "Game", "Game" + ModuleTables.FileSuffix));
        Check(!table.Contains("Fixtures", StringComparison.Ordinal) && table.Contains("RocksModuleEntry_Engine", StringComparison.Ordinal),
            "the module table must not reference a reflection-only module:\n" + table);
        // 생성 소스도 compile_commands.json에 그 모듈의 include와 함께 실린다.
        using (JsonDocument database = JsonDocument.Parse(File.ReadAllText(cold.CompilationDatabasePath)))
        {
            JsonElement[] records = database.RootElement.EnumerateArray().ToArray();
            JsonElement record = records.Single(item => NativeToolchains.PathComparer.Equals(item.GetProperty("file").GetString(), moduleSource));
            string[] arguments = record.GetProperty("arguments").EnumerateArray().Select(item => item.GetString()!).ToArray();
            Check(arguments.Any(argument => argument.EndsWith(generatedPublic, StringComparison.Ordinal)) &&
                arguments.Any(argument => argument.EndsWith(coreInclude, StringComparison.Ordinal)) &&
                !arguments.Any(argument => argument.Contains(ModuleTables.ModuleNameMacro, StringComparison.Ordinal)),
                "the generated source must compile with the module's own and dependency includes and without ROCKS_MODULE_NAME");
            JsonElement engineRecord = records.Single(item => NativeToolchains.PathComparer.Equals(item.GetProperty("file").GetString(), engineReflection));
            Check(engineRecord.GetProperty("arguments").EnumerateArray().Any(item => item.GetString()!.EndsWith(ModuleTables.ModuleNameMacro + "=Engine", StringComparison.Ordinal)),
                "the generated source of a module with hand-written sources must receive that module's defines");
        }

        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            ProcessResult run = await ProcessRunner.RunAsync(new(cold.ModuleOutputs["Game"], [], fixture, request.Toolchain.Environment));
            Check(run.ExitCode == RegisteredValue && run.StdOut.Replace("\r", "", StringComparison.Ordinal) == "Core\nEngine\nGame\nreflection=Engine\n",
                $"registration through the declaration must link and run (exit {run.ExitCode}):\n" + run.StdOut + run.StdErr);
        }

        NativeBuildResult warm = await builder.BuildAsync(game);
        Check(warm.Succeeded && warm.ExecutedActions == 0, "an unchanged build with reflection sources must be a no-op: " + Failure(warm));

        // 생성 소스 디렉터리는 include 루트도 header 디렉터리도 아니다: 그 안의 header 확장자 파일은 어떤 action의 입력도 아니다.
        string stray = Path.Combine(Path.GetDirectoryName(moduleSource)!, "stray.h");
        File.WriteAllText(stray, "#pragma once\n");
        NativeBuildResult withStray = await builder.BuildAsync(game);
        Check(withStray.Succeeded && withStray.ExecutedActions == 0, "the generated source directory must not be a header directory: " + Failure(withStray));
        File.Delete(stray);

        // 내용이 바뀌면 그 번역 단위와 archive, 그것을 링크하는 실행 파일만 다시 돈다.
        File.WriteAllText(moduleSource, ModuleSource(ChangedValue).Replace("\r\n", "\n", StringComparison.Ordinal));
        NativeBuildResult changed = await builder.BuildAsync(game);
        string[] executed = changed.Actions.Where(action => action.Executed).Select(action => action.Module + ":" + action.Kind).Order(StringComparer.Ordinal).ToArray();
        Check(changed.Succeeded && executed.SequenceEqual(["Fixtures:Archive", "Fixtures:Compile", "Game:Link"]),
            "a changed generated source must rebuild only its translation unit, archive and the link: " + string.Join(", ", executed) + Failure(changed));
        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            ProcessResult rerun = await ProcessRunner.RunAsync(new(changed.ModuleOutputs["Game"], [], fixture, request.Toolchain.Environment));
            Check(rerun.ExitCode == ChangedValue, $"the changed generated source did not reach the executable (exit {rerun.ExitCode})");
        }

        // 입력 검사.
        NativeBuildRequest With(NativeModuleInput replacement, string name) => game with
        {
            OutputDirectory = Path.Combine(fixture, "out-" + name),
            Modules = modules.Select(module => module.Name == replacement.Name ? replacement : module).ToArray()
        };

        // 실패 안내문(ReflectionSourceFailureNote): 리플렉션 생성 소스의 compile action이 실패하면 컴파일러 출력 뒤에 붙고, 같은 모듈의 손으로
        // 쓴 소스가 실패할 때는 붙지 않는다. 표시 전용이라 문구를 바꿔도 action key가 그대로다(다시 컴파일하지 않는다).
        const string note = "note: RMC3000: fixture note for a failing reflection generated source";
        string brokenReflection = Write("out/Generated/Engine/Source/Public/Broken.generated.cpp", "#error broken generated body\n");
        NativeModuleInput engine = modules.Single(module => module.Name == "Engine");
        NativeBuildResult rejected = await builder.BuildAsync(With(engine with
        {
            ReflectionSources = [engineReflection, brokenReflection],
            ReflectionSourceFailureNote = note
        }, "note-reflection"));
        NativeActionResult[] failedActions = rejected.Actions.Where(action => action.Executed && !action.Succeeded).ToArray();
        Check(!rejected.Succeeded && failedActions.Length == 1 && failedActions[0].Kind == "Compile" && failedActions[0].Module == "Engine" &&
            failedActions[0].StdErr.Contains("broken generated body", StringComparison.Ordinal) &&
            failedActions[0].StdErr.TrimEnd().EndsWith(note, StringComparison.Ordinal),
            "a failing reflection generated source must end its diagnostics with the module's failure note: " + Failure(rejected));
        string brokenSource = Write("engine/broken.cpp", "#error broken hand-written source\n");
        NativeBuildResult plain = await builder.BuildAsync(With(engine with
        {
            Sources = [engineSource, brokenSource],
            ReflectionSourceFailureNote = note
        }, "note-source"));
        Check(!plain.Succeeded && plain.Actions.Where(action => action.Executed && !action.Succeeded)
                .All(action => !action.StdErr.Contains(note, StringComparison.Ordinal)),
            "a failing hand-written source must not carry the reflection source failure note: " + Failure(plain));
        NativeBuildResult renoted = await builder.BuildAsync(game with
        {
            Modules = modules.Select(module => module.Name == "Engine" ? engine with { ReflectionSourceFailureNote = note } : module).ToArray()
        });
        Check(renoted.Succeeded && renoted.ExecutedActions == 0, "the failure note is display-only and must not be part of an action key: " + Failure(renoted));
        await Rejects<ArgumentException>(builder, With(fixtures with { ReflectionSources = [Path.ChangeExtension(moduleSource, ".h")] }, "header"),
            "ReflectionSources", "a reflection source that is not a .cpp file");
        await Rejects<ArgumentException>(builder, With(fixtures with { ReflectionSources = [""] }, "empty"), "ReflectionSources", "an empty reflection source path");
        await Rejects<FileNotFoundException>(builder, With(fixtures with { ReflectionSources = [Path.Combine(fixture, "missing.generated.cpp")] }, "missing"),
            "Source does not exist", "a reflection source that was not generated");
        await Rejects<ArgumentException>(builder, With(fixtures with { Sources = [moduleSource] }, "duplicate"), "Duplicate source",
            "a reflection source that is also a module source");
        await Rejects<ArgumentException>(builder, With(fixtures with { ReflectionOnlyLibrary = true }, "marker-with-sources"), "ReflectionOnlyLibrary",
            "the installed SDK marker on a module with its own generated sources");
        await Rejects<ArgumentException>(builder, With(fixtures with { ReflectionSources = [], ReflectionOnlyLibrary = true }, "marker-without-library"),
            "ReflectionOnlyLibrary", "the installed SDK marker without a prebuilt library");
        await Rejects<ArgumentException>(builder, game with
        {
            OutputDirectory = Path.Combine(fixture, "out-executable"),
            Modules = [new() { Name = "Tool", Kind = NativeModuleKind.Executable, ReflectionSources = [moduleSource] }],
            Targets = ["Tool"]
        }, "Executable requires a source", "an executable with only reflection generated sources");
    }

    private static async Task Rejects<TException>(NativeBuildEngine builder, NativeBuildRequest request, string expected, string name)
        where TException : Exception
    {
        try
        {
            await builder.BuildAsync(request);
        }
        catch (TException error)
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
