#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "AssetDatabaseTestSupport.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetAliasTable.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/AssetTextResolver.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetDatabase/TemporaryFiles.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <Core/Time/DateTime.h>
#include <TestSupport/NFailAllocator.h>

// 경로 별칭 표 시험이다(Docs/Design/AssetSystem.ko.md 4.3절, 7.1절 17 A2e1): 왕복(결정적 byte —
// golden, 빈 표도 파일, 정렬·같은 키 뒤가 이김·caseless 키), 신뢰하지 않는 입력(칸마다 위반·JSON
// 문법·중복 키·크기·항목 수 상한, 정렬이 어긋난 파일은 받음), 쓰기(잠금·입력 검사·IO 고장·
// NotDurable·할당 실패 — 실패하면 파일·메모리 표 그대로), 되돌리기, prune(만료 경계·텍스트 사용
// 경계·대소문자·하위 디렉터리·링크·자신 제외·표시 이름 escape·상한·IO), 서비스(지연 적재·해석과
// AliasResolved·열기·새 패키지 점유·remove·prune·깨진 표), 고아 임시 파일 정리.
using namespace rocks;
namespace asset_alias_table_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;
using reflection_text::TextAssignment;
using reflection_text::TextDiagnostics;

namespace {

constexpr StringView AliasFile = "Config/AssetAliases.json";
constexpr int64 Day = TimeSpan::TicksPerDay;

DateTime Time(StringView text) noexcept {
    auto parsed = TryParseIso8601(text);
    CHECK(parsed);
    return parsed ? parsed.Value() : DateTime{};
}

int64 Ticks(StringView text) noexcept {
    return Time(text).Ticks();
}

DateTime At(int64 ticks) noexcept {
    auto made = DateTime::TryFromTicks(ticks);
    CHECK(made);
    return made ? made.Value() : DateTime{};
}

// 별칭 장면이다: 루트는 디스크 임시 디렉터리(잠금), 파일은 그 루트에 놓인 메모리 파일 시스템
// (Content·Config), 잡은 잠금.
struct AliasScene {
    AliasScene(AssetEnv& env, const char* tag) noexcept
        : root(tag), layout(env.allocator), files(env.allocator, Initialized(layout, root)) {
        ready = root.made && !layout.Root().IsEmpty() && files.Ready() &&
                files.AddDirectory("Content") && files.AddDirectory("Config") &&
                static_cast<bool>(lock.Acquire(layout, 0));
    }

    static StringView Initialized(ProjectLayout& layout, const TempProject& project) noexcept {
        return layout.Initialize(project.Root()) ? layout.Root() : project.Root();
    }

    bool Put(StringView text) noexcept {
        files.Remove(AliasFile);
        return files.AddFile(
            AliasFile,
            Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
    }

    StringView Text() const noexcept {
        const Span<const uint8> bytes = files.Bytes(AliasFile);
        return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
    }

    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetDatabaseLock lock;
    bool ready = false;
};

// 한 항목 파일이다(칸 하나를 바꿔 위반을 만든다).
String Entry(Allocator& allocator, StringView path, StringView asset, StringView created,
             StringView days) noexcept {
    auto text = TryFormat(allocator,
                          "{{\"version\": 1, \"aliases\": [{{\"path\": {}, \"asset\": {}, "
                          "\"created\": {}, \"days\": {}}}]}}",
                          path, asset, created, days);
    String out(allocator);
    CHECK(text && out.TryAssign(text.Value().View()));
    return out;
}

constexpr StringView GoodPath = "\"/Game/Old/Hero\"";
constexpr StringView GoodAsset = "\"5e570000-a55e-7000-0000-000000000b01\"";
constexpr StringView GoodCreated = "\"2026-11-22T00:00:00Z\"";
constexpr StringView GoodDays = "90";

} // namespace

// 왕복: 파일이 없으면 빈 표, 더하기(정렬·같은 키는 뒤가 이김)·바꾸기·지우기, 결정적 byte(golden),
// 빈 표도 파일로 쓴다, 다시 읽으면 같은 표, caseless 키로 찾는다, 만료 시각 계산(넘침 포화).
TEST(AssetAliasTable_RoundTrips) {
    AssetEnv env;
    REQUIRE(env.Ready());
    AliasScene scene(env, "alias-roundtrip");
    REQUIRE(scene.ready);
    AssetAliasTable table(env.allocator, scene.layout);
    CHECK(!table.IsLoaded());
    REQUIRE(table.Load(scene.files));
    CHECK(table.IsLoaded() && table.Count() == 0 && !scene.files.Has(AliasFile));

    const DateTime created = Time("2026-11-22T00:00:00Z");
    const AssetAliasInput adds[] = {{"/Game/Zeta", FixedId(0xb03), created, 30},
                                    {"/Game/Old/Hero", FixedId(0xb01), created, 90},
                                    {"/Game/zeta", FixedId(0xb02), At(created.Ticks() + 1), 7}};
    bool notDurable = true;
    REQUIRE(table.Write(scene.files, scene.lock, {}, adds, notDurable));
    CHECK(!notDurable && table.Count() == 2);
    CHECK(table.At(0).path.View() == "/Game/Old/Hero" && table.At(1).path.View() == "/Game/zeta");
    CHECK(table.At(1).asset == FixedId(0xb02) && table.At(1).days == 7);
    const StringView golden = "{\n"
                              "  \"version\": 1,\n"
                              "  \"aliases\": [\n"
                              "    {\n"
                              "      \"path\": \"/Game/Old/Hero\",\n"
                              "      \"asset\": \"5e570000-a55e-7000-0000-000000000b01\",\n"
                              "      \"created\": \"2026-11-22T00:00:00Z\",\n"
                              "      \"days\": 90\n"
                              "    },\n"
                              "    {\n"
                              "      \"path\": \"/Game/zeta\",\n"
                              "      \"asset\": \"5e570000-a55e-7000-0000-000000000b02\",\n"
                              "      \"created\": \"2026-11-22T00:00:00.0000001Z\",\n"
                              "      \"days\": 7\n"
                              "    }\n"
                              "  ]\n"
                              "}\n";
    CHECK(scene.Text() == golden);

    // 다시 읽으면 같다. caseless 키로 찾는다.
    AssetAliasTable again(env.allocator, scene.layout);
    REQUIRE(again.Load(scene.files));
    REQUIRE(again.Count() == 2);
    auto key = ParseLogicalPath(env.allocator, "/game/OLD/hero");
    REQUIRE(key);
    CHECK(again.Find(key.Value().Key()) == 0 && again.Find("game/none") == AssetAliasNone);
    CHECK(again.At(0).created.Ticks() == created.Ticks() &&
          again.At(1).created.Ticks() == created.Ticks() + 1);

    // 지우기와 바꾸기(같은 키는 바꾼다), 모두 지우면 빈 표 파일.
    const StringView removeKeys[] = {again.At(1).key.View()};
    const AssetAliasInput replace[] = {{"/Game/OLD/hero", FixedId(0xb09), created, 1}};
    REQUIRE(again.Write(scene.files, scene.lock, removeKeys, replace, notDurable));
    CHECK(again.Count() == 1 && again.At(0).asset == FixedId(0xb09) &&
          again.At(0).path.View() == "/Game/OLD/hero");
    const StringView all[] = {again.At(0).key.View()};
    REQUIRE(again.Write(scene.files, scene.lock, all, {}, notDurable));
    CHECK(again.Count() == 0 && scene.Text() == "{\n  \"version\": 1,\n  \"aliases\": []\n}\n");

    // 만료 시각: created + days, 넘치면 최댓값.
    AssetAlias alias(env.allocator);
    alias.created = created;
    alias.days = 2;
    CHECK(AssetAliasExpiryTicks(alias) == created.Ticks() + 2 * Day);
    alias.created = At(DateTime::MaxValue().Ticks() - Day);
    CHECK(AssetAliasExpiryTicks(alias) == DateTime::MaxValue().Ticks());
}

// 신뢰하지 않는 입력: 칸마다 위반(진단 CorruptAliasTable·칸·항목 번호), JSON 문법(줄·열)·중복
// 키, 파일 크기·항목 수 상한(CapacityExceeded), 실패하면 표는 비고 읽지 않은 상태. 정렬이 어긋난
// 파일은 받아 정렬한다. 경계 값(days 1·36500)은 받는다. 읽기 IO는 진단 None과 그 오류.
TEST(AssetAliasTable_RejectsInvalidFile) {
    AssetEnv env;
    REQUIRE(env.Ready());
    AliasScene scene(env, "alias-invalid");
    REQUIRE(scene.ready);
    struct Case {
        String text;
        AssetAliasField field;
        uint64 detail;
        ErrorCode error;
    };

    Allocator& a = env.allocator;
    const uint64 none = AssetAliasNoEntry;
    Case cases[] = {
        {Entry(a, GoodPath, GoodAsset, GoodCreated, "90,"), AssetAliasField::Json, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, GoodAsset, GoodCreated, GoodDays), AssetAliasField::Json, 0,
         ErrorCode::None},
        {String(a), AssetAliasField::Root, none, ErrorCode::InvalidArgument},
        {String(a), AssetAliasField::Root, none, ErrorCode::InvalidArgument},
        {String(a), AssetAliasField::Root, none, ErrorCode::InvalidArgument},
        {String(a), AssetAliasField::Version, none, ErrorCode::InvalidArgument},
        {String(a), AssetAliasField::Version, none, ErrorCode::InvalidArgument},
        {String(a), AssetAliasField::Version, none, ErrorCode::InvalidArgument},
        {String(a), AssetAliasField::Aliases, none, ErrorCode::InvalidArgument},
        {String(a), AssetAliasField::Entry, 0, ErrorCode::InvalidArgument},
        {String(a), AssetAliasField::Entry, 0, ErrorCode::InvalidArgument},
        {String(a), AssetAliasField::Entry, 0, ErrorCode::InvalidArgument},
        {Entry(a, "1", GoodAsset, GoodCreated, GoodDays), AssetAliasField::Path, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, "\"/Other/Hero\"", GoodAsset, GoodCreated, GoodDays), AssetAliasField::Path, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, "\"/Game/Bad Name\"", GoodAsset, GoodCreated, GoodDays), AssetAliasField::Path, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, "1", GoodCreated, GoodDays), AssetAliasField::Asset, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, "\"5E570000-A55E-7000-0000-000000000B01\"", GoodCreated, GoodDays),
         AssetAliasField::Asset, 0, ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, "\"00000000-0000-0000-0000-000000000000\"", GoodCreated, GoodDays),
         AssetAliasField::Asset, 0, ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, GoodAsset, "1", GoodDays), AssetAliasField::Created, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, GoodAsset, "\"2026-13-01\"", GoodDays), AssetAliasField::Created, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, GoodAsset, GoodCreated, "0"), AssetAliasField::Days, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, GoodAsset, GoodCreated, "36501"), AssetAliasField::Days, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, GoodAsset, GoodCreated, "\"90\""), AssetAliasField::Days, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, GoodAsset, GoodCreated, "1.5"), AssetAliasField::Days, 0,
         ErrorCode::InvalidArgument},
        {Entry(a, GoodPath, GoodAsset, GoodCreated, "-1"), AssetAliasField::Days, 0,
         ErrorCode::InvalidArgument},
    };
    // 문법 위반: 끝 쉼표 뒤 닫힘(엄격 JSON). 두 번째는 올바른 파일(대조용 — 받는다).
    const StringView structural[] = {
        "[]",
        "{\"version\": 1}",
        "{\"version\": 1, \"aliases\": [], \"extra\": 0}",
        "{\"version\": 2, \"aliases\": []}",
        "{\"version\": \"1\", \"aliases\": []}",
        "{\"version\": 1.5, \"aliases\": []}",
        "{\"version\": 1, \"aliases\": {}}",
        "{\"version\": 1, \"aliases\": [0]}",
        "{\"version\": 1, \"aliases\": [{\"path\": \"/Game/A\"}]}",
        "{\"version\": 1, \"aliases\": [{\"path\": \"/Game/A\", \"asset\": "
        "\"5e570000-a55e-7000-0000-000000000b01\", \"created\": \"2026-11-22\", \"days\": 1, "
        "\"x\": 0}]}"};
    for (usize at = 0; at < sizeof(structural) / sizeof(structural[0]); ++at) {
        REQUIRE(cases[2 + at].text.TryAssign(structural[at]));
    }

    for (const Case& entry : cases) {
        REQUIRE(scene.Put(entry.text.View()));
        AssetAliasTable table(env.allocator, scene.layout);
        AssetDiagnostic failure;
        auto loaded = table.Load(scene.files, &failure);
        if (entry.error == ErrorCode::None) {
            CHECK(loaded && table.Count() == 1);
            continue;
        }

        CHECK(!loaded && loaded.Error() == entry.error && !table.IsLoaded() && table.Count() == 0);
        CHECK(failure.code == AssetDiagnosticCode::CorruptAliasTable &&
              failure.section == static_cast<uint32>(entry.field));
        if (entry.field == AssetAliasField::Json) {
            CHECK((failure.detail >> 32) == 1 && (failure.detail & 0xffffffffull) > 1);
        } else {
            CHECK(failure.detail == entry.detail);
        }
    }

    // 키 수는 같지만 이름이 다른 최상위·항목, 진단 칸 없이 부른 실패.
    const StringView renamed[] = {
        "{\"version\": 1, \"alias\": []}",
        "{\"version\": 1, \"aliases\": [{\"path\": \"/Game/A\", \"asset\": "
        "\"5e570000-a55e-7000-0000-000000000b01\", \"created\": \"2026-11-22\", \"day\": 1}]}"};
    const AssetAliasField renamedFields[] = {AssetAliasField::Root, AssetAliasField::Entry};
    for (usize at = 0; at < 2; ++at) {
        REQUIRE(scene.Put(renamed[at]));
        AssetAliasTable table(env.allocator, scene.layout);
        AssetDiagnostic failure;
        CHECK(!table.Load(scene.files, &failure) &&
              failure.section == static_cast<uint32>(renamedFields[at]));
        CHECK(!table.Load(scene.files));
    }

    // 중복 키(JSON — 같은 객체 안)·같은 경로 키(caseless — 정렬 뒤 뒤쪽 항목 번호)·정렬이 어긋난
    // 파일(받는다)·경계 days.
    REQUIRE(scene.Put("{\"version\": 1, \"version\": 1, \"aliases\": []}"));
    AssetAliasTable duplicateKey(env.allocator, scene.layout);
    AssetDiagnostic failure;
    CHECK(!duplicateKey.Load(scene.files, &failure) &&
          failure.section == static_cast<uint32>(AssetAliasField::Json));
    const StringView twoSame =
        "{\"version\": 1, \"aliases\": ["
        "{\"path\": \"/Game/B\", \"asset\": \"5e570000-a55e-7000-0000-000000000b01\", "
        "\"created\": \"2026-11-22T00:00:00Z\", \"days\": 1},"
        "{\"path\": \"/Game/A\", \"asset\": \"5e570000-a55e-7000-0000-000000000b02\", "
        "\"created\": \"2026-11-22T00:00:00Z\", \"days\": 36500},"
        "{\"path\": \"/game/b\", \"asset\": \"5e570000-a55e-7000-0000-000000000b03\", "
        "\"created\": \"2026-11-22T00:00:00Z\", \"days\": 2}]}";
    REQUIRE(scene.Put(twoSame));
    AssetAliasTable duplicatePath(env.allocator, scene.layout);
    CHECK(!duplicatePath.Load(scene.files, &failure) &&
          failure.section == static_cast<uint32>(AssetAliasField::DuplicatePath) &&
          failure.detail == 2);
    const StringView unsorted =
        "{\"version\": 1, \"aliases\": ["
        "{\"path\": \"/Game/B\", \"asset\": \"5e570000-a55e-7000-0000-000000000b01\", "
        "\"created\": \"2026-11-22T00:00:00Z\", \"days\": 1},"
        "{\"path\": \"/Game/A\", \"asset\": \"5e570000-a55e-7000-0000-000000000b02\", "
        "\"created\": \"2026-11-22T09:00:00+09:00\", \"days\": 36500}]}";
    REQUIRE(scene.Put(unsorted));
    AssetAliasTable sorted(env.allocator, scene.layout);
    REQUIRE(sorted.Load(scene.files));
    CHECK(sorted.Count() == 2 && sorted.At(0).path.View() == "/Game/A" &&
          sorted.At(0).days == 36500 &&
          sorted.At(0).created.Ticks() == Ticks("2026-11-22T00:00:00Z"));

    // 항목 수 상한(읽기 — 상한 2에 셋), 파일 크기 상한.
    REQUIRE(scene.Put("{\"version\": 1, \"aliases\": [0, 0, 0]}"));
    AssetAliasTable capped(env.allocator, scene.layout, 2);
    CHECK(!capped.Load(scene.files, &failure) &&
          failure.section == static_cast<uint32>(AssetAliasField::Aliases));
    Array<uint8> huge(env.allocator);
    REQUIRE(huge.TryResize(static_cast<usize>(MaxAssetAliasFileBytes) + 1));
    for (uint8& byte : huge) {
        byte = ' ';
    }

    scene.files.Remove(AliasFile);
    REQUIRE(scene.files.AddFile(AliasFile, huge.View()));
    AssetAliasTable large(env.allocator, scene.layout);
    auto tooLarge = large.Load(scene.files, &failure);
    CHECK(!tooLarge && tooLarge.Error() == ErrorCode::CapacityExceeded &&
          failure.section == static_cast<uint32>(AssetAliasField::File));

    // 읽기 IO: 열기 Busy·읽기 실패는 진단 None과 그 오류.
    const MemoryFault faults[] = {MemoryFault::OpenBusy, MemoryFault::Read};
    for (MemoryFault fault : faults) {
        scene.files.Remove(AliasFile);
        REQUIRE(scene.files.AddFile(AliasFile, Span<const uint8>(huge.Data(), 4), fault));
        AssetAliasTable broken(env.allocator, scene.layout);
        failure = AssetDiagnostic{};
        auto read = broken.Load(scene.files, &failure);
        CHECK(!read && read.Error() != ErrorCode::OutOfMemory &&
              failure.code == AssetDiagnosticCode::None && failure.error == read.Error());
    }
}

// 쓰기: 읽지 않은 표·잠금 아님·잘못된 입력(경로·null id·days·시각)·항목 수 상한은 거부(파일·표
// 그대로), IO 고장(만들기·쓰기·Flush·Close·교체)은 오류·임시 파일 없음·파일·표 그대로, NotDurable은
// 성공 표시, 할당 실패는 OutOfMemory이고 그대로.
TEST(AssetAliasTable_WriteFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    AliasScene scene(env, "alias-write");
    REQUIRE(scene.ready);
    const DateTime created = Time("2026-11-22T00:00:00Z");
    const AssetAliasInput first[] = {{"/Game/A", FixedId(0xb01), created, 90}};
    bool notDurable = false;
    AssetAliasTable unloaded(env.allocator, scene.layout);
    AssetDiagnostic failure;
    auto early = unloaded.Write(scene.files, scene.lock, {}, first, notDurable, &failure);
    CHECK(!early && early.Error() == ErrorCode::InvalidArgument);
    AssetAliasTable table(env.allocator, scene.layout, 2);
    REQUIRE(table.Load(scene.files));
    REQUIRE(table.Write(scene.files, scene.lock, {}, first, notDurable));
    const String before = [&]() noexcept {
        String text(env.allocator);
        CHECK(text.TryAssign(scene.Text()));
        return text;
    }();
    auto unchanged = [&]() noexcept {
        bool temporary = false;
        for (usize at = 0; at < scene.files.NodeCount(); ++at) {
            temporary = temporary || scene.files.NodeName(at).EndsWith(".tmp");
        }

        return !temporary && scene.Text() == before.View() && table.Count() == 1 &&
               table.At(0).path.View() == "/Game/A";
    };

    // 잠금 아님.
    AssetDatabaseLock other;
    auto unlocked = table.Write(scene.files, other, {}, first, notDurable, &failure);
    CHECK(!unlocked && unlocked.Error() == ErrorCode::InvalidArgument && unchanged());

    // 잘못된 입력.
    const AssetAliasInput bad[] = {{"/Game/Bad Name", FixedId(0xb02), created, 90},
                                   {"/Game/B", AssetId{}, created, 90},
                                   {"/Game/B", FixedId(0xb02), created, 0},
                                   {"/Game/B", FixedId(0xb02), created, AssetAliasMaxDays + 1}};
    for (const AssetAliasInput& input : bad) {
        failure = AssetDiagnostic{};
        auto rejected = table.Write(scene.files, scene.lock, {},
                                    Span<const AssetAliasInput>(&input, 1), notDurable, &failure);
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument && unchanged());
    }

    CHECK(failure.code == AssetDiagnosticCode::None);
    failure = AssetDiagnostic{};
    (void)table.Write(scene.files, scene.lock, {}, Span<const AssetAliasInput>(bad, 1), notDurable,
                      &failure);
    CHECK(failure.code == AssetDiagnosticCode::InvalidPath);
    CHECK(!table.Write(scene.files, scene.lock, {}, Span<const AssetAliasInput>(bad, 1),
                       notDurable) &&
          unchanged());

    // 항목 수 상한(상한 2에 셋째).
    const AssetAliasInput more[] = {{"/Game/B", FixedId(0xb02), created, 90},
                                    {"/Game/C", FixedId(0xb03), created, 90}};
    auto capped = table.Write(scene.files, scene.lock, {}, more, notDurable, &failure);
    CHECK(!capped && capped.Error() == ErrorCode::CapacityExceeded && unchanged());

    // IO 고장.
    const AssetAliasInput one[] = {{"/Game/B", FixedId(0xb02), created, 90}};
    auto failWith = [&](auto&& inject, auto&& clear) noexcept {
        inject();
        failure = AssetDiagnostic{};
        auto written = table.Write(scene.files, scene.lock, {}, one, notDurable, &failure);
        clear();
        return !written && written.Error() != ErrorCode::OutOfMemory &&
               failure.code == AssetDiagnosticCode::None && unchanged();
    };
    CHECK(failWith([&]() noexcept { scene.files.faults.create = MemoryError; },
                   [&]() noexcept { scene.files.faults.create = {}; }));
    CHECK(failWith(
        [&]() noexcept { scene.files.faults.failWriteCall = scene.files.WriteCalls() + 1; },
        [&]() noexcept { scene.files.faults.failWriteCall = 0; }));
    CHECK(failWith([&]() noexcept { scene.files.faults.flush = MemoryError; },
                   [&]() noexcept { scene.files.faults.flush = {}; }));
    CHECK(failWith([&]() noexcept { scene.files.faults.close = MemoryError; },
                   [&]() noexcept { scene.files.faults.close = {}; }));
    CHECK(failWith([&]() noexcept { scene.files.faults.replace = MemoryBusy; },
                   [&]() noexcept { scene.files.faults.replace = {}; }));
    // Config가 파일이면 디렉터리 만들기가 실패한다(별칭 표 노드는 그대로 남는다).
    scene.files.Remove("Config");
    REQUIRE(scene.files.AddFile("Config", Span<const uint8>()));
    CHECK(failWith([]() noexcept {}, []() noexcept {}));
    scene.files.Remove("Config");
    REQUIRE(scene.files.AddDirectory("Config"));

    // 외부 변경: 읽은 뒤 파일이 바뀌거나(내용·크기) 생기거나 지워지면 쓰지 않는다
    // (ExternalModification / StaleHandle — 파일 그대로), 다시 읽기 실패는 IO 오류.
    {
        Array<char> bytes(env.allocator);
        REQUIRE(bytes.TryAppend(Span<const char>(before.View().Data(), before.View().ByteCount())));
        bytes[bytes.Size() - 2] = ' ';
        const StringView variants[] = {StringView(bytes.Data(), bytes.Size()),
                                       "{\"version\": 1, \"aliases\": []}\n"};
        for (StringView variant : variants) {
            REQUIRE(scene.Put(variant));
            failure = AssetDiagnostic{};
            auto stale = table.Write(scene.files, scene.lock, {}, one, notDurable, &failure);
            CHECK(!stale && stale.Error() == ErrorCode::StaleHandle &&
                  failure.code == AssetDiagnosticCode::ExternalModification &&
                  scene.Text() == variant && table.Count() == 1);
        }

        scene.files.Remove(AliasFile);
        auto removed = table.Write(scene.files, scene.lock, {}, one, notDurable, &failure);
        CHECK(!removed && removed.Error() == ErrorCode::StaleHandle && !scene.files.Has(AliasFile));
        REQUIRE(scene.files.AddFile(
            AliasFile,
            Span<const uint8>(reinterpret_cast<const uint8*>(before.View().Data()),
                              before.View().ByteCount()),
            MemoryFault::Read));
        failure = AssetDiagnostic{};
        auto unreadable = table.Write(scene.files, scene.lock, {}, one, notDurable, &failure);
        CHECK(!unreadable && failure.code == AssetDiagnosticCode::None);
        scene.files.Remove(AliasFile);
        REQUIRE(scene.Put(before.View()));
        // 파일이 없던 표에 파일이 생겨도 바뀐 것이다.
        AssetAliasTable absent(env.allocator, scene.layout);
        scene.files.Remove(AliasFile);
        REQUIRE(absent.Load(scene.files));
        REQUIRE(scene.Put(before.View()));
        CHECK(!absent.Write(scene.files, scene.lock, {}, one, notDurable));
        // 너무 커진 파일도 바뀐 것이다.
        Array<uint8> huge(env.allocator);
        REQUIRE(huge.TryResize(static_cast<usize>(MaxAssetAliasFileBytes) + 1));
        scene.files.Remove(AliasFile);
        REQUIRE(scene.files.AddFile(AliasFile, huge.View()));
        CHECK(!table.Write(scene.files, scene.lock, {}, one, notDurable));
        scene.files.Remove(AliasFile);
        REQUIRE(scene.Put(before.View()));
    }

    // NotDurable은 성공 표시.
    scene.files.faults.replace = MemoryNotDurable;
    REQUIRE(table.Write(scene.files, scene.lock, {}, one, notDurable));
    scene.files.faults.replace = {};
    CHECK(notDurable && table.Count() == 2);

    // 할당 실패(지우기와 더하기를 함께 — 지울 키 집합도 할당한다).
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    const StringView dropA[] = {"game/a"};
    for (uint64 attempt = 1;; ++attempt) {
        AssetAliasTable fresh(failing, scene.layout);
        failing.FailOnAttempt(attempt);
        auto loaded = fresh.Load(scene.files);
        Result<void> written = loaded
                                   ? fresh.Write(scene.files, scene.lock, dropA, more, notDurable)
                                   : Result<void>::Failure(loaded.Error());
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(written);
            CHECK(fresh.Count() == 2 && fresh.Find("game/a") == AssetAliasNone);
            REQUIRE(fresh.Restore(scene.files, scene.lock));
            break;
        }

        CHECK(!written && written.Error() == ErrorCode::OutOfMemory);
        CHECK(fresh.Count() <= 2);
        ++failures;
    }

    CHECK(failures > 0);
}

// 되돌리기: 쓰기 전 파일 byte·표로(파일이 없었으면 지운다), 되돌릴 것이 없으면 아무것도 하지
// 않는다, 잠금 아님, 쓰기·지우기 IO 실패.
TEST(AssetAliasTable_Restores) {
    AssetEnv env;
    REQUIRE(env.Ready());
    AliasScene scene(env, "alias-restore");
    REQUIRE(scene.ready);
    const DateTime created = Time("2026-11-22T00:00:00Z");
    const AssetAliasInput first[] = {{"/Game/A", FixedId(0xb01), created, 90}};
    const AssetAliasInput second[] = {{"/Game/B", FixedId(0xb02), created, 90}};
    bool notDurable = false;
    AssetAliasTable table(env.allocator, scene.layout);
    REQUIRE(table.Load(scene.files));
    REQUIRE(table.Restore(scene.files, scene.lock));
    REQUIRE(table.Write(scene.files, scene.lock, {}, first, notDurable));
    REQUIRE(table.Restore(scene.files, scene.lock));
    CHECK(!scene.files.Has(AliasFile) && table.Count() == 0);
    REQUIRE(table.Restore(scene.files, scene.lock));

    REQUIRE(table.Write(scene.files, scene.lock, {}, first, notDurable));
    String saved(env.allocator);
    REQUIRE(saved.TryAssign(scene.Text()));
    REQUIRE(table.Write(scene.files, scene.lock, {}, second, notDurable));
    AssetDatabaseLock other;
    CHECK(!table.Restore(scene.files, other));
    scene.files.faults.create = MemoryError;
    CHECK(!table.Restore(scene.files, scene.lock));
    scene.files.faults.create = {};
    CHECK(table.Count() == 2);
    REQUIRE(table.Restore(scene.files, scene.lock));
    CHECK(scene.Text() == saved.View() && table.Count() == 1 &&
          table.At(0).path.View() == "/Game/A");

    // 파일이 없던 상태로 되돌리기의 지우기 실패.
    AssetAliasTable fresh(env.allocator, scene.layout);
    scene.files.Remove(AliasFile);
    REQUIRE(fresh.Load(scene.files));
    REQUIRE(fresh.Write(scene.files, scene.lock, {}, first, notDurable));
    scene.files.faults.remove = MemoryBusy;
    CHECK(!fresh.Restore(scene.files, scene.lock));
    scene.files.faults.remove = {};
    REQUIRE(fresh.Restore(scene.files, scene.lock));
    CHECK(!scene.files.Has(AliasFile));
    // 마지막 Write 뒤 파일이 바뀌었으면(밖에서 지움·고침) 되돌리지 않는다(그 변경을 잃는다).
    REQUIRE(fresh.Write(scene.files, scene.lock, {}, first, notDurable));
    scene.files.Remove(AliasFile);
    auto gone = fresh.Restore(scene.files, scene.lock);
    CHECK(!gone && gone.Error() == ErrorCode::StaleHandle && fresh.Count() == 1);
    REQUIRE(scene.Put("{\"version\": 1, \"aliases\": []}\n"));
    auto edited = fresh.Restore(scene.files, scene.lock);
    CHECK(!edited && edited.Error() == ErrorCode::StaleHandle && fresh.Count() == 1);
    // 다시 읽기 실패.
    scene.files.Remove(AliasFile);
    REQUIRE(scene.files.AddFile(AliasFile, Span<const uint8>(), MemoryFault::Read));
    CHECK(!fresh.Restore(scene.files, scene.lock));
    scene.files.Remove(AliasFile);

    // 할당 실패: 파일이 있던 상태(다시 쓰기)와 없던 상태(지우기) 둘 다.
    test::NFailAllocator failing(env.allocator);
    const bool states[] = {true, false};
    for (bool existed : states) {
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            scene.files.Remove(AliasFile);
            if (existed) {
                REQUIRE(scene.Put(saved.View()));
            }

            AssetAliasTable attempted(failing, scene.layout);
            REQUIRE(attempted.Load(scene.files));
            REQUIRE(attempted.Write(scene.files, scene.lock, {}, second, notDurable));
            failing.FailOnAttempt(attempt);
            auto restored = attempted.Restore(scene.files, scene.lock);
            const bool triggered = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!triggered) {
                REQUIRE(restored);
                CHECK(existed ? scene.Text() == saved.View() : !scene.files.Has(AliasFile));
                break;
            }

            CHECK(!restored && restored.Error() == ErrorCode::OutOfMemory &&
                  attempted.Count() == (existed ? 2 : 1));
            ++failures;
        }

        CHECK(failures > 0);
    }
}

// prune: 만료 경계(created + days == 지금은 만료, 1 tick 전은 아님), 쓰인 항목은 남김(뒤 경계
// 문자마다·파일 끝·대소문자 무시·하위 디렉터리·줄 번호, 경계가 아닌 뒤 글자·더 긴 이름은 쓰임
// 아님), 링크·별칭 표 자신·그 임시 파일·일반 파일 아님은 건너뜀, 파일 이름 표시 escape, 검색 상한
// (파일 수·총 byte·파일 하나)에 닿으면 아무것도 지우지 않음, 열거·읽기 IO·사라진 파일, 읽지 않은
// 표· 잠금 아님, 만료가 없으면·모두 쓰이면 쓰지 않음, 할당 실패.
TEST(AssetAliasTable_Prunes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    AliasScene scene(env, "alias-prune");
    REQUIRE(scene.ready);
    const DateTime created = Time("2026-01-01T00:00:00Z");
    const int64 now = created.Ticks() + 10 * Day;
    const StringView boundaries[] = {".", "\"", "'", " ", "\t", "\r", "\n", ",", ")", "]"};
    const usize boundaryCount = sizeof(boundaries) / sizeof(boundaries[0]);
    // 만료 항목: Used0..Used9(경계 문자마다 쓰임), UsedEnd(파일 끝), UsedCase(대소문자 무시),
    // UsedDeep(하위 디렉터리), Free(쓰임 없음 — 지움), Near(Free 뒤에 글자가 붙은 것만 나옴 —
    // 지움). 만료 아님: Fresh(1 tick 덜 지남).
    Array<AssetAliasInput> inputs(env.allocator);
    Array<String> names(env.allocator);
    for (usize at = 0; at < boundaryCount; ++at) {
        auto name = TryFormat(env.allocator, "/Game/Used{}", at);
        REQUIRE(name);
        String owned(env.allocator);
        REQUIRE(owned.TryAssign(name.Value().View()));
        REQUIRE(names.TryPushBack(Move(owned)));
    }

    const StringView fixed[] = {"/Game/UsedEnd", "/Game/UsedCase", "/Game/UsedDeep",
                                "/Game/Free",    "/Game/Near",     "/Game/Fresh"};
    for (StringView name : fixed) {
        String owned(env.allocator);
        REQUIRE(owned.TryAssign(name));
        REQUIRE(names.TryPushBack(Move(owned)));
    }

    for (usize at = 0; at < names.Size(); ++at) {
        const bool fresh = names[at].View() == "/Game/Fresh";
        AssetAliasInput input{names[at].View(), FixedId(0xc00 + at),
                              fresh ? At(created.Ticks() + 1) : created, 10};
        REQUIRE(inputs.TryPushBack(input));
    }

    AssetAliasTable table(env.allocator, scene.layout);
    REQUIRE(table.Load(scene.files));
    bool notDurable = false;
    REQUIRE(table.Write(scene.files, scene.lock, {}, inputs.View(), notDurable));
    String text(env.allocator);
    for (usize at = 0; at < boundaryCount; ++at) {
        REQUIRE(text.TryAppend("ref "));
        REQUIRE(text.TryAppend(names[at].View()));
        REQUIRE(text.TryAppend(boundaries[at]));
        REQUIRE(text.TryAppend("\n"));
    }

    REQUIRE(text.TryAppend("x /Game/NearX /Game/FreeY\n/game/usedcase\n/Game/UsedEnd"));
    auto bytesOf = [](StringView view) noexcept {
        return Span<const uint8>(reinterpret_cast<const uint8*>(view.Data()), view.ByteCount());
    };
    REQUIRE(scene.files.AddFile("Config/Game.ini", bytesOf(text.View())));
    // 쓰임이 아닌 것: 논리 경로 상한(256 byte)을 넘는 후보, 마운트가 아닌 앞부분, 파일 끝에서 잘린
    // 마운트.
    String noise(env.allocator);
    REQUIRE(noise.TryAppend("/Game/"));
    for (usize at = 0; at < 300; ++at) {
        REQUIRE(noise.TryAppend("a"));
    }

    REQUIRE(noise.TryAppend("\n/Gxme/Free\n/Game/Zzz\n/Ga"));
    REQUIRE(scene.files.AddFile("Config/Noise.txt", bytesOf(noise.View())));
    REQUIRE(scene.files.AddDirectory("Config/Sub"));
    REQUIRE(scene.files.AddFile(StringView("Config/Sub/\x01weird.txt"),
                                bytesOf("first\n/Game/UsedDeep\n")));
    // 건너뛰는 것: 링크·별칭 표 임시 파일·일반 파일이 아닌 것·링크 디렉터리.
    REQUIRE(scene.files.AddFile("Config/Link.txt", bytesOf("/Game/Free"), MemoryFault::None,
                                platform::FileType::File, true));
    REQUIRE(scene.files.AddFile("Config/AssetAliases.json.0123abcd-0123-4567-89ab-0123456789ab.tmp",
                                bytesOf("/Game/Free")));
    REQUIRE(scene.files.AddFile("Config/Device", bytesOf("/Game/Free"), MemoryFault::None,
                                platform::FileType::Other));
    REQUIRE(scene.files.AddDirectory("Config/LinkedDir", true));
    REQUIRE(scene.files.AddFile("Config/LinkedDir/In.txt", bytesOf("/Game/Free")));

    // 읽지 않은 표·잠금 아님.
    AssetAliasPruneReport report(env.allocator);
    AssetAliasTable unloaded(env.allocator, scene.layout);
    CHECK(!unloaded.Prune(scene.files, scene.lock, now, {}, report));
    AssetDatabaseLock other;
    CHECK(!table.Prune(scene.files, other, now, {}, report));

    // 검색 상한(파일 수 1·총 byte 1·파일 하나 1 byte) — 아무것도 지우지 않는다.
    const AssetAliasSearchLimits tight[] = {{1, uint64{1} << 30, uint64{1} << 20},
                                            {100, 1, uint64{1} << 20},
                                            {100, uint64{1} << 30, 1}};
    for (const AssetAliasSearchLimits& limits : tight) {
        AssetDiagnostic failure;
        REQUIRE(table.Prune(scene.files, scene.lock, now, limits, report, &failure));
        CHECK(report.truncated && report.removed == 0 && report.kept == table.Count() &&
              failure.code == AssetDiagnosticCode::AliasSearchTruncated &&
              table.Count() == names.Size());
    }

    REQUIRE(table.Prune(scene.files, scene.lock, now, tight[0], report));
    CHECK(report.truncated && report.removed == 0);

    // IO: 하위 디렉터리 열거 실패·읽기 실패는 오류(표 그대로), 열거 뒤 사라진 파일은 건너뜀.
    REQUIRE(scene.files.AddDirectory("Config/Broken", false, MemoryFault::Enumerate));
    CHECK(!table.Prune(scene.files, scene.lock, now, {}, report) && table.Count() == names.Size());
    scene.files.Remove("Config/Broken");
    REQUIRE(scene.files.AddFile("Config/Unreadable.txt", bytesOf("x"), MemoryFault::Read));
    CHECK(!table.Prune(scene.files, scene.lock, now, {}, report));
    scene.files.Remove("Config/Unreadable.txt");
    REQUIRE(
        scene.files.AddFile("Config/Gone.txt", bytesOf("/Game/Free"), MemoryFault::OpenMissing));
    REQUIRE(scene.files.AddDirectory("Config/Vanished", false, MemoryFault::EnumerateMissing));

    // 만료 전에는(지금 = 만든 시각) 아무것도 하지 않는다.
    REQUIRE(table.Prune(scene.files, scene.lock, created.Ticks(), {}, report));
    CHECK(report.removed == 0 && report.kept == names.Size() && report.uses.IsEmpty());

    // prune.
    REQUIRE(table.Prune(scene.files, scene.lock, now, {}, report));
    CHECK(report.removed == 2 && report.inUse == boundaryCount + 3 && report.kept == 1);
    CHECK(table.Count() == names.Size() - 2);
    auto has = [&](StringView path) noexcept {
        auto parsed = ParseLogicalPath(env.allocator, path);
        return parsed && table.Find(parsed.Value().Key()) != AssetAliasNone;
    };
    CHECK(!has("/Game/Free") && !has("/Game/Near") && has("/Game/Fresh") && has("/Game/UsedEnd"));
    bool weirdName = false;
    bool deepLine = false;
    for (const AssetAliasUse& use : report.uses) {
        if (use.alias.View() == "/Game/UsedDeep") {
            weirdName = use.file.View() == "Config/Sub/\\x01weird.txt";
            deepLine = use.line == 2;
        }

        if (use.alias.View() == "/Game/Used3") {
            CHECK(use.file.View() == "Config/Game.ini" && use.line == 4);
        }
    }

    CHECK(weirdName && deepLine);

    // 모두 쓰이면 쓰지 않는다(파일 그대로).
    String saved(env.allocator);
    REQUIRE(saved.TryAssign(scene.Text()));
    REQUIRE(table.Prune(scene.files, scene.lock, now, {}, report));
    CHECK(report.removed == 0 && scene.Text() == saved.View());

    // 할당 실패(지울 만료 항목 Spare 하나, 형제 하위 디렉터리 여럿 — 검색 목록이 자란다).
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    REQUIRE(scene.files.AddFile("Config/More.txt", bytesOf("/Game/Fresh")));
    const AssetAliasInput spare[] = {{"/Game/Spare", FixedId(0xcff), created, 10}};
    REQUIRE(table.Write(scene.files, scene.lock, {}, spare, notDurable));
    const StringView siblings[] = {"Config/D0", "Config/D1", "Config/D2", "Config/D3",
                                   "Config/D4", "Config/D5", "Config/D6", "Config/D7"};
    for (StringView sibling : siblings) {
        REQUIRE(scene.files.AddDirectory(sibling));
    }

    for (uint64 attempt = 1;; ++attempt) {
        AssetAliasTable fresh(failing, scene.layout);
        REQUIRE(fresh.Load(scene.files));
        AssetAliasPruneReport attemptReport(failing);
        failing.FailOnAttempt(attempt);
        auto pruned = fresh.Prune(scene.files, scene.lock, now + Day, {}, attemptReport);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(pruned);
            CHECK(attemptReport.inUse == boundaryCount + 4 && attemptReport.removed == 1);
            break;
        }

        CHECK(!pruned && pruned.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
}

// 서비스: Initialize 전 Aliases는 InvalidArgument, 지연 적재(깨진 별칭 파일도 색인에 있는 경로는
// 문제 없음 — 별칭이 필요한 호출만 실패), 열기(asset-info)가 옛 경로를 별칭으로 풀고 경고
// AliasResolved (detail = 항목 번호), 참조 텍스트(asset-set Hard)도 별칭으로 푼다, 별칭 경로에 새
// 패키지는 DuplicatePath / AlreadyExists(related = 대상), 같은 경로가 색인에도 있으면 색인이
// 이긴다, remove(잘못된 경로·없음·성공), prune(설정 시계·상한), 없는 경로는 NotFound.
TEST(AssetAliasTable_ServiceResolves) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    AliasScene scene(env, "alias-service");
    REQUIRE(scene.ready);
    REQUIRE(scene.files.AddFile("Content/Hero.rasset", sources.hero.View()));
    REQUIRE(scene.files.AddFile("Content/Base.rasset", sources.base.View()));
    REQUIRE(scene.files.AddFile("Content/Derived.rasset", sources.derived.View()));
    REQUIRE(scene.files.AddFile("Content/Villain.rasset", sources.villain.View()));
    AssetTypeHookTable hooks(env.allocator, env.types);
    ManualAssetClock clock(Ticks("2026-11-22T00:00:00Z"));
    AssetServiceConfig config;
    config.packages.types = &env.types;
    config.packages.files = &scene.files;
    config.packages.hooks = &hooks;
    config.packages.read.engineVersion = TestEngineVersion;
    config.packages.registryContext = ContextId{0x414c494153ull, 1};
    config.packages.registryOptions.names = &env.names;
    config.clock = &clock;
    AssetService service(env.allocator, scene.layout, config);
    CHECK(!service.Aliases());
    REQUIRE(service.Initialize());

    // 깨진 별칭 파일: 색인에 있는 경로는 열린다, 없는 경로·새 패키지는 별칭 오류.
    REQUIRE(scene.Put("{"));
    String out(env.allocator);
    Array<EditorWarning> warnings(env.allocator);
    REQUIRE(service.Info("/Game/Hero", AssetInfoOptions{}, out, &warnings));
    AssetDiagnostic failure;
    auto broken = service.Info("/Game/Old/Hero", AssetInfoOptions{}, out, &warnings, &failure);
    CHECK(!broken && broken.Error() == ErrorCode::InvalidArgument &&
          failure.code == AssetDiagnosticCode::CorruptAliasTable);
    // 경고 목록 없이도 같은 진단이다.
    failure = AssetDiagnostic{};
    CHECK(!service.Info("/Game/Old/Hero", AssetInfoOptions{}, out, nullptr, &failure) &&
          failure.code == AssetDiagnosticCode::CorruptAliasTable);
    TextDiagnostics diagnostics(env.allocator);
    auto blocked =
        service.New(scene.lock, "TestAsset", "/Game/Fresh", {}, diagnostics, &warnings, &failure);
    CHECK(!blocked && failure.code == AssetDiagnosticCode::CorruptAliasTable);
    // 참조 값 해석(asset-set)은 대입 진단 Unresolved로 실패하고 원인을 경고로 남긴다.
    {
        Array<TextAssignment> broken(env.allocator);
        TextAssignment assignment;
        REQUIRE(reflection_text::MakeTextAssignment("hard=/Game/Old/Hero", 0, assignment));
        REQUIRE(broken.TryPushBack(assignment));
        warnings.Clear();
        TextDiagnostics brokenDiagnostics(env.allocator);
        CHECK(!service.Set(scene.lock, "/Game/Villain", broken.View(), false, brokenDiagnostics,
                           &warnings));
        bool unreadable = false;
        for (const EditorWarning& warning : warnings) {
            unreadable =
                unreadable || (warning.kind == EditorWarningKind::AliasTableUnreadable &&
                               warning.diagnostic.code == AssetDiagnosticCode::CorruptAliasTable);
        }

        CHECK(unreadable);
        // 그 경고를 붙이지 못하면(할당 실패) 열기가 OutOfMemory다.
        test::NFailAllocator brokenWarnings(env.allocator);
        Array<EditorWarning> failingList(brokenWarnings);
        brokenWarnings.FailOnAttempt(1);
        auto lostWarning = service.Info("/Game/Old/Hero", AssetInfoOptions{}, out, &failingList);
        brokenWarnings.DisableFailure();
        CHECK(!lostWarning && lostWarning.Error() == ErrorCode::OutOfMemory);
    }
    bool notDurable = false;
    AssetAliasPruneReport brokenReport(env.allocator);
    failure = AssetDiagnostic{};
    CHECK(!service.AliasRemove(scene.lock, "/Game/Old/Hero", notDurable, &failure) &&
          failure.code == AssetDiagnosticCode::CorruptAliasTable);
    failure = AssetDiagnostic{};
    CHECK(!service.AliasPrune(scene.lock, brokenReport, &failure) &&
          failure.code == AssetDiagnosticCode::CorruptAliasTable);

    // 올바른 별칭 표(서비스는 실패한 적재를 다음 호출에 다시 읽는다).
    scene.files.Remove(AliasFile);
    auto aliases = service.Aliases();
    REQUIRE(aliases);
    const AssetAliasInput adds[] = {{"/Game/Old/Hero", FixedId(HeroId), At(clock.now), 90},
                                    {"/Game/Hero", FixedId(BaseId), At(clock.now), 90},
                                    {"/Game/Taken", FixedId(VillainId), At(clock.now), 1}};
    REQUIRE(aliases.Value()->Write(scene.files, scene.lock, {}, adds, notDurable));

    // 열기가 옛 경로를 푼다(경고).
    warnings.Clear();
    out.Clear();
    REQUIRE(service.Info("/Game/Old/Hero", AssetInfoOptions{}, out, &warnings));
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].kind == EditorWarningKind::AliasResolved &&
          warnings[0].diagnostic.asset == FixedId(HeroId) &&
          aliases.Value()->At(warnings[0].diagnostic.detail).path.View() == "/Game/Old/Hero" &&
          warnings[0].Text() == "/Game/Old/Hero");
    // 경고 목록이 없어도 푼다.
    REQUIRE(service.Info("/Game/Old/Hero", AssetInfoOptions{}, out));
    // 색인에 같은 경로가 있으면 색인이 이긴다(경고 없음).
    warnings.Clear();
    REQUIRE(service.Info("/Game/Hero", AssetInfoOptions{}, out, &warnings));
    CHECK(warnings.IsEmpty());
    // 없는 경로는 NotFound(진단 None).
    failure = AssetDiagnostic{};
    auto missing = service.Info("/Game/Nowhere", AssetInfoOptions{}, out, nullptr, &failure);
    CHECK(!missing && missing.Error() == ErrorCode::NotFound &&
          failure.code == AssetDiagnosticCode::None);

    // 색인에 있지만 파일이 사라진 경로(MissingFile)는 별칭으로 가지 않는다.
    REQUIRE(scene.files.AddFile("Content/Lost.rasset", sources.leaf.View()));
    AssetService lostService(env.allocator, scene.layout, config);
    REQUIRE(lostService.Initialize());
    scene.files.Remove("Content/Lost.rasset");
    failure = AssetDiagnostic{};
    auto lost = lostService.Info("/Game/Lost", AssetInfoOptions{}, out, &warnings, &failure);
    CHECK(!lost && failure.code == AssetDiagnosticCode::MissingFile);

    // 경고를 붙이지 못하면(할당 실패) 열기가 실패한다.
    test::NFailAllocator warningAllocator(env.allocator);
    Array<EditorWarning> failingWarnings(warningAllocator);
    warningAllocator.FailOnAttempt(1);
    auto unwarned = service.Info("/Game/Old/Hero", AssetInfoOptions{}, out, &failingWarnings);
    warningAllocator.DisableFailure();
    CHECK(!unwarned && unwarned.Error() == ErrorCode::OutOfMemory);

    // 참조 텍스트도 별칭으로 푼다(asset-set의 Hard 참조 값).
    Array<TextAssignment> assignments(env.allocator);
    TextAssignment assignment;
    REQUIRE(reflection_text::MakeTextAssignment("hard=/Game/Old/Hero", 0, assignment));
    REQUIRE(assignments.TryPushBack(assignment));
    warnings.Clear();
    REQUIRE(service.Set(scene.lock, "/Game/Villain", assignments.View(), false, diagnostics,
                        &warnings));
    bool resolved = false;
    for (const EditorWarning& warning : warnings) {
        resolved = resolved || warning.kind == EditorWarningKind::AliasResolved;
    }

    CHECK(resolved);

    // 별칭 경로에 새 패키지는 점유.
    failure = AssetDiagnostic{};
    auto occupied =
        service.New(scene.lock, "TestAsset", "/Game/TAKEN", {}, diagnostics, &warnings, &failure);
    CHECK(!occupied && occupied.Error() == ErrorCode::AlreadyExists &&
          failure.code == AssetDiagnosticCode::DuplicatePath &&
          failure.related == FixedId(VillainId));
    CHECK(!service.New(scene.lock, "TestAsset", "/Game/Taken", {}, diagnostics));

    // remove.
    failure = AssetDiagnostic{};
    auto invalid = service.AliasRemove(scene.lock, "Game/Taken", notDurable, &failure);
    CHECK(!invalid && failure.code == AssetDiagnosticCode::InvalidPath);
    auto absent = service.AliasRemove(scene.lock, "/Game/None", notDurable, &failure);
    CHECK(!absent && absent.Error() == ErrorCode::NotFound);
    REQUIRE(service.AliasRemove(scene.lock, "/Game/TAKEN", notDurable));
    CHECK(aliases.Value()->Count() == 2);
    REQUIRE(service.New(scene.lock, "TestAsset", "/Game/Taken", {}, diagnostics, &warnings));

    // prune(설정 시계 — 91일 뒤 둘 다 만료, Config에 쓰인 곳 없음).
    clock.Advance(91 * Day);
    AssetAliasPruneReport report(env.allocator);
    REQUIRE(service.AliasPrune(scene.lock, report));
    CHECK(report.removed == 2 && aliases.Value()->Count() == 0);

    // 서비스 할당 실패(시계 없음 — 시스템 시계): 별칭 해석으로 열기·remove·prune.
    // 만든 시각은 먼 과거다(시스템 시계로 판정해도 만료).
    const DateTime past = Time("2000-01-01T00:00:00Z");
    const AssetAliasInput later[] = {{"/Game/Old/Base", FixedId(BaseId), past, 1},
                                     {"/Game/Old/Leaf", FixedId(BaseId), past, 1}};
    REQUIRE(aliases.Value()->Write(scene.files, scene.lock, {}, later, notDurable));
    test::NFailAllocator failing(env.allocator);
    AssetServiceConfig plain = config;
    plain.clock = nullptr;
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetService attempted(failing, scene.layout, plain);
        REQUIRE(attempted.Initialize());
        String text(env.allocator);
        failing.FailOnAttempt(attempt);
        Array<EditorWarning> loopWarnings(env.allocator);
        Result<void> step =
            attempted.Info("/Game/Old/Base", AssetInfoOptions{}, text, &loopWarnings);
        bool removedOne = false;
        if (step) {
            step = attempted.AliasRemove(scene.lock, "/Game/Old/Leaf", removedOne);
        }

        AssetAliasPruneReport attemptReport(env.allocator);
        if (step) {
            step = attempted.AliasPrune(scene.lock, attemptReport);
        }

        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(step);
            CHECK(attemptReport.removed == 1);
            break;
        }

        CHECK(!step && step.Error() == ErrorCode::OutOfMemory);
        // 지운 항목이 있으면 다음 시도를 위해 되돌린다.
        AssetAliasTable reset(env.allocator, scene.layout);
        REQUIRE(reset.Load(scene.files));
        REQUIRE(reset.Write(scene.files, scene.lock, {}, later, notDurable));
        ++failures;
    }

    CHECK(failures > 0);

    // 고아 임시 파일 정리가 별칭 표 임시 파일도 지운다(Config 바로 아래만).
    REQUIRE(scene.files.AddFile("Config/AssetAliases.json.0123abcd-0123-4567-89ab-0123456789ab.tmp",
                                Span<const uint8>()));
    REQUIRE(scene.files.AddDirectory("Config/Nested"));
    REQUIRE(scene.files.AddFile(
        "Config/Nested/AssetAliases.json.0123abcd-0123-4567-89ab-0123456789ab.tmp",
        Span<const uint8>()));
    usize removed = 0;
    Array<TemporaryFileFailure> leftovers(env.allocator);
    REQUIRE(DeleteTemporaryOrphans(scene.files, scene.layout, scene.lock, removed, leftovers));
    CHECK(removed == 1 &&
          !scene.files.Has("Config/AssetAliases.json.0123abcd-0123-4567-89ab-0123456789ab.tmp") &&
          scene.files.Has(
              "Config/Nested/AssetAliases.json.0123abcd-0123-4567-89ab-0123456789ab.tmp"));
}

} // namespace asset_alias_table_tests_file
