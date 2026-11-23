#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "AssetDatabaseTestSupport.h"
#include "AssetIndexCompare.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetAliasTable.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <Core/Time/DateTime.h>
#include <TestSupport/NFailAllocator.h>

// 이동 시험이다(Docs/Design/AssetSystem 4.4절, 7.1절 17 A2e2): 색인 이름 바꾸기(RefreshPaths
// moves — 파일·디렉터리·대소문자만·없는 원본·진단 이름·규칙 위반·할당 실패, 결과는 이동 뒤 새
// 검색과 같다), 서비스 이동(파일·디렉터리·열린 패키지·저장하지 않은 새 패키지·빈 디렉터리·
// 대소문자만·자기 별칭 자리로 되돌아가기, 모든 거부와 실패 — 파일·별칭·색인 그대로, OS 단계 실패의
// 별칭 되돌리기·되돌리기 실패 경고·NotDurable·할당 실패), 실제 디스크 디렉터리 이동(열린 bulk).
using namespace rocks;
namespace asset_move_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;
using reflection_text::TextAssignment;
using reflection_text::TextDiagnostics;

namespace {

constexpr StringView AliasFile = "Config/AssetAliases.json";
// 논리 경로 상한(MaxLogicalPathBytes)을 디렉터리 아래 이름에서만 넘기는 대상이다: 구성 요소
// (MaxNameComponentBytes 안) LongComponents개 — "/Game/" + 4 × 60 + 3 = 249 byte는 상한 안,
// 그 아래 "/Sub/Leaf"(258)·"/Nested/Leaf"(261)는 밖, "/Hero"(254)는 안.
constexpr usize LongComponentBytes = 60;
constexpr usize LongComponents = 4;

PackageReadOptions ReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    options.engineVersion = TestEngineVersion;
    return options;
}

int64 StartTicks() noexcept {
    auto parsed = TryParseIso8601("2026-11-22T00:00:00Z");
    CHECK(parsed);
    return parsed ? parsed.Value().Ticks() : 0;
}

Span<const uint8> BytesOf(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

// 이동 장면이다: 루트는 디스크 임시 디렉터리(잠금), 파일은 그 루트에 놓인 메모리 파일 시스템 —
// Content에 Base·Villain·Dir/Hero(bulk)·Dir/Sub/Leaf, Config, 잡은 잠금.
struct MoveScene {
    MoveScene(AssetEnv& env, Sources& sources, const char* tag) noexcept
        : root(tag), layout(env.allocator), files(env.allocator, Initialized(layout, root)),
          hooks(env.allocator, env.types), clock(StartTicks()) {
        ready = root.made && !layout.Root().IsEmpty() && files.Ready() &&
                files.AddDirectory("Content") && files.AddDirectory("Config") &&
                files.AddDirectory("Content/Dir") && files.AddDirectory("Content/Dir/Sub") &&
                files.AddFile("Content/Base.rasset", sources.base.View()) &&
                files.AddFile("Content/Derived.rasset", sources.derived.View()) &&
                files.AddFile("Content/Villain.rasset", sources.villain.View()) &&
                files.AddFile("Content/Dir/Hero.rasset", sources.hero.View()) &&
                files.AddFile("Content/Dir/Sub/Leaf.rasset", sources.leaf.View()) &&
                static_cast<bool>(lock.Acquire(layout, 0));
    }

    static StringView Initialized(ProjectLayout& layout, const TempProject& project) noexcept {
        return layout.Initialize(project.Root()) ? layout.Root() : project.Root();
    }

    AssetServiceConfig Config(AssetEnv& env) noexcept {
        AssetServiceConfig config;
        config.packages.types = &env.types;
        config.packages.files = &files;
        config.packages.hooks = &hooks;
        config.packages.read.engineVersion = TestEngineVersion;
        config.packages.registryContext = ContextId{0x4d4f5645ull, 1};
        config.packages.registryOptions.names = &env.names;
        config.clock = &clock;
        return config;
    }

    StringView AliasText() const noexcept {
        const Span<const uint8> bytes = files.Bytes(AliasFile);
        return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
    }

    // relative 파일을 bytes로 바꾼다(fault 주입).
    bool Put(StringView relative, Span<const uint8> bytes,
             MemoryFault fault = MemoryFault::None) noexcept {
        files.Remove(relative);
        return files.AddFile(relative, bytes, fault);
    }

    // Content 상대 이름 from을 to로 옮긴다(대상 부모 디렉터리를 만든다 — OS 이동 단계).
    bool MoveOnDisk(Allocator& allocator, StringView from, StringView to) noexcept {
        String fromFull(allocator);
        String toFull(allocator);
        const StringView fromPieces[] = {layout.ContentDirectory(), "/", from};
        const StringView toPieces[] = {layout.ContentDirectory(), "/", to};
        for (StringView piece : fromPieces) {
            if (!fromFull.TryAppend(piece)) {
                return false;
            }
        }

        for (StringView piece : toPieces) {
            if (!toFull.TryAppend(piece)) {
                return false;
            }
        }

        usize slash = toFull.View().ByteCount();
        while (slash > 0 && toFull.View().Data()[slash - 1] != '/') {
            --slash;
        }

        return platform::MakeDirectoryTree(files, toFull.View().Data(), slash - 1) &&
               files.Move(fromFull.View().Data(), fromFull.View().ByteCount(), toFull.View().Data(),
                          toFull.View().ByteCount(), false);
    }

    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetTypeHookTable hooks;
    ManualAssetClock clock;
    AssetDatabaseLock lock;
    bool ready = false;
};

// 색인이 지금 파일 상태의 새 검색과 같은지다.
bool Matches(AssetEnv& env, MoveScene& scene, const AssetIndex& index) noexcept {
    AssetIndex fresh(env.allocator, scene.layout);
    return fresh.Rebuild(scene.files, ReadOptions()) && SameIndex(fresh, index);
}

usize AliasCount(AssetService& service) noexcept {
    auto aliases = service.Aliases();
    return aliases ? aliases.Value()->Count() : 0;
}

AssetId AliasTarget(AssetEnv& env, AssetService& service, StringView path) noexcept {
    auto aliases = service.Aliases();
    auto parsed = ParseLogicalPath(env.allocator, path);
    if (!aliases || !parsed) {
        return AssetId{};
    }

    const usize at = aliases.Value()->Find(parsed.Value().Key());
    return at == AssetAliasNone ? AssetId{} : aliases.Value()->At(at).asset;
}

Result<ObjectHandle> CreateUnsaved(AssetEnv& env, AssetService& service, StringView path,
                                   uint64 id) noexcept {
    auto parsed = ParseLogicalPath(env.allocator, path);
    if (!parsed) {
        return Result<ObjectHandle>::Failure(parsed.Error());
    }

    return service.Packages().Create(parsed.Value(), IdOf<TestAsset>(), FixedId(id));
}

} // namespace

// 색인 이름 바꾸기: 디렉터리(중첩·손상 파일·열거 실패 하위 디렉터리 — 진단 이름도 새 이름)·파일·
// 대소문자만·정규화 대조로 부른 이동·없는 원본을 이동 전에 준비하고 OS 이동 뒤 확정하면 새 검색과
// 같다. 규칙 위반(이름·서로의 아래)은 InvalidArgument, 할당 실패는 OutOfMemory이고 색인 그대로.
TEST(AssetMove_IndexRenames) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    MoveScene scene(env, sources, "move-index");
    REQUIRE(scene.ready);
    const uint8 junk[] = {1, 2, 3};
    REQUIRE(scene.files.AddFile("Content/Dir/Junk.rasset", Span<const uint8>(junk, 3)));
    REQUIRE(scene.files.AddDirectory("Content/Dir/Broken", false, MemoryFault::Enumerate));
    // 잘못된 UTF-8 이름(POSIX 열거 — 디렉터리·패키지 파일은 진단, 그 밖의 파일은 무시).
    const char badPackage[] = "Content/Dir/Bad\xff.rasset";
    const char badDirectory[] = "Content/Dir/Sub\xc3";
    const char badText[] = "Content/Dir/Bad\xfe.txt";
    REQUIRE(
        scene.files.AddFile(StringView(badPackage, sizeof(badPackage) - 1), sources.cycleA.View()));
    REQUIRE(scene.files.AddDirectory(StringView(badDirectory, sizeof(badDirectory) - 1)));
    REQUIRE(scene.files.AddFile(StringView(badText, sizeof(badText) - 1), BytesOf("x")));
    // 이름 바꾸기의 시작 이름 종류: 링크(그 아래 없음)·패키지가 아닌 파일(무시), 한 가지 진단만 든
    // 디렉터리(이름 할당이 그 진단에서 처음 일어난다 — 할당 실패 시험).
    REQUIRE(scene.files.AddDirectory("Content/Linked", true));
    REQUIRE(scene.files.AddFile("Content/Notes.txt", BytesOf("notes")));
    REQUIRE(scene.files.AddDirectory("Content/Lone"));
    REQUIRE(scene.files.AddDirectory("Content/Lone/UnreadableDirectoryWithALongName", false,
                                     MemoryFault::Enumerate));
    const char oddPackage[] = "Content/Odd/AnInvalidPackageNameWithALongTail\xff.rasset";
    REQUIRE(scene.files.AddDirectory("Content/Odd"));
    REQUIRE(
        scene.files.AddFile(StringView(oddPackage, sizeof(oddPackage) - 1), sources.cycleB.View()));
    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, ReadOptions()));
    auto moveAndCommit = [&](StringView from, StringView to, bool normalizes,
                             bool exists) noexcept {
        const AssetIndexMove move{from, to};
        AssetIndexRefreshPaths paths;
        paths.moves = Span<const AssetIndexMove>(&move, 1);
        paths.normalizesNames = normalizes;
        AssetIndexRefresh plan(env.allocator);
        if (!index.PrepareRefreshPaths(scene.files, paths, ReadOptions(), plan)) {
            return false;
        }

        if (exists && !scene.MoveOnDisk(env.allocator, from, to)) {
            return false;
        }

        index.CommitRefresh(plan);
        return Matches(env, scene, index);
    };

    // 디렉터리(진단 이름도 새 이름 아래로 — 손상 파일·열거 실패·잘못된 UTF-8 이름 둘).
    CHECK(moveAndCommit("Dir", "Place/Dir2", false, true));
    usize renamed = 0;
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        CHECK(!diagnostic.fileName.View().StartsWith("Dir/", StringComparison::CaseSensitive));
        renamed +=
            diagnostic.fileName.View().StartsWith("Place/Dir2/", StringComparison::CaseSensitive)
                ? 1
                : 0;
    }

    CHECK(renamed == 4);
    // 파일, 대소문자만(같은 키), 정규화 대조로 부른 이동, 없는 원본(아무것도 읽지 않음), 링크(그
    // 아래를 따라가지 않는다), 패키지가 아닌 파일.
    CHECK(moveAndCommit("Base.rasset", "Moved.rasset", false, true));
    CHECK(moveAndCommit("Place/Dir2/Hero.rasset", "Place/Dir2/hero.rasset", true, true));
    CHECK(moveAndCommit("Gone.rasset", "Here.rasset", false, false));
    CHECK(moveAndCommit("Linked", "Linked2", false, true));
    CHECK(moveAndCommit("Notes.txt", "Notes2.txt", false, true));
    // 같은 이름(같은 키 — 서로의 아래 판정을 하지 않는다)은 그 이름을 다시 읽는 것과 같다.
    CHECK(moveAndCommit("Moved.rasset", "Moved.rasset", false, false));
    CHECK(moveAndCommit("Place", "Place", true, false));

    // 시작 이름의 Stat 실패(없음 밖)는 새 이름의 진단(None과 그 오류)이고 그 아래를 빼지 않고 둔다
    // — 이동하지 않은 색인 사본에서 본다.
    {
        AssetIndex copy(env.allocator, scene.layout);
        REQUIRE(copy.Rebuild(scene.files, ReadOptions()));
        const AssetIndexMove move{"Moved.rasset", "Faulty.rasset"};
        AssetIndexRefreshPaths paths;
        paths.moves = Span<const AssetIndexMove>(&move, 1);
        scene.files.faults.stat = MemoryBusy;
        scene.files.faults.statPath = "Content/Moved.rasset";
        auto refreshed = copy.RefreshPaths(scene.files, paths, ReadOptions());
        scene.files.faults.stat = {};
        scene.files.faults.statPath = nullptr;
        REQUIRE(refreshed);
        bool faulty = false;
        for (const AssetIndexDiagnostic& diagnostic : copy.Diagnostics()) {
            faulty = faulty || (diagnostic.fileName.View() == "Faulty.rasset" &&
                                diagnostic.code == AssetDiagnosticCode::None &&
                                diagnostic.error == ErrorCode::Busy);
        }

        auto parsed = ParseLogicalPath(env.allocator, "/Game/Moved");
        REQUIRE(parsed);
        CHECK(faulty && !copy.FindByPath(parsed.Value()));
    }

    // 규칙 위반: 이름 규칙·서로의 아래(같은 키가 아니면).
    AssetIndex before(env.allocator, scene.layout);
    REQUIRE(before.Rebuild(scene.files, ReadOptions()));
    REQUIRE(SameIndex(before, index));
    const AssetIndexMove bad[] = {
        {"", "A"}, {"A", "/B"}, {"Place", "Place/In"}, {"Place/In", "Place"}, {"A/../B", "C"}};
    for (const AssetIndexMove& move : bad) {
        AssetIndexRefreshPaths paths;
        paths.moves = Span<const AssetIndexMove>(&move, 1);
        auto rejected = index.RefreshPaths(scene.files, paths, ReadOptions());
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument &&
              SameIndex(before, index));
    }

    // 할당 실패: 디렉터리(항목·진단 — 정규화 대조), 파일, 열거 실패 진단뿐인 디렉터리, 잘못된
    // UTF-8 이름뿐인 디렉터리, Stat 실패한 시작 이름(각 장면에서 이름 할당이 처음 일어나는 곳이
    // 다르다).
    struct FailureCase {
        AssetIndexMove move;
        bool normalizes;
        const char* statFault;
    };
    const FailureCase failureCases[] = {
        {{"Place", "Elsewhere"}, true, nullptr},
        {{"Moved.rasset", "Other.rasset"}, false, nullptr},
        {{"Lone", "Lone2"}, false, nullptr},
        {{"Odd", "Odd2"}, false, nullptr},
        {{"Moved.rasset", "Faulty.rasset"}, false, "Content/Moved.rasset"}};
    test::NFailAllocator failing(env.allocator);
    for (const FailureCase& failureCase : failureCases) {
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            AssetIndex attempted(failing, scene.layout);
            REQUIRE(attempted.Rebuild(scene.files, ReadOptions()));
            AssetIndexRefreshPaths paths;
            paths.moves = Span<const AssetIndexMove>(&failureCase.move, 1);
            paths.normalizesNames = failureCase.normalizes;
            AssetIndexRefresh plan(failing);
            if (failureCase.statFault != nullptr) {
                scene.files.faults.stat = MemoryBusy;
                scene.files.faults.statPath = failureCase.statFault;
            }

            failing.FailOnAttempt(attempt);
            auto prepared = attempted.PrepareRefreshPaths(scene.files, paths, ReadOptions(), plan);
            const bool triggered = failing.Attempts() >= attempt;
            failing.DisableFailure();
            scene.files.faults.stat = {};
            scene.files.faults.statPath = nullptr;
            if (!triggered) {
                REQUIRE(prepared);
                break;
            }

            CHECK(!prepared && prepared.Error() == ErrorCode::OutOfMemory &&
                  SameIndex(before, attempted));
            ++failures;
        }

        CHECK(failures > 0);
    }
}

// 서비스 이동: 파일(별칭·옛 경로 해석), 자기 별칭 자리로 되돌아가기(그 별칭을 지움), 디렉터리(열린
// 패키지 — Detach·새 경로 저장, 저장하지 않은 새 패키지 — 새 경로에 첫 저장, 하위 디렉터리),
// 대소문자만(별칭 없음), 디스크에 없는 디렉터리(새 패키지뿐 — OS 이동 없음), 저장하지 않은 새
// 패키지 하나, 빈 디렉터리. 매번 색인은 새 검색과 같다.
TEST(AssetMove_MovesPackagesAndDirectories) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    MoveScene scene(env, sources, "move-service");
    REQUIRE(scene.ready);
    // 이름이 Dir로 시작하지만 그 아래가 아닌 형제(DirX — 열린 패키지, DirNew — 새 패키지).
    REQUIRE(scene.files.AddFile("Content/DirX.rasset", sources.cycleA.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    AssetMoveResult result;
    Array<EditorWarning> warnings(env.allocator);

    // 파일.
    REQUIRE(service.Move(scene.lock, "/Game/Base", "/Game/Moved/Base2", result, &warnings));
    CHECK(result.packages == 1 && result.aliases == 1 && warnings.IsEmpty());
    CHECK(!scene.files.Has("Content/Base.rasset") && scene.files.Has("Content/Moved/Base2.rasset"));
    CHECK(Matches(env, scene, service.Index()));
    CHECK(AliasTarget(env, service, "/Game/Base") == FixedId(BaseId));
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Base", AssetInfoOptions{}, out, &warnings));
    CHECK(warnings.Size() == 1 && warnings[0].kind == EditorWarningKind::AliasResolved);
    warnings.Clear();

    // 자기 별칭 자리로 되돌아가기: /Game/Base 별칭(자신)을 지우고 /Game/Moved/Base2 별칭을 더한다.
    REQUIRE(service.Move(scene.lock, "/Game/Moved/Base2", "/Game/Base", result, &warnings));
    CHECK(result.aliases == 1 && AliasTarget(env, service, "/Game/Base").IsNull() &&
          AliasTarget(env, service, "/Game/Moved/Base2") == FixedId(BaseId) &&
          scene.files.Has("Content/Base.rasset") && Matches(env, scene, service.Index()));

    // 디렉터리: 열린 Hero(Detach)와 저장하지 않은 새 패키지.
    REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out));
    REQUIRE(CreateUnsaved(env, service, "/Game/Dir/Fresh", 0xd01));
    REQUIRE(service.Info("/Game/DirX", AssetInfoOptions{}, out));
    REQUIRE(CreateUnsaved(env, service, "/Game/DirNew", 0xd04));
    REQUIRE(service.Move(scene.lock, "/Game/Dir", "/Game/Place/Dir2", result, &warnings));
    CHECK(result.packages == 3 && result.aliases == 2);
    CHECK(scene.files.Has("Content/DirX.rasset") &&
          service.Packages().Find(FixedId(CycleAId))->fileName.View() == "DirX.rasset" &&
          service.Packages().Find(FixedId(0xd04))->logicalPath.View() == "/Game/DirNew");
    CHECK(scene.files.Has("Content/Place/Dir2/Hero.rasset") &&
          scene.files.Has("Content/Place/Dir2/Sub/Leaf.rasset") && !scene.files.Has("Content/Dir"));
    CHECK(AliasTarget(env, service, "/Game/Dir/Hero") == FixedId(HeroId) &&
          AliasTarget(env, service, "/Game/Dir/Sub/Leaf") == FixedId(LeafId) &&
          AliasTarget(env, service, "/Game/Dir/Fresh").IsNull());
    const EditorPackageInfo* hero = service.Packages().Find(FixedId(HeroId));
    REQUIRE(hero != nullptr);
    CHECK(hero->fileName.View() == "Place/Dir2/Hero.rasset" && hero->logicalPath.IsEmpty() &&
          !hero->store->IsAttached());
    // 열린 Base(별칭으로 연 — 옮기지 않은 패키지)는 그대로다.
    CHECK(service.Packages().Find(FixedId(BaseId))->fileName.View() == "Base.rasset");
    const EditorPackageInfo* unsaved = service.Packages().Find(FixedId(0xd01));
    REQUIRE(unsaved != nullptr);
    CHECK(unsaved->unsaved && unsaved->fileName.IsEmpty() &&
          unsaved->logicalPath.View() == "/Game/Place/Dir2/Fresh");
    CHECK(Matches(env, scene, service.Index()));
    // 새 경로에 저장한다(열린 패키지 — 옮긴 파일을 덮는다, 새 패키지 — 첫 저장).
    Array<TextAssignment> assignments(env.allocator);
    TextAssignment assignment;
    REQUIRE(reflection_text::MakeTextAssignment("value=11", 0, assignment));
    REQUIRE(assignments.TryPushBack(assignment));
    TextDiagnostics diagnostics(env.allocator);
    REQUIRE(
        service.Set(scene.lock, "/Game/Place/Dir2/Hero", assignments.View(), false, diagnostics));
    EditorSaveOptions save;
    REQUIRE(service.Packages().Save(FixedId(0xd01), scene.lock, save));
    CHECK(scene.files.Has("Content/Place/Dir2/Fresh.rasset") && !scene.files.Has("Content/Dir"));
    CHECK(Matches(env, scene, service.Index()));

    // 대소문자만 바꾸기(별칭 없음).
    REQUIRE(service.Move(scene.lock, "/Game/Place/Dir2", "/Game/Place/dir2", result, &warnings));
    CHECK(result.packages == 3 && result.aliases == 0 &&
          scene.files.Has("Content/Place/dir2/Hero.rasset") &&
          service.Packages().Find(FixedId(HeroId))->fileName.View() == "Place/dir2/Hero.rasset");
    CHECK(Matches(env, scene, service.Index()));

    // 디스크에 없는 디렉터리(새 패키지뿐)와 새 패키지 하나: OS 이동이 없다.
    REQUIRE(CreateUnsaved(env, service, "/Game/Lonely/New", 0xd02));
    REQUIRE(CreateUnsaved(env, service, "/Game/One", 0xd03));
    const uint32 moves = scene.files.moves;
    REQUIRE(service.Move(scene.lock, "/Game/Lonely", "/Game/Alone", result, &warnings));
    CHECK(result.packages == 1 && result.aliases == 0 &&
          service.Packages().Find(FixedId(0xd02))->logicalPath.View() == "/Game/Alone/New");
    REQUIRE(service.Move(scene.lock, "/Game/One", "/Game/Two", result, &warnings));
    CHECK(result.packages == 1 && result.aliases == 0 &&
          service.Packages().Find(FixedId(0xd03))->logicalPath.View() == "/Game/Two" &&
          scene.files.moves == moves);

    // 빈 디렉터리(패키지 없음)도 옮긴다.
    REQUIRE(scene.files.AddDirectory("Content/Empty"));
    REQUIRE(service.Move(scene.lock, "/Game/Empty", "/Game/Full", result, &warnings));
    CHECK(result.packages == 0 && scene.files.Has("Content/Full") &&
          !scene.files.Has("Content/Empty") && Matches(env, scene, service.Index()));
    CHECK(warnings.IsEmpty());
}

// 거부와 실패: 파일·별칭 표·색인이 그대로다. Initialize 전·잠금 아님·경로 규칙·대상이 원본 아래·
// 원본 없음·원본 경로 키 둘·대상이 차 있음(색인 항목·색인 디렉터리·새 패키지·디스크 파일·다른 에셋
// 별칭)·새 경로 규칙(길이)·대상 Stat 실패·원본 Stat 실패·색인 항목의 파일이 사라짐(MissingFile)·
// 별칭 표 손상·Detach 실패·별칭 쓰기 실패. OS 단계 실패(부모 디렉터리 만들기·Move)는 별칭을
// 되돌리고, 되돌리기가 실패하면 경고 AliasNotRestored다. 별칭 NotDurable은 성공 경고.
TEST(AssetMove_RejectsAndRestores) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    MoveScene scene(env, sources, "move-reject");
    REQUIRE(scene.ready);
    const uint8 junk[] = {1, 2, 3};
    REQUIRE(scene.files.AddFile("Content/Junky.rasset", Span<const uint8>(junk, 3)));
    REQUIRE(scene.files.AddFile("Content/FileParent", Span<const uint8>(junk, 3)));
    // 대소문자만 다른 OS 디렉터리 둘에 걸친 논리 디렉터리와 그 안의 같은 경로 키 둘(대소문자 구분
    // FS).
    REQUIRE(scene.files.AddDirectory("Content/Split") && scene.files.AddDirectory("Content/split"));
    REQUIRE(scene.files.AddFile("Content/Split/Dup.rasset", sources.cycleA.View()));
    REQUIRE(scene.files.AddFile("Content/split/dup.rasset", sources.cycleB.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    AssetMoveResult result;
    AssetDiagnostic failure;
    auto early = service.Move(scene.lock, "/Game/Base", "/Game/X", result, nullptr, &failure);
    CHECK(!early && early.Error() == ErrorCode::InvalidArgument);
    REQUIRE(service.Initialize());
    // 다른 에셋의 별칭(/Game/Aliased → Villain).
    {
        auto aliases = service.Aliases();
        auto created = DateTime::TryFromTicks(scene.clock.now);
        REQUIRE(aliases && created);
        const AssetAliasInput add[] = {{"/Game/Aliased", FixedId(VillainId), created.Value(), 1}};
        bool notDurable = false;
        REQUIRE(aliases.Value()->Write(scene.files, scene.lock, {}, add, notDurable));
    }

    String aliasText(env.allocator);
    REQUIRE(aliasText.TryAssign(scene.AliasText()));
    auto unchanged = [&]() noexcept {
        return scene.files.Has("Content/Base.rasset") &&
               scene.files.Has("Content/Dir/Hero.rasset") && AliasCount(service) == 1 &&
               scene.AliasText() == aliasText.View() && Matches(env, scene, service.Index());
    };
    auto rejects = [&](StringView from, StringView to, ErrorCode error,
                       AssetDiagnosticCode code) noexcept {
        failure = AssetDiagnostic{};
        auto moved = service.Move(scene.lock, from, to, result, nullptr, &failure);
        return !moved && moved.Error() == error && failure.code == code && unchanged();
    };

    AssetDatabaseLock other;
    auto unlocked = service.Move(other, "/Game/Base", "/Game/X", result);
    CHECK(!unlocked && unlocked.Error() == ErrorCode::InvalidArgument);
    CHECK(rejects("Game/Base", "/Game/X", ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("/Game/Base", "/Game/Bad Name", ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("/Game/Dir", "/Game/Dir/Sub/Deeper", ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::None));
    CHECK(rejects("/Game/Nowhere", "/Game/X", ErrorCode::NotFound, AssetDiagnosticCode::None));
    CHECK(rejects("/Game/Split/Dup", "/Game/X", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath));
    CHECK(rejects("/Game/Split", "/Game/Joined", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath));
    CHECK(rejects("/Game/Base", "/Game/Dir/Hero", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath) &&
          failure.related == FixedId(HeroId));
    CHECK(rejects("/Game/Base", "/Game/Dir", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath));
    CHECK(rejects("/Game/Base", "/Game/Junky", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath));
    CHECK(rejects("/Game/Base", "/Game/ALIASED", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath) &&
          failure.related == FixedId(VillainId));
    REQUIRE(CreateUnsaved(env, service, "/Game/Taken/New", 0xd10));
    CHECK(rejects("/Game/Base", "/Game/Taken", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath) &&
          failure.related == FixedId(0xd10));
    CHECK(rejects("/Game/Base", "/Game/Taken/New", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath));
    // 새 경로 규칙(길이 — 디렉터리 아래 이름만 상한을 넘는다).
    String longTarget(env.allocator);
    REQUIRE(longTarget.TryAppend("/Game"));
    for (usize component = 0; component < LongComponents; ++component) {
        REQUIRE(longTarget.TryAppend("/"));
        for (usize at = 0; at < LongComponentBytes; ++at) {
            REQUIRE(longTarget.TryAppend("a"));
        }
    }

    REQUIRE(ParseLogicalPath(env.allocator, longTarget.View()));
    CHECK(rejects("/Game/Dir", longTarget.View(), ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidPath) &&
          failure.asset == FixedId(LeafId));

    // 대상·원본 Stat 실패(그 오류, 진단 None).
    const char* const statPaths[] = {"Content/Elsewhere.rasset", "Content/Base.rasset"};
    for (const char* statPath : statPaths) {
        scene.files.faults.stat = MemoryBusy;
        scene.files.faults.statPath = statPath;
        failure = AssetDiagnostic{};
        auto statFailed =
            service.Move(scene.lock, "/Game/Base", "/Game/Elsewhere", result, nullptr, &failure);
        scene.files.faults.stat = {};
        scene.files.faults.statPath = nullptr;
        CHECK(!statFailed && statFailed.Error() == ErrorCode::Busy &&
              failure.code == AssetDiagnosticCode::None && unchanged());
    }

    // 색인 항목의 파일이 사라졌다(MissingFile — 옮길 것이 없다).
    scene.files.Remove("Content/Villain.rasset");
    failure = AssetDiagnostic{};
    auto missing =
        service.Move(scene.lock, "/Game/Villain", "/Game/Elsewhere", result, nullptr, &failure);
    CHECK(!missing && missing.Error() == ErrorCode::NotFound &&
          failure.code == AssetDiagnosticCode::MissingFile && failure.asset == FixedId(VillainId));
    REQUIRE(scene.files.AddFile("Content/Villain.rasset", sources.villain.View()));
    CHECK(unchanged());

    // 별칭 쓰기 실패: 아무것도 옮기지 않는다.
    scene.files.faults.create = MemoryError;
    failure = AssetDiagnostic{};
    auto writeFailed =
        service.Move(scene.lock, "/Game/Base", "/Game/Other", result, nullptr, &failure);
    scene.files.faults.create = {};
    CHECK(!writeFailed && unchanged());

    // OS 단계 실패는 별칭을 되돌린다: 대상 부모가 파일·Move 실패.
    failure = AssetDiagnostic{};
    auto parentFailed =
        service.Move(scene.lock, "/Game/Base", "/Game/FileParent/X", result, nullptr, &failure);
    CHECK(!parentFailed && failure.code == AssetDiagnosticCode::None && unchanged());
    scene.files.faults.move = MemoryBusy;
    Array<EditorWarning> warnings(env.allocator);
    failure = AssetDiagnostic{};
    auto moveFailed =
        service.Move(scene.lock, "/Game/Base", "/Game/Other", result, &warnings, &failure);
    scene.files.faults.move = {};
    CHECK(!moveFailed && moveFailed.Error() == ErrorCode::Busy && warnings.IsEmpty() &&
          unchanged());

    // 되돌리기 실패(별칭 파일이 없던 상태 — 지우기가 실패): 경고 AliasNotRestored. 경고 목록이
    // 없으면 실패만 돌려준다.
    const bool listedModes[] = {true, false};
    for (bool listed : listedModes) {
        AssetService fresh(env.allocator, scene.layout, scene.Config(env));
        scene.files.Remove(AliasFile);
        REQUIRE(fresh.Initialize());
        warnings.Clear();
        scene.files.faults.move = MemoryBusy;
        scene.files.faults.remove = MemoryError;
        auto notRestored = fresh.Move(scene.lock, "/Game/Base", "/Game/Other", result,
                                      listed ? &warnings : nullptr);
        scene.files.faults.move = {};
        scene.files.faults.remove = {};
        CHECK(!notRestored && notRestored.Error() == ErrorCode::Busy);
        CHECK(listed ? warnings.Size() == 1 &&
                           warnings[0].kind == EditorWarningKind::AliasNotRestored &&
                           warnings[0].diagnostic.error != ErrorCode::None
                     : warnings.IsEmpty());
        // 되돌리지 못한 별칭(옮기지 못한 옛 경로 → 그 에셋)은 해롭지 않다 — 색인이 이긴다.
        CHECK(scene.files.Has(AliasFile) && scene.files.Has("Content/Base.rasset"));
        REQUIRE(scene.Put(AliasFile, BytesOf(aliasText.View())));
    }

    CHECK(unchanged());

    // 별칭 표 손상.
    {
        AssetService broken(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(scene.Put(AliasFile, BytesOf("{")));
        REQUIRE(broken.Initialize());
        failure = AssetDiagnostic{};
        auto corrupt =
            broken.Move(scene.lock, "/Game/Base", "/Game/Other", result, nullptr, &failure);
        CHECK(!corrupt && failure.code == AssetDiagnosticCode::CorruptAliasTable);
        CHECK(scene.files.Has("Content/Base.rasset"));
        REQUIRE(scene.Put(AliasFile, BytesOf(aliasText.View())));
    }

    // Detach 실패(열린 Hero의 파일을 읽을 수 없게 된 뒤): 아무것도 옮기지 않는다.
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out));
    REQUIRE(scene.Put("Content/Dir/Hero.rasset", sources.hero.View(), MemoryFault::Read));
    failure = AssetDiagnostic{};
    auto detachFailed =
        service.Move(scene.lock, "/Game/Dir", "/Game/Elsewhere", result, nullptr, &failure);
    CHECK(!detachFailed && scene.files.Has("Content/Dir/Hero.rasset") &&
          !scene.files.Has("Content/Elsewhere") && scene.AliasText() == aliasText.View());
    REQUIRE(scene.Put("Content/Dir/Hero.rasset", sources.hero.View()));
    CHECK(unchanged());

    // 시계가 날짜 범위 밖이면(별칭의 만든 시각) 그 오류다.
    const int64 now = scene.clock.now;
    scene.clock.now = -1;
    failure = AssetDiagnostic{};
    auto badClock =
        service.Move(scene.lock, "/Game/Base", "/Game/Other", result, nullptr, &failure);
    scene.clock.now = now;
    CHECK(!badClock && badClock.Error() == ErrorCode::Overflow &&
          failure.code == AssetDiagnosticCode::None && unchanged());

    // 경고 자리를 잡지 못하면(바꾸기 전) OutOfMemory이고 아무것도 바꾸지 않는다.
    {
        test::NFailAllocator warningAllocator(env.allocator);
        Array<EditorWarning> failingWarnings(warningAllocator);
        warningAllocator.FailOnAttempt(1);
        auto noRoom =
            service.Move(scene.lock, "/Game/Base", "/Game/Other", result, &failingWarnings);
        warningAllocator.DisableFailure();
        CHECK(!noRoom && noRoom.Error() == ErrorCode::OutOfMemory && unchanged());
    }

    // 집합을 바로 부른 경로 바꾸기: 새 패키지의 새 논리 경로가 규칙 밖이면 InvalidPath(그 id)이고
    // 집합 그대로(서비스는 이 판정을 먼저 한다).
    {
        REQUIRE(CreateUnsaved(env, service, "/Game/Deep/Nested/Leaf", 0xd11));
        auto from = ParseLogicalPath(env.allocator, "/Game/Deep");
        auto to = ParseLogicalPath(env.allocator, longTarget.View());
        REQUIRE(from && to);
        EditorRelocation plan(env.allocator);
        failure = AssetDiagnostic{};
        auto prepared =
            service.Packages().PrepareRelocate(from.Value(), to.Value(), {}, {}, plan, &failure);
        CHECK(!prepared && prepared.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidPath && failure.asset == FixedId(0xd11) &&
              service.Packages().Find(FixedId(0xd11))->logicalPath.View() ==
                  "/Game/Deep/Nested/Leaf");
        // 진단 칸 없이도 같은 오류다.
        auto unreported =
            service.Packages().PrepareRelocate(from.Value(), to.Value(), {}, {}, plan);
        CHECK(!unreported && unreported.Error() == ErrorCode::InvalidArgument);
        CHECK(rejects("/Game/Deep", longTarget.View(), ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::InvalidPath) &&
              failure.asset == FixedId(0xd11));
    }

    // 별칭을 쓰지 않는 이동(패키지 없는 디렉터리)의 OS 실패는 되돌릴 별칭이 없다.
    REQUIRE(scene.files.AddDirectory("Content/Bare"));
    scene.files.faults.move = MemoryBusy;
    warnings.Clear();
    auto bareFailed = service.Move(scene.lock, "/Game/Bare", "/Game/Bare2", result, &warnings);
    scene.files.faults.move = {};
    CHECK(!bareFailed && bareFailed.Error() == ErrorCode::Busy && warnings.IsEmpty() &&
          scene.files.Has("Content/Bare") && unchanged());

    // 대상 부모를 만든 뒤 OS Move가 실패하면 만든 부모(빈 디렉터리)는 남는다(해롭지 않다 —
    // 색인에 항목이 없다).
    scene.files.faults.move = MemoryBusy;
    auto parentLeft = service.Move(scene.lock, "/Game/Base", "/Game/NewParent/X", result);
    scene.files.faults.move = {};
    CHECK(!parentLeft && parentLeft.Error() == ErrorCode::Busy &&
          scene.files.Has("Content/NewParent") && unchanged());

    // 저장하지 않은 새 패키지 하나: 원본 이름은 파일 이름이다 — 같은 이름의 디렉터리(패키지 없음)는
    // 옮기지 않고, 색인에 없는 같은 이름 파일(낡은 색인)이 있으면 그 파일을 옮기지 않고 거부한다.
    REQUIRE(CreateUnsaved(env, service, "/Game/Shadow", 0xd13));
    REQUIRE(scene.files.AddDirectory("Content/Shadow"));
    const uint32 movesBefore = scene.files.moves;
    REQUIRE(service.Move(scene.lock, "/Game/Shadow", "/Game/Shadow2", result));
    CHECK(result.packages == 1 && scene.files.moves == movesBefore &&
          scene.files.Has("Content/Shadow") &&
          service.Packages().Find(FixedId(0xd13))->logicalPath.View() == "/Game/Shadow2");
    REQUIRE(CreateUnsaved(env, service, "/Game/Stale", 0xd12));
    REQUIRE(scene.files.AddFile("Content/Stale.rasset", sources.hub.View()));
    failure = AssetDiagnostic{};
    auto stale =
        service.Move(scene.lock, "/Game/Stale", "/Game/Fresher", result, nullptr, &failure);
    CHECK(!stale && stale.Error() == ErrorCode::AlreadyExists &&
          failure.code == AssetDiagnosticCode::DuplicatePath &&
          scene.files.Has("Content/Stale.rasset") && !scene.files.Has("Content/Fresher.rasset") &&
          service.Packages().Find(FixedId(0xd12))->logicalPath.View() == "/Game/Stale");
    scene.files.Remove("Content/Stale.rasset");

    // 별칭 NotDurable은 성공 경고다(경고 목록이 없으면 성공만).
    warnings.Clear();
    scene.files.faults.replace = MemoryNotDurable;
    REQUIRE(service.Move(scene.lock, "/Game/Base", "/Game/Other", result, &warnings));
    REQUIRE(service.Move(scene.lock, "/Game/Other", "/Game/Other2", result));
    scene.files.faults.replace = {};
    CHECK(warnings.Size() == 1 && warnings[0].kind == EditorWarningKind::NotDurable &&
          scene.files.Has("Content/Other2.rasset"));
}

// 할당 실패 장면이다: 고장 없는 서비스로 한 번 준비하고(prefix 이동·빈 디렉터리), 시도마다 새
// 서비스에서 열기·새 패키지를 만든 뒤 from → to를 옮긴다. sourceFile·targetFile은 Content 아래
// 이름(디스크에 없는 이동은 null — 새 패키지 경로로 본다).
struct AllocationCase {
    const char* tag;
    StringView from;
    StringView to;
    const char* sourceFile;
    const char* targetFile;
    usize packages;
    usize aliases;
    StringView prefixFrom;
    StringView prefixTo;
    const char* emptyDirectory;
    bool openHero;
    StringView unsavedPath;
};

constexpr uint64 AllocationUnsavedId = 0xd20;

void RunAllocationCase(AssetEnv& env, Sources& sources, const AllocationCase& testCase) noexcept {
    MoveScene scene(env, sources, testCase.tag);
    REQUIRE(scene.ready);
    if (!testCase.prefixFrom.IsEmpty()) {
        AssetService prepared(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(prepared.Initialize());
        AssetMoveResult prefix;
        REQUIRE(prepared.Move(scene.lock, testCase.prefixFrom, testCase.prefixTo, prefix));
    }

    if (testCase.emptyDirectory != nullptr) {
        REQUIRE(scene.files.AddDirectory(testCase.emptyDirectory));
    }

    String aliasBefore(env.allocator);
    REQUIRE(aliasBefore.TryAssign(scene.AliasText()));
    const bool aliasExisted = scene.files.Has(AliasFile);
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetService service(failing, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        String out(env.allocator);
        if (testCase.openHero) {
            REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out));
        }

        if (!testCase.unsavedPath.IsEmpty()) {
            REQUIRE(CreateUnsaved(env, service, testCase.unsavedPath, AllocationUnsavedId));
        }

        AssetMoveResult result;
        Array<EditorWarning> warnings(env.allocator);
        failing.FailOnAttempt(attempt);
        auto moved = service.Move(scene.lock, testCase.from, testCase.to, result, &warnings);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        const EditorPackageInfo* unsaved = service.Packages().Find(FixedId(AllocationUnsavedId));
        if (!triggered) {
            REQUIRE(moved);
            CHECK(result.packages == testCase.packages && result.aliases == testCase.aliases &&
                  warnings.IsEmpty() && Matches(env, scene, service.Index()));
            CHECK(testCase.targetFile == nullptr ||
                  (scene.files.Has(testCase.targetFile) && !scene.files.Has(testCase.sourceFile)));
            CHECK(unsaved == nullptr || unsaved->logicalPath.View().StartsWith(
                                            testCase.to, StringComparison::CaseSensitive));
            break;
        }

        CHECK(!moved && moved.Error() == ErrorCode::OutOfMemory && warnings.IsEmpty());
        CHECK(scene.files.Has(AliasFile) == aliasExisted &&
              scene.AliasText() == aliasBefore.View() && Matches(env, scene, service.Index()));
        CHECK(testCase.sourceFile == nullptr ||
              (scene.files.Has(testCase.sourceFile) && !scene.files.Has(testCase.targetFile)));
        CHECK(unsaved == nullptr || unsaved->logicalPath.View() == testCase.unsavedPath);
        ++failures;
    }

    CHECK(failures > 0);
}

// 할당 실패: 디렉터리(열린 패키지·저장하지 않은 새 패키지·하위 디렉터리), 파일, 자기 별칭 자리로
// 되돌아가기(별칭 지우기), 디스크에 없는 디렉터리(새 패키지뿐), 빈 디렉터리 — 모든 할당 실패는
// OutOfMemory이고 파일·별칭 파일·색인·집합 경로가 그대로다. 실패가 없어지면 끝까지 옮긴다.
TEST(AssetMove_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    const AllocationCase cases[] = {
        {"move-oom-directory",
         "/Game/Dir",
         "/Game/Place",
         "Content/Dir",
         "Content/Place",
         3,
         2,
         {},
         {},
         nullptr,
         true,
         "/Game/Dir/Fresh"},
        {"move-oom-file",
         "/Game/Base",
         "/Game/Sub/Base2",
         "Content/Base.rasset",
         "Content/Sub/Base2.rasset",
         1,
         1,
         {},
         {},
         nullptr,
         false,
         {}},
        {"move-oom-back",
         "/Game/Away/Villain",
         "/Game/Villain",
         "Content/Away/Villain.rasset",
         "Content/Villain.rasset",
         1,
         1,
         "/Game/Villain",
         "/Game/Away/Villain",
         nullptr,
         false,
         {}},
        {"move-oom-unsaved",
         "/Game/Lonely",
         "/Game/Alone",
         nullptr,
         nullptr,
         1,
         0,
         {},
         {},
         nullptr,
         false,
         "/Game/Lonely/New"},
        {"move-oom-empty",
         "/Game/Empty",
         "/Game/Full",
         "Content/Empty",
         "Content/Full",
         0,
         0,
         {},
         {},
         "Content/Empty",
         false,
         {}},
    };
    for (const AllocationCase& testCase : cases) {
        RunAllocationCase(env, sources, testCase);
    }
}

// OS 경로 상한(4.2절·7.1절 10): 깊은 프로젝트 루트(디스크 — 잠금 파일)에서 새 이름의 절대 OS
// 경로가 상한을 넘으면 InvalidPath / CapacityExceeded이고 아무것도 바꾸지 않는다 — 대상 이름,
// 패키지가 없는 원본 디렉터리(배치 규칙으로 만든 이름). 짧은 이름은 같은 루트에서 옮겨진다.
TEST(AssetMove_NativePathLimit) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject project("move-limit");
    REQUIRE(project.made);
    // 루트 길이: "/Content/" + ShortNameRoom byte 안의 이름 + ".rasset" + 임시 접미사는 상한 안,
    // LongComponentBytes 이름은 밖.
    constexpr usize ContentPrefixBytes = 9;
    constexpr usize ShortNameRoom = 30;
    const usize rootBytes = MaxNativePathBytes() - TemporaryFileSuffixBytes -
                            EditorPackageExtension.ByteCount() - ContentPrefixBytes - ShortNameRoom;
    String root(env.allocator);
    REQUIRE(MakeDeepRoot(project, rootBytes, root));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(root.View()));
    MemoryFileSystem files(env.allocator, layout.Root());
    String longName(env.allocator);
    for (usize at = 0; at < LongComponentBytes; ++at) {
        REQUIRE(longName.TryAppend("c"));
    }

    String longDirectory(env.allocator);
    String longPath(env.allocator);
    REQUIRE(longDirectory.TryAppend("Content/") && longDirectory.TryAppend(longName.View()));
    REQUIRE(longPath.TryAppend("/Game/") && longPath.TryAppend(longName.View()));
    REQUIRE(files.Ready() && files.AddDirectory("Content") && files.AddDirectory("Config") &&
            files.AddFile("Content/Base.rasset", sources.base.View()) &&
            files.AddDirectory(longDirectory.View()));
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(layout, 0));
    AssetTypeHookTable hooks(env.allocator, env.types);
    AssetServiceConfig config;
    config.packages.types = &env.types;
    config.packages.files = &files;
    config.packages.hooks = &hooks;
    config.packages.read.engineVersion = TestEngineVersion;
    config.packages.registryContext = ContextId{0x4c494d4954ull, 1};
    config.packages.registryOptions.names = &env.names;
    AssetService service(env.allocator, layout, config);
    REQUIRE(service.Initialize());
    AssetMoveResult result;
    AssetDiagnostic failure;
    auto longTarget = service.Move(lock, "/Game/Base", longPath.View(), result, nullptr, &failure);
    CHECK(!longTarget && longTarget.Error() == ErrorCode::CapacityExceeded &&
          failure.code == AssetDiagnosticCode::InvalidPath && files.Has("Content/Base.rasset") &&
          !files.Has("Config/AssetAliases.json"));
    failure = AssetDiagnostic{};
    auto longSource = service.Move(lock, longPath.View(), "/Game/Short", result, nullptr, &failure);
    CHECK(!longSource && longSource.Error() == ErrorCode::CapacityExceeded &&
          failure.code == AssetDiagnosticCode::InvalidPath && files.Has(longDirectory.View()));
    REQUIRE(service.Move(lock, "/Game/Base", "/Game/Short", result));
    CHECK(files.Has("Content/Short.rasset"));
}

// 실제 디스크: 열린 패키지(bulk를 읽어 파일 핸들이 캐시된)가 든 디렉터리를 옮긴다 — Detach가 핸들을
// 닫지 않으면 Windows는 디렉터리 이름을 바꾸지 못한다. 옮긴 뒤 bulk는 메모리에서 읽히고 새 경로에
// 저장되며 옛 경로는 별칭으로 열린다.
TEST(AssetMove_RealDisk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject project("move-disk");
    REQUIRE(project.made);
    REQUIRE(WriteContentFile(env.allocator, project, "Dir/Hero.rasset", sources.hero.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Base.rasset", sources.base.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Derived.rasset", sources.derived.View()));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(layout, 0));
    AssetTypeHookTable hooks(env.allocator, env.types);
    AssetServiceConfig config;
    config.packages.types = &env.types;
    config.packages.files = &platform::PhysicalFileSystem();
    config.packages.hooks = &hooks;
    config.packages.read.engineVersion = TestEngineVersion;
    config.packages.registryContext = ContextId{0x4449534bull, 1};
    config.packages.registryOptions.names = &env.names;
    AssetService service(env.allocator, layout, config);
    REQUIRE(service.Initialize());
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out));
    const EditorPackageInfo* hero = service.Packages().Find(FixedId(HeroId));
    REQUIRE(hero != nullptr && hero->store != nullptr);
    TestAsset* value = AssetOf(service.Packages(), hero->package);
    REQUIRE(value != nullptr);
    auto read = hero->store->Read(value->pixels);
    REQUIRE(read && read.Value().Size() == PixelBytes);
    AssetMoveResult result;
    REQUIRE(service.Move(lock, "/Game/Dir", "/Game/Moved", result));
    CHECK(result.packages == 1 && result.aliases == 1 && !hero->store->IsAttached());
    auto again = hero->store->Read(value->pixels);
    CHECK(again && again.Value().Size() == PixelBytes);
    REQUIRE(service.Packages().MarkDirty(FixedId(HeroId)));
    EditorSaveOptions save;
    REQUIRE(service.Packages().Save(FixedId(HeroId), lock, save));
    String moved(env.allocator);
    String old(env.allocator);
    const StringView movedPieces[] = {project.Root(), "/Content/Moved/Hero.rasset"};
    const StringView oldPieces[] = {project.Root(), "/Content/Dir"};
    for (StringView piece : movedPieces) {
        REQUIRE(moved.TryAppend(piece));
    }

    for (StringView piece : oldPieces) {
        REQUIRE(old.TryAppend(piece));
    }

    bool movedExists = false;
    bool oldExists = true;
    REQUIRE(platform::PhysicalFileSystem().FileExists(moved.View().Data(), moved.View().ByteCount(),
                                                      movedExists));
    REQUIRE(platform::PhysicalFileSystem().FileExists(old.View().Data(), old.View().ByteCount(),
                                                      oldExists));
    CHECK(movedExists && !oldExists);
    // 옛 경로는 별칭으로 열린다.
    Array<EditorWarning> warnings(env.allocator);
    REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out, &warnings));
    CHECK(warnings.Size() == 1 && warnings[0].kind == EditorWarningKind::AliasResolved);
}

} // namespace asset_move_tests_file
