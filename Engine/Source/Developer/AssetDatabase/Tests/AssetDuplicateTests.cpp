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

// 복제 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 17 A2e3b): 메모리 상태 복제(저장하지 않은
// 편집·bulk·의존, 원본 정보 그대로, 사본은 열지 않음, 색인에 새 항목), 자기 Soft 참조의 새 id,
// 원본 고르기(경로·id·별칭·저장하지 않은 새 패키지), ImportRecord(소유 기록 다시 쓰기·참조 기록
// 버리기), 모든 거부와 IO 실패(대상·색인·원본 그대로), OS 경로 상한, 할당 실패, 실제 디스크.
using namespace rocks;
namespace asset_duplicate_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;

namespace {

constexpr StringView AliasFile = "Config/AssetAliases.json";
constexpr StringView CopyFile = "Content/Copies/Hero2.rasset";
constexpr int32 EditedValue = 99;
constexpr uint64 SelfishId = 0xf30;

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

// 복제 장면이다(삭제 장면과 같다): Content에 Base·Derived·Dir/Hero(bulk)·Villain·Leaf, Config,
// 디스크 임시 루트(잠금).
struct DuplicateScene {
    DuplicateScene(AssetEnv& env, Sources& sources, const char* tag) noexcept
        : root(tag), layout(env.allocator), files(env.allocator, Initialized(layout, root)),
          hooks(env.allocator, env.types), clock(StartTicks()) {
        ready = root.made && !layout.Root().IsEmpty() && files.Ready() &&
                files.AddDirectory("Content") && files.AddDirectory("Config") &&
                files.AddDirectory("Content/Dir") &&
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
        config.packages.registryContext = ContextId{0x44555045ull, 1};
        config.packages.registryOptions.names = &env.names;
        config.clock = &clock;
        return config;
    }

    bool Put(StringView relative, Span<const uint8> bytes) noexcept {
        files.Remove(relative);
        return files.AddFile(relative, bytes);
    }

    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetTypeHookTable hooks;
    ManualAssetClock clock;
    AssetDatabaseLock lock;
    bool ready = false;
};

bool Matches(AssetEnv& env, DuplicateScene& scene, const AssetIndex& index) noexcept {
    AssetIndex fresh(env.allocator, scene.layout);
    return fresh.Rebuild(scene.files, ReadOptions()) && SameIndex(fresh, index);
}

TestAsset* Representative(EditorPackageSet& set, AssetId id) noexcept {
    const EditorPackageInfo* info = set.Find(id);
    return info == nullptr ? nullptr : AssetOf(set, info->package);
}

Result<PackageContents> ReadFile(AssetEnv& env, DuplicateScene& scene,
                                 StringView relative) noexcept {
    return ReadPackage(env.allocator, scene.files.Bytes(relative), ReadOptions());
}

// Leaf를 ImportRecord가 있는 모양으로 바꾼다: owned면 소유 기록(출력 Leaf·Base), 아니면 Base가
// 소유한 출력의 참조 기록 + ImportOwner 의존.
bool PutLeafRecord(AssetEnv& env, Sources& sources, DuplicateScene& scene, bool owned) noexcept {
    const ImportSourceInput recordSources[] = {{"source", "Raw", "Leaf.png", Sha256Digest{}, 1}};
    const ImportOutputInput recordOutputs[] = {{"leaf", FixedId(LeafId), IdOf<TestAsset>()},
                                               {"extra", FixedId(BaseId), IdOf<BaseGameAsset>()}};
    const PackageDependencyEntry owner[] = {
        {FixedId(BaseId), PackageDependencyKind::ImportOwner, TypeId{}, false}};
    ImportRecordInput input;
    PackageWriteOptions options = EditorOptions();
    if (owned) {
        input.owner = FixedId(LeafId);
        input.importerId = Guid{0x494d50, 3};
        input.importerName = "LeafImporter";
        input.importerVersion = 2;
        input.sources = recordSources;
        input.outputs = recordOutputs;
    } else {
        input.owner = FixedId(BaseId);
        options.extraDependencies = owner;
    }

    auto record = WriteImportRecord(env.allocator, FixedId(LeafId), input);
    if (!record) {
        return false;
    }

    options.importRecord = record.Value().View();
    const Array<uint8> bytes =
        Write(env.allocator, *env.source, sources.packages[4].Handle(), options);
    return !bytes.IsEmpty() && scene.Put("Content/Leaf.rasset", bytes.View());
}

// 복제의 hook 상태다(hook은 함수 포인터라 전역으로 넘긴다): failTags면 Tags hook이 Busy, Build
// hook은 Leaf Build 의존을 더한다.
bool failDuplicateTags = false;
// hook이 받은 저장할 패키지 id(복제면 사본 id)다.
AssetId hookedAsset{};

Result<void> DuplicateTags(const AssetTypeHookContext& context, AssetTagSink& sink) noexcept {
    hookedAsset = context.asset;
    if (failDuplicateTags) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return sink.Add("Kind", "Copy");
}

Result<void> DuplicateBuild(const AssetTypeHookContext&, AssetDependencySink& sink) noexcept {
    return sink.Add(FixedId(LeafId));
}

} // namespace

// 메모리 상태 복제: 열린 Hero의 저장하지 않은 편집·bulk·의존이 사본에 있고(헤더 id는 새 id), 원본
// 정보(파일·dirty·저장소)는 그대로, 사본은 열지 않고 색인에 새 항목. 원본을 id·별칭 경로(경고
// AliasResolved)·저장하지 않은 새 패키지로 고른다. 자기 Soft 참조는 사본 id다. NotDurable 경고.
TEST(AssetDuplicate_CopiesMemoryState) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    DuplicateScene scene(env, sources, "duplicate-state");
    REQUIRE(scene.ready);
    RootRef<Package> selfPackage;
    const ObjectHandle selfAsset = MakeAsset<TestAsset>(*env.source, SelfishId, selfPackage);
    TestAsset* selfValue =
        selfAsset.IsNull() ? nullptr : Pointer<TestAsset>(*env.source, selfAsset);
    REQUIRE(selfValue != nullptr && selfValue->soft.TryAssign(FixedId(SelfishId), ""));
    const Array<uint8> selfish =
        Write(env.allocator, *env.source, selfPackage.Handle(), EditorOptions());
    REQUIRE(!selfish.IsEmpty() && scene.files.AddFile("Content/Selfish.rasset", selfish.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());

    // 열린 Hero를 고친다(저장하지 않음).
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out));
    TestAsset* hero = Representative(service.Packages(), FixedId(HeroId));
    REQUIRE(hero != nullptr);
    hero->value = EditedValue;
    REQUIRE(service.Packages().MarkDirty(FixedId(HeroId)));
    AssetDuplicateResult result(env.allocator);
    Array<EditorWarning> warnings(env.allocator);
    REQUIRE(
        service.Duplicate(scene.lock, "/Game/Dir/Hero", "/Game/copies/Hero2", result, &warnings));
    CHECK(!result.id.IsNull() && result.path.View() == "/Game/copies/Hero2" && warnings.IsEmpty());
    // 사본 파일(새 디렉터리 — 철자는 논리 경로 그대로), 헤더 id, 의존(Hard Base·EditorOnly
    // Derived·Soft SoftOnly).
    const StringView copyFile = "Content/copies/Hero2.rasset";
    REQUIRE(scene.files.Has(copyFile));
    auto copied = ReadFile(env, scene, copyFile);
    REQUIRE(copied);
    CHECK(copied.Value().header.assetId == result.id && copied.Value().dependencies.Size() == 3);
    CHECK(Matches(env, scene, service.Index()));
    // 원본은 그대로(열린 채 dirty, 같은 파일·저장소), 사본은 열지 않았다.
    const EditorPackageInfo* source = service.Packages().Find(FixedId(HeroId));
    REQUIRE(source != nullptr);
    CHECK(source->dirty && source->fileName.View() == "Dir/Hero.rasset" &&
          source->store->IsAttached() && service.Packages().Find(result.id) == nullptr);
    // 사본을 열면 편집한 값과 bulk가 있다.
    REQUIRE(service.Info("/Game/copies/Hero2", AssetInfoOptions{}, out));
    TestAsset* copy = Representative(service.Packages(), result.id);
    REQUIRE(copy != nullptr);
    CHECK(copy->value == EditedValue);
    const EditorPackageInfo* copyInfo = service.Packages().Find(result.id);
    REQUIRE(copyInfo != nullptr);
    auto pixels = copyInfo->store->Read(copy->pixels);
    CHECK(pixels && pixels.Value().Size() == PixelBytes);

    // id로 고른 자기 참조 패키지: 사본의 Soft는 사본 id이고 자기 의존이 아니다.
    char selfText[40] = {};
    {
        FixedStringBuilder text{Span<char>(selfText)};
        REQUIRE(text.TryAppendFormat("{}", FixedId(SelfishId)));
    }

    AssetDuplicateResult selfCopy(env.allocator);
    REQUIRE(service.Duplicate(scene.lock, StringView(selfText), "/Game/Selfish2", selfCopy));
    auto selfRead = ReadFile(env, scene, "Content/Selfish2.rasset");
    REQUIRE(selfRead);
    CHECK(selfRead.Value().dependencies.IsEmpty());
    REQUIRE(service.Info("/Game/Selfish2", AssetInfoOptions{}, out));
    TestAsset* selfCopied = Representative(service.Packages(), selfCopy.id);
    REQUIRE(selfCopied != nullptr);
    CHECK(selfCopied->soft.asset == selfCopy.id);

    // 이동(Detach — bulk가 메모리에만 있다) 뒤의 원본도 bulk째 복제한다.
    AssetMoveResult moved;
    REQUIRE(service.Move(scene.lock, "/Game/Dir", "/Game/Dir9", moved));
    AssetDuplicateResult detached(env.allocator);
    REQUIRE(service.Duplicate(scene.lock, "/Game/Dir9/Hero", "/Game/Hero3", detached));
    REQUIRE(service.Info("/Game/Hero3", AssetInfoOptions{}, out));
    TestAsset* fromDetached = Representative(service.Packages(), detached.id);
    REQUIRE(fromDetached != nullptr);
    auto detachedPixels = service.Packages().Find(detached.id)->store->Read(fromDetached->pixels);
    CHECK(detachedPixels && detachedPixels.Value().Size() == PixelBytes &&
          fromDetached->value == EditedValue);

    // 별칭 경로로 고른 원본(경고 AliasResolved).
    {
        auto aliases = service.Aliases();
        auto created = DateTime::TryFromTicks(scene.clock.now);
        REQUIRE(aliases && created);
        const AssetAliasInput add[] = {{"/Game/Old/Leaf", FixedId(LeafId), created.Value(), 90}};
        bool notDurable = false;
        REQUIRE(aliases.Value()->Write(scene.files, scene.lock, {}, add, notDurable));
    }

    warnings.Clear();
    AssetDuplicateResult aliased(env.allocator);
    REQUIRE(service.Duplicate(scene.lock, "/Game/Old/Leaf", "/Game/Leaf2", aliased, &warnings));
    CHECK(warnings.Size() == 1 && warnings[0].kind == EditorWarningKind::AliasResolved &&
          scene.files.Has("Content/Leaf2.rasset"));

    // 저장하지 않은 새 패키지(원본은 저장하지 않은 채).
    auto path = ParseLogicalPath(env.allocator, "/Game/New/Thing");
    REQUIRE(path);
    REQUIRE(service.Packages().Create(path.Value(), IdOf<TestAsset>(), FixedId(0xf10)));
    // 저장하지 않은 새 bulk(AddUnsaved)도 사본에 쓴다.
    {
        TestAsset* fresh = Representative(service.Packages(), FixedId(0xf10));
        REQUIRE(fresh != nullptr);
        Array<uint8> raw(env.allocator);
        REQUIRE(raw.TryAppend(sources.pixels.View()));
        auto bulk = service.Packages().Find(FixedId(0xf10))->store->AddUnsaved(Move(raw));
        REQUIRE(bulk);
        fresh->pixels = bulk.Value();
    }

    AssetDuplicateResult fromNew(env.allocator);
    REQUIRE(service.Duplicate(scene.lock, "/Game/New/Thing", "/Game/Thing2", fromNew));
    CHECK(scene.files.Has("Content/Thing2.rasset") &&
          service.Packages().Find(FixedId(0xf10))->unsaved && !scene.files.Has("Content/New"));
    REQUIRE(service.Info("/Game/Thing2", AssetInfoOptions{}, out));
    TestAsset* thing = Representative(service.Packages(), fromNew.id);
    REQUIRE(thing != nullptr);
    auto thingPixels = service.Packages().Find(fromNew.id)->store->Read(thing->pixels);
    CHECK(thingPixels && thingPixels.Value().Size() == PixelBytes);

    // NotDurable 경고.
    warnings.Clear();
    scene.files.faults.replace = MemoryNotDurable;
    AssetDuplicateResult durable(env.allocator);
    REQUIRE(service.Duplicate(scene.lock, "/Game/Leaf", "/Game/Leaf3", durable, &warnings));
    scene.files.faults.replace = {};
    CHECK(warnings.Size() == 1 && warnings[0].kind == EditorWarningKind::NotDurable &&
          warnings[0].diagnostic.asset == durable.id);
    // 경고 목록 없이도.
    scene.files.faults.replace = MemoryNotDurable;
    AssetDuplicateResult quiet(env.allocator);
    REQUIRE(service.Duplicate(scene.lock, "/Game/Leaf", "/Game/Leaf4", quiet));
    scene.files.faults.replace = {};
    CHECK(Matches(env, scene, service.Index()));
}

// ImportRecord: 소유 기록은 owner와 원본 자신의 출력만 사본 id로 남겨 쓰고(형제 출력은 빠진다 —
// 원본·임포터 그대로), 참조 기록은 사본에서 기록과 ImportOwner 의존을 버리고 경고
// ImportRecordDropped (related = 소유자, 경고 목록 없이도 성공).
TEST(AssetDuplicate_ImportRecords) {
    const bool shapes[] = {true, false};
    for (bool owned : shapes) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        DuplicateScene scene(env, sources, owned ? "duplicate-owned" : "duplicate-reference");
        REQUIRE(scene.ready);
        REQUIRE(PutLeafRecord(env, sources, scene, owned));
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        AssetDuplicateResult result(env.allocator);
        Array<EditorWarning> warnings(env.allocator);
        REQUIRE(service.Duplicate(scene.lock, "/Game/Leaf", "/Game/Leaf2", result, &warnings));
        auto copied = ReadFile(env, scene, "Content/Leaf2.rasset");
        REQUIRE(copied);
        if (owned) {
            CHECK(warnings.IsEmpty() && copied.Value().dependencies.IsEmpty());
            auto record = ReadImportRecord(env.allocator, result.id, copied.Value().importRecord);
            REQUIRE(record);
            CHECK(record.Value().owner == result.id &&
                  record.Value().importerName.View() == "LeafImporter" &&
                  record.Value().importerVersion == 2 && record.Value().sources.Size() == 1);
            // 원본 자신의 출력 하나만 사본 id로 남는다(형제 Base는 원본 임포트의 것).
            REQUIRE(record.Value().outputs.Size() == 1);
            CHECK(record.Value().outputs[0].asset == result.id &&
                  record.Value().outputs[0].outputKey.View() == "leaf");
        } else {
            REQUIRE(warnings.Size() == 1);
            CHECK(warnings[0].kind == EditorWarningKind::ImportRecordDropped &&
                  warnings[0].diagnostic.asset == result.id &&
                  warnings[0].diagnostic.related == FixedId(BaseId));
            CHECK(copied.Value().importRecord.IsEmpty() && copied.Value().dependencies.IsEmpty());
            AssetDuplicateResult quiet(env.allocator);
            REQUIRE(service.Duplicate(scene.lock, "/Game/Leaf", "/Game/Leaf3", quiet));
        }

        CHECK(Matches(env, scene, service.Index()));
    }
}

// 거부와 실패: 대상 파일·색인·원본이 그대로다. 서비스: Initialize 전·잠금 아님·대상 경로 규칙·
// 대상이 별칭(다른 에셋 — related)·대상 점유(색인 항목·색인에 없는 디스크 파일·저장하지 않은 새
// 패키지)·원본 없음·읽기 전용 원본·미해결 import 원본·별칭 표 손상. 집합: hook 없음·잠금 아님·
// null id·Initialize 전·초기화 안 Busy·원본이 열리지 않음·새 id가 색인·집합에 있음. IO: 부모
// 디렉터리·임시 파일 만들기·쓰기·Flush·Close·교체 실패(임시 파일 없음).
TEST(AssetDuplicate_RejectsAndFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    DuplicateScene scene(env, sources, "duplicate-reject");
    REQUIRE(scene.ready);
    const uint8 junk[] = {1, 2, 3};
    REQUIRE(scene.files.AddFile("Content/Junk.rasset", Span<const uint8>(junk, 3)));
    // 읽기 전용 Derived(모르는 커스텀 버전).
    PackageWriteOptions readOnly = EditorOptions();
    readOnly.customVersions = UnknownCustomVersions;
    const Array<uint8> derived =
        Write(env.allocator, *env.source, sources.packages[1].Handle(), readOnly);
    REQUIRE(!derived.IsEmpty() && scene.Put("Content/Derived.rasset", derived.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    AssetDuplicateResult result(env.allocator);
    AssetDiagnostic failure;
    CHECK(
        service.Duplicate(scene.lock, "/Game/Leaf", "/Game/X", result, nullptr, &failure).Error() ==
        ErrorCode::InvalidArgument);
    REQUIRE(service.Initialize());
    {
        auto aliases = service.Aliases();
        auto created = DateTime::TryFromTicks(scene.clock.now);
        REQUIRE(aliases && created);
        const AssetAliasInput add[] = {{"/Game/Aliased", FixedId(VillainId), created.Value(), 9}};
        bool notDurable = false;
        REQUIRE(aliases.Value()->Write(scene.files, scene.lock, {}, add, notDurable));
    }

    const usize nodes = scene.files.NodeCount();
    auto unchanged = [&]() noexcept {
        return scene.files.NodeCount() == nodes && Matches(env, scene, service.Index());
    };
    auto rejects = [&](StringView source, StringView target, ErrorCode error,
                       AssetDiagnosticCode code) noexcept {
        failure = AssetDiagnostic{};
        auto made = service.Duplicate(scene.lock, source, target, result, nullptr, &failure);
        return !made && made.Error() == error && failure.code == code && result.id.IsNull() &&
               unchanged();
    };

    AssetDatabaseLock other;
    CHECK(service.Duplicate(other, "/Game/Leaf", "/Game/X", result).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(rejects("/Game/Leaf", "Game/X", ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("/Game/Leaf", "/Game/ALIASED", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath) &&
          failure.related == FixedId(VillainId));
    CHECK(rejects("/Game/Leaf", "/Game/Villain", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath));
    CHECK(rejects("/Game/Leaf", "/Game/Junk", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath));
    CHECK(rejects("/Game/Nowhere", "/Game/X", ErrorCode::NotFound, AssetDiagnosticCode::None));
    CHECK(rejects("/Game/Derived", "/Game/X", ErrorCode::Unsupported,
                  AssetDiagnosticCode::NewerVersion));
    auto fresh = ParseLogicalPath(env.allocator, "/Game/Fresh");
    REQUIRE(fresh);
    REQUIRE(service.Packages().Create(fresh.Value(), IdOf<TestAsset>(), FixedId(0xf20)));
    const usize nodesWithFresh = scene.files.NodeCount();
    CHECK(nodesWithFresh == nodes);
    CHECK(rejects("/Game/Leaf", "/Game/FRESH", ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath) &&
          failure.related == FixedId(0xf20));

    // 미해결 import 원본(Base 파일이 사라진 뒤 Hero를 연다).
    {
        AssetService broken(env.allocator, scene.layout, scene.Config(env));
        scene.files.Remove("Content/Base.rasset");
        REQUIRE(broken.Initialize());
        failure = AssetDiagnostic{};
        auto unresolved =
            broken.Duplicate(scene.lock, "/Game/Dir/Hero", "/Game/X", result, nullptr, &failure);
        CHECK(!unresolved && failure.code == AssetDiagnosticCode::MissingDependency &&
              failure.related == FixedId(BaseId) && !scene.files.Has("Content/X.rasset"));
        REQUIRE(scene.files.AddFile("Content/Base.rasset", sources.base.View()));
    }

    // 대상 Stat 실패(그 오류)와 색인에만 있는 대상(파일이 밖에서 지워진 낡은 색인 — DuplicatePath).
    scene.files.faults.stat = MemoryBusy;
    scene.files.faults.statPath = "Content/StatFail.rasset";
    CHECK(rejects("/Game/Leaf", "/Game/StatFail", ErrorCode::Busy, AssetDiagnosticCode::None));
    scene.files.faults.stat = {};
    scene.files.faults.statPath = nullptr;
    scene.files.Remove("Content/Villain.rasset");
    failure = AssetDiagnostic{};
    auto stale =
        service.Duplicate(scene.lock, "/Game/Leaf", "/Game/Villain", result, nullptr, &failure);
    CHECK(!stale && failure.code == AssetDiagnosticCode::DuplicatePath &&
          !scene.files.Has("Content/Villain.rasset"));
    REQUIRE(scene.files.AddFile("Content/Villain.rasset", sources.villain.View()));
    CHECK(unchanged());

    // hook: 사본의 Tags·Build 의존은 hook으로 다시 만든다, hook 실패는 아무것도 쓰지 않는다.
    {
        auto registration = scene.hooks.Register(IdOf<TestAsset>(),
                                                 AssetTypeHooks{&DuplicateTags, &DuplicateBuild});
        REQUIRE(registration);
        AssetDuplicateResult hooked(env.allocator);
        REQUIRE(service.Duplicate(scene.lock, "/Game/Villain", "/Game/Hooked", hooked));
        auto read =
            ReadPackage(env.allocator, scene.files.Bytes("Content/Hooked.rasset"), ReadOptions());
        REQUIRE(read);
        bool build = false;
        for (const PackageDependencyEntry& entry : read.Value().dependencies) {
            build = build ||
                    (entry.kind == PackageDependencyKind::Build && entry.asset == FixedId(LeafId));
        }

        CHECK(build && !read.Value().tags.IsEmpty() && hookedAsset == hooked.id);
        // 저장은 자기 id를 준다.
        REQUIRE(service.Packages().Save(FixedId(VillainId), scene.lock, EditorSaveOptions{}));
        CHECK(hookedAsset == FixedId(VillainId));
        scene.files.Remove("Content/Hooked.rasset");
        AssetIndexRefreshPaths paths;
        const StringView hookedName = "Hooked.rasset";
        paths.files = Span<const StringView>(&hookedName, 1);
        REQUIRE(service.Index().RefreshPaths(scene.files, paths, ReadOptions()));
        failDuplicateTags = true;
        failure = AssetDiagnostic{};
        auto failedHook = service.Duplicate(scene.lock, "/Game/Villain", "/Game/Hooked", hooked,
                                            nullptr, &failure);
        failDuplicateTags = false;
        CHECK(!failedHook && failedHook.Error() == ErrorCode::Busy &&
              failure.code == AssetDiagnosticCode::None && unchanged());
    }

    // 경고 자리 할당 실패(쓰기 전).
    {
        test::NFailAllocator warningAllocator(env.allocator);
        Array<EditorWarning> failingWarnings(warningAllocator);
        warningAllocator.FailOnAttempt(1);
        auto noRoom =
            service.Duplicate(scene.lock, "/Game/Leaf", "/Game/NoRoom", result, &failingWarnings);
        warningAllocator.DisableFailure();
        CHECK(!noRoom && noRoom.Error() == ErrorCode::OutOfMemory && unchanged());
    }

    // 별칭 표 손상.
    {
        AssetService broken(env.allocator, scene.layout, scene.Config(env));
        const Span<const uint8> aliasBytes = scene.files.Bytes(AliasFile);
        Array<uint8> saved(env.allocator);
        REQUIRE(saved.TryAppend(aliasBytes));
        const char text[] = "{";
        REQUIRE(scene.Put(AliasFile, Span<const uint8>(reinterpret_cast<const uint8*>(text), 1)));
        REQUIRE(broken.Initialize());
        failure = AssetDiagnostic{};
        CHECK(!broken.Duplicate(scene.lock, "/Game/Leaf", "/Game/X", result, nullptr, &failure) &&
              failure.code == AssetDiagnosticCode::CorruptAliasTable);
        REQUIRE(scene.Put(AliasFile, saved.View()));
    }

    CHECK(unchanged());

    // 집합을 바로 부른 거부.
    EditorPackageSet& set = service.Packages();
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));
    auto target = ParseLogicalPath(env.allocator, "/Game/Direct");
    REQUIRE(target);
    auto setRejects = [&](AssetId source, AssetId newId, const AssetDatabaseLock& lock,
                          ErrorCode error, AssetDiagnosticCode code) noexcept {
        failure = AssetDiagnostic{};
        auto made = set.Duplicate(source, target.Value(), newId, lock, nullptr, &failure);
        return !made && made.Error() == error && failure.code == code && unchanged();
    };
    CHECK(setRejects(FixedId(LeafId), AssetId{}, scene.lock, ErrorCode::InvalidArgument,
                     AssetDiagnosticCode::None));
    CHECK(setRejects(FixedId(LeafId), FixedId(0xf40), other, ErrorCode::InvalidArgument,
                     AssetDiagnosticCode::None));
    CHECK(setRejects(FixedId(0xf99), FixedId(0xf40), scene.lock, ErrorCode::NotFound,
                     AssetDiagnosticCode::None));
    CHECK(setRejects(FixedId(LeafId), FixedId(BaseId), scene.lock, ErrorCode::AlreadyExists,
                     AssetDiagnosticCode::DuplicateAssetId));
    CHECK(setRejects(FixedId(LeafId), FixedId(0xf20), scene.lock, ErrorCode::AlreadyExists,
                     AssetDiagnosticCode::DuplicateAssetId));
    {
        Project bare(env.allocator);
        REQUIRE(bare.ready && bare.Rebuild());
        EditorPackageSetConfig config = rocks::editor_set_test::Config(env, bare);
        config.hooks = nullptr;
        EditorPackageSet noHooks(env.allocator, config);
        failure = AssetDiagnostic{};
        CHECK(noHooks
                  .Duplicate(FixedId(LeafId), target.Value(), FixedId(0xf40), scene.lock, nullptr,
                             &failure)
                  .Error() == ErrorCode::InvalidArgument);
        REQUIRE(noHooks.Initialize());
        CHECK(noHooks.Duplicate(FixedId(LeafId), target.Value(), FixedId(0xf40), scene.lock)
                  .Error() == ErrorCode::InvalidArgument);
    }

    // 만들기 초기화 안에서는 Busy다.
    struct DuplicateInside final : EditorAssetInitializer {
        Result<void> Initialize(ConstructionContext&, ObjectHandle) noexcept override {
            auto made = set->Duplicate(FixedId(LeafId), *path, FixedId(0xf41), *lock);
            error = made ? ErrorCode::None : made.Error();
            return Result<void>::Success();
        }

        EditorPackageSet* set = nullptr;
        const LogicalPath* path = nullptr;
        const AssetDatabaseLock* lock = nullptr;
        ErrorCode error = ErrorCode::None;
    } inside;
    inside.set = &set;
    inside.path = &target.Value();
    inside.lock = &scene.lock;
    auto innerPath = ParseLogicalPath(env.allocator, "/Game/Inner");
    REQUIRE(innerPath);
    REQUIRE(set.Create(innerPath.Value(), IdOf<TestAsset>(), FixedId(0xf42), nullptr, &inside));
    CHECK(inside.error == ErrorCode::Busy);

    // IO 실패: 아무것도 남지 않는다(임시 파일·대상 파일 없음 — 만든 부모 디렉터리는 남을 수 있다).
    auto ioFails = [&](auto inject, auto clear) noexcept {
        const usize before = scene.files.NodeCount();
        inject();
        failure = AssetDiagnostic{};
        auto made =
            service.Duplicate(scene.lock, "/Game/Leaf", "/Game/Io/Copy", result, nullptr, &failure);
        clear();
        bool clean = !scene.files.Has("Content/Io/Copy.rasset");
        for (usize at = 0; at < scene.files.NodeCount(); ++at) {
            clean = clean && !scene.files.NodeName(at).EndsWith(".tmp");
        }

        return !made && failure.code == AssetDiagnosticCode::None && clean &&
               scene.files.NodeCount() <= before + 1 && Matches(env, scene, service.Index());
    };
    CHECK(ioFails([&]() noexcept { scene.files.faults.makeDirectory = MemoryError; },
                  [&]() noexcept { scene.files.faults.makeDirectory = {}; }));
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
}

// OS 경로 상한: 깊은 디스크 루트에서 대상의 절대 OS 경로가 상한을 넘으면 InvalidPath /
// CapacityExceeded이고 아무것도 쓰지 않는다. 짧은 이름은 같은 루트에서 복제된다.
TEST(AssetDuplicate_NativePathLimit) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject project("duplicate-limit");
    REQUIRE(project.made);
    constexpr usize ContentPrefixBytes = 9;
    constexpr usize ShortNameRoom = 30;
    constexpr usize LongNameBytes = 60;
    const usize rootBytes = MaxNativePathBytes() - TemporaryFileSuffixBytes -
                            EditorPackageExtension.ByteCount() - ContentPrefixBytes - ShortNameRoom;
    String root(env.allocator);
    REQUIRE(MakeDeepRoot(project, rootBytes, root));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(root.View()));
    MemoryFileSystem files(env.allocator, layout.Root());
    REQUIRE(files.Ready() && files.AddDirectory("Content") && files.AddDirectory("Config") &&
            files.AddFile("Content/Leaf.rasset", sources.leaf.View()));
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(layout, 0));
    AssetTypeHookTable hooks(env.allocator, env.types);
    AssetServiceConfig config;
    config.packages.types = &env.types;
    config.packages.files = &files;
    config.packages.hooks = &hooks;
    config.packages.read.engineVersion = TestEngineVersion;
    config.packages.registryContext = ContextId{0x44554c4dull, 1};
    config.packages.registryOptions.names = &env.names;
    AssetService service(env.allocator, layout, config);
    REQUIRE(service.Initialize());
    String longPath(env.allocator);
    REQUIRE(longPath.TryAppend("/Game/"));
    for (usize at = 0; at < LongNameBytes; ++at) {
        REQUIRE(longPath.TryAppend("c"));
    }

    AssetDuplicateResult result(env.allocator);
    AssetDiagnostic failure;
    auto tooLong =
        service.Duplicate(lock, "/Game/Leaf", longPath.View(), result, nullptr, &failure);
    CHECK(!tooLong && tooLong.Error() == ErrorCode::CapacityExceeded &&
          failure.code == AssetDiagnosticCode::InvalidPath && files.NodeCount() == 4);
    REQUIRE(service.Duplicate(lock, "/Game/Leaf", "/Game/Short", result));
    CHECK(files.Has("Content/Short.rasset"));
}

// 할당 실패: 편집한 열린 Hero(bulk·의존)와 소유 기록 Leaf의 복제 — 모든 할당 실패는 OutOfMemory이고
// 대상 파일·색인이 그대로다(GC 표 실패는 Registry가 삼킨 성공).
TEST(AssetDuplicate_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    const StringView sourcesToCopy[] = {"/Game/Dir/Hero", "/Game/Leaf"};
    for (StringView sourcePath : sourcesToCopy) {
        DuplicateScene scene(
            env, sources, sourcePath == "/Game/Leaf" ? "duplicate-oom-leaf" : "duplicate-oom-hero");
        REQUIRE(scene.ready);
        REQUIRE(PutLeafRecord(env, sources, scene, true));
        TaggedFailAllocator failing(env.allocator);
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            AssetService service(failing, scene.layout, scene.Config(env));
            REQUIRE(service.Initialize());
            String out(env.allocator);
            REQUIRE(service.Info(sourcePath, AssetInfoOptions{}, out));
            // 저장하지 않은 새 패키지가 있으면 대상·원본 판정이 그 경로도 본다.
            auto unsavedPath = ParseLogicalPath(env.allocator, "/Game/Pending");
            REQUIRE(unsavedPath);
            REQUIRE(
                service.Packages().Create(unsavedPath.Value(), IdOf<TestAsset>(), FixedId(0xf50)));
            // 쌓인 간선 편집(새 패키지 → Leaf)이 있으면 복제가 먼저 적용한다(그 할당도 실패 지점).
            REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));
            const ObjectHandle pending = RepresentativeOf(service.Packages(), 0xf50);
            TestAsset* pendingValue =
                pending.IsNull() ? nullptr
                                 : Pointer<TestAsset>(service.Packages().Registry(), pending);
            REQUIRE(pendingValue != nullptr);
            REQUIRE(service.Packages().Registry().SetReference(
                service.Packages().Access(), pending, pendingValue->hard,
                RepresentativeOf(service.Packages(), LeafId)));
            AssetDuplicateResult result(failing);
            Array<EditorWarning> warnings(env.allocator);
            failing.FailOnAttempt(attempt);
            auto made =
                service.Duplicate(scene.lock, sourcePath, "/Game/Copies/Hero2", result, &warnings);
            const bool failed = failing.Failed();
            const uint32 tag = failing.FailedTag();
            failing.DisableFailure();
            if (!failed || (made && tag == GcReferenceTableTag)) {
                REQUIRE(made);
                CHECK(scene.files.Has(CopyFile) && Matches(env, scene, service.Index()));
                if (!failed) {
                    break;
                }

                scene.files.Remove(CopyFile);
                continue;
            }

            CHECK(!made && made.Error() == ErrorCode::OutOfMemory && warnings.IsEmpty());
            CHECK(!scene.files.Has(CopyFile) && Matches(env, scene, service.Index()));
            ++failures;
        }

        CHECK(failures > 0);
    }
}

// 실제 디스크: bulk를 읽어 캐시 핸들이 열린 Hero를 복제하고 사본을 열어 bulk를 읽는다.
TEST(AssetDuplicate_RealDisk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject project("duplicate-disk");
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
    config.packages.registryContext = ContextId{0x44555044ull, 1};
    config.packages.registryOptions.names = &env.names;
    AssetService service(env.allocator, layout, config);
    REQUIRE(service.Initialize());
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out));
    const EditorPackageInfo* hero = service.Packages().Find(FixedId(HeroId));
    REQUIRE(hero != nullptr);
    TestAsset* value = AssetOf(service.Packages(), hero->package);
    REQUIRE(value != nullptr);
    REQUIRE(hero->store->Read(value->pixels));
    AssetDuplicateResult result(env.allocator);
    REQUIRE(service.Duplicate(lock, "/Game/Dir/Hero", "/Game/Copies/Hero2", result));
    REQUIRE(service.Info("/Game/Copies/Hero2", AssetInfoOptions{}, out));
    const EditorPackageInfo* copy = service.Packages().Find(result.id);
    REQUIRE(copy != nullptr);
    TestAsset* copied = AssetOf(service.Packages(), copy->package);
    REQUIRE(copied != nullptr);
    auto pixels = copy->store->Read(copied->pixels);
    CHECK(pixels && pixels.Value().Size() == PixelBytes);
    // 원본은 그대로 원본 파일에 이어져 있다(정보는 다음 Open까지 유효 — 다시 찾는다).
    hero = service.Packages().Find(FixedId(HeroId));
    REQUIRE(hero != nullptr);
    CHECK(hero->store->IsAttached());
}

} // namespace asset_duplicate_tests_file
