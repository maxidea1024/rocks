#include "PackageEditing.h"

#include <AssetRuntime/Package.generated.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>

// 빈틈 리뷰가 찾은 경로의 시험이다(Docs/Design/AssetSystem.ko.md 5.3절, 5.7절 13·14, 14.1.2절):
// 헤더 크기 상한의 두 칸, 엔진·컨테이너 버전 경계, writer의 섹션 크기·null id 패키지·Package 객체
// 참조, NewPackage의 스레드·할당 실패(typed root 실패 시 Discard), InstantiatePackages의 트랜잭션
// 오류 분류·재호출, Required 참조 누락(쿠킹 손상·에디터 계속), bulk 값 규칙(빈 ref·쓰이지 않는
// entry·EditorOnly 위치만의 공유·원본 크기 불일치), 커스텀 버전 여럿.
using namespace rocks;
using namespace rocks::asset_test;

namespace {

// 대표 에셋(TestAsset)만 있는 패키지다.
struct Lone {
    RootRef<Package> package;
    ObjectHandle asset{};
};

bool MakeLone(ObjectRegistry& registry, Lone& lone, uint64 id,
              PackageKind kind = PackageKind::Cooked) noexcept {
    auto package = NewPackage(registry, registry.OwnerAccess(), FixedId(id), kind);
    if (!package) {
        return false;
    }

    lone.package = Move(package.Value());
    lone.asset = Make<TestAsset>(registry, TestAssetType(), lone.package.Handle(), "");
    return !lone.asset.IsNull();
}

// 다른 스레드에서 fn(context)을 부르고 끝날 때까지 기다린다(순서는 Join이 정한다).
template <class Fn>
void RunOnWorker(Fn& fn) noexcept {
    platform::Thread thread;
    auto entry = [](void* context) noexcept { (*static_cast<Fn*>(context))(); };
    REQUIRE(thread.Start(entry, &fn) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
}

// type(클래스 또는 struct) 스키마의 필드 name의 ID다.
Guid FieldIdOf(TypeRegistry& types, TypeId type, StringView name) noexcept {
    auto lease = types.Acquire(type);
    if (!lease) {
        lease = types.AcquireValueType(type, TypeKind::Struct);
    }

    if (!lease) {
        return Guid{};
    }

    for (const FieldSchema& field : lease.Value().Schema().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field.id;
        }
    }

    return Guid{};
}

// Objects 섹션에서 필드 id의 첫 TLV를 찾아 payload offset(섹션 기준)을 돌려준다. TLV는
// fieldGuid16(high·low LE)·type4·byteLength8·payload다(Serialization Record).
constexpr usize RecordFieldIdBytes = 16;
// TLV에서 payload가 시작하는 위치다(id 16 + type 4 + byteLength 8).
constexpr usize RecordFieldPayloadOffset = RecordFieldIdBytes + sizeof(uint32) + sizeof(uint64);

usize FieldPayload(Span<const uint8> objects, Guid id) noexcept {
    uint8 pattern[RecordFieldIdBytes];
    StoreLittleEndian<uint64>(pattern, id.high);
    StoreLittleEndian<uint64>(pattern + sizeof(uint64), id.low);
    for (usize offset = 0; offset + RecordFieldPayloadOffset <= objects.Size(); ++offset) {
        bool same = true;
        for (usize index = 0; same && index < RecordFieldIdBytes; ++index) {
            same = objects[offset + index] == pattern[index];
        }

        if (same) {
            return offset + RecordFieldPayloadOffset;
        }
    }

    return IndexNone;
}

// Objects 섹션의 필드 id 첫 payload에 u32 value를 쓴 패키지다(섹션 CRC는 다시 맞춘다).
Array<uint8> WithObjectsU32(Allocator& allocator, const Array<uint8>& file, Guid id,
                            uint32 value) noexcept {
    EditablePackage edit(allocator);
    if (!edit.Parse(file.View())) {
        return Array<uint8>(allocator);
    }

    EditableSection* objects = edit.Find(PackageSectionKind::Objects);
    const usize payload = objects != nullptr ? FieldPayload(objects->bytes.View(), id) : IndexNone;
    if (payload == IndexNone) {
        return Array<uint8>(allocator);
    }

    StoreLittleEndian<uint32>(objects->bytes.Data() + payload, value);
    return edit.Build();
}

// state 하나를 transaction에 올린 결과와 진단이다.
Result<void> InstantiateOne(ConstructionContext& transaction, PackageLoadState& state,
                            AssetDiagnostic* failure = nullptr,
                            PackageImportResolver* resolver = nullptr) noexcept {
    PackageLoadState* states[1] = {&state};
    ObjectHandle handles[1];
    return InstantiatePackages(transaction, states, resolver, handles, failure);
}

} // namespace

// 헤더 상한(AS 5.1절): ReadPackageHeader는 Stat 크기를 따로 받으므로 64 GiB 파일 없이 두 칸을 본다
// — sectionTableOffset = 64 GiB(상한 안)이고 fileBytes = 64 GiB + 36(식·Stat과 같음)이면 fileBytes
// 상한, sectionTableOffset = 64 GiB + 8이면 표 offset 상한이다. 쿠킹 formatVersion 0은
// UnsupportedVersion이다(에디터 0은 AssetPackage_HeaderFields).
TEST(AssetPackage_HeaderUpperBounds) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Lone lone;
    REQUIRE(MakeLone(*env.source, lone, 1300));
    const Array<uint8> file =
        Write(env.allocator, *env.source, lone.package.Handle(), CookedOptions());
    REQUIRE(!file.IsEmpty());
    auto header = [&](uint64 table, uint32 count) noexcept {
        Array<uint8> bytes = Copy(env.allocator, file.View().Slice(0, PackageHeaderBytes));
        StoreLittleEndian<uint32>(bytes.Data() + 12, count);
        StoreLittleEndian<uint64>(bytes.Data() + 16, table);
        StoreLittleEndian<uint64>(bytes.Data() + 24, table + PackageSectionEntryBytes * count + 4);
        FixHeaderCrc(bytes);
        return bytes;
    };

    const uint64 cases[] = {PackageMaxFileBytes, PackageMaxFileBytes + 8};
    for (uint64 table : cases) {
        const Array<uint8> bytes = header(table, 1);
        AssetDiagnostic failure;
        CHECK(ReadPackageHeader(bytes.View(), table + PackageSectionEntryBytes + 4, &failure)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
    }

    // 대조: 상한 안(64 GiB − 40에서 끝)은 받는다.
    const uint64 inside = PackageMaxFileBytes - 40;
    CHECK(ReadPackageHeader(header(inside, 1).View(), inside + PackageSectionEntryBytes + 4));

    Array<uint8> zero = Copy(env.allocator, file.View());
    StoreLittleEndian<uint16>(zero.Data() + 4, 0);
    FixHeaderCrc(zero);
    AssetDiagnostic failure;
    CHECK(ReadPackage(env.allocator, zero.View(), PackageReadOptions{}, &failure).Error() ==
          ErrorCode::Unsupported);
    CHECK(failure.code == AssetDiagnosticCode::UnsupportedVersion);
    // 진단 out 인자 없이도 같은 오류다(헤더 판정과 내용 판정 — 쿠킹 패키지를 에디터로 기대).
    CHECK(ReadPackage(env.allocator, zero.View(), PackageReadOptions{}).Error() ==
          ErrorCode::Unsupported);
    PackageReadOptions editorRead;
    editorRead.expectedKind = PackageKind::Editor;
    CHECK(ReadPackage(env.allocator, file.View(), editorRead).Error() ==
          ErrorCode::InvalidArgument);
}

// 대표 에셋 타입(AS 5.7절 10): export 1 타입과 그 노드 Record가 함께 Asset이 아닌
// 타입(TestPart)이면 그래프 1단계는 받고(타입이 서로 맞다) Prepare의 대표 에셋 대조가 Exports
// 손상(detail 1)으로 거부한다. Objects는 writer를 거치지 않고 graph v2 저장으로 만든다(writer는
// 이런 패키지를 쓰지 않는다).
TEST(AssetPackage_RepresentativeMustBeAsset) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    Allocator& allocator = env.allocator;
    auto odd = NewPackage(registry, access, FixedId(1390), PackageKind::Editor);
    REQUIRE(odd);
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), odd.Value().Handle(), "");
    REQUIRE(!part.IsNull());
    ObjectGraphSaveOptions save;
    save.unsaveable = UnsaveablePolicy::Exclude;
    save.writeNodeTable = false;
    auto saved =
        SaveObjectGraphSet(allocator, registry, access, Span<const ObjectHandle>(&part, 1), save);
    REQUIRE(saved);
    Array<uint8> objects = saved.Value().TakeBytes();
    auto lease = env.types.Acquire(TestPartType());
    REQUIRE(lease);
    Lone lone;
    REQUIRE(MakeLone(registry, lone, 1391, PackageKind::Editor));
    const Array<uint8> file = Write(allocator, registry, lone.package.Handle(), EditorOptions());
    REQUIRE(!file.IsEmpty());
    const PackageExportEntry exports[] = {
        {StringView(), TestPartType(), 0, static_cast<uint32>(PackageExportFlags::Primary)}};
    const PackageTypeVersion types[] = {{TestPartType(), lease.Value().Schema().version}};
    EditablePackage edit(allocator);
    REQUIRE(edit.Parse(file.View()));
    Array<uint8> exportBytes = ExportsBytes(allocator, exports, 1);
    Array<uint8> summary =
        SummaryBytes(allocator, SummaryInput{types, {}, true}, SummaryFault::None);
    REQUIRE(edit.Replace(PackageSectionKind::Exports, exportBytes.View()));
    REQUIRE(edit.Replace(PackageSectionKind::Summary, summary.View()));
    REQUIRE(edit.Replace(PackageSectionKind::Objects, objects.View()));
    PackageLoadState state(allocator, *env.target, env.types, env.target->OwnerAccess(),
                           LoadOptions(PackageKind::Editor));
    AssetDiagnostic failure;
    CHECK(!state.Prepare(edit.Build(), &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.section == static_cast<uint32>(PackageSectionKind::Exports) &&
          failure.detail == 1);
}

// 엔진 버전 축(AS 5.3절): compatibleWith의 주가 더 새면 NewerVersion, 패치는 보지 않으며(1.2.0
// 엔진이 1.2.9가 저장한 패키지를 읽는다) 더 오래된 패키지(1.1.5)는 받는다.
TEST(AssetPackage_EngineVersionBoundaries) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Lone lone;
    REQUIRE(MakeLone(*env.source, lone, 1310, PackageKind::Editor));
    auto readWith = [&](EngineVersion saved, EngineVersion current, AssetDiagnostic& failure) {
        PackageWriteOptions options = EditorOptions();
        options.engineVersion = saved;
        const Array<uint8> file = Write(env.allocator, *env.source, lone.package.Handle(), options);
        PackageReadOptions read;
        read.expectedKind = PackageKind::Editor;
        read.engineVersion = current;
        return ReadPackage(env.allocator, file.View(), read, &failure).IsOk();
    };

    AssetDiagnostic failure;
    CHECK(!readWith(EngineVersion{2, 0, 0}, EngineVersion{1, 9, 9}, failure));
    CHECK(failure.code == AssetDiagnosticCode::NewerVersion &&
          failure.error == ErrorCode::Unsupported);
    CHECK(readWith(EngineVersion{1, 2, 9}, EngineVersion{1, 2, 0}, failure));
    CHECK(readWith(EngineVersion{1, 1, 5}, EngineVersion{1, 2, 3}, failure));
    CHECK(readWith(EngineVersion{0, 9, 0}, EngineVersion{1, 0, 0}, failure));
}

// writer 경계: ImportRecord 하나가 64 MiB + 1 byte면(옵션 검증은 Record 모양만 본다) 섹션 상한
// CapacityExceeded다. NewPackage를 거치지 않아 id가 null인 Package는 쓰기 InvalidArgument이고, 그
// 패키지 객체를 가리키는 다른 패키지 쓰기도 InvalidArgument다(AS 5.7절 13). 다른 Package 객체
// 자체를 가리키는 참조는 InvalidArgument다. 대표 에셋 없는 패키지의 객체를 가리키면 NotFound다.
// 커스텀 버전 셋(정렬)은 쓰고 읽힌다.
TEST(AssetPackage_WriterEdgeRejections) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    Allocator& allocator = env.allocator;
    {
        Lone lone;
        REQUIRE(MakeLone(registry, lone, 1320, PackageKind::Editor));
        RecordLimits limits;
        limits.archive = EditLimits();
        RecordBuilder record(allocator, Guid{0x51, 0x52}, 1, limits);
        Array<uint8> payload(allocator);
        REQUIRE(payload.TryResize(static_cast<usize>(PackageMaxSectionBytes) + 1 - 44 - 28));
        REQUIRE(record.AddField(Guid{0x53, 0x54}, WireType::Bytes, payload.View()));
        auto built = record.Build();
        REQUIRE(built);
        REQUIRE(built.Value().Size() == PackageMaxSectionBytes + 1);
        PackageWriteOptions options = EditorOptions();
        options.importRecord = built.Value().View();
        MemoryPackageSink sink(allocator);
        CHECK(WritePackage(allocator, registry, access, lone.package.Handle(), options, sink)
                  .Error() == ErrorCode::CapacityExceeded);
    }

    {
        auto bare = registry.NewObject<Package>(access, GeneratedType<Package>::Id());
        REQUIRE(bare);
        const ObjectHandle asset =
            Make<TestAsset>(registry, TestAssetType(), bare.Value().Handle(), "");
        REQUIRE(!asset.IsNull());
        MemoryPackageSink sink(allocator);
        CHECK(
            WritePackage(allocator, registry, access, bare.Value().Handle(), CookedOptions(), sink)
                .Error() == ErrorCode::InvalidArgument);
        Lone lone;
        REQUIRE(MakeLone(registry, lone, 1321));
        REQUIRE(registry.SetReference(access, lone.asset,
                                      Pointer<TestAsset>(registry, lone.asset)->hard, asset));
        MemoryPackageSink other(allocator);
        CHECK(
            WritePackage(allocator, registry, access, lone.package.Handle(), CookedOptions(), other)
                .Error() == ErrorCode::InvalidArgument);
        // 다른 Package 객체 자체를 가리킨다.
        REQUIRE(registry.SetReference(
            access, lone.asset, Pointer<TestAsset>(registry, lone.asset)->hard, ObjectHandle{}));
        Lone target;
        REQUIRE(MakeLone(registry, target, 1322));
        REQUIRE(WithBorrow<TestAsset>(registry, lone.asset, [&](TestAsset& value) {
            return registry
                .AddReference(access, lone.asset, value.hardList, target.package.Handle())
                .IsOk();
        }));
        MemoryPackageSink third(allocator);
        CHECK(
            WritePackage(allocator, registry, access, lone.package.Handle(), CookedOptions(), third)
                .Error() == ErrorCode::InvalidArgument);
    }

    {
        // 대표 에셋 없이 이름 있는 자식만 둔 패키지의 객체를 가리키면 그 패키지의 대표 에셋(Hard
        // expectedType)을 찾지 못해 NotFound다.
        auto loose = NewPackage(registry, access, FixedId(1324), PackageKind::Cooked);
        REQUIRE(loose);
        const ObjectHandle child =
            Make<TestPart>(registry, TestPartType(), loose.Value().Handle(), "Loose");
        REQUIRE(!child.IsNull());
        Lone lone;
        REQUIRE(MakeLone(registry, lone, 1325));
        REQUIRE(WithBorrow<TestAsset>(registry, lone.asset, [&](TestAsset& value) {
            return registry.AddReference(access, lone.asset, value.hardList, child).IsOk();
        }));
        MemoryPackageSink sink(allocator);
        CHECK(
            WritePackage(allocator, registry, access, lone.package.Handle(), CookedOptions(), sink)
                .Error() == ErrorCode::NotFound);
    }

    {
        Lone lone;
        REQUIRE(MakeLone(registry, lone, 1323, PackageKind::Editor));
        const PackageCustomVersion customs[] = {{Guid{1, 1}, 4}, {Guid{1, 2}, 9}, {Guid{2, 0}, 1}};
        PackageWriteOptions options = EditorOptions();
        options.customVersions = customs;
        const Array<uint8> file = Write(allocator, registry, lone.package.Handle(), options);
        REQUIRE(!file.IsEmpty());
        PackageReadOptions read;
        read.expectedKind = PackageKind::Editor;
        read.customVersions = customs;
        auto contents = ReadPackage(allocator, file.View(), read);
        REQUIRE(contents);
        REQUIRE(contents.Value().summary.customVersions.Size() == 3);
        CHECK((contents.Value().summary.customVersions[2].id == Guid{2, 0}));
        CHECK(!contents.Value().readOnly);
    }
}

// NewPackage(AS 5.7절 7): 다른 스레드에서 부르면 WrongThread, Registry allocator의 n번째 할당을
// 실패시키면 OutOfMemory이고 어느 지점에서 실패해도(Commit 뒤 typed root를 얻지 못하면 Discard)
// 패키지가 남지 않는다.
TEST(AssetPackage_NewPackageFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    struct Job {
        ObjectRegistry* registry;
        ErrorCode error = ErrorCode::None;

        void operator()() noexcept {
            auto made = NewPackage(*registry, registry->OwnerAccess(), FixedId(1330));
            error = made ? ErrorCode::None : made.Error();
        }
    } job{&registry};
    RunOnWorker(job);
    CHECK(job.error == ErrorCode::WrongThread);

    test::NFailAllocator failing(env.allocator);
    RegistryOptions options;
    options.names = &env.names;
    options.policy.packageNativeType = NativeTypeKey<Package>();
    options.policy.rejectWorldOnly = true;
    auto made = UniquePtr<ObjectRegistry>::TryMake(env.allocator, failing, env.types,
                                                   ContextId{0x4153534554ull, 11}, options);
    REQUIRE(made);
    ObjectRegistry& target = *made.Value();
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto package = NewPackage(target, target.OwnerAccess(), FixedId(1331));
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!package) {
            CHECK(package.Error() == ErrorCode::OutOfMemory);
            ++failures;
        } else {
            CHECK(target.Discard(target.OwnerAccess(), package.Value().Handle()));
            package.Value().Reset();
        }

        CHECK(target.Collect(target.OwnerAccess()));
        CHECK(target.Stats().liveObjects == 0);
        if (!triggered) {
            REQUIRE(package);
            break;
        }
    }

    CHECK(failures > 0);
}

// InstantiatePackages의 트랜잭션 오류 분류(AS 5.7절 8): 다른 스레드에서 부르면 WrongThread, 다른
// 접근 문맥의 트랜잭션은 WrongContext, 시작 오류로 고정된 트랜잭션(종료 ShuttingDown·재진입
// Busy)은 그 오류이며 모두 진단 None이다. 같은 상태를 다시 넘기면(성공 뒤·실패 뒤)
// InvalidArgument다 (AS 5.7절 14 — 그래프 Stage 순서 오류가 CorruptPackage로 나가지 않는다).
TEST(AssetPackage_InstantiateTransactionErrors) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Lone lone;
    REQUIRE(MakeLone(*env.source, lone, 1340));
    const Array<uint8> file =
        Write(env.allocator, *env.source, lone.package.Handle(), CookedOptions());
    REQUIRE(!file.IsEmpty());
    ObjectRegistry& target = *env.target;
    const ObjectAccessContext access = target.OwnerAccess();
    auto prepared = [&](PackageLoadState& state) noexcept {
        return state.Prepare(Copy(env.allocator, file.View())).IsOk();
    };

    {
        PackageLoadState state(env.allocator, target, env.types, access,
                               LoadOptions(PackageKind::Cooked));
        REQUIRE(prepared(state));
        ConstructionContext transaction(target, access);
        struct Job {
            ConstructionContext* transaction;
            PackageLoadState* state;
            AssetDiagnostic failure{};
            ErrorCode error = ErrorCode::None;

            void operator()() noexcept {
                auto made = InstantiateOne(*transaction, *state, &failure);
                error = made ? ErrorCode::None : made.Error();
            }
        } job{&transaction, &state};
        RunOnWorker(job);
        CHECK(job.error == ErrorCode::WrongThread);
        CHECK(job.failure.code == AssetDiagnosticCode::None &&
              job.failure.error == ErrorCode::WrongThread);
        transaction.Rollback();
        // 실패 뒤 다시 넘기면 InvalidArgument다.
        ConstructionContext again(target, access);
        CHECK(InstantiateOne(again, state).Error() == ErrorCode::InvalidArgument);
        again.Rollback();
    }

    {
        PackageLoadState state(env.allocator, target, env.types, access,
                               LoadOptions(PackageKind::Cooked));
        REQUIRE(prepared(state));
        ConstructionContext transaction(target, ObjectAccessContext{{0x4153534554ull, 99}});
        AssetDiagnostic failure;
        CHECK(InstantiateOne(transaction, state, &failure).Error() == ErrorCode::WrongContext);
        CHECK(failure.code == AssetDiagnosticCode::None);
    }

    const ErrorCode fixed[] = {ErrorCode::ShuttingDown, ErrorCode::Busy};
    for (ErrorCode error : fixed) {
        PackageLoadState state(env.allocator, target, env.types, access,
                               LoadOptions(PackageKind::Cooked));
        REQUIRE(prepared(state));
        ConstructionContext transaction(target, access);
        REQUIRE(transaction.Fail(error));
        AssetDiagnostic failure;
        CHECK(InstantiateOne(transaction, state, &failure).Error() == error);
        CHECK(failure.code == AssetDiagnosticCode::None && failure.error == error);
        transaction.Rollback();
    }

    {
        PackageLoadState state(env.allocator, target, env.types, access,
                               LoadOptions(PackageKind::Cooked));
        REQUIRE(prepared(state));
        ObjectHandle handles[1];
        RootRef<Object> roots[1];
        {
            ConstructionContext transaction(target, access);
            PackageLoadState* states[1] = {&state};
            REQUIRE(InstantiatePackages(transaction, states, nullptr, handles));
            REQUIRE(transaction.Commit(handles, roots));
        }

        ConstructionContext again(target, access);
        CHECK(InstantiateOne(again, state).Error() == ErrorCode::InvalidArgument);
        again.Rollback();
        CHECK(target.Discard(access, handles[0]));
        roots[0].Reset();
    }

    CHECK(target.Collect(access));
    CHECK(target.Stats().liveObjects == 0);
}

// Required 참조 누락(AS 5.7절 9, 14.1.2절): 같은 패키지 export(하위 Asset)를 가리키는 Required
// 참조 값을 0(null)으로 바꾸고 CRC를 맞추면 쿠킹 로드는 CheckRequired에서 CorruptPackage(트랜잭션
// 실패 — 객체가 남지 않는다), 에디터 로드는 누락을 세고 계속해 null 참조로 공개한다.
TEST(AssetPackage_RequiredReferenceZeroed) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    const Guid targetField = FieldIdOf(env.types, TestRequiredType(), "target");
    REQUIRE(!(targetField == Guid{}));
    const PackageKind kinds[] = {PackageKind::Cooked, PackageKind::Editor};
    for (PackageKind kind : kinds) {
        auto package =
            NewPackage(registry, access, FixedId(kind == PackageKind::Cooked ? 1350 : 1351), kind);
        REQUIRE(package);
        const ObjectHandle asset =
            Make<TestRequiredAsset>(registry, TestRequiredType(), package.Value().Handle(), "");
        const ObjectHandle child = Make<TestAsset>(registry, TestAssetType(), asset, "Child");
        REQUIRE(!child.IsNull());
        REQUIRE(registry.SetReference(access, asset,
                                      Pointer<TestRequiredAsset>(registry, asset)->target, child));
        const Array<uint8> file =
            Write(env.allocator, registry, package.Value().Handle(),
                  kind == PackageKind::Cooked ? CookedOptions() : EditorOptions());
        REQUIRE(!file.IsEmpty());
        Array<uint8> zeroed = WithObjectsU32(env.allocator, file, targetField, 0);
        REQUIRE(!zeroed.IsEmpty());
        Array<uint8> files[1] = {Move(zeroed)};
        RootRef<Object> roots[1];
        ObjectHandle handles[1];
        AssetDiagnostic failure;
        auto loaded = LoadAll(env, *env.target, files, kind, nullptr, roots, handles, &failure);
        if (kind == PackageKind::Cooked) {
            CHECK(!loaded);
            CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
                  failure.error == ErrorCode::InvalidArgument &&
                  failure.section == static_cast<uint32>(PackageSectionKind::Objects));
            CHECK(env.target->Stats().liveObjects == 0);
        } else {
            REQUIRE(loaded);
            auto representative =
                env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
            REQUIRE(representative);
            TestRequiredAsset* pointer =
                Pointer<TestRequiredAsset>(*env.target, representative.Value());
            REQUIRE(pointer != nullptr);
            CHECK(pointer->target.Handle().IsNull());
            CHECK(env.target->Discard(env.target->OwnerAccess(), handles[0]));
            roots[0].Reset();
        }
    }
}

// bulk 값 규칙(AS 5.7절 14): 빈 ref(bulkId 0)에 크기·해시가 있으면 writer는 InvalidArgument, 그런
// 값을 담은 파일(bulkId만 0으로 고침)은 Objects 손상이다. 어떤 ref도 가리키지 않는 BulkIndex
// entry는 BulkIndex 손상(detail = 그 bulkId)이고 에디터 읽기 전용 열기는 받는다.
TEST(AssetPackage_BulkValueRules) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    Allocator& allocator = env.allocator;
    Array<uint8> data(allocator);
    FillIncompressible(data, 700, 13);
    TestBulkSource bulk;
    const BulkDataRef ref = bulk.Add(1, data.View());
    {
        Lone lone;
        REQUIRE(MakeLone(registry, lone, 1360));
        BulkDataRef malformed{};
        malformed.rawBytes = 5;
        Pointer<TestAsset>(registry, lone.asset)->pixels = malformed;
        CHECK(Write(allocator, registry, lone.package.Handle(), CookedOptions()).IsEmpty());
        malformed.rawBytes = 0;
        malformed.sha256.bytes[31] = 1;
        Pointer<TestAsset>(registry, lone.asset)->pixels = malformed;
        CHECK(Write(allocator, registry, lone.package.Handle(), CookedOptions()).IsEmpty());
    }

    Lone lone;
    REQUIRE(MakeLone(registry, lone, 1361, PackageKind::Editor));
    Pointer<TestAsset>(registry, lone.asset)->pixels = ref;
    PackageWriteOptions cooked = CookedOptions();
    cooked.bulkSource = &bulk;
    PackageWriteOptions editor = EditorOptions();
    editor.bulkSource = &bulk;
    const Array<uint8> cookedFile = Write(allocator, registry, lone.package.Handle(), cooked);
    const Array<uint8> editorFile = Write(allocator, registry, lone.package.Handle(), editor);
    REQUIRE(!cookedFile.IsEmpty() && !editorFile.IsEmpty());

    // bulkId만 0으로: {0, rawBytes, sha}.
    const Guid bulkIdField = FieldIdOf(env.types, GeneratedType<BulkDataRef>::Id(), "bulkId");
    REQUIRE(!(bulkIdField == Guid{}));
    {
        PackageLoadState state(allocator, *env.target, env.types, env.target->OwnerAccess(),
                               LoadOptions(PackageKind::Cooked));
        AssetDiagnostic failure;
        CHECK(!state.Prepare(WithObjectsU32(allocator, cookedFile, bulkIdField, 0), &failure));
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
              failure.section == static_cast<uint32>(PackageSectionKind::Objects));
    }

    // 쓰이지 않는 entry 2를 덧붙인다.
    auto withExtraEntry = [&](const Array<uint8>& file, PackageKind kind) noexcept {
        PackageReadOptions read;
        read.expectedKind = kind;
        auto contents = ReadPackage(allocator, file.View(), read);
        EditablePackage edit(allocator);
        (void)edit.Parse(file.View());
        EditableSection* section = edit.Find(PackageSectionKind::Bulk);
        Array<PackageBulkEntry> rows = CopyEntries(allocator, contents.Value().bulkEntries);
        const uint32 alignment = contents.Value().header.bulkAlignment;
        PackageBulkEntry extra;
        extra.bulkId = 2;
        extra.offset = package_detail::AlignUp(section->bytes.Size(), alignment);
        extra.storedBytes = 3;
        extra.rawBytes = 3;
        const uint8 bytes[3] = {7, 8, 9};
        extra.crc32 = Crc32(Span<const uint8>(bytes, 3));
        extra.sha256 = Sha256::Hash(Span<const uint8>(bytes, 3));
        (void)section->bytes.TryResize(static_cast<usize>(extra.offset));
        (void)section->bytes.TryAppend(Span<const uint8>(bytes, 3));
        (void)rows.TryPushBack(extra);
        Array<uint8> index = BulkIndexBytes(allocator, rows.View(), 2);
        (void)edit.Replace(PackageSectionKind::BulkIndex, index.View());
        return edit.Build();
    };

    {
        Array<uint8> extra = withExtraEntry(cookedFile, PackageKind::Cooked);
        PackageReadOptions read;
        CHECK(ReadPackage(allocator, extra.View(), read));
        PackageLoadState state(allocator, *env.target, env.types, env.target->OwnerAccess(),
                               LoadOptions(PackageKind::Cooked));
        AssetDiagnostic failure;
        CHECK(!state.Prepare(Move(extra), &failure));
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
              failure.section == static_cast<uint32>(PackageSectionKind::BulkIndex) &&
              failure.detail == 2);
    }

    {
        // 에디터 읽기 전용(Required가 아닌 더 새 Tags 섹션)은 쓰임 검사를 하지 않는다.
        Array<uint8> extra = withExtraEntry(editorFile, PackageKind::Editor);
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(extra.View()));
        RecordBuilder record(allocator, Guid{1, 1}, 1);
        auto built = record.Build();
        REQUIRE(built);
        EditableSection* tags =
            edit.Insert(static_cast<uint32>(PackageSectionKind::Tags), 0, built.Value().View());
        REQUIRE(tags != nullptr);
        tags->schemaVersion = 2;
        PackageLoadState state(allocator, *env.target, env.types, env.target->OwnerAccess(),
                               LoadOptions(PackageKind::Editor));
        CHECK(state.Prepare(edit.Build()));
        CHECK(state.IsReadOnly());
        // 같은 파일에 Tags를 넣지 않으면 손상이다.
        PackageLoadState strict(allocator, *env.target, env.types, env.target->OwnerAccess(),
                                LoadOptions(PackageKind::Editor));
        CHECK(!strict.Prepare(Move(extra)));
    }
}

// bulk 쓰기 규칙(AS 5.7절 6): 같은 ref가 EditorOnly 위치에서만 두 번 쓰이면 entry는 EditorOnly
// 표시, 원본 크기가 ref의 rawBytes와 다르면(SHA-256 같음) CorruptPackage다.
TEST(AssetPackage_BulkWriterRules) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    Allocator& allocator = env.allocator;
    Array<uint8> data(allocator);
    FillCompressible(data, 900, 14);
    TestBulkSource bulk;
    const BulkDataRef ref = bulk.Add(4, data.View());
    Lone lone;
    REQUIRE(MakeLone(registry, lone, 1370, PackageKind::Editor));
    const ObjectHandle sub = Make<TestAsset>(registry, TestAssetType(), lone.asset, "Sub");
    REQUIRE(!sub.IsNull());
    Pointer<TestAsset>(registry, lone.asset)->source = ref;
    Pointer<TestAsset>(registry, sub)->source = ref;
    PackageWriteOptions options = EditorOptions();
    options.bulkSource = &bulk;
    const Array<uint8> file = Write(allocator, registry, lone.package.Handle(), options);
    REQUIRE(!file.IsEmpty());
    PackageReadOptions read;
    read.expectedKind = PackageKind::Editor;
    auto contents = ReadPackage(allocator, file.View(), read);
    REQUIRE(contents);
    REQUIRE(contents.Value().bulkEntries.Size() == 1);
    CHECK(contents.Value().bulkEntries[0].flags == static_cast<uint32>(BulkEntryFlags::EditorOnly));

    BulkDataRef wrongSize = ref;
    wrongSize.rawBytes += 1;
    Pointer<TestAsset>(registry, lone.asset)->source = wrongSize;
    Pointer<TestAsset>(registry, sub)->source = BulkDataRef{};
    MemoryPackageSink sink(allocator);
    AssetDiagnostic failure;
    CHECK(WritePackage(allocator, registry, registry.OwnerAccess(), lone.package.Handle(), options,
                       sink, &failure)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.section == static_cast<uint32>(PackageSectionKind::Bulk) && failure.detail == 4);
}
