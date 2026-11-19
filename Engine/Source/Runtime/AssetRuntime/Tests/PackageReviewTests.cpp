#include "PackageEditing.h"

#include <AssetRuntime/Package.generated.h>
#include <Core/Text/NameComponent.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>

// 재리뷰가 찾은 경로의 시험이다(Docs/Design/AssetSystem.ko.md 5.2절, 5.5절 3, 5.7절 13–18):
// 선언 타입이 기대 타입보다 넓은 import의 연결 단계 대조, 쓰이지 않는 import 거부, resolver 결과
// 검증, Prepare의 allocator 스레드 계약, 에디터 읽기 전용 열기의 끝까지 로드, export 이름 규칙의
// 로드 분류, Zstd bulk 블록 압축의 메모리 피크, Tags 키 정렬·형식·할당 실패, 종료된 TypeRegistry,
// 클래스가 아닌 export 타입, 파괴 요청만 받은 import 대상, Prepare 도중 닫히는 TypeRegistry.
using namespace rocks;
using namespace rocks::asset_test;

namespace {

// graph v2 header(magic 8 · formatVersion · flags · nodeCount · externalCount)의 externalCount
// 위치다.
constexpr usize GraphExternalCountOffset = 8 + 3 * sizeof(uint32);

// file의 ObjectImports(와 주면 Dependencies)를 바꾸고 외부 번호 수를 import 수에 맞춘 패키지다.
Array<uint8> Rewrite(Allocator& allocator, const Array<uint8>& file,
                     Span<const PackageImportEntry> imports,
                     Span<const PackageDependencyEntry> dependencies = {}) noexcept {
    EditablePackage edit(allocator);
    (void)edit.Parse(file.View());
    Array<uint8> importBytes =
        ImportsBytes(allocator, imports, static_cast<uint32>(imports.Size()));
    (void)edit.Replace(PackageSectionKind::ObjectImports, importBytes.View());
    if (!dependencies.IsEmpty()) {
        Array<uint8> dependencyBytes =
            DependenciesBytes(allocator, dependencies, static_cast<uint32>(dependencies.Size()));
        (void)edit.Replace(PackageSectionKind::Dependencies, dependencyBytes.View());
    }

    EditableSection* objects = edit.Find(PackageSectionKind::Objects);
    if (objects != nullptr && objects->bytes.Size() >= GraphExternalCountOffset + sizeof(uint32)) {
        StoreLittleEndian<uint32>(objects->bytes.Data() + GraphExternalCountOffset,
                                  static_cast<uint32>(imports.Size()));
    }

    return edit.Build();
}

// Required가 아닌 더 새 Tags 섹션(schemaVersion 2)을 넣어 에디터 읽기 전용으로 여는 패키지다.
Array<uint8> WithNewerTags(Allocator& allocator, const Array<uint8>& file) noexcept {
    EditablePackage edit(allocator);
    (void)edit.Parse(file.View());
    RecordBuilder record(allocator, Guid{1, 1}, 1);
    auto built = record.Build();
    EditableSection* tags = edit.Insert(static_cast<uint32>(PackageSectionKind::Tags), 0,
                                        built ? built.Value().View() : Span<const uint8>());
    if (tags != nullptr) {
        tags->schemaVersion = 2;
    }

    return edit.Build();
}

// Objects에서 필드 id의 첫 TLV id를 다른 id(low 비트 반전 — 정렬 위치는 그대로)로 바꾼다(그
// 필드는 reader가 모르는 필드가 된다).
Array<uint8> WithUnknownField(Allocator& allocator, const Array<uint8>& file, Guid id) noexcept {
    EditablePackage edit(allocator);
    (void)edit.Parse(file.View());
    EditableSection* objects = edit.Find(PackageSectionKind::Objects);
    const usize payload = objects != nullptr ? FieldPayload(objects->bytes.View(), id) : IndexNone;
    if (payload == IndexNone) {
        return Array<uint8>(allocator);
    }

    const usize at = payload - RecordFieldPayloadOffset + sizeof(uint64);
    objects->bytes[at] ^= 1;
    return edit.Build();
}

// Objects에서 type 노드 Record의 스키마 버전(envelope 1 · schema · version 순서)과 Summary 타입
// 표의 그 행을 version + 1로 올린 에디터 패키지다(더 새 타입 버전 — 읽기 전용). Summary 타입 표는
// type 한 행이어야 한다. 노드 Record가 정확히 하나가 아니면 빈 배열이다.
Array<uint8> WithNewerTypeVersion(Allocator& allocator, const Array<uint8>& file, TypeId type,
                                  uint32 version) noexcept {
    constexpr usize EnvelopeBytes = sizeof(uint32);
    constexpr usize SchemaBytes = 2 * sizeof(uint64);
    constexpr usize PatternBytes = EnvelopeBytes + SchemaBytes + sizeof(uint32);
    EditablePackage edit(allocator);
    (void)edit.Parse(file.View());
    EditableSection* objects = edit.Find(PackageSectionKind::Objects);
    if (objects == nullptr) {
        return Array<uint8>(allocator);
    }

    uint8 pattern[PatternBytes];
    StoreLittleEndian<uint32>(pattern, 1);
    StoreLittleEndian<uint64>(pattern + EnvelopeBytes, type.high);
    StoreLittleEndian<uint64>(pattern + EnvelopeBytes + sizeof(uint64), type.low);
    StoreLittleEndian<uint32>(pattern + EnvelopeBytes + SchemaBytes, version);
    usize found = 0;
    usize at = 0;
    for (usize offset = 0; offset + PatternBytes <= objects->bytes.Size(); ++offset) {
        bool same = true;
        for (usize index = 0; same && index < PatternBytes; ++index) {
            same = objects->bytes[offset + index] == pattern[index];
        }

        if (same) {
            ++found;
            at = offset;
        }
    }

    if (found != 1) {
        return Array<uint8>(allocator);
    }

    StoreLittleEndian<uint32>(objects->bytes.Data() + at + EnvelopeBytes + SchemaBytes,
                              version + 1);
    const PackageTypeVersion types[] = {{type, version + 1}};
    Array<uint8> summary =
        SummaryBytes(allocator, SummaryInput{types, {}, true}, SummaryFault::None);
    if (summary.IsEmpty() || !edit.Replace(PackageSectionKind::Summary, summary.View())) {
        return Array<uint8>(allocator);
    }

    return edit.Build();
}

// package의 대표 에셋 handle이다(없으면 null).
ObjectHandle RepresentativeOf(ObjectRegistry& registry, ObjectHandle package) noexcept {
    auto found = registry.FindObject(registry.OwnerAccess(), package, StringView());
    return found ? found.Value() : ObjectHandle{};
}

struct PrepareResult {
    bool prepared = false;
    AssetDiagnostic failure{};
};

PrepareResult PrepareOnly(AssetEnv& env, Array<uint8>&& bytes, PackageKind kind) noexcept {
    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           LoadOptions(kind));
    PrepareResult result;
    result.prepared = state.Prepare(Move(bytes), &result.failure).IsOk();
    return result;
}

bool CorruptAt(const PrepareResult& result, PackageSectionKind section, uint64 detail) noexcept {
    return !result.prepared && result.failure.code == AssetDiagnosticCode::CorruptPackage &&
           result.failure.error == ErrorCode::InvalidArgument &&
           result.failure.section == static_cast<uint32>(section) &&
           result.failure.detail == detail;
}

// resolver가 handle 하나를 돌려준다.
struct FixedResolver final : PackageImportResolver {
    ObjectHandle handle{};

    Result<ObjectHandle> FindPackage(AssetId) noexcept override {
        return Result<ObjectHandle>::Success(handle);
    }
};

// file을 target에 Prepare·Instantiate한다(공개하지 않는다 — Rollback).
AssetDiagnostic InstantiateFailure(AssetEnv& env, const Array<uint8>& file, PackageKind kind,
                                   PackageImportResolver& resolver, bool& instantiated) noexcept {
    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           LoadOptions(kind));
    AssetDiagnostic failure;
    instantiated = false;
    if (!state.Prepare(Copy(env.allocator, file.View()), &failure)) {
        return failure;
    }

    ConstructionContext transaction(*env.target, env.target->OwnerAccess());
    PackageLoadState* states[1] = {&state};
    ObjectHandle handles[1];
    instantiated = InstantiatePackages(transaction, states, &resolver, handles, &failure).IsOk();
    transaction.Rollback();
    return failure;
}

// 할당한 스레드를 기록하는 allocator다(소유 스레드 밖 할당 수).
struct ThreadRecordingAllocator final : Allocator {
    explicit ThreadRecordingAllocator(Allocator& inner) noexcept
        : inner(inner), owner(platform::CurrentThreadId()) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (platform::CurrentThreadId() == owner) {
            ++ownerCalls;
        } else {
            ++foreignCalls;
        }

        return inner.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner.Deallocate(address);
    }

    Allocator& inner;
    uint64 owner = 0;
    uint64 ownerCalls = 0;
    uint64 foreignCalls = 0;
};

// 살아 있는 byte의 최댓값을 기록하는 allocator다(할당 앞에 크기 머리를 둔다).
struct HighWaterAllocator final : Allocator {
    static constexpr usize HeaderBytes = 64;

    explicit HighWaterAllocator(Allocator& inner) noexcept : inner(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (alignment > HeaderBytes) {
            return Result<void*>::Failure(ErrorCode::Unsupported);
        }

        auto block = inner.Allocate(bytes + HeaderBytes, HeaderBytes, tag);
        if (!block) {
            return block;
        }

        auto* base = static_cast<uint8*>(block.Value());
        StoreLittleEndian<uint64>(base, bytes);
        live += bytes;
        peak = live > peak ? live : peak;
        return Result<void*>::Success(base + HeaderBytes);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == nullptr) {
            return Result<void>::Success();
        }

        auto* base = static_cast<uint8*>(address) - HeaderBytes;
        live -= LoadLittleEndian<uint64>(base);
        return inner.Deallocate(base);
    }

    Allocator& inner;
    uint64 live = 0;
    uint64 peak = 0;
};

} // namespace

// 선언 타입이 기대 타입보다 넓은 import(AS 5.7절 14): 그래프 1단계는 선언 타입과 필드 기대 타입이
// 어느 쪽으로든 파생 관계이면 받고 로드의 import 대조는 IsA(실제, 선언)만 보므로, 실제 대상이 선언
// 타입(Asset)은 만족하지만 필드 기대 타입(TestAsset)은 아니면 연결 단계(ApplyFields)가
// TypeMismatch(section Objects)로 거부한다. 하위 객체 import와 대표 에셋 import(선언 타입과 Hard
// expectedType을 함께 Asset으로) 모두 같다.
TEST(AssetPackage_ImportDeclaredBaseTypeMismatch) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    ObjectRegistry& target = *env.target;
    Allocator& allocator = env.allocator;
    const ObjectAccessContext access = source.OwnerAccess();
    const TypeId assetType = GeneratedType<Asset>::Id();
    // A(1500)는 B(1501)의 하위 객체 Gear를, C(1502)는 D(1503)의 대표 에셋을 typed(TestAsset 기대)로
    // 가리킨다.
    auto b = NewPackage(source, access, FixedId(1501), PackageKind::Cooked);
    auto a = NewPackage(source, access, FixedId(1500), PackageKind::Cooked);
    auto c = NewPackage(source, access, FixedId(1502), PackageKind::Cooked);
    auto d = NewPackage(source, access, FixedId(1503), PackageKind::Cooked);
    REQUIRE(a && b && c && d);
    const ObjectHandle bAsset = Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
    const ObjectHandle gear = Make<TestAsset>(source, TestAssetType(), bAsset, "Gear");
    const ObjectHandle aAsset = Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
    const ObjectHandle cAsset = Make<TestAsset>(source, TestAssetType(), c.Value().Handle(), "");
    const ObjectHandle dAsset = Make<TestAsset>(source, TestAssetType(), d.Value().Handle(), "");
    REQUIRE(!gear.IsNull() && !aAsset.IsNull() && !cAsset.IsNull() && !dAsset.IsNull());
    REQUIRE(source.SetReference(access, aAsset, Pointer<TestAsset>(source, aAsset)->typed, gear));
    REQUIRE(source.SetReference(access, cAsset, Pointer<TestAsset>(source, cAsset)->typed, dAsset));
    const Array<uint8> aFile = Write(allocator, source, a.Value().Handle(), CookedOptions());
    const Array<uint8> cFile = Write(allocator, source, c.Value().Handle(), CookedOptions());
    REQUIRE(!aFile.IsEmpty() && !cFile.IsEmpty());

    // target: B의 Gear와 D의 대표 에셋이 TestRequiredAsset(Asset이지만 TestAsset이 아님). B의 대표
    // 에셋은 TestAsset이라 Hard expectedType 대조는 지난다.
    auto targetB = NewPackage(target, target.OwnerAccess(), FixedId(1501), PackageKind::Cooked);
    auto targetD = NewPackage(target, target.OwnerAccess(), FixedId(1503), PackageKind::Cooked);
    REQUIRE(targetB && targetD);
    const ObjectHandle representative =
        Make<TestAsset>(target, TestAssetType(), targetB.Value().Handle(), "");
    REQUIRE(!Make<TestRequiredAsset>(target, TestRequiredType(), representative, "Gear").IsNull());
    REQUIRE(!Make<TestRequiredAsset>(target, TestRequiredType(), targetD.Value().Handle(), "")
                 .IsNull());
    TestResolver resolver;
    resolver.Add(FixedId(1501), targetB.Value().Handle());
    resolver.Add(FixedId(1503), targetD.Value().Handle());

    auto widen = [&](const Array<uint8>& file, bool expected) noexcept {
        auto read = ReadPackage(allocator, file.View(), PackageReadOptions{});
        Array<PackageImportEntry> imports = CopyEntries(allocator, read.Value().imports);
        Array<PackageDependencyEntry> dependencies =
            CopyEntries(allocator, read.Value().dependencies);
        imports[0].type = assetType;
        if (expected) {
            dependencies[0].expectedType = assetType;
        }

        return Rewrite(allocator, file, imports.View(), dependencies.View());
    };

    // 넓히지 않으면 import 대조(IsA(TestRequiredAsset, TestAsset))가 먼저 잡는다(ObjectImports).
    bool instantiated = false;
    AssetDiagnostic failure =
        InstantiateFailure(env, aFile, PackageKind::Cooked, resolver, instantiated);
    CHECK(!instantiated && failure.code == AssetDiagnosticCode::TypeMismatch &&
          failure.section == static_cast<uint32>(PackageSectionKind::ObjectImports));
    const Array<uint8> files[] = {widen(aFile, false), widen(cFile, true)};
    for (const Array<uint8>& file : files) {
        failure = InstantiateFailure(env, file, PackageKind::Cooked, resolver, instantiated);
        CHECK(!instantiated);
        CHECK(failure.code == AssetDiagnosticCode::TypeMismatch &&
              failure.error == ErrorCode::TypeMismatch &&
              failure.section == static_cast<uint32>(PackageSectionKind::Objects));
    }

    CHECK(target.Discard(target.OwnerAccess(), targetB.Value().Handle()));
    CHECK(target.Discard(target.OwnerAccess(), targetD.Value().Handle()));
}

// 쓰이지 않는 import(AS 5.7절 15): Objects가 가리키지 않고 뒤 import의 outer도 아닌 import는
// ObjectImports 손상(detail = 그 위치)이다 — 이름 있는 잎(대표 에셋만 가리키는데 B.Junk), 깊은
// 사슬 끝에 붙은 잎(Gear.Tooth 옆 Gear.Zed), 자식 없는 outer 전용 이름(Head). 에디터 읽기 전용
// 열기는 모르는 필드 안의 참조를 보지 못하므로 받는다.
TEST(AssetPackage_UnusedImportIsCorrupt) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    const ObjectAccessContext access = source.OwnerAccess();
    auto b = NewPackage(source, access, FixedId(1511), PackageKind::Editor);
    auto a = NewPackage(source, access, FixedId(1510), PackageKind::Editor);
    auto d = NewPackage(source, access, FixedId(1512), PackageKind::Editor);
    REQUIRE(a && b && d);
    const ObjectHandle bAsset = Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
    const ObjectHandle gear = Make<TestPart>(source, TestPartType(), bAsset, "Gear");
    const ObjectHandle tooth = Make<TestPart>(source, TestPartType(), gear, "Tooth");
    const ObjectHandle aAsset = Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
    const ObjectHandle dAsset = Make<TestAsset>(source, TestAssetType(), d.Value().Handle(), "");
    REQUIRE(!tooth.IsNull() && !aAsset.IsNull() && !dAsset.IsNull());
    REQUIRE(source.SetReference(access, aAsset, Pointer<TestAsset>(source, aAsset)->hard, bAsset));
    REQUIRE(WithBorrow<TestAsset>(source, dAsset, [&](TestAsset& owner) {
        return source.AddReference(access, dAsset, owner.hardList, tooth).IsOk();
    }));
    const Array<uint8> aFile = Write(allocator, source, a.Value().Handle(), EditorOptions());
    const Array<uint8> dFile = Write(allocator, source, d.Value().Handle(), EditorOptions());
    REQUIRE(!aFile.IsEmpty() && !dFile.IsEmpty());
    const AssetId bId = FixedId(1511);
    const PackageImportEntry orphan[] = {{bId, 0, "", TestAssetType()},
                                         {bId, 0, "Junk", TestPartType()}};
    CHECK(CorruptAt(PrepareOnly(env, Rewrite(allocator, aFile, orphan), PackageKind::Editor),
                    PackageSectionKind::ObjectImports, 1));
    const PackageImportEntry deepLeaf[] = {{bId, 0, "Gear", TestPartType()},
                                           {bId, 1, "Tooth", TestPartType()},
                                           {bId, 1, "Zed", TestPartType()}};
    CHECK(CorruptAt(PrepareOnly(env, Rewrite(allocator, dFile, deepLeaf), PackageKind::Editor),
                    PackageSectionKind::ObjectImports, 2));
    const PackageImportEntry childless[] = {{bId, 0, "Gear", TestPartType()},
                                            {bId, 1, "Tooth", TestPartType()},
                                            {bId, 0, "Head", TestPartType()}};
    CHECK(CorruptAt(PrepareOnly(env, Rewrite(allocator, dFile, childless), PackageKind::Editor),
                    PackageSectionKind::ObjectImports, 2));
    // 대조: 쓰이는 import만이면 받는다. 읽기 전용 열기는 쓰이지 않는 import를 받는다.
    CHECK(PrepareOnly(env, Copy(allocator, dFile.View()), PackageKind::Editor).prepared);
    PrepareResult readOnly = PrepareOnly(
        env, WithNewerTags(allocator, Rewrite(allocator, dFile, childless)), PackageKind::Editor);
    CHECK(readOnly.prepared);
}

// resolver 결과 검증(AS 5.7절 16): resolver가 준 handle이 그 에셋의 공개된 Package가 아니면 —
// 다른 AssetId의 Package, Package가 아닌 객체(대표 에셋), null, 거둔 handle — 진단 None +
// InvalidArgument이고 조용히 잇지 않는다.
TEST(AssetPackage_ResolverResultIsValidated) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    ObjectRegistry& target = *env.target;
    Allocator& allocator = env.allocator;
    const ObjectAccessContext access = source.OwnerAccess();
    auto b = NewPackage(source, access, FixedId(1521), PackageKind::Cooked);
    auto a = NewPackage(source, access, FixedId(1520), PackageKind::Cooked);
    REQUIRE(a && b);
    const ObjectHandle bAsset = Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
    const ObjectHandle aAsset = Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
    REQUIRE(source.SetReference(access, aAsset, Pointer<TestAsset>(source, aAsset)->hard, bAsset));
    const Array<uint8> aFile = Write(allocator, source, a.Value().Handle(), CookedOptions());
    REQUIRE(!aFile.IsEmpty());

    const ObjectAccessContext owner = target.OwnerAccess();
    auto other = NewPackage(target, owner, FixedId(1529), PackageKind::Cooked);
    auto real = NewPackage(target, owner, FixedId(1521), PackageKind::Cooked);
    auto gone = NewPackage(target, owner, FixedId(1521), PackageKind::Cooked);
    REQUIRE(other && real);
    const ObjectHandle otherAsset =
        Make<TestAsset>(target, TestAssetType(), other.Value().Handle(), "");
    const ObjectHandle realAsset =
        Make<TestAsset>(target, TestAssetType(), real.Value().Handle(), "");
    REQUIRE(!otherAsset.IsNull() && !realAsset.IsNull() && gone);
    const ObjectHandle stale = gone.Value().Handle();
    REQUIRE(target.Discard(owner, stale));
    gone.Value().Reset();
    REQUIRE(target.Collect(owner));
    FixedResolver resolver;
    const ObjectHandle wrong[] = {other.Value().Handle(), realAsset, ObjectHandle{}, stale};
    for (ObjectHandle handle : wrong) {
        resolver.handle = handle;
        bool instantiated = false;
        const AssetDiagnostic failure =
            InstantiateFailure(env, aFile, PackageKind::Cooked, resolver, instantiated);
        CHECK(!instantiated);
        CHECK(failure.code == AssetDiagnosticCode::None &&
              failure.error == ErrorCode::InvalidArgument &&
              failure.section == static_cast<uint32>(PackageSectionKind::ObjectImports) &&
              failure.related == FixedId(1521));
    }

    resolver.handle = real.Value().Handle();
    bool instantiated = false;
    (void)InstantiateFailure(env, aFile, PackageKind::Cooked, resolver, instantiated);
    CHECK(instantiated);
}

// Prepare 스레드 계약(AS 5.7절 16): worker의 Prepare는 상태 allocator만 쓰고 Registry
// allocator(소유 스레드 전용)는 쓰지 않는다. Registry allocator는 Instantiate(소유 스레드)에서만
// 쓰이고, 압축을 푼 bulk는 복사 없이 Package가 이어받아 공개 뒤에도 읽힌다.
TEST(AssetPackage_PrepareUsesOnlyStateAllocator) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    auto package = NewPackage(source, source.OwnerAccess(), FixedId(1530), PackageKind::Cooked);
    REQUIRE(package);
    const ObjectHandle asset =
        Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "");
    REQUIRE(!asset.IsNull());
    Array<uint8> data(allocator);
    FillCompressible(data, 300000, 8);
    TestBulkSource bulk;
    Pointer<TestAsset>(source, asset)->pixels = bulk.Add(1, data.View());
    PackageWriteOptions options = CookedOptions();
    options.bulkSource = &bulk;
    options.bulkCodec = BulkCodec::Zstd;
    Array<uint8> file = Write(allocator, source, package.Value().Handle(), options);
    REQUIRE(!file.IsEmpty());

    ThreadRecordingAllocator recording(allocator);
    RegistryOptions registryOptions;
    registryOptions.names = &env.names;
    registryOptions.policy.packageNativeType = NativeTypeKey<Package>();
    registryOptions.policy.rejectWorldOnly = true;
    auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, recording, env.types,
                                                   ContextId{0x4153534554ull, 15}, registryOptions);
    REQUIRE(made);
    ObjectRegistry& target = *made.Value();
    PackageLoadState state(allocator, target, env.types, target.OwnerAccess(),
                           LoadOptions(PackageKind::Cooked));
    struct Job {
        PackageLoadState* state;
        Array<uint8>* file;
        bool prepared = false;

        void operator()() noexcept {
            prepared = state->Prepare(Move(*file)).IsOk();
        }
    } job{&state, &file};
    {
        platform::Thread thread;
        auto entry = [](void* context) noexcept { (*static_cast<Job*>(context))(); };
        REQUIRE(thread.Start(entry, &job) == platform::ThreadStatus::Success);
        REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    }

    REQUIRE(job.prepared);
    CHECK(recording.foreignCalls == 0);
    const uint64 before = recording.ownerCalls;
    ConstructionContext transaction(target, target.OwnerAccess());
    PackageLoadState* states[1] = {&state};
    ObjectHandle handles[1];
    RootRef<Object> roots[1];
    REQUIRE(InstantiatePackages(transaction, states, nullptr, handles));
    REQUIRE(transaction.Commit(handles, roots));
    CHECK(recording.ownerCalls > before);
    CHECK(recording.foreignCalls == 0);
    auto loaded = target.FindObject(target.OwnerAccess(), handles[0], StringView());
    REQUIRE(loaded);
    Package* pointer = Pointer<Package>(target, handles[0]);
    REQUIRE(pointer != nullptr);
    auto bytes = pointer->BulkBytes(Pointer<TestAsset>(target, loaded.Value())->pixels);
    REQUIRE(bytes);
    CHECK(SameBytes(bytes.Value(), data.View()));
    CHECK(target.Discard(target.OwnerAccess(), handles[0]));
    roots[0].Reset();
}

// 에디터 읽기 전용 열기를 끝까지(AS 5.3절, 5.7절 18): 읽기 전용 원인 넷 — 모르는 필드(Hard import
// 참조·Soft 참조·bulk ref가 그 안에 있다), 더 새 타입 버전(노드 Record와 Summary 타입 표), 모르는
// 커스텀 버전, Required가 아닌 모르는 섹션 — 마다 Prepare가 읽기 전용으로 받고(Dependencies는
// 부분집합, 쓰이지 않는 import·bulk entry는 검사하지 않는다) Instantiate·Commit까지 간다. 모르는
// 필드는 건너뛰어 기본값(null·빈 값)이고 아는 필드는 그대로이며, Package는 bulk entry를 그대로
// 가진다. 미등록 export 타입(5.7절 9)은 reader가 읽기 전용으로 표시하지만 객체를 만들 수 없어
// Prepare가 reader 판정으로 Summary UnsupportedVersion이다(5.7절 18).
TEST(AssetPackage_EditorReadOnlyLoadsEndToEnd) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    ObjectRegistry& target = *env.target;
    Allocator& allocator = env.allocator;
    const ObjectAccessContext access = source.OwnerAccess();
    auto b = NewPackage(source, access, FixedId(1541), PackageKind::Editor);
    auto a = NewPackage(source, access, FixedId(1540), PackageKind::Editor);
    REQUIRE(a && b);
    const ObjectHandle bAsset = Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
    const ObjectHandle aAsset = Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
    REQUIRE(!bAsset.IsNull() && !aAsset.IsNull());
    TestAsset* pointer = Pointer<TestAsset>(source, aAsset);
    Array<uint8> data(allocator);
    FillIncompressible(data, 500, 9);
    TestBulkSource bulk;
    pointer->pixels = bulk.Add(1, data.View());
    pointer->value = 41;
    REQUIRE(pointer->soft.TryAssign(FixedId(1542), "Sub"));
    REQUIRE(source.SetReference(access, aAsset, pointer->hard, bAsset));
    PackageWriteOptions options = EditorOptions();
    options.bulkSource = &bulk;
    const Array<uint8> file = Write(allocator, source, a.Value().Handle(), options);
    const Array<uint8> bFile = Write(allocator, source, b.Value().Handle(), EditorOptions());
    // 현재 엔진이 모르는 커스텀 버전 id로 쓴 같은 패키지다(로드 옵션에는 커스텀 버전이 없다).
    const PackageCustomVersion unknownCustom[] = {{Guid{0x5e57, 0xC0}, 1}};
    options.customVersions = unknownCustom;
    const Array<uint8> customFile = Write(allocator, source, a.Value().Handle(), options);
    REQUIRE(!file.IsEmpty() && !bFile.IsEmpty() && !customFile.IsEmpty());

    // 원인 넷의 파일이다(순서: 모르는 필드, 더 새 타입 버전, 모르는 커스텀 버전, 모르는 섹션).
    constexpr usize CauseCount = 4;
    Array<uint8> causes[CauseCount] = {Copy(allocator, file.View()), Array<uint8>(allocator),
                                       Copy(allocator, customFile.View()), Array<uint8>(allocator)};
    const char* fields[] = {"hard", "soft", "pixels"};
    for (const char* name : fields) {
        causes[0] = WithUnknownField(allocator, causes[0],
                                     FieldIdOf(env.types, TestAssetType(), StringView(name)));
        REQUIRE(!causes[0].IsEmpty());
    }

    {
        auto lease = env.types.Acquire(TestAssetType());
        REQUIRE(lease);
        causes[1] =
            WithNewerTypeVersion(allocator, file, TestAssetType(), lease.Value().Schema().version);
    }

    {
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(file.View()));
        const uint8 unknownBytes[4] = {1, 2, 3, 4};
        REQUIRE(edit.Insert(PackageKnownSectionKinds + 1, 0, Span<const uint8>(unknownBytes, 4)) !=
                nullptr);
        causes[3] = edit.Build();
    }

    REQUIRE(!causes[1].IsEmpty() && !causes[3].IsEmpty());
    Array<uint8> dependency[1] = {Copy(allocator, bFile.View())};
    RootRef<Object> bRoot[1];
    ObjectHandle bHandle[1];
    REQUIRE(LoadAll(env, target, dependency, PackageKind::Editor, nullptr, bRoot, bHandle));
    const ObjectHandle loadedB = RepresentativeOf(target, bHandle[0]);
    REQUIRE(!loadedB.IsNull());
    TestResolver resolver;
    resolver.Add(FixedId(1541), bHandle[0]);
    for (usize cause = 0; cause < CauseCount; ++cause) {
        PackageLoadState state(allocator, target, env.types, target.OwnerAccess(),
                               LoadOptions(PackageKind::Editor));
        AssetDiagnostic failure;
        REQUIRE(state.Prepare(Move(causes[cause]), &failure));
        CHECK(state.IsReadOnly());
        ConstructionContext transaction(target, target.OwnerAccess());
        PackageLoadState* states[1] = {&state};
        ObjectHandle handles[1];
        RootRef<Object> roots[1];
        REQUIRE(InstantiatePackages(transaction, states, &resolver, handles, &failure));
        REQUIRE(transaction.Commit(handles, roots));
        auto representative = target.FindObject(target.OwnerAccess(), handles[0], StringView());
        REQUIRE(representative);
        TestAsset* loaded = Pointer<TestAsset>(target, representative.Value());
        REQUIRE(loaded != nullptr);
        CHECK(loaded->value == 41);
        // 모르는 필드 안의 참조만 기본값이 되고, 다른 원인은 아는 필드를 그대로 잇는다.
        const bool dropped = cause == 0;
        CHECK((loaded->hard.Handle() == loadedB) != dropped);
        CHECK(loaded->soft.IsNull() == dropped);
        CHECK(loaded->pixels.IsEmpty() == dropped);
        CHECK(Pointer<Package>(target, handles[0])->BulkCount() == 1);
        CHECK(target.Discard(target.OwnerAccess(), handles[0]));
        roots[0].Reset();
    }

    // 미등록 export 타입(에디터).
    auto read = ReadPackage(allocator, bFile.View(), [] {
        PackageReadOptions editor;
        editor.expectedKind = PackageKind::Editor;
        return editor;
    }());
    REQUIRE(read);
    Array<PackageExportEntry> exports = CopyEntries(allocator, read.Value().exports);
    const TypeId unknown{0x7E57000000000001ull, 0x1234};
    exports[0].type = unknown;
    const PackageTypeVersion types[] = {{unknown, read.Value().summary.types[0].version}};
    EditablePackage edit(allocator);
    REQUIRE(edit.Parse(bFile.View()));
    Array<uint8> exportBytes = ExportsBytes(allocator, exports.View(), 1);
    Array<uint8> summary =
        SummaryBytes(allocator, SummaryInput{types, {}, true}, SummaryFault::None);
    REQUIRE(edit.Replace(PackageSectionKind::Exports, exportBytes.View()));
    REQUIRE(edit.Replace(PackageSectionKind::Summary, summary.View()));
    const Array<uint8> unregistered = edit.Build();
    PackageReadOptions withTypes;
    withTypes.expectedKind = PackageKind::Editor;
    withTypes.types = &env.types;
    auto opened = ReadPackage(allocator, unregistered.View(), withTypes);
    REQUIRE(opened);
    CHECK(opened.Value().readOnly);
    const PrepareResult prepared =
        PrepareOnly(env, Copy(allocator, unregistered.View()), PackageKind::Editor);
    CHECK(!prepared.prepared && prepared.failure.code == AssetDiagnosticCode::UnsupportedVersion &&
          prepared.failure.error == ErrorCode::Unsupported &&
          prepared.failure.section == static_cast<uint32>(PackageSectionKind::Summary));
    CHECK(target.Discard(target.OwnerAccess(), bHandle[0]));
    bRoot[0].Reset();
}

// export 이름 규칙(AS 5.7절 3): Exports 표 규칙(byte 순서)을 지나는 이름 규칙 위반 — 제어 문자,
// '.', 예약 장치 이름, 65 byte, 대소문자만 다른 형제("AB"·"Ab") — 은 그래프 1단계가 거부해
// Prepare가 CorruptPackage(section Objects, error InvalidArgument)다.
TEST(AssetPackage_ExportNameRulesOnLoad) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    auto package = NewPackage(source, source.OwnerAccess(), FixedId(1550), PackageKind::Cooked);
    REQUIRE(package);
    const ObjectHandle asset =
        Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "");
    REQUIRE(!Make<TestPart>(source, TestPartType(), asset, "Pa").IsNull());
    REQUIRE(!Make<TestPart>(source, TestPartType(), asset, "Pb").IsNull());
    const Array<uint8> file = Write(allocator, source, package.Value().Handle(), CookedOptions());
    REQUIRE(!file.IsEmpty());
    auto read = ReadPackage(allocator, file.View(), PackageReadOptions{});
    REQUIRE(read);
    char longName[MaxNameComponentBytes + 1];
    for (char& value : longName) {
        value = 'A';
    }

    struct Names {
        StringView first;
        StringView second;
    };

    const Names cases[] = {{StringView("A\x01"), StringView("Pb")},
                           {StringView("A.B"), StringView("Pb")},
                           {StringView("CON"), StringView("Pb")},
                           {StringView(longName, sizeof(longName)), StringView("Pb")},
                           {StringView("AB"), StringView("Ab")}};
    for (const Names& names : cases) {
        Array<PackageExportEntry> exports = CopyEntries(allocator, read.Value().exports);
        exports[1].name = names.first;
        exports[2].name = names.second;
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(file.View()));
        Array<uint8> bytes = ExportsBytes(allocator, exports.View(), 3);
        REQUIRE(edit.Replace(PackageSectionKind::Exports, bytes.View()));
        // 표 규칙은 지난다.
        CHECK(ReadPackage(allocator, edit.Build().View(), PackageReadOptions{}));
        const PrepareResult result = PrepareOnly(env, edit.Build(), PackageKind::Cooked);
        CHECK(!result.prepared && result.failure.code == AssetDiagnosticCode::CorruptPackage &&
              result.failure.error == ErrorCode::InvalidArgument &&
              result.failure.section == static_cast<uint32>(PackageSectionKind::Objects));
    }
}

// Zstd bulk 블록 압축(AS 5.5절 3, 5.7절 6): 압축되지 않는 entry를 쓸 때 writer allocator의 살아
// 있는 byte 최댓값이 entry 크기(4 MiB·16 MiB)와 무관하고 entry의 1/4보다 작다(entry 전체 압축
// 버퍼를 잡지 않는다). 저장 byte는 블록 컨테이너이고 읽어 풀면 원본이다. 0 byte entry는 None이다.
TEST(AssetPackage_BulkBlockCompressionPeak) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    auto package = NewPackage(source, source.OwnerAccess(), FixedId(1560), PackageKind::Cooked);
    REQUIRE(package);
    const ObjectHandle asset =
        Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "");
    const ObjectHandle part = Make<TestPart>(source, TestPartType(), asset, "Part");
    REQUIRE(!part.IsNull());
    // 같은 장면을 entry 크기 4 MiB와 16 MiB로 쓴다. 블록 단위면 피크는 entry 크기와 무관하다(Zstd
    // 문맥 + 블록 하나의 출력) — entry 전체를 압축하면 피크가 entry 크기를 따라 커진다.
    constexpr usize SmallBytes = usize{4} * 1024 * 1024;
    constexpr usize RawBytes = 4 * SmallBytes;
    constexpr usize BlockBytes = usize{1} << PackageBulkBlockSizeLog2;
    Array<uint8> data(allocator);
    FillIncompressible(data, RawBytes, 10);
    auto peakOf = [&](usize bytes, Array<uint8>& file) noexcept {
        TestBulkSource bulk;
        Pointer<TestAsset>(source, asset)->pixels = bulk.Add(1, data.View().Slice(0, bytes));
        Pointer<TestPart>(source, part)->data = bulk.Add(2, Span<const uint8>());
        PackageWriteOptions options = CookedOptions();
        options.bulkSource = &bulk;
        options.bulkCodec = BulkCodec::Zstd;
        HighWaterAllocator watched(allocator);
        MemoryPackageSink sink(allocator);
        const bool written = WritePackage(watched, source, source.OwnerAccess(),
                                          package.Value().Handle(), options, sink)
                                 .IsOk();
        file = sink.Take();
        return written ? watched.peak : NumericLimits<uint64>::Max();
    };

    Array<uint8> smallFile(allocator);
    Array<uint8> file(allocator);
    const uint64 smallPeak = peakOf(SmallBytes, smallFile);
    const uint64 peak = peakOf(RawBytes, file);
    REQUIRE(peak != NumericLimits<uint64>::Max());
    CHECK(peak <= smallPeak + BlockBytes);
    CHECK(peak < RawBytes / 4);
    auto read = ReadPackage(allocator, file.View(), PackageReadOptions{});
    REQUIRE(read);
    REQUIRE(read.Value().bulkEntries.Size() == 2);
    CHECK(read.Value().bulkEntries[0].codec == BulkCodec::Zstd);
    CHECK(read.Value().bulkEntries[1].codec == BulkCodec::None);
    Array<uint8> out(allocator);
    Array<PackageBulkSlot> slots(allocator);
    REQUIRE(DecodePackageBulk(read.Value(), out, slots, NumericLimits<uint64>::Max()));
    CHECK(SameBytes(out.View().Slice(0, RawBytes), data.View()));
}

// Tags 키 정렬(AS 5.2절): Tags Record의 필드 ID(키)는 기록 순서로 엄격 오름차순이다 — 뒤집힌 순서는
// 쓰기 옵션 InvalidArgument, 그런 섹션을 담은 파일은 Tags 손상이다. 정렬된 Tags는 쓰고 읽힌다.
TEST(AssetPackage_TagsKeysAreSorted) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    auto package = NewPackage(source, source.OwnerAccess(), FixedId(1570), PackageKind::Editor);
    REQUIRE(package);
    REQUIRE(!Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "").IsNull());
    const uint8 value[4] = {1, 2, 3, 4};
    auto tags = [&](Guid first, Guid second) noexcept {
        RecordBuilder record(allocator, Guid{0x7A, 0x6B}, 1);
        (void)record.AddField(first, WireType::U32, value);
        (void)record.AddField(second, WireType::U32, value);
        auto built = record.Build();
        return built ? Move(built.Value()) : Array<uint8>(allocator);
    };

    const Array<uint8> sorted = tags(Guid{1, 1}, Guid{1, 2});
    const Array<uint8> reversed = tags(Guid{1, 2}, Guid{1, 1});
    PackageWriteOptions options = EditorOptions();
    options.tags = reversed.View();
    MemoryPackageSink rejected(allocator);
    CHECK(WritePackage(allocator, source, source.OwnerAccess(), package.Value().Handle(), options,
                       rejected)
              .Error() == ErrorCode::InvalidArgument);
    options.tags = sorted.View();
    const Array<uint8> file = Write(allocator, source, package.Value().Handle(), options);
    REQUIRE(!file.IsEmpty());
    PackageReadOptions editor;
    editor.expectedKind = PackageKind::Editor;
    CHECK(ReadPackage(allocator, file.View(), editor));
    EditablePackage edit(allocator);
    REQUIRE(edit.Parse(file.View()));
    REQUIRE(edit.Replace(PackageSectionKind::Tags, reversed.View()));
    AssetDiagnostic failure;
    CHECK(!ReadPackage(allocator, edit.Build().View(), editor, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.section == static_cast<uint32>(PackageSectionKind::Tags));

    // 몸이 잘린 Record(헤더 검사가 막는다): 쓰기 InvalidArgument, 읽기 Tags 손상.
    Array<uint8> broken = Copy(allocator, sorted.View());
    REQUIRE(broken.TryResize(broken.Size() - 1));
    options.tags = broken.View();
    MemoryPackageSink truncated(allocator);
    CHECK(WritePackage(allocator, source, source.OwnerAccess(), package.Value().Handle(), options,
                       truncated)
              .Error() == ErrorCode::InvalidArgument);
    REQUIRE(edit.Replace(PackageSectionKind::Tags, broken.View()));
    failure = AssetDiagnostic{};
    CHECK(!ReadPackage(allocator, edit.Build().View(), editor, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.section == static_cast<uint32>(PackageSectionKind::Tags));

    // 같은 키 둘(헤더는 맞고 필드 표 열기가 중복 id로 실패): 쓰기 InvalidArgument, 읽기 Tags 손상.
    // RecordBuilder는 중복 id를 받지 않으므로 정렬된 Record의 둘째 키 byte를 첫째 키로 바꾼다.
    Array<uint8> duplicate = Copy(allocator, sorted.View());
    {
        uint8 second[16];
        StoreLittleEndian<uint64>(second, 1);
        StoreLittleEndian<uint64>(second + 8, 2);
        usize found = 0;
        usize at = 0;
        for (usize index = 0; index + sizeof(second) <= duplicate.Size(); ++index) {
            bool same = true;
            for (usize offset = 0; same && offset < sizeof(second); ++offset) {
                same = duplicate[index + offset] == second[offset];
            }

            if (same) {
                ++found;
                at = index;
            }
        }

        REQUIRE(found == 1);
        duplicate[at + 8] = 1;
    }

    options.tags = duplicate.View();
    MemoryPackageSink duplicated(allocator);
    CHECK(WritePackage(allocator, source, source.OwnerAccess(), package.Value().Handle(), options,
                       duplicated)
              .Error() == ErrorCode::InvalidArgument);
    REQUIRE(edit.Replace(PackageSectionKind::Tags, duplicate.View()));
    failure = AssetDiagnostic{};
    CHECK(!ReadPackage(allocator, edit.Build().View(), editor, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.section == static_cast<uint32>(PackageSectionKind::Tags));

    // Tags 검사의 할당 실패는 쓰기·읽기 모두 OutOfMemory(진단 None)다 — 모든 할당 지점.
    options.tags = sorted.View();
    test::NFailAllocator failing(allocator);
    uint32 writeFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        MemoryPackageSink sink(allocator);
        AssetDiagnostic written;
        auto result = WritePackage(failing, source, source.OwnerAccess(), package.Value().Handle(),
                                   options, sink, &written);
        if (failing.Attempts() < attempt) {
            REQUIRE(result);
            break;
        }

        CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
        CHECK(written.code == AssetDiagnosticCode::None);
        ++writeFailures;
    }

    CHECK(writeFailures > 0);
    uint32 readFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        AssetDiagnostic read;
        auto result = ReadPackage(failing, file.View(), editor, &read);
        if (failing.Attempts() < attempt) {
            REQUIRE(result);
            break;
        }

        CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
        CHECK(read.code == AssetDiagnosticCode::None);
        ++readFailures;
    }

    CHECK(readFailures > 0);
}

// 종료된 TypeRegistry(AS 5.3절 타입 축): 타입 조회의 ShuttingDown은 파일 판정(미등록 →
// UnsupportedVersion·읽기 전용)이 아니라 그 오류 그대로다(진단 None).
TEST(AssetPackage_ClosedTypeRegistryIsNotVersionVerdict) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    TypeRegistry closed(allocator);
    REQUIRE(closed.Shutdown());
    const PackageKind kinds[] = {PackageKind::Cooked, PackageKind::Editor};
    uint64 id = 1580;
    for (PackageKind kind : kinds) {
        auto package = NewPackage(source, source.OwnerAccess(), FixedId(id++), kind);
        REQUIRE(package);
        REQUIRE(!Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "").IsNull());
        const PackageWriteOptions options =
            kind == PackageKind::Cooked ? CookedOptions() : EditorOptions();
        const Array<uint8> bytes = Write(allocator, source, package.Value().Handle(), options);
        REQUIRE(!bytes.IsEmpty());
        PackageReadOptions read;
        read.expectedKind = kind;
        read.types = &closed;
        AssetDiagnostic failure;
        auto result = ReadPackage(allocator, bytes.View(), read, &failure);
        CHECK(!result && result.Error() == ErrorCode::ShuttingDown);
        CHECK(failure.code == AssetDiagnosticCode::None);
    }
}

// 클래스가 아닌 타입 id(AS 5.3절 타입 축, 5.7절 18): export 타입과 Summary 타입 표가 struct id면
// 등록 저장소 조회가 TypeMismatch이고 "현재 버전 없음"이다 — 쿠킹 reader는 UnsupportedVersion
// (Summary), 에디터 reader는 읽기 전용으로 받되 unavailableExportType을 남기고, Prepare는 두 종류
// 모두 Summary UnsupportedVersion(Unsupported)이다(그래프 1단계의 TypeMismatch → CorruptPackage가
// 아니다).
TEST(AssetPackage_NonClassTypeHasNoCurrentVersion) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    const TypeId structType = GeneratedType<BulkDataRef>::Id();
    const PackageKind kinds[] = {PackageKind::Cooked, PackageKind::Editor};
    uint64 id = 1590;
    for (PackageKind kind : kinds) {
        const bool cooked = kind == PackageKind::Cooked;
        auto package = NewPackage(source, source.OwnerAccess(), FixedId(id++), kind);
        REQUIRE(package);
        REQUIRE(!Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "").IsNull());
        const Array<uint8> file = Write(allocator, source, package.Value().Handle(),
                                        cooked ? CookedOptions() : EditorOptions());
        REQUIRE(!file.IsEmpty());
        const PackageExportEntry exports[] = {
            {StringView(), structType, 0, static_cast<uint32>(PackageExportFlags::Primary)}};
        const PackageTypeVersion types[] = {{structType, 1}};
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(file.View()));
        Array<uint8> exportBytes = ExportsBytes(allocator, exports, 1);
        Array<uint8> summary =
            SummaryBytes(allocator, SummaryInput{types, {}, !cooked}, SummaryFault::None);
        REQUIRE(!summary.IsEmpty());
        REQUIRE(edit.Replace(PackageSectionKind::Exports, exportBytes.View()));
        REQUIRE(edit.Replace(PackageSectionKind::Summary, summary.View()));
        const Array<uint8> changed = edit.Build();
        PackageReadOptions read;
        read.expectedKind = kind;
        read.types = &env.types;
        AssetDiagnostic failure;
        auto result = ReadPackage(allocator, changed.View(), read, &failure);
        if (cooked) {
            CHECK(!result && result.Error() == ErrorCode::Unsupported);
            CHECK(failure.code == AssetDiagnosticCode::UnsupportedVersion &&
                  failure.section == static_cast<uint32>(PackageSectionKind::Summary));
        } else {
            REQUIRE(result);
            CHECK(result.Value().readOnly && result.Value().unavailableExportType);
        }

        const PrepareResult prepared = PrepareOnly(env, Copy(allocator, changed.View()), kind);
        CHECK(!prepared.prepared &&
              prepared.failure.code == AssetDiagnosticCode::UnsupportedVersion &&
              prepared.failure.error == ErrorCode::Unsupported &&
              prepared.failure.section == static_cast<uint32>(PackageSectionKind::Summary));
    }
}

// 패키지 정책이 Package가 아닌 Registry(AS 5.7절 19): 패키지는 packageNativeType = Package인
// Registry에만 로드한다 — 정책 없는 Registry와 다른 타입(TestPart)을 패키지 타입으로 둔 Registry로
// 연 트랜잭션은 InstantiatePackages 인자 InvalidArgument(진단 없음)이고 객체를 만들지 않는다. 처음
// 구현은 그래프 생성 단계까지 가서 Objects CorruptPackage로 거부했다(파일 손상이 아니다). 판정
// 리뷰가 든 "RequestDestroy만 받은 import 대상" 경로(정책 없는 Registry에서만 생긴다)는 이 검사가
// 먼저 막는다.
TEST(AssetPackage_LoadRequiresPackagePolicyRegistry) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    auto package = NewPackage(source, source.OwnerAccess(), FixedId(1600), PackageKind::Cooked);
    REQUIRE(package);
    REQUIRE(!Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "").IsNull());
    const Array<uint8> file = Write(allocator, source, package.Value().Handle(), CookedOptions());
    REQUIRE(!file.IsEmpty());
    // 정책 없음, 다른 타입을 패키지 타입으로 둔 정책.
    const void* const packageTypes[] = {nullptr, NativeTypeKey<TestPart>()};
    uint64 instance = 40;
    for (const void* packageType : packageTypes) {
        RegistryOptions options;
        options.names = &env.names;
        options.policy.packageNativeType = packageType;
        auto made = UniquePtr<ObjectRegistry>::TryMake(
            allocator, allocator, env.types, ContextId{0x4153534554ull, instance++}, options);
        REQUIRE(made);
        ObjectRegistry& target = *made.Value();
        const ObjectAccessContext owner = target.OwnerAccess();
        PackageLoadState state(allocator, target, env.types, owner,
                               LoadOptions(PackageKind::Cooked));
        AssetDiagnostic failure;
        REQUIRE(state.Prepare(Copy(allocator, file.View()), &failure));
        const uint64 before = target.Stats().liveObjects;
        ConstructionContext transaction(target, owner);
        PackageLoadState* states[1] = {&state};
        ObjectHandle handles[1];
        CHECK(InstantiatePackages(transaction, states, nullptr, handles, &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::None);
        transaction.Rollback();
        CHECK(target.Stats().liveObjects == before);
    }
}

namespace {

// n번째 할당에서 TypeRegistry를 닫는(Shutdown) allocator다. 할당은 inner에 맡긴다. Prepare가 쥔
// lease가 있으면 Shutdown은 Busy지만 closed_를 먼저 세우므로 이후 조회는 ShuttingDown이다.
struct ClosingAllocator final : Allocator {
    explicit ClosingAllocator(Allocator& innerIn, TypeRegistry& typesIn, uint64 closeAtIn) noexcept
        : inner(innerIn), types(typesIn), closeAt(closeAtIn) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++attempts;
        if (attempts == closeAt) {
            (void)types.Shutdown();
            closed = true;
        }

        return inner.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner.Deallocate(address);
    }

    Allocator& inner;
    TypeRegistry& types;
    uint64 closeAt = 0;
    uint64 attempts = 0;
    bool closed = false;
};

} // namespace

// Prepare 도중 닫히는 TypeRegistry(AS 5.3절, 5.7절 19): Prepare의 상태 allocator가 n번째
// 할당에서 TypeRegistry::Shutdown을 부르게 해(모든 n, 쿠킹·에디터) 결과가 성공이거나 진단 None의
// ShuttingDown임을 본다 — 단언에 걸리거나 손상·버전 판정으로 바뀌지 않는다. 대표 에셋 lease를 잡는
// 지점(section Exports, detail 1)에도 닿는다. TypeRegistry는 n마다 새로 만든다(닫히면 다시 열 수
// 없다).
TEST(AssetPackage_TypeRegistryClosesDuringPrepare) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    const PackageKind kinds[] = {PackageKind::Cooked, PackageKind::Editor};
    uint64 id = 1610;
    for (PackageKind kind : kinds) {
        auto package = NewPackage(source, source.OwnerAccess(), FixedId(id++), kind);
        REQUIRE(package);
        const ObjectHandle asset =
            Make<TestAsset>(source, TestAssetType(), package.Value().Handle(), "");
        REQUIRE(!asset.IsNull() && !Make<TestPart>(source, TestPartType(), asset, "Part").IsNull());
        const Array<uint8> file =
            Write(allocator, source, package.Value().Handle(),
                  kind == PackageKind::Cooked ? CookedOptions() : EditorOptions());
        REQUIRE(!file.IsEmpty());
        uint32 closings = 0;
        uint32 atRepresentative = 0;
        for (uint64 attempt = 1;; ++attempt) {
            // 그래프 Stage는 같은 TypeRegistry를 쓰는 Registry를 요구하므로 Registry도 n마다
            // 만든다.
            TypeRegistry types(allocator);
            ModuleRegistration modules(types, allocator);
            REQUIRE(RegisterAssetTestTypes(modules));
            RegistryOptions options;
            options.names = &env.names;
            options.policy.packageNativeType = NativeTypeKey<Package>();
            options.policy.rejectWorldOnly = true;
            auto made = UniquePtr<ObjectRegistry>::TryMake(allocator, allocator, types,
                                                           ContextId{0x4153534554ull, 50}, options);
            REQUIRE(made);
            ObjectRegistry& target = *made.Value();
            ClosingAllocator closing(allocator, types, attempt);
            PackageLoadState state(closing, target, types, target.OwnerAccess(), LoadOptions(kind));
            AssetDiagnostic failure;
            auto result = state.Prepare(Copy(allocator, file.View()), &failure);
            if (!closing.closed) {
                REQUIRE(result);
                break;
            }

            ++closings;
            CHECK(result || (result.Error() == ErrorCode::ShuttingDown &&
                             failure.code == AssetDiagnosticCode::None));
            if (!result && failure.section == static_cast<uint32>(PackageSectionKind::Exports) &&
                failure.detail == 1) {
                ++atRepresentative;
            }
        }

        CHECK(closings > 0);
        CHECK(atRepresentative > 0);
    }
}

namespace {

// 로컬 TypeRegistry를 쓰는 패키지 정책 Registry다(TypeRegistry를 닫는 시험 — 그래프 Stage는 같은
// TypeRegistry를 쓰는 Registry를 요구한다).
UniquePtr<ObjectRegistry> MakeClosableRegistry(AssetEnv& env, TypeRegistry& types,
                                               uint64 instance) noexcept {
    RegistryOptions options;
    options.names = &env.names;
    options.policy.packageNativeType = NativeTypeKey<Package>();
    options.policy.rejectWorldOnly = true;
    auto made = UniquePtr<ObjectRegistry>::TryMake(env.allocator, env.allocator, types,
                                                   ContextId{0x4153534554ull, instance}, options);
    return made ? Move(made.Value()) : UniquePtr<ObjectRegistry>();
}

// files를 registry에 한 트랜잭션으로 로드한다(상태 allocator는 allocator — LoadAll과 같다).
Result<void> LoadWith(Allocator& allocator, ObjectRegistry& registry, TypeRegistry& types,
                      const Array<uint8>* const* files, usize count, PackageKind kind,
                      PackageImportResolver* resolver, Span<RootRef<Object>> roots,
                      Span<ObjectHandle> handles) noexcept {
    constexpr usize MaxFiles = 2;
    UniquePtr<PackageLoadState> states[MaxFiles];
    PackageLoadState* pointers[MaxFiles] = {};
    if (count > MaxFiles) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < count; ++index) {
        auto made = UniquePtr<PackageLoadState>::TryMake(allocator, allocator, registry, types,
                                                         registry.OwnerAccess(), LoadOptions(kind));
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        states[index] = Move(made.Value());
        pointers[index] = states[index].Get();
        if (auto prepared = states[index]->Prepare(Copy(allocator, files[index]->View()));
            !prepared) {
            return prepared;
        }
    }

    ConstructionContext transaction(registry, registry.OwnerAccess());
    if (auto made = InstantiatePackages(transaction, Span<PackageLoadState* const>(pointers, count),
                                        resolver, handles);
        !made) {
        return made;
    }

    return transaction.Commit(handles, roots);
}

} // namespace

// InstantiatePackages 도중 닫히는 TypeRegistry(AS 5.3절, 5.7절 19): 상태
// allocator(ClosingAllocator)가 Prepare 뒤 n번째 할당에서 TypeRegistry::Shutdown을 부르게 해(모든 n
// — 첫 할당은 import 해석 전의 targets 예약) 결과가 성공이거나 진단 None의 ShuttingDown임을 본다.
// Shutdown은 lease가 남아 Busy여도 closed_를 먼저 세우므로 찾은 import 대상의 AcquireType(lease
// 복제)이 ShuttingDown이다. 두 장면이 로드의 두 조회 지점에 닿는다:
// - resolver 장면(쿠킹): A가 공개된 B의 대표 에셋을 Hard로 가리킨다 — Hard 의존의 대표 에셋 대조.
// - 묶음 장면(에디터): A가 같은 묶음의 B 대표 에셋을 EditorOnly 참조로만 가리킨다 — expectedType이
//   없어 대조를 건너뛰고 import 대상 조회에 닿는다.
TEST(AssetPackage_TypeRegistryClosesDuringInstantiate) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& source = *env.source;
    Allocator& allocator = env.allocator;
    const ObjectAccessContext access = source.OwnerAccess();
    // 장면: 0 resolver(쿠킹, Hard), 1 묶음(에디터, EditorOnly).
    constexpr usize SceneCount = 2;
    for (usize scene = 0; scene < SceneCount; ++scene) {
        const bool bundle = scene == 1;
        const PackageKind kind = bundle ? PackageKind::Editor : PackageKind::Cooked;
        auto b = NewPackage(source, access, FixedId(1631 + 2 * scene), kind);
        auto a = NewPackage(source, access, FixedId(1630 + 2 * scene), kind);
        REQUIRE(a && b);
        const ObjectHandle bAsset =
            Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
        const ObjectHandle aAsset =
            Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
        REQUIRE(!bAsset.IsNull() && !aAsset.IsNull());
        TestAsset* pointer = Pointer<TestAsset>(source, aAsset);
        REQUIRE(pointer != nullptr);
        REQUIRE(source.SetReference(access, aAsset, bundle ? pointer->editorRef : pointer->hard,
                                    bAsset));
        const PackageWriteOptions options = bundle ? EditorOptions() : CookedOptions();
        const Array<uint8> aFile = Write(allocator, source, a.Value().Handle(), options);
        const Array<uint8> bFile = Write(allocator, source, b.Value().Handle(), options);
        REQUIRE(!aFile.IsEmpty() && !bFile.IsEmpty());
        uint32 closings = 0;
        uint32 atImports = 0;
        for (uint64 attempt = 1;; ++attempt) {
            TypeRegistry types(allocator);
            ModuleRegistration modules(types, allocator);
            REQUIRE(RegisterAssetTestTypes(modules));
            UniquePtr<ObjectRegistry> made = MakeClosableRegistry(env, types, 60 + scene);
            REQUIRE(made.Get() != nullptr);
            ObjectRegistry& target = *made;
            const ObjectAccessContext owner = target.OwnerAccess();
            // resolver 장면은 B를 먼저 공개한다.
            RootRef<Object> bRoot[1];
            ObjectHandle bHandle[1];
            TestResolver resolver;
            if (!bundle) {
                const Array<uint8>* bOnly[1] = {&bFile};
                REQUIRE(
                    LoadWith(allocator, target, types, bOnly, 1, kind, nullptr, bRoot, bHandle));
                resolver.Add(FixedId(1631), bHandle[0]);
            }

            // Prepare까지는 닫지 않고, Instantiate의 n번째 상태 할당에서 닫는다.
            ClosingAllocator closing(allocator, types, 0);
            PackageLoadState aState(closing, target, types, owner, LoadOptions(kind));
            PackageLoadState bState(closing, target, types, owner, LoadOptions(kind));
            REQUIRE(aState.Prepare(Copy(allocator, aFile.View())));
            if (bundle) {
                REQUIRE(bState.Prepare(Copy(allocator, bFile.View())));
            }

            closing.attempts = 0;
            closing.closeAt = attempt;
            ConstructionContext transaction(target, owner);
            PackageLoadState* both[2] = {&bState, &aState};
            const Span<PackageLoadState* const> states =
                bundle ? Span<PackageLoadState* const>(both, 2)
                       : Span<PackageLoadState* const>(both + 1, 1);
            ObjectHandle handles[2];
            AssetDiagnostic failure;
            auto result = InstantiatePackages(transaction, states, bundle ? nullptr : &resolver,
                                              Span<ObjectHandle>(handles, states.Size()), &failure);
            transaction.Rollback();
            if (bRoot[0].IsValid()) {
                CHECK(target.Discard(owner, bHandle[0]));
                bRoot[0].Reset();
            }

            if (!closing.closed) {
                REQUIRE(result);
                break;
            }

            ++closings;
            CHECK(result || (result.Error() == ErrorCode::ShuttingDown &&
                             failure.code == AssetDiagnosticCode::None));
            if (!result &&
                failure.section == static_cast<uint32>(PackageSectionKind::ObjectImports)) {
                ++atImports;
            }
        }

        CHECK(closings > 0);
        CHECK(atImports > 0);
    }
}
