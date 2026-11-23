#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <Core/Text/StringBuilder.h>
#include <ModuleLoader/ModuleLoader.h>
#include <ModuleLoader/Private/ModuleManifest.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <TestSupport/DynamicLibraryFixture.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;

// Modular 모듈 manifest 적재(Docs/Design/ModuleLoader.ko.md 3.13·3.15절, ML4a·ML5a)를 검사한다.
// 해석·검사의 양성과 키·형식·의존 위반, 실행 파일 행만 있는 manifest로 만든 로더(OS 적재 없이 성공
// 경로), 그리고 Prepare의 실패 경로(파일 없음·크기 다름·의존 DLL 없음·진입점 없음)를 본다. OS 적재
// 실패 경로는 실행 중에 쓴 데이터 전용 시험 라이브러리(TestSupport/DynamicLibraryFixture.h)를
// 쓰므로 그 라이브러리를 만들 수 없는 플랫폼에서는 SKIP이다. 진입점을 부르는 성공 경로는 코드가
// 있는 공유 라이브러리가 필요해 Modular FoundationTests(빌드 타깃 BS8d5)가 본다. 플러그인
// manifest(plugins 목록· 머리 대조·order 정렬·플러그인 폴더의 행 파일)도 본다 — 실제 플러그인
// 적재는 EngineModular가 본다.
namespace module_manifest_tests_file {

using namespace rocks::platform;
using namespace rocks::test;

namespace {

constexpr const char* ValidBuildId = "0123456789abcdef0123456789abcdef";
constexpr const char* ValidFingerprint =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

const ModuleEntry& RunnerEntry() noexcept {
    static constexpr ModuleEntry entry =
        rocks::detail::MakeModuleEntry<DefaultModuleImplementation>("Runner");
    return entry;
}

// 머리(schema·buildId·engineVersion·toolchainFingerprint), plugins(JSON 배열 원소들)와 rows(JSON
// 배열 원소들)로 manifest 본문을 만든다.
void ManifestWith(char* out, usize capacity, const char* buildId, const char* engine,
                  const char* fingerprint, const char* plugins, const char* rows) noexcept {
    snprintf(out, capacity,
             "{\"schema\": 4, \"buildId\": \"%s\", \"engineVersion\": \"%s\", "
             "\"toolchainFingerprint\": \"%s\", \"plugins\": [%s], \"modules\": [%s]}",
             buildId, engine, fingerprint, plugins, rows);
}

void Manifest(char* out, usize capacity, const char* rows, const char* plugins = "") noexcept {
    ManifestWith(out, capacity, ValidBuildId, "0.1.0", ValidFingerprint, plugins, rows);
}

// 행 하나의 JSON이다. order는 Monolithic 표 순서 번호다(3.15절).
void Row(char* out, usize capacity, const char* name, const char* file, uint64 size,
         const char* dependencies, const char* phase, bool host, uint64 order = 0) noexcept {
    snprintf(out, capacity,
             "{\"name\": \"%s\", \"order\": %llu, \"file\": \"%s\", \"size\": %llu, "
             "\"dependencies\": [%s], \"loadingPhase\": \"%s\", \"onRequest\": false, "
             "\"host\": %s}",
             name, static_cast<unsigned long long>(order), file,
             static_cast<unsigned long long>(size), dependencies, phase, host ? "true" : "false");
}

// 플러그인 manifest 목록 항목 하나의 JSON이다.
void PluginEntry(char* out, usize capacity, const char* name, const char* path) noexcept {
    snprintf(out, capacity, "{\"name\": \"%s\", \"manifest\": \"%s\"}", name, path);
}

bool Parse(const char* text, rocks::detail::ModuleManifestData& data,
           ModuleLoadReport& report) noexcept {
    return rocks::detail::ParseModuleManifest(StringView(text), data, report);
}

uint32 NextSerial() noexcept {
    static uint32 serial = 0;
    return AtomicFetchAdd<MemoryOrder::Relaxed>(&serial, 1u) + 1u;
}

// 현재 디렉터리 아래의 작업 디렉터리다. 소멸 때 지운다(시험은 그 전에 로더·라이브러리를 닫는다).
struct Scratch {
    char path[1024]{};
    usize length = 0;

    bool Make() noexcept {
        char base[512];
        usize bytes = 0;
        if (!GetStandardDirectory(StandardDirectory::Working, base, sizeof(base), bytes)) {
            return false;
        }

        const int count =
            snprintf(path, sizeof(path), "%s/rocks-manifest-%llu-%u", base,
                     static_cast<unsigned long long>(MonotonicNanoseconds()), NextSerial());
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path)) {
            return false;
        }

        length = static_cast<usize>(count);
        if (!MakeDirectoryTree(PhysicalFileSystem(), path, length)) {
            length = 0;
            return false;
        }

        return true;
    }

    bool Child(const char* name, char* out, usize capacity) const noexcept {
        const int count = snprintf(out, capacity, "%s/%s", path, name);
        return count > 0 && static_cast<usize>(count) < capacity;
    }

    ~Scratch() noexcept {
        if (length != 0) {
            (void)DeleteDirectoryTree(PhysicalFileSystem(), path, length);
        }
    }
};

bool WriteBytes(const char* path, const char* content, usize bytes) noexcept {
    File file;
    if (!file.Open(PhysicalFileSystem(), path, strlen(path), FileOpenMode::CreateAlways)) {
        return false;
    }

    const auto written = file.WriteAll(content, bytes);
    return written.status && static_cast<bool>(file.Close());
}

uint64 FileSize(const char* path) noexcept {
    FileInfo info;
    return PhysicalFileSystem().Stat(path, strlen(path), info) ? info.size : 0;
}

// 작업 디렉터리에 호스트 행 파일(Fixture-Runner.dll — 내용은 아무 byte)과 manifest를 쓴다.
struct ManifestFolder {
    Scratch scratch;
    char manifest[1100]{};
    char runner[1100]{};

    bool Make(uint64 runnerSize) noexcept {
        if (!scratch.Make() || !scratch.Child("Fixture-Runner.dll", runner, sizeof(runner)) ||
            !WriteBytes(runner, "runner", 6) ||
            !scratch.Child("Fixture.modules.json", manifest, sizeof(manifest))) {
            return false;
        }

        char runnerRow[512];
        Row(runnerRow, sizeof(runnerRow), "Runner", "Fixture-Runner.dll", runnerSize, "", "None",
            true);
        char text[4096];
        Manifest(text, sizeof(text), runnerRow);
        return WriteBytes(manifest, text, strlen(text));
    }
};

} // namespace

// 바른 manifest: 행 순서·이름·파일·크기·의존(앞 행 이름을 가리킨다)·단계·표지가 그대로 담긴다.
TEST(ModuleManifest_ParsesRowsInOrder) {
    SystemAllocator allocator;
    {
        char base[512];
        char middle[512];
        char runner[512];
        Row(base, sizeof(base), "Core", "Fixture-Core.dll", 10, "", "EarliestPossible", false, 0);
        Row(middle, sizeof(middle), "Render", "Fixture-Render.dll", 20, "\"Core\"", "PreDefault",
            false, 1);
        Row(runner, sizeof(runner), "Runner", "Runner.exe", 30, "\"Core\", \"Render\"", "None",
            true, 2);
        char rows[2048];
        snprintf(rows, sizeof(rows), "%s, %s, %s", base, middle, runner);
        char text[4096];
        Manifest(text, sizeof(text), rows);
        rocks::detail::ModuleManifestData data(allocator);
        ModuleLoadReport report;
        REQUIRE(Parse(text, data, report));
        REQUIRE(data.rows.Size() == 3);
        CHECK(strcmp(data.rows[0].name, "Core") == 0 && data.rows[0].dependencyCount == 0);
        CHECK(data.rows[1].phase == LoadingPhase::PreDefault && data.rows[1].dependencyCount == 1 &&
              data.rows[1].dependencies[0] == data.rows[0].name);
        CHECK(data.rows[2].dependencyCount == 2 &&
              data.rows[2].dependencies[1] == data.rows[1].name &&
              data.rows[2].phase == LoadingPhase::None);
        CHECK(strcmp(data.files[1], "Fixture-Render.dll") == 0 && data.sizes[1] == 20 &&
              data.sizes[2] == 30);
        CHECK(!data.host[0] && !data.host[1] && data.host[2]);
        CHECK(data.rows[0].entry == nullptr && data.entries[0] == nullptr);
        CHECK(data.plugins[0] == nullptr && data.plugins[2] == nullptr);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 형식 위반은 모두 Incompatible(단계 "Manifest")이고 문구가 그 이유를 말한다: JSON 아님, 최상위 키
// 빠짐·더함, schema·buildId·지문·엔진 버전 모양, plugins 형식·읽은 플러그인 manifest와 다른 목록,
// 빈 modules, 행 키 빠짐·더함, 이름·파일 이름·크기·단계·표지·order 형식, 커지지 않는 order, 뒤
// 행이나 없는 행을 가리키는 의존, 중복 의존, 같은 이름 두 행, 호스트 행 둘.
TEST(ModuleManifest_RejectsMalformedManifests) {
    SystemAllocator allocator;
    char good[512];
    Row(good, sizeof(good), "Core", "Core.dll", 1, "", "Default", false);
    char again[512];
    Row(again, sizeof(again), "Core", "Core.dll", 1, "", "Default", false, 1);
    const char* header = "\"schema\": 4, \"buildId\": \"0123456789abcdef0123456789abcdef\", "
                         "\"engineVersion\": \"0.1.0\", \"toolchainFingerprint\": "
                         "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\", "
                         "\"plugins\": []";
    // 행 하나의 키·값만 바꾼 행이다(나머지는 바른 값).
    auto row = [](char* out, usize capacity, const char* name, const char* file, const char* size,
                  const char* dependencies, const char* phase, const char* onRequest,
                  const char* extra) {
        snprintf(out, capacity,
                 "{\"name\": \"%s\", \"order\": 1, \"file\": \"%s\", \"size\": %s, "
                 "\"dependencies\": [%s], \"loadingPhase\": \"%s\", \"onRequest\": %s%s}",
                 name, file, size, dependencies, phase, onRequest, extra);
    };
    struct Case {
        char text[4096];
        const char* reason;
    };
    static Case cases[32];
    usize count = 0;
    auto add = [&](const char* reason) -> char* {
        cases[count].reason = reason;
        return cases[count++].text;
    };
    auto withRows = [&](const char* reason, const char* rows) {
        snprintf(add(reason), sizeof(cases[0].text), "{%s, \"modules\": [%s]}", header, rows);
    };
    auto withHeader = [&](const char* reason, const char* schema, const char* buildId,
                          const char* engine, const char* fingerprint) {
        snprintf(add(reason), sizeof(cases[0].text),
                 "{\"schema\": %s, \"buildId\": \"%s\", \"engineVersion\": \"%s\", "
                 "\"toolchainFingerprint\": \"%s\", \"plugins\": [], \"modules\": [%s]}",
                 schema, buildId, engine, fingerprint, good);
    };
    const char* id = "0123456789abcdef0123456789abcdef";
    const char* print = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    snprintf(add("not valid JSON"), sizeof(cases[0].text), "not json");
    snprintf(add("exactly the keys schema"), sizeof(cases[0].text),
             "{\"buildId\": \"x\", \"modules\": [%s]}", good);
    snprintf(add("exactly the keys schema"), sizeof(cases[0].text),
             "{%s, \"modules\": [%s], \"extra\": 1}", header, good);
    withHeader("schema must be", "3", id, "0.1.0", print);
    withHeader("buildId must be", "4", "0123456789ABCDEF0123456789abcdef", "0.1.0", print);
    withHeader("engineVersion must be", "4", id, "", print);
    withHeader("toolchainFingerprint must be", "4", id, "0.1.0", "0123");
    withRows("modules must be a non-empty array", "");
    // plugins 키: 배열이 아님, 읽지 않은 플러그인 manifest를 가리킴(이 시험은 실행 파일 manifest만
    // 준다).
    snprintf(add("plugins must be an array"), sizeof(cases[0].text),
             "{\"schema\": 4, \"buildId\": \"%s\", \"engineVersion\": \"0.1.0\", "
             "\"toolchainFingerprint\": \"%s\", \"plugins\": {}, \"modules\": [%s]}",
             id, print, good);
    char plugin[256];
    PluginEntry(plugin, sizeof(plugin), "Extra", "../Plugins/Extra/Binaries/Win64/F.modules.json");
    snprintf(add("lists 1 plugin manifests but 0 were read"), sizeof(cases[0].text),
             "{\"schema\": 4, \"buildId\": \"%s\", \"engineVersion\": \"0.1.0\", "
             "\"toolchainFingerprint\": \"%s\", \"plugins\": [%s], \"modules\": [%s]}",
             id, print, plugin, good);
    char rows[2048];
    char text[1024];
    snprintf(text, sizeof(text),
             "{\"name\": \"Core\", \"order\": 0, \"file\": \"Core.dll\", \"size\": 1, "
             "\"dependencies\": [], \"loadingPhase\": \"Default\", \"onRequest\": false}");
    withRows("must have exactly the keys name", text);
    row(text, sizeof(text), "Core", "Core.dll", "1", "", "Default", "false",
        ", \"host\": false, \"extra\": 0");
    withRows("must have exactly the keys name", text);
    // order: 음수, 앞 행보다 크지 않음(같은 manifest 안).
    snprintf(text, sizeof(text),
             "{\"name\": \"Core\", \"order\": -1, \"file\": \"Core.dll\", \"size\": 1, "
             "\"dependencies\": [], \"loadingPhase\": \"Default\", \"onRequest\": false, "
             "\"host\": false}");
    withRows("order must be a non-negative integer", text);
    snprintf(rows, sizeof(rows), "%s, %s", again, good);
    withRows("greater than the previous row's order", rows);
    row(text, sizeof(text), "1Core", "Core.dll", "1", "", "Default", "false", ", \"host\": false");
    withRows("is not a C identifier", text);
    row(text, sizeof(text), "Core", "sub/Core.dll", "1", "", "Default", "false",
        ", \"host\": false");
    withRows("must be a plain file name", text);
    row(text, sizeof(text), "Core", "..", "1", "", "Default", "false", ", \"host\": false");
    withRows("must be a plain file name", text);
    row(text, sizeof(text), "Core", "Core.dll", "-1", "", "Default", "false", ", \"host\": false");
    withRows("size must be a non-negative integer", text);
    row(text, sizeof(text), "Core", "Core.dll", "1.5", "", "Default", "false", ", \"host\": false");
    withRows("size must be a non-negative integer", text);
    row(text, sizeof(text), "Core", "Core.dll", "1", "", "Later", "false", ", \"host\": false");
    withRows("loadingPhase must be", text);
    row(text, sizeof(text), "Core", "Core.dll", "1", "", "Default", "0", ", \"host\": false");
    withRows("onRequest and host must be", text);
    row(text, sizeof(text), "Core", "Core.dll", "1", "\"Later\"", "Default", "false",
        ", \"host\": false");
    withRows("must name an earlier module", text);
    row(text, sizeof(text), "Render", "Render.dll", "1", "\"Core\", \"Core\"", "Default", "false",
        ", \"host\": false");
    snprintf(rows, sizeof(rows), "%s, %s", good, text);
    withRows("lists dependency Core twice", rows);
    snprintf(rows, sizeof(rows), "%s, %s", good, again);
    withRows("lists module Core twice", rows);
    char runner[512];
    Row(runner, sizeof(runner), "Runner", "Fixture-Runner.dll", 1, "", "None", true, 0);
    char second[512];
    Row(second, sizeof(second), "Tool", "Fixture-Tool.dll", 1, "", "None", true, 1);
    snprintf(rows, sizeof(rows), "%s, %s", runner, second);
    withRows("more than one host row", rows);
    for (usize index = 0; index < count; ++index) {
        rocks::detail::ModuleManifestData data(allocator);
        ModuleLoadReport report;
        const bool parsed = Parse(cases[index].text, data, report);
        const bool reason = strstr(report.message, cases[index].reason) != nullptr;
        CHECK(!parsed);
        CHECK(report.code == ModuleLoadCode::Incompatible);
        CHECK(strcmp(report.stage, "Manifest") == 0);
        CHECK(reason);
        if (parsed || !reason) {
            printf("manifest #%llu (expected '%s'): %s -> %s\n",
                   static_cast<unsigned long long>(index), cases[index].reason, cases[index].text,
                   report.message);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// OS 적재 전에 끝나는 실패: 호스트 행 파일 크기가 다르면 Incompatible(Lookup), manifest가 없으면
// NotFound(requested는 파일 이름), 상대 경로는 Incompatible이다. 호스트 행도 OS로 싣으므로(3.14절)
// 로더 생성 성공 경로는 실제 모듈 라이브러리가 있는 BS8d5의 Modular FoundationTests가 본다.
TEST(ModuleManifest_CreateFailsBeforeLoading) {
    SystemAllocator allocator;
    {
        ManifestFolder folder;
        REQUIRE(folder.Make(7));
        ModuleLoadReport report;
        auto created =
            ModuleLoader::TryCreateFromManifest(allocator, StringView(folder.manifest), report);
        CHECK(!created && created.Error() == ErrorCode::InvalidArgument);
        CHECK(report.code == ModuleLoadCode::Incompatible && strcmp(report.stage, "Lookup") == 0 &&
              strcmp(report.failed, "Runner") == 0);
        CHECK(strstr(report.message, "is 6 bytes but the manifest expects 7") != nullptr);
    }

    {
        ManifestFolder folder;
        REQUIRE(folder.Make(6));
        char missing[1100];
        REQUIRE(folder.scratch.Child("Absent.modules.json", missing, sizeof(missing)));
        ModuleLoadReport report;
        auto created = ModuleLoader::TryCreateFromManifest(allocator, StringView(missing), report);
        CHECK(!created && created.Error() == ErrorCode::NotFound &&
              report.code == ModuleLoadCode::NotFound);
        CHECK(strcmp(report.requested, "Absent.modules.json") == 0);
    }

    {
        // 상대 경로는 작업 폴더에 기대므로 받지 않는다(Incompatible, InvalidArgument).
        ModuleLoadReport report;
        auto created =
            ModuleLoader::TryCreateFromManifest(allocator, "bin/Fixture.modules.json", report);
        CHECK(!created && created.Error() == ErrorCode::InvalidArgument &&
              report.code == ModuleLoadCode::Incompatible &&
              strstr(report.message, "not an absolute file path") != nullptr);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 정적 표 로더(TryCreate — Monolithic)에는 호스트 라이브러리가 없으므로 FindHostSymbol은 false와
// null 주소다.
TEST(ModuleManifest_FindHostSymbolNeedsManifestLoader) {
    SystemAllocator allocator;
    {
        static constexpr ModuleTableRow rows[] = {
            {"Runner", &RunnerEntry, nullptr, 0, LoadingPhase::None, false},
        };
        auto created = ModuleLoader::TryCreate(allocator, ModuleTable(rows, 1));
        REQUIRE(created);
        void* address = reinterpret_cast<void*>(&allocator);
        DynamicLibraryLease lease;
        CHECK(!created.Value()->FindHostSymbol("RocksApplicationDescription", address, lease));
        CHECK(address == nullptr);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Prepare의 실패 경로(OS 적재): 모듈 파일 없음(NotFound/Lookup), 크기 다름(Incompatible/Lookup),
// 의존 DLL 없음(OsLoadFailed/OsLoad, nativeError), 진입점 없음(EntryMissing/Entry). 진입점이 없는
// 라이브러리는 열렸다가 실패 처리에서 닫힌다.
TEST(ModuleManifest_PrepareReportsLoadFailures) {
    if (!DynamicLibraryFixturesSupported()) {
        printf("SKIP ModuleManifest_PrepareReportsLoadFailures: no fixture libraries on this "
               "platform\n");
        return;
    }

    SystemAllocator allocator;
    const uint32 serial = NextSerial();
    char plainName[64];
    char importerName[64];
    char absentName[64];
    snprintf(plainName, sizeof(plainName), "RocksManifestPlain%u%s", serial,
             DynamicLibraryExtension());
    snprintf(importerName, sizeof(importerName), "RocksManifestImporter%u%s", serial,
             DynamicLibraryExtension());
    snprintf(absentName, sizeof(absentName), "RocksManifestAbsent%u%s", serial,
             DynamicLibraryExtension());

    // 코드 없는 데이터 라이브러리: plain은 RocksModuleEntry를 export하지 않고, importer는 없는
    // DLL을 import한다.
    const FixtureExport exports[] = {{"RocksValue", 7u}};
    FixtureLibrary plain;
    plain.exports = exports;
    plain.exportCount = 1;
    FixtureLibrary importer = plain;
    importer.importLibrary = absentName;
    importer.importSymbol = "RocksValue";
    importer.importSlotExport = "RocksImport";

    struct Case {
        const char* file;
        bool write;
        const FixtureLibrary* library;
        bool wrongSize;
        ModuleLoadCode code;
        const char* stage;
    };
    const Case cases[] = {
        {"RocksManifestMissing.dll", false, nullptr, false, ModuleLoadCode::NotFound, "Lookup"},
        {plainName, true, &plain, true, ModuleLoadCode::Incompatible, "Lookup"},
        {importerName, true, &importer, false, ModuleLoadCode::OsLoadFailed, "OsLoad"},
        {plainName, true, &plain, false, ModuleLoadCode::EntryMissing, "Entry"},
    };
    for (const Case& test : cases) {
        Scratch scratch;
        REQUIRE(scratch.Make());
        char library[1100];
        REQUIRE(scratch.Child(test.file, library, sizeof(library)));
        if (test.write) {
            REQUIRE(WriteFixtureLibrary(library, *test.library));
        }

        const uint64 size = test.write ? FileSize(library) + (test.wrongSize ? 1u : 0u) : 1u;
        char row[512];
        Row(row, sizeof(row), "Probe", test.file, size, "", "Default", false);
        char text[2048];
        Manifest(text, sizeof(text), row);
        char manifest[1100];
        REQUIRE(scratch.Child("Fixture.modules.json", manifest, sizeof(manifest)));
        REQUIRE(WriteBytes(manifest, text, strlen(text)));
        ModuleLoadReport report;
        {
            auto created =
                ModuleLoader::TryCreateFromManifest(allocator, StringView(manifest), report);
            CHECK(!created);
        }

        CHECK(report.code == test.code);
        CHECK(strcmp(report.stage, test.stage) == 0);
        CHECK(strcmp(report.failed, "Probe") == 0);
        CHECK(report.message[0] != '\0');
        if (test.code == ModuleLoadCode::OsLoadFailed) {
            CHECK(report.nativeError != 0);
        }

        // 실패한 Prepare는 연 라이브러리를 닫는다(Windows는 올라온 DLL을 지울 수 없으므로 Scratch
        // 정리도 이에 기댄다).
        CHECK(!LibraryIsLoaded(test.file));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 실행 파일 manifest의 plugins 목록(3.15절): 바른 목록은 이름·경로를 순서대로 담고, 항목 키·이름·
// 중복·경로 모양 위반은 Incompatible(단계 "Manifest")이다. 경로는 "../Plugins/<조각>/…/Binaries/
// <조각>/<실행 파일 manifest 파일 이름>"만 받는다.
TEST(ModuleManifest_ListsPluginManifests) {
    SystemAllocator allocator;
    constexpr StringView fileName = "Game-Windows-Debug.modules.json";
    char good[256];
    char second[256];
    PluginEntry(good, sizeof(good), "Exchange",
                "../Plugins/Importers/Exchange/Binaries/Win64/Game-Windows-Debug.modules.json");
    PluginEntry(second, sizeof(second), "Tools",
                "../Plugins/Tools/Binaries/Win64/Game-Windows-Debug.modules.json");
    char rows[512];
    Row(rows, sizeof(rows), "Core", "Core.dll", 1, "", "Default", false);
    {
        char list[600];
        snprintf(list, sizeof(list), "%s, %s", good, second);
        char text[2048];
        Manifest(text, sizeof(text), rows, list);
        rocks::detail::ModuleManifestPlugins plugins(allocator);
        ModuleLoadReport report;
        REQUIRE(
            rocks::detail::ListModuleManifestPlugins(StringView(text), fileName, plugins, report));
        REQUIRE(plugins.Count() == 2);
        CHECK(plugins.Name(0) == "Exchange" && plugins.Name(1) == "Tools");
        CHECK(plugins.Path(1) == "../Plugins/Tools/Binaries/Win64/Game-Windows-Debug.modules.json");
    }

    {
        // 머리를 먼저 본다: JSON이 아니거나 schema가 틀린 본문은 목록을 읽기 전에 거부한다.
        rocks::detail::ModuleManifestPlugins plugins(allocator);
        ModuleLoadReport report;
        CHECK(!rocks::detail::ListModuleManifestPlugins("not json", fileName, plugins, report));
        CHECK(strstr(report.message, "not valid JSON") != nullptr);
        char text[2048];
        Manifest(text, sizeof(text), rows, good);
        char* schema = strstr(text, "\"schema\": 4");
        REQUIRE(schema != nullptr);
        schema[10] = '3';
        ModuleLoadReport header;
        CHECK(
            !rocks::detail::ListModuleManifestPlugins(StringView(text), fileName, plugins, header));
        CHECK(strstr(header.message, "schema must be 4") != nullptr && plugins.Count() == 0);
    }

    struct Case {
        const char* entry;
        const char* reason;
    };
    const Case cases[] = {
        {"{\"name\": \"Exchange\"}", "exactly the string keys name and manifest"},
        {"{\"name\": 1, \"manifest\": \"x\"}", "exactly the string keys name and manifest"},
        {"{\"name\": \"Exchange\", \"manifest\": \"x\", \"extra\": 0}",
         "exactly the string keys name and manifest"},
        {"{\"name\": \"1Exchange\", \"manifest\": \"x\"}", "is not a C identifier"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"/Plugins/Exchange/Binaries/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"Plugins/Exchange/Binaries/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"../Plugins/Exchange/Binaries/Win64/Other.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"../Plugins/Exchange/../Binaries/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"../Plugins/Binaries/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"../Plugins/Exchange/Bin/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"../Plugins/Exchange//Binaries/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"../Plugins/./Exchange/Binaries/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"../Plugins/Ex\\\\change/Binaries/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"../Plugins/C:/Binaries/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
        {"{\"name\": \"Exchange\", \"manifest\": "
         "\"../../Plugins/Exchange/Binaries/Win64/Game-Windows-Debug.modules.json\"}",
         "must be ../Plugins/"},
    };
    for (const Case& test : cases) {
        char text[2048];
        Manifest(text, sizeof(text), rows, test.entry);
        rocks::detail::ModuleManifestPlugins plugins(allocator);
        ModuleLoadReport report;
        const bool listed =
            rocks::detail::ListModuleManifestPlugins(StringView(text), fileName, plugins, report);
        const bool reason = strstr(report.message, test.reason) != nullptr;
        CHECK(!listed && report.code == ModuleLoadCode::Incompatible &&
              strcmp(report.stage, "Manifest") == 0);
        CHECK(reason);
        if (listed || !reason) {
            printf("plugins entry %s (expected '%s') -> %s\n", test.entry, test.reason,
                   report.message);
        }
    }

    {
        // 같은 플러그인 이름 둘.
        char list[600];
        snprintf(list, sizeof(list), "%s, %s", good, good);
        char text[2048];
        Manifest(text, sizeof(text), rows, list);
        rocks::detail::ModuleManifestPlugins plugins(allocator);
        ModuleLoadReport report;
        CHECK(
            !rocks::detail::ListModuleManifestPlugins(StringView(text), fileName, plugins, report));
        CHECK(strstr(report.message, "lists plugin Exchange twice") != nullptr);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 실행 파일 manifest 하나와 플러그인 manifest 둘(Exchange·Tools)의 바른 본문이다. order로 합치면
// Core(0) → Plug(1, Exchange) → Tool(2, Tools — Plug에 의존, 플러그인 사이 의존) → Runner(3, 호스트
// — Plug에 의존, 앞 manifest 행이 뒤 manifest 행에 의존)다.
struct PluginManifests {
    char executable[2048]{};
    char exchange[1024]{};
    char tools[1024]{};

    void Make() noexcept {
        char core[512];
        char runner[512];
        Row(core, sizeof(core), "Core", "Fixture-Core.dll", 1, "", "EarliestPossible", false, 0);
        Row(runner, sizeof(runner), "Runner", "Fixture-Runner.dll", 1, "\"Core\", \"Plug\"", "None",
            true, 3);
        char rows[1100];
        snprintf(rows, sizeof(rows), "%s, %s", core, runner);
        char exchangeEntry[256];
        char toolsEntry[256];
        PluginEntry(exchangeEntry, sizeof(exchangeEntry), "Exchange",
                    "../Plugins/Exchange/Binaries/Win64/Fixture.modules.json");
        PluginEntry(toolsEntry, sizeof(toolsEntry), "Tools",
                    "../Plugins/Tools/Binaries/Win64/Fixture.modules.json");
        char list[600];
        snprintf(list, sizeof(list), "%s, %s", exchangeEntry, toolsEntry);
        Manifest(executable, sizeof(executable), rows, list);
        char plug[512];
        Row(plug, sizeof(plug), "Plug", "Fixture-Plug.dll", 1, "\"Core\"", "Default", false, 1);
        Manifest(exchange, sizeof(exchange), plug);
        char tool[512];
        Row(tool, sizeof(tool), "Tool", "Fixture-Tool.dll", 1, "\"Plug\"", "Default", false, 2);
        Manifest(tools, sizeof(tools), tool);
    }
};

bool ParseAll(const char* executable, const char* exchange, const char* tools,
              rocks::detail::ModuleManifestData& data, ModuleLoadReport& report) noexcept {
    const rocks::detail::ModuleManifestText manifests[] = {
        {StringView(executable), "/out/bin", StringView()},
        {StringView(exchange), "/out/Plugins/Exchange/Binaries/Win64", "Exchange"},
        {StringView(tools), "/out/Plugins/Tools/Binaries/Win64", "Tools"},
    };
    return rocks::detail::ParseModuleManifests(
        Span<const rocks::detail::ModuleManifestText>(manifests), data, report);
}

} // namespace

// 플러그인 manifest를 합친 표: 행은 order 순서이고, 행마다 자기 manifest 폴더·플러그인 이름을
// 가지며, 의존은 다른 manifest의 행(플러그인 사이, 실행 파일 행 → 플러그인 행)도 가리킨다.
TEST(ModuleManifest_MergesPluginManifestsByOrder) {
    SystemAllocator allocator;
    {
        PluginManifests texts;
        texts.Make();
        rocks::detail::ModuleManifestData data(allocator);
        ModuleLoadReport report;
        REQUIRE(ParseAll(texts.executable, texts.exchange, texts.tools, data, report));
        REQUIRE(data.rows.Size() == 4);
        CHECK(strcmp(data.rows[0].name, "Core") == 0 && strcmp(data.rows[1].name, "Plug") == 0 &&
              strcmp(data.rows[2].name, "Tool") == 0 && strcmp(data.rows[3].name, "Runner") == 0);
        CHECK(strcmp(data.directories[0], "/out/bin") == 0 &&
              strcmp(data.directories[1], "/out/Plugins/Exchange/Binaries/Win64") == 0 &&
              strcmp(data.directories[2], "/out/Plugins/Tools/Binaries/Win64") == 0 &&
              strcmp(data.directories[3], "/out/bin") == 0);
        CHECK(data.plugins[0] == nullptr && strcmp(data.plugins[1], "Exchange") == 0 &&
              strcmp(data.plugins[2], "Tools") == 0 && data.plugins[3] == nullptr);
        CHECK(data.rows[2].dependencyCount == 1 &&
              data.rows[2].dependencies[0] == data.rows[1].name);
        CHECK(data.rows[3].dependencyCount == 2 &&
              data.rows[3].dependencies[1] == data.rows[1].name);
        CHECK(data.host[3] && !data.host[1]);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 플러그인 manifest 위반은 Incompatible(단계 "Manifest")이고, 그 manifest의 실패면 requested가
// 플러그인 이름이다: 머리 값(buildId·engineVersion·toolchainFingerprint)이 실행 파일 manifest와
// 다름, 호스트 행, 비지 않은 plugins, manifest 사이 같은 이름·같은 order, order가 더 큰 행에
// 의존(manifest 사이 순환 포함), 실행 파일 manifest의 plugins와 다른 이름.
TEST(ModuleManifest_RejectsMismatchedPluginManifests) {
    SystemAllocator allocator;
    PluginManifests base;
    base.Make();
    char plug[512];
    Row(plug, sizeof(plug), "Plug", "Fixture-Plug.dll", 1, "\"Core\"", "Default", false, 1);
    struct Case {
        char exchange[1024];
        const char* reason;
        const char* requested;
    };
    static Case cases[12];
    usize count = 0;
    auto add = [&](const char* reason, const char* requested) -> char* {
        cases[count].reason = reason;
        cases[count].requested = requested;
        return cases[count++].exchange;
    };
    ManifestWith(add("buildId 'ffffffffffffffffffffffffffffffff' differs", "Exchange"), 1024,
                 "ffffffffffffffffffffffffffffffff", "0.1.0", ValidFingerprint, "", plug);
    ManifestWith(add("engineVersion '0.2.0' differs", "Exchange"), 1024, ValidBuildId, "0.2.0",
                 ValidFingerprint, "", plug);
    ManifestWith(add("toolchainFingerprint", "Exchange"), 1024, ValidBuildId, "0.1.0",
                 "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", "", plug);
    char row[512];
    Row(row, sizeof(row), "Plug", "Fixture-Plug.dll", 1, "\"Core\"", "Default", true, 1);
    Manifest(add("must not have a host row", "Exchange"), 1024, row);
    char entry[256];
    PluginEntry(entry, sizeof(entry), "Tools", "../Plugins/Tools/Binaries/Win64/F.modules.json");
    Manifest(add("must have an empty plugins list", "Exchange"), 1024, plug, entry);
    Row(row, sizeof(row), "Core", "Fixture-Core2.dll", 1, "", "Default", false, 1);
    Manifest(add("lists module Core twice", "Exchange"), 1024, row);
    Row(row, sizeof(row), "Plug", "Fixture-Plug.dll", 1, "\"Core\"", "Default", false, 0);
    Manifest(add("order 0 is used by two modules", "Exchange"), 1024, row);
    // Plug(1)가 Tool(2)에 의존 — Tool은 Plug에 의존하므로 manifest 사이 순환이다.
    Row(row, sizeof(row), "Plug", "Fixture-Plug.dll", 1, "\"Tool\"", "Default", false, 1);
    Manifest(add("must name an earlier module", "Exchange"), 1024, row);
    for (usize index = 0; index < count; ++index) {
        rocks::detail::ModuleManifestData data(allocator);
        ModuleLoadReport report;
        const bool parsed =
            ParseAll(base.executable, cases[index].exchange, base.tools, data, report);
        const bool reason = strstr(report.message, cases[index].reason) != nullptr;
        CHECK(!parsed && report.code == ModuleLoadCode::Incompatible &&
              strcmp(report.stage, "Manifest") == 0);
        CHECK(reason);
        CHECK(strcmp(report.requested, cases[index].requested) == 0);
        if (parsed || !reason) {
            printf("plugin manifest #%llu (expected '%s'): %s\n",
                   static_cast<unsigned long long>(index), cases[index].reason, report.message);
        }
    }

    {
        // 실행 파일 manifest가 다른 이름을 적음(읽은 플러그인 manifest와 다르다).
        rocks::detail::ModuleManifestData data(allocator);
        ModuleLoadReport report;
        const rocks::detail::ModuleManifestText manifests[] = {
            {StringView(base.executable), "/out/bin", StringView()},
            {StringView(base.tools), "/out/Plugins/Tools/Binaries/Win64", "Tools"},
            {StringView(base.exchange), "/out/Plugins/Exchange/Binaries/Win64", "Exchange"},
        };
        CHECK(!rocks::detail::ParseModuleManifests(
            Span<const rocks::detail::ModuleManifestText>(manifests), data, report));
        CHECK(strstr(report.message, "plugins[0] must name plugin Tools") != nullptr);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 로더 생성(파일): 실행 파일 manifest가 가리키는 플러그인 manifest가 없으면 NotFound(단계
// "Manifest", requested는 플러그인 이름)이고, 있으면 그 행 파일을 플러그인 폴더에서 찾는다(크기
// 대조 실패 — Incompatible/Lookup, failed는 행 이름, requested는 플러그인 이름).
TEST(ModuleManifest_CreateReadsPluginManifests) {
    SystemAllocator allocator;
    {
        Scratch scratch;
        REQUIRE(scratch.Make());
        char bin[1100];
        char pluginFolder[1100];
        REQUIRE(scratch.Child("bin", bin, sizeof(bin)));
        REQUIRE(
            scratch.Child("Plugins/Exchange/Binaries/Win64", pluginFolder, sizeof(pluginFolder)));
        REQUIRE(MakeDirectoryTree(PhysicalFileSystem(), bin, strlen(bin)));
        REQUIRE(MakeDirectoryTree(PhysicalFileSystem(), pluginFolder, strlen(pluginFolder)));
        char runner[1100];
        snprintf(runner, sizeof(runner), "%s/Fixture-Runner.dll", bin);
        REQUIRE(WriteBytes(runner, "runner", 6));
        char plugFile[1100];
        snprintf(plugFile, sizeof(plugFile), "%s/Fixture-Plug.dll", pluginFolder);
        REQUIRE(WriteBytes(plugFile, "plug", 4));

        char entry[256];
        PluginEntry(entry, sizeof(entry), "Exchange",
                    "../Plugins/Exchange/Binaries/Win64/Fixture.modules.json");
        char runnerRow[512];
        Row(runnerRow, sizeof(runnerRow), "Runner", "Fixture-Runner.dll", 6, "\"Plug\"", "None",
            true, 1);
        char text[2048];
        Manifest(text, sizeof(text), runnerRow, entry);
        char manifest[1100];
        snprintf(manifest, sizeof(manifest), "%s/Fixture.modules.json", bin);
        REQUIRE(WriteBytes(manifest, text, strlen(text)));
        {
            ModuleLoadReport report;
            auto created =
                ModuleLoader::TryCreateFromManifest(allocator, StringView(manifest), report);
            CHECK(!created && created.Error() == ErrorCode::NotFound &&
                  report.code == ModuleLoadCode::NotFound);
            CHECK(strcmp(report.stage, "Manifest") == 0 &&
                  strcmp(report.requested, "Exchange") == 0);
        }

        {
            // 실행 파일 manifest의 머리가 틀리면 플러그인 파일을 열기 전에 그 오류다(requested는
            // 실행 파일 manifest 이름 — 없는 플러그인 파일이 원인을 가리지 않는다).
            char broken[2048];
            ManifestWith(broken, sizeof(broken), ValidBuildId, "", ValidFingerprint, entry,
                         runnerRow);
            char brokenPath[1100];
            snprintf(brokenPath, sizeof(brokenPath), "%s/Broken.modules.json", bin);
            REQUIRE(WriteBytes(brokenPath, broken, strlen(broken)));
            ModuleLoadReport report;
            auto created =
                ModuleLoader::TryCreateFromManifest(allocator, StringView(brokenPath), report);
            CHECK(!created && report.code == ModuleLoadCode::Incompatible &&
                  strcmp(report.requested, "Broken.modules.json") == 0);
            CHECK(strstr(report.message, "engineVersion must be") != nullptr);
        }

        // 플러그인 행 Plug(order 0)를 먼저 연다 — 파일은 4바이트인데 manifest는 5를 기대한다.
        char plugRow[512];
        Row(plugRow, sizeof(plugRow), "Plug", "Fixture-Plug.dll", 5, "", "Default", false, 0);
        char plugin[1024];
        Manifest(plugin, sizeof(plugin), plugRow);
        char pluginManifest[1100];
        snprintf(pluginManifest, sizeof(pluginManifest), "%s/Fixture.modules.json", pluginFolder);
        REQUIRE(WriteBytes(pluginManifest, plugin, strlen(plugin)));
        {
            ModuleLoadReport report;
            auto created =
                ModuleLoader::TryCreateFromManifest(allocator, StringView(manifest), report);
            CHECK(!created && report.code == ModuleLoadCode::Incompatible &&
                  strcmp(report.stage, "Lookup") == 0);
            CHECK(strcmp(report.failed, "Plug") == 0 && strcmp(report.requested, "Exchange") == 0);
            CHECK(strstr(report.message, "Plugins/Exchange/Binaries/Win64/Fixture-Plug.dll is 4 "
                                         "bytes but the manifest expects 5") != nullptr);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace module_manifest_tests_file
