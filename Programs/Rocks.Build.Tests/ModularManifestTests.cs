using System.Runtime.InteropServices;
using System.Text.Json;
using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Tooling.Common;

/// <summary>
/// Modular 빌드 기반(Docs/Design/BuildTargets 3.8절 BS8d1)과 실행 파일 연결(Docs/Design/ModuleLoader 3.14절 ML4b)을 실제
/// 컴파일·링크로 검사한다. 픽스처: 가짜 Core(모듈 표·빌드 정보 제공, 최소 진입 헤더) ← Lib·가짜 Launch(Modular 진입 C 함수) ← App(실행 파일
/// 모듈 — Modular에서 호스트 공유 라이브러리), App의 동적 모듈 Dyn.
/// <list type="bullet">
/// <item>빌드 정보 TU는 Core 공유 라이브러리에 들어가 export되고 호스트 모듈이 부른다. 실행 파일 모듈 표 TU는 없다.</item>
/// <item>실행 파일은 생성 진입 TU와 Launch만 링크해 Launch의 Modular 진입을 manifest 이름과 함께 부르고, import하는 픽스처 공유 라이브러리는
/// Launch뿐이다. 호스트 공유 라이브러리는 앱 기술 함수를 export하고 동적 모듈을 import하지 않는다. Windows 창 subsystem은 창 진입을 부른다.</item>
/// <item>진입점 모듈의 공유 라이브러리마다 export 진입점 <c>RocksModuleEntry</c>가 있고, 진입점 매크로가 빠지면 링크 오류다.</item>
/// <item>manifest: 행 순서·파일·크기·의존·단계·요청 시 적재 표지, 같은 입력의 다시 빌드는 <c>buildId</c>를 다시 쓰고 엔진 버전이 바뀌면 새로
/// 만든다.</item>
/// <item>플러그인 모듈(ModuleLoader 3.15절 ML5a): 플러그인 binaries 폴더·플러그인 manifest·실행 파일 manifest의 plugins·Monolithic 표 순서의
/// order, 호스트는 직접 의존하는 플러그인 모듈만 import하고 다른 폴더를 넘는 적재가 된다.</item>
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

        enum class TargetType : unsigned char { Game, Editor, Client, Server, Program };

        ROCKS_CORE_API TargetType CurrentTargetType() noexcept;

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
        // 앱 본문은 Core 공유 라이브러리가 export한 빌드 정보를 부른다(값이 비어 있지 않고 엔진 버전이 픽스처 값이다). Modular 호스트 모듈에서는
        // 앱 기술 함수 자리(RocksApplicationDescription)에 그 검사를 export하고, Monolithic에서는 main이 부른다.
        string mainSource = Write("app/main.cpp", """
            #include <Core/Build/BuildInfo.h>
            #include <Core/Module/ModuleEntry.h>
            #include <lib/lib.h>

            ROCKS_IMPLEMENT_MODULE(Unused, App)

            static int Check() noexcept {
                const rocks::Sha256Digest id = rocks::build::ToolBuildId();
                int nonZero = 0;
                for (unsigned index = 0; index < rocks::Sha256Digest::Bytes; ++index) {
                    nonZero |= id.bytes[index];
                }

                const rocks::EngineVersion version = rocks::build::CurrentEngineVersion();
                const bool versionMatches = version.majorVersion == 2 && version.minorVersion == 4 && version.patchVersion == 6;
                return LibValue() == 7 && nonZero != 0 && versionMatches ? 0 : 1;
            }

            #if ROCKS_MODULE_SHARED_LIBRARY
            extern "C" FIXTURE_EXPORT int RocksApplicationDescription() noexcept;
            extern "C" FIXTURE_EXPORT int RocksApplicationDescription() noexcept {
                return Check();
            }
            #else
            int main() {
                return Check();
            }
            #endif

            """);
        string manifestName = ModuleManifests.FileName("Fixture", request.Toolchain.Platform, NativeConfiguration.Debug);
        // 가짜 Launch: Modular 진입 C 함수가 받은 manifest 이름을 대조한다(콘솔 0, 창 0, 틀리면 3).
        string launchSource = Write("launch/launch.cpp", $$"""
            #include <Core/Module/ModuleEntry.h>
            #include <string.h>

            ROCKS_IMPLEMENT_MODULE(Unused, Launch)

            static int CheckManifest(const char* manifest) noexcept {
                return strcmp(manifest, "{{manifestName}}") == 0 ? 0 : 3;
            }

            extern "C" ROCKS_LAUNCH_API int RocksLaunchModularApplication(int argc, char** argv, const char* manifest) noexcept;
            extern "C" ROCKS_LAUNCH_API int RocksLaunchModularApplication(int argc, char** argv, const char* manifest) noexcept {
                return argc >= 1 && argv != nullptr ? CheckManifest(manifest) : 4;
            }

            extern "C" ROCKS_LAUNCH_API int RocksLaunchModularWideApplication(int argc, wchar_t** argv, const char* manifest) noexcept;
            extern "C" ROCKS_LAUNCH_API int RocksLaunchModularWideApplication(int argc, wchar_t** argv, const char* manifest) noexcept {
                return argc >= 1 && argv != nullptr ? CheckManifest(manifest) : 4;
            }

            extern "C" ROCKS_LAUNCH_API int RocksLaunchModularWindowedApplication(const char* manifest) noexcept;
            extern "C" ROCKS_LAUNCH_API int RocksLaunchModularWindowedApplication(const char* manifest) noexcept {
                return CheckManifest(manifest);
            }

            """);
        string dynSource = Write("dyn/dyn.cpp", "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Dyn)\n");
        NativeModuleInput[] modules =
        [
            new()
            {
                Name = "Core", Sources = [coreSource], PublicIncludeDirectories = [coreInclude], ProvidesModuleTable = true, ProvidesBuildInfo = true,
                LoadingPhase = LoadingPhase.EarliestPossible
            },
            new() { Name = "Lib", Sources = [libSource], PublicIncludeDirectories = [libInclude], PublicDependencies = ["Core"] },
            new() { Name = "Launch", Sources = [launchSource], PublicDependencies = ["Core"], ProvidesLaunch = true, LoadingPhase = LoadingPhase.EarliestPossible },
            new() { Name = "Dyn", Sources = [dynSource], PublicDependencies = ["Core"] },
            new()
            {
                Name = "App", Kind = NativeModuleKind.Executable, Sources = [mainSource], PublicDependencies = ["Lib", "Launch"], DynamicModules = ["Dyn"]
            }
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
        string bin = Path.Combine(modular.OutputDirectory, "bin");
        string Shared(string module) => Path.Combine(bin, LinkTypes.SharedLibraryFileName("Fixture", module, request.Toolchain.Platform, NativeConfiguration.Debug));
        // 실행 파일은 진입 스텁이다: Launch의 Modular 진입을 manifest 이름과 함께 부른다.
        string executable = Path.Combine(bin, "App" + (request.Toolchain.Platform == NativePlatform.Windows ? ".exe" : ""));
        Check(built.ModuleOutputs["App"] == executable, "the executable module output is the launcher executable: " + built.ModuleOutputs["App"]);
        ProcessResult run = await ProcessRunner.RunAsync(new(executable, [], fixture, request.Toolchain.Environment));
        Check(run.ExitCode == 0, "the launcher must call the Launch Modular entry with the manifest name: exit " + run.ExitCode + " " + run.StdErr);
        // 호스트 공유 라이브러리가 앱 기술 자리에 export한 본문 검사(Core 공유 라이브러리의 빌드 정보)를 부른다.
        IntPtr hostLibrary = NativeLibrary.Load(Shared("App"));
        try
        {
            Check(NativeLibrary.TryGetExport(hostLibrary, "RocksApplicationDescription", out IntPtr description) && description != IntPtr.Zero,
                "the host shared library must export RocksApplicationDescription");
            Check(Marshal.GetDelegateForFunctionPointer<HostCheck>(description)() == 0,
                "the host module must call the build information exported by the Core shared library");
        }
        finally
        {
            NativeLibrary.Free(hostLibrary);
        }

        // import: 실행 파일은 픽스처 공유 라이브러리 가운데 Launch만, 호스트는 동적 모듈 Dyn을 import하지 않는다.
        string[] executableImports = await FixtureImports(request, executable);
        Check(executableImports.SequenceEqual([Path.GetFileName(Shared("Launch"))], StringComparer.OrdinalIgnoreCase),
            "the launcher executable must import only the Launch shared library: " + string.Join(", ", executableImports));
        string[] hostImports = await FixtureImports(request, Shared("App"));
        Check(hostImports.Contains(Path.GetFileName(Shared("Lib")), StringComparer.OrdinalIgnoreCase) &&
            !hostImports.Contains(Path.GetFileName(Shared("Dyn")), StringComparer.OrdinalIgnoreCase),
            "the host shared library must import its link dependencies but not its dynamic modules: " + string.Join(", ", hostImports));
        string generated = Path.Combine(modular.OutputDirectory, GeneratedSources.DirectoryName);
        Check(!Directory.Exists(generated) || !Directory.EnumerateFiles(generated, "*" + ModuleTables.FileSuffix, SearchOption.AllDirectories).Any(),
            "Modular must not generate an executable module table TU");
        foreach (string module in new[] { "Core", "Lib", "Launch", "Dyn", "App" })
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

        // export 수 진단(BS8d3): Windows Modular 빌드는 모듈별 export 수 한 줄을 남기고, 문턱 이상인 모듈을 따로 알린다.
        if (request.Toolchain.Platform == NativePlatform.Windows)
        {
            string? exports = built.Notices.SingleOrDefault(notice => notice.StartsWith("Modular exports: ", StringComparison.Ordinal));
            Check(exports is not null && exports.Contains("Core ", StringComparison.Ordinal) && exports.Contains("Launch ", StringComparison.Ordinal),
                "a Windows Modular build reports the export count of each shared library: " + string.Join(" | ", built.Notices));
            int coreExports = ModularExports.CountPeExports(Shared("Core"));
            Check(coreExports > 0 && exports!.Contains("Core " + coreExports, StringComparison.Ordinal), "the Core export count matches its PE export table");
            IReadOnlyList<string> warned = ModularExports.Diagnose(new Dictionary<string, int> { ["Core"] = coreExports, ["Lib"] = 1 }, coreExports);
            Check(warned.Count == 2 && warned[1].StartsWith("Module Core exports " + coreExports, StringComparison.Ordinal),
                "a module at the warning threshold gets its own line: " + string.Join(" | ", warned));
            Check(ModularExports.Diagnose(new Dictionary<string, int>()).Count == 0, "no shared libraries, no diagnostic");
        }

        pass("modular manifest: build information in the Core shared library, an exported entry point per shared library, no module table TU, " +
            "a launcher executable importing only Launch and a host shared library");

        // manifest: 실행 파일 표와 같은 행(Core → Lib → App)에 파일·크기·의존·단계를 싣는다.
        string manifestPath = Path.Combine(bin, ModuleManifests.FileName("Fixture", request.Toolchain.Platform, NativeConfiguration.Debug));
        Check(built.ModuleManifestPath == manifestPath && File.Exists(manifestPath), "manifest path: " + built.ModuleManifestPath);
        string firstId;
        using (JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(manifestPath)))
        {
            JsonElement document = manifest.RootElement;
            Check(document.EnumerateObject().Select(property => property.Name)
                    .SequenceEqual(["schema", "buildId", "engineVersion", "toolchainFingerprint", "plugins", "modules"]), "manifest keys");
            Check(document.GetProperty("plugins").GetArrayLength() == 0, "a target without plugins lists no plugin manifests");
            Check(document.GetProperty("schema").GetInt32() == ModuleManifests.Schema && document.GetProperty("engineVersion").GetString() == "2.4.6" &&
                document.GetProperty("toolchainFingerprint").GetString()!.Length == 64, "manifest header");
            firstId = document.GetProperty("buildId").GetString()!;
            Check(firstId.Length == 32 && firstId.All(character => character is >= '0' and <= '9' or >= 'a' and <= 'f'), "buildId shape: " + firstId);
            JsonElement[] rows = document.GetProperty("modules").EnumerateArray().ToArray();
            string[] order = rows.Select(row => row.GetProperty("name").GetString()!).ToArray();
            Check(order.Length == 5 && order[0] == "Core" && order[^1] == "App" && order.Order().SequenceEqual(["App", "Core", "Dyn", "Launch", "Lib"]),
                "manifest row order: " + string.Join(", ", order));
            foreach (JsonElement row in rows)
            {
                string name = row.GetProperty("name").GetString()!;
                string file = Path.Combine(bin, row.GetProperty("file").GetString()!);
                Check(row.EnumerateObject().Select(property => property.Name)
                        .SequenceEqual(["name", "order", "file", "size", "dependencies", "loadingPhase", "onRequest", "host"]), "row keys of " + name);
                Check(row.GetProperty("order").GetInt32() == Array.IndexOf(order, name), "row order of " + name + " is its table position");
                Check(row.GetProperty("host").GetBoolean() == (name == "App"), "row host flag of " + name);
                Check(File.Exists(file) && new FileInfo(file).Length == row.GetProperty("size").GetInt64(), "row file and size of " + name);
                Check(row.GetProperty("onRequest").GetBoolean() == (name == "Dyn"), "row on-request flag of " + name);
                Check(file == Shared(name), "row file of " + name + " is its shared library (the host row too)");
            }

            JsonElement Row(string name) => rows.Single(row => row.GetProperty("name").GetString() == name);
            Check(Row("Lib").GetProperty("dependencies").EnumerateArray().Select(value => value.GetString()).SequenceEqual(["Core"]) &&
                Row("App").GetProperty("dependencies").EnumerateArray().Select(value => value.GetString()).Order().SequenceEqual(["Launch", "Lib"]),
                "row dependencies");
            Check(Row("Core").GetProperty("loadingPhase").GetString() == "EarliestPossible" && Row("App").GetProperty("loadingPhase").GetString() == "None",
                "row loading phases (the host row is None)");
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

        // Windows 창 subsystem 실행 파일은 wWinMain에서 Launch의 Modular 창 진입을 부른다.
        if (request.Toolchain.Platform == NativePlatform.Windows)
        {
            NativeBuildResult windowed = await builder.BuildAsync(modular with
            {
                OutputDirectory = Path.Combine(fixture, "out-windowed"),
                Modules = modules.Select(module => module.Name == "App" ? module with { WindowsSubsystem = WindowsSubsystem.Windows } : module).ToArray()
            });
            Check(windowed.Succeeded, "Modular windowed fixture build failed: " + Failure(windowed));
            ProcessResult windowedRun = await ProcessRunner.RunAsync(new(windowed.ModuleOutputs["App"], [], fixture, request.Toolchain.Environment));
            Check(windowedRun.ExitCode == 0, "the windowed launcher must call the Launch Modular windowed entry: exit " + windowedRun.ExitCode);
            pass("modular manifest: a windowed launcher calls the Launch Modular windowed entry");
        }

        // 입력 검사: 모듈 표 제공 모듈에 닿는 Modular 실행 파일은 Launch에 닿아야 한다(ML4b). 그런 대상 실행 파일이 둘이거나 엔진 버전이 없으면
        // 거부한다(manifest·빌드 정보는 타깃 하나 몫이다).
        Rejects(modular with
        {
            Modules = modules.Select(module => module.Name == "App" ? module with { PublicDependencies = ["Lib"] } : module).ToArray()
        }, "must reach the launch module", "a Modular executable that does not reach Launch");
        string otherSource = Write("other/main.cpp", "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Other)\nint main() { return 0; }\n");
        NativeModuleInput[] twoExecutables =
            [.. modules, new() { Name = "Other", Kind = NativeModuleKind.Executable, Sources = [otherSource], PublicDependencies = ["Launch"] }];
        Rejects(modular with { Modules = twoExecutables, Targets = ["App", "Other"] }, "targets reach App, Other", "two Modular executables");
        Rejects(modular with { Modules = twoExecutables, Targets = [] }, "targets reach App, Other", "two Modular executables through every target");
        Rejects(modular with { EngineVersion = null }, "needs NativeBuildRequest.EngineVersion", "a Modular executable without an engine version");
        pass("modular manifest: an executable without Launch, two executables and a missing engine version are rejected");

        // 플러그인 모듈(Docs/Design/ModuleLoader 3.15절 ML5a): 켠 플러그인 모듈은 플러그인 binaries 폴더에 링크하고 플러그인마다 manifest를
        // 쓰며, 실행 파일 manifest가 그 목록(plugins)을 싣는다. 행 order는 Monolithic 표 순서다. 호스트는 켠 플러그인 모듈(PluginModules)을
        // 링크하지 않고 직접 의존하는 플러그인 모듈(Plug)만 import한다. Plug는 bin의 Lib를, 호스트는 플러그인 폴더의 Plug를 import한다.
        Write("plug/include/plug/plug.h", "#pragma once\nROCKS_PLUG_API int PlugValue() noexcept;\n");
        string plugSource = Write("plug/plug.cpp",
            "#include <Core/Module/ModuleEntry.h>\n#include <lib/lib.h>\n#include <plug/plug.h>\nROCKS_IMPLEMENT_MODULE(Unused, Plug)\n" +
            "int PlugValue() noexcept { return LibValue() - 2; }\n");
        string optSource = Write("opt/opt.cpp", "#include <Core/Module/ModuleEntry.h>\nROCKS_IMPLEMENT_MODULE(Unused, Opt)\n");
        string pluginMain = Write("app/plugin-main.cpp", """
            #include <Core/Module/ModuleEntry.h>
            #include <plug/plug.h>

            ROCKS_IMPLEMENT_MODULE(Unused, App)

            extern "C" FIXTURE_EXPORT int RocksApplicationDescription() noexcept;
            extern "C" FIXTURE_EXPORT int RocksApplicationDescription() noexcept {
                return PlugValue() == 5 ? 0 : 1;
            }

            """);
        NativeModuleInput[] pluginModules =
        [
            .. modules.Where(module => module.Name is not "App" and not "Dyn"),
            new()
            {
                Name = "Plug", Sources = [plugSource], PublicIncludeDirectories = [Path.Combine(fixture, "plug", "include")], PublicDependencies = ["Lib"],
                Plugin = new("Plug", "Group/Plug")
            },
            new() { Name = "Opt", Sources = [optSource], PublicDependencies = ["Core"], Plugin = new("Opt", "Opt"), LoadingPhase = LoadingPhase.PostDefault },
            new()
            {
                Name = "App", Kind = NativeModuleKind.Executable, Sources = [pluginMain], PublicDependencies = ["Plug", "Launch"], PluginModules = ["Plug", "Opt"]
            }
        ];
        NativeBuildRequest plugged = modular with { OutputDirectory = Path.Combine(fixture, "out-plugins"), Modules = pluginModules };
        NativeBuildResult withPlugins = await builder.BuildAsync(plugged);
        Check(withPlugins.Succeeded, "Modular plugin fixture build failed: " + Failure(withPlugins));
        NativePlatform platform = request.Toolchain.Platform;
        string pluggedBin = Path.Combine(plugged.OutputDirectory, "bin");
        string PluginFolder(string folder) =>
            Path.Combine([plugged.OutputDirectory, "Plugins", .. folder.Split('/'), "Binaries", LinkTypes.PlatformDirectory(platform)]);
        string Library(string module, string folder) =>
            Path.Combine(folder, LinkTypes.SharedLibraryFileName("Fixture", module, platform, NativeConfiguration.Debug));
        string plugLibrary = Library("Plug", PluginFolder("Group/Plug"));
        string optLibrary = Library("Opt", PluginFolder("Opt"));
        Check(File.Exists(plugLibrary) && File.Exists(optLibrary) && !File.Exists(Library("Plug", pluggedBin)),
            "plugin modules link into their plugin binaries folders");
        var orders = new Dictionary<string, int>(StringComparer.Ordinal);
        string pluggedManifest = Path.Combine(pluggedBin, manifestName);
        string pluggedId = BuildId(pluggedManifest);
        using (JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(pluggedManifest)))
        {
            JsonElement document = manifest.RootElement;
            (string, string)[] listed = [.. document.GetProperty("plugins").EnumerateArray()
                .Select(entry => (entry.GetProperty("name").GetString()!, entry.GetProperty("manifest").GetString()!))];
            string platformFolder = LinkTypes.PlatformDirectory(platform);
            Check(listed.SequenceEqual([("Opt", $"../Plugins/Opt/Binaries/{platformFolder}/{manifestName}"),
                ("Plug", $"../Plugins/Group/Plug/Binaries/{platformFolder}/{manifestName}")]),
                "the executable manifest lists the plugin manifests by name: " + string.Join(", ", listed));
            foreach (JsonElement row in document.GetProperty("modules").EnumerateArray())
            {
                orders[row.GetProperty("name").GetString()!] = row.GetProperty("order").GetInt32();
            }
        }

        Check(!orders.ContainsKey("Plug") && !orders.ContainsKey("Opt"), "the executable manifest leaves out the plugin module rows");
        foreach ((string module, string folder) in new[] { ("Plug", PluginFolder("Group/Plug")), ("Opt", PluginFolder("Opt")) })
        {
            using JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, manifestName)));
            JsonElement document = manifest.RootElement;
            JsonElement[] rows = [.. document.GetProperty("modules").EnumerateArray()];
            Check(document.GetProperty("buildId").GetString() == pluggedId && document.GetProperty("plugins").GetArrayLength() == 0 &&
                rows.Length == 1 && rows[0].GetProperty("name").GetString() == module && !rows[0].GetProperty("host").GetBoolean() &&
                new FileInfo(Library(module, folder)).Length == rows[0].GetProperty("size").GetInt64(),
                "plugin manifest of " + module + ": same build id, no plugins, its own row and file size");
            orders[module] = rows[0].GetProperty("order").GetInt32();
        }

        // Monolithic 표 순서: App의 링크 폐포(Core ← Lib ← Plug, Launch), 켠 플러그인 모듈(Opt), 실행 파일.
        Check(orders.OrderBy(pair => pair.Value).Select(pair => pair.Key).SequenceEqual(["Core", "Lib", "Plug", "Launch", "Opt", "App"]) &&
            orders.Values.Order().SequenceEqual(Enumerable.Range(0, orders.Count)),
            "row orders follow the Monolithic module table: " + string.Join(", ", orders.OrderBy(pair => pair.Value).Select(pair => pair.Key + "=" + pair.Value)));
        string[] pluggedHostImports = await FixtureImports(request, Library("App", pluggedBin));
        Check(pluggedHostImports.Contains(Path.GetFileName(plugLibrary), StringComparer.OrdinalIgnoreCase) &&
            !pluggedHostImports.Contains(Path.GetFileName(optLibrary), StringComparer.OrdinalIgnoreCase),
            "the host imports the plugin module it depends on but not the other enabled plugin modules: " + string.Join(", ", pluggedHostImports));
        // 적재: Windows는 로더처럼 order 순서로 먼저 연 라이브러리가 다른 폴더의 import를 푼다. macOS·Linux는 rpath로 호스트 하나만 연다.
        var handles = new List<IntPtr>();
        try
        {
            string[] preload = platform == NativePlatform.Windows
                ? [Library("Core", pluggedBin), Library("Lib", pluggedBin), plugLibrary, Library("Launch", pluggedBin)]
                : [];
            foreach (string library in preload)
            {
                handles.Add(NativeLibrary.Load(library));
            }

            IntPtr host = NativeLibrary.Load(Library("App", pluggedBin));
            handles.Add(host);
            Check(NativeLibrary.TryGetExport(host, "RocksApplicationDescription", out IntPtr description) &&
                Marshal.GetDelegateForFunctionPointer<HostCheck>(description)() == 0,
                "the host module calls the plugin module in another folder, which calls a module in bin");
        }
        finally
        {
            handles.Reverse();
            handles.ForEach(NativeLibrary.Free);
        }

        NativeBuildResult pluggedAgain = await builder.BuildAsync(plugged);
        Check(pluggedAgain.Succeeded && pluggedAgain.ExecutedActions == 0 && BuildId(pluggedManifest) == pluggedId &&
            BuildId(Path.Combine(PluginFolder("Opt"), manifestName)) == pluggedId, "an unchanged rebuild keeps every manifest and its build id");
        // 끈 플러그인(Opt)의 옛 폴더는 남지만 실행 파일 manifest가 가리키지 않는다.
        NativeBuildResult withoutOpt = await builder.BuildAsync(plugged with
        {
            Modules = [.. pluginModules.Where(module => module.Name is not "Opt" and not "App"),
                pluginModules.Single(module => module.Name == "App") with { PluginModules = ["Plug"] }]
        });
        using (JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(pluggedManifest)))
        {
            Check(withoutOpt.Succeeded && File.Exists(Path.Combine(PluginFolder("Opt"), manifestName)) &&
                manifest.RootElement.GetProperty("plugins").EnumerateArray().Select(entry => entry.GetProperty("name").GetString())
                    .SequenceEqual(["Plug"]), "a disabled plugin's old folder stays but the executable manifest no longer lists it");
        }

        pass("modular manifest: plugin binaries folders, plugin manifests listed by the executable manifest, Monolithic row order, host links only " +
            "the plugin modules it depends on");

        // 입력 검사: 플러그인 폴더 모양, 플러그인 모듈이 있을 때 모듈 표 행이 없는 공유 라이브러리는 Launch 링크 폐포 안이어야 한다.
        Rejects(plugged with
        {
            Modules = [.. pluginModules.Select(module => module.Name == "Opt" ? module with { Plugin = new("Opt", "../Opt") } : module)]
        }, "must have a C identifier name and a relative folder", "a plugin folder outside the plugin root");
        Rejects(plugged with
        {
            Modules = [.. pluginModules.Select(module => module.Name == "Opt" ? module with { Plugin = new("Plug", "Opt") } : module)]
        }, "has modules in two folders", "one plugin in two folders");
        Rejects(plugged with
        {
            Modules = [.. pluginModules.Select(module => module.Name == "Opt" ? module with { Plugin = new("Opt", "Group/Plug") } : module)]
        }, "share the folder", "two plugins in one folder");
        string rowless = Write("rowless/rowless.cpp", "int RowlessValue() noexcept { return 1; }\n");
        // Plug가 의존해 빌드 대상에서 닿는 행 없는 모듈이다(닿지 않는 모듈은 보지 않는다).
        Rejects(plugged with
        {
            Modules = [.. pluginModules.Select(module => module.Name == "Plug" ? module with { PublicDependencies = ["Lib", "Rowless"] } : module),
                new() { Name = "Rowless", Sources = [rowless] }]
        },
            "without a module table row must be in the link closure of the launch module", "a row-less shared library outside the boot closure");
        pass("modular manifest: plugin folders and row-less shared libraries are validated");

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

    // 호스트 모듈이 앱 기술 자리에 export한 픽스처 검사 함수다.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int HostCheck();

    // 바이너리가 import하는 픽스처 공유 라이브러리 이름(Fixture- 접두)이다. Windows는 llvm-readobj(컴파일러 옆), macOS는 otool -L, 그 밖은
    // readelf -d로 읽는다.
    private static async Task<string[]> FixtureImports(NativeBuildRequest request, string binary)
    {
        NativePlatform platform = request.Toolchain.Platform;
        string tool = platform == NativePlatform.Windows
            ? Path.Combine(Path.GetDirectoryName(request.Toolchain.CompilerPath)!, "llvm-readobj.exe")
            : platform == NativePlatform.MacOS ? "otool" : "readelf";
        string[] arguments = platform == NativePlatform.Windows ? ["--coff-imports", binary] : platform == NativePlatform.MacOS ? ["-L", binary] : ["-d", binary];
        ProcessResult listed = await ProcessRunner.RunAsync(new(tool, arguments, Path.GetDirectoryName(binary)!, request.Toolchain.Environment));
        Check(listed.ExitCode == 0, $"{tool} failed for {binary}: {listed.StdErr}");
        return System.Text.RegularExpressions.Regex.Matches(listed.StdOut, @"(?:lib)?Fixture-[A-Za-z0-9_-]+\.(?:dll|dylib|so)")
            .Select(match => match.Value).Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.Ordinal).ToArray();
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
