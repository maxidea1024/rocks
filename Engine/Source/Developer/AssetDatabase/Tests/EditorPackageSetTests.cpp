#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "AssetDatabaseTestSupport.h"
#include "MemoryFileSystem.h"

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

namespace {

constexpr uint64 BaseId = 2800;
constexpr uint64 DerivedId = 2801;
constexpr uint64 HeroId = 2802;
constexpr uint64 VillainId = 2803;
constexpr uint64 LeafId = 2804;
constexpr uint64 CycleAId = 2805;
constexpr uint64 CycleBId = 2806;
constexpr uint64 HubId = 2807;
// Hero의 Soft 대상(열지 않는다 — 색인에도 없다)과 어디에도 없는 id다.
constexpr uint64 SoftOnlyId = 2898;
constexpr uint64 NowhereId = 2899;
constexpr int32 HeroValue = 7;
constexpr usize PixelBytes = 3000;
// 더 새 엔진 버전(TestEngineVersion보다 주 버전이 크다 — NewerVersion)이다.
constexpr EngineVersion NewerEngine{9, 0, 0};
const PackageCustomVersion UnknownCustomVersions[] = {{Guid{0x1DE, 0x99}, 1}};

// Registry가 실패를 삼키는 GC 참조 표 할당의 tag('GCTY' — ObjectRegistryGcInternal.h GcTypeTag)다.
constexpr uint32 GcReferenceTableTag = 0x47435459u;

// n번째 할당을 실패시키고 그 tag를 기록한다(NFailAllocator에 실패 tag를 더한 것).
class TaggedFailAllocator final : public Allocator {
public:
    explicit TaggedFailAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    void FailOnAttempt(uint64 attempt) noexcept {
        attempts_ = 0;
        target_ = attempt;
        failed_ = false;
        failedTag_ = 0;
    }

    void DisableFailure() noexcept {
        FailOnAttempt(0);
    }

    bool Failed() const noexcept {
        return failed_;
    }

    uint32 FailedTag() const noexcept {
        return failedTag_;
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        if (target_ != 0 && ++attempts_ == target_) {
            failed_ = true;
            failedTag_ = tag;
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

private:
    Allocator& inner_;
    uint64 attempts_ = 0;
    uint64 target_ = 0;
    bool failed_ = false;
    uint32 failedTag_ = 0;
};

template <class T>
ObjectHandle MakeAsset(ObjectRegistry& registry, uint64 id, RootRef<Package>& package) noexcept {
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(id), PackageKind::Editor);
    if (!made) {
        return ObjectHandle{};
    }

    package = Move(made.Value());
    return Make<T>(registry, IdOf<T>(), package.Handle(), "");
}

// fields 하나만 든 Record다(손상·다른 스키마 ImportRecord).
Array<uint8> OneFieldRecord(Allocator& allocator, Guid schema) noexcept {
    const uint8 value[4] = {1, 2, 3, 4};
    RecordBuilder builder(allocator, schema, 1);
    if (!builder.AddField(Guid{0, 9}, WireType::U32, value)) {
        return Array<uint8>(allocator);
    }

    auto built = builder.Build();
    return built ? Move(built.Value()) : Array<uint8>(allocator);
}

// 시험 원본 패키지다(env.source). Base(BaseGameAsset), Derived(DerivedGameAsset), Hero(TestAsset —
// Hard → Base, EditorOnly → Derived, Soft → SoftOnly, value, pixels bulk), Villain(TestAsset —
// Hard· typed → Hero), Leaf(TestAsset 참조 없음), CycleA ↔ CycleB(서로 Hard), Hub(TestAsset —
// hardList → Base·Derived·Leaf·CycleA·Villain, 닫힘 8개).
struct Sources {
    explicit Sources(Allocator& allocator) noexcept
        : pixels(allocator), base(allocator), derived(allocator), hero(allocator),
          villain(allocator), leaf(allocator), cycleA(allocator), cycleB(allocator),
          hub(allocator) {
    }

    RootRef<Package> packages[8];
    Array<uint8> pixels;
    TestBulkSource bulk;
    Array<uint8> base;
    Array<uint8> derived;
    Array<uint8> hero;
    Array<uint8> villain;
    Array<uint8> leaf;
    Array<uint8> cycleA;
    Array<uint8> cycleB;
    Array<uint8> hub;
};

bool MakeSources(AssetEnv& env, Sources& out) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    const ObjectHandle base = MakeAsset<BaseGameAsset>(registry, BaseId, out.packages[0]);
    const ObjectHandle derived = MakeAsset<DerivedGameAsset>(registry, DerivedId, out.packages[1]);
    const ObjectHandle hero = MakeAsset<TestAsset>(registry, HeroId, out.packages[2]);
    const ObjectHandle villain = MakeAsset<TestAsset>(registry, VillainId, out.packages[3]);
    const ObjectHandle leaf = MakeAsset<TestAsset>(registry, LeafId, out.packages[4]);
    const ObjectHandle cycleA = MakeAsset<TestAsset>(registry, CycleAId, out.packages[5]);
    const ObjectHandle cycleB = MakeAsset<TestAsset>(registry, CycleBId, out.packages[6]);
    const ObjectHandle hub = MakeAsset<TestAsset>(registry, HubId, out.packages[7]);
    TestAsset* heroValue = Pointer<TestAsset>(registry, hero);
    TestAsset* villainValue = Pointer<TestAsset>(registry, villain);
    TestAsset* cycleAValue = Pointer<TestAsset>(registry, cycleA);
    TestAsset* cycleBValue = Pointer<TestAsset>(registry, cycleB);
    if (base.IsNull() || derived.IsNull() || leaf.IsNull() || heroValue == nullptr ||
        villainValue == nullptr || cycleAValue == nullptr || cycleBValue == nullptr ||
        hub.IsNull() || !out.pixels.TryResize(PixelBytes)) {
        return false;
    }

    for (usize index = 0; index < PixelBytes; ++index) {
        out.pixels[index] = static_cast<uint8>(index * 7 + 3);
    }

    heroValue->value = HeroValue;
    heroValue->pixels = out.bulk.Add(1, out.pixels.View());
    if (!registry.SetReference(access, hero, heroValue->hard, base) ||
        !registry.SetReference(access, hero, heroValue->editorRef, derived) ||
        !heroValue->soft.TryAssign(FixedId(SoftOnlyId), "") ||
        !registry.SetReference(access, villain, villainValue->hard, hero) ||
        !registry.SetReference(access, villain, villainValue->typed, hero) ||
        !registry.SetReference(access, cycleA, cycleAValue->hard, cycleB) ||
        !registry.SetReference(access, cycleB, cycleBValue->hard, cycleA)) {
        return false;
    }

    const ObjectHandle hubTargets[] = {base, derived, leaf, cycleA, villain};
    for (ObjectHandle target : hubTargets) {
        if (!WithBorrow<TestAsset>(registry, hub, [&](TestAsset& held) noexcept {
                return static_cast<bool>(registry.AddReference(access, hub, held.hardList, target));
            })) {
            return false;
        }
    }

    PackageWriteOptions heroOptions = EditorOptions();
    heroOptions.bulkSource = &out.bulk;
    out.base = Write(env.allocator, registry, out.packages[0].Handle(), EditorOptions());
    out.derived = Write(env.allocator, registry, out.packages[1].Handle(), EditorOptions());
    out.hero = Write(env.allocator, registry, out.packages[2].Handle(), heroOptions);
    out.villain = Write(env.allocator, registry, out.packages[3].Handle(), EditorOptions());
    out.leaf = Write(env.allocator, registry, out.packages[4].Handle(), EditorOptions());
    out.cycleA = Write(env.allocator, registry, out.packages[5].Handle(), EditorOptions());
    out.cycleB = Write(env.allocator, registry, out.packages[6].Handle(), EditorOptions());
    out.hub = Write(env.allocator, registry, out.packages[7].Handle(), EditorOptions());
    const Array<uint8>* const all[] = {&out.base, &out.derived, &out.hero,   &out.villain,
                                       &out.leaf, &out.cycleA,  &out.cycleB, &out.hub};
    for (const Array<uint8>* bytes : all) {
        if (bytes->IsEmpty()) {
            return false;
        }
    }

    return true;
}

// 메모리 프로젝트다(Content 파일 + 색인).
struct Project {
    explicit Project(Allocator& allocator) noexcept
        : layout(allocator), ready(static_cast<bool>(layout.Initialize(MemoryRoot))),
          files(allocator, layout.ContentDirectory()), index(allocator, layout) {
    }

    bool Rebuild() noexcept {
        return static_cast<bool>(index.Rebuild(files, PackageReadOptions{}));
    }

    ProjectLayout layout;
    bool ready;
    MemoryFileSystem files;
    AssetIndex index;
};

EditorPackageSetConfig Config(AssetEnv& env, Project& project) noexcept {
    EditorPackageSetConfig config;
    config.types = &env.types;
    config.files = &project.files;
    config.layout = &project.layout;
    config.index = &project.index;
    config.read.engineVersion = TestEngineVersion;
    config.registryContext = ContextId{0x4550534554ull, 1};
    config.registryOptions.names = &env.names;
    return config;
}

// bytes를 집합과 같은 옵션(Defer·에디터·TestEngineVersion)으로 직접 Prepare한 실패 진단이다(손상·
// 버전 모양의 기대값 — 집합이 Prepare 진단을 그대로 넘기는지 본다).
AssetDiagnostic DirectPrepareFailure(AssetEnv& env, Span<const uint8> bytes) noexcept {
    PackageLoadOptions options;
    options.bulk = PackageBulkMode::Defer;
    options.read.expectedKind = PackageKind::Editor;
    options.read.engineVersion = TestEngineVersion;
    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           options);
    MemoryPackageRangeReader reader(bytes);
    AssetDiagnostic failure;
    CHECK(!state.Prepare(reader, &failure));
    return failure;
}

TestAsset* AssetOf(EditorPackageSet& set, ObjectHandle package) noexcept {
    auto representative = set.Registry().FindChild(set.Access(), package, "");
    return representative ? Pointer<TestAsset>(set.Registry(), representative.Value()) : nullptr;
}

ObjectHandle RepresentativeOf(EditorPackageSet& set, uint64 id) noexcept {
    const EditorPackageInfo* info = set.Find(FixedId(id));
    if (info == nullptr) {
        return ObjectHandle{};
    }

    auto representative = set.Registry().FindChild(set.Access(), info->package, "");
    return representative ? representative.Value() : ObjectHandle{};
}

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
        const ImportSourceInput recordSources[] = {{"source", "", "Base.png", Sha256Digest{}, 1}};
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
    Array<EditorOpenWarning> warnings(env.allocator);
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
    Array<EditorOpenWarning> warnings(env.allocator);
    auto hero = other.Open(FixedId(HeroId), &warnings);
    REQUIRE(hero);
    CHECK(other.Count() == 3);
    TestAsset* heroAsset = AssetOf(other, hero.Value());
    REQUIRE(heroAsset != nullptr);
    CHECK(heroAsset->hard.Handle().IsNull());
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].kind == EditorOpenWarningKind::UnresolvedImport);
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
        Array<EditorOpenWarning> warnings(env.allocator);
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
            const EditorOpenWarning& warning = warnings[at++];
            CHECK(warning.kind == EditorOpenWarningKind::ExcludedDependency);
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
            const EditorOpenWarning& warning = warnings[at];
            CHECK(warning.kind == EditorOpenWarningKind::UnresolvedImport);
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
        Array<EditorOpenWarning> warnings(env.allocator);
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
// 하나다(실패하면 trace로 대신 — 정확성 같음, ObjectRegistryGcMark): 그 tag의 실패만 성공을 받는다.
// 유효 ImportRecord가 있는 Base로 연 Hub 닫힘(8개 — 닫힘 표가 자란다)과 경고가 생기는 닫힘(Base
// 없이 Villain — 경고 표도 같은 allocator)을 모두 훑는다.
TEST(EditorPackageSet_AllocationFailures) {
    const BaseShape shapes[] = {BaseShape::OwnedRecord, BaseShape::Absent};
    for (BaseShape shape : shapes) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        ShapeScene scene(env, sources, shape);
        REQUIRE(scene.ready);
        const bool hub = shape == BaseShape::OwnedRecord;
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
            Array<EditorOpenWarning> warnings(failing);
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
        Array<EditorOpenWarning> warnings(env.allocator);
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
