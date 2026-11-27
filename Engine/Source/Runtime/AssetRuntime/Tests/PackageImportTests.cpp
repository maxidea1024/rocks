#include "PackageEditing.h"

#include <Base/Checked.h>
#include <Platform/Memory.h>

// import 표 시험이다(Docs/Design/AssetSystem 5.2절 ObjectImports, 5.7절 13·14):
// - 다른 패키지의 깊은 하위 객체만 참조하면 그 Outer는 outer 전용 import(slot 없음)가 되고, 쿠킹·
//   에디터 모두 묶음 안·resolver로 로드되며, 중간 객체 타입이 다르면 TypeMismatch다.
// - 다른 에셋의 outer 전용 import는 그래프가 받는다(쓰이지 않는 외부 번호) — 그 에셋이 Hard·
//   EditorOnly 의존이 아니면 Prepare가 ObjectImports 손상, Hard 의존을 더하면 디코딩 결과에 없는
//   남는 항목이라 Dependencies 손상으로 거부한다.
// - Hard 의존 expectedType: reader는 0을, Prepare는 대표 에셋 import 타입과 다름을, 로드는 대상
//   대표 에셋의 실제 타입이 파생이 아님(TypeMismatch)·대표 에셋 없음(MissingDependency)을 거부한다.
// - 잘못된 import 이름은 로드(Prepare)가 CorruptPackage로 거부한다(진단 None이 아니다).
// - 규모: 깊이 2040 사슬 아래 잎으로 import 65536개(상한)를 쓰기·읽기·Prepare·Instantiate까지
//   통과하고 65537개는 쓰기 CapacityExceeded다.
using namespace rocks;
namespace package_import_tests_file {

using namespace rocks::asset_test;

namespace {

// A(1100)가 B(1101)의 Gear.Tooth만 참조한다.
struct DeepScene {
    RootRef<Package> a;
    RootRef<Package> b;
    ObjectHandle aAsset{};
    ObjectHandle bAsset{};
    ObjectHandle gear{};
    ObjectHandle tooth{};
};

bool MakeDeep(ObjectRegistry& registry, DeepScene& scene, PackageKind kind) noexcept {
    const ObjectAccessContext access = registry.OwnerAccess();
    auto a = NewPackage(registry, access, FixedId(1100), kind);
    auto b = NewPackage(registry, access, FixedId(1101), kind);
    if (!a || !b) {
        return false;
    }

    scene.a = Move(a.Value());
    scene.b = Move(b.Value());
    scene.bAsset = Make<TestAsset>(registry, TestAssetType(), scene.b.Handle(), "");
    scene.gear = Make<TestPart>(registry, TestPartType(), scene.bAsset, "Gear");
    scene.tooth = Make<TestPart>(registry, TestPartType(), scene.gear, "Tooth");
    scene.aAsset = Make<TestAsset>(registry, TestAssetType(), scene.a.Handle(), "");
    if (scene.tooth.IsNull() || scene.aAsset.IsNull()) {
        return false;
    }

    Pointer<TestPart>(registry, scene.tooth)->index = 77;
    return WithBorrow<TestAsset>(registry, scene.aAsset, [&](TestAsset& owner) {
        return registry.AddReference(access, scene.aAsset, owner.hardList, scene.tooth).IsOk();
    });
}

PackageWriteOptions OptionsOf(PackageKind kind) noexcept {
    return kind == PackageKind::Cooked ? CookedOptions() : EditorOptions();
}

// 로드한 A의 hardList[0]이 B의 Gear.Tooth(index 77)인지다.
bool PointsAtTooth(ObjectRegistry& registry, ObjectHandle package) noexcept {
    auto asset = registry.FindObject(registry.OwnerAccess(), package, StringView());
    if (!asset) {
        return false;
    }

    TestAsset* pointer = Pointer<TestAsset>(registry, asset.Value());
    if (pointer == nullptr || pointer->hardList.Size() != 1) {
        return false;
    }

    TestPart* tooth = Pointer<TestPart>(registry, pointer->hardList[0]);
    String path(ScratchAllocator());
    return tooth != nullptr && tooth->index == 77 &&
           registry.GetObjectPath(registry.OwnerAccess(), pointer->hardList[0], path) &&
           path.View() == StringView("Gear.Tooth");
}

AssetDiagnostic PrepareFailure(AssetEnv& env, Array<uint8>&& bytes, PackageKind kind,
                               bool& prepared) noexcept {
    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           LoadOptions(kind));
    AssetDiagnostic failure;
    prepared = state.Prepare(Move(bytes), &failure).IsOk();
    return failure;
}

bool CorruptOnPrepare(AssetEnv& env, Array<uint8>&& bytes, PackageKind kind,
                      PackageSectionKind section) noexcept {
    bool prepared = false;
    const AssetDiagnostic failure = PrepareFailure(env, Move(bytes), kind, prepared);
    return !prepared && failure.code == AssetDiagnosticCode::CorruptPackage &&
           failure.error == ErrorCode::CorruptData &&
           failure.section == static_cast<uint32>(section);
}

// graph v2 header(magic 8 · formatVersion · flags · nodeCount · externalCount)의 externalCount
// 위치다.
constexpr usize GraphExternalCountOffset = 8 + 3 * sizeof(uint32);

// file의 ObjectImports·Dependencies를 바꾸고, 외부 번호 수(graph v2 header offset 20의
// externalCount)를 import 수에 맞춘다.
Array<uint8> WithImports(Allocator& allocator, const Array<uint8>& file,
                         Span<const PackageImportEntry> imports,
                         Span<const PackageDependencyEntry> dependencies) noexcept {
    EditablePackage edit(allocator);
    (void)edit.Parse(file.View());
    Array<uint8> importBytes =
        ImportsBytes(allocator, imports, static_cast<uint32>(imports.Size()));
    Array<uint8> dependencyBytes =
        DependenciesBytes(allocator, dependencies, static_cast<uint32>(dependencies.Size()));
    (void)edit.Replace(PackageSectionKind::ObjectImports, importBytes.View());
    (void)edit.Replace(PackageSectionKind::Dependencies, dependencyBytes.View());
    EditableSection* objects = edit.Find(PackageSectionKind::Objects);
    if (objects != nullptr && objects->bytes.Size() >= GraphExternalCountOffset + sizeof(uint32)) {
        StoreLittleEndian<uint32>(objects->bytes.Data() + GraphExternalCountOffset,
                                  static_cast<uint32>(imports.Size()));
    }

    return edit.Build();
}

// 시험 resolver처럼 AssetId → handle을 주되 개수 제한이 없다(규모 시험).
struct OneResolver final : PackageImportResolver {
    AssetId id{};
    ObjectHandle handle{};

    Result<ObjectHandle> FindPackage(AssetId asset) noexcept override {
        return asset == id ? Result<ObjectHandle>::Success(handle)
                           : Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }
};

} // namespace

// 깊은 하위 객체만 참조(AS 5.2절 outer import): A가 B의 Gear.Tooth만 가리키면 import는 Gear(outer
// 0, 쓰이지 않는 외부 번호)와 Gear.Tooth(outer 1)이고 의존은 B의 Hard 하나(expectedType = B 대표
// 에셋 타입)다. 쿠킹·에디터 모두 같은 묶음 로드와 resolver 로드가 Gear.Tooth를 가리킨다.
TEST(AssetPackage_DeepSubobjectImportOnly) {
    const PackageKind kinds[] = {PackageKind::Cooked, PackageKind::Editor};
    for (PackageKind kind : kinds) {
        AssetEnv env;
        REQUIRE(env.Ready());
        DeepScene scene;
        REQUIRE(MakeDeep(*env.source, scene, kind));
        Allocator& allocator = env.allocator;
        const Array<uint8> a = Write(allocator, *env.source, scene.a.Handle(), OptionsOf(kind));
        const Array<uint8> b = Write(allocator, *env.source, scene.b.Handle(), OptionsOf(kind));
        REQUIRE(!a.IsEmpty() && !b.IsEmpty());
        PackageReadOptions read;
        read.expectedKind = kind;
        auto contents = ReadPackage(allocator, a.View(), read);
        REQUIRE(contents);
        const Array<PackageImportEntry>& imports = contents.Value().imports;
        REQUIRE(imports.Size() == 2);
        CHECK(imports[0].asset == FixedId(1101) && imports[0].outerImportIndex == 0 &&
              imports[0].name == StringView("Gear") && imports[0].type == TestPartType());
        CHECK(imports[1].asset == FixedId(1101) && imports[1].outerImportIndex == 1 &&
              imports[1].name == StringView("Tooth") && imports[1].type == TestPartType());
        REQUIRE(contents.Value().dependencies.Size() == 1);
        CHECK(contents.Value().dependencies[0].kind == PackageDependencyKind::Hard);
        CHECK(contents.Value().dependencies[0].expectedType == TestAssetType());

        // 같은 묶음(A·B 한 트랜잭션).
        {
            Array<uint8> files[2] = {Copy(allocator, a.View()), Copy(allocator, b.View())};
            RootRef<Object> roots[2];
            ObjectHandle handles[2];
            REQUIRE(LoadAll(env, *env.target, files, kind, nullptr, roots, handles));
            CHECK(PointsAtTooth(*env.target, handles[0]));
            for (usize index = 0; index < 2; ++index) {
                CHECK(env.target->Discard(env.target->OwnerAccess(), handles[index]));
                roots[index].Reset();
            }

            CHECK(env.target->Collect(env.target->OwnerAccess()));
        }

        // resolver(B를 먼저 로드).
        {
            Array<uint8> first[1] = {Copy(allocator, b.View())};
            RootRef<Object> bRoot[1];
            ObjectHandle bHandle[1];
            REQUIRE(LoadAll(env, *env.target, first, kind, nullptr, bRoot, bHandle));
            TestResolver resolver;
            resolver.Add(FixedId(1101), bHandle[0]);
            Array<uint8> second[1] = {Copy(allocator, a.View())};
            RootRef<Object> aRoot[1];
            ObjectHandle aHandle[1];
            REQUIRE(LoadAll(env, *env.target, second, kind, &resolver, aRoot, aHandle));
            CHECK(PointsAtTooth(*env.target, aHandle[0]));
            CHECK(env.target->Discard(env.target->OwnerAccess(), aHandle[0]));
            CHECK(env.target->Discard(env.target->OwnerAccess(), bHandle[0]));
            aRoot[0].Reset();
            bRoot[0].Reset();
            CHECK(env.target->Collect(env.target->OwnerAccess()));
        }

        CHECK(env.target->Stats().liveObjects == 0);
    }
}

// resolver가 준 B에서 중간 객체(Gear) 타입이 다르면 TypeMismatch(position 0), Tooth가 없으면
// MissingDependency(position 1)다. 대표 에셋이 Hard expectedType의 파생이 아니면 TypeMismatch
// (section Dependencies), 대표 에셋이 없으면 MissingDependency다(AS 5.7절 14).
TEST(AssetPackage_DeepImportResolutionFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    DeepScene scene;
    REQUIRE(MakeDeep(*env.source, scene, PackageKind::Cooked));
    Allocator& allocator = env.allocator;
    const Array<uint8> a = Write(allocator, *env.source, scene.a.Handle(), CookedOptions());
    REQUIRE(!a.IsEmpty());
    ObjectRegistry& target = *env.target;
    const ObjectAccessContext access = target.OwnerAccess();

    // B를 target에 직접 만든다(그림마다 다른 모양).
    auto attempt = [&](auto&& build) noexcept {
        auto package = NewPackage(target, access, FixedId(1101), PackageKind::Cooked);
        AssetDiagnostic failure;
        if (!package) {
            failure.code = AssetDiagnosticCode::DuplicateAssetId;
            return failure;
        }

        build(package.Value().Handle());
        PackageLoadState state(allocator, target, env.types, access,
                               LoadOptions(PackageKind::Cooked));
        if (!state.Prepare(Copy(allocator, a.View()), &failure)) {
            return failure;
        }

        TestResolver resolver;
        resolver.Add(FixedId(1101), package.Value().Handle());
        ConstructionContext transaction(target, access);
        PackageLoadState* states[1] = {&state};
        ObjectHandle handles[1];
        if (InstantiatePackages(transaction, states, &resolver, handles, &failure)) {
            failure.code = AssetDiagnosticCode::None;
            failure.error = ErrorCode::None;
        }

        transaction.Rollback();
        (void)target.Discard(access, package.Value().Handle());
        package.Value().Reset();
        (void)target.Collect(access);
        return failure;
    };

    // Gear가 TestScratch(TestPart가 아님).
    AssetDiagnostic failure = attempt([&](ObjectHandle package) noexcept {
        const ObjectHandle asset = Make<TestAsset>(target, TestAssetType(), package, "");
        const ObjectHandle gear = Make<TestScratch>(target, TestScratchType(), asset, "Gear");
        (void)Make<TestPart>(target, TestPartType(), gear, "Tooth");
    });
    CHECK(failure.code == AssetDiagnosticCode::TypeMismatch &&
          failure.error == ErrorCode::TypeMismatch && failure.detail == 0 &&
          failure.section == static_cast<uint32>(PackageSectionKind::ObjectImports));
    // Tooth가 없다.
    failure = attempt([&](ObjectHandle package) noexcept {
        const ObjectHandle asset = Make<TestAsset>(target, TestAssetType(), package, "");
        (void)Make<TestPart>(target, TestPartType(), asset, "Gear");
    });
    CHECK(failure.code == AssetDiagnosticCode::MissingDependency &&
          failure.error == ErrorCode::NotFound && failure.detail == 1);
    // 대표 에셋이 TestRequiredAsset(expectedType TestAsset의 파생이 아님).
    failure = attempt([&](ObjectHandle package) noexcept {
        const ObjectHandle asset = Make<TestRequiredAsset>(target, TestRequiredType(), package, "");
        const ObjectHandle gear = Make<TestPart>(target, TestPartType(), asset, "Gear");
        (void)Make<TestPart>(target, TestPartType(), gear, "Tooth");
    });
    CHECK(failure.code == AssetDiagnosticCode::TypeMismatch &&
          failure.error == ErrorCode::TypeMismatch &&
          failure.section == static_cast<uint32>(PackageSectionKind::Dependencies) &&
          failure.related == FixedId(1101));
    // 대표 에셋이 없다(빈 패키지).
    failure = attempt([](ObjectHandle) noexcept {});
    CHECK(failure.code == AssetDiagnosticCode::MissingDependency &&
          failure.error == ErrorCode::NotFound &&
          failure.section == static_cast<uint32>(PackageSectionKind::ObjectImports));
    // 모양이 맞으면 로드된다(위 실패가 그 모양 때문임을 보인다).
    failure = attempt([&](ObjectHandle package) noexcept {
        const ObjectHandle asset = Make<TestAsset>(target, TestAssetType(), package, "");
        const ObjectHandle gear = Make<TestPart>(target, TestPartType(), asset, "Gear");
        (void)Make<TestPart>(target, TestPartType(), gear, "Tooth");
    });
    CHECK(failure.code == AssetDiagnosticCode::None && failure.error == ErrorCode::None);
    CHECK(target.Stats().liveObjects == 0);
}

// outer 전용 import와 의존 대조(AS 5.7절 5·14): 그래프는 쓰이지 않는 외부 번호를 받으므로(outer
// import) 외부 번호 수를 import 수에 맞추면 Prepare까지 온다. 다른 에셋(C)의 outer 전용 import는 그
// 에셋이 Hard·EditorOnly 의존이 아니면 ObjectImports 손상이고, 의존을 더하면 받는다. 외부 번호 수를
// 맞추지 않으면 그래프 1단계가 import 수 불일치로 거부한다(Objects 손상 —
// AssetPackage_PrepareCrossChecks의 쓰이지 않은 import 경우가 이것이다). Hard expectedType이 0이면
// reader가, 대표 에셋 import 타입과 다르면 Prepare가 Dependencies 손상으로 거부한다.
TEST(AssetPackage_OuterOnlyImportCrossChecks) {
    AssetEnv env;
    REQUIRE(env.Ready());
    DeepScene scene;
    REQUIRE(MakeDeep(*env.source, scene, PackageKind::Cooked));
    Allocator& allocator = env.allocator;
    const Array<uint8> a = Write(allocator, *env.source, scene.a.Handle(), CookedOptions());
    REQUIRE(!a.IsEmpty());
    auto read = ReadPackage(allocator, a.View(), PackageReadOptions{});
    REQUIRE(read);
    const PackageImportEntry gear = read.Value().imports[0];
    const PackageImportEntry tooth = read.Value().imports[1];
    const PackageDependencyEntry hard = read.Value().dependencies[0];
    const AssetId other = FixedId(1102);
    const PackageImportEntry withOther[] = {gear, tooth, {other, 0, "Gear", TestPartType()}};
    const PackageDependencyEntry onlyHard[] = {hard};
    CHECK(CorruptOnPrepare(env, WithImports(allocator, a, withOther, onlyHard), PackageKind::Cooked,
                           PackageSectionKind::ObjectImports));
    const PackageDependencyEntry withHard[] = {
        hard, {other, PackageDependencyKind::Hard, TestAssetType(), false}};
    bool prepared = false;
    (void)PrepareFailure(env, WithImports(allocator, a, withOther, withHard), PackageKind::Cooked,
                         prepared);
    // 디코딩 결과에 C가 없으므로 Hard 의존을 더하면 Dependencies 대조가 남는 항목으로 거부한다.
    CHECK(CorruptOnPrepare(env, WithImports(allocator, a, withOther, withHard), PackageKind::Cooked,
                           PackageSectionKind::Dependencies));
    // 외부 번호 수를 맞추지 않으면 그래프가 먼저 거부한다.
    {
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(a.View()));
        Array<uint8> imports = ImportsBytes(allocator, withOther, 3);
        REQUIRE(edit.Replace(PackageSectionKind::ObjectImports, imports.View()));
        CHECK(
            CorruptOnPrepare(env, edit.Build(), PackageKind::Cooked, PackageSectionKind::Objects));
    }

    // Hard expectedType 0은 reader가 거부한다(Hard ⇔ expectedType ≠ 0).
    {
        const PackageImportEntry same[] = {gear, tooth};
        const PackageDependencyEntry zero[] = {
            {hard.asset, PackageDependencyKind::Hard, TypeId{}, hard.required}};
        CHECK(CorruptOnPrepare(env, WithImports(allocator, a, same, zero), PackageKind::Cooked,
                               PackageSectionKind::Dependencies));
    }

    // 대표 에셋 import가 있으면 그 타입 = expectedType: B 대표 에셋을 직접 가리키는 패키지에서
    // expectedType을 TestPart로 바꾸면 Prepare가 거부한다.
    {
        ObjectRegistry& source = *env.source;
        auto direct = NewPackage(source, source.OwnerAccess(), FixedId(1103), PackageKind::Cooked);
        REQUIRE(direct);
        const ObjectHandle asset =
            Make<TestAsset>(source, TestAssetType(), direct.Value().Handle(), "");
        REQUIRE(source.SetReference(source.OwnerAccess(), asset,
                                    Pointer<TestAsset>(source, asset)->hard, scene.bAsset));
        const Array<uint8> file =
            Write(allocator, source, direct.Value().Handle(), CookedOptions());
        REQUIRE(!file.IsEmpty());
        auto parsed = ReadPackage(allocator, file.View(), PackageReadOptions{});
        REQUIRE(parsed);
        REQUIRE(parsed.Value().imports.Size() == 1);
        REQUIRE(parsed.Value().imports[0].name.ByteCount() == 0);
        const PackageImportEntry representative[] = {parsed.Value().imports[0]};
        PackageDependencyEntry wrong = parsed.Value().dependencies[0];
        REQUIRE(wrong.expectedType == TestAssetType());
        wrong.expectedType = TestPartType();
        const PackageDependencyEntry wrongRows[] = {wrong};
        CHECK(CorruptOnPrepare(env, WithImports(allocator, file, representative, wrongRows),
                               PackageKind::Cooked, PackageSectionKind::Dependencies));
        // 바꾸지 않은 재구성은 받는다.
        const PackageDependencyEntry rightRows[] = {parsed.Value().dependencies[0]};
        (void)PrepareFailure(env, WithImports(allocator, file, representative, rightRows),
                             PackageKind::Cooked, prepared);
        CHECK(prepared);
    }
}

// 잘못된 import 이름은 로드(Prepare)에서 CorruptPackage다(AS 5.7절 13 — 처음 구현은 Instantiate의
// FindObject가 InvalidArgument로 거부해 진단 코드가 None이었다).
TEST(AssetPackage_InvalidImportNameIsCorruptOnLoad) {
    AssetEnv env;
    REQUIRE(env.Ready());
    DeepScene scene;
    REQUIRE(MakeDeep(*env.source, scene, PackageKind::Cooked));
    Allocator& allocator = env.allocator;
    const Array<uint8> a = Write(allocator, *env.source, scene.a.Handle(), CookedOptions());
    REQUIRE(!a.IsEmpty());
    auto read = ReadPackage(allocator, a.View(), PackageReadOptions{});
    REQUIRE(read);
    const PackageDependencyEntry deps[] = {read.Value().dependencies[0]};
    const StringView names[] = {StringView("Too\x01th"), StringView("Toe\xCC\x81"),
                                StringView("Tooth.X"), StringView("NUL")};
    for (StringView name : names) {
        const PackageImportEntry imports[] = {read.Value().imports[0],
                                              {FixedId(1101), 1, name, TestPartType()}};
        CHECK(CorruptOnPrepare(env, WithImports(allocator, a, imports, deps), PackageKind::Cooked,
                               PackageSectionKind::ObjectImports));
    }
}

namespace {

// 규모 시험의 Registry allocator다. SystemAllocator는 살아 있는 할당의 선형 원장이라(해제 O(n))
// 객체 13만 개를 가진 Registry의 종료 회수가 제곱 시간이 된다(처음 구현은 시험 시간의 대부분이
// Registry 종료였다). 이 allocator는 플랫폼 정렬 할당에 바로 맡기고 살아 있는 수만 센다 — 끝에서
// 0인지 확인해 누수 판정은 같다(외부 주소·중복 해제 검증은 하지 않는다).
class ScaleAllocator final : public Allocator {
public:
    ~ScaleAllocator() noexcept override {
        CHECK(live_ == 0);
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 = 0) noexcept override {
        if (!IsPowerOfTwo(alignment)) {
            return Result<void*>::Failure(ErrorCode::InvalidArgument);
        }

        if (bytes == 0) {
            return Result<void*>::Success(nullptr);
        }

        void* address = platform::AlignedAllocate(bytes, alignment);
        if (address == nullptr) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        ++live_;
        return Result<void*>::Success(address);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address != nullptr) {
            platform::AlignedFree(address);
            --live_;
        }

        return Result<void>::Success();
    }

private:
    uint64 live_ = 0;
};

// 규모 시험의 패키지 정책 Registry다(AssetEnv와 같은 설정, allocator만 ScaleAllocator).
UniquePtr<ObjectRegistry> MakeScaleRegistry(AssetEnv& env, Allocator& objects,
                                            uint64 instance) noexcept {
    RegistryOptions options;
    options.names = &env.names;
    options.policy.packageNativeType = NativeTypeKey<Package>();
    options.policy.rejectWorldOnly = true;
    auto made = UniquePtr<ObjectRegistry>::TryMake(env.allocator, objects, env.types,
                                                   ContextId{0x4153534554ull, instance}, options);
    return made ? Move(made.Value()) : UniquePtr<ObjectRegistry>();
}

} // namespace

// 규모(AS 5.7절 13): B에 깊이 2040 사슬(이름 "a", 경로 4079 byte)과 맨 아래 잎을 두고 A가 잎만
// 가리킨다. import = 사슬 2040(outer 전용) + 잎 63496 = 65536(상한)이고 쓰기·읽기·Prepare·
// Instantiate(resolver — B는 export 상한을 넘어 패키지로 쓰지 않고 target에 같은 모양으로 만든다)를
// 지나 모든 잎을 가리킨다. 잎 하나를 더 가리키면 import 65537개라 쓰기 CapacityExceeded다. 단언은
// 결과(항목 수·경로·대상)로 한다 — 처음 구현은 import마다 경로를 처음부터 다시 만들어 이 규모에서
// 수억 단계였다. 두 Registry는 ScaleAllocator를 쓴다(위 설명).
TEST(AssetPackage_ImportScale) {
    constexpr usize Depth = 2040;
    constexpr usize Leaves = PackageMaxObjectImports - Depth;
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    // Registry가 RootRef·handle보다 오래 살도록 먼저 선언한다.
    ScaleAllocator objects;
    UniquePtr<ObjectRegistry> sourceRegistry = MakeScaleRegistry(env, objects, 21);
    UniquePtr<ObjectRegistry> targetRegistry = MakeScaleRegistry(env, objects, 22);
    REQUIRE(sourceRegistry.Get() != nullptr && targetRegistry.Get() != nullptr);

    // registry에 B(대표 에셋 + 사슬 + 잎 count개)를 만들고 잎 handle을 돌려준다.
    auto makeB = [&](ObjectRegistry& registry, RootRef<Package>& package,
                     Array<ObjectHandle>& leaves, usize count) noexcept {
        auto made =
            NewPackage(registry, registry.OwnerAccess(), FixedId(1201), PackageKind::Cooked);
        if (!made) {
            return false;
        }

        package = Move(made.Value());
        ObjectHandle cursor = Make<TestAsset>(registry, TestAssetType(), package.Handle(), "");
        for (usize level = 0; level < Depth && !cursor.IsNull(); ++level) {
            cursor = Make<TestPart>(registry, TestPartType(), cursor, "a");
        }

        if (cursor.IsNull() || !leaves.TryReserve(count)) {
            return false;
        }

        for (usize index = 0; index < count; ++index) {
            char name[6] = {'L', 0, 0, 0, 0, 0};
            usize value = index;
            for (usize digit = 5; digit > 1; --digit) {
                name[digit - 1] = static_cast<char>('a' + value % 26);
                value /= 26;
            }

            const ObjectHandle leaf =
                Make<TestPart>(registry, TestPartType(), cursor, StringView(name, 5));
            if (leaf.IsNull()) {
                return false;
            }

            (void)leaves.TryPushBack(leaf);
        }

        return true;
    };

    ObjectRegistry& source = *sourceRegistry;
    RootRef<Package> sourceB;
    Array<ObjectHandle> sourceLeaves(allocator);
    REQUIRE(makeB(source, sourceB, sourceLeaves, Leaves + 1));
    auto aPackage = NewPackage(source, source.OwnerAccess(), FixedId(1200), PackageKind::Cooked);
    REQUIRE(aPackage);
    const ObjectHandle aAsset =
        Make<TestAsset>(source, TestAssetType(), aPackage.Value().Handle(), "");
    REQUIRE(!aAsset.IsNull());
    REQUIRE(WithBorrow<TestAsset>(source, aAsset, [&](TestAsset& owner) {
        for (usize index = 0; index < Leaves; ++index) {
            if (!source.AddReference(source.OwnerAccess(), aAsset, owner.hardList,
                                     sourceLeaves[index])) {
                return false;
            }
        }

        return true;
    }));
    const Array<uint8> file = Write(allocator, source, aPackage.Value().Handle(), CookedOptions());
    REQUIRE(!file.IsEmpty());
    auto read = ReadPackage(allocator, file.View(), PackageReadOptions{});
    REQUIRE(read);
    const Array<PackageImportEntry>& imports = read.Value().imports;
    REQUIRE(imports.Size() == PackageMaxObjectImports);
    CHECK(imports[0].outerImportIndex == 0 && imports[0].name == StringView("a"));
    CHECK(imports[Depth - 1].outerImportIndex == Depth - 1);
    CHECK(imports[Depth].outerImportIndex == Depth && imports[Depth].name == StringView("Laaaa"));
    CHECK(imports[PackageMaxObjectImports - 1].outerImportIndex == Depth);

    ObjectRegistry& target = *targetRegistry;
    RootRef<Package> targetB;
    Array<ObjectHandle> targetLeaves(allocator);
    REQUIRE(makeB(target, targetB, targetLeaves, Leaves));
    OneResolver resolver;
    resolver.id = FixedId(1201);
    resolver.handle = targetB.Handle();
    Array<uint8> files[1] = {Copy(allocator, file.View())};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(LoadAll(env, target, files, PackageKind::Cooked, &resolver, roots, handles));
    auto loaded = target.FindObject(target.OwnerAccess(), handles[0], StringView());
    REQUIRE(loaded);
    TestAsset* asset = Pointer<TestAsset>(target, loaded.Value());
    REQUIRE(asset != nullptr);
    REQUIRE(asset->hardList.Size() == Leaves);
    bool same = true;
    for (usize index = 0; index < Leaves; ++index) {
        same = same && asset->hardList[index] == targetLeaves[index];
    }

    CHECK(same);

    // 잎 하나 더: import 65537개.
    REQUIRE(WithBorrow<TestAsset>(source, aAsset, [&](TestAsset& owner) {
        return source
            .AddReference(source.OwnerAccess(), aAsset, owner.hardList, sourceLeaves[Leaves])
            .IsOk();
    }));
    MemoryPackageSink sink(allocator);
    CHECK(WritePackage(allocator, source, source.OwnerAccess(), aPackage.Value().Handle(),
                       CookedOptions(), sink)
              .Error() == ErrorCode::CapacityExceeded);
}
} // namespace package_import_tests_file
