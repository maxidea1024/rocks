#include "../../AssetDatabase/Tests/EditorPackageSetTestSupport.h"

#include <AssetCooker/CookRun.h>
#include <AssetRuntime/PackageReader.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>
#include <TestSupport/NFailAllocator.h>

// 쿠킹 실행 시험이다(Docs/Design/AssetSystem.ko.md 10.2절 결정 6·7, "A4c 구현 세부" — A4c2): 기본
// 처리기로 전체 쿠킹(의존 먼저 순서·Hard 순환·검증된 쿠킹 byte·SHA-256·cookKey·경로 힌트·bulk 크기·
// 의존 목록 — EditorOnly·색인에 없는 Soft·제외 Soft 빼기, EditorOnly 필드 부재(시나리오 11), 원본
// 없이 쿠킹(2)), 두 번 같은 byte(13), 열린 집합 관리(시작 때 열린 패키지 유지·열기 횟수·상한으로
// 한꺼번에 닫기 — 결과 같음), 에셋별 오류 계속(읽기 전용·열기 실패·미해결 import·처리기 실패·깨진
// 출력·로더 대조 실패), 요청 거부(인자·dirty·sink 오류·처리기 조회 오류 — 연 패키지는 닫힘), 할당
// 실패.
using namespace rocks;

namespace cook_run_tests_file {

using namespace rocks::editor_set_test;

namespace {

// 읽기 전용(모르는 커스텀 버전) 패키지의 id다.
constexpr uint64 ReadOnlyId = 2810;
// Soft 장면: NeverCook X, SoftA(Soft → Leaf, bundle Ui = Leaf), SoftB(Soft → X)다.
constexpr uint64 NeverXId = 2812;
constexpr uint64 SoftAId = 2813;
constexpr uint64 SoftBId = 2814;
// Zstd 프로파일의 bulk 정렬이다.
constexpr uint32 ZstdAlignment = 8192;
// 열린 집합 상한 시험의 작은 상한이다.
constexpr usize TinyOpenLimit = 1;
const uint64 AllIds[] = {BaseId, DerivedId, HeroId, VillainId, LeafId, CycleAId, CycleBId, HubId};

// 출력 하나의 사본이다.
struct OutputRecord {
    explicit OutputRecord(Allocator& allocator) noexcept
        : bytes(allocator), path(allocator), dependencies(allocator) {
    }

    AssetId asset{};
    TypeId type{};
    Array<uint8> bytes;
    Sha256Digest sha256{};
    Sha256Digest cookKey{};
    uint64 bulkRawBytes = 0;
    uint32 flags = 0;
    String path;
    Array<CookedManifestDependency> dependencies;
    usize bundles = 0;
};

// 메모리 출력이다(failAt번째 Put이 실패 — 0은 끔).
class MemoryOutput final : public CookOutputSink {
public:
    explicit MemoryOutput(Allocator& allocator) noexcept : records(allocator) {
    }

    Result<void> Put(const CookedPackage& package) noexcept override {
        ++puts;
        if (puts == failAt) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        OutputRecord record(records.GetAllocator());
        record.asset = package.asset;
        record.type = package.type;
        record.sha256 = package.sha256;
        record.cookKey = package.cookKey;
        record.bulkRawBytes = package.bulkRawBytes;
        record.flags = package.flags;
        record.bundles = package.bundles.Size();
        if (!record.bytes.TryAppend(package.bytes) || !record.path.TryAssign(package.pathHint) ||
            !record.dependencies.TryAppend(package.dependencies)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return records.TryPushBack(Move(record));
    }

    const OutputRecord* Of(uint64 id) const noexcept {
        for (const OutputRecord& record : records) {
            if (record.asset == FixedId(id)) {
                return &record;
            }
        }

        return nullptr;
    }

    usize PositionOf(uint64 id) const noexcept {
        for (usize index = 0; index < records.Size(); ++index) {
            if (records[index].asset == FixedId(id)) {
                return index;
            }
        }

        return records.Size();
    }

    Array<OutputRecord> records;
    uint32 puts = 0;
    uint32 failAt = 0;
};

// 대표 타입이 BaseGameAsset 계열이면 실패하는 처리기다(진단 InvalidSetting).
class FailingProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return NuggetProcessorId{Guid{0xFA17, 1}, Guid{0xFA17, 2}};
    }

    Result<void> Cook(CookContext& context) const noexcept override {
        context.failure->code = AssetDiagnosticCode::InvalidSetting;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

// 다른 프로파일 id로 쿠킹 패키지를 쓰는 처리기다(로더 대조가 잡아야 한다).
class WrongProfileProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return NuggetProcessorId{Guid{0x0B0F, 1}, Guid{0x0B0F, 2}};
    }

    Result<void> Cook(CookContext& context) const noexcept override {
        PackageWriteOptions options;
        options.kind = PackageKind::Cooked;
        options.profileId = "Other";
        options.profileSettings = context.profileSettings;
        options.cookerVersion = context.cookerVersion;
        options.bulkSource = context.bulkSource;
        auto written = WritePackage(*context.allocator, *context.registry, context.access,
                                    context.package, options, *context.sink, context.failure);
        return written ? Result<void>::Success() : Result<void>::Failure(written.Error());
    }
};

// 쿠킹 패키지가 아닌 byte를 쓰고 성공하는 처리기다(검증이 잡아야 한다).
class JunkProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return NuggetProcessorId{Guid{0x7A4C, 1}, Guid{0x7A4C, 2}};
    }

    Result<void> Cook(CookContext& context) const noexcept override {
        const uint8 junk[] = {'J', 'U', 'N', 'K'};
        return context.sink->Append(Span<const uint8>(junk, sizeof(junk)));
    }
};

// 실행 장면이다: 메모리 프로젝트(Sources 여덟 + 선택 읽기 전용 패키지)·색인·집합·처리기 표·대상·키.
struct RunScene {
    // fallback이 있으면 기본 처리기 대신 표의 fallback으로 쓴다.
    explicit RunScene(AssetEnv& sceneEnv, bool withReadOnly = false,
                      const CookProcessor* fallback = nullptr, bool withSoft = false) noexcept
        : env(sceneEnv), sources(env.allocator), project(env.allocator),
          set(env.allocator, Config(env, project)), settings(env.allocator),
          table(env.allocator, env.types, fallback != nullptr ? *fallback : defaultProcessor),
          targets(env.allocator), keys(env.allocator), profile(env.allocator) {
        ready = MakeSources(env, sources) && project.ready && project.files.Ready() &&
                project.files.AddFile("Base.rasset", sources.base.View()) &&
                project.files.AddFile("Derived.rasset", sources.derived.View()) &&
                project.files.AddDirectory("Characters") &&
                project.files.AddFile("Characters/Hero.rasset", sources.hero.View()) &&
                project.files.AddFile("Characters/Villain.rasset", sources.villain.View()) &&
                project.files.AddFile("Leaf.rasset", sources.leaf.View()) &&
                project.files.AddFile("CycleA.rasset", sources.cycleA.View()) &&
                project.files.AddFile("CycleB.rasset", sources.cycleB.View()) &&
                project.files.AddFile("Hub.rasset", sources.hub.View()) &&
                (!withReadOnly || AddReadOnly()) && (!withSoft || AddSoft()) && project.Rebuild() &&
                set.Initialize() && profile.id.TryAssign("Windows") && Push("/Game/");
    }

    bool AddReadOnly() noexcept {
        RootRef<Package> package;
        if (MakeAsset<TestAsset>(*env.source, ReadOnlyId, package).IsNull()) {
            return false;
        }

        PackageWriteOptions options = EditorOptions();
        options.customVersions = UnknownCustomVersions;
        const Array<uint8> bytes = Write(env.allocator, *env.source, package.Handle(), options);
        return !bytes.IsEmpty() && project.files.AddFile("ReadOnly.rasset", bytes.View());
    }

    // soft 참조 하나와 태그를 가진 TestAsset 패키지를 file에 쓴다(softTarget 0이면 참조 없음).
    bool AddSoftAsset(StringView file, uint64 id, uint64 softTarget,
                      Span<const PackageTagInput> tags) noexcept {
        RootRef<Package> package;
        const ObjectHandle asset = MakeAsset<TestAsset>(*env.source, id, package);
        TestAsset* value = Pointer<TestAsset>(*env.source, asset);
        if (value == nullptr ||
            (softTarget != 0 && !value->soft.TryAssign(FixedId(softTarget), ""))) {
            return false;
        }

        auto tagBytes = WritePackageTags(env.allocator, tags);
        if (!tagBytes) {
            return false;
        }

        PackageWriteOptions options = EditorOptions();
        options.tags = tagBytes.Value().View();
        const Array<uint8> bytes = Write(env.allocator, *env.source, package.Handle(), options);
        return !bytes.IsEmpty() && project.files.AddFile(file, bytes.View());
    }

    bool AddSoft() noexcept {
        auto leafText = TryFormat(env.allocator, "{}", FixedId(LeafId));
        if (!leafText) {
            return false;
        }

        const PackageTagInput bundle[] = {{"Bundle.Ui", leafText.Value().View()}};
        return project.files.AddDirectory("Never") && project.files.AddDirectory("Soft") &&
               AddSoftAsset("Never/X.rasset", NeverXId, 0, {}) &&
               AddSoftAsset("Soft/SoftA.rasset", SoftAId, LeafId, bundle) &&
               AddSoftAsset("Soft/SoftB.rasset", SoftBId, NeverXId, {});
    }

    bool Push(StringView path) noexcept {
        String value(env.allocator);
        return value.TryAssign(path) && settings.alwaysCook.TryPushBack(Move(value));
    }

    // 대상과 키를 계산한다(키 오류 없음을 요구하지 않는다 — 시험이 본다).
    bool Prepare() noexcept {
        CookTargetRequest targetRequest;
        targetRequest.index = &project.index;
        targetRequest.types = &env.types;
        targetRequest.settings = &settings;
        if (!ComputeCookTargets(env.allocator, targetRequest, targets)) {
            return false;
        }

        CookKeyRequest keyRequest;
        keyRequest.index = &project.index;
        keyRequest.files = &project.files;
        keyRequest.types = &env.types;
        keyRequest.processors = &table;
        keyRequest.targets = &targets;
        keyRequest.profileSettings = ProfileHash();
        return static_cast<bool>(ComputeCookKeys(env.allocator, keyRequest, keys));
    }

    static Sha256Digest ProfileHash() noexcept {
        return Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("Windows"), 7));
    }

    CookRunRequest Request(CookOutputSink& output) noexcept {
        CookRunRequest request;
        request.index = &project.index;
        request.packages = &set;
        request.processors = &table;
        request.targets = &targets;
        request.keys = &keys;
        request.profile = &profile;
        request.profileSettings = ProfileHash();
        request.output = &output;
        return request;
    }

    AssetEnv& env;
    Sources sources;
    Project project;
    EditorPackageSet set;
    CookSettings settings;
    DefaultCookProcessor defaultProcessor;
    CookProcessorTable table;
    CookTargetSet targets;
    CookKeySet keys;
    TargetProfile profile;
    bool ready = false;
};

bool SameBytes(const Array<uint8>& left, const Array<uint8>& right) noexcept {
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

bool HasDependency(const OutputRecord& record, uint64 id, PackageDependencyKind kind) noexcept {
    for (const CookedManifestDependency& dependency : record.dependencies) {
        if (dependency.asset == FixedId(id) && dependency.kind == kind) {
            return true;
        }
    }

    return false;
}

} // namespace

// 전체 쿠킹: 대상 여덟이 의존 먼저(Base·Derived → Hero → Villain, CycleA·CycleB 이웃, Hub는 그
// 대상들 뒤) 쿠킹되고 진단이 없다. 출력은 검증된 쿠킹 byte(프로파일 id·쿠커 버전)와 그 SHA-256·키의
// cookKey·논리 경로·대표 타입이다. Hero: 의존 = Base Hard뿐(EditorOnly → Derived와 색인에 없는
// Soft는 빠진다), 쿠킹 import에 Derived 없음(EditorOnly 필드 부재 — 시나리오 11), bulk raw = 픽셀
// byte. 원본 파일 없이 에디터 패키지만으로 쿠킹한다(시나리오 2). 끝나면 연 패키지는 모두 닫힌다.
TEST(CookRun_CooksAllTargets) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RunScene scene(env);
    REQUIRE(scene.ready && scene.Prepare());
    REQUIRE(scene.targets.targets.Size() == 8);
    for (usize index = 0; index < 8; ++index) {
        CHECK(scene.targets.targets[index].id == FixedId(AllIds[index]));
    }

    REQUIRE(!scene.keys.HasErrors());
    MemoryOutput output(env.allocator);
    CookRunResult result(env.allocator);
    REQUIRE(result.diagnostics.TryPushBack(CookDiagnostic{}));
    REQUIRE(CookPackages(env.allocator, scene.Request(output), result));
    CHECK(result.diagnostics.IsEmpty() && !result.HasErrors());
    CHECK(result.cooked == 8 && output.records.Size() == 8 && result.releases == 0);
    CHECK(scene.set.Count() == 0);

    // 의존 먼저.
    CHECK(output.PositionOf(BaseId) < output.PositionOf(HeroId));
    CHECK(output.PositionOf(HeroId) < output.PositionOf(VillainId));
    const usize cycleA = output.PositionOf(CycleAId);
    const usize cycleB = output.PositionOf(CycleBId);
    CHECK((cycleA > cycleB ? cycleA - cycleB : cycleB - cycleA) == 1);
    const uint64 hubDeps[] = {BaseId, DerivedId, LeafId, CycleAId, VillainId};
    for (uint64 id : hubDeps) {
        CHECK(output.PositionOf(id) < output.PositionOf(HubId));
    }

    for (const OutputRecord& record : output.records) {
        PackageReadOptions read;
        read.expectedKind = PackageKind::Cooked;
        auto contents = ReadPackage(env.allocator, record.bytes.View(), read);
        REQUIRE(contents);
        CHECK(contents.Value().summary.profileId.View() == "Windows");
        CHECK(contents.Value().summary.cookerVersion == CookerVersion);
        CHECK(record.sha256 == Sha256::Hash(record.bytes.View()));
        bool keyed = false;
        for (const CookKey& key : scene.keys.keys) {
            keyed = keyed || (key.asset == record.asset && key.cookKey == record.cookKey);
        }

        CHECK(keyed && record.flags == 0 && record.bundles == 0);
        // 매니페스트 의존 = 쿠킹 패키지 Dependencies 그대로(로더 대조 — AS 11.6절 3).
        REQUIRE(record.dependencies.Size() == contents.Value().dependencies.Size());
        for (usize index = 0; index < record.dependencies.Size(); ++index) {
            CHECK(record.dependencies[index].asset == contents.Value().dependencies[index].asset &&
                  record.dependencies[index].kind == contents.Value().dependencies[index].kind);
        }
    }

    const OutputRecord* hero = output.Of(HeroId);
    REQUIRE(hero != nullptr);
    CHECK(hero->path.View() == "/Game/Characters/Hero" && hero->type == TestAssetType());
    // Hard → Base와 색인에 없는 Soft → SoftOnly(로드가 NotFound)다. EditorOnly → Derived는 없다.
    REQUIRE(hero->dependencies.Size() == 2);
    CHECK(HasDependency(*hero, BaseId, PackageDependencyKind::Hard) &&
          HasDependency(*hero, SoftOnlyId, PackageDependencyKind::Soft));
    CHECK(hero->bulkRawBytes == PixelBytes);
    PackageReadOptions read;
    read.expectedKind = PackageKind::Cooked;
    auto heroContents = ReadPackage(env.allocator, hero->bytes.View(), read);
    REQUIRE(heroContents);
    for (const PackageImportEntry& import : heroContents.Value().imports) {
        CHECK(!(import.asset == FixedId(DerivedId)));
    }

    const OutputRecord* villain = output.Of(VillainId);
    REQUIRE(villain != nullptr);
    CHECK(HasDependency(*villain, HeroId, PackageDependencyKind::Hard));

    // 키가 없는 대상(Leaf)은 건너뛴다 — 진단은 키 단계 몫이라 실행 진단은 없다.
    CookKeySet partial(env.allocator);
    for (const CookKey& key : scene.keys.keys) {
        CookKey copy = key;
        copy.hasKey = !(key.asset == FixedId(LeafId));
        REQUIRE(partial.keys.TryPushBack(copy));
    }

    MemoryOutput skipped(env.allocator);
    CookRunRequest request = scene.Request(skipped);
    request.keys = &partial;
    REQUIRE(CookPackages(env.allocator, request, result));
    CHECK(result.cooked == 7 && skipped.Of(LeafId) == nullptr && result.diagnostics.IsEmpty());
}

// 매니페스트 재료: 의존은 쿠킹 패키지 Dependencies 그대로라 Soft 의존은 색인에 있어도(SoftA → Leaf)
// NeverCook으로 제외됐어도(SoftB → X) 색인에 없어도(Hero → SoftOnly) 남는다. PrimaryAsset 규칙에
// 맞으면 flags PrimaryAsset과 bundle이 실린다. Zstd 프로파일(정렬 8192)은 기본 처리기가 bulk를
// Zstd로, 그 정렬로 쓴다.
TEST(CookRun_ManifestInputsAndProfile) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RunScene scene(env, false, nullptr, true);
    REQUIRE(scene.ready);
    String never(env.allocator);
    REQUIRE(never.TryAssign("/Game/Never/") && scene.settings.neverCook.TryPushBack(Move(never)));
    scene.settings.cookSoftReferences = true;
    PrimaryAssetRule rule(env.allocator);
    String directory(env.allocator);
    REQUIRE(rule.type.TryAssign("TestAsset") && directory.TryAssign("/Game/Soft/") &&
            rule.directories.TryPushBack(Move(directory)) &&
            scene.settings.primaryAssets.TryPushBack(Move(rule)));
    scene.profile.bulkCodec = CookBulkCodec::Zstd;
    scene.profile.bulkAlignment = ZstdAlignment;
    REQUIRE(scene.Prepare());
    REQUIRE(scene.targets.excluded.Size() == 1 && scene.targets.excluded[0] == FixedId(NeverXId));
    MemoryOutput output(env.allocator);
    CookRunResult result(env.allocator);
    REQUIRE(CookPackages(env.allocator, scene.Request(output), result));
    CHECK(result.diagnostics.IsEmpty());

    const OutputRecord* softA = output.Of(SoftAId);
    const OutputRecord* softB = output.Of(SoftBId);
    const OutputRecord* hero = output.Of(HeroId);
    REQUIRE(softA != nullptr && softB != nullptr && hero != nullptr);
    CHECK(softA->dependencies.Size() == 1 &&
          HasDependency(*softA, LeafId, PackageDependencyKind::Soft));
    CHECK(softA->flags == static_cast<uint32>(CookedManifestFlags::PrimaryAsset) &&
          softA->bundles == 1);
    // NeverCook으로 제외된 Soft 대상도 의존 목록에 남는다(쿠킹 패키지와 같아야 한다).
    CHECK(softB->dependencies.Size() == 1 &&
          HasDependency(*softB, NeverXId, PackageDependencyKind::Soft) &&
          softB->flags == static_cast<uint32>(CookedManifestFlags::PrimaryAsset));
    CHECK(HasDependency(*hero, SoftOnlyId, PackageDependencyKind::Soft));
    CHECK(output.Of(NeverXId) == nullptr);

    PackageReadOptions read;
    read.expectedKind = PackageKind::Cooked;
    auto contents = ReadPackage(env.allocator, hero->bytes.View(), read);
    REQUIRE(contents);
    CHECK(contents.Value().header.bulkAlignment == ZstdAlignment);
    REQUIRE(!contents.Value().bulkEntries.IsEmpty());
    CHECK(contents.Value().bulkEntries[0].codec == BulkCodec::Zstd);
    CHECK(hero->bulkRawBytes == PixelBytes);
}

// 결정성(시나리오 13): 두 번 쿠킹하면 모든 패키지가 같은 byte다. 열린 집합 상한을 1로 줄여 매번
// 한꺼번에 닫아도(다시 열어도) byte가 같다.
TEST(CookRun_DeterministicBytes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RunScene scene(env);
    REQUIRE(scene.ready && scene.Prepare());
    MemoryOutput first(env.allocator);
    MemoryOutput second(env.allocator);
    MemoryOutput tiny(env.allocator);
    CookRunResult result(env.allocator);
    REQUIRE(CookPackages(env.allocator, scene.Request(first), result));
    REQUIRE(CookPackages(env.allocator, scene.Request(second), result));
    CookRunRequest small = scene.Request(tiny);
    small.maxOpenPackages = TinyOpenLimit;
    REQUIRE(CookPackages(env.allocator, small, result));
    CHECK(result.releases > 0);
    REQUIRE(first.records.Size() == 8 && second.records.Size() == 8 && tiny.records.Size() == 8);
    for (usize index = 0; index < first.records.Size(); ++index) {
        const OutputRecord& a = first.records[index];
        const OutputRecord* b = second.Of(a.asset.value.low);
        const OutputRecord* c = tiny.Of(a.asset.value.low);
        REQUIRE(b != nullptr && c != nullptr);
        CHECK(SameBytes(a.bytes, b->bytes) && SameBytes(a.bytes, c->bytes));
    }
}

// 열린 집합: 시작 때 열린 패키지(Leaf)는 남고 이번 쿠킹이 연 것만 닫힌다. 상한이 넉넉하면 대상
// Open 호출은 아직 열리지 않은 대상만(의존으로 이미 열린 대상은 다시 열지 않는다), 패키지 파일
// 열기는 패키지 수를 넘지 않는다. 상한 1이면 한꺼번에 닫기가 일어나고 Open 호출이 는다.
TEST(CookRun_OpenSetManagement) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RunScene scene(env);
    REQUIRE(scene.ready && scene.Prepare());
    REQUIRE(scene.set.Open(FixedId(LeafId)));
    MemoryOutput output(env.allocator);
    CookRunResult result(env.allocator);
    const uint32 opensBefore = scene.project.files.opens;
    REQUIRE(CookPackages(env.allocator, scene.Request(output), result));
    CHECK(output.records.Size() == 8 && result.releases == 0);
    CHECK(scene.set.Count() == 1 && scene.set.At(0).id == FixedId(LeafId));
    // Leaf는 이미 열려 있었고 Base·Derived·Hero는 앞 대상이 열면서 함께 열린다.
    CHECK(result.opens < 8);
    CHECK(scene.project.files.opens - opensBefore <= 8);

    MemoryOutput tiny(env.allocator);
    CookRunRequest small = scene.Request(tiny);
    small.maxOpenPackages = TinyOpenLimit;
    CookRunResult tinyResult(env.allocator);
    REQUIRE(CookPackages(env.allocator, small, tinyResult));
    CHECK(tiny.records.Size() == 8 && tinyResult.releases > 0 && tinyResult.opens > result.opens);
    CHECK(scene.set.Count() == 1);
}

// 에셋별 오류는 진단으로 모으고 나머지를 쿠킹한다: 읽기 전용 패키지 UnsupportedVersion, 키 계산 뒤
// 사라진 파일(Leaf) MissingFile, 그것을 Hard로 쓰는 Hub는 의존을 빼고 열려 미해결 import
// MissingDependency(related = Leaf), 처리기 실패(Base 계열 — 그 진단 코드·오류), 깨진 출력(Junk —
// CorruptPackage).
TEST(CookRun_ErrorsContinue) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RunScene scene(env, true);
    REQUIRE(scene.ready);
    FailingProcessor failing;
    REQUIRE(scene.table.Register(IdOf<BaseGameAsset>(), failing));
    REQUIRE(scene.Prepare());
    REQUIRE(!scene.keys.HasErrors());
    scene.project.files.Remove("Leaf.rasset");
    MemoryOutput output(env.allocator);
    CookRunResult result(env.allocator);
    REQUIRE(CookPackages(env.allocator, scene.Request(output), result));
    CHECK(result.HasErrors());
    bool readOnly = false;
    bool missingFile = false;
    bool unresolved = false;
    usize processorFailures = 0;
    for (const CookDiagnostic& diagnostic : result.diagnostics) {
        readOnly = readOnly || (diagnostic.asset == FixedId(ReadOnlyId) &&
                                diagnostic.code == AssetDiagnosticCode::UnsupportedVersion);
        missingFile = missingFile || (diagnostic.asset == FixedId(LeafId) &&
                                      diagnostic.code == AssetDiagnosticCode::MissingFile &&
                                      diagnostic.error == ErrorCode::NotFound);
        unresolved = unresolved || (diagnostic.asset == FixedId(HubId) &&
                                    diagnostic.code == AssetDiagnosticCode::MissingDependency &&
                                    diagnostic.related == FixedId(LeafId));
        processorFailures += diagnostic.code == AssetDiagnosticCode::InvalidSetting &&
                                     diagnostic.error == ErrorCode::InvalidArgument
                                 ? 1
                                 : 0;
    }

    CHECK(readOnly && missingFile && unresolved && processorFailures == 2);
    CHECK(result.diagnostics.Size() == 5);
    // Hero·Villain·CycleA·CycleB는 쿠킹된다(Base는 처리기가 실패해도 Hero의 Hard 의존으로 열린다).
    const uint64 cooked[] = {HeroId, VillainId, CycleAId, CycleBId};
    for (uint64 id : cooked) {
        CHECK(output.Of(id) != nullptr);
    }

    CHECK(output.records.Size() == 4 && result.cooked == 4 && scene.set.Count() == 0);

    // 깨진 출력: 모든 대상이 CorruptPackage(error = 읽기 오류)다.
    AssetEnv junkEnv;
    REQUIRE(junkEnv.Ready());
    JunkProcessor junk;
    RunScene junkScene(junkEnv, false, &junk);
    REQUIRE(junkScene.ready && junkScene.Prepare());
    MemoryOutput junkOutput(junkEnv.allocator);
    CookRunResult junkResult(junkEnv.allocator);
    REQUIRE(CookPackages(junkEnv.allocator, junkScene.Request(junkOutput), junkResult));
    CHECK(junkOutput.records.IsEmpty() && junkResult.diagnostics.Size() == 8);
    for (const CookDiagnostic& diagnostic : junkResult.diagnostics) {
        CHECK(diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
              diagnostic.error != ErrorCode::None);
    }

    // 로더 대조 실패: 처리기가 다른 프로파일 id로 쓰면 모든 대상이 CorruptPackage다(게시 뒤 로드가
    // 거부할 것을 쿠킹 때 잡는다).
    AssetEnv wrongEnv;
    REQUIRE(wrongEnv.Ready());
    WrongProfileProcessor wrong;
    RunScene wrongScene(wrongEnv, false, &wrong);
    REQUIRE(wrongScene.ready && wrongScene.Prepare());
    MemoryOutput wrongOutput(wrongEnv.allocator);
    CookRunResult wrongResult(wrongEnv.allocator);
    REQUIRE(CookPackages(wrongEnv.allocator, wrongScene.Request(wrongOutput), wrongResult));
    CHECK(wrongOutput.records.IsEmpty() && wrongResult.diagnostics.Size() == 8);
    for (const CookDiagnostic& diagnostic : wrongResult.diagnostics) {
        CHECK(diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
              diagnostic.error != ErrorCode::None);
    }
}

// 요청 거부: 인자 null·상한 0·키 수 다름·다른 색인의 대상 InvalidArgument(결과 비움), 시작 때 dirty
// 패키지 Busy, 출력 sink 오류·처리기 조회 오류는 그대로이고 이번 쿠킹이 연 패키지는 닫힌다.
TEST(CookRun_Rejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RunScene scene(env);
    REQUIRE(scene.ready && scene.Prepare());
    MemoryOutput output(env.allocator);
    CookRunResult result(env.allocator);
    REQUIRE(result.diagnostics.TryPushBack(CookDiagnostic{}));
    result.cooked = 9;
    CookRunRequest request = scene.Request(output);
    request.index = nullptr;
    CHECK(CookPackages(env.allocator, request, result).Error() == ErrorCode::InvalidArgument);
    CHECK(result.diagnostics.IsEmpty() && result.cooked == 0);
    const auto rejects = [&](void (*mutate)(CookRunRequest&)) noexcept {
        CookRunRequest changed = scene.Request(output);
        mutate(changed);
        return CookPackages(env.allocator, changed, result).Error() == ErrorCode::InvalidArgument;
    };
    CHECK(rejects([](CookRunRequest& r) noexcept { r.packages = nullptr; }));
    CHECK(rejects([](CookRunRequest& r) noexcept { r.processors = nullptr; }));
    CHECK(rejects([](CookRunRequest& r) noexcept { r.targets = nullptr; }));
    CHECK(rejects([](CookRunRequest& r) noexcept { r.keys = nullptr; }));
    CHECK(rejects([](CookRunRequest& r) noexcept { r.profile = nullptr; }));
    CHECK(rejects([](CookRunRequest& r) noexcept { r.output = nullptr; }));
    CHECK(rejects([](CookRunRequest& r) noexcept { r.maxOpenPackages = 0; }));
    CookKeySet fewer(env.allocator);
    request = scene.Request(output);
    request.keys = &fewer;
    CHECK(CookPackages(env.allocator, request, result).Error() == ErrorCode::InvalidArgument);
    CookTargetSet foreign(env.allocator);
    REQUIRE(foreign.targets.TryEmplaceBack(env.allocator));
    foreign.targets[0].id = FixedId(NowhereId);
    CookKeySet foreignKeys(env.allocator);
    REQUIRE(foreignKeys.keys.TryPushBack(CookKey{}));
    request = scene.Request(output);
    request.targets = &foreign;
    request.keys = &foreignKeys;
    CHECK(CookPackages(env.allocator, request, result).Error() == ErrorCode::InvalidArgument);

    // dirty 패키지.
    REQUIRE(scene.set.Open(FixedId(LeafId)));
    REQUIRE(scene.set.MarkDirty(FixedId(LeafId)));
    CHECK(CookPackages(env.allocator, scene.Request(output), result).Error() == ErrorCode::Busy);
    REQUIRE(scene.set.Close(FixedId(LeafId)));

    // 출력 sink 오류: 그 오류로 멈추고 연 패키지는 닫힌다.
    output.failAt = 3;
    CHECK(CookPackages(env.allocator, scene.Request(output), result).Error() == ErrorCode::Busy);
    CHECK(output.records.Size() == 2 && scene.set.Count() == 0);

    // 처리기 조회 오류(종료한 TypeRegistry의 표).
    TypeRegistry closed(env.allocator);
    REQUIRE(closed.Shutdown());
    CookProcessorTable closedTable(env.allocator, closed, scene.defaultProcessor);
    MemoryOutput fresh(env.allocator);
    request = scene.Request(fresh);
    request.processors = &closedTable;
    CHECK(CookPackages(env.allocator, request, result).Error() == ErrorCode::ShuttingDown);
    CHECK(scene.set.Count() == 0);
}

// 할당 실패: 실행 allocator(처리기의 쓰기·검증 읽기·순서·의존 목록)와 집합 allocator(열기)의 모든
// 지점에서 OutOfMemory이고 연 패키지는 닫힌다. 지점을 다 지나면 기준과 byte가 같다.
TEST(CookRun_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RunScene scene(env);
    REQUIRE(scene.ready && scene.Prepare());
    MemoryOutput baseline(env.allocator);
    CookRunResult result(env.allocator);
    REQUIRE(CookPackages(env.allocator, scene.Request(baseline), result));
    REQUIRE(baseline.records.Size() == 8);

    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        MemoryOutput output(env.allocator);
        CookRunResult run(failing);
        failing.FailOnAttempt(attempt);
        auto cooked = CookPackages(failing, scene.Request(output), run);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        CHECK(scene.set.Count() == 0);
        if (!failed) {
            REQUIRE(cooked);
            REQUIRE(output.records.Size() == baseline.records.Size());
            for (usize index = 0; index < output.records.Size(); ++index) {
                CHECK(SameBytes(output.records[index].bytes, baseline.records[index].bytes));
            }

            break;
        }

        CHECK(cooked.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures >= 20);

    // 집합 allocator: 열기의 할당 실패는 OutOfMemory로 멈춘다(Registry가 삼키는 GC 참조 칸 표
    // 할당만 결과에 드러나지 않는다 — EditorPackageSetTestSupport GcReferenceTableTag). 어느 경우도
    // 연 패키지는 닫힌다.
    TaggedFailAllocator setFailing(env.allocator);
    EditorPackageSet failingSet(setFailing, Config(env, scene.project));
    REQUIRE(failingSet.Initialize());
    uint32 setFailures = 0;
    uint32 closeFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        MemoryOutput output(env.allocator);
        CookRunRequest request = scene.Request(output);
        request.packages = &failingSet;
        CookRunResult run(env.allocator);
        setFailing.FailOnAttempt(attempt);
        auto cooked = CookPackages(env.allocator, request, run);
        const bool failed = setFailing.Failed();
        const uint32 tag = setFailing.FailedTag();
        setFailing.DisableFailure();
        if (failingSet.Count() != 0) {
            // 닫기 자체의 할당 실패다 — OutOfMemory이고 다시 닫으면 닫힌다.
            CHECK(!cooked && cooked.Error() == ErrorCode::OutOfMemory);
            REQUIRE(failingSet.CloseOpened(Span<const AssetId>()));
            ++closeFailures;
        }
        if (!failed) {
            REQUIRE(cooked);
            CHECK(output.records.Size() == baseline.records.Size());
            break;
        }

        if (cooked) {
            CHECK(tag == GcReferenceTableTag);
        } else {
            CHECK(cooked.Error() == ErrorCode::OutOfMemory);
            ++setFailures;
        }
    }

    CHECK(setFailures >= 10 && closeFailures >= 1);

    // 진단이 있는 장면(읽기 전용·처리기 실패·사라진 파일·미해결 import)과 시작 때 열린 패키지
    // (CycleA·CycleB): 진단 push·시작 목록 예약을 포함한 모든 지점에서 OutOfMemory, 시작 때 열린
    // 패키지는 남는다.
    AssetEnv errorEnv;
    REQUIRE(errorEnv.Ready());
    FailingProcessor failingProcessor;
    RunScene errors(errorEnv, true);
    REQUIRE(errors.ready);
    REQUIRE(errors.table.Register(IdOf<BaseGameAsset>(), failingProcessor));
    REQUIRE(errors.Prepare());
    errors.project.files.Remove("Leaf.rasset");
    REQUIRE(errors.set.Open(FixedId(CycleAId)));
    const usize openBefore = errors.set.Count();
    MemoryOutput errorBaseline(errorEnv.allocator);
    CookRunResult errorResult(errorEnv.allocator);
    REQUIRE(CookPackages(errorEnv.allocator, errors.Request(errorBaseline), errorResult));
    REQUIRE(errorResult.diagnostics.Size() == 5);
    test::NFailAllocator errorFailing(errorEnv.allocator);
    uint32 errorFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        MemoryOutput output(errorEnv.allocator);
        CookRunResult run(errorFailing);
        errorFailing.FailOnAttempt(attempt);
        auto cooked = CookPackages(errorFailing, errors.Request(output), run);
        const bool failed = errorFailing.Attempts() >= attempt;
        errorFailing.DisableFailure();
        CHECK(errors.set.Count() == openBefore);
        if (!failed) {
            REQUIRE(cooked);
            CHECK(run.diagnostics.Size() == 5 &&
                  output.records.Size() == errorBaseline.records.Size());
            break;
        }

        CHECK(cooked.Error() == ErrorCode::OutOfMemory);
        ++errorFailures;
    }

    CHECK(errorFailures >= 10);
}

} // namespace cook_run_tests_file
