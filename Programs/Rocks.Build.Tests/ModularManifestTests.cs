using System.Runtime.InteropServices;
using System.Text.Json;
using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// Modular 빌드 기반(Docs/Design/BuildTargets.ko.md 3.8절 BS8d1)을 실제 컴파일·링크로 검사한다. 픽스처: 가짜 Core(모듈 표·빌드 정보 제공,
/// 최소 진입 헤더) ← Lib ← App(실행 파일).
/// <list type="bullet">
/// <item>빌드 정보 TU는 Core 공유 라이브러리에 들어가 export되고 실행 파일이 부른다. 실행 파일 모듈 표 TU는 없다.</item>
/// <item>진입점 모듈의 공유 라이브러리마다 export 진입점 <c>RocksModuleEntry</c>가 있고, 진입점 매크로가 빠지면 링크 오류다.</item>
/// <item>manifest: 행 순서·파일·크기·의존·단계·요청 시 적재 표지, 같은 입력의 다시 빌드는 <c>buildId</c>를 다시 쓰고 엔진 버전이 바뀌면 새로
/// 만든다.</item>
/// <item>같은 픽스처의 Monolithic 빌드는 manifest가 없고 실행 파일이 표 TU·빌드 정보를 갖는다.</item>
/// </list>
/// </summary>
internal static class ModularManifestTests
{
    // 표 형식만 같은 최소 진입 헤더다(ModuleTableTests.EntryHeader와 같은 형식). 진입점 매크로는 실제 헤더처럼 공유 라이브러리 표지가 1이면
    // export 진입점을 더 정의한다.
    private const string EntryHeader = """
        #pragma once

        #if defined(_WIN32)
        #define FIXTURE_EXPORT __declspec(dllexport)
        #else
        #define FIXTURE_EXPORT __attribute__((visibility("default")))
        #endif

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

        #define FIXTURE_EXPORTED_ENTRY_0(Name)
        #define FIXTURE_EXPORTED_ENTRY_1(Name)                                                      \
            extern "C" const ::rocks::ModuleEntry* RocksModuleEntryOf_##Name() noexcept;            \
            extern "C" const ::rocks::ModuleEntry* RocksModuleEntryOf_##Name() noexcept {           \
                return &RocksModuleEntry_##Name();                                                  \
            }                                                                                       \
            extern "C" FIXTURE_EXPORT const ::rocks::ModuleEntry* RocksModuleEntry() noexcept;      \
            extern "C" FIXTURE_EXPORT const ::rocks::ModuleEntry* RocksModuleEntry() noexcept {     \
                return RocksModuleEntryOf_##Name();                                                 \
            }
        #define FIXTURE_SELECT_EXPANDED(prefix, flag) prefix##flag
        #define FIXTURE_SELECT(prefix, flag) FIXTURE_SELECT_EXPANDED(prefix, flag)
        #define ROCKS_IMPLEMENT_MODULE(Class, Name)                                                 \
            const ::rocks::ModuleEntry& RocksModuleEntry_##Name() noexcept;                         \
            const ::rocks::ModuleEntry& RocksModuleEntry_##Name() noexcept {                        \
                static constexpr ::rocks::ModuleEntry entry{#Name};                                 \
                return entry;                                                                       \
            }                                                                                       \
            FIXTURE_SELECT(FIXTURE_EXPORTED_ENTRY_, ROCKS_MODULE_SHARED_LIBRARY)(Name)

        """;

    private const string BuildInfoHeader = """
        #pragma once

        namespace rocks {

        struct Sha256Digest {
            static constexpr unsigned Bytes = 32;
            unsigned char bytes[Bytes] = {};
        };

        struct EngineVersion {
            unsigned short majorVersion = 0;
            unsigned short minorVersion = 0;
            unsigned short patchVersion = 0;
        };

        namespace build {

        ROCKS_CORE_API Sha256Digest ToolchainFingerprint() noexcept;
        ROCKS_CORE_API Sha256Digest ToolBuildId() noexcept;
        ROCKS_CORE_API EngineVersion CurrentEngineVersion() noexcept;

        } // namespace build

        } // namespace rocks

        """;

    private static readonly EngineVersion FixtureVersion = new(2, 4, 6);

    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        if (request.Toolchain.Platform == NativePlatform.Android)
        {
            return;
        }

        string fixture = Directory.CreateDirectory(Path.Combine(root, "modular-manifest")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content.Replace("\r\n", "\n", StringComparison.Ordinal));
            return path;
        }

        Write("core/include/Core/Module/ModuleEntry.h", EntryHeader);
        Write("core/include/Core/Build/BuildInfo.h", BuildInfoHeader);
        Write("lib/include/lib/lib.h", "#pragma once\nROCKS_LIB_API int LibValue() noexcept;\n");
        string coreInclude = Path.Combine(fixture, "core", "include");
        string libInclude = Path.Combine(fixture, "lib", "include");
        string coreSource = Write("core/core.cpp", "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Core)\n");
        string libSource = Write("lib/lib.cpp",
            "#include <Core/Module/ModuleEntry.h>\n#include <lib/lib.h>\nROCKS_IMPLEMENT_MODULE(Unused, Lib)\nint LibValue() noexcept { return 7; }\n");
        string libWithoutEntry = Write("lib/lib-no-entry.cpp", "#include <lib/lib.h>\nint LibValue() noexcept { return 7; }\n");
        // 실행 파일은 Core 공유 라이브러리가 export한 빌드 정보를 부른다(값이 비어 있지 않고 엔진 버전이 픽스처 값이다).
        string mainSource = Write("app/main.cpp", """
            #include <Core/Build/BuildInfo.h>
            #include <Core/Module/ModuleEntry.h>
            #include <lib/lib.h>

            ROCKS_IMPLEMENT_MODULE(Unused, App)

            int main() {
                const rocks::Sha256Digest id = rocks::build::ToolBuildId();
                int nonZero = 0;
                for (unsigned index = 0; index < rocks::Sha256Digest::Bytes; ++index) {
                    nonZero |= id.bytes[index];
                }

                const rocks::EngineVersion version = rocks::build::CurrentEngineVersion();
                const bool versionMatches = version.majorVersion == 2 && version.minorVersion == 4 && version.patchVersion == 6;
                return LibValue() == 7 && nonZero != 0 && versionMatches ? 0 : 1;
            }

            """);
        NativeModuleInput[] modules =
        [
            new() { Name = "Core", Sources = [coreSource], PublicIncludeDirectories = [coreInclude], ProvidesModuleTable = true, ProvidesBuildInfo = true },
            new() { Name = "Lib", Sources = [libSource], PublicIncludeDirectories = [libInclude], PublicDependencies = ["Core"] },
            new() { Name = "App", Kind = NativeModuleKind.Executable, Sources = [mainSource], PublicDependencies = ["Lib"] }
        ];
        NativeBuildRequest modular = request with
        {
            OutputDirectory = Path.Combine(fixture, "out-modular"),
            Modules = modules,
            Targets = ["App"],
            TargetName = "Fixture",
            Configuration = NativeConfiguration.Debug,
            LinkType = NativeLinkType.Modular,
            EngineVersion = FixtureVersion
        };
        var builder = new NativeBuildEngine();
        NativeBuildResult built = await builder.BuildAsync(modular);
        Check(built.Succeeded, "Modular core fixture build failed: " + Failure(built));
        ProcessResult run = await ProcessRunner.RunAsync(new(built.ModuleOutputs["App"], [], fixture, request.Toolchain.Environment));
        Check(run.ExitCode == 0, "the executable must call the build information exported by the Core shared library: exit " + run.ExitCode + " " + run.StdErr);
        string generated = Path.Combine(modular.OutputDirectory, GeneratedSources.DirectoryName);
        Check(!Directory.Exists(generated) || !Directory.EnumerateFiles(generated, "*" + ModuleTables.FileSuffix, SearchOption.AllDirectories).Any(),
            "Modular must not generate an executable module table TU");
        string bin = Path.Combine(modular.OutputDirectory, "bin");
        string Shared(string module) => Path.Combine(bin, LinkTypes.SharedLibraryFileName("Fixture", module, request.Toolchain.Platform, NativeConfiguration.Debug));
        foreach (string module in new[] { "Core", "Lib" })
        {
            IntPtr handle = NativeLibrary.Load(Shared(module));
            try
            {
                Check(NativeLibrary.TryGetExport(handle, ModuleTables.ExportedEntrySymbol, out IntPtr entry) && entry != IntPtr.Zero,
                    module + " shared library must export " + ModuleTables.ExportedEntrySymbol);
            }
            finally
            {
                NativeLibrary.Free(handle);
            }
        }

        pass("modular manifest: build information in the Core shared library, an exported entry point per shared library, no module table TU");

        // manifest: 실행 파일 표와 같은 행(Core → Lib → App)에 파일·크기·의존·단계를 싣는다.
        string manifestPath = Path.Combine(bin, ModuleManifests.FileName("Fixture", request.Toolchain.Platform, NativeConfiguration.Debug));
        Check(built.ModuleManifestPath == manifestPath && File.Exists(manifestPath), "manifest path: " + built.ModuleManifestPath);
        string firstId;
        using (JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(manifestPath)))
        {
            JsonElement document = manifest.RootElement;
            Check(document.EnumerateObject().Select(property => property.Name)
                    .SequenceEqual(["schema", "buildId", "engineVersion", "toolchainFingerprint", "modules"]), "manifest keys");
            Check(document.GetProperty("schema").GetInt32() == ModuleManifests.Schema && document.GetProperty("engineVersion").GetString() == "2.4.6" &&
                document.GetProperty("toolchainFingerprint").GetString()!.Length == 64, "manifest header");
            firstId = document.GetProperty("buildId").GetString()!;
            Check(firstId.Length == 32 && firstId.All(character => character is >= '0' and <= '9' or >= 'a' and <= 'f'), "buildId shape: " + firstId);
            JsonElement[] rows = document.GetProperty("modules").EnumerateArray().ToArray();
            Check(rows.Select(row => row.GetProperty("name").GetString()).SequenceEqual(["Core", "Lib", "App"]), "manifest row order");
            foreach (JsonElement row in rows)
            {
                string name = row.GetProperty("name").GetString()!;
                string file = Path.Combine(bin, row.GetProperty("file").GetString()!);
                Check(row.EnumerateObject().Select(property => property.Name)
                        .SequenceEqual(["name", "file", "size", "dependencies", "loadingPhase", "onRequest", "executable"]), "row keys of " + name);
                Check(row.GetProperty("executable").GetBoolean() == (name == "App"), "row executable flag of " + name);
                Check(File.Exists(file) && new FileInfo(file).Length == row.GetProperty("size").GetInt64(), "row file and size of " + name);
                Check(!row.GetProperty("onRequest").GetBoolean(), "row on-request flag of " + name);
            }

            Check(Path.Combine(bin, rows[0].GetProperty("file").GetString()!) == Shared("Core") && rows[2].GetProperty("file").GetString() ==
                Path.GetFileName(built.ModuleOutputs["App"]), "row files point to the shared libraries and the executable");
            Check(rows[1].GetProperty("dependencies").EnumerateArray().Select(value => value.GetString()).SequenceEqual(["Core"]) &&
                rows[2].GetProperty("dependencies").EnumerateArray().Select(value => value.GetString()).SequenceEqual(["Lib"]), "row dependencies");
            Check(rows[0].GetProperty("loadingPhase").GetString() == "Default" && rows[2].GetProperty("loadingPhase").GetString() == "None",
                "row loading phases (an executable row is None)");
        }

        pass("modular manifest: rows, files, sizes, dependencies and loading phases");

        // buildId(MD13): 같은 입력의 다시 빌드는 이전 값을 다시 쓰고, 엔진 버전이 바뀌면 새 값이다.
        NativeBuildResult again = await builder.BuildAsync(modular);
        Check(again.Succeeded && again.ExecutedActions == 0 && BuildId(manifestPath) == firstId, "an unchanged rebuild must recycle the build id");
        NativeBuildResult bumped = await builder.BuildAsync(modular with { EngineVersion = new EngineVersion(2, 4, 7) });
        Check(bumped.Succeeded && BuildId(manifestPath) != firstId, "a new engine version must create a new build id");
        pass("modular manifest: the build id is recycled for the same engine version and toolchain and renewed otherwise");

        // 진입점 매크로가 빠진 진입점 모듈은 공유 라이브러리 링크가 export 진입점을 찾지 못해 실패한다(강제 참조).
        NativeBuildResult missing = await builder.BuildAsync(modular with
        {
            OutputDirectory = Path.Combine(fixture, "out-missing-entry"),
            Modules = modules.Select(module => module.Name == "Lib" ? module with { Sources = [libWithoutEntry] } : module).ToArray()
        });
        Check(!missing.Succeeded && missing.Actions.Any(action => action.Kind == "Link" && action.Module == "Lib" && !action.Succeeded),
            "a module without ROCKS_IMPLEMENT_MODULE must fail its shared library link");
        pass("modular manifest: a missing module entry point is a link error");

        // 입력 검사: Launch에 닿는 타깃은 실행 파일 연결(ML4b) 전까지 거부한다. 모듈 표·빌드 정보 제공 모듈에 닿는 대상 실행 파일이 둘이거나
        // 엔진 버전이 없으면 거부한다(manifest·빌드 정보는 타깃 하나 몫이다).
        Rejects(modular with { Modules = modules.Select(module => module.Name == "Lib" ? module with { ProvidesLaunch = true } : module).ToArray() },
            "reaches the launch module", "a Modular target reaching Launch");
        string otherSource = Write("other/main.cpp", "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Other)\nint main() { return 0; }\n");
        NativeModuleInput[] twoExecutables =
            [.. modules, new() { Name = "Other", Kind = NativeModuleKind.Executable, Sources = [otherSource], PublicDependencies = ["Core"] }];
        Rejects(modular with { Modules = twoExecutables, Targets = ["App", "Other"] }, "targets reach App, Other", "two Modular executables");
        Rejects(modular with { Modules = twoExecutables, Targets = [] }, "targets reach App, Other", "two Modular executables through every target");
        Rejects(modular with { EngineVersion = null }, "needs NativeBuildRequest.EngineVersion", "a Modular executable without an engine version");
        pass("modular manifest: Launch, two executables and a missing engine version are rejected");

        // 같은 픽스처의 Monolithic 빌드는 manifest가 없고 실행 파일이 표 TU를 갖는다.
        NativeBuildRequest monolithic = modular with { OutputDirectory = Path.Combine(fixture, "out-monolithic"), LinkType = NativeLinkType.Monolithic };
        string tableSource = Write("app/table-user.cpp",
            "#include <Core/Module/ModuleEntry.h>\nint TableRows() noexcept { return static_cast<int>(rocks::GeneratedModuleTable().count); }\n");
        NativeBuildResult statically = await builder.BuildAsync(monolithic with
        {
            Modules = modules.Select(module => module.Name == "App" ? module with { Sources = [mainSource, tableSource] } : module).ToArray()
        });
        Check(statically.Succeeded && statically.ModuleManifestPath is null &&
            !Directory.EnumerateFiles(Path.Combine(monolithic.OutputDirectory, "bin"), "*" + ModuleManifests.FileSuffix).Any(),
            "Monolithic must not write a module manifest: " + Failure(statically));
        ProcessResult staticRun = await ProcessRunner.RunAsync(new(statically.ModuleOutputs["App"], [], fixture, request.Toolchain.Environment));
        Check(staticRun.ExitCode == 0, "the Monolithic executable must hold the build information itself");
        pass("modular manifest: Monolithic keeps the executable module table and writes no manifest");
    }

    private static void Rejects(NativeBuildRequest request, string expected, string name)
    {
        try
        {
            InputValidation.Validate(request);
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains(expected, StringComparison.Ordinal), $"{name}: unexpected message: {error.Message}");
            return;
        }

        Check(false, name + " was not rejected");
    }

    private static string BuildId(string path)
    {
        using JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(path));
        return manifest.RootElement.GetProperty("buildId").GetString()!;
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
