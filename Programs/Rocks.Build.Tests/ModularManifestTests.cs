using System.Runtime.InteropServices;
using System.Text.Json;
using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Tooling.Common;

/// <summary>
/// Modular 빌드 기반(Docs/Design/BuildTargets.ko.md 3.8절 BS8d1)과 실행 파일 연결(Docs/Design/ModuleLoader.ko.md 3.14절 ML4b)을 실제
/// 컴파일·링크로 검사한다. 픽스처: 가짜 Core(모듈 표·빌드 정보 제공, 최소 진입 헤더) ← Lib·가짜 Launch(Modular 진입 C 함수) ← App(실행 파일
/// 모듈 — Modular에서 호스트 공유 라이브러리), App의 동적 모듈 Dyn.
/// <list type="bullet">
/// <item>빌드 정보 TU는 Core 공유 라이브러리에 들어가 export되고 호스트 모듈이 부른다. 실행 파일 모듈 표 TU는 없다.</item>
/// <item>실행 파일은 생성 진입 TU와 Launch만 링크해 Launch의 Modular 진입을 manifest 이름과 함께 부르고, import하는 픽스처 공유 라이브러리는
/// Launch뿐이다. 호스트 공유 라이브러리는 앱 기술 함수를 export하고 동적 모듈을 import하지 않는다. Windows 창 subsystem은 창 진입을 부른다.</item>
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
                    .SequenceEqual(["schema", "buildId", "engineVersion", "toolchainFingerprint", "modules"]), "manifest keys");
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
                        .SequenceEqual(["name", "file", "size", "dependencies", "loadingPhase", "onRequest", "host"]), "row keys of " + name);
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
