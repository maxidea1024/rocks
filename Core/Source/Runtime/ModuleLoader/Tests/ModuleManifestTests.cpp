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

// Modular 모듈 manifest 적재(Docs/Design/ModuleLoader.ko.md 3.13절, ML4a)를 검사한다. 해석·검사의
// 양성과 키·형식·의존 위반, 실행 파일 행만 있는 manifest로 만든 로더(OS 적재 없이 성공 경로),
// 그리고 Prepare의 실패 경로(파일 없음·크기 다름·의존 DLL 없음·진입점 없음)를 본다. OS 적재 실패
// 경로는 실행 중에 쓴 데이터 전용 시험 라이브러리(TestSupport/DynamicLibraryFixture.h)를 쓰므로 그
// 라이브러리를 만들 수 없는 플랫폼에서는 SKIP이다. 진입점을 부르는 성공 경로는 코드가 있는 공유
// 라이브러리가 필요해 Modular FoundationTests(빌드 타깃 BS8d5)가 본다.
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

// 머리(schema·buildId·engineVersion·toolchainFingerprint)와 rows(JSON 배열 원소들)로 manifest
// 본문을 만든다.
void Manifest(char* out, usize capacity, const char* rows) noexcept {
    snprintf(out, capacity,
             "{\"schema\": 3, \"buildId\": \"%s\", \"engineVersion\": \"0.1.0\", "
             "\"toolchainFingerprint\": \"%s\", \"modules\": [%s]}",
             ValidBuildId, ValidFingerprint, rows);
}

// 행 하나의 JSON이다.
void Row(char* out, usize capacity, const char* name, const char* file, uint64 size,
         const char* dependencies, const char* phase, bool host) noexcept {
    snprintf(out, capacity,
             "{\"name\": \"%s\", \"file\": \"%s\", \"size\": %llu, \"dependencies\": [%s], "
             "\"loadingPhase\": \"%s\", \"onRequest\": false, \"host\": %s}",
             name, file, static_cast<unsigned long long>(size), dependencies, phase,
             host ? "true" : "false");
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
        Row(base, sizeof(base), "Core", "Fixture-Core.dll", 10, "", "EarliestPossible", false);
        Row(middle, sizeof(middle), "Render", "Fixture-Render.dll", 20, "\"Core\"", "PreDefault",
            false);
        Row(runner, sizeof(runner), "Runner", "Runner.exe", 30, "\"Core\", \"Render\"", "None",
            true);
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
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 형식 위반은 모두 Incompatible(단계 "Manifest")이고 문구가 그 이유를 말한다: JSON 아님, 최상위 키
// 빠짐·더함, schema·buildId·지문·엔진 버전 모양, 빈 modules, 행 키 빠짐·더함, 이름·파일 이름·크기·
// 단계·표지 형식, 뒤 행이나 없는 행을 가리키는 의존, 중복 의존, 같은 이름 두 행, 호스트 행 둘.
TEST(ModuleManifest_RejectsMalformedManifests) {
    SystemAllocator allocator;
    char good[512];
    Row(good, sizeof(good), "Core", "Core.dll", 1, "", "Default", false);
    const char* header = "\"schema\": 3, \"buildId\": \"0123456789abcdef0123456789abcdef\", "
                         "\"engineVersion\": \"0.1.0\", \"toolchainFingerprint\": "
                         "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"";
    // 행 하나의 키·값만 바꾼 행이다(나머지는 바른 값).
    auto row = [](char* out, usize capacity, const char* name, const char* file, const char* size,
                  const char* dependencies, const char* phase, const char* onRequest,
                  const char* extra) {
        snprintf(out, capacity,
                 "{\"name\": \"%s\", \"file\": \"%s\", \"size\": %s, \"dependencies\": [%s], "
                 "\"loadingPhase\": \"%s\", \"onRequest\": %s%s}",
                 name, file, size, dependencies, phase, onRequest, extra);
    };
    struct Case {
        char text[4096];
        const char* reason;
    };
    static Case cases[24];
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
                 "\"toolchainFingerprint\": \"%s\", \"modules\": [%s]}",
                 schema, buildId, engine, fingerprint, good);
    };
    const char* id = "0123456789abcdef0123456789abcdef";
    const char* print = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    snprintf(add("not valid JSON"), sizeof(cases[0].text), "not json");
    snprintf(add("exactly the keys schema"), sizeof(cases[0].text),
             "{\"buildId\": \"x\", \"modules\": [%s]}", good);
    snprintf(add("exactly the keys schema"), sizeof(cases[0].text),
             "{%s, \"modules\": [%s], \"extra\": 1}", header, good);
    withHeader("schema must be", "2", id, "0.1.0", print);
    withHeader("buildId must be", "3", "0123456789ABCDEF0123456789abcdef", "0.1.0", print);
    withHeader("engineVersion must be", "3", id, "", print);
    withHeader("toolchainFingerprint must be", "3", id, "0.1.0", "0123");
    withRows("modules must be a non-empty array", "");
    char text[1024];
    snprintf(text, sizeof(text),
             "{\"name\": \"Core\", \"file\": \"Core.dll\", \"size\": 1, \"dependencies\": [], "
             "\"loadingPhase\": \"Default\", \"onRequest\": false}");
    withRows("must have exactly the keys name", text);
    row(text, sizeof(text), "Core", "Core.dll", "1", "", "Default", "false",
        ", \"host\": false, \"extra\": 0");
    withRows("must have exactly the keys name", text);
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
    char rows[2048];
    row(text, sizeof(text), "Render", "Render.dll", "1", "\"Core\", \"Core\"", "Default", "false",
        ", \"host\": false");
    snprintf(rows, sizeof(rows), "%s, %s", good, text);
    withRows("lists dependency Core twice", rows);
    snprintf(rows, sizeof(rows), "%s, %s", good, good);
    withRows("lists module Core twice", rows);
    char runner[512];
    Row(runner, sizeof(runner), "Runner", "Fixture-Runner.dll", 1, "", "None", true);
    char second[512];
    Row(second, sizeof(second), "Tool", "Fixture-Tool.dll", 1, "", "None", true);
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

} // namespace module_manifest_tests_file
