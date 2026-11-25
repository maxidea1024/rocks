#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "AssetDatabaseTestSupport.h"
#include "AssetIndexCompare.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <Core/Text/StringBuilder.h>
#include <TestSupport/NFailAllocator.h>

// 고아 출력 시험이다(Docs/Design/AssetSystem 7.1절 17 A2e4a): 판정(소유 기록이 적은 출력은
// 고아 아님, 적지 않음 NotListed, owner 없음 OwnerMissing, owner가 기록 없음·참조 기록
// OwnerNotImporter, 같은 id owner 둘 — 하나라도 적으면 아님·소유 기록 있으면 NotListed·없으면
// OwnerNotImporter, 기록 없는·소유 기록 항목은 고아 아님), Orphans 순서, Verify 경고 진단·줄·경고
// 판정, 캐시 왕복의 출력 목록, 서비스 DeleteOrphan(거부·참조자·지우기·할당 실패).
using namespace rocks;
namespace asset_orphan_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;

namespace {

constexpr uint64 OwnerId = 0xa00;
constexpr uint64 ListedId = 0xa01;
constexpr uint64 UnlistedId = 0xa02;
constexpr uint64 LostOwnerId = 0xa03;
constexpr uint64 PlainOwnerId = 0xa04;
constexpr uint64 OfPlainId = 0xa05;
constexpr uint64 ReferenceOwnerId = 0xa06;
constexpr uint64 OfReferenceId = 0xa07;
constexpr uint64 TwinOwnerId = 0xa08;
constexpr uint64 TwinListedId = 0xa09;
constexpr uint64 TwinUnlistedId = 0xa0a;
constexpr uint64 TwinPlainId = 0xa0b;
constexpr uint64 OfTwinPlainId = 0xa0c;
constexpr uint64 MissingId = 0xa0f;
constexpr uint64 MissingReferenceOwnerId = 0xa0e;

PackageReadOptions ReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    options.engineVersion = TestEngineVersion;
    return options;
}

// id 패키지(TestAsset)를 쓴다: owner가 null이면 기록 없음, owner == id면 소유 기록(출력 = outputs,
// 첫째는 자신), 아니면 owner를 가리키는 참조 기록 + ImportOwner 의존.
Array<uint8> WritePackage(AssetEnv& env, uint64 id, uint64 owner,
                          Span<const uint64> outputs = {}) noexcept {
    RootRef<Package> package;
    if (MakeAsset<TestAsset>(*env.source, id, package).IsNull()) {
        return Array<uint8>(env.allocator);
    }

    PackageWriteOptions options = EditorOptions();
    Array<uint8> record(env.allocator);
    const PackageDependencyEntry owners[] = {
        {FixedId(owner), PackageDependencyKind::ImportOwner, TypeId{}, false}};
    if (owner != 0) {
        const ImportSourceInput sources[] = {{"source", "Raw", "Source.png", Sha256Digest{}, 1}};
        ImportOutputInput produced[4];
        const char* const keys[] = {"", "a", "b", "c"};
        ImportRecordInput input;
        input.owner = FixedId(owner);
        if (owner == id) {
            for (usize index = 0; index < outputs.Size(); ++index) {
                produced[index] =
                    ImportOutputInput{keys[index], FixedId(outputs[index]), IdOf<TestAsset>()};
            }

            input.importerId = Guid{0x4f52, 1};
            input.importerName = "OrphanImporter";
            input.importerVersion = 1;
            input.sources = sources;
            input.outputs = Span<const ImportOutputInput>(produced, outputs.Size());
        } else {
            options.extraDependencies = owners;
        }

        auto written = WriteImportRecord(env.allocator, FixedId(id), input);
        if (!written) {
            return Array<uint8>(env.allocator);
        }

        record = Move(written.Value());
        options.importRecord = record.View();
    }

    return Write(env.allocator, *env.source, package.Handle(), options);
}

struct OrphanCase {
    const char* name;
    uint64 id;
    AssetOrphanReason reason;
};

} // namespace

// 판정·목록·Verify·줄·경고 판정·캐시 왕복.
TEST(AssetOrphan_Judgement) {
    AssetEnv env;
    REQUIRE(env.Ready());
    // 디스크 임시 루트(잠금·캐시 파일)와 그 Content에 놓인 메모리 파일 시스템.
    TempProject disk("orphan-judgement");
    REQUIRE(disk.made);
    Project project(env.allocator, disk.Root());
    REQUIRE(project.ready && project.files.Ready());
    MemoryFileSystem& files = project.files;
    const uint64 ownerOutputs[] = {OwnerId, ListedId};
    const uint64 twinListing[] = {TwinOwnerId, TwinListedId};
    const uint64 twinOther[] = {TwinOwnerId};
    struct FileSpec {
        const char* name;
        uint64 id;
        uint64 owner;
        Span<const uint64> outputs;
    };
    const FileSpec specs[] = {
        {"Owner.rasset", OwnerId, OwnerId, ownerOutputs},
        {"Listed.rasset", ListedId, OwnerId, {}},
        {"Unlisted.rasset", UnlistedId, OwnerId, {}},
        {"LostOwner.rasset", LostOwnerId, MissingId, {}},
        {"PlainOwner.rasset", PlainOwnerId, 0, {}},
        {"OfPlain.rasset", OfPlainId, PlainOwnerId, {}},
        {"ReferenceOwner.rasset", ReferenceOwnerId, MissingReferenceOwnerId, {}},
        {"OfReference.rasset", OfReferenceId, ReferenceOwnerId, {}},
        {"TwinOwnerA.rasset", TwinOwnerId, TwinOwnerId, twinListing},
        {"TwinOwnerB.rasset", TwinOwnerId, TwinOwnerId, twinOther},
        {"TwinListed.rasset", TwinListedId, TwinOwnerId, {}},
        {"TwinUnlisted.rasset", TwinUnlistedId, TwinOwnerId, {}},
        {"TwinPlainA.rasset", TwinPlainId, 0, {}},
        {"TwinPlainB.rasset", TwinPlainId, 0, {}},
        {"OfTwinPlain.rasset", OfTwinPlainId, TwinPlainId, {}},
    };
    for (const FileSpec& spec : specs) {
        const Array<uint8> bytes = WritePackage(env, spec.id, spec.owner, spec.outputs);
        REQUIRE(!bytes.IsEmpty() && files.AddFile(spec.name, bytes.View()));
    }

    REQUIRE(project.Rebuild());
    const OrphanCase cases[] = {
        {"/Game/Owner", OwnerId, AssetOrphanReason::None},
        {"/Game/Listed", ListedId, AssetOrphanReason::None},
        {"/Game/Unlisted", UnlistedId, AssetOrphanReason::NotListed},
        {"/Game/LostOwner", LostOwnerId, AssetOrphanReason::OwnerMissing},
        {"/Game/PlainOwner", PlainOwnerId, AssetOrphanReason::None},
        {"/Game/OfPlain", OfPlainId, AssetOrphanReason::OwnerNotImporter},
        {"/Game/ReferenceOwner", ReferenceOwnerId, AssetOrphanReason::OwnerMissing},
        {"/Game/OfReference", OfReferenceId, AssetOrphanReason::OwnerNotImporter},
        {"/Game/TwinListed", TwinListedId, AssetOrphanReason::None},
        {"/Game/TwinUnlisted", TwinUnlistedId, AssetOrphanReason::NotListed},
        {"/Game/OfTwinPlain", OfTwinPlainId, AssetOrphanReason::OwnerNotImporter},
    };
    usize expectedOrphans = 0;
    for (const OrphanCase& item : cases) {
        auto path = ParseLogicalPath(env.allocator, item.name);
        REQUIRE(path);
        auto entry = project.index.FindByPath(path.Value());
        REQUIRE(entry);
        CHECK(project.index.OrphanReason(*entry.Value()) == item.reason);
        expectedOrphans += item.reason == AssetOrphanReason::None ? 0 : 1;
    }

    // 같은 id 파일의 판정(소유 기록은 고아가 아니다).
    for (usize index = 0; index < project.index.Count(); ++index) {
        const AssetIndexEntry& entry = project.index.At(index);
        if (entry.id == FixedId(TwinOwnerId) || entry.id == FixedId(TwinPlainId)) {
            CHECK(project.index.OrphanReason(entry) == AssetOrphanReason::None);
        }
    }

    // 목록은 경로 키 순이다.
    Array<const AssetIndexEntry*> orphans(env.allocator);
    REQUIRE(project.index.Orphans(orphans));
    REQUIRE(orphans.Size() == expectedOrphans);
    for (usize index = 1; index < orphans.Size(); ++index) {
        CHECK(StringView::CompareBytes(orphans[index - 1]->key.View(), orphans[index]->key.View()) <
              0);
    }

    // Verify: 고아마다 경고 진단(relatedAsset = owner, detail = 사유)과 줄.
    Array<AssetIndexDiagnostic> verified(env.allocator);
    REQUIRE(project.index.Verify(nullptr, verified));
    usize warnings = 0;
    bool lines[3] = {};
    for (const AssetIndexDiagnostic& diagnostic : verified) {
        if (diagnostic.code != AssetDiagnosticCode::OrphanedOutput) {
            CHECK(!AssetIndexDiagnosticIsWarning(diagnostic));
            continue;
        }

        ++warnings;
        CHECK(AssetIndexDiagnosticIsWarning(diagnostic) && diagnostic.error == ErrorCode::NotFound);
        StringBuilder<256> line(env.allocator);
        REQUIRE(AppendAssetIndexDiagnosticLine(line, diagnostic));
        if (diagnostic.asset == FixedId(LostOwnerId)) {
            CHECK(diagnostic.relatedAsset == FixedId(MissingId));
            lines[0] = line.View().StartsWith("OrphanedOutput: Content/LostOwner.rasset: warning "
                                              "the owning import ") &&
                       line.View().EndsWith(" is not in the index\n");
        } else if (diagnostic.asset == FixedId(OfPlainId)) {
            lines[1] = line.View().EndsWith(" has no import record of its own\n");
        } else if (diagnostic.asset == FixedId(UnlistedId)) {
            lines[2] = line.View().EndsWith(" does not list this output\n");
        }
    }

    CHECK(warnings == expectedOrphans && lines[0] && lines[1] && lines[2]);

    // 캐시 왕복은 출력 목록을 그대로 둔다(SameIndex가 목록을 비교한다).
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(project.layout, 0));
    {
        AssetIndex copy(env.allocator, project.layout);
        REQUIRE(copy.Rebuild(files, ReadOptions()));
        REQUIRE(copy.Count() == project.index.Count());
        REQUIRE(WriteAssetIndexCache(platform::PhysicalFileSystem(), project.layout, lock, copy,
                                     ReadOptions()));
        AssetIndexCache cache(env.allocator);
        auto loaded = cache.Load(platform::PhysicalFileSystem(), project.layout, ReadOptions());
        REQUIRE(loaded && loaded.Value());
        AssetIndex reused(env.allocator, project.layout);
        REQUIRE(reused.Rebuild(files, ReadOptions(), &cache));
        CHECK(SameIndex(copy, reused));
    }

    // 할당 실패: Orphans·Verify의 고아 진단.
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        Array<const AssetIndexEntry*> listed(failing);
        Array<AssetIndexDiagnostic> checked(failing);
        failing.FailOnAttempt(attempt);
        Result<void> step = project.index.Orphans(listed);
        if (step) {
            step = project.index.Verify(nullptr, checked);
        }

        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(step);
            break;
        }

        CHECK(!step && step.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
}

// 판정할 수 없음(Undetermined — 고아가 아니다): owner의 ImportRecord를 읽지 못함(다른 스키마 —
// UnsupportedVersion 진단), owner가 색인에 없고 그 id의 패키지를 읽지 못함(손상 Exports —
// 진단 asset = owner), owner가 색인에 없고 id를 모르는 진단이 있음(손상 파일). 목록·Verify 경고에
// 나오지 않고 DeleteOrphan은 거부한다. 판정할 수 없게 하던 파일을 고치면 고아로 바뀐다.
TEST(AssetOrphan_Undetermined) {
    AssetEnv env;
    REQUIRE(env.Ready());
    TempProject disk("orphan-undetermined");
    REQUIRE(disk.made);
    Project project(env.allocator, disk.Root());
    REQUIRE(project.ready && project.files.Ready());
    MemoryFileSystem& files = project.files;
    // owner 0xb00: 기록이 다른 스키마(읽지 못함). 출력 0xb01이 가리킨다.
    {
        RootRef<Package> package;
        REQUIRE(!MakeAsset<TestAsset>(*env.source, 0xb00, package).IsNull());
        const Array<uint8> record = OneFieldRecord(env.allocator, Guid{0x7A, 0x6D});
        PackageWriteOptions options = EditorOptions();
        options.importRecord = record.View();
        const Array<uint8> bytes = Write(env.allocator, *env.source, package.Handle(), options);
        REQUIRE(!bytes.IsEmpty() && files.AddFile("UnreadableOwner.rasset", bytes.View()));
    }

    // owner 0xb06: 기록 형식 위반(CorruptPackage — 칸 ImportRecord). 출력 0xb07이 가리킨다.
    {
        RootRef<Package> package;
        REQUIRE(!MakeAsset<TestAsset>(*env.source, 0xb06, package).IsNull());
        const Array<uint8> record =
            OneFieldRecord(env.allocator, ImportRecordSchema, ImportRecordVersion);
        PackageWriteOptions options = EditorOptions();
        options.importRecord = record.View();
        const Array<uint8> bytes = Write(env.allocator, *env.source, package.Handle(), options);
        REQUIRE(!bytes.IsEmpty() && files.AddFile("CorruptRecordOwner.rasset", bytes.View()));
    }

    const Array<uint8> ofCorruptRecord = WritePackage(env, 0xb07, 0xb06);
    REQUIRE(!ofCorruptRecord.IsEmpty() &&
            files.AddFile("OfCorruptRecord.rasset", ofCorruptRecord.View()));
    const Array<uint8> ofUnreadable = WritePackage(env, 0xb01, 0xb00);
    REQUIRE(!ofUnreadable.IsEmpty() && files.AddFile("OfUnreadable.rasset", ofUnreadable.View()));
    // owner 0xb02: 패키지를 읽지 못함(Exports 섹션 손상 — 헤더 id는 남는다). 출력 0xb03.
    {
        const Array<uint8> owner = WritePackage(env, 0xb02, 0);
        EditablePackage edit(env.allocator);
        REQUIRE(edit.Parse(owner.View()));
        bool corrupted = false;
        for (EditableSection& section : edit.sections) {
            if (section.kind == static_cast<uint32>(PackageSectionKind::Exports) &&
                !section.bytes.IsEmpty()) {
                section.bytes[0] = static_cast<uint8>(section.bytes[0] ^ 0xff);
                corrupted = true;
            }
        }

        REQUIRE(corrupted);
        const Array<uint8> broken = edit.Build();
        REQUIRE(!broken.IsEmpty() && files.AddFile("BrokenOwner.rasset", broken.View()));
    }

    const Array<uint8> ofBroken = WritePackage(env, 0xb03, 0xb02);
    REQUIRE(!ofBroken.IsEmpty() && files.AddFile("OfBroken.rasset", ofBroken.View()));
    // owner 0xb04는 어디에도 없다. 출력 0xb05.
    const Array<uint8> ofNowhere = WritePackage(env, 0xb05, 0xb04);
    REQUIRE(!ofNowhere.IsEmpty() && files.AddFile("OfNowhere.rasset", ofNowhere.View()));
    REQUIRE(project.Rebuild());
    auto reasonOf = [&](StringView text) noexcept {
        auto path = ParseLogicalPath(env.allocator, text);
        auto entry = path ? project.index.FindByPath(path.Value())
                          : Result<const AssetIndexEntry*>::Failure(path.Error());
        CHECK(entry);
        return entry ? project.index.OrphanReason(*entry.Value()) : AssetOrphanReason::None;
    };
    // 손상 owner의 진단은 id가 있고 칸이 ImportRecord가 아니다(기록을 읽지 못한 진단과 다르다).
    bool brokenSeen = false;
    for (const AssetIndexDiagnostic& diagnostic : project.index.Diagnostics()) {
        if (diagnostic.asset == FixedId(0xb02)) {
            brokenSeen = true;
            CHECK(diagnostic.section != static_cast<uint32>(PackageSectionKind::ImportRecord));
        }
    }

    REQUIRE(brokenSeen);
    // 기록 진단: 다른 스키마는 UnsupportedVersion, 형식 위반은 CorruptPackage(둘 다 칸
    // ImportRecord).
    for (const AssetIndexDiagnostic& diagnostic : project.index.Diagnostics()) {
        if (diagnostic.asset == FixedId(0xb00)) {
            CHECK(diagnostic.code == AssetDiagnosticCode::UnsupportedVersion);
        }

        if (diagnostic.asset == FixedId(0xb06)) {
            CHECK(diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
                  diagnostic.section == static_cast<uint32>(PackageSectionKind::ImportRecord));
        }
    }

    CHECK(reasonOf("/Game/OfUnreadable") == AssetOrphanReason::Undetermined);
    CHECK(reasonOf("/Game/OfCorruptRecord") == AssetOrphanReason::Undetermined);
    CHECK(reasonOf("/Game/OfBroken") == AssetOrphanReason::Undetermined);
    // 손상 owner의 진단만 있고(asset이 있다) id를 모르는 진단은 없다 — 없는 owner는 확정이다.
    CHECK(reasonOf("/Game/OfNowhere") == AssetOrphanReason::OwnerMissing);
    Array<const AssetIndexEntry*> orphans(env.allocator);
    REQUIRE(project.index.Orphans(orphans));
    REQUIRE(orphans.Size() == 1);
    CHECK(orphans[0]->id == FixedId(0xb05));
    Array<AssetIndexDiagnostic> verified(env.allocator);
    REQUIRE(project.index.Verify(nullptr, verified));
    usize warnings = 0;
    for (const AssetIndexDiagnostic& diagnostic : verified) {
        warnings += diagnostic.code == AssetDiagnosticCode::OrphanedOutput ? 1 : 0;
    }

    CHECK(warnings == 1);

    // 판정 사실 표(진단 id·읽지 못한 기록 id)의 할당 실패: Orphans·Verify가 OutOfMemory.
    {
        test::NFailAllocator failing(env.allocator);
        AssetIndex attempted(failing, project.layout);
        REQUIRE(attempted.Rebuild(files, ReadOptions()));
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            Array<const AssetIndexEntry*> listed(env.allocator);
            Array<AssetIndexDiagnostic> checked(env.allocator);
            failing.FailOnAttempt(attempt);
            Result<void> step = attempted.Orphans(listed);
            if (step) {
                step = attempted.Verify(nullptr, checked);
            }

            const bool triggered = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!triggered) {
                REQUIRE(step);
                CHECK(listed.Size() == 1);
                break;
            }

            CHECK(!step && step.Error() == ErrorCode::OutOfMemory);
            ++failures;
        }

        CHECK(failures > 0);
    }

    // id를 모르는 진단(손상 파일)이 생기면 없는 owner도 확정하지 않는다.
    const uint8 junk[] = {1, 2, 3};
    REQUIRE(files.AddFile("Junk.rasset", Span<const uint8>(junk, 3)));
    REQUIRE(project.Rebuild());
    CHECK(reasonOf("/Game/OfNowhere") == AssetOrphanReason::Undetermined);
    orphans.Clear();
    REQUIRE(project.index.Orphans(orphans));
    CHECK(orphans.IsEmpty());

    // 서비스는 판정할 수 없는 출력을 지우지 않는다.
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(project.layout, 0));
    AssetTypeHookTable hooks(env.allocator, env.types);
    AssetServiceConfig config;
    config.packages.types = &env.types;
    config.packages.files = &files;
    config.packages.hooks = &hooks;
    config.packages.read.engineVersion = TestEngineVersion;
    config.packages.registryContext = ContextId{0x4f525055ull, 1};
    config.packages.registryOptions.names = &env.names;
    AssetService service(env.allocator, project.layout, config);
    REQUIRE(service.Initialize());
    AssetDeleteResult result(env.allocator);
    AssetDiagnostic failure;
    const StringView undetermined[] = {"/Game/OfUnreadable", "/Game/OfCorruptRecord",
                                       "/Game/OfBroken", "/Game/OfNowhere"};
    for (StringView path : undetermined) {
        failure = AssetDiagnostic{};
        auto refused = service.DeleteOrphan(lock, path, result, nullptr, &failure);
        CHECK(!refused && refused.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::None);
    }

    CHECK(files.Has("OfUnreadable.rasset") && files.Has("OfBroken.rasset") &&
          files.Has("OfNowhere.rasset"));

    // 판정을 막던 파일을 고치면 고아로 바뀐다(손상 파일을 지우면 없는 owner가 확정된다).
    files.Remove("Junk.rasset");
    REQUIRE(project.Rebuild());
    CHECK(reasonOf("/Game/OfNowhere") == AssetOrphanReason::OwnerMissing);
}

// 서비스 DeleteOrphan: Initialize 전·잠금 아님·경로 규칙·없음·같은 경로 키 둘·고아 아님은
// 거부(파일 그대로), 참조자가 있는 고아는 ReferencedBy, 고아는 지운다, 할당 실패는 OutOfMemory.
TEST(AssetOrphan_ServiceDelete) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject root("orphan-delete");
    REQUIRE(root.made);
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(root.Root()));
    MemoryFileSystem files(env.allocator, layout.Root());
    // Hero는 고아(owner 없음)이고 Villain이 참조한다. Lost는 아무도 참조하지 않는 고아다.
    const Array<uint8> lost = WritePackage(env, LostOwnerId, MissingId);
    RootRef<Package> heroPackage;
    REQUIRE(files.Ready() && files.AddDirectory("Content") && files.AddDirectory("Config") &&
            files.AddFile("Content/Base.rasset", sources.base.View()) &&
            files.AddFile("Content/Derived.rasset", sources.derived.View()) &&
            files.AddFile("Content/Villain.rasset", sources.villain.View()) &&
            files.AddFile("Content/Lost.rasset", lost.View()) &&
            files.AddDirectory("Content/Split") && files.AddDirectory("Content/split") &&
            files.AddFile("Content/Split/Dup.rasset", sources.cycleA.View()) &&
            files.AddFile("Content/split/dup.rasset", sources.cycleB.View()));
    {
        ImportRecordInput input;
        input.owner = FixedId(MissingId);
        auto record = WriteImportRecord(env.allocator, FixedId(HeroId), input);
        REQUIRE(record);
        const PackageDependencyEntry owners[] = {
            {FixedId(MissingId), PackageDependencyKind::ImportOwner, TypeId{}, false}};
        PackageWriteOptions options = EditorOptions();
        options.bulkSource = &sources.bulk;
        options.importRecord = record.Value().View();
        options.extraDependencies = owners;
        const Array<uint8> hero =
            Write(env.allocator, *env.source, sources.packages[2].Handle(), options);
        REQUIRE(!hero.IsEmpty() && files.AddFile("Content/Hero.rasset", hero.View()));
    }

    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(layout, 0));
    AssetTypeHookTable hooks(env.allocator, env.types);
    AssetServiceConfig config;
    config.packages.types = &env.types;
    config.packages.files = &files;
    config.packages.hooks = &hooks;
    config.packages.read.engineVersion = TestEngineVersion;
    config.packages.registryContext = ContextId{0x4f525048ull, 1};
    config.packages.registryOptions.names = &env.names;
    AssetService service(env.allocator, layout, config);
    AssetDeleteResult result(env.allocator);
    AssetDiagnostic failure;
    CHECK(service.DeleteOrphan(lock, "/Game/Lost", result).Error() == ErrorCode::InvalidArgument);
    REQUIRE(service.Initialize());
    AssetDatabaseLock other;
    CHECK(service.DeleteOrphan(other, "/Game/Lost", result).Error() == ErrorCode::InvalidArgument);
    auto rejects = [&](StringView path, ErrorCode error, AssetDiagnosticCode code) noexcept {
        failure = AssetDiagnostic{};
        auto deleted = service.DeleteOrphan(lock, path, result, nullptr, &failure);
        return !deleted && deleted.Error() == error && failure.code == code &&
               files.Has("Content/Lost.rasset") && files.Has("Content/Hero.rasset") &&
               files.Has("Content/Base.rasset");
    };
    CHECK(rejects("Game/Lost", ErrorCode::InvalidArgument, AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("/Game/Nowhere", ErrorCode::NotFound, AssetDiagnosticCode::None));
    CHECK(rejects("/Game/Split/Dup", ErrorCode::AlreadyExists, AssetDiagnosticCode::DuplicatePath));
    CHECK(rejects("/Game/Base", ErrorCode::InvalidArgument, AssetDiagnosticCode::None) &&
          failure.asset == FixedId(BaseId));
    CHECK(rejects("/Game/Hero", ErrorCode::InvalidArgument, AssetDiagnosticCode::ReferencedBy) &&
          failure.related == FixedId(VillainId) && result.referencerIds.Size() == 1);

    // 할당 실패 뒤 고아를 지운다. 긴 경로는 해석의 할당도 실패 지점이다(짧은 경로는 할당하지
    // 않는다).
    test::NFailAllocator failing(env.allocator);
    {
        AssetService parsing(failing, layout, config);
        REQUIRE(parsing.Initialize());
        AssetDeleteResult attempted(env.allocator);
        failing.FailOnAttempt(1);
        auto longPath = parsing.DeleteOrphan(
            lock, "/Game/A-path-long-enough-to-leave-the-inline-storage-of-the-string", attempted);
        failing.DisableFailure();
        CHECK(!longPath && longPath.Error() == ErrorCode::OutOfMemory);
    }

    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetDeleteResult attempted(failing);
        failing.FailOnAttempt(attempt);
        auto deleted = service.DeleteOrphan(lock, "/Game/Lost", attempted);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(deleted);
            CHECK(attempted.id == FixedId(LostOwnerId) && !files.Has("Content/Lost.rasset"));
            break;
        }

        CHECK(!deleted && deleted.Error() == ErrorCode::OutOfMemory &&
              files.Has("Content/Lost.rasset"));
        ++failures;
    }

    CHECK(failures > 0);

    // 같은 서비스에서 owner를 지우면(출력이 ImportOwner로 참조 — force) 그 출력은 곧바로 고아다.
    const uint64 ownedOutputs[] = {OwnerId, ListedId};
    const Array<uint8> owner = WritePackage(env, OwnerId, OwnerId, ownedOutputs);
    const Array<uint8> output = WritePackage(env, ListedId, OwnerId);
    REQUIRE(files.AddFile("Content/Owner.rasset", owner.View()) &&
            files.AddFile("Content/Output.rasset", output.View()));
    {
        AssetIndexRefreshPaths paths;
        const StringView names[] = {"Owner.rasset", "Output.rasset"};
        paths.files = names;
        REQUIRE(service.Index().RefreshPaths(files, paths, ReadOptions()));
    }

    failure = AssetDiagnostic{};
    CHECK(!service.DeleteOrphan(lock, "/Game/Output", result, nullptr, &failure));
    REQUIRE(service.Delete(lock, "/Game/Owner", true, result));
    REQUIRE(service.DeleteOrphan(lock, "/Game/Output", result));
    CHECK(!files.Has("Content/Output.rasset") && !files.Has("Content/Owner.rasset"));

    AssetIndex fresh(env.allocator, layout);
    REQUIRE(fresh.Rebuild(files, ReadOptions()));
    CHECK(SameIndex(fresh, service.Index()));
}

} // namespace asset_orphan_tests_file
