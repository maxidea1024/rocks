using System.Text.Json;
using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 리플렉션 생성 소스(<see cref="NativeModuleInput.ReflectionSources"/> — Docs/Design/ReflectionGeneratedSources.ko.md 6.3절)를 실제
/// 컴파일·링크로 검사한다. 픽스처는 MetaCompiler 출력과 같은 모양이다: 모듈 생성 헤더는 등록 함수의 선언만, 모듈 생성 소스는 그 정의를
/// 담는다. 확인하는 것:
/// <list type="bullet">
/// <item>손으로 쓴 소스가 없는 리플렉션 모듈이 archive(ModuleOutputs)가 되고, 실행 파일이 선언만 보고 부른 등록 함수가 링크·실행된다.</item>
/// <item>리플렉션 생성 소스는 모듈 코드다(Docs/Design/ModuleLoader 3.10절): 그런 모듈은 진입점 모듈이라 모듈 표에 들고
/// ROCKS_MODULE_NAME과 자기 타입 표지 ROCKS_MODULE_TYPES=1을 받는다(진입점 TU는 MetadataLayout이 생성 소스로 더한다 — 픽스처는 그
/// 모양을 직접 둔다). 리플렉션 생성 소스가 없는 진입점 모듈은 ROCKS_MODULE_TYPES=0이다.</item>
/// <item>손으로 쓴 소스가 있는 모듈의 생성 소스는 그 모듈 소스와 같은 include·정의로 컴파일된다(compile_commands.json에도 실린다).</item>
/// <item>증분: no-op 0 action, 생성 소스 디렉터리는 header 디렉터리가 아님, 내용이 바뀌면 그 번역 단위·archive·link만 다시 돈다.</item>
/// <item>잘못된 입력(.cpp가 아닌 경로, 없는 파일, Sources와 겹침, 생성 소스뿐인 실행 파일, 규칙이 정의한 ROCKS_MODULE_TYPES) 거부.</item>
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

    // 손으로 쓴 소스가 없는 메타데이터 모듈의 생성 소스다. 진입점 모듈이므로 자기 타입 표지를 받는다.
    private static string ModuleSource(int value) => $$"""
        #if ROCKS_MODULE_TYPES != 1
        #error "a module with reflection generated sources must receive ROCKS_MODULE_TYPES=1"
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
        // 진입점 TU는 MetadataLayout이 손으로 쓴 소스 없는 메타데이터 모듈에 더하는 생성 소스와 같은 모양이다.
        NativeModuleInput fixtures = new()
        {
            Name = "Fixtures",
            PublicIncludeDirectories = [generatedPublic],
            PublicDependencies = ["Core"],
            ReflectionSources = [moduleSource],
            GeneratedSources = [new("Fixtures.ModuleEntry.cpp", "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Fixtures)\n")]
        };
        NativeModuleInput[] modules =
        [
            new() { Name = "Core", Sources = [coreSource], PublicIncludeDirectories = [coreInclude], ProvidesModuleTable = true },
            fixtures,
            new() { Name = "Engine", Sources = [engineSource], PublicDependencies = ["Core"], ReflectionSources = [engineReflection] },
            new() { Name = "Game", Kind = NativeModuleKind.Executable, Sources = [mainSource], PublicDependencies = ["Engine", "Fixtures", "Core"] }
        ];
        NativeBuildRequest game = request with { OutputDirectory = output, Modules = modules, Targets = ["Game"] };
        var builder = new NativeBuildEngine();

        // 진입점 판정: 리플렉션 생성 소스만 있어도 모듈 코드다. 설치 SDK의 미리 빌드한 라이브러리도 모듈 코드다.
        var byName = modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        byName["Fixtures"] = fixtures with { GeneratedSources = [] };
        HashSet<string> entries = ModuleTables.EntryModules(byName);
        Check(entries.SetEquals(["Core", "Engine", "Fixtures", "Game"]), "entry modules must count reflection generated sources: " + string.Join(", ", entries.Order()));
        byName["Fixtures"] = new() { Name = "Fixtures", PublicDependencies = ["Core"], PrebuiltLibraries = [Path.Combine(fixture, "prebuilt.lib")] };
        Check(ModuleTables.EntryModules(byName).Contains("Fixtures"), "an installed SDK library of a metadata module is module code");

        NativeBuildResult cold = await builder.BuildAsync(game);
        Check(cold.Succeeded, "reflection source fixture build failed: " + Failure(cold));
        // 손으로 쓴 소스가 없는 리플렉션 모듈이 archive가 된다(모듈 생성 소스와 진입점 TU).
        Check(cold.ModuleOutputs.TryGetValue("Fixtures", out string? archive) && File.Exists(archive) &&
            cold.Actions.Any(action => action.Module == "Fixtures" && action.Kind == "Archive" && action.Executed) &&
            cold.Actions.Count(action => action.Module == "Fixtures" && action.Kind == "Compile" && action.Executed) == 2,
            "a module with only reflection generated sources must be compiled into an archive: " + Failure(cold));
        string table = File.ReadAllText(Path.Combine(output, GeneratedSources.DirectoryName, "Game", "Game" + ModuleTables.FileSuffix));
        Check(table.Contains("RocksModuleEntry_Fixtures", StringComparison.Ordinal) && table.Contains("RocksModuleEntry_Engine", StringComparison.Ordinal),
            "the module table must reference the metadata module's entry point:\n" + table);
        // 생성 소스도 compile_commands.json에 그 모듈의 include와 함께 실린다.
        using (JsonDocument database = JsonDocument.Parse(File.ReadAllText(cold.CompilationDatabasePath)))
        {
            JsonElement[] records = database.RootElement.EnumerateArray().ToArray();
            JsonElement record = records.Single(item => NativeToolchains.PathComparer.Equals(item.GetProperty("file").GetString(), moduleSource));
            string[] arguments = record.GetProperty("arguments").EnumerateArray().Select(item => item.GetString()!).ToArray();
            Check(arguments.Any(argument => argument.EndsWith(generatedPublic, StringComparison.Ordinal)) &&
                arguments.Any(argument => argument.EndsWith(coreInclude, StringComparison.Ordinal)) &&
                arguments.Any(argument => argument.EndsWith(ModuleDefinitions.PathOf(output, "Fixtures"), StringComparison.Ordinal)) &&
                File.ReadAllText(ModuleDefinitions.PathOf(output, "Fixtures")).Contains("#define " + ModuleTables.ModuleNameMacro + " Fixtures\n", StringComparison.Ordinal) &&
                File.ReadAllText(ModuleDefinitions.PathOf(output, "Fixtures")).Contains("#define " + ModuleTables.ModuleTypesMacro + " 1\n", StringComparison.Ordinal),
                "the generated source must compile with the module's own and dependency includes and its entry module defines");
            JsonElement engineRecord = records.Single(item => NativeToolchains.PathComparer.Equals(item.GetProperty("file").GetString(), engineReflection));
            // 모듈 정의(ROCKS_MODULE_NAME·ROCKS_MODULE_TYPES 포함)는 명령줄이 아니라 강제 include하는 정의 헤더로 간다
            // (Docs/Design/UnityBuildPch.ko.md 8.2절).
            string engineDefinitions = ModuleDefinitions.PathOf(output, "Engine");
            Check(engineRecord.GetProperty("arguments").EnumerateArray().Any(item => item.GetString()!.EndsWith(engineDefinitions, StringComparison.Ordinal)) &&
                File.ReadAllText(engineDefinitions).Contains("#define " + ModuleTables.ModuleNameMacro + " Engine\n", StringComparison.Ordinal),
                "the generated source of a module with hand-written sources must receive that module's definitions header");
            // 리플렉션 생성 소스가 없는 진입점 모듈은 자기 타입 표지가 0이다.
            Check(File.ReadAllText(ModuleDefinitions.PathOf(output, "Game")).Contains("#define " + ModuleTables.ModuleTypesMacro + " 0\n", StringComparison.Ordinal),
                "an entry module without reflection generated sources must receive ROCKS_MODULE_TYPES=0");
        }

        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            ProcessResult run = await ProcessRunner.RunAsync(new(cold.ModuleOutputs["Game"], [], fixture, request.Toolchain.Environment));
            Check(run.ExitCode == RegisteredValue && run.StdOut.Replace("\r", "", StringComparison.Ordinal) == "Core\nEngine\nFixtures\nGame\nreflection=Engine\n",
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
        await Rejects<ArgumentException>(builder, With(fixtures with { PrivateDefines = [ModuleTables.ModuleTypesMacro + "=1"] }, "types-macro"),
            ModuleTables.ModuleTypesMacro, "a rule that defines the module types marker");
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
