#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "AssetDatabaseTestSupport.h"
#include "MemoryFileSystem.h"

#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <stdio.h>
#include <string.h>

// 에디터 패키지 색인 시험이다(Docs/Design/AssetSystem 7.1절 11·13 — A2b): 실제 디스크의
// 검색과 모든 조회(경로·id·디렉터리·타입·태그·정방향 의존의 모든 종류 묶음·역방향 참조자), Verify의
// MissingDependency·TypeMismatch(상속·미등록·종료한 TypeRegistry), 메모리 파일 시스템으로 만든 진단
// 장면(InvalidPath·패키지 판정·쿠킹·다른 Tags 스키마·깨진 Tags·사라진 파일·IO 실패·링크·장치·같은
// 경로 키·같은 id), ImportRecord 요약과 ImportOwner 대조(A2c2a — 7.1절 15), 열거 실패, 모든 할당
// 실패.
using namespace rocks;
namespace asset_index_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;

namespace {

constexpr uint64 HeroId = 2100;
constexpr uint64 BaseId = 2101;
constexpr uint64 DerivedId = 2102;
constexpr uint64 BuildId = 2103;
constexpr uint64 ImportOwnerId = 2104;
constexpr uint64 VillainId = 2105;

// Verify 결과에서 고아 출력 경고를 떼고 그 수를 준다(Hero의 참조 기록 — owner ImportOwnerId가
// 색인에 없다, 7.1절 17 A2e4). 의존 대조 시험은 나머지를 본다.
usize TakeOrphans(Array<AssetIndexDiagnostic>& verified) noexcept {
    usize orphans = 0;
    for (usize index = verified.Size(); index-- > 0;) {
        if (verified[index].code == AssetDiagnosticCode::OrphanedOutput) {
            CHECK(verified[index].asset == FixedId(HeroId) &&
                  verified[index].relatedAsset == FixedId(ImportOwnerId) &&
                  verified[index].detail == static_cast<uint64>(AssetOrphanReason::OwnerMissing));
            (void)verified.TryRemoveAt(index);
            ++orphans;
        }
    }

    return orphans;
}

constexpr uint64 CookedId = 2106;
constexpr uint64 OtherTagsId = 2107;
constexpr uint64 BadTagsId = 2108;
constexpr uint64 MissingSoftId = 2109;
constexpr uint64 PlainId = 2110;
// Plain의 id는 high가 다르다(id 순 정렬·조회가 high를 먼저 보는지).
constexpr uint64 PlainHigh = 1;
// 할당 실패 장면의 깊은 디렉터리 단계 수다(구성 요소 61 byte씩 — 경로 임시 저장소 512 byte를
// 넘는다).
constexpr uint32 DeepLevels = 9;
// 할당 실패 장면의 긴 프로젝트 루트(구성 요소 65 byte씩 단계 수)와 그 Content 경로가 넘어야 하는
// 길이(경로 임시 저장소 512 byte)다.
constexpr uint32 LongRootLevels = 8;
constexpr usize LongRootMinimumBytes = 512;
// 진단 줄 시험의 줄 저장소 인라인 byte 수(작게 두어 줄마다 할당이 생긴다)와 미리 채우는 최대 byte
// 수(가장 긴 줄보다 길게 — 자라는 지점이 줄의 모든 조각 위를 지난다)다.
constexpr usize LineInlineBytes = 8;
constexpr usize LinePrefixBytes = 160;
// 종류 묶음 bit 수다(PackageDependencyKind 다섯 — 묶음 2^5개를 모두 본다).
constexpr uint32 DependencyKindCount = 5;

const PackageDependencyEntry HeroExtraDependencies[] = {
    {FixedId(BuildId), PackageDependencyKind::Build, TypeId{}, false},
    {FixedId(ImportOwnerId), PackageDependencyKind::ImportOwner, TypeId{}, false}};
// 색인 읽기가 모르는 커스텀 버전이다(그 패키지는 읽기 전용 — 5.3절).
const PackageCustomVersion UnknownCustomVersions[] = {{Guid{0x1DE, 0x99}, 1}};
const PackageTagInput HeroTags[] = {{"Kind", "Hero"},
                                    {"UnrealExchange.FormerPaths", "/Game/Old/B"},
                                    {"UnrealExchange.FormerPaths", "/Game/Old/A"}};

// 시험 패키지 byte 묶음이다. 기본(source Registry): Hero(TestAsset — Hard → Base, EditorOnly →
// Derived, Soft → 2109 "Sub", Build 2103, ImportOwner 2104와 그것을 가리키는 참조 ImportRecord,
// 태그 셋), Base(BaseGameAsset),
// Derived(DerivedGameAsset), Villain(TestAsset — Hard → Derived, Soft → Hero), Plain(TestAsset 참조
// 없음), OtherTags(다른 스키마 Tags), BadTags(Tags 스키마인데 목록 없음). 변형(target Registry):
// Base id인데 대표가 DerivedGameAsset, Derived id인데 대표가 BaseGameAsset, 쿠킹 패키지.
struct IndexPackages {
    explicit IndexPackages(Allocator& allocator) noexcept
        : hero(allocator), base(allocator), derived(allocator), villain(allocator),
          plain(allocator), otherTags(allocator), badTags(allocator), baseAsDerived(allocator),
          derivedAsBase(allocator), cooked(allocator) {
    }

    Array<uint8> hero;
    Array<uint8> base;
    Array<uint8> derived;
    Array<uint8> villain;
    Array<uint8> plain;
    Array<uint8> otherTags;
    Array<uint8> badTags;
    Array<uint8> baseAsDerived;
    Array<uint8> derivedAsBase;
    Array<uint8> cooked;
};

// registry에 id 패키지와 대표 에셋 T를 만든다.
template <class T>
ObjectHandle MakeRepresentative(ObjectRegistry& registry, AssetId id, PackageKind kind,
                                RootRef<Package>& package) noexcept {
    auto made = NewPackage(registry, registry.OwnerAccess(), id, kind);
    if (!made) {
        return ObjectHandle{};
    }

    package = Move(made.Value());
    return Make<T>(registry, IdOf<T>(), package.Handle(), "");
}

// fields 하나만 든 Record byte다(schema가 다른 Tags·목록 없는 Tags에 쓴다).
Array<uint8> OneFieldRecord(Allocator& allocator, Guid schema) noexcept {
    const uint8 value[4] = {1, 2, 3, 4};
    RecordBuilder builder(allocator, schema, PackageTagsVersion);
    if (!builder.AddField(Guid{0, 9}, WireType::U32, value)) {
        return Array<uint8>(allocator);
    }

    auto built = builder.Build();
    return built ? Move(built.Value()) : Array<uint8>(allocator);
}

bool MakePackages(AssetEnv& env, IndexPackages& out) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    RootRef<Package> heroPackage;
    RootRef<Package> basePackage;
    RootRef<Package> derivedPackage;
    RootRef<Package> villainPackage;
    RootRef<Package> plainPackage;
    RootRef<Package> otherPackage;
    RootRef<Package> badPackage;
    const ObjectHandle base = MakeRepresentative<BaseGameAsset>(registry, FixedId(BaseId),
                                                                PackageKind::Editor, basePackage);
    const ObjectHandle derived = MakeRepresentative<DerivedGameAsset>(
        registry, FixedId(DerivedId), PackageKind::Editor, derivedPackage);
    const ObjectHandle hero =
        MakeRepresentative<TestAsset>(registry, FixedId(HeroId), PackageKind::Editor, heroPackage);
    const ObjectHandle villain = MakeRepresentative<TestAsset>(registry, FixedId(VillainId),
                                                               PackageKind::Editor, villainPackage);
    const ObjectHandle plain = MakeRepresentative<TestAsset>(
        registry, AssetId{Guid{PlainHigh, PlainId}}, PackageKind::Editor, plainPackage);
    const ObjectHandle other = MakeRepresentative<TestAsset>(registry, FixedId(OtherTagsId),
                                                             PackageKind::Editor, otherPackage);
    const ObjectHandle bad = MakeRepresentative<TestAsset>(registry, FixedId(BadTagsId),
                                                           PackageKind::Editor, badPackage);
    TestAsset* heroValue = Pointer<TestAsset>(registry, hero);
    TestAsset* villainValue = Pointer<TestAsset>(registry, villain);
    if (base.IsNull() || derived.IsNull() || heroValue == nullptr || villainValue == nullptr ||
        plain.IsNull() || other.IsNull() || bad.IsNull()) {
        return false;
    }

    if (!registry.SetReference(access, hero, heroValue->hard, base) ||
        !registry.SetReference(access, hero, heroValue->editorRef, derived) ||
        !heroValue->soft.TryAssign(FixedId(MissingSoftId), "Sub") ||
        !registry.SetReference(access, villain, villainValue->hard, derived) ||
        !villainValue->soft.TryAssign(FixedId(HeroId), "")) {
        return false;
    }

    auto tags = WritePackageTags(env.allocator, HeroTags);
    if (!tags) {
        return false;
    }

    const Array<uint8> otherTags = OneFieldRecord(env.allocator, Guid{0x7A, 0x6B});
    const Array<uint8> badTags = OneFieldRecord(env.allocator, PackageTagsSchema);
    ImportRecordInput heroImport;
    heroImport.owner = FixedId(ImportOwnerId);
    auto heroRecord = WriteImportRecord(env.allocator, FixedId(HeroId), heroImport);
    if (!heroRecord) {
        return false;
    }

    PackageWriteOptions heroOptions = EditorOptions();
    heroOptions.tags = tags.Value().View();
    heroOptions.importRecord = heroRecord.Value().View();
    heroOptions.extraDependencies = HeroExtraDependencies;
    PackageWriteOptions otherOptions = EditorOptions();
    otherOptions.tags = otherTags.View();
    otherOptions.customVersions = UnknownCustomVersions;
    PackageWriteOptions badOptions = EditorOptions();
    badOptions.tags = badTags.View();
    out.base = Write(env.allocator, registry, basePackage.Handle(), EditorOptions());
    out.derived = Write(env.allocator, registry, derivedPackage.Handle(), EditorOptions());
    out.hero = Write(env.allocator, registry, heroPackage.Handle(), heroOptions);
    out.villain = Write(env.allocator, registry, villainPackage.Handle(), EditorOptions());
    out.plain = Write(env.allocator, registry, plainPackage.Handle(), EditorOptions());
    out.otherTags = Write(env.allocator, registry, otherPackage.Handle(), otherOptions);
    out.badTags = Write(env.allocator, registry, badPackage.Handle(), badOptions);

    ObjectRegistry& variants = *env.target;
    RootRef<Package> baseAsDerived;
    RootRef<Package> derivedAsBase;
    RootRef<Package> cooked;
    if (MakeRepresentative<DerivedGameAsset>(variants, FixedId(BaseId), PackageKind::Editor,
                                             baseAsDerived)
            .IsNull() ||
        MakeRepresentative<BaseGameAsset>(variants, FixedId(DerivedId), PackageKind::Editor,
                                          derivedAsBase)
            .IsNull() ||
        MakeRepresentative<TestAsset>(variants, FixedId(CookedId), PackageKind::Cooked, cooked)
            .IsNull()) {
        return false;
    }

    out.baseAsDerived = Write(env.allocator, variants, baseAsDerived.Handle(), EditorOptions());
    out.derivedAsBase = Write(env.allocator, variants, derivedAsBase.Handle(), EditorOptions());
    out.cooked = Write(env.allocator, variants, cooked.Handle(), CookedOptions());
    const Array<uint8>* const all[] = {
        &out.hero,      &out.base,    &out.derived,       &out.villain,       &out.plain,
        &out.otherTags, &out.badTags, &out.baseAsDerived, &out.derivedAsBase, &out.cooked};
    for (const Array<uint8>* bytes : all) {
        if (bytes->IsEmpty()) {
            return false;
        }
    }

    return true;
}

AssetId IdOfEntry(const AssetIndexEntry* entry) noexcept {
    return entry == nullptr ? AssetId{} : entry->id;
}

// out의 항목 id가 expected(순서대로)와 같은지 본다.
bool SameIds(const Array<const AssetIndexEntry*>& out, Span<const uint64> expected) noexcept {
    if (out.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < out.Size(); ++index) {
        if (!(IdOfEntry(out[index]) == FixedId(expected[index]))) {
            return false;
        }
    }

    return true;
}

// 진단 하나의 기대값이다.
struct ExpectedDiagnostic {
    AssetDiagnosticCode code;
    ErrorCode error;
    StringView fileName;
};

bool SameDiagnostics(Span<const AssetIndexDiagnostic> actual,
                     Span<const ExpectedDiagnostic> expected) noexcept {
    bool same = actual.Size() == expected.Size();
    for (usize index = 0; index < actual.Size() || index < expected.Size(); ++index) {
        const bool both = index < actual.Size() && index < expected.Size();
        if (both && actual[index].code == expected[index].code &&
            actual[index].error == expected[index].error &&
            actual[index].fileName.View() == expected[index].fileName) {
            continue;
        }

        same = false;
        printf("  diagnostic %zu: actual %s %u %s, expected %s %u %.*s\n", index,
               index < actual.Size() ? AssetDiagnosticName(actual[index].code) : "-",
               index < actual.Size() ? static_cast<unsigned>(actual[index].error) : 0u,
               index < actual.Size() ? actual[index].fileName.CStr() : "-",
               index < expected.Size() ? AssetDiagnosticName(expected[index].code) : "-",
               index < expected.Size() ? static_cast<unsigned>(expected[index].error) : 0u,
               index < expected.Size() ? static_cast<int>(expected[index].fileName.ByteCount()) : 1,
               index < expected.Size() ? expected[index].fileName.Data() : "-");
    }

    return same;
}

// 패키지 섹션 안의 Guid byte(little endian high·low — ArchiveWriter::WriteGuid)를 모두 바꾸고 바꾼
// 수를 돌려준다.
usize ReplaceGuid(EditablePackage& edit, Guid from, Guid to) noexcept {
    uint8 fromBytes[sizeof(uint64) * 2];
    uint8 toBytes[sizeof(uint64) * 2];
    StoreLittleEndian<uint64>(fromBytes, from.high);
    StoreLittleEndian<uint64>(fromBytes + sizeof(uint64), from.low);
    StoreLittleEndian<uint64>(toBytes, to.high);
    StoreLittleEndian<uint64>(toBytes + sizeof(uint64), to.low);
    usize replaced = 0;
    for (EditableSection& section : edit.sections) {
        for (usize offset = 0; offset + sizeof(fromBytes) <= section.bytes.Size(); ++offset) {
            if (memcmp(section.bytes.Data() + offset, fromBytes, sizeof(fromBytes)) == 0) {
                memcpy(section.bytes.Data() + offset, toBytes, sizeof(toBytes));
                ++replaced;
            }
        }
    }

    return replaced;
}

// 부분 읽기로 직접 판정한 진단이다(색인 진단의 기대값).
AssetDiagnostic DirectFailure(Allocator& allocator, Span<const uint8> bytes,
                              ErrorCode& error) noexcept {
    MemoryPackageRangeReader reader(bytes);
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    AssetDiagnostic failure;
    auto view = ReadPackageIndex(allocator, reader, options, &failure);
    error = view ? ErrorCode::None : view.Error();
    return failure;
}

} // namespace

// 디스크 검색과 조회(7.1절 11·13): Content를 재귀 검색해 `.rasset`(대문자 확장자 포함)만 색인하고
// 그 밖의 파일·디렉터리 이름의 `.rasset`·빈 디렉터리는 무시한다. 항목은 경로 키 순이고 경로·키·실제
// 파일 이름·대표 타입·지문(전체 읽기와 같다)·크기·시각·의존·태그를 가진다. 비 NFC 파일 이름은
// 이름을 정규화하지 않는 파일 시스템에서 InvalidPath(NotNfcOnDisk)다. 조회: id·경로(대소문자 무시
// 키)·디렉터리(마운트·논리 경로, 바로 아래·재귀, 구성 요소 경계)·잘못된 디렉터리 InvalidArgument·
// 타입(정확·파생, 0 거부)·태그(목록 값 원소마다)·정방향 의존과 역방향 참조자(모든 종류 묶음 32개를
// 전수 대조)·Verify(Soft 누락만 — Build·ImportOwner·EditorOnly는 대조하지 않는다).
TEST(AssetIndex_DiskScanAndQueries) {
    AssetEnv env;
    REQUIRE(env.Ready());
    IndexPackages packages(env.allocator);
    REQUIRE(MakePackages(env, packages));
    TempProject project("index-disk");
    REQUIRE(project.made);
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    AssetIndex index(env.allocator, layout);
    // Content가 없으면 빈 색인이다.
    REQUIRE(index.Rebuild(platform::PhysicalFileSystem(), PackageReadOptions{}));
    CHECK(index.Count() == 0 && index.Diagnostics().IsEmpty());

    REQUIRE(
        WriteContentFile(env.allocator, project, "Characters/Hero.rasset", packages.hero.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Characters/Villain.rasset",
                             packages.villain.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Items/Base.rasset", packages.base.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Items/Weapons/Derived.RASSET",
                             packages.derived.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Items/Base.rasset.0123.tmp",
                             packages.base.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Notes.txt", packages.plain.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Cafe\xcc\x81.rasset", packages.plain.View()));
    REQUIRE(MakeContentDirectory(env.allocator, project, "Empty"));
    REQUIRE(MakeContentDirectory(env.allocator, project, "Folder.rasset"));
    REQUIRE(index.Rebuild(platform::PhysicalFileSystem(), PackageReadOptions{}));
    if constexpr (FileSystemNormalizesNames) {
        REQUIRE(index.Count() == 5);
        CHECK(index.Diagnostics().IsEmpty());
    } else {
        REQUIRE(index.Count() == 4);
        REQUIRE(index.Diagnostics().Size() == 1);
        const AssetIndexDiagnostic& nfd = index.Diagnostics()[0];
        CHECK(nfd.code == AssetDiagnosticCode::InvalidPath);
        CHECK(nfd.error == ErrorCode::InvalidArgument);
        CHECK(nfd.pathError == LogicalPathError::NotNfcOnDisk);
        CHECK(nfd.fileName.View() == "Cafe\xcc\x81.rasset");
    }

    // 경로 키 순(game/caf…는 NFC 이름일 때만 있다).
    const usize first = FileSystemNormalizesNames ? 1 : 0;
    const AssetIndexEntry& hero = index.At(first + 0);
    CHECK(hero.path.View() == "/Game/Characters/Hero");
    CHECK(hero.key.View() == "game/characters/hero");
    CHECK(hero.fileName.View() == "Characters/Hero.rasset");
    // Hero는 2104를 가리키는 참조 ImportRecord(요약은 owner만), Villain은 ImportRecord 없음이다.
    CHECK(hero.importRecord.present && hero.importRecord.owner == FixedId(ImportOwnerId));
    CHECK(hero.importRecord.importerId == Guid{} && hero.importRecord.importerName.IsEmpty() &&
          hero.importRecord.importerVersion == 0 && hero.importRecord.outputs.Size() == 0);
    CHECK(!index.At(first + 1).importRecord.present);
    CHECK(index.At(first + 1).fileName.View() == "Characters/Villain.rasset");
    CHECK(index.At(first + 2).fileName.View() == "Items/Base.rasset");
    const AssetIndexEntry& derived = index.At(first + 3);
    CHECK(derived.fileName.View() == "Items/Weapons/Derived.RASSET");
    CHECK(derived.path.View() == "/Game/Items/Weapons/Derived");
    CHECK(derived.type == IdOf<DerivedGameAsset>());
    auto full =
        ReadPackage(env.allocator, packages.hero.View(), PackageReadOptions{PackageKind::Editor});
    REQUIRE(full);
    CHECK(hero.id == FixedId(HeroId));
    CHECK(hero.type == TestAssetType());
    CHECK(hero.fingerprint.digest == full.Value().fingerprint.digest);
    CHECK(hero.fileBytes == packages.hero.Size());
    CHECK(hero.modifiedTicks > 0);
    CHECK(!hero.readOnly && !hero.duplicateId && !hero.duplicatePath);
    REQUIRE(hero.dependencies.Size() == full.Value().dependencies.Size());
    REQUIRE(hero.tags.Size() == 3);
    CHECK(hero.tags[0].key.View() == "Kind" && hero.tags[2].value.View() == "/Game/Old/B");

    // id·경로 조회.
    const uint64 ids[] = {HeroId, BaseId, DerivedId, VillainId};
    for (uint64 id : ids) {
        auto found = index.FindById(FixedId(id));
        CHECK(found && found.Value()->id == FixedId(id));
    }

    CHECK(index.FindById(FixedId(MissingSoftId)).Error() == ErrorCode::NotFound);
    CHECK(index.FindById(AssetId{}).Error() == ErrorCode::NotFound);
    auto heroPath = ParseLogicalPath(env.allocator, "/game/CHARACTERS/hero");
    auto missingPath = ParseLogicalPath(env.allocator, "/Game/Characters/Nobody");
    auto lastPath = ParseLogicalPath(env.allocator, "/Game/Zzz");
    REQUIRE(heroPath && missingPath && lastPath);
    auto byPath = index.FindByPath(heroPath.Value());
    CHECK(byPath && byPath.Value() == &hero);
    CHECK(index.FindByPath(missingPath.Value()).Error() == ErrorCode::NotFound);
    CHECK(index.FindByPath(lastPath.Value()).Error() == ErrorCode::NotFound);

    // 디렉터리 조회.
    struct Listing {
        StringView directory;
        bool recursive;
        uint32 count;
    };
    const uint32 nfcExtra = FileSystemNormalizesNames ? 1 : 0;
    const Listing listings[] = {
        {"/Game", true, 4 + nfcExtra}, {"/game", true, 4 + nfcExtra},
        {"/Game", false, nfcExtra},    {"/Game/Characters", false, 2},
        {"/Game/characters", true, 2}, {"/Game/Items", false, 1},
        {"/Game/Items", true, 2},      {"/Game/Items/Weapons", false, 1},
        {"/Game/Char", true, 0},       {"/Game/Characters/Hero", true, 0},
    };
    for (const Listing& listing : listings) {
        Array<const AssetIndexEntry*> out(env.allocator);
        REQUIRE(index.ListUnder(listing.directory, listing.recursive, out));
        if (out.Size() != listing.count) {
            printf("  ListUnder %.*s %d: %zu\n", static_cast<int>(listing.directory.ByteCount()),
                   listing.directory.Data(), static_cast<int>(listing.recursive), out.Size());
        }

        CHECK(out.Size() == listing.count);
    }

    const StringView badDirectories[] = {"Game", "/Other", "/Gamx", "/Game/", "", "/Game//Items"};
    for (StringView directory : badDirectories) {
        Array<const AssetIndexEntry*> out(env.allocator);
        CHECK(index.ListUnder(directory, true, out).Error() == ErrorCode::InvalidArgument);
        CHECK(out.IsEmpty());
    }

    // 타입 조회: 정확 일치, 파생 포함, 미등록 id, 0.
    {
        Array<const AssetIndexEntry*> out(env.allocator);
        REQUIRE(index.ListByType(IdOf<BaseGameAsset>(), nullptr, out));
        const uint64 exact[] = {BaseId};
        CHECK(SameIds(out, exact));
        out.Clear();
        REQUIRE(index.ListByType(IdOf<BaseGameAsset>(), &env.types, out));
        const uint64 withDerived[] = {BaseId, DerivedId};
        CHECK(SameIds(out, withDerived));
        out.Clear();
        REQUIRE(index.ListByType(TestAssetType(), &env.types, out));
        CHECK(out.Size() == 2 + nfcExtra);
        out.Clear();
        REQUIRE(index.ListByType(Guid{0x7E57, 0x7E57}, &env.types, out));
        CHECK(out.IsEmpty());
        CHECK(index.ListByType(TypeId{}, nullptr, out).Error() == ErrorCode::InvalidArgument);
    }

    // 태그 조회.
    {
        Array<const AssetIndexEntry*> out(env.allocator);
        const uint64 heroOnly[] = {HeroId};
        REQUIRE(index.FindByTag("Kind", "Hero", out));
        CHECK(SameIds(out, heroOnly));
        out.Clear();
        REQUIRE(index.FindByTag("UnrealExchange.FormerPaths", "/Game/Old/A", out));
        CHECK(SameIds(out, heroOnly));
        out.Clear();
        REQUIRE(index.FindByTag("UnrealExchange.FormerPaths", "/Game/Old/B", out));
        CHECK(SameIds(out, heroOnly));
        out.Clear();
        REQUIRE(index.FindByTag("Kind", "Villain", out));
        REQUIRE(index.FindByTag("kind", "Hero", out));
        REQUIRE(index.FindByTag("Hero", "Kind", out));
        CHECK(out.IsEmpty());
    }

    // 정방향 의존과 역방향 참조자: 모든 종류 묶음을 저장 내용에서 직접 고른 것과 대조한다.
    const uint64 targets[] = {HeroId,  BaseId,        DerivedId,     VillainId,
                              BuildId, ImportOwnerId, MissingSoftId, PlainId};
    uint32 mismatches = 0;
    for (uint32 kinds = 0; kinds < (1u << DependencyKindCount); ++kinds) {
        for (uint64 id : ids) {
            auto entry = index.FindById(FixedId(id));
            REQUIRE(entry);
            Array<const PackageDependencyEntry*> out(env.allocator);
            REQUIRE(index.Dependencies(FixedId(id), kinds, out));
            usize expected = 0;
            for (const PackageDependencyEntry& dependency : entry.Value()->dependencies) {
                if ((kinds & DependencyKindBit(dependency.kind)) == 0) {
                    continue;
                }

                if (expected >= out.Size() || out[expected] != &dependency) {
                    ++mismatches;
                }

                ++expected;
            }

            if (expected != out.Size()) {
                ++mismatches;
            }
        }

        for (uint64 target : targets) {
            Array<const AssetIndexEntry*> out(env.allocator);
            REQUIRE(index.Referencers(FixedId(target), kinds, out));
            usize expected = 0;
            for (usize position = 0; position < index.Count(); ++position) {
                bool refers = false;
                for (const PackageDependencyEntry& dependency : index.At(position).dependencies) {
                    refers = refers || (dependency.asset == FixedId(target) &&
                                        (kinds & DependencyKindBit(dependency.kind)) != 0);
                }

                if (!refers) {
                    continue;
                }

                if (expected >= out.Size() || out[expected] != &index.At(position)) {
                    ++mismatches;
                }

                ++expected;
            }

            if (expected != out.Size()) {
                ++mismatches;
            }
        }
    }

    CHECK(mismatches == 0);
    // 대표 장면 값: Hero의 Hard는 Base, Derived는 Hero(EditorOnly)·Villain(Hard)이 참조한다.
    {
        Array<const PackageDependencyEntry*> hard(env.allocator);
        REQUIRE(index.Dependencies(FixedId(HeroId), DependencyKindBit(PackageDependencyKind::Hard),
                                   hard));
        REQUIRE(hard.Size() == 1);
        CHECK(hard[0]->asset == FixedId(BaseId));
        CHECK(hard[0]->expectedType == IdOf<BaseGameAsset>());
        Array<const AssetIndexEntry*> referencers(env.allocator);
        REQUIRE(index.Referencers(FixedId(DerivedId), AllDependencyKinds, referencers));
        const uint64 both[] = {HeroId, VillainId};
        CHECK(SameIds(referencers, both));
        Array<const PackageDependencyEntry*> none(env.allocator);
        CHECK(index.Dependencies(FixedId(MissingSoftId), AllDependencyKinds, none).Error() ==
              ErrorCode::NotFound);
    }

    // Verify: Hero의 Soft → 2109만 누락이다.
    Array<AssetIndexDiagnostic> verified(env.allocator);
    REQUIRE(index.Verify(&env.types, verified));
    // 이 장면에는 id를 모르는 진단(손상·이름 규칙)이 있어 Hero의 owner 없음을 확정하지 않는다
    // (Undetermined — 고아 경고 없음, 7.1절 17 A2e4).
    CHECK(TakeOrphans(verified) == 0);
    REQUIRE(verified.Size() == 1);
    CHECK(verified[0].code == AssetDiagnosticCode::MissingDependency);
    CHECK(verified[0].error == ErrorCode::NotFound);
    CHECK(verified[0].fileName.View() == "Characters/Hero.rasset");
    CHECK(verified[0].asset == FixedId(HeroId));
    CHECK(verified[0].relatedAsset == FixedId(MissingSoftId));

    // 다시 검색하면 파일 변화가 반영된다(Villain을 지우면 사라지고 Derived 참조자는 Hero뿐).
    String villainPath(env.allocator);
    const StringView villainPieces[] = {project.Root(), "/Content/Characters/Villain.rasset"};
    for (StringView piece : villainPieces) {
        REQUIRE(villainPath.TryAppend(piece));
    }

    REQUIRE(platform::PhysicalFileSystem().Delete(villainPath.View().Data(),
                                                  villainPath.View().ByteCount()));
    REQUIRE(index.Rebuild(platform::PhysicalFileSystem(), PackageReadOptions{}));
    CHECK(index.Count() == 3 + nfcExtra);
    CHECK(index.FindById(FixedId(VillainId)).Error() == ErrorCode::NotFound);
}

// Verify의 형식 대조(7.1절 13): Hard 의존의 expectedType과 대상 대표 타입이 다르면 TypeMismatch다.
// 대상이 기대의 파생이면 TypeRegistry가 있을 때만 통과하고(없거나 대상 타입이 등록되지 않았으면
// TypeMismatch), 파생이 아니면 언제나 TypeMismatch다. 대상 파일이 없으면 MissingDependency, 대상
// id가 둘이면(DuplicateAssetId로 이미 진단) 대조하지 않는다. 종료한 TypeRegistry는 그 오류다
// (ShuttingDown — Verify·ListByType).
TEST(AssetIndex_VerifyTypes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    IndexPackages packages(env.allocator);
    REQUIRE(MakePackages(env, packages));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(MemoryRoot));
    TypeRegistry empty(env.allocator);
    TypeRegistry closed(env.allocator);
    REQUIRE(closed.Shutdown());

    struct Scene {
        Span<const uint8> base;
        Span<const uint8> derived;
        bool duplicateBase;
        // 대상 대표 타입이 expectedType과 다른 장면이다(파생 판정이 Registry를 부른다).
        bool typeDiffers;
        // 기대: Hero의 Hard(→ Base), Villain의 Hard(→ Derived)의 진단 코드(TypeRegistry 없음 —
        // 형식 대조 안 함·있음·빈 Registry).
        AssetDiagnosticCode heroCodes[3];
        AssetDiagnosticCode villainCodes[3];
    };
    using C = AssetDiagnosticCode;
    const Scene scenes[] = {
        {packages.base.View(),
         packages.derived.View(),
         false,
         false,
         {C::None, C::None, C::None},
         {C::None, C::None, C::None}},
        {packages.baseAsDerived.View(),
         packages.derived.View(),
         false,
         true,
         {C::None, C::None, C::TypeMismatch},
         {C::None, C::None, C::None}},
        {packages.base.View(),
         packages.derivedAsBase.View(),
         false,
         true,
         {C::None, C::None, C::None},
         {C::None, C::TypeMismatch, C::TypeMismatch}},
        {Span<const uint8>(),
         packages.derived.View(),
         false,
         false,
         {C::MissingDependency, C::MissingDependency, C::MissingDependency},
         {C::None, C::None, C::None}},
        {packages.baseAsDerived.View(),
         packages.derived.View(),
         true,
         false,
         {C::None, C::None, C::None},
         {C::None, C::None, C::None}},
    };
    for (const Scene& scene : scenes) {
        MemoryFileSystem files(env.allocator, layout.ContentDirectory());
        REQUIRE(files.Ready());
        REQUIRE(files.AddFile("Hero.rasset", packages.hero.View()));
        REQUIRE(files.AddFile("Villain.rasset", packages.villain.View()));
        REQUIRE(files.AddFile("Derived.rasset", scene.derived));
        if (!scene.base.IsEmpty()) {
            REQUIRE(files.AddFile("Base.rasset", scene.base));
        }

        if (scene.duplicateBase) {
            REQUIRE(files.AddFile("Base2.rasset", packages.base.View()));
        }

        AssetIndex index(env.allocator, layout);
        REQUIRE(index.Rebuild(files, PackageReadOptions{}));
        TypeRegistry* const registries[] = {nullptr, &env.types, &empty};
        for (usize which = 0; which < 3; ++which) {
            Array<AssetIndexDiagnostic> out(env.allocator);
            REQUIRE(index.Verify(registries[which], out));
            CHECK(TakeOrphans(out) == 1);
            C hero = C::None;
            C villain = C::None;
            for (const AssetIndexDiagnostic& diagnostic : out) {
                if (diagnostic.relatedAsset == FixedId(MissingSoftId)) {
                    continue; // Hero의 Soft 누락은 모든 장면에 있다.
                }

                const bool fromHero = diagnostic.asset == FixedId(HeroId);
                CHECK(diagnostic.relatedAsset == FixedId(fromHero ? BaseId : DerivedId));
                CHECK(diagnostic.error == (diagnostic.code == C::MissingDependency
                                               ? ErrorCode::NotFound
                                               : ErrorCode::TypeMismatch));
                (fromHero ? hero : villain) = diagnostic.code;
            }

            CHECK(hero == scene.heroCodes[which]);
            CHECK(villain == scene.villainCodes[which]);
        }

        // 종료한 Registry: 파생 판정이 필요한 장면만 ShuttingDown이다.
        Array<AssetIndexDiagnostic> out(env.allocator);
        auto verified = index.Verify(&closed, out);
        CHECK(scene.typeDiffers ? verified.Error() == ErrorCode::ShuttingDown
                                : static_cast<bool>(verified));
        Array<const AssetIndexEntry*> listed(env.allocator);
        CHECK(index.ListByType(IdOf<BaseGameAsset>(), &closed, listed).Error() ==
              ErrorCode::ShuttingDown);
        // 빈 Registry(미등록 — 정확 일치만).
        listed.Clear();
        REQUIRE(index.ListByType(IdOf<BaseGameAsset>(), &empty, listed));
        for (const AssetIndexEntry* entry : listed) {
            CHECK(entry->type == IdOf<BaseGameAsset>());
        }
    }

    // 대표 타입 id가 등록된 구조체(클래스 아님)인 패키지(Summary·Exports의 타입 id를 바꿔 만든다 —
    // 타입 Registry 없이 읽는 색인은 받는다): TypeRegistry 판정의 TypeMismatch(클래스 아님)는 파생
    // 아님이다. ListByType은 빼고, 그것을 Hard로 기대하는 Hero는 TypeMismatch 진단이다.
    EditablePackage edit(env.allocator);
    REQUIRE(edit.Parse(packages.base.View()));
    REQUIRE(ReplaceGuid(edit, IdOf<BaseGameAsset>(), IdOf<FixturePoint>()) > 0);
    const Array<uint8> structTyped = edit.Build();
    MemoryFileSystem files(env.allocator, layout.ContentDirectory());
    REQUIRE(files.Ready());
    REQUIRE(files.AddFile("Hero.rasset", packages.hero.View()));
    REQUIRE(files.AddFile("Derived.rasset", packages.derived.View()));
    REQUIRE(files.AddFile("Base.rasset", structTyped.View()));
    AssetIndex index(env.allocator, layout);
    REQUIRE(index.Rebuild(files, PackageReadOptions{}));
    CHECK(index.Diagnostics().IsEmpty());
    auto base = index.FindById(FixedId(BaseId));
    REQUIRE(base);
    CHECK(base.Value()->type == IdOf<FixturePoint>());
    Array<const AssetIndexEntry*> listed(env.allocator);
    REQUIRE(index.ListByType(IdOf<BaseGameAsset>(), &env.types, listed));
    const uint64 derivedOnly[] = {DerivedId};
    CHECK(SameIds(listed, derivedOnly));
    Array<AssetIndexDiagnostic> verified(env.allocator);
    REQUIRE(index.Verify(&env.types, verified));
    uint32 mismatches = 0;
    for (const AssetIndexDiagnostic& diagnostic : verified) {
        mismatches += diagnostic.code == AssetDiagnosticCode::TypeMismatch &&
                              diagnostic.relatedAsset == FixedId(BaseId)
                          ? 1
                          : 0;
    }

    CHECK(mismatches == 1);
}

// 진단 장면(7.1절 13): 이름 규칙 위반 InvalidPath(사유), 패키지 판정 실패는 부분 읽기와 같은 코드·
// 오류·칸·위치, 쿠킹 패키지 CorruptPackage, 다른 스키마 Tags UnsupportedVersion(태그 없이 색인),
// 깨진 Tags CorruptPackage(Tags 칸, 색인), 열거 뒤 사라진 파일 MissingFile, 그 밖의 열기·읽기 실패
// 진단 None과 그 오류. 링크(파일·디렉터리 — 따라가지 않는다)·장치·`.rasset` 아닌 파일은 무시한다.
// 같은 경로 키(대소문자만 다름)는 둘 다 DuplicatePath, 같은 id는 둘 다 DuplicateAssetId(상대 파일을
// related에), 조회는 AlreadyExists다. 진단은 파일 이름 → 코드 순이다.
TEST(AssetIndex_Diagnostics) {
    AssetEnv env;
    REQUIRE(env.Ready());
    IndexPackages packages(env.allocator);
    REQUIRE(MakePackages(env, packages));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(MemoryRoot));
    MemoryFileSystem files(env.allocator, layout.ContentDirectory());
    REQUIRE(files.Ready());
    const uint8 junk[] = {'J', 'U', 'N', 'K', 0, 1, 2, 3};
    REQUIRE(files.AddDirectory("Good"));
    REQUIRE(files.AddFile("Good/Hero.rasset", packages.hero.View()));
    REQUIRE(files.AddFile("Good/hero.RASSET", packages.villain.View()));
    REQUIRE(files.AddDirectory("Copy"));
    REQUIRE(files.AddFile("Copy/HeroCopy.rasset", packages.hero.View()));
    REQUIRE(files.AddFile("Bad#Name.rasset", packages.plain.View()));
    REQUIRE(files.AddFile("Broken.rasset", junk));
    REQUIRE(files.AddFile("Cooked.rasset", packages.cooked.View()));
    REQUIRE(files.AddFile("OtherTags.rasset", packages.otherTags.View()));
    REQUIRE(files.AddFile("BadTags.rasset", packages.badTags.View()));
    REQUIRE(files.AddFile("Vanished.rasset", packages.base.View(), MemoryFault::OpenMissing));
    REQUIRE(files.AddFile("Locked.rasset", packages.base.View(), MemoryFault::OpenBusy));
    REQUIRE(files.AddFile("Unreadable.rasset", packages.base.View(), MemoryFault::Read));
    REQUIRE(files.AddFile("Device.rasset", packages.base.View(), MemoryFault::None,
                          platform::FileType::Other));
    REQUIRE(files.AddFile("Link.rasset", packages.base.View(), MemoryFault::None,
                          platform::FileType::File, true));
    REQUIRE(files.AddDirectory("LinkDir", true));
    REQUIRE(files.AddFile("LinkDir/Inside.rasset", packages.derived.View()));
    REQUIRE(files.AddFile("Notes.txt", packages.base.View()));
    REQUIRE(files.AddFile("Upper.RASSET", packages.base.View()));
    REQUIRE(files.AddFile("Aaa.rasset", packages.plain.View()));
    AssetIndex index(env.allocator, layout);
    REQUIRE(index.Rebuild(files, PackageReadOptions{}));

    ErrorCode junkError = ErrorCode::None;
    ErrorCode cookedError = ErrorCode::None;
    const AssetDiagnostic junkFailure = DirectFailure(env.allocator, junk, junkError);
    const AssetDiagnostic cookedFailure =
        DirectFailure(env.allocator, packages.cooked.View(), cookedError);
    CHECK(cookedFailure.code == AssetDiagnosticCode::CorruptPackage);
    const ErrorCode systemError = FileErrorCode(MemoryError);
    const ErrorCode busyError = FileErrorCode(MemoryBusy);
    using C = AssetDiagnosticCode;
    const ExpectedDiagnostic expected[] = {
        {C::InvalidPath, ErrorCode::InvalidArgument, "Bad#Name.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "BadTags.rasset"},
        {junkFailure.code, junkError, "Broken.rasset"},
        {C::CorruptPackage, cookedError, "Cooked.rasset"},
        {C::DuplicateAssetId, ErrorCode::AlreadyExists, "Copy/HeroCopy.rasset"},
        {C::DuplicateAssetId, ErrorCode::AlreadyExists, "Good/Hero.rasset"},
        {C::DuplicatePath, ErrorCode::AlreadyExists, "Good/Hero.rasset"},
        {C::DuplicatePath, ErrorCode::AlreadyExists, "Good/hero.RASSET"},
        {C::None, busyError, "Locked.rasset"},
        {C::UnsupportedVersion, ErrorCode::Unsupported, "OtherTags.rasset"},
        {C::None, systemError, "Unreadable.rasset"},
        {C::MissingFile, ErrorCode::NotFound, "Vanished.rasset"},
    };
    CHECK(SameDiagnostics(index.Diagnostics(), expected));
    REQUIRE(index.Diagnostics().Size() == 12);
    const Span<const AssetIndexDiagnostic> diagnostics = index.Diagnostics();
    CHECK(diagnostics[0].pathError == LogicalPathError::InvalidComponent);
    CHECK(diagnostics[1].asset == FixedId(BadTagsId));
    CHECK(diagnostics[1].section == static_cast<uint32>(PackageSectionKind::Tags));
    CHECK(diagnostics[2].section == junkFailure.section &&
          diagnostics[2].detail == junkFailure.detail);
    CHECK(diagnostics[3].section == cookedFailure.section &&
          diagnostics[3].detail == cookedFailure.detail &&
          diagnostics[3].asset == cookedFailure.asset);
    CHECK(diagnostics[4].related.View() == "Good/Hero.rasset");
    CHECK(diagnostics[4].asset == FixedId(HeroId));
    CHECK(diagnostics[5].related.View() == "Copy/HeroCopy.rasset");
    CHECK(diagnostics[6].related.View() == "Good/hero.RASSET");
    CHECK(diagnostics[7].related.View() == "Good/Hero.rasset");
    CHECK(diagnostics[7].asset == FixedId(VillainId));
    CHECK(diagnostics[9].asset == FixedId(OtherTagsId));
    CHECK(diagnostics[9].section == static_cast<uint32>(PackageSectionKind::Tags));

    // 색인된 항목: BadTags·Copy/HeroCopy·Good/Hero·Good/hero·OtherTags·Upper(경로 키 순).
    REQUIRE(index.Count() == 7);
    const StringView names[] = {"Aaa.rasset",       "BadTags.rasset",   "Copy/HeroCopy.rasset",
                                "Good/Hero.rasset", "Good/hero.RASSET", "OtherTags.rasset",
                                "Upper.RASSET"};
    for (usize position = 0; position < index.Count(); ++position) {
        CHECK(index.At(position).fileName.View() == names[position]);
    }

    CHECK(index.At(1).tags.IsEmpty() && index.At(5).tags.IsEmpty());
    // 모르는 커스텀 버전으로 저장한 OtherTags만 읽기 전용이다.
    for (usize position = 0; position < index.Count(); ++position) {
        CHECK(index.At(position).readOnly == (position == 5));
    }

    CHECK(index.At(2).duplicateId && !index.At(2).duplicatePath);
    CHECK(index.At(3).duplicateId && index.At(3).duplicatePath);
    CHECK(!index.At(4).duplicateId && index.At(4).duplicatePath);
    CHECK(index.At(6).path.View() == "/Game/Upper");
    auto plain = index.FindById(AssetId{Guid{PlainHigh, PlainId}});
    CHECK(plain && plain.Value() == &index.At(0));
    CHECK(index.FindById(AssetId{Guid{PlainHigh, HeroId}}).Error() == ErrorCode::NotFound);
    CHECK(index.FindById(FixedId(HeroId)).Error() == ErrorCode::AlreadyExists);
    auto found = index.FindById(FixedId(VillainId));
    CHECK(found && found.Value() == &index.At(4));
    auto goodHero = ParseLogicalPath(env.allocator, "/Game/Good/Hero");
    auto upper = ParseLogicalPath(env.allocator, "/Game/Upper");
    REQUIRE(goodHero && upper);
    CHECK(index.FindByPath(goodHero.Value()).Error() == ErrorCode::AlreadyExists);
    auto upperEntry = index.FindByPath(upper.Value());
    CHECK(upperEntry && upperEntry.Value() == &index.At(6));
    Array<const PackageDependencyEntry*> dependencies(env.allocator);
    CHECK(index.Dependencies(FixedId(HeroId), AllDependencyKinds, dependencies).Error() ==
          ErrorCode::AlreadyExists);
    // Verify: Hero 둘의 Hard 대상 Base는 Upper로 있고 Soft 대상 2109는 둘 다 누락이다. Villain
    // (Good/hero)의 Hard 대상 Derived는 색인에 없고, Soft 대상 Hero는 id가 둘이라 대조하지 않는다.
    Array<AssetIndexDiagnostic> verified(env.allocator);
    REQUIRE(index.Verify(nullptr, verified));
    // id를 모르는 진단이 있어 Hero 둘의 owner 없음을 확정하지 않는다(Undetermined).
    CHECK(TakeOrphans(verified) == 0);
    uint32 missingDerived = 0;
    uint32 missingSoft = 0;
    for (const AssetIndexDiagnostic& diagnostic : verified) {
        CHECK(diagnostic.code == AssetDiagnosticCode::MissingDependency);
        missingDerived += diagnostic.relatedAsset == FixedId(DerivedId) ? 1 : 0;
        missingSoft += diagnostic.relatedAsset == FixedId(MissingSoftId) ? 1 : 0;
    }

    CHECK(missingDerived == 1 && missingSoft == 2 && verified.Size() == 3);
}

namespace {

constexpr uint64 RecordOwnerId = 2120;
constexpr uint64 RecordFirstId = 2121;
constexpr Guid RecordImporter{0x494d50, 7};

// ImportRecord 장면 패키지 하나다: 이름, ImportRecord 모양, ImportOwner 의존 대상(0은 없음),
// 깨진 Tags 여부.
enum class RecordShape {
    None,
    Owner,
    Reference,
    OtherSchema,
    Newer,
    Corrupt
};

struct RecordPackage {
    StringView name;
    RecordShape shape;
    uint64 importOwner;
    bool badTags;
};

// 소유 기록 장면은 모두 RecordOwnerId 하나를 owner로 쓴다(참조 기록의 owner도 같다).
const RecordPackage RecordPackages[] = {
    {"A-Owner.rasset", RecordShape::Owner, 0, false},
    {"B-Reference.rasset", RecordShape::Reference, RecordOwnerId, false},
    {"C-OtherSchema.rasset", RecordShape::OtherSchema, 0, false},
    {"D-Newer.rasset", RecordShape::Newer, 0, false},
    {"E-Corrupt.rasset", RecordShape::Corrupt, 0, false},
    {"F-DependencyOnly.rasset", RecordShape::None, RecordOwnerId, false},
    {"G-ReferenceNoDependency.rasset", RecordShape::Reference, 0, false},
    {"H-ReferenceWrongOwner.rasset", RecordShape::Reference, RecordFirstId, false},
    {"I-OwnedWithDependency.rasset", RecordShape::Owner, RecordFirstId, false},
    {"J-OtherSchemaWithDependency.rasset", RecordShape::OtherSchema, RecordOwnerId, false},
    {"K-CorruptAndBadTags.rasset", RecordShape::Corrupt, RecordOwnerId, true},
    {"L-DependencyAndBadTags.rasset", RecordShape::None, RecordOwnerId, true},
};

// 장면 index의 패키지 id다(A-Owner만 RecordOwnerId).
AssetId RecordPackageId(usize index) noexcept {
    return FixedId(index == 0 ? RecordOwnerId : RecordFirstId + index);
}

// 장면 패키지 byte를 만든다(TestAsset 대표 하나).
Array<uint8> MakeRecordPackage(AssetEnv& env, usize index) noexcept {
    const RecordPackage& scene = RecordPackages[index];
    const AssetId id = RecordPackageId(index);
    ObjectRegistry& registry = *env.source;
    RootRef<Package> package;
    if (MakeRepresentative<TestAsset>(registry, id, PackageKind::Editor, package).IsNull()) {
        return Array<uint8>(env.allocator);
    }

    Array<uint8> record(env.allocator);
    const ImportSourceInput sources[] = {{"source", "Raw", "Rock.png", Sha256Digest{}, 3}};
    const ImportOutputInput outputs[] = {{"", id, IdOf<TestAsset>()},
                                         {"extra", FixedId(RecordFirstId + 90), IdOf<TestAsset>()}};
    ImportRecordInput input;
    if (scene.shape == RecordShape::Owner) {
        input.owner = id;
        input.importerId = RecordImporter;
        input.importerName = "RockImporter";
        input.importerVersion = 4;
        input.sources = sources;
        input.outputs = outputs;
    } else if (scene.shape == RecordShape::Reference) {
        input.owner = FixedId(RecordOwnerId);
    }

    if (scene.shape == RecordShape::Owner || scene.shape == RecordShape::Reference) {
        auto written = WriteImportRecord(env.allocator, id, input);
        if (!written) {
            return Array<uint8>(env.allocator);
        }

        record = Move(written.Value());
    } else if (scene.shape == RecordShape::OtherSchema) {
        record = OneFieldRecord(env.allocator, Guid{0x7A, 0x6C});
    } else if (scene.shape == RecordShape::Newer || scene.shape == RecordShape::Corrupt) {
        // 더 새 버전(필드는 상관없다)과 버전 1인데 모르는 필드 하나뿐인 기록.
        const uint8 value[4] = {1, 2, 3, 4};
        RecordBuilder builder(env.allocator, ImportRecordSchema,
                              scene.shape == RecordShape::Newer ? ImportRecordVersion + 1
                                                                : ImportRecordVersion);
        auto built = builder.AddField(Guid{0, 9}, WireType::U32, value)
                         ? builder.Build()
                         : Result<Array<uint8>>::Failure(ErrorCode::OutOfMemory);
        if (!built) {
            return Array<uint8>(env.allocator);
        }

        record = Move(built.Value());
    }

    const Array<uint8> badTags = OneFieldRecord(env.allocator, PackageTagsSchema);
    const PackageDependencyEntry owner[] = {
        {FixedId(scene.importOwner), PackageDependencyKind::ImportOwner, TypeId{}, false}};
    PackageWriteOptions options = EditorOptions();
    options.importRecord = record.View();
    if (scene.importOwner != 0) {
        options.extraDependencies = owner;
    }

    if (scene.badTags) {
        options.tags = badTags.View();
    }

    return Write(env.allocator, registry, package.Handle(), options);
}

} // namespace

// ImportRecord 색인(7.1절 15 — A2c2a): 소유 기록은 요약(임포터 id·이름·버전·출력 수, owner = 자기),
// 참조 기록 + 맞는 ImportOwner는 요약(owner만)이고 진단이 없다. 다른 스키마·더 새 버전은
// UnsupportedVersion, 형식 위반은 CorruptPackage(둘 다 칸 ImportRecord — 요약 없음, ImportOwner
// 대조 안 함). 대조 위반(기록 없이 ImportOwner, 참조 기록에 ImportOwner 없음·다른 대상, 소유
// 기록에 ImportOwner)은 CorruptPackage(칸 Dependencies). 어느 경우에도 항목은 색인된다. 한 파일의
// 같은 코드 진단 둘(깨진 Tags + 손상 기록, 깨진 Tags + 대조 위반)은 칸 순이다. 모든 할당 실패는
// OutOfMemory다.
TEST(AssetIndex_ImportRecords) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(MemoryRoot));
    MemoryFileSystem files(env.allocator, layout.ContentDirectory());
    REQUIRE(files.Ready());
    // 메모리 파일 시스템은 byte를 빌리므로 시험 동안 살려 둔다.
    constexpr usize count = sizeof(RecordPackages) / sizeof(RecordPackages[0]);
    Array<Array<uint8>> bytes(env.allocator);
    for (usize index = 0; index < count; ++index) {
        REQUIRE(bytes.TryPushBack(MakeRecordPackage(env, index)));
        REQUIRE(!bytes[index].IsEmpty());
        REQUIRE(files.AddFile(RecordPackages[index].name, bytes[index].View()));
    }

    AssetIndex index(env.allocator, layout);
    REQUIRE(index.Rebuild(files, PackageReadOptions{}));
    using C = AssetDiagnosticCode;
    const ExpectedDiagnostic expected[] = {
        {C::UnsupportedVersion, ErrorCode::Unsupported, "C-OtherSchema.rasset"},
        {C::UnsupportedVersion, ErrorCode::Unsupported, "D-Newer.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "E-Corrupt.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "F-DependencyOnly.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "G-ReferenceNoDependency.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "H-ReferenceWrongOwner.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "I-OwnedWithDependency.rasset"},
        {C::UnsupportedVersion, ErrorCode::Unsupported, "J-OtherSchemaWithDependency.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "K-CorruptAndBadTags.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "K-CorruptAndBadTags.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "L-DependencyAndBadTags.rasset"},
        {C::CorruptPackage, ErrorCode::CorruptData, "L-DependencyAndBadTags.rasset"},
    };
    CHECK(SameDiagnostics(index.Diagnostics(), expected));
    REQUIRE(index.Diagnostics().Size() == 12);
    constexpr uint32 Record = static_cast<uint32>(PackageSectionKind::ImportRecord);
    constexpr uint32 Dependencies = static_cast<uint32>(PackageSectionKind::Dependencies);
    constexpr uint32 Tags = static_cast<uint32>(PackageSectionKind::Tags);
    const uint32 sections[] = {Record,       Record,       Record,       Dependencies,
                               Dependencies, Dependencies, Dependencies, Record,
                               Record,       Tags,         Dependencies, Tags};
    const usize owners[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 11, 11};
    for (usize position = 0; position < index.Diagnostics().Size(); ++position) {
        const AssetIndexDiagnostic& diagnostic = index.Diagnostics()[position];
        CHECK(diagnostic.section == sections[position]);
        CHECK(diagnostic.asset == RecordPackageId(owners[position]));
    }

    // 모든 파일이 색인된다(경로 키 순 = 장면 순). 요약은 A(소유)·B·G·H(참조)·I(소유)만 있다.
    REQUIRE(index.Count() == count);
    for (usize position = 0; position < count; ++position) {
        const AssetIndexEntry& entry = index.At(position);
        CHECK(entry.fileName.View() == RecordPackages[position].name);
        const RecordShape shape = RecordPackages[position].shape;
        const bool present = shape == RecordShape::Owner || shape == RecordShape::Reference;
        CHECK(entry.importRecord.present == present);
        if (!present) {
            CHECK(entry.importRecord.owner.IsNull() && entry.importRecord.outputs.Size() == 0);
            continue;
        }

        if (shape == RecordShape::Owner) {
            CHECK(entry.importRecord.owner == entry.id);
            CHECK(entry.importRecord.importerId == RecordImporter);
            CHECK(entry.importRecord.importerName.View() == "RockImporter");
            CHECK(entry.importRecord.importerVersion == 4);
            CHECK(entry.importRecord.outputs.Size() == 2);
        } else {
            CHECK(entry.importRecord.owner == FixedId(RecordOwnerId));
            CHECK(entry.importRecord.importerName.IsEmpty());
            CHECK(entry.importRecord.outputs.Size() == 0);
        }
    }

    // 할당 실패: 시도마다 OutOfMemory이고 색인은 비며, 끝나면 같은 결과다.
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndex failingIndex(failing, layout);
        failing.FailOnAttempt(attempt);
        const Result<void> result = failingIndex.Rebuild(files, PackageReadOptions{});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(result);
            CHECK(failingIndex.Count() == count && failingIndex.Diagnostics().Size() == 12);
            break;
        }

        CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
        CHECK(failingIndex.Count() == 0 && failingIndex.Diagnostics().IsEmpty());
        ++failures;
    }

    CHECK(failures > 0);
}

// 검색 실패(7.1절 13·14): Content 열거 실패와 Content가 파일인 것(WrongType)은 Rebuild의 그 파일
// 오류이고 색인을 비운다(앞 결과를 남기지 않는다). 하위 디렉터리 열거 실패는 진단 None과 원본
// 오류를 남기고 그 디렉터리만 뺀다. 없는 디렉터리(Content 없음·열거에서 사라진 하위 디렉터리)는
// 빈 디렉터리다. 확장자뿐인 이름(".rasset"·".RASSET" — Content 바로 아래와 하위 디렉터리)은 검색을
// 멈추지 않고 InvalidPath(EmptyComponent)다.
TEST(AssetIndex_RebuildFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    IndexPackages packages(env.allocator);
    REQUIRE(MakePackages(env, packages));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(MemoryRoot));
    const ErrorCode systemError = FileErrorCode(MemoryError);
    const ErrorCode wrongType = FileErrorCode(MemoryWrongType);
    enum class Fault {
        ContentEnumerate,
        ContentIsFile,
        SubEnumerate,
        ContentMissing,
        SubMissing
    };
    const Fault faults[] = {Fault::ContentEnumerate, Fault::ContentIsFile, Fault::SubEnumerate,
                            Fault::ContentMissing, Fault::SubMissing};
    for (Fault fault : faults) {
        MemoryFileSystem files(env.allocator, layout.ContentDirectory());
        REQUIRE(files.Ready());
        REQUIRE(files.AddFile("Hero.rasset", packages.hero.View()));
        REQUIRE(files.AddFile("Broken.rasset", packages.hero.View().Slice(0, 10)));
        REQUIRE(files.AddDirectory("Sub"));
        REQUIRE(files.AddFile("Sub/Base.rasset", packages.base.View()));
        AssetIndex index(env.allocator, layout);
        REQUIRE(index.Rebuild(files, PackageReadOptions{}));
        CHECK(index.Count() == 2 && index.Diagnostics().Size() == 1);
        const bool content = fault == Fault::ContentEnumerate || fault == Fault::ContentIsFile ||
                             fault == Fault::ContentMissing;
        files.Remove(content ? "" : "Sub");
        if (fault == Fault::ContentIsFile) {
            REQUIRE(files.AddFile("", packages.hero.View()));
        } else if (fault != Fault::ContentMissing) {
            const MemoryFault memory =
                fault == Fault::SubMissing ? MemoryFault::EnumerateMissing : MemoryFault::Enumerate;
            REQUIRE(files.AddDirectory(content ? "" : "Sub", false, memory));
        }

        auto rebuilt = index.Rebuild(files, PackageReadOptions{});
        if (fault == Fault::ContentMissing || fault == Fault::SubMissing) {
            // Content가 없으면 빈 색인, 사라진 Sub는 건너뛴다(Hero만 남는다).
            REQUIRE(rebuilt);
            CHECK(index.Count() == (fault == Fault::SubMissing ? 1u : 0u));
            continue;
        }

        if (fault == Fault::SubEnumerate) {
            // 하위 디렉터리 실패는 진단 None과 원본 오류를 남기고 그 디렉터리만 뺀다.
            REQUIRE(rebuilt);
            CHECK(index.Count() == 1 && index.Diagnostics().Size() == 2);
            const AssetIndexDiagnostic& sub = index.Diagnostics()[1];
            CHECK(sub.code == AssetDiagnosticCode::None && sub.error == systemError);
            CHECK(sub.fileName.View() == "Sub");
            continue;
        }

        CHECK(rebuilt.Error() == (fault == Fault::ContentIsFile ? wrongType : systemError));
        CHECK(index.Count() == 0 && index.Diagnostics().IsEmpty());
        CHECK(index.FindById(FixedId(HeroId)).Error() == ErrorCode::NotFound);
    }

    MemoryFileSystem files(env.allocator, layout.ContentDirectory());
    REQUIRE(files.Ready());
    REQUIRE(files.AddFile("Hero.rasset", packages.hero.View()));
    REQUIRE(files.AddFile(".rasset", packages.base.View()));
    REQUIRE(files.AddFile(".RASSET", packages.base.View()));
    REQUIRE(files.AddDirectory("Dir"));
    REQUIRE(files.AddFile("Dir/.rasset", packages.base.View()));
    // 잘못된 UTF-8 이름(POSIX 열거는 byte 그대로 준다): 패키지 파일·디렉터리는 InvalidPath
    // (InvalidComponent — 표시 이름은 0x80 이상 byte를 16진으로), 그 밖의 파일은 무시하고 그
    // 디렉터리 안으로는 내려가지 않는다.
    REQUIRE(files.AddFile(StringView("Bad\xff.rasset", 11), packages.base.View()));
    // 표시 이름은 겹치지 않는다: 글자 그대로 "\\xff"인 이름은 역슬래시를 16진으로(Backslash),
    // 개행이 든 이름은 개행을 16진으로(InvalidComponent) 적는다.
    REQUIRE(files.AddFile("Bad\\xff.rasset", packages.base.View()));
    REQUIRE(files.AddFile("Ctl\n.rasset", packages.base.View()));
    REQUIRE(files.AddFile("Del\x7f.rasset", packages.base.View()));
    REQUIRE(files.AddFile(StringView("Dir/Sub\xfe.rasset", 15), packages.base.View()));
    REQUIRE(files.AddFile(StringView("Bad\xff.txt", 8), packages.base.View()));
    REQUIRE(files.AddDirectory(StringView("Dir\xc3", 4)));
    REQUIRE(files.AddFile(StringView("Dir\xc3/Inside.rasset", 18), packages.derived.View()));
    AssetIndex index(env.allocator, layout);
    REQUIRE(index.Rebuild(files, PackageReadOptions{}));
    CHECK(index.Count() == 1);
    struct NameCase {
        StringView name;
        LogicalPathError error;
    };
    const NameCase names[] = {{".RASSET", LogicalPathError::EmptyComponent},
                              {".rasset", LogicalPathError::EmptyComponent},
                              {"Bad\\x5cxff.rasset", LogicalPathError::Backslash},
                              {"Bad\\xff.rasset", LogicalPathError::InvalidComponent},
                              {"Ctl\\x0a.rasset", LogicalPathError::InvalidComponent},
                              {"Del\\x7f.rasset", LogicalPathError::InvalidComponent},
                              {"Dir/.rasset", LogicalPathError::EmptyComponent},
                              {"Dir/Sub\\xfe.rasset", LogicalPathError::InvalidComponent},
                              {"Dir\\xc3", LogicalPathError::InvalidComponent}};
    REQUIRE(index.Diagnostics().Size() == 9);
    for (usize position = 0; position < 9; ++position) {
        const AssetIndexDiagnostic& diagnostic = index.Diagnostics()[position];
        CHECK(diagnostic.code == AssetDiagnosticCode::InvalidPath);
        CHECK(diagnostic.error == ErrorCode::InvalidArgument);
        CHECK(diagnostic.pathError == names[position].error);
        CHECK(diagnostic.fileName.View() == names[position].name);
    }
}

// 진단 줄(13절 명령 출력): 코드마다 `<코드>: Content/<파일>: <설명>` 한 줄이다 — 코드 None은 원본
// 오류 이름, InvalidPath는 사유 이름, 같은 id·경로는 상대 파일, 의존은 대상 id, 패키지 판정은
// 칸·위치·원본 오류. 줄 저장소의 모든 할당 실패는 그 오류다.
TEST(AssetIndex_DiagnosticLines) {
    SystemAllocator allocator;
    struct LineCase {
        AssetDiagnosticCode code;
        ErrorCode error;
        StringView expected;
    };
    using C = AssetDiagnosticCode;
    const LineCase cases[] = {
        {C::None, ErrorCode::Busy,
         "Busy: Content/Dir/A.rasset: cannot read the file or directory\n"},
        {C::InvalidPath, ErrorCode::InvalidArgument,
         "InvalidPath: Content/Dir/A.rasset: invalid logical path (TooLong)\n"},
        {C::MissingFile, ErrorCode::NotFound,
         "MissingFile: Content/Dir/A.rasset: the file disappeared during the scan\n"},
        {C::DuplicateAssetId, ErrorCode::AlreadyExists,
         "DuplicateAssetId: Content/Dir/A.rasset: asset id 5e570000-a55e-7000-0000-000000000834 is "
         "also in 'Content/Other.rasset'\n"},
        {C::DuplicatePath, ErrorCode::AlreadyExists,
         "DuplicatePath: Content/Dir/A.rasset: the logical path is also used by "
         "'Content/Other.rasset'\n"},
        {C::MissingDependency, ErrorCode::NotFound,
         "MissingDependency: Content/Dir/A.rasset: dependency 5e570000-a55e-7000-0000-000000000835 "
         "is "
         "not in the index\n"},
        {C::TypeMismatch, ErrorCode::TypeMismatch,
         "TypeMismatch: Content/Dir/A.rasset: dependency 5e570000-a55e-7000-0000-000000000835 has "
         "a "
         "different representative type\n"},
        {C::CorruptPackage, ErrorCode::CorruptData,
         "CorruptPackage: Content/Dir/A.rasset: package check failed (section 3, detail 40, "
         "CorruptData)\n"},
    };
    test::NFailAllocator failing(allocator);
    // 줄 앞에 0 … LinePrefixBytes byte를 미리 채워 저장소가 자라는 지점을 줄의 모든 조각(코드·
    // 이름·escape·설명·상대 이름) 위로 옮기며 모든 할당 지점을 실패시킨다.
    auto sweep = [&](const AssetIndexDiagnostic& diagnostic, usize expectedBytes) noexcept {
        uint32 failures = 0;
        for (usize prefix = 0; prefix <= LinePrefixBytes; ++prefix) {
            for (uint64 attempt = 1;; ++attempt) {
                StringBuilder<LineInlineBytes> small(failing);
                for (usize filled = 0; filled < prefix; ++filled) {
                    (void)small.TryAppend("-");
                }

                // 채우기의 할당은 세지 않는다(FailOnAttempt가 시도 수를 0으로 되돌린다).
                failing.FailOnAttempt(attempt);
                auto appended = AppendAssetIndexDiagnosticLine(small, diagnostic);
                const bool failed = failing.Attempts() >= attempt;
                failing.DisableFailure();
                if (!failed) {
                    CHECK(appended);
                    CHECK(small.View().ByteCount() == prefix + expectedBytes);
                    break;
                }

                CHECK(!appended && appended.Error() == ErrorCode::OutOfMemory);
                ++failures;
            }
        }

        CHECK(failures > 0);
    };
    for (const LineCase& one : cases) {
        AssetIndexDiagnostic diagnostic(allocator);
        diagnostic.code = one.code;
        diagnostic.error = one.error;
        diagnostic.pathError = LogicalPathError::TooLong;
        diagnostic.asset = FixedId(HeroId);
        diagnostic.relatedAsset = FixedId(HeroId + 1);
        diagnostic.section = 3;
        diagnostic.detail = 40;
        REQUIRE(diagnostic.fileName.TryAssign("Dir/A.rasset"));
        REQUIRE(diagnostic.related.TryAssign("Other.rasset"));
        StringBuilder<LineInlineBytes> line(allocator);
        REQUIRE(AppendAssetIndexDiagnosticLine(line, diagnostic));
        if (line.View() != one.expected) {
            printf("  line: %.*s", static_cast<int>(line.View().ByteCount()), line.View().Data());
        }

        CHECK(line.View() == one.expected);
        sweep(diagnostic, one.expected.ByteCount());
    }
}

// 할당 실패(7.1절 13): 진단 장면의 Rebuild와 모든 조회를 이어 부르며 모든 할당 지점에서 실패시키면
// 그 호출이 OutOfMemory이고(Rebuild면 색인이 빈다), 실패가 없으면 끝까지 같은 결과다.
TEST(AssetIndex_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    IndexPackages packages(env.allocator);
    REQUIRE(MakePackages(env, packages));
    test::NFailAllocator failing(env.allocator);
    // 긴 프로젝트 루트(Content 절대 경로만으로 경로 임시 저장소 512 byte를 넘는다 — 디렉터리·파일
    // 경로를 만드는 할당이 생긴다).
    String root(env.allocator);
    REQUIRE(root.TryAppend(MemoryRoot));
    for (uint32 level = 0; level < LongRootLevels; ++level) {
        REQUIRE(root.TryAppend("/Root-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-0123"));
    }

    ProjectLayout layout(failing);
    REQUIRE(layout.Initialize(root.View()));
    REQUIRE(layout.ContentDirectory().ByteCount() > LongRootMinimumBytes);
    MemoryFileSystem files(env.allocator, layout.ContentDirectory());
    REQUIRE(files.Ready());
    REQUIRE(files.AddDirectory("Good"));
    REQUIRE(files.AddFile("Good/Hero.rasset", packages.hero.View()));
    REQUIRE(files.AddFile("Good/hero.rasset", packages.villain.View()));
    REQUIRE(files.AddFile("Copy.rasset", packages.hero.View()));
    REQUIRE(files.AddFile("Bad#Name.rasset", packages.plain.View()));
    REQUIRE(files.AddFile("Broken.rasset", packages.hero.View().Slice(0, 70)));
    REQUIRE(files.AddFile("OtherTags.rasset", packages.otherTags.View()));
    REQUIRE(files.AddFile("BadTags.rasset", packages.badTags.View()));
    REQUIRE(files.AddFile("Base.rasset", packages.base.View()));
    REQUIRE(files.AddFile("A-very-long-file-name-that-does-not-fit-inline-storage.rasset",
                          packages.derived.View()));
    // 형제 디렉터리 여럿(검색 대기 표가 자란다)과 경로 임시 저장소(512 byte)를 넘는 깊은 디렉터리
    // (그 안 파일은 논리 경로 256 byte 초과 InvalidPath).
    const StringView siblings[] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"};
    for (StringView sibling : siblings) {
        REQUIRE(files.AddDirectory(sibling));
    }

    String deep(env.allocator);
    for (uint32 level = 0; level < DeepLevels; ++level) {
        REQUIRE(deep.TryAppend(level == 0 ? "" : "/"));
        REQUIRE(deep.TryAppend("Level-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-0"));
        REQUIRE(files.AddDirectory(deep.View()));
    }

    REQUIRE(deep.TryAppend("/Deep.rasset"));
    REQUIRE(files.AddFile(deep.View(), packages.plain.View()));
    // 잘못된 UTF-8 이름(표시 이름이 인라인 저장소를 넘게 길다).
    const char invalidName[] = "Invalid-\xff-name-long-enough-to-grow-the-display-string.rasset";
    REQUIRE(files.AddFile(StringView(invalidName, sizeof(invalidName) - 1), packages.base.View()));
    // 열거할 수 없는 하위 디렉터리(진단 None).
    REQUIRE(files.AddDirectory("Unlistable", false, MemoryFault::Enumerate));
    // 진단 이름 순 맨 앞으로 옮겨지는 깨진 파일(열거에서는 마지막).
    REQUIRE(files.AddFile("Aaa-broken.rasset", packages.hero.View().Slice(0, 70)));
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndex index(failing, layout);
        failing.FailOnAttempt(attempt);
        // 조회마다 따로 붙일 곳을 둔다(각 조회의 첫 할당이 실패 지점이 되게).
        Array<const AssetIndexEntry*> under(failing);
        Array<const AssetIndexEntry*> typed(failing);
        Array<const AssetIndexEntry*> tagged(failing);
        Array<const AssetIndexEntry*> referencers(failing);
        Array<const PackageDependencyEntry*> dependencies(failing);
        Array<AssetIndexDiagnostic> verified(failing);
        Result<void> result = index.Rebuild(files, PackageReadOptions{});
        const bool rebuilt = static_cast<bool>(result);
        if (result) {
            result = index.ListUnder("/Game/Good", true, under);
        }

        if (result) {
            result = index.ListByType(IdOf<BaseGameAsset>(), &env.types, typed);
        }

        if (result) {
            result = index.FindByTag("Kind", "Hero", tagged);
        }

        if (result) {
            result = index.Dependencies(FixedId(VillainId), AllDependencyKinds, dependencies);
        }

        if (result) {
            result = index.Referencers(FixedId(DerivedId), AllDependencyKinds, referencers);
        }

        if (result) {
            result = index.Verify(&env.types, verified);
        }

        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(result);
            CHECK(index.Count() == 7 && index.Diagnostics().Size() == 12);
            CHECK(index.Diagnostics()[0].fileName.View() == "Aaa-broken.rasset");
            CHECK(under.Size() == 2 && typed.Size() == 2 && tagged.Size() == 2);
            CHECK(referencers.Size() == 3 && dependencies.Size() == 2);
            // id를 모르는 진단(손상 파일·이름 규칙)이 있어 고아를 확정하지 않는다(Undetermined).
            CHECK(TakeOrphans(verified) == 0 && verified.Size() == 2);
            break;
        }

        CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
        if (!rebuilt) {
            CHECK(index.Count() == 0 && index.Diagnostics().IsEmpty());
        }

        ++failures;
    }

    CHECK(failures > 0);
}

} // namespace asset_index_tests_file
