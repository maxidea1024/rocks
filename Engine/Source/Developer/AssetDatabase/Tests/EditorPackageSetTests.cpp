#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "AssetDatabaseTestSupport.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/EditorPackageSet.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <Core/FileError.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <stdio.h>
#include <string.h>

// 에디터 패키지 집합 열기 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 15 — A2c2a): Hard·EditorOnly
// 닫힘(Soft는 따르지 않음)·순환·이미 연 패키지 잇기·경로 열기·지연 bulk 읽기, 의존 패키지 Base의
// 모든 실패 모양(색인에 없음·같은 id 둘·색인 뒤 사라짐·열기·읽기 IO·손상·다른 id·ImportRecord
// 손상·ImportOwner 위반·다른 스키마 기록·모르는 커스텀 버전·대표 타입 다름·더 새 엔진)마다 열기
// 결과와 경고, 같은 모양을 요청한 패키지로 열 때의 실패, 인자·수명, 모든 할당 실패, 실제 디스크.
using namespace rocks;
namespace editor_package_set_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;

namespace {

// 의존 패키지 Base의 모양이다.
enum class BaseShape {
    Normal,
    Absent,
    Duplicate,
    RemovedAfterIndex,
    OpenBusy,
    ReadFault,
    Corrupt,
    OtherId,
    CorruptRecord,
    OwnerViolation,
    OtherSchemaRecord,
    ReadOnly,
    WrongType,
    Newer,
    OwnedRecord,
    ReferenceRecord
};

// Base 모양 장면이다: Hero·Derived는 정상, Base.rasset은 모양대로(색인 뒤에 바꾸는 모양은 Rebuild
// 뒤에 바꾼다). 파일 byte는 이 장면이 살린다.
struct ShapeScene {
    ShapeScene(AssetEnv& env, Sources& sources, BaseShape shape) noexcept
        : project(env.allocator), variant(env.allocator), shape(shape) {
        ready = project.ready && project.files.Ready() && Build(env, sources);
    }

    bool Build(AssetEnv& env, Sources& sources) noexcept {
        MemoryFileSystem& files = project.files;
        if (!files.AddFile("Hero.rasset", sources.hero.View()) ||
            !files.AddFile("Derived.rasset", sources.derived.View())) {
            return false;
        }

        ObjectRegistry& registry = *env.source;
        const ObjectHandle base = sources.packages[0].Handle();
        PackageWriteOptions options = EditorOptions();
        Array<uint8> record(env.allocator);
        const PackageDependencyEntry owner[] = {
            {FixedId(DerivedId), PackageDependencyKind::ImportOwner, TypeId{}, false}};
        const ImportSourceInput recordSources[] = {
            {"source", "Raw", "Base.png", Sha256Digest{}, 1}};
        const ImportOutputInput recordOutputs[] = {{"", FixedId(BaseId), IdOf<BaseGameAsset>()}};
        ImportRecordInput recordInput;
        Span<const uint8> bytes = sources.base.View();
        switch (shape) {
        case BaseShape::Normal:
        case BaseShape::Duplicate:
        case BaseShape::RemovedAfterIndex:
        case BaseShape::OpenBusy:
        case BaseShape::ReadFault:
        case BaseShape::OtherId:
            break;
        case BaseShape::Absent:
            bytes = {};
            break;
        case BaseShape::Corrupt: {
            // Objects 섹션의 첫 byte(graph magic)를 바꾼다(색인은 Objects 내용을 보지 않는다).
            EditablePackage edit(env.allocator);
            if (!edit.Parse(sources.base.View())) {
                return false;
            }

            for (EditableSection& section : edit.sections) {
                if (section.kind == static_cast<uint32>(PackageSectionKind::Objects)) {
                    section.bytes[0] = static_cast<uint8>(section.bytes[0] ^ 0xff);
                }
            }

            variant = edit.Build();
            bytes = variant.View();
            break;
        }
        case BaseShape::CorruptRecord:
            record = OneFieldRecord(env.allocator, ImportRecordSchema);
            options.importRecord = record.View();
            break;
        case BaseShape::OwnerViolation:
            options.extraDependencies = owner;
            break;
        case BaseShape::OtherSchemaRecord:
            record = OneFieldRecord(env.allocator, Guid{0x7A, 0x6D});
            options.importRecord = record.View();
            break;
        case BaseShape::ReadOnly:
            options.customVersions = UnknownCustomVersions;
            break;
        case BaseShape::WrongType: {
            // 같은 id에 TestAsset 대표(Hero가 기대한 BaseGameAsset의 파생이 아니다).
            RootRef<Package> other;
            if (MakeAsset<TestAsset>(*env.target, BaseId, other).IsNull()) {
                return false;
            }

            variant = Write(env.allocator, *env.target, other.Handle(), EditorOptions());
            bytes = variant.View();
            break;
        }
        case BaseShape::Newer:
            options.engineVersion = NewerEngine;
            break;
        case BaseShape::OwnedRecord:
        case BaseShape::ReferenceRecord: {
            // 유효한 소유 기록, 또는 Derived를 가리키는 참조 기록 + 맞는 ImportOwner 의존.
            recordInput.owner = FixedId(shape == BaseShape::OwnedRecord ? BaseId : DerivedId);
            if (shape == BaseShape::OwnedRecord) {
                recordInput.importerId = Guid{0x494d50, 2};
                recordInput.importerName = "BaseImporter";
                recordInput.importerVersion = 1;
                recordInput.sources = recordSources;
                recordInput.outputs = recordOutputs;
            } else {
                options.extraDependencies = owner;
            }

            auto written = WriteImportRecord(env.allocator, FixedId(BaseId), recordInput);
            if (!written) {
                return false;
            }

            record = Move(written.Value());
            options.importRecord = record.View();
            break;
        }
        }

        const bool rewrite = shape == BaseShape::CorruptRecord ||
                             shape == BaseShape::OwnerViolation ||
                             shape == BaseShape::OtherSchemaRecord ||
                             shape == BaseShape::ReadOnly || shape == BaseShape::Newer ||
                             shape == BaseShape::OwnedRecord || shape == BaseShape::ReferenceRecord;
        if (rewrite) {
            variant = Write(env.allocator, registry, base, options);
            bytes = variant.View();
        }

        if (shape != BaseShape::Absent &&
            (bytes.IsEmpty() || !files.AddFile("Base.rasset", bytes))) {
            return false;
        }

        if (shape == BaseShape::Duplicate && !files.AddFile("Copy.rasset", bytes)) {
            return false;
        }

        if (!project.Rebuild()) {
            return false;
        }

        // 색인 뒤에 바꾸는 모양.
        const MemoryFault faults[] = {MemoryFault::None, MemoryFault::OpenBusy, MemoryFault::Read};
        MemoryFault fault = faults[0];
        if (shape == BaseShape::OpenBusy) {
            fault = faults[1];
        } else if (shape == BaseShape::ReadFault) {
            fault = faults[2];
        }

        if (shape == BaseShape::RemovedAfterIndex) {
            files.Remove("Base.rasset");
        } else if (fault != MemoryFault::None || shape == BaseShape::OtherId) {
            files.Remove("Base.rasset");
            const Span<const uint8> replaced =
                shape == BaseShape::OtherId ? sources.leaf.View() : sources.base.View();
            return files.AddFile("Base.rasset", replaced, fault);
        }

        return true;
    }

    Project project;
    Array<uint8> variant;
    BaseShape shape;
    bool ready = false;
};

struct ShapeExpectation {
    BaseShape shape;
    // 의존으로 열 때: Base가 열리는지·읽기 전용인지, 뺀 경고(코드 None이고 오류 None이면 없음)·그
    // 칸, Hero의 Base import가 null인지와 그 미해결 코드.
    bool baseOpen;
    bool readOnly;
    AssetDiagnosticCode excluded;
    ErrorCode excludedError;
    uint32 excludedSection;
    bool unresolved;
    AssetDiagnosticCode unresolvedCode;
};

constexpr uint32 RecordSection = static_cast<uint32>(PackageSectionKind::ImportRecord);
constexpr uint32 DependencySection = static_cast<uint32>(PackageSectionKind::Dependencies);
// 오류를 표에서 고정하지 않는 모양(Prepare가 정하는 손상·버전 오류 — 코드만 본다)이다.
constexpr ErrorCode AnyError = ErrorCode::None;

ShapeExpectation Expect(BaseShape shape) noexcept {
    using C = AssetDiagnosticCode;
    const ErrorCode busy = FileErrorCode(MemoryBusy);
    const ErrorCode readError = FileErrorCode(MemoryError);
    switch (shape) {
    case BaseShape::Normal:
        return {shape, true, false, C::None, ErrorCode::None, 0, false, C::None};
    case BaseShape::Absent:
        return {shape,
                false,
                false,
                C::MissingDependency,
                ErrorCode::NotFound,
                0,
                true,
                C::MissingDependency};
    case BaseShape::Duplicate:
        return {shape,
                false,
                false,
                C::DuplicateAssetId,
                ErrorCode::AlreadyExists,
                0,
                true,
                C::MissingDependency};
    case BaseShape::RemovedAfterIndex:
        return {shape,
                false,
                false,
                C::MissingFile,
                ErrorCode::NotFound,
                0,
                true,
                C::MissingDependency};
    case BaseShape::OpenBusy:
        return {shape, false, false, C::None, busy, 0, true, C::MissingDependency};
    case BaseShape::ReadFault:
        return {shape, false, false, C::None, readError, 0, true, C::MissingDependency};
    case BaseShape::Corrupt:
        return {shape, false, false, C::CorruptPackage, AnyError, 0, true, C::MissingDependency};
    case BaseShape::OtherId:
        return {shape,
                false,
                false,
                C::ExternalModification,
                ErrorCode::StaleHandle,
                0,
                true,
                C::MissingDependency};
    case BaseShape::CorruptRecord:
        return {shape,
                false,
                false,
                C::CorruptPackage,
                ErrorCode::InvalidArgument,
                RecordSection,
                true,
                C::MissingDependency};
    case BaseShape::OwnerViolation:
        return {shape,
                false,
                false,
                C::CorruptPackage,
                ErrorCode::InvalidArgument,
                DependencySection,
                true,
                C::MissingDependency};
    case BaseShape::OtherSchemaRecord:
    case BaseShape::ReadOnly:
        return {shape, true, true, C::None, ErrorCode::None, 0, false, C::None};
    case BaseShape::WrongType:
        return {shape, true, false, C::None, ErrorCode::None, 0, true, C::TypeMismatch};
    case BaseShape::Newer:
        return {shape, false, false, C::NewerVersion, AnyError, 0, true, C::MissingDependency};
    case BaseShape::OwnedRecord:
    case BaseShape::ReferenceRecord:
        return {shape, true, false, C::None, ErrorCode::None, 0, false, C::None};
    }

    return {shape, false, false, C::None, ErrorCode::None, 0, false, C::None};
}

constexpr BaseShape AllShapes[] = {BaseShape::Normal,
                                   BaseShape::Absent,
                                   BaseShape::Duplicate,
                                   BaseShape::RemovedAfterIndex,
                                   BaseShape::OpenBusy,
                                   BaseShape::ReadFault,
                                   BaseShape::Corrupt,
                                   BaseShape::OtherId,
                                   BaseShape::CorruptRecord,
                                   BaseShape::OwnerViolation,
                                   BaseShape::OtherSchemaRecord,
                                   BaseShape::ReadOnly,
                                   BaseShape::WrongType,
                                   BaseShape::Newer,
                                   BaseShape::OwnedRecord,
                                   BaseShape::ReferenceRecord};

} // namespace

// 닫힘(7.1절 15): Villain을 열면 Hard(Villain → Hero → Base)·EditorOnly(Hero → Derived) 닫힘이 한
// 번에 열리고(Soft 대상은 열지 않는다) 참조가 이어진다. 정보는 id 순이고 파일 이름·지문(색인과
// 같다)·읽기 전용 아님·미해결 없음이다. bulk는 처음 읽을 때 파일을 연다. 이미 연 패키지는 같은
// handle이고 다시 읽지 않으며, 논리 경로로도 같은 패키지를 연다. 순환(CycleA ↔ CycleB)은 한
// 트랜잭션이다. 열린 패키지만 resolver로 찾는다.
TEST(EditorPackageSet_OpenClosure) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    Project project(env.allocator);
    REQUIRE(project.ready && project.files.Ready());
    MemoryFileSystem& files = project.files;
    REQUIRE(files.AddFile("Base.rasset", sources.base.View()));
    REQUIRE(files.AddFile("Derived.rasset", sources.derived.View()));
    REQUIRE(files.AddDirectory("Characters"));
    REQUIRE(files.AddFile("Characters/Hero.rasset", sources.hero.View()));
    REQUIRE(files.AddFile("Characters/Villain.rasset", sources.villain.View()));
    REQUIRE(files.AddFile("Leaf.rasset", sources.leaf.View()));
    REQUIRE(files.AddFile("CycleA.rasset", sources.cycleA.View()));
    REQUIRE(files.AddFile("CycleB.rasset", sources.cycleB.View()));
    REQUIRE(project.Rebuild());
    EditorPackageSet set(env.allocator, Config(env, project));
    REQUIRE(set.Initialize());
    Array<EditorWarning> warnings(env.allocator);
    auto villain = set.Open(FixedId(VillainId), &warnings);
    REQUIRE(villain);
    CHECK(warnings.IsEmpty());
    REQUIRE(set.Count() == 4);
    const uint64 openIds[] = {BaseId, DerivedId, HeroId, VillainId};
    const StringView names[] = {"Base.rasset", "Derived.rasset", "Characters/Hero.rasset",
                                "Characters/Villain.rasset"};
    for (usize position = 0; position < set.Count(); ++position) {
        const EditorPackageInfo& info = set.At(position);
        CHECK(info.id == FixedId(openIds[position]));
        CHECK(info.fileName.View() == names[position]);
        CHECK(!info.readOnly && info.unresolved.IsEmpty());
        auto entry = project.index.FindById(info.id);
        REQUIRE(entry);
        CHECK(info.fingerprint == entry.Value()->fingerprint);
        auto found = set.FindPackage(info.id);
        CHECK(found && found.Value() == info.package);
    }

    CHECK(set.Find(FixedId(VillainId))->package == villain.Value());
    CHECK(set.Find(FixedId(SoftOnlyId)) == nullptr);
    CHECK(set.FindPackage(FixedId(LeafId)).Error() == ErrorCode::NotFound);
    TestAsset* villainAsset = AssetOf(set, villain.Value());
    TestAsset* heroAsset = AssetOf(set, set.Find(FixedId(HeroId))->package);
    REQUIRE(villainAsset != nullptr && heroAsset != nullptr);
    const ObjectHandle heroRepresentative = RepresentativeOf(set, HeroId);
    CHECK(villainAsset->hard.Handle() == heroRepresentative);
    CHECK(villainAsset->typed.Handle() == heroRepresentative);
    CHECK(heroAsset->hard.Handle() == RepresentativeOf(set, BaseId));
    CHECK(heroAsset->editorRef.Handle() == RepresentativeOf(set, DerivedId));
    CHECK(heroAsset->soft.asset == FixedId(SoftOnlyId));
    CHECK(heroAsset->value == HeroValue);

    // 지연 bulk 읽기: 처음 읽을 때만 파일을 연다.
    Package* heroPackage = Pointer<Package>(set.Registry(), set.Find(FixedId(HeroId))->package);
    REQUIRE(heroPackage != nullptr);
    const uint32 opensBefore = files.opens;
    auto pixels = heroPackage->BulkBytes(heroAsset->pixels);
    REQUIRE(pixels);
    CHECK(pixels.Value().Size() == PixelBytes &&
          memcmp(pixels.Value().Data(), sources.pixels.Data(), PixelBytes) == 0);
    CHECK(files.opens == opensBefore + 1);
    CHECK(heroPackage->BulkBytes(heroAsset->pixels));
    CHECK(files.opens == opensBefore + 1);

    // 이미 연 패키지는 같은 handle이고 파일을 다시 열지 않는다(경고 없음).
    auto hero = set.Open(FixedId(HeroId), &warnings);
    CHECK(hero && hero.Value() == set.Find(FixedId(HeroId))->package);
    auto path = ParseLogicalPath(env.allocator, "/Game/Characters/Villain");
    REQUIRE(path);
    auto byPath = set.Open(path.Value(), &warnings);
    CHECK(byPath && byPath.Value() == villain.Value());
    CHECK(files.opens == opensBefore + 1 && set.Count() == 4 && warnings.IsEmpty());

    // 경고 표 없이 연다.
    REQUIRE(set.Open(FixedId(LeafId)));
    CHECK(set.Count() == 5);

    // 순환.
    REQUIRE(set.Open(FixedId(CycleAId), &warnings));
    REQUIRE(set.Count() == 7);
    TestAsset* cycleA = AssetOf(set, set.Find(FixedId(CycleAId))->package);
    TestAsset* cycleB = AssetOf(set, set.Find(FixedId(CycleBId))->package);
    REQUIRE(cycleA != nullptr && cycleB != nullptr);
    CHECK(cycleA->hard.Handle() == RepresentativeOf(set, CycleBId));
    CHECK(cycleB->hard.Handle() == RepresentativeOf(set, CycleAId));
    CHECK(warnings.IsEmpty());
}

// 이어 열기(7.1절 15): Hero를 먼저 열고(Hero·Base·Derived) Villain을 열면 Villain만 새로 읽고 Hero
// import는 열린 Hero로 잇는다(resolver). 먼저 연 Base의 대표 타입이 Hero의 기대와 다르면 resolver로
// 찾은 대상도 Hero의 import만 null이다(UnresolvedImport TypeMismatch).
TEST(EditorPackageSet_IncrementalOpen) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    Project project(env.allocator);
    REQUIRE(project.ready && project.files.Ready());
    REQUIRE(project.files.AddFile("Base.rasset", sources.base.View()));
    REQUIRE(project.files.AddFile("Derived.rasset", sources.derived.View()));
    REQUIRE(project.files.AddFile("Hero.rasset", sources.hero.View()));
    REQUIRE(project.files.AddFile("Villain.rasset", sources.villain.View()));
    REQUIRE(project.Rebuild());
    EditorPackageSet set(env.allocator, Config(env, project));
    REQUIRE(set.Initialize());
    REQUIRE(set.Open(FixedId(HeroId)));
    CHECK(set.Count() == 3);
    const ObjectHandle heroRepresentative = RepresentativeOf(set, HeroId);
    const uint32 opensBefore = project.files.opens;
    auto villain = set.Open(FixedId(VillainId));
    REQUIRE(villain);
    CHECK(set.Count() == 4);
    CHECK(project.files.opens == opensBefore + 1);
    TestAsset* villainAsset = AssetOf(set, villain.Value());
    REQUIRE(villainAsset != nullptr);
    CHECK(villainAsset->hard.Handle() == heroRepresentative);
    CHECK(villainAsset->typed.Handle() == heroRepresentative);

    ShapeScene wrong(env, sources, BaseShape::WrongType);
    REQUIRE(wrong.ready);
    EditorPackageSet other(env.allocator, Config(env, wrong.project));
    REQUIRE(other.Initialize());
    REQUIRE(other.Open(FixedId(BaseId)));
    Array<EditorWarning> warnings(env.allocator);
    auto hero = other.Open(FixedId(HeroId), &warnings);
    REQUIRE(hero);
    CHECK(other.Count() == 3);
    TestAsset* heroAsset = AssetOf(other, hero.Value());
    REQUIRE(heroAsset != nullptr);
    CHECK(heroAsset->hard.Handle().IsNull());
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].kind == EditorWarningKind::UnresolvedImport);
    CHECK(warnings[0].diagnostic.code == AssetDiagnosticCode::TypeMismatch);
    CHECK(warnings[0].diagnostic.related == FixedId(BaseId));
}

// 의존 실패(7.1절 15): Base 모양마다 Hero를 연다. Hero·Derived는 언제나 열리고, Base는 정상·다른
// 스키마 기록·모르는 커스텀 버전(둘 다 읽기 전용)·대표 타입 다름(열리지만 Hero의 import만 null —
// TypeMismatch)이면 열린다. 그 밖은 묶음에서 빠지고(경고 ExcludedDependency — asset Base, related
// Hero, 모양의 코드·오류·칸) Hero의 Base import는 null(경고 UnresolvedImport — asset Hero, related
// Base, ObjectImports 칸 import 0, MissingDependency)이며 정보의 미해결 표에도 남는다. Soft·
// EditorOnly 참조는 영향이 없다.
TEST(EditorPackageSet_DependencyFailures) {
    for (BaseShape shape : AllShapes) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        ShapeScene scene(env, sources, shape);
        REQUIRE(scene.ready);
        const ShapeExpectation expected = Expect(shape);
        EditorPackageSet set(env.allocator, Config(env, scene.project));
        REQUIRE(set.Initialize());
        Array<EditorWarning> warnings(env.allocator);
        auto hero = set.Open(FixedId(HeroId), &warnings);
        REQUIRE(hero);
        const bool excluded = expected.excluded != AssetDiagnosticCode::None ||
                              expected.excludedError != ErrorCode::None;
        CHECK(set.Count() == (expected.baseOpen ? 3u : 2u));
        CHECK(set.Find(FixedId(DerivedId)) != nullptr);
        const EditorPackageInfo* base = set.Find(FixedId(BaseId));
        CHECK((base != nullptr) == expected.baseOpen);
        if (base != nullptr) {
            CHECK(base->readOnly == expected.readOnly);
            CHECK(base->unresolved.IsEmpty());
        }

        const usize warningCount = (excluded ? 1u : 0u) + (expected.unresolved ? 1u : 0u);
        if (warnings.Size() != warningCount) {
            printf("  shape %u: %zu warnings\n", static_cast<unsigned>(shape), warnings.Size());
            CHECK(false);
            continue;
        }

        usize at = 0;
        if (excluded) {
            const EditorWarning& warning = warnings[at++];
            CHECK(warning.kind == EditorWarningKind::ExcludedDependency);
            CHECK(warning.diagnostic.code == expected.excluded);
            if (expected.excludedError != AnyError) {
                CHECK(warning.diagnostic.error == expected.excludedError);
                CHECK(warning.diagnostic.section == expected.excludedSection);
            } else {
                const AssetDiagnostic direct = DirectPrepareFailure(env, scene.variant.View());
                CHECK(direct.code == expected.excluded);
                CHECK(warning.diagnostic.error == direct.error &&
                      warning.diagnostic.section == direct.section &&
                      warning.diagnostic.detail == direct.detail);
            }
            CHECK(warning.diagnostic.asset == FixedId(BaseId));
            CHECK(warning.diagnostic.related == FixedId(HeroId));
        }

        const EditorPackageInfo* heroInfo = set.Find(FixedId(HeroId));
        REQUIRE(heroInfo != nullptr);
        TestAsset* heroAsset = AssetOf(set, hero.Value());
        REQUIRE(heroAsset != nullptr);
        CHECK(heroAsset->hard.Handle().IsNull() == expected.unresolved);
        CHECK(heroAsset->editorRef.Handle() == RepresentativeOf(set, DerivedId));
        CHECK(heroInfo->unresolved.Size() == (expected.unresolved ? 1u : 0u));
        if (expected.unresolved) {
            const EditorWarning& warning = warnings[at];
            CHECK(warning.kind == EditorWarningKind::UnresolvedImport);
            CHECK(warning.diagnostic.code == expected.unresolvedCode);
            CHECK(warning.diagnostic.asset == FixedId(HeroId));
            CHECK(warning.diagnostic.related == FixedId(BaseId));
            CHECK(warning.diagnostic.section ==
                  static_cast<uint32>(PackageSectionKind::ObjectImports));
            CHECK(warning.diagnostic.detail == 0);
            const PackageUnresolvedImport& record = heroInfo->unresolved[0];
            CHECK(record.package == FixedId(HeroId) && record.target == FixedId(BaseId));
            CHECK(record.import == 0 && record.code == expected.unresolvedCode);
            CHECK(record.error == warning.diagnostic.error);
        }
    }
}

// EditorOnly 누락·깊이 2 제외(7.1절 15): EditorOnly로만 가리킨 Derived가 없으면 빠지고(asset
// Derived, related Hero) Hero의 editorRef import(import 1)만 null이다(UnresolvedImport — Hard
// 참조는 이어진다). Villain을 열 때 Base가 없으면 빠진 Base의 related는 Base를 가리킨 중간 패키지
// Hero이고 미해결 import는 Hero의 것이다(Villain 참조는 이어진다).
TEST(EditorPackageSet_EditorOnlyAndDeepExclusion) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    {
        Project project(env.allocator);
        REQUIRE(project.ready && project.files.Ready());
        REQUIRE(project.files.AddFile("Base.rasset", sources.base.View()));
        REQUIRE(project.files.AddFile("Hero.rasset", sources.hero.View()));
        REQUIRE(project.Rebuild());
        EditorPackageSet set(env.allocator, Config(env, project));
        REQUIRE(set.Initialize());
        Array<EditorWarning> warnings(env.allocator);
        auto hero = set.Open(FixedId(HeroId), &warnings);
        REQUIRE(hero);
        CHECK(set.Count() == 2 && set.Find(FixedId(DerivedId)) == nullptr);
        TestAsset* heroAsset = AssetOf(set, hero.Value());
        REQUIRE(heroAsset != nullptr);
        CHECK(heroAsset->editorRef.Handle().IsNull());
        CHECK(heroAsset->hard.Handle() == RepresentativeOf(set, BaseId));
        REQUIRE(warnings.Size() == 2);
        CHECK(warnings[0].kind == EditorWarningKind::ExcludedDependency);
        CHECK(warnings[0].diagnostic.code == AssetDiagnosticCode::MissingDependency);
        CHECK(warnings[0].diagnostic.asset == FixedId(DerivedId));
        CHECK(warnings[0].diagnostic.related == FixedId(HeroId));
        CHECK(warnings[1].kind == EditorWarningKind::UnresolvedImport);
        CHECK(warnings[1].diagnostic.code == AssetDiagnosticCode::MissingDependency);
        CHECK(warnings[1].diagnostic.asset == FixedId(HeroId));
        CHECK(warnings[1].diagnostic.related == FixedId(DerivedId));
        CHECK(warnings[1].diagnostic.detail == 1);
    }

    Project project(env.allocator);
    REQUIRE(project.ready && project.files.Ready());
    REQUIRE(project.files.AddFile("Derived.rasset", sources.derived.View()));
    REQUIRE(project.files.AddFile("Hero.rasset", sources.hero.View()));
    REQUIRE(project.files.AddFile("Villain.rasset", sources.villain.View()));
    REQUIRE(project.Rebuild());
    EditorPackageSet set(env.allocator, Config(env, project));
    REQUIRE(set.Initialize());
    Array<EditorWarning> warnings(env.allocator);
    auto villain = set.Open(FixedId(VillainId), &warnings);
    REQUIRE(villain);
    CHECK(set.Count() == 3);
    TestAsset* villainAsset = AssetOf(set, villain.Value());
    REQUIRE(villainAsset != nullptr);
    CHECK(villainAsset->hard.Handle() == RepresentativeOf(set, HeroId));
    REQUIRE(warnings.Size() == 2);
    CHECK(warnings[0].kind == EditorWarningKind::ExcludedDependency);
    CHECK(warnings[0].diagnostic.asset == FixedId(BaseId));
    CHECK(warnings[0].diagnostic.related == FixedId(HeroId));
    CHECK(warnings[1].kind == EditorWarningKind::UnresolvedImport);
    CHECK(warnings[1].diagnostic.asset == FixedId(HeroId));
    CHECK(warnings[1].diagnostic.related == FixedId(BaseId));
    CHECK(set.Find(FixedId(VillainId))->unresolved.IsEmpty());
    CHECK(set.Find(FixedId(HeroId))->unresolved.Size() == 1);
}

// 요청한 패키지의 실패(7.1절 15): 같은 Base 모양을 Base로 직접 열면 뺀 경고 자리의 진단이 Open
// 실패다(읽기 전용·대표 타입 다름·정상은 열린다). 실패하면 아무것도 열리지 않는다. 색인에 없는 id는
// NotFound(진단 None), 논리 경로가 색인에 없으면 NotFound, 같은 경로 키 둘은 DuplicatePath /
// AlreadyExists다.
TEST(EditorPackageSet_RootFailures) {
    for (BaseShape shape : AllShapes) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        ShapeScene scene(env, sources, shape);
        REQUIRE(scene.ready);
        const ShapeExpectation expected = Expect(shape);
        EditorPackageSet set(env.allocator, Config(env, scene.project));
        REQUIRE(set.Initialize());
        Array<EditorWarning> warnings(env.allocator);
        AssetDiagnostic failure;
        auto base = set.Open(FixedId(BaseId), &warnings, &failure);
        CHECK(warnings.IsEmpty());
        if (expected.baseOpen) {
            REQUIRE(base);
            CHECK(set.Count() == 1 && set.Find(FixedId(BaseId))->readOnly == expected.readOnly);
            continue;
        }

        CHECK(!base && set.Count() == 0);
        // 요청한 패키지가 색인에 없으면 MissingDependency가 아니라 진단 None이다.
        const AssetDiagnosticCode code =
            shape == BaseShape::Absent ? AssetDiagnosticCode::None : expected.excluded;
        CHECK(failure.code == code);
        if (expected.excludedError != AnyError) {
            CHECK(failure.error == expected.excludedError);
            CHECK(failure.section == expected.excludedSection);
        } else {
            const AssetDiagnostic direct = DirectPrepareFailure(env, scene.variant.View());
            CHECK(failure.error == direct.error && failure.section == direct.section &&
                  failure.detail == direct.detail);
        }

        CHECK(failure.error == base.Error());
        CHECK(failure.asset == FixedId(BaseId));
    }

    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    Project project(env.allocator);
    REQUIRE(project.ready && project.files.Ready());
    REQUIRE(project.files.AddFile("Leaf.rasset", sources.leaf.View()));
    REQUIRE(project.files.AddFile("leaf.RASSET", sources.villain.View()));
    REQUIRE(project.Rebuild());
    EditorPackageSet set(env.allocator, Config(env, project));
    REQUIRE(set.Initialize());
    AssetDiagnostic failure;
    CHECK(set.Open(FixedId(NowhereId), nullptr, &failure).Error() == ErrorCode::NotFound);
    CHECK(failure.code == AssetDiagnosticCode::None && failure.asset == FixedId(NowhereId));
    auto missing = ParseLogicalPath(env.allocator, "/Game/Nowhere");
    auto duplicate = ParseLogicalPath(env.allocator, "/Game/Leaf");
    REQUIRE(missing && duplicate);
    CHECK(set.Open(missing.Value(), nullptr, &failure).Error() == ErrorCode::NotFound);
    CHECK(failure.code == AssetDiagnosticCode::None);
    CHECK(set.Open(duplicate.Value(), nullptr, &failure).Error() == ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicatePath);
    CHECK(set.Count() == 0);
}

// 인자와 수명(7.1절 15): 설정의 null 포인터·캐시 용량 0·관찰자 키 0·두 번째 Initialize는
// InvalidArgument, Initialize 전 Open·null id도 InvalidArgument다. 집합을 파괴하면 집합 allocator의
// 할당이 모두 돌아온다(열린 패키지·bulk raw byte 포함).
TEST(EditorPackageSet_ArgumentsAndLifetime) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    Project project(env.allocator);
    REQUIRE(project.ready && project.files.Ready());
    REQUIRE(project.files.AddFile("Base.rasset", sources.base.View()));
    REQUIRE(project.files.AddFile("Derived.rasset", sources.derived.View()));
    REQUIRE(project.files.AddFile("Hero.rasset", sources.hero.View()));
    REQUIRE(project.Rebuild());
    for (usize broken = 0; broken < 6; ++broken) {
        EditorPackageSetConfig config = Config(env, project);
        config.types = broken == 0 ? nullptr : config.types;
        config.files = broken == 1 ? nullptr : config.files;
        config.layout = broken == 2 ? nullptr : config.layout;
        config.index = broken == 3 ? nullptr : config.index;
        config.fileCacheCapacity = broken == 4 ? 0 : config.fileCacheCapacity;
        config.observerMaxKeys = broken == 5 ? 0 : config.observerMaxKeys;
        EditorPackageSet set(env.allocator, config);
        CHECK(set.Initialize().Error() == ErrorCode::InvalidArgument);
        AssetDiagnostic failure;
        CHECK(set.Open(FixedId(HeroId), nullptr, &failure).Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::None);
        auto path = ParseLogicalPath(env.allocator, "/Game/Hero");
        REQUIRE(path);
        CHECK(set.Open(path.Value()).Error() == ErrorCode::InvalidArgument);
    }

    SystemAllocator owned;
    {
        EditorPackageSet set(owned, Config(env, project));
        REQUIRE(set.Initialize());
        CHECK(set.Initialize().Error() == ErrorCode::InvalidArgument);
        CHECK(set.Open(AssetId{}).Error() == ErrorCode::InvalidArgument);
        auto hero = set.Open(FixedId(HeroId));
        REQUIRE(hero);
        TestAsset* asset = AssetOf(set, hero.Value());
        Package* package = Pointer<Package>(set.Registry(), hero.Value());
        REQUIRE(asset != nullptr && package != nullptr);
        CHECK(package->BulkBytes(asset->pixels));
        CHECK(project.files.OpenHandles() == 1);
    }

    CHECK(owned.Stats().liveAllocations == 0);
    CHECK(project.files.OpenHandles() == 0);
}

// 할당 실패(7.1절 15): 집합 allocator의 할당을 하나씩 실패시키면 Initialize·Open은 OutOfMemory(진단
// None)이고 아무것도 열리지 않는다. 예외는 Registry가 계약으로 삼키는 GC 참조 표 할당
// 하나다(실패하면 trace로 대신 — 정확성 같음, ObjectRegistryGcMark — 그 할당만 tag GCRT를 쓴다): 그
// tag의 실패만 성공을 받는다. 유효 ImportRecord(소유·참조 + ImportOwner)가 있는 Base로 연 Hub
// 닫힘(8개 — 닫힘 표가 자란다)과 경고가 생기는 닫힘(Base 없이 Villain — 경고 표도 같은 allocator)을
// 모두 훑는다.
TEST(EditorPackageSet_AllocationFailures) {
    const BaseShape shapes[] = {BaseShape::OwnedRecord, BaseShape::ReferenceRecord,
                                BaseShape::Absent};
    for (BaseShape shape : shapes) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        ShapeScene scene(env, sources, shape);
        REQUIRE(scene.ready);
        const bool hub = shape != BaseShape::Absent;
        MemoryFileSystem& files = scene.project.files;
        REQUIRE(files.AddFile("Villain.rasset", sources.villain.View()));
        REQUIRE(files.AddFile("Leaf.rasset", sources.leaf.View()));
        REQUIRE(files.AddFile("CycleA.rasset", sources.cycleA.View()));
        REQUIRE(files.AddFile("CycleB.rasset", sources.cycleB.View()));
        REQUIRE(files.AddFile("Hub.rasset", sources.hub.View()));
        REQUIRE(scene.project.Rebuild());
        const uint64 root = hub ? HubId : VillainId;
        TaggedFailAllocator failing(env.allocator);
        uint32 failures = 0;
        uint32 tolerated = 0;
        for (uint64 attempt = 1;; ++attempt) {
            EditorPackageSet set(failing, Config(env, scene.project));
            Array<EditorWarning> warnings(failing);
            AssetDiagnostic failure;
            failing.FailOnAttempt(attempt);
            Result<void> result = set.Initialize();
            if (result) {
                auto opened = set.Open(FixedId(root), &warnings, &failure);
                result = opened ? Result<void>::Success() : Result<void>::Failure(opened.Error());
            }

            const bool failed = failing.Failed();
            const uint32 failedTag = failing.FailedTag();
            failing.DisableFailure();
            if (!failed || (result && failedTag == GcReferenceTableTag)) {
                REQUIRE(result);
                CHECK(set.Count() == (hub ? 8u : 3u));
                CHECK(warnings.Size() == (hub ? 0u : 2u));
                if (!failed) {
                    break;
                }

                ++tolerated;
                continue;
            }

            CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
            if (failure.error != ErrorCode::None) {
                CHECK(failure.code == AssetDiagnosticCode::None &&
                      failure.error == ErrorCode::OutOfMemory);
            }

            CHECK(set.Count() == 0);
            ++failures;
        }

        CHECK(failures > 0 && tolerated == 1);
    }
}

// 실제 디스크(7.1절 15): 디스크 프로젝트의 Hero를 물리 파일 시스템으로 열고 bulk를 지연 읽기로
// 읽는다.
TEST(EditorPackageSet_Disk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject project("editor-set");
    REQUIRE(project.made);
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    const struct {
        StringView name;
        Span<const uint8> bytes;
    } files[] = {{"Base.rasset", sources.base.View()},
                 {"Derived.rasset", sources.derived.View()},
                 {"Characters/Hero.rasset", sources.hero.View()}};
    for (const auto& file : files) {
        REQUIRE(WriteContentFile(env.allocator, project, file.name, file.bytes));
    }

    AssetIndex index(env.allocator, layout);
    REQUIRE(index.Rebuild(platform::PhysicalFileSystem(), PackageReadOptions{}));
    EditorPackageSetConfig config;
    config.types = &env.types;
    config.files = &platform::PhysicalFileSystem();
    config.layout = &layout;
    config.index = &index;
    config.registryContext = ContextId{0x4550534554ull, 2};
    config.registryOptions.names = &env.names;
    {
        EditorPackageSet set(env.allocator, config);
        REQUIRE(set.Initialize());
        Array<EditorWarning> warnings(env.allocator);
        auto hero = set.Open(FixedId(HeroId), &warnings);
        REQUIRE(hero);
        CHECK(warnings.IsEmpty() && set.Count() == 3);
        TestAsset* asset = AssetOf(set, hero.Value());
        Package* package = Pointer<Package>(set.Registry(), hero.Value());
        REQUIRE(asset != nullptr && package != nullptr);
        auto pixels = package->BulkBytes(asset->pixels);
        REQUIRE(pixels);
        CHECK(pixels.Value().Size() == PixelBytes &&
              memcmp(pixels.Value().Data(), sources.pixels.Data(), PixelBytes) == 0);
        CHECK(asset->hard.Handle() == RepresentativeOf(set, BaseId));
    }
}

} // namespace editor_package_set_tests_file
