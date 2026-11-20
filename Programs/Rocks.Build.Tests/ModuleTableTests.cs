using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Tooling.Common;

/// <summary>
/// 모듈 표 TU(Docs/Design/ModuleLoader.ko.md 3.2절, <see cref="ModuleTables"/>)를 실제 컴파일·링크로 검사한다. 픽스처는 가짜 Core(표지
/// <see cref="NativeModuleInput.ProvidesModuleTable"/>, 최소 <c>Core/Module/ModuleEntry.h</c>)와 그 아래 모듈·header-only·서드파티·엔진·게임
/// 실행 파일이다. 확인하는 것: 표 행(의존 순서, 면제 모듈 제외, 면제 모듈을 거치는 의존의 대체, 기술자 단계), 진입점 모듈만 받는
/// <c>ROCKS_MODULE_NAME</c>, 증분 no-op, 매크로가 빠진 모듈의 링크 오류, 대소문자만 다른 모듈 이름·의존 철자 거부(MD17), 예약 매크로 거부,
/// 식별자가 아닌 진입점 모듈 이름 거부, 표지가 없는 빌드의 무변화, 단계 순서(MD6)·부트 모듈 단계 거부(3.11절).
/// </summary>
internal static class ModuleTableTests
{
    // 실제 ModuleEntry.h는 Core 컨테이너·할당기를 끌어오므로 표 형식만 같은 최소 헤더를 쓴다. 매크로는 진입 기술 이름만 채운다.
    // ReflectionSourceTests도 같은 표 형식 헤더를 쓴다.
    internal const string EntryHeader = """
        #pragma once

        namespace rocks {

        enum class LoadingPhase : unsigned char { EarliestPossible, PostConfigInit, PreDefault, Default, PostDefault, PostEngineInit, None };

        struct ModuleEntry {
            const char* name;
        };

        using ModuleEntryFunction = const ModuleEntry& (*)() noexcept;

        struct ModuleTableRow {
            const char* name;
            ModuleEntryFunction entry;
            const char* const* dependencies;
            unsigned long long dependencyCount;
            LoadingPhase phase;
            bool onRequest;
        };

        struct ModuleTable {
            template <unsigned long long Count>
            constexpr ModuleTable(const ModuleTableRow (&values)[Count]) noexcept : rows(values), count(Count) {
            }

            const ModuleTableRow* rows;
            unsigned long long count;
        };

        ModuleTable GeneratedModuleTable() noexcept;

        } // namespace rocks

        #define ROCKS_FIXTURE_TEXT_EXPANDED(value) #value
        #define ROCKS_FIXTURE_TEXT(value) ROCKS_FIXTURE_TEXT_EXPANDED(value)
        #define ROCKS_IMPLEMENT_MODULE(Class, Name)                                         \
            const ::rocks::ModuleEntry& RocksModuleEntry_##Name() noexcept;                 \
            const ::rocks::ModuleEntry& RocksModuleEntry_##Name() noexcept {                \
                static constexpr ::rocks::ModuleEntry entry{#Name};                         \
                return entry;                                                               \
            }

        """;

    private const string MainSource = """
        #include <Core/Module/ModuleEntry.h>
        #include <stdio.h>
        #include <string.h>

        ROCKS_IMPLEMENT_MODULE(Unused, Game)

        const char* EngineModuleName() noexcept;

        // 표 행을 "이름|의존,의존|단계"로 출력하고, 행 이름과 진입 기술 이름이 다르면 실패한다.
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

                printf("|%d\n", static_cast<int>(row.phase));
            }

            printf("name=%s,%s\n", ROCKS_FIXTURE_TEXT(ROCKS_MODULE_NAME), EngineModuleName());
            return 0;
        }

        """;

    public static async Task RunAsync(NativeBuildRequest request, string root)
    {
        string fixture = Directory.CreateDirectory(Path.Combine(root, "module-table")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.Combine(fixture, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content.Replace("\r\n", "\n", StringComparison.Ordinal));
            return path;
        }

        string coreInclude = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(
            Write("core/include/Core/Module/ModuleEntry.h", EntryHeader)))!)!;
        // 면제 모듈(Core 아래·서드파티)은 ROCKS_MODULE_NAME을 받지 않는다.
        const string exempt = "#ifdef ROCKS_MODULE_NAME\n#error \"exempt module received ROCKS_MODULE_NAME\"\n#endif\nint {0}Value() noexcept {{ return 1; }}\n";
        string platformSource = Write("platform/platform.cpp", string.Format(System.Globalization.CultureInfo.InvariantCulture, exempt, "Platform"));
        string thirdSource = Write("third/third.cpp", string.Format(System.Globalization.CultureInfo.InvariantCulture, exempt, "Third"));
        string coreSource = Write("core/src/core.cpp", "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Core)\n");
        string headersInclude = Directory.CreateDirectory(Path.Combine(fixture, "headers", "include")).FullName;
        const string engineText = "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Engine)\n" +
            "const char* EngineModuleName() noexcept;\nconst char* EngineModuleName() noexcept { return ROCKS_FIXTURE_TEXT(ROCKS_MODULE_NAME); }\n";
        string engineSource = Write("engine/engine.cpp", engineText);
        string mainSource = Write("game/main.cpp", MainSource);
        NativeModuleInput[] modules =
        [
            new() { Name = "Platform", Sources = [platformSource] },
            new() { Name = "Core", Sources = [coreSource], PublicIncludeDirectories = [coreInclude], Dependencies = ["Platform"], ProvidesModuleTable = true },
            new() { Name = "Headers", PublicIncludeDirectories = [headersInclude], Dependencies = ["Core"] },
            new() { Name = "Third", Sources = [thirdSource], ThirdParty = true },
            new() { Name = "Engine", Sources = [engineSource], Dependencies = ["Headers", "Third"] },
            new()
            {
                Name = "Game", Kind = NativeModuleKind.Executable, Sources = [mainSource], Dependencies = ["Engine", "Core"],
                LoadingPhase = LoadingPhase.PostDefault
            }
        ];
        NativeBuildRequest game = request with
        {
            OutputDirectory = Path.Combine(fixture, "out"),
            Modules = modules,
            Targets = ["Game"]
        };
        var builder = new NativeBuildEngine();
        NativeBuildResult cold = await builder.BuildAsync(game);
        Check(cold.Succeeded, "module table fixture build failed: " + Failure(cold));
        string tablePath = Path.Combine(game.OutputDirectory, GeneratedSources.DirectoryName, "Game", "Game" + ModuleTables.FileSuffix);
        string table = File.ReadAllText(tablePath);
        Check(table.StartsWith("// Generated by Rocks.Build. Do not edit.\n", StringComparison.Ordinal) && !table.Contains('\r') &&
            table.All(char.IsAscii) && !table.Contains(fixture, StringComparison.OrdinalIgnoreCase) &&
            !table.Contains("Platform", StringComparison.Ordinal) && !table.Contains("Third", StringComparison.Ordinal) &&
            !table.Contains("Headers", StringComparison.Ordinal), "module table must exclude exempt modules and host paths:\n" + table);
        if (request.Toolchain.Platform != NativePlatform.Android)
        {
            ProcessResult run = await ProcessRunner.RunAsync(new(cold.ModuleOutputs["Game"], [], fixture, request.Toolchain.Environment));
            // Engine의 의존 Headers(header-only)는 그 너머의 Core로 바뀐다. Game의 직접 의존 Core는 그대로 남는다. 실행 파일 행의 단계는
            // 규칙 값(PostDefault)과 무관하게 None이다(단계 실행 대상이 아니다 — Docs/Design/ModuleLoader.ko.md 3.11절).
            Check(run.ExitCode == 0 && run.StdOut.Replace("\r", "", StringComparison.Ordinal) ==
                "Core||3\nEngine|Core|3\nGame|Engine,Core|6\nname=Game,Engine\n", "module table rows and ROCKS_MODULE_NAME:\n" + run.StdOut + run.StdErr);
        }

        NativeBuildResult warm = await builder.BuildAsync(game);
        Check(warm.Succeeded && warm.ExecutedActions == 0, "an unchanged module table must not rebuild: " + Failure(warm));

        // ROCKS_IMPLEMENT_MODULE이 빠진 모듈은 표가 그 진입 함수를 참조하므로 링크 오류다.
        string missingSource = Write("engine-missing/engine.cpp",
            "#include <Core/Module/ModuleEntry.h>\nconst char* EngineModuleName() noexcept;\nconst char* EngineModuleName() noexcept { return \"\"; }\n");
        NativeBuildRequest missing = game with
        {
            OutputDirectory = Path.Combine(fixture, "out-missing"),
            Modules = modules.Select(module => module.Name == "Engine" ? module with { Sources = [missingSource] } : module).ToArray()
        };
        NativeBuildResult unlinked = await builder.BuildAsync(missing);
        NativeActionResult link = unlinked.Actions.Single(action => action.Kind == "Link");
        Check(!unlinked.Succeeded && !link.Succeeded && (link.StdOut + link.StdErr).Contains("RocksModuleEntry_Engine", StringComparison.Ordinal),
            "a module without ROCKS_IMPLEMENT_MODULE must fail to link: " + link.StdOut + link.StdErr);

        // 표지가 없으면 표·정의를 만들지 않는다(빌드는 그대로 성공한다 — 진입 함수는 참조되지 않는다).
        string plainMain = Write("game-plain/main.cpp", "int main() { return 0; }\n");
        NativeBuildRequest unmarked = game with
        {
            OutputDirectory = Path.Combine(fixture, "out-unmarked"),
            Modules = modules.Select(module => module with
            {
                ProvidesModuleTable = false,
                Sources = module.Name == "Game" ? [plainMain] : module.Name == "Engine" ? [missingSource] : module.Sources
            }).ToArray()
        };
        NativeBuildResult plain = await builder.BuildAsync(unmarked);
        Check(plain.Succeeded && !Directory.Exists(Path.Combine(unmarked.OutputDirectory, GeneratedSources.DirectoryName)),
            "a build without the module table marker must not generate a table: " + Failure(plain));

        // MD17: 대소문자만 다른 모듈 이름, 대소문자가 다른 의존 철자. 예약 매크로. 식별자가 아닌 진입점 모듈 이름.
        await Rejects(builder, game with { Modules = [.. modules, new() { Name = "engine", Sources = [engineSource] }] },
            "differ only in case", "module names that differ only in case");
        await Rejects(builder, game with
        {
            Modules = modules.Select(module => module.Name == "Engine" ? module with { Dependencies = ["headers", "Third"] } : module).ToArray()
        }, "differs in case from module 'Headers'", "a dependency spelled with different case");
        await Rejects(builder, game with
        {
            Modules = modules.Select(module => module.Name == "Engine" ? module with { PrivateDefines = ["ROCKS_MODULE_NAME=Other"] } : module).ToArray()
        }, "is reserved", "a rule-defined ROCKS_MODULE_NAME");
        await Rejects(builder, game with
        {
            Modules = modules.Select(module => module.Name == "Engine" ? module with { Name = "Engine-2" }
                : module.Name == "Game" ? module with { Dependencies = ["Engine-2", "Core"] } : module).ToArray()
        }, "must have a C identifier name", "a non-identifier entry module name");

        // MD6: 의존의 단계가 행보다 늦으면 거부한다(Engine Default → Core PostEngineInit). 실행 파일 행은 None이라 검사하지 않는다.
        await Rejects(builder, game with
        {
            Modules = modules.Select(module => module.Name == "Core" ? module with { LoadingPhase = LoadingPhase.PostEngineInit } : module).ToArray()
        }, "depends on Core (loading phase PostEngineInit)", "a dependency in a later loading phase");
        // None 행은 검사하지 않고 None 의존은 늦은 단계가 아니다(코드가 Load로 시작한다).
        NativeBuildResult manual = await builder.BuildAsync(game with
        {
            OutputDirectory = Path.Combine(fixture, "out-none"),
            Modules = modules.Select(module => module.Name == "Engine" ? module with { LoadingPhase = LoadingPhase.None }
                : module.Name == "Core" ? module with { LoadingPhase = LoadingPhase.PostEngineInit } : module).ToArray(),
            Targets = ["Engine"]
        });
        Check(manual.Succeeded, "a None module may depend on a later phase: " + Failure(manual));
        // 부트 모듈(공통 실행 진입점 표지 모듈의 링크 폐포)은 EarliestPossible이어야 한다.
        await Rejects(builder, game with
        {
            Modules = modules.Select(module => module.Name == "Engine" ? module with { ProvidesLaunch = true, LoadingPhase = LoadingPhase.EarliestPossible }
                : module).ToArray()
        }, "Module Core: boot module (in the dependency closure of Engine) must use the EarliestPossible loading phase", "a boot module in a later phase");
    }

    private static async Task Rejects(NativeBuildEngine builder, NativeBuildRequest request, string expected, string name)
    {
        string output = request.OutputDirectory + "-rejected";
        try
        {
            await builder.BuildAsync(request with { OutputDirectory = output });
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
