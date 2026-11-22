#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "AssetDatabaseTestSupport.h"
#include "AssetIndexCompare.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetAliasTable.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Time/DateTime.h>
#include <TestSupport/NFailAllocator.h>

// 같은 id 고치기 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 17 A2e4b): 밖에서 복사한 파일을 새
// id로 제자리에 다시 쓰기(헤더 id·자기 Soft·bulk·의존·hook 출력, 색인의 같은 id 진단이 사라짐,
// 남은 같은 id 항목 경로), ImportRecord(소유 기록의 자기 출력만 새 id, 형제는 빠짐 — 쓰기 뒤 색인
// 판정으로 실제 고아만 경고), Hard 순환 묶음 닫기, 모든 거부와 IO 실패(파일·색인 그대로, 연
// 패키지는 닫힘), 할당 실패, 실제 디스크.
using namespace rocks;
namespace asset_fix_id_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;

namespace {

constexpr StringView CopyFile = "Content/Copies/Hero2.rasset";
constexpr StringView LeafCopyFile = "Content/LeafCopy.rasset";
constexpr uint64 SelfishId = 0xe30;
constexpr uint64 UnsavedId = 0xe20;

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

// 고치기 장면이다(복제 장면과 같다): Content에 Base·Derived·Dir/Hero(bulk)·Villain·Leaf, Config,
// 디스크 임시 루트(잠금).
struct FixScene {
    FixScene(AssetEnv& env, Sources& sources, const char* tag) noexcept
        : root(tag), layout(env.allocator), files(env.allocator, Initialized(layout, root)),
          hooks(env.allocator, env.types), clock(StartTicks()) {
        ready = root.made && !layout.Root().IsEmpty() && files.Ready() &&
                files.AddDirectory("Content") && files.AddDirectory("Config") &&
                files.AddDirectory("Content/Dir") && files.AddDirectory("Content/Copies") &&
                files.AddFile("Content/Base.rasset", sources.base.View()) &&
                files.AddFile("Content/Derived.rasset", sources.derived.View()) &&
                files.AddFile("Content/Dir/Hero.rasset", sources.hero.View()) &&
                files.AddFile("Content/Villain.rasset", sources.villain.View()) &&
                files.AddFile("Content/Leaf.rasset", sources.leaf.View()) &&
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
        config.packages.registryContext = ContextId{0x46495845ull, 1};
        config.packages.registryOptions.names = &env.names;
        config.clock = &clock;
        return config;
    }

    bool Put(StringView relative, Span<const uint8> bytes) noexcept {
        files.Remove(relative);
        return files.AddFile(relative, bytes);
    }

    // 지금 파일 byte의 사본이다(고치기 전후 비교).
    Array<uint8> Snapshot(Allocator& allocator, StringView relative) noexcept {
        Array<uint8> out(allocator);
        CHECK(out.TryAppend(files.Bytes(relative)));
        return out;
    }

    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetTypeHookTable hooks;
    ManualAssetClock clock;
    AssetDatabaseLock lock;
    bool ready = false;
};

bool Matches(AssetEnv& env, FixScene& scene, const AssetIndex& index) noexcept {
    AssetIndex fresh(env.allocator, scene.layout);
    return fresh.Rebuild(scene.files, ReadOptions()) && SameIndex(fresh, index);
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

TestAsset* Representative(EditorPackageSet& set, AssetId id) noexcept {
    const EditorPackageInfo* info = set.Find(id);
    return info == nullptr ? nullptr : AssetOf(set, info->package);
}

Result<PackageContents> ReadFile(AssetEnv& env, FixScene& scene, StringView relative) noexcept {
    return ReadPackage(env.allocator, scene.files.Bytes(relative), ReadOptions());
}

// sources.packages[slot](id self)를 ImportRecord와 함께 쓴다: owner가 self면 소유 기록(outputs),
// 아니면 owner의 참조 기록 + ImportOwner 의존.
Array<uint8> WithRecord(AssetEnv& env, Sources& sources, usize slot, uint64 self, uint64 owner,
                        Span<const ImportOutputInput> outputs) noexcept {
    const ImportSourceInput recordSources[] = {{"source", "Raw", "Leaf.png", Sha256Digest{}, 1}};
    const PackageDependencyEntry ownerDependency[] = {
        {FixedId(owner), PackageDependencyKind::ImportOwner, TypeId{}, false}};
    ImportRecordInput input;
    input.owner = FixedId(owner);
    PackageWriteOptions options = EditorOptions();
    if (self == owner) {
        input.importerId = Guid{0x464958, 4};
        input.importerName = "FixImporter";
        input.importerVersion = 3;
        input.sources = recordSources;
        input.outputs = outputs;
    } else {
        options.extraDependencies = ownerDependency;
    }

    auto record = WriteImportRecord(env.allocator, FixedId(self), input);
    if (!record) {
        return Array<uint8>(env.allocator);
    }

    options.importRecord = record.Value().View();
    return Write(env.allocator, *env.source, sources.packages[slot].Handle(), options);
}

// 고치기 hook 상태다(복제 시험과 같다): failTags면 Tags hook이 Busy, Build hook은 Leaf Build
// 의존을 더한다.
bool failFixTags = false;
AssetId hookedAsset{};

Result<void> FixTags(const AssetTypeHookContext& context, AssetTagSink& sink) noexcept {
    hookedAsset = context.asset;
    if (failFixTags) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return sink.Add("Kind", "Fixed");
}

Result<void> FixBuild(const AssetTypeHookContext&, AssetDependencySink& sink) noexcept {
    return sink.Add(FixedId(LeafId));
}

// relative 하나를 색인에 다시 읽힌다(밖에서 파일을 더한 뒤).
bool Refresh(FixScene& scene, AssetService& service, StringView relative) noexcept {
    AssetIndexRefreshPaths paths;
    paths.files = Span<const StringView>(&relative, 1);
    return static_cast<bool>(service.Index().RefreshPaths(scene.files, paths, ReadOptions()));
}

} // namespace

// 밖에서 복사한 Hero 사본(Copies/Hero2)을 새 id로 다시 쓴다: 헤더 id·bulk·의존(Hard Base·
// EditorOnly Derived·Soft SoftOnly)·hook 출력(Tags·Build — hook이 받은 id는 새 id), 색인은 새
// 검색과 같고 같은 id 진단이 사라진다(Hero id는 다시 하나), 남은 항목 경로, 연 패키지는 닫힘.
// 자기 Soft 참조는 새 id다. 같은 id 셋이면 남은 경로 둘. 경고 목록 없이도, NotDurable 경고.
TEST(AssetFixId_RewritesCopy) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    FixScene scene(env, sources, "fixid-copy");
    REQUIRE(scene.ready);
    REQUIRE(scene.files.AddFile(CopyFile, sources.hero.View()));
    RootRef<Package> selfPackage;
    const ObjectHandle selfAsset = MakeAsset<TestAsset>(*env.source, SelfishId, selfPackage);
    TestAsset* selfValue =
        selfAsset.IsNull() ? nullptr : Pointer<TestAsset>(*env.source, selfAsset);
    REQUIRE(selfValue != nullptr && selfValue->soft.TryAssign(FixedId(SelfishId), ""));
    const Array<uint8> selfish =
        Write(env.allocator, *env.source, selfPackage.Handle(), EditorOptions());
    REQUIRE(!selfish.IsEmpty() && scene.files.AddFile("Content/Selfish.rasset", selfish.View()) &&
            scene.files.AddFile("Content/Selfish2.rasset", selfish.View()) &&
            scene.files.AddFile("Content/Selfish3.rasset", selfish.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    CHECK(service.Index().FindById(FixedId(HeroId)).Error() == ErrorCode::AlreadyExists);
    auto registration =
        scene.hooks.Register(IdOf<TestAsset>(), AssetTypeHooks{&FixTags, &FixBuild});
    REQUIRE(registration);

    AssetFixIdResult result(env.allocator);
    Array<EditorWarning> warnings(env.allocator);
    REQUIRE(service.FixDuplicateId(scene.lock, "/Game/Copies/Hero2", result, &warnings));
    CHECK(warnings.IsEmpty() && result.path.View() == "/Game/Copies/Hero2" &&
          result.oldId == FixedId(HeroId) && !result.newId.IsNull() &&
          !(result.newId == FixedId(HeroId)) && hookedAsset == result.newId);
    REQUIRE(result.remaining.Size() == 1);
    CHECK(result.remaining[0].View() == "/Game/Dir/Hero");
    auto fixed = ReadFile(env, scene, CopyFile);
    REQUIRE(fixed);
    CHECK(fixed.Value().header.assetId == result.newId && !fixed.Value().tags.IsEmpty());
    bool hard = false;
    bool build = false;
    for (const PackageDependencyEntry& entry : fixed.Value().dependencies) {
        hard =
            hard || (entry.kind == PackageDependencyKind::Hard && entry.asset == FixedId(BaseId));
        build =
            build || (entry.kind == PackageDependencyKind::Build && entry.asset == FixedId(LeafId));
    }

    CHECK(hard && build);
    // 원본은 그대로, 색인은 새 검색과 같고 Hero id·새 id가 하나씩이다.
    CHECK(SameBytes(scene.files.Bytes("Content/Dir/Hero.rasset"), sources.hero.View()));
    CHECK(Matches(env, scene, service.Index()));
    auto heroEntry = service.Index().FindById(FixedId(HeroId));
    auto newEntry = service.Index().FindById(result.newId);
    REQUIRE(heroEntry && newEntry);
    CHECK(!heroEntry.Value()->duplicateId && !newEntry.Value()->duplicateId &&
          newEntry.Value()->fileName.View() == "Copies/Hero2.rasset");
    // 이번에 연 패키지(Hero 사본과 의존 Base·Derived)는 모두 닫혔다.
    CHECK(service.Packages().Count() == 0);
    // 고친 파일을 새 id로 열면 bulk가 있다, 원본도 이제 id로 열린다.
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Copies/Hero2", AssetInfoOptions{}, out));
    TestAsset* copy = Representative(service.Packages(), result.newId);
    REQUIRE(copy != nullptr);
    auto pixels = service.Packages().Find(result.newId)->store->Read(copy->pixels);
    CHECK(pixels && pixels.Value().Size() == PixelBytes && copy->value == HeroValue);
    REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out));

    // 자기 Soft 참조는 새 id다(자기 의존이 아니다). 같은 id 셋이면 남은 경로 둘(경로 키 순).
    AssetFixIdResult self(env.allocator);
    REQUIRE(service.FixDuplicateId(scene.lock, "/Game/Selfish2", self));
    REQUIRE(self.remaining.Size() == 2);
    CHECK(self.remaining[0].View() == "/Game/Selfish" &&
          self.remaining[1].View() == "/Game/Selfish3");
    auto selfRead = ReadFile(env, scene, "Content/Selfish2.rasset");
    REQUIRE(selfRead);
    for (const PackageDependencyEntry& entry : selfRead.Value().dependencies) {
        CHECK(!(entry.asset == self.newId) && !(entry.asset == FixedId(SelfishId)));
    }

    REQUIRE(service.Info("/Game/Selfish2", AssetInfoOptions{}, out));
    TestAsset* selfFixed = Representative(service.Packages(), self.newId);
    REQUIRE(selfFixed != nullptr);
    CHECK(selfFixed->soft.asset == self.newId);
    // 셋째를 고치면 Selfish만 옛 id로 남는다(진단 없음), 경고 NotDurable.
    warnings.Clear();
    scene.files.faults.replace = MemoryNotDurable;
    AssetFixIdResult third(env.allocator);
    REQUIRE(service.FixDuplicateId(scene.lock, "/Game/Selfish3", third, &warnings));
    scene.files.faults.replace = {};
    CHECK(third.remaining.Size() == 1 && warnings.Size() == 1 &&
          warnings[0].kind == EditorWarningKind::NotDurable &&
          warnings[0].diagnostic.asset == third.newId);
    CHECK(service.Index().FindById(FixedId(SelfishId)));

    // 경고 목록 없이도(NotDurable). Villain 사본(Hero에 Hard 의존 — 이제 Hero id가 하나다).
    REQUIRE(scene.files.AddFile("Content/Villain2.rasset", sources.villain.View()));
    REQUIRE(Refresh(scene, service, "Villain2.rasset"));
    scene.files.faults.replace = MemoryNotDurable;
    AssetFixIdResult quiet(env.allocator);
    REQUIRE(service.FixDuplicateId(scene.lock, "/Game/Villain2", quiet));
    scene.files.faults.replace = {};
    CHECK(Matches(env, scene, service.Index()));
    auto villain = ReadFile(env, scene, "Content/Villain2.rasset");
    REQUIRE(villain);
    CHECK(villain.Value().header.assetId == quiet.newId);
    // 전에 열려 있던 패키지(Hero·Base — Villain 사본의 의존)는 그대로 열려 있고 이번에 연
    // Villain 사본만 닫혔다.
    CHECK(service.Packages().Find(FixedId(HeroId)) != nullptr &&
          service.Packages().Find(FixedId(BaseId)) != nullptr &&
          service.Packages().Find(FixedId(VillainId)) == nullptr);
}

// ImportRecord: 소유 기록은 owner와 자기 출력만 새 id로 쓰고 형제 출력은 뺀다. 고아 경고는 쓰기 뒤
// 색인 판정이다 — 남는 같은 id 파일이 형제를 적으면 경고 없음(대개의 복사), 적지 않으면(우연한 id
// 충돌) 형제가 OwnerNotImporter, 판정할 수 없으면(남는 파일의 기록을 읽지 못함) 경고 없음. 참조
// 기록은 그대로이고 소유자가 옛 id를 적으므로 고친 패키지가 NotListed 경고. 경고 목록 없이도 성공.
TEST(AssetFixId_ImportRecords) {
    // 장면: 0 소유 기록 사본(남는 Leaf도 같은 기록), 1 우연한 충돌(남는 Leaf는 기록 없음),
    // 2 남는 Leaf의 기록을 읽지 못함, 3 참조 기록 사본.
    for (uint32 shape = 0; shape < 4; ++shape) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        const char* const tags[] = {"fixid-owned", "fixid-collision", "fixid-undetermined",
                                    "fixid-reference"};
        FixScene scene(env, sources, tags[shape]);
        REQUIRE(scene.ready);
        const ImportOutputInput leafOutputs[] = {{"leaf", FixedId(LeafId), IdOf<TestAsset>()},
                                                 {"extra", FixedId(BaseId), IdOf<BaseGameAsset>()}};
        const ImportOutputInput baseOutputs[] = {{"base", FixedId(BaseId), IdOf<BaseGameAsset>()},
                                                 {"leaf", FixedId(LeafId), IdOf<TestAsset>()}};
        if (shape < 3) {
            // Leaf 소유 기록(출력 Leaf·Base), Base는 Leaf의 참조 기록.
            const Array<uint8> owned = WithRecord(env, sources, 4, LeafId, LeafId, leafOutputs);
            const Array<uint8> base = WithRecord(env, sources, 0, BaseId, LeafId, {});
            REQUIRE(!owned.IsEmpty() && !base.IsEmpty());
            REQUIRE(scene.Put("Content/Base.rasset", base.View()));
            REQUIRE(scene.files.AddFile(LeafCopyFile, owned.View()));
            if (shape == 0) {
                REQUIRE(scene.Put("Content/Leaf.rasset", owned.View()));
            }

            if (shape == 2) {
                // 남는 Leaf의 기록을 읽지 못한다(형식 위반 — 칸 ImportRecord): Base를 적는지 알 수
                // 없다.
                const Array<uint8> corrupt = OneFieldRecord(env.allocator, ImportRecordSchema);
                PackageWriteOptions options = EditorOptions();
                options.importRecord = corrupt.View();
                const Array<uint8> leaf =
                    Write(env.allocator, *env.source, sources.packages[4].Handle(), options);
                REQUIRE(!leaf.IsEmpty() && scene.Put("Content/Leaf.rasset", leaf.View()));
            }
        } else {
            // Base 소유 기록(출력 Base·Leaf), Leaf는 Base의 참조 기록.
            const Array<uint8> base = WithRecord(env, sources, 0, BaseId, BaseId, baseOutputs);
            const Array<uint8> leaf = WithRecord(env, sources, 4, LeafId, BaseId, {});
            REQUIRE(!base.IsEmpty() && !leaf.IsEmpty());
            REQUIRE(scene.Put("Content/Base.rasset", base.View()));
            REQUIRE(scene.Put("Content/Leaf.rasset", leaf.View()));
            REQUIRE(scene.files.AddFile(LeafCopyFile, leaf.View()));
        }

        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        // 고치기 전에는 고아가 없다(같은 id 둘 중 하나가 적는다 — 참조 기록 사본도 옛 id).
        Array<const AssetIndexEntry*> before(env.allocator);
        REQUIRE(service.Index().Orphans(before));
        CHECK(before.IsEmpty());
        const Array<uint8> original = scene.Snapshot(env.allocator, LeafCopyFile);
        AssetFixIdResult result(env.allocator);
        Array<EditorWarning> warnings(env.allocator);
        REQUIRE(service.FixDuplicateId(scene.lock, "/Game/LeafCopy", result, &warnings));
        auto fixed = ReadFile(env, scene, LeafCopyFile);
        REQUIRE(fixed);
        CHECK(fixed.Value().header.assetId == result.newId);
        Array<const AssetIndexEntry*> after(env.allocator);
        REQUIRE(service.Index().Orphans(after));
        if (shape < 3) {
            auto record = ReadImportRecord(env.allocator, result.newId, fixed.Value().importRecord);
            REQUIRE(record);
            CHECK(record.Value().owner == result.newId &&
                  record.Value().importerName.View() == "FixImporter" &&
                  record.Value().importerVersion == 3 && record.Value().sources.Size() == 1);
            REQUIRE(record.Value().outputs.Size() == 1);
            CHECK(record.Value().outputs[0].asset == result.newId &&
                  record.Value().outputs[0].outputKey.View() == "leaf");
            if (shape == 1) {
                // 남는 Leaf는 Base를 적지 않는다: Base가 고아(OwnerNotImporter).
                REQUIRE(warnings.Size() == 1 && after.Size() == 1);
                CHECK(warnings[0].kind == EditorWarningKind::OrphanedOutput &&
                      warnings[0].diagnostic.code == AssetDiagnosticCode::OrphanedOutput &&
                      warnings[0].diagnostic.asset == FixedId(BaseId) &&
                      warnings[0].diagnostic.related == FixedId(LeafId) &&
                      warnings[0].diagnostic.detail ==
                          static_cast<uint64>(AssetOrphanReason::OwnerNotImporter) &&
                      after[0]->id == FixedId(BaseId));
            } else {
                CHECK(warnings.IsEmpty() && after.IsEmpty());
            }
        } else {
            // 기록 byte는 그대로(owner Base), 고친 Leaf는 Base가 적지 않는 고아(NotListed).
            auto record = ReadImportRecord(env.allocator, result.newId, fixed.Value().importRecord);
            REQUIRE(record);
            CHECK(record.Value().owner == FixedId(BaseId));
            REQUIRE(warnings.Size() == 1 && after.Size() == 1);
            CHECK(warnings[0].kind == EditorWarningKind::OrphanedOutput &&
                  warnings[0].diagnostic.asset == result.newId &&
                  warnings[0].diagnostic.related == FixedId(BaseId) &&
                  warnings[0].diagnostic.detail ==
                      static_cast<uint64>(AssetOrphanReason::NotListed) &&
                  after[0]->id == result.newId);
        }

        CHECK(Matches(env, scene, service.Index()));
        // 경고 목록 없이도 성공한다(원본 byte로 되돌려 다시).
        REQUIRE(scene.Put(LeafCopyFile, original.View()));
        REQUIRE(Refresh(scene, service, "LeafCopy.rasset"));
        AssetFixIdResult quiet(env.allocator);
        REQUIRE(service.FixDuplicateId(scene.lock, "/Game/LeafCopy", quiet));
        CHECK(Matches(env, scene, service.Index()));
    }
}

// Hard 순환(CycleA ↔ CycleB)의 사본 CycleA2: 열기가 CycleB까지 열고 CycleB가 CycleA2를 참조해
// 단독 닫기는 ReferencedBy다 — 묶음째 닫아 고친다(둘 다 닫힘). 고친 뒤 CycleB는 남은 CycleA를
// 가리키고 둘 다 다시 열린다.
TEST(AssetFixId_HardCycle) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    FixScene scene(env, sources, "fixid-cycle");
    REQUIRE(scene.ready);
    REQUIRE(scene.files.AddFile("Content/CycleA.rasset", sources.cycleA.View()) &&
            scene.files.AddFile("Content/CycleB.rasset", sources.cycleB.View()) &&
            scene.files.AddFile("Content/CycleA2.rasset", sources.cycleA.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    AssetFixIdResult result(env.allocator);
    REQUIRE(service.FixDuplicateId(scene.lock, "/Game/CycleA2", result));
    CHECK(service.Packages().Find(FixedId(CycleAId)) == nullptr &&
          service.Packages().Find(FixedId(CycleBId)) == nullptr &&
          service.Packages().Find(result.newId) == nullptr);
    CHECK(Matches(env, scene, service.Index()));
    auto fixed = ReadFile(env, scene, "Content/CycleA2.rasset");
    REQUIRE(fixed);
    bool toB = false;
    for (const PackageDependencyEntry& entry : fixed.Value().dependencies) {
        toB =
            toB || (entry.kind == PackageDependencyKind::Hard && entry.asset == FixedId(CycleBId));
    }

    CHECK(fixed.Value().header.assetId == result.newId && toB);
    String out(env.allocator);
    REQUIRE(service.Info("/Game/CycleB", AssetInfoOptions{}, out));
    CHECK(service.Packages().Find(FixedId(CycleAId)) != nullptr);
    REQUIRE(service.Info("/Game/CycleA2", AssetInfoOptions{}, out));
    CHECK(service.Packages().Find(result.newId) != nullptr);
}

// 거부와 실패: 파일·색인이 그대로이고 연 패키지는 닫힌다. 서비스: Initialize 전·잠금 아님·경로
// 규칙·없음·별칭 경로(풀지 않는다 — NotFound)·같은 경로 키 둘·같은 id 아님. 집합: hook 없음·
// Initialize 전·잠금 아님·null id·파일 없음·같은 id 아님·옛 id가 열림(Busy)·읽기 전용·새 id가
// 색인·집합에 있음·초기화 안 Busy·미해결 import·hook 실패·경고 자리. IO: 디렉터리·임시 파일
// 만들기·쓰기·Flush·Close·교체 실패(부모 디렉터리는 있다 — 만들지 않는다).
TEST(AssetFixId_RejectsAndFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    FixScene scene(env, sources, "fixid-reject");
    REQUIRE(scene.ready);
    REQUIRE(scene.files.AddFile(CopyFile, sources.hero.View()));
    REQUIRE(scene.files.AddFile(LeafCopyFile, sources.leaf.View()));
    // 같은 경로 키 둘(대소문자만 다른 디렉터리).
    REQUIRE(scene.files.AddDirectory("Content/Split") && scene.files.AddDirectory("Content/split"));
    REQUIRE(scene.files.AddFile("Content/Split/Dup.rasset", sources.cycleA.View()) &&
            scene.files.AddFile("Content/split/Dup.rasset", sources.cycleB.View()));
    // 읽기 전용 Derived 사본(모르는 커스텀 버전).
    PackageWriteOptions readOnly = EditorOptions();
    readOnly.customVersions = UnknownCustomVersions;
    const Array<uint8> derived =
        Write(env.allocator, *env.source, sources.packages[1].Handle(), readOnly);
    REQUIRE(!derived.IsEmpty() && scene.files.AddFile("Content/Derived2.rasset", derived.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    AssetFixIdResult result(env.allocator);
    AssetDiagnostic failure;
    CHECK(service.FixDuplicateId(scene.lock, "/Game/LeafCopy", result, nullptr, &failure).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(service.Initialize());
    {
        auto aliases = service.Aliases();
        auto created = DateTime::TryFromTicks(scene.clock.now);
        REQUIRE(aliases && created);
        const AssetAliasInput add[] = {{"/Game/Old/Leaf", FixedId(LeafId), created.Value(), 9}};
        bool notDurable = false;
        REQUIRE(aliases.Value()->Write(scene.files, scene.lock, {}, add, notDurable));
    }

    const usize nodes = scene.files.NodeCount();
    const Array<uint8> leafCopy = scene.Snapshot(env.allocator, LeafCopyFile);
    const Array<uint8> heroCopy = scene.Snapshot(env.allocator, CopyFile);
    auto unchanged = [&]() noexcept {
        return scene.files.NodeCount() == nodes &&
               SameBytes(scene.files.Bytes(LeafCopyFile), leafCopy.View()) &&
               SameBytes(scene.files.Bytes(CopyFile), heroCopy.View()) &&
               Matches(env, scene, service.Index());
    };
    auto rejects = [&](StringView path, ErrorCode error, AssetDiagnosticCode code) noexcept {
        failure = AssetDiagnostic{};
        auto fixed = service.FixDuplicateId(scene.lock, path, result, nullptr, &failure);
        return !fixed && fixed.Error() == error && failure.code == code && result.newId.IsNull() &&
               unchanged();
    };

    AssetDatabaseLock other;
    CHECK(service.FixDuplicateId(other, "/Game/LeafCopy", result).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(rejects("Game/X", ErrorCode::InvalidArgument, AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("/Game/Nowhere", ErrorCode::NotFound, AssetDiagnosticCode::None) &&
          result.oldId.IsNull() && result.path.IsEmpty());
    CHECK(rejects("/Game/Old/Leaf", ErrorCode::NotFound, AssetDiagnosticCode::None));
    CHECK(rejects("/Game/Split/Dup", ErrorCode::AlreadyExists, AssetDiagnosticCode::DuplicatePath));
    CHECK(rejects("/Game/Villain", ErrorCode::InvalidArgument, AssetDiagnosticCode::None) &&
          failure.asset == FixedId(VillainId) && result.oldId == FixedId(VillainId) &&
          result.path.View() == "/Game/Villain" && result.remaining.IsEmpty());
    CHECK(rejects("/Game/Derived2", ErrorCode::Unsupported, AssetDiagnosticCode::NewerVersion) &&
          failure.asset == FixedId(DerivedId));

    // 미해결 import: Base 파일이 사라진 뒤 Hero 사본을 고친다(연 패키지는 닫힌다).
    {
        AssetService broken(env.allocator, scene.layout, scene.Config(env));
        scene.files.Remove("Content/Base.rasset");
        REQUIRE(broken.Initialize());
        failure = AssetDiagnostic{};
        auto unresolved =
            broken.FixDuplicateId(scene.lock, "/Game/Copies/Hero2", result, nullptr, &failure);
        CHECK(!unresolved && unresolved.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::MissingDependency &&
              failure.asset == FixedId(HeroId) && failure.related == FixedId(BaseId) &&
              broken.Packages().Find(FixedId(HeroId)) == nullptr &&
              SameBytes(scene.files.Bytes(CopyFile), heroCopy.View()));
        REQUIRE(scene.files.AddFile("Content/Base.rasset", sources.base.View()));
    }

    CHECK(unchanged());

    // hook 실패(Tags): 아무것도 쓰지 않고 닫는다. 경고 자리 할당 실패도 같다.
    {
        auto registration =
            scene.hooks.Register(IdOf<TestAsset>(), AssetTypeHooks{&FixTags, &FixBuild});
        REQUIRE(registration);
        failFixTags = true;
        failure = AssetDiagnostic{};
        auto failedHook =
            service.FixDuplicateId(scene.lock, "/Game/LeafCopy", result, nullptr, &failure);
        failFixTags = false;
        CHECK(!failedHook && failedHook.Error() == ErrorCode::Busy &&
              failure.code == AssetDiagnosticCode::None && !hookedAsset.IsNull() &&
              !(hookedAsset == FixedId(LeafId)) &&
              service.Packages().Find(FixedId(LeafId)) == nullptr && unchanged());
    }

    {
        test::NFailAllocator warningAllocator(env.allocator);
        Array<EditorWarning> failingWarnings(warningAllocator);
        warningAllocator.FailOnAttempt(1);
        auto noRoom =
            service.FixDuplicateId(scene.lock, "/Game/LeafCopy", result, &failingWarnings);
        warningAllocator.DisableFailure();
        CHECK(!noRoom && noRoom.Error() == ErrorCode::OutOfMemory &&
              service.Packages().Find(FixedId(LeafId)) == nullptr && unchanged());
    }

    // IO 실패: 임시 파일이 남지 않고 파일·색인 그대로, 연 패키지는 닫힌다.
    auto ioFails = [&](auto inject, auto clear) noexcept {
        inject();
        failure = AssetDiagnostic{};
        auto fixed =
            service.FixDuplicateId(scene.lock, "/Game/LeafCopy", result, nullptr, &failure);
        clear();
        bool clean = true;
        for (usize at = 0; at < scene.files.NodeCount(); ++at) {
            clean = clean && !scene.files.NodeName(at).EndsWith(".tmp");
        }

        return !fixed && failure.code == AssetDiagnosticCode::None &&
               failure.asset == FixedId(LeafId) && clean &&
               service.Packages().Find(FixedId(LeafId)) == nullptr && unchanged();
    };
    CHECK(ioFails([&]() noexcept { scene.files.faults.create = MemoryError; },
                  [&]() noexcept { scene.files.faults.create = {}; }));
    CHECK(
        ioFails([&]() noexcept { scene.files.faults.failWriteCall = scene.files.WriteCalls() + 1; },
                [&]() noexcept { scene.files.faults.failWriteCall = 0; }));
    CHECK(ioFails([&]() noexcept { scene.files.faults.flush = MemoryError; },
                  [&]() noexcept { scene.files.faults.flush = {}; }));
    CHECK(ioFails([&]() noexcept { scene.files.faults.close = MemoryError; },
                  [&]() noexcept { scene.files.faults.close = {}; }));
    CHECK(ioFails([&]() noexcept { scene.files.faults.replace = MemoryBusy; },
                  [&]() noexcept { scene.files.faults.replace = {}; }));

    // 집합을 바로 부른 거부.
    EditorPackageSet& set = service.Packages();
    auto fresh = ParseLogicalPath(env.allocator, "/Game/Fresh");
    REQUIRE(fresh);
    REQUIRE(set.Create(fresh.Value(), IdOf<TestAsset>(), FixedId(UnsavedId)));
    auto setRejects = [&](StringView file, AssetId newId, const AssetDatabaseLock& lock,
                          ErrorCode error, AssetDiagnosticCode code) noexcept {
        failure = AssetDiagnostic{};
        auto fixed = set.FixDuplicateId(file, newId, lock, nullptr, &failure);
        return !fixed && fixed.Error() == error && failure.code == code && unchanged();
    };
    CHECK(setRejects("Copies/Hero2.rasset", AssetId{}, scene.lock, ErrorCode::InvalidArgument,
                     AssetDiagnosticCode::None));
    CHECK(setRejects("Copies/Hero2.rasset", FixedId(0xe40), other, ErrorCode::InvalidArgument,
                     AssetDiagnosticCode::None));
    CHECK(setRejects("Nope.rasset", FixedId(0xe40), scene.lock, ErrorCode::NotFound,
                     AssetDiagnosticCode::None));
    CHECK(setRejects("Villain.rasset", FixedId(0xe40), scene.lock, ErrorCode::InvalidArgument,
                     AssetDiagnosticCode::None) &&
          failure.asset == FixedId(VillainId));
    CHECK(setRejects("Copies/Hero2.rasset", FixedId(BaseId), scene.lock, ErrorCode::AlreadyExists,
                     AssetDiagnosticCode::DuplicateAssetId) &&
          failure.related == FixedId(BaseId) && failure.asset == FixedId(HeroId));
    CHECK(setRejects("Copies/Hero2.rasset", FixedId(UnsavedId), scene.lock,
                     ErrorCode::AlreadyExists, AssetDiagnosticCode::DuplicateAssetId));
    // 새 id가 색인에서 같은 id 둘(AlreadyExists — Hero와 사본)이어도 쓰고 있다.
    CHECK(setRejects("LeafCopy.rasset", FixedId(HeroId), scene.lock, ErrorCode::AlreadyExists,
                     AssetDiagnosticCode::DuplicateAssetId));
    {
        Project bare(env.allocator);
        REQUIRE(bare.ready && bare.Rebuild());
        EditorPackageSetConfig config = rocks::editor_set_test::Config(env, bare);
        config.hooks = nullptr;
        EditorPackageSet noHooks(env.allocator, config);
        CHECK(noHooks.FixDuplicateId("Copies/Hero2.rasset", FixedId(0xe40), scene.lock).Error() ==
              ErrorCode::InvalidArgument);
        REQUIRE(noHooks.Initialize());
        CHECK(noHooks.FixDuplicateId("Copies/Hero2.rasset", FixedId(0xe40), scene.lock).Error() ==
              ErrorCode::InvalidArgument);
    }

    // 만들기 초기화 안에서는 Busy다.
    struct FixInside final : EditorAssetInitializer {
        Result<void> Initialize(ConstructionContext&, ObjectHandle) noexcept override {
            auto fixed = set->FixDuplicateId("Copies/Hero2.rasset", FixedId(0xe41), *lock);
            error = fixed ? ErrorCode::None : fixed.Error();
            return Result<void>::Success();
        }

        EditorPackageSet* set = nullptr;
        const AssetDatabaseLock* lock = nullptr;
        ErrorCode error = ErrorCode::None;
    } inside;
    inside.set = &set;
    inside.lock = &scene.lock;
    auto innerPath = ParseLogicalPath(env.allocator, "/Game/Inner");
    REQUIRE(innerPath);
    REQUIRE(set.Create(innerPath.Value(), IdOf<TestAsset>(), FixedId(0xe42), nullptr, &inside));
    CHECK(inside.error == ErrorCode::Busy);
    CHECK(unchanged());

    // 옛 id가 열려 있음(먼저 연 Base에 밖에서 사본이 생긴 경우): Busy, 사본·원본 모두.
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Base", AssetInfoOptions{}, out));
    REQUIRE(scene.files.AddFile("Content/Base2.rasset", sources.base.View()));
    REQUIRE(Refresh(scene, service, "Base2.rasset"));
    const StringView opened[] = {"/Game/Base2", "/Game/Base"};
    for (StringView path : opened) {
        failure = AssetDiagnostic{};
        auto busy = service.FixDuplicateId(scene.lock, path, result, nullptr, &failure);
        CHECK(!busy && busy.Error() == ErrorCode::Busy && failure.asset == FixedId(BaseId) &&
              SameBytes(scene.files.Bytes("Content/Base2.rasset"), sources.base.View()) &&
              Matches(env, scene, service.Index()));
    }
}

// 할당 실패: 소유 기록 Leaf 사본·Hard 순환 사본·Hero 사본(bulk·의존·hook) — 모든 할당 실패는
// OutOfMemory이고 파일·색인이 그대로, 연 패키지는 닫힌다(GC 표 실패는 Registry가 삼킨 성공).
TEST(AssetFixId_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    struct Case {
        const char* tag;
        StringView path;
        StringView file;
        uint64 oldId;
    };
    const Case cases[] = {{"fixid-oom-leaf", "/Game/LeafCopy", LeafCopyFile, LeafId},
                          {"fixid-oom-cycle", "/Game/CycleA2", "Content/CycleA2.rasset", CycleAId},
                          {"fixid-oom-hero", "/Game/Copies/Hero2", CopyFile, HeroId}};
    for (const Case& one : cases) {
        FixScene scene(env, sources, one.tag);
        REQUIRE(scene.ready);
        const ImportOutputInput outputs[] = {{"leaf", FixedId(LeafId), IdOf<TestAsset>()},
                                             {"extra", FixedId(BaseId), IdOf<BaseGameAsset>()}};
        const Array<uint8> owned = WithRecord(env, sources, 4, LeafId, LeafId, outputs);
        const Array<uint8> base = WithRecord(env, sources, 0, BaseId, LeafId, {});
        REQUIRE(!owned.IsEmpty() && !base.IsEmpty());
        REQUIRE(scene.Put("Content/Base.rasset", base.View()) &&
                scene.files.AddFile(LeafCopyFile, owned.View()) &&
                scene.files.AddFile("Content/CycleA.rasset", sources.cycleA.View()) &&
                scene.files.AddFile("Content/CycleB.rasset", sources.cycleB.View()) &&
                scene.files.AddFile("Content/CycleA2.rasset", sources.cycleA.View()) &&
                scene.files.AddFile(CopyFile, sources.hero.View()));
        auto registration =
            scene.hooks.Register(IdOf<TestAsset>(), AssetTypeHooks{&FixTags, &FixBuild});
        REQUIRE(registration);
        const Array<uint8> original = scene.Snapshot(env.allocator, one.file);
        TaggedFailAllocator failing(env.allocator);
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            AssetService service(failing, scene.layout, scene.Config(env));
            REQUIRE(service.Initialize());
            // 먼저 연 Derived(Hero 사본의 의존)는 고치기 뒤에도 열린 채다(열린 id 기록도 실패
            // 지점).
            String out(env.allocator);
            REQUIRE(service.Info("/Game/Derived", AssetInfoOptions{}, out));
            AssetFixIdResult result(failing);
            Array<EditorWarning> warnings(env.allocator);
            failing.FailOnAttempt(attempt);
            auto fixed = service.FixDuplicateId(scene.lock, one.path, result, &warnings);
            const bool failed = failing.Failed();
            const uint32 tag = failing.FailedTag();
            failing.DisableFailure();
            if (!failed || (fixed && tag == GcReferenceTableTag)) {
                REQUIRE(fixed);
                CHECK(!SameBytes(scene.files.Bytes(one.file), original.View()) &&
                      Matches(env, scene, service.Index()) && service.Packages().Count() == 1 &&
                      service.Packages().Find(FixedId(DerivedId)) != nullptr);
                if (!failed) {
                    break;
                }

                REQUIRE(scene.Put(one.file, original.View()));
                continue;
            }

            CHECK(!fixed && fixed.Error() == ErrorCode::OutOfMemory && warnings.IsEmpty());
            CHECK(SameBytes(scene.files.Bytes(one.file), original.View()) &&
                  Matches(env, scene, service.Index()) &&
                  service.Packages().Find(FixedId(one.oldId)) == nullptr &&
                  service.Packages().Count() == 1 &&
                  service.Packages().Find(FixedId(DerivedId)) != nullptr);
            ++failures;
        }

        CHECK(failures > 0);
    }
}

// 실제 디스크: Hero와 그 사본을 디스크에 두고 사본을 고친 뒤 새 id로 열어 bulk를 읽는다.
TEST(AssetFixId_RealDisk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject project("fixid-disk");
    REQUIRE(project.made);
    REQUIRE(WriteContentFile(env.allocator, project, "Dir/Hero.rasset", sources.hero.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Copies/Hero2.rasset", sources.hero.View()));
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
    config.packages.registryContext = ContextId{0x46495844ull, 1};
    config.packages.registryOptions.names = &env.names;
    AssetService service(env.allocator, layout, config);
    REQUIRE(service.Initialize());
    AssetFixIdResult result(env.allocator);
    REQUIRE(service.FixDuplicateId(lock, "/Game/Copies/Hero2", result));
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Copies/Hero2", AssetInfoOptions{}, out));
    const EditorPackageInfo* copy = service.Packages().Find(result.newId);
    REQUIRE(copy != nullptr);
    TestAsset* copied = AssetOf(service.Packages(), copy->package);
    REQUIRE(copied != nullptr);
    auto pixels = copy->store->Read(copied->pixels);
    CHECK(pixels && pixels.Value().Size() == PixelBytes);
    REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out));
    CHECK(service.Packages().Find(FixedId(HeroId)) != nullptr);
}

} // namespace asset_fix_id_tests_file
