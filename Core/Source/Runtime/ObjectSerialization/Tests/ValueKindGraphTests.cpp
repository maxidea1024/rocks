#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// B3a 값 종류(AssetId·Sha256Digest·SoftObjectRef)의 스키마·등록된 쓰기 검증·그래프 왕복·잘못된
// 저장값 거부 시험이다(Docs/Design/ReflectionFoundations.ko.md 3.1–3.3절). 필드 접근의 원자성과
// SoftObjectRef 자체 의미는 ObjectRuntime의 AssetValueFieldTests가 본다.
using namespace rocks;
using reflection_fixture::AssetFields;
using reflection_fixture::Node;

namespace {

const FieldSchema& SchemaField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field;
        }
    }

    platform::Fatal("fixture field missing");
}

const RuntimeFieldOps& FieldOps(const TypeLease& type, StringView name) noexcept {
    const Guid id = SchemaField(type.Schema(), name).id;
    for (const RuntimeFieldOps& field : type.Ops().fields) {
        if (field.fieldId == id) {
            return field;
        }
    }

    platform::Fatal("fixture field ops missing");
}

constexpr AssetId SourceAsset{Guid{0x0123456789abcdefull, 0xfedcba9876543210ull}};
constexpr AssetId IconAsset{Guid{0x1111222233334444ull, 0x5555666677778888ull}};
constexpr AssetId AnyAsset{Guid{0x9999aaaabbbbccccull, 0xddddeeeeffff0000ull}};

Sha256Digest SampleDigest() noexcept {
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(index * 7 + 1);
    }

    return digest;
}

// 길이가 bytes인 경로다. 끝은 3 byte UTF-8 문자("한")이므로 bytes는 3 이상이다.
StringView PathOfBytes(usize bytes) noexcept {
    static char path[SoftObjectRef<>::MaxObjectPathBytes + 1];
    if (bytes < 3 || bytes > sizeof(path)) {
        platform::Fatal("path size");
    }

    for (usize index = 0; index < bytes - 3; ++index) {
        path[index] = 'a';
    }

    path[bytes - 3] = '\xed';
    path[bytes - 2] = '\x95';
    path[bytes - 1] = '\x9c';
    return StringView(path, bytes);
}

// 레코드 하나(AssetFields)짜리 graph format 1 바이트를 만든다(ObjectGraphTests의 SingleGraph와 같은
// envelope).
Result<Array<uint8>> SingleAssetGraph(Allocator& allocator, Span<const uint8> record) noexcept {
    const uint8 magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
    ArchiveWriter writer(allocator);
    Result<void> written = writer.WriteRaw(magic);
    const uint32 header[] = {1, 1, 1};
    for (uint32 value : header) {
        if (written) {
            written = writer.WriteU32(value);
        }
    }

    if (written) {
        written = writer.WriteU64(1);
    }

    if (written) {
        written = writer.WriteGuid(TypeIdOf<AssetFields>());
    }

    if (written) {
        written = writer.WriteU32(1);
    }

    if (written) {
        written = writer.WriteBytes(record);
    }

    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    return Result<Array<uint8>>::Success(writer.TakeBytes());
}

// AssetFields 하나를 만들어 값을 채운다. 시험이 RootRef를 소유한다.
Result<RootRef<AssetFields>> NewFilled(ObjectRegistry& objects, StringView iconPath) noexcept {
    auto created = objects.NewObject<AssetFields>(objects.OwnerAccess(), TypeIdOf<AssetFields>());
    if (!created) {
        return created;
    }

    auto borrowed = objects.Resolve<AssetFields>(objects.OwnerAccess(), created.Value().Handle());
    if (!borrowed) {
        return Result<RootRef<AssetFields>>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<RootRef<AssetFields>>::Failure(value.Error());
    }

    value.Value()->source = SourceAsset;
    value.Value()->hash = SampleDigest();
    if (auto icon = value.Value()->icon.TryAssign(IconAsset, iconPath); !icon) {
        return Result<RootRef<AssetFields>>::Failure(icon.Error());
    }

    // 에셋만 있고 경로가 빈 소프트 참조(대표 객체)도 왕복해야 한다.
    if (auto any = value.Value()->anyAsset.TryAssign(AnyAsset, ""); !any) {
        return Result<RootRef<AssetFields>>::Failure(any.Error());
    }

    return created;
}

Result<void> LoadWithFailure(test::NFailAllocator& allocator, TypeRegistry& types,
                             Span<const uint8> bytes, uint64* attempts = nullptr) noexcept {
    ObjectRegistry registry(allocator, types, {0x5643, 9});
    auto result = LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(), bytes);
    if (attempts != nullptr) {
        *attempts = allocator.Attempts();
    }

    if (!result) {
        if (registry.Stats().liveObjects != 0 || registry.Stats().reservedObjects != 0) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        return Result<void>::Failure(result.Error());
    }

    result.Value().Reset();
    auto collected = registry.Collect(registry.OwnerAccess());
    return collected ? Result<void>::Success() : Result<void>::Failure(collected.Error());
}

} // namespace

TEST(ValueKinds_SchemaDescribesAssetDigestAndSoftReferenceFields) {
    const TypeSchema& schema = TypeOf<AssetFields>();
    CHECK(SchemaField(schema, "source").value.kind == ValueKind::AssetId);
    CHECK(SchemaField(schema, "source").value.type == TypeId{});
    CHECK(SchemaField(schema, "hash").value.kind == ValueKind::Digest256);
    const FieldSchema& icon = SchemaField(schema, "icon");
    CHECK(icon.value.kind == ValueKind::SoftObjectReference);
    CHECK(icon.kind == FieldKind::Value);
    CHECK(icon.value.type == TypeIdOf<Node>());
    CHECK(HasFlag(icon.flags, FieldFlags::Required));
    CHECK(icon.element.kind == ValueKind::Opaque && icon.maxElements == 0);
    const FieldSchema& any = SchemaField(schema, "anyAsset");
    CHECK(any.value.kind == ValueKind::SoftObjectReference);
    CHECK(any.value.type == TypeId{});
}

TEST(ValueKinds_RegisteredWritesValidateAndReadBack) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto registered = GeneratedType<AssetFields>::Register(types);
    REQUIRE(registered);
    {
        ObjectRegistry objects(allocator, types, {0x5643, 1});
        const auto access = objects.OwnerAccess();
        {
            auto created = objects.NewObject<AssetFields>(access, TypeIdOf<AssetFields>());
            REQUIRE(created);
            const ObjectHandle handle = created.Value().Handle();
            auto lease = types.Acquire(TypeIdOf<AssetFields>());
            REQUIRE(lease);

            FieldValue asset(allocator);
            asset.kind = ValueKind::AssetId;
            asset.guid = SourceAsset.value;
            REQUIRE(FieldOps(lease.Value(), "source").write(objects, access, handle, asset));
            FieldValue digest(allocator);
            digest.kind = ValueKind::Digest256;
            digest.digest = SampleDigest();
            REQUIRE(FieldOps(lease.Value(), "hash").write(objects, access, handle, digest));
            FieldValue soft(allocator);
            soft.kind = ValueKind::SoftObjectReference;
            soft.guid = IconAsset.value;
            REQUIRE(soft.string.TryAssign("Body.Socket"));
            REQUIRE(FieldOps(lease.Value(), "icon").write(objects, access, handle, soft));

            // 소프트 참조 규칙: asset이 0인데 경로가 있거나 경로가 4 KiB를 넘으면 거부하고 값은
            // 그대로다. (FieldValue의 String은 UTF-8 불변식을 지키므로 UTF-8 검사는
            // AssetValueFieldTests의 SoftObjectRef 직접 대입 시험에서 본다.)
            FieldValue invalid(allocator);
            invalid.kind = ValueKind::SoftObjectReference;
            REQUIRE(invalid.string.TryAssign("Orphan"));
            CHECK(FieldOps(lease.Value(), "icon").write(objects, access, handle, invalid).Error() ==
                  ErrorCode::InvalidArgument);
            invalid.guid = IconAsset.value;
            REQUIRE(invalid.string.TryAssign(PathOfBytes(SoftObjectRef<>::MaxObjectPathBytes + 1)));
            CHECK(FieldOps(lease.Value(), "icon").write(objects, access, handle, invalid).Error() ==
                  ErrorCode::InvalidArgument);
            FieldValue wrongKind(allocator);
            wrongKind.kind = ValueKind::Guid;
            CHECK(FieldOps(lease.Value(), "source")
                      .write(objects, access, handle, wrongKind)
                      .Error() == ErrorCode::TypeMismatch);

            FieldValue read(allocator);
            REQUIRE(FieldOps(lease.Value(), "source").read(objects, access, handle, read));
            CHECK(read.kind == ValueKind::AssetId && read.guid == SourceAsset.value);
            REQUIRE(FieldOps(lease.Value(), "hash").read(objects, access, handle, read));
            CHECK(read.kind == ValueKind::Digest256 && read.digest == SampleDigest());
            REQUIRE(FieldOps(lease.Value(), "icon").read(objects, access, handle, read));
            CHECK(read.kind == ValueKind::SoftObjectReference && read.guid == IconAsset.value);
            CHECK(read.string.View() == StringView("Body.Socket"));

            // nonnull 뒤 null을 쓰면 null이 된다.
            FieldValue null(allocator);
            null.kind = ValueKind::SoftObjectReference;
            REQUIRE(FieldOps(lease.Value(), "icon").write(objects, access, handle, null));
            REQUIRE(FieldOps(lease.Value(), "icon").read(objects, access, handle, read));
            CHECK(read.guid == Guid{} && read.string.ByteCount() == 0);
        }

        REQUIRE(objects.Shutdown(access));
    }

    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

// 왕복은 값을 보존하고, 복원한 그래프를 다시 저장하면 같은 byte다. 경로는 정확히 4 KiB(끝이 다중
// byte 문자)까지 받는다.
TEST(ValueKinds_GraphRoundTripPreservesValuesAndResavesIdentically) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto registered = GeneratedType<AssetFields>::Register(types);
    REQUIRE(registered);
    {
        ObjectRegistry source(allocator, types, {0x5643, 2});
        ObjectRegistry target(allocator, types, {0x5643, 3});
        const StringView paths[] = {StringView("몸통.소켓"),
                                    PathOfBytes(SoftObjectRef<>::MaxObjectPathBytes)};
        for (const StringView path : paths) {
            auto created = NewFilled(source, path);
            REQUIRE(created);
            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            {
                auto borrowed =
                    target.Resolve<AssetFields>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                auto value = borrowed.Value().Get();
                REQUIRE(value);
                CHECK(value.Value()->source == SourceAsset);
                CHECK(value.Value()->hash == SampleDigest());
                CHECK(value.Value()->icon.asset == IconAsset);
                CHECK(value.Value()->icon.objectPath.View() == path);
                CHECK(value.Value()->anyAsset.asset == AnyAsset);
                CHECK(value.Value()->anyAsset.objectPath.ByteCount() == 0);
            }

            auto resaved =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(resaved);
            CHECK(resaved.Value().Size() == saved.Value().Size());
            bool same = resaved.Value().Size() == saved.Value().Size();
            for (usize index = 0; same && index < saved.Value().Size(); ++index) {
                same = saved.Value()[index] == resaved.Value()[index];
            }

            CHECK(same);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

// C++에서 public 멤버에 직접 넣은 잘못된 소프트 참조는 저장이 다시 검사해 거부하고, 임시 소유를
// 모두 돌려준다.
TEST(ValueKinds_SaveRejectsDirectlyAssignedInvalidSoftReference) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto registered = GeneratedType<AssetFields>::Register(types);
    REQUIRE(registered);
    {
        ObjectRegistry objects(allocator, types, {0x5643, 5});
        {
            auto created = NewFilled(objects, "Body.Socket");
            REQUIRE(created);
            struct Case {
                AssetId asset;
                StringView path;
            };

            const Case cases[] = {
                {AssetId{}, StringView("Orphan")},
                {IconAsset, PathOfBytes(SoftObjectRef<>::MaxObjectPathBytes + 1)}};
            for (const Case& example : cases) {
                {
                    auto borrowed = objects.Resolve<AssetFields>(objects.OwnerAccess(),
                                                                 created.Value().Handle());
                    REQUIRE(borrowed);
                    auto value = borrowed.Value().Get();
                    REQUIRE(value);
                    value.Value()->icon.asset = example.asset;
                    REQUIRE(value.Value()->icon.objectPath.TryAssign(example.path));
                }

                const auto baseline = allocator.Stats();
                const auto leases = types.Stats().leases;
                auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(),
                                             created.Value().Handle());
                CHECK(!saved && saved.Error() == ErrorCode::InvalidArgument);
                CHECK(allocator.Stats().liveAllocations == baseline.liveAllocations);
                CHECK(allocator.Stats().liveBytes == baseline.liveBytes);
                CHECK(types.Stats().leases == leases);
                CHECK(objects.Stats().borrows == 0);
            }
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

// 저장된 값의 wire 종류·길이·잘림·trailing byte·소프트 참조 규칙 오류는 객체를 공개하지 않고
// 거부한다.
TEST(ValueKinds_MalformedStoredValuesAreRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto registered = GeneratedType<AssetFields>::Register(types);
    REQUIRE(registered);
    {
        ObjectRegistry target(allocator, types, {0x5643, 4});
        ObjectGraphLoadOptions options;
        options.policy.allowMissingFields = true;
        const TypeSchema& schema = TypeOf<AssetFields>();
        const Guid source = SchemaField(schema, "source").id;
        const Guid hash = SchemaField(schema, "hash").id;
        const Guid icon = SchemaField(schema, "icon").id;
        const uint8 zeros[33] = {};
        const uint8 invalidUtf8[] = {0xff};

        // 각 payload를 만든다. writers는 아래 사례가 끝날 때까지 bytes를 소유한다.
        ArchiveWriter assetAsBytes(allocator);
        REQUIRE(assetAsBytes.WriteBytes(Span<const uint8>(zeros, 16)));
        ArchiveWriter digestAsGuid(allocator);
        REQUIRE(digestAsGuid.WriteGuid(Guid{1, 2}));
        ArchiveWriter softAsString(allocator);
        REQUIRE(softAsString.WriteString("Body.Socket"));
        ArchiveWriter shortDigest(allocator);
        REQUIRE(shortDigest.WriteBytes(Span<const uint8>(zeros, 31)));
        ArchiveWriter longDigest(allocator);
        REQUIRE(longDigest.WriteBytes(Span<const uint8>(zeros, 33)));
        ArchiveWriter trailingDigest(allocator);
        REQUIRE(trailingDigest.WriteBytes(Span<const uint8>(zeros, 32)));
        REQUIRE(trailingDigest.WriteRaw(Span<const uint8>(zeros, 1)));
        ArchiveWriter orphan(allocator);
        REQUIRE(orphan.WriteGuid(Guid{}));
        REQUIRE(orphan.WriteString("Orphan"));
        ArchiveWriter trailingSoft(allocator);
        REQUIRE(trailingSoft.WriteGuid(IconAsset.value));
        REQUIRE(trailingSoft.WriteString("Body.Socket"));
        REQUIRE(trailingSoft.WriteRaw(Span<const uint8>(zeros, 1)));
        ArchiveWriter badUtf8(allocator);
        REQUIRE(badUtf8.WriteGuid(IconAsset.value));
        REQUIRE(badUtf8.WriteU64(sizeof(invalidUtf8)));
        REQUIRE(badUtf8.WriteRaw(Span<const uint8>(invalidUtf8, sizeof(invalidUtf8))));
        ArchiveWriter truncatedSoft(allocator);
        REQUIRE(truncatedSoft.WriteGuid(IconAsset.value));
        ArchiveWriter longSoft(allocator);
        REQUIRE(longSoft.WriteGuid(IconAsset.value));
        REQUIRE(longSoft.WriteString(PathOfBytes(SoftObjectRef<>::MaxObjectPathBytes + 1)));
        auto assetAsBytesPayload = assetAsBytes.TakeBytes();
        auto digestAsGuidPayload = digestAsGuid.TakeBytes();
        auto softAsStringPayload = softAsString.TakeBytes();
        auto shortDigestPayload = shortDigest.TakeBytes();
        auto longDigestPayload = longDigest.TakeBytes();
        auto trailingDigestPayload = trailingDigest.TakeBytes();
        auto orphanPayload = orphan.TakeBytes();
        auto trailingSoftPayload = trailingSoft.TakeBytes();
        auto badUtf8Payload = badUtf8.TakeBytes();
        auto truncatedSoftPayload = truncatedSoft.TakeBytes();
        auto longSoftPayload = longSoft.TakeBytes();

        struct Case {
            Guid field;
            WireType wire;
            Span<const uint8> payload;
            ErrorCode expected;
        };

        const Case cases[] = {
            {source, WireType::Bytes, assetAsBytesPayload.View(), ErrorCode::TypeMismatch},
            {hash, WireType::Guid, digestAsGuidPayload.View(), ErrorCode::TypeMismatch},
            {icon, WireType::String, softAsStringPayload.View(), ErrorCode::TypeMismatch},
            {hash, WireType::Bytes, shortDigestPayload.View(), ErrorCode::InvalidArgument},
            {hash, WireType::Bytes, longDigestPayload.View(), ErrorCode::InvalidArgument},
            {hash, WireType::Bytes, trailingDigestPayload.View(), ErrorCode::InvalidArgument},
            {icon, WireType::SoftObjectReference, orphanPayload.View(), ErrorCode::InvalidArgument},
            {icon, WireType::SoftObjectReference, trailingSoftPayload.View(),
             ErrorCode::InvalidArgument},
            {icon, WireType::SoftObjectReference, badUtf8Payload.View(),
             ErrorCode::InvalidArgument},
            {icon, WireType::SoftObjectReference, truncatedSoftPayload.View(),
             ErrorCode::InvalidArgument},
            {icon, WireType::SoftObjectReference, longSoftPayload.View(),
             ErrorCode::InvalidArgument}};
        for (const Case& example : cases) {
            RecordBuilder record(allocator, TypeIdOf<AssetFields>(), 1);
            REQUIRE(
                record.AddField(example.field, static_cast<uint32>(example.wire), example.payload));
            auto recordBytes = record.Build();
            REQUIRE(recordBytes);
            auto graph = SingleAssetGraph(allocator, recordBytes.Value().View());
            REQUIRE(graph);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          graph.Value().View(), options);
            CHECK(!loaded && loaded.Error() == example.expected);
            CHECK(target.Stats().liveObjects == 0);
            CHECK(target.Stats().reservedObjects == 0);
        }

        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

// 저장·복원의 모든 할당 실패는 OutOfMemory이고 할당·lease·borrow·객체를 원상태로 돌린다.
TEST(ValueKinds_EverySaveAndLoadAllocationFailureRollsBack) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto registered = GeneratedType<AssetFields>::Register(types);
    REQUIRE(registered);
    {
        ObjectRegistry objects(allocator, types, {0x5643, 6});
        {
            auto created = NewFilled(objects, "몸통.소켓");
            REQUIRE(created);
            test::NFailAllocator failure(allocator);
            uint64 attempts = 0;
            {
                auto baseline = SaveObjectGraph(failure, objects, objects.OwnerAccess(),
                                                created.Value().Handle());
                REQUIRE(baseline);
                attempts = failure.Attempts();
            }

            const auto baselineStats = allocator.Stats();
            const auto baselineLeases = types.Stats().leases;
            for (uint64 index = 1; index <= attempts; ++index) {
                failure.FailOnAttempt(index);
                auto saved = SaveObjectGraph(failure, objects, objects.OwnerAccess(),
                                             created.Value().Handle());
                CHECK(!saved && saved.Error() == ErrorCode::OutOfMemory);
                CHECK(allocator.Stats().liveAllocations == baselineStats.liveAllocations);
                CHECK(allocator.Stats().liveBytes == baselineStats.liveBytes);
                CHECK(types.Stats().leases == baselineLeases);
                CHECK(objects.Stats().borrows == 0);
            }

            failure.DisableFailure();
            auto bytes = SaveObjectGraph(allocator, objects, objects.OwnerAccess(),
                                         created.Value().Handle());
            REQUIRE(bytes);
            REQUIRE(LoadWithFailure(failure, types, bytes.Value().View(), &attempts));
            const auto loadStats = allocator.Stats();
            for (uint64 index = 1; index <= attempts; ++index) {
                failure.FailOnAttempt(index);
                auto loaded = LoadWithFailure(failure, types, bytes.Value().View());
                CHECK(!loaded && loaded.Error() == ErrorCode::OutOfMemory);
                CHECK(allocator.Stats().liveAllocations == loadStats.liveAllocations);
                CHECK(allocator.Stats().liveBytes == loadStats.liveBytes);
                CHECK(types.Stats().leases == baselineLeases);
            }
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}
