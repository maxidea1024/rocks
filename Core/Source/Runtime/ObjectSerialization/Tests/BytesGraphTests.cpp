#include <ReflectionTestFixtures/BytesFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// `Array<uint8>`의 Bytes 블록 wire 시험이다(Docs/Design/CurveData.ko.md D18 — CV4, fixture
// ReflectionTestFixtures/BytesFixture.h): 클래스 필드·struct 멤버·struct 원소 배열 안 멤버의 왕복과
// 다시 저장한 byte 같음(빈 배열·큰 배열·MaxArrayElements 경계), 저장 크기가 원소 수 + 상수인지,
// 필드 MaxElements·MaxArrayElements 초과 저장 거부, 변조한 블록 길이의 로드 거부, 정적 배열
// `uint8 x[4]`은 원소마다(ValueArray) 그대로인지, 경로 원소 읽기·쓰기(`payload[3]`), 2단계 할당
// 실패의 그래프 rollback.
using namespace rocks;
using reflection_fixture::BytesBlob;
using reflection_fixture::BytesHolder;

namespace {

bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<BytesBlob>::Register(types).IsOk() &&
           GeneratedType<BytesHolder>::Register(types).IsOk();
}

RootRef<BytesHolder> New(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<BytesHolder>(objects.OwnerAccess(), TypeIdOf<BytesHolder>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

template <class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<BytesHolder>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

// count byte를 (index·7 + seed) 패턴으로 채운다.
bool Fill(Array<uint8>& bytes, usize count, uint8 seed) noexcept {
    bytes.Clear();
    if (!bytes.TryReserve(count)) {
        return false;
    }

    for (usize index = 0; index < count; ++index) {
        if (!bytes.TryPushBack(static_cast<uint8>(index * 7u + seed))) {
            return false;
        }
    }

    return true;
}

bool SamePattern(const Array<uint8>& bytes, usize count, uint8 seed) noexcept {
    if (bytes.Size() != count) {
        return false;
    }

    for (usize index = 0; index < count; ++index) {
        if (bytes[index] != static_cast<uint8>(index * 7u + seed)) {
            return false;
        }
    }

    return true;
}

// payload만 count byte로 채운 holder의 저장 크기다(실패하면 0).
usize SavedSize(Allocator& allocator, TypeRegistry& types, usize count) noexcept {
    ObjectRegistry objects(allocator, types, {0x4259, 9});
    usize size = 0;
    {
        RootRef<BytesHolder> holder = New(objects);
        With(objects, holder.Handle(),
             [&](BytesHolder& value) noexcept { CHECK(Fill(value.payload, count, 3)); });
        auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
        CHECK(saved);
        size = saved ? saved.Value().Size() : 0;
    }

    CHECK(objects.Shutdown(objects.OwnerAccess()));
    return size;
}

Result<void> LoadWithFailure(test::NFailAllocator& allocator, TypeRegistry& types,
                             Span<const uint8> bytes, uint64* attempts = nullptr) noexcept {
    ObjectRegistry registry(allocator, types, {0x4259, 90});
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
    if (auto collected = registry.Collect(registry.OwnerAccess()); !collected) {
        return Result<void>::Failure(collected.Error());
    }

    return registry.Shutdown(registry.OwnerAccess());
}

// haystack에서 needle이 처음 나오는 위치다(없으면 IndexNone).
usize Find(Span<const uint8> haystack, Span<const uint8> needle) noexcept {
    for (usize start = 0; start + needle.Size() <= haystack.Size(); ++start) {
        bool same = true;
        for (usize index = 0; same && index < needle.Size(); ++index) {
            same = haystack[start + index] == needle[index];
        }

        if (same) {
            return start;
        }
    }

    return IndexNone;
}

} // namespace

// 모든 자리가 왕복하고 다시 저장한 byte가 같다: 빈 배열, 큰 배열(100,000 byte), struct 멤버, struct
// 원소 배열 안 멤버, 정적 배열 원소.
TEST(BytesGraph_RoundTripEveryPosition) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x4258, 1});
        ObjectRegistry target(allocator, types, {0x4258, 2});
        {
            RootRef<BytesHolder> holder = New(source);
            With(source, holder.Handle(), [&](BytesHolder& value) noexcept {
                REQUIRE(Fill(value.payload, 100000, 1));
                REQUIRE(Fill(value.blob.data, 33, 2));
                value.blob.tag = 7;
                REQUIRE(value.blobs.TryPushBack(BytesBlob(allocator)));
                REQUIRE(value.blobs.TryPushBack(BytesBlob(allocator)));
                REQUIRE(Fill(value.blobs[1].data, 5, 9));
                value.fixed[0] = 1;
                value.fixed[3] = 255;
                value.plain = -4;
            });
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            With(target, loaded.Value().Handle(), [](BytesHolder& value) noexcept {
                CHECK(SamePattern(value.payload, 100000, 1));
                CHECK(value.small.Size() == 0);
                CHECK(SamePattern(value.blob.data, 33, 2) && value.blob.tag == 7);
                REQUIRE(value.blobs.Size() == 2);
                CHECK(value.blobs[0].data.Size() == 0 && SamePattern(value.blobs[1].data, 5, 9));
                CHECK(value.fixed[0] == 1 && value.fixed[1] == 0 && value.fixed[3] == 255);
                CHECK(value.plain == -4);
            });
            auto again =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(again);
            CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }
}

// 저장 크기는 원소 수 + 상수다(원소마다 1 byte — 원소별 U32 4 byte가 아니다).
TEST(BytesGraph_WireSizeIsByteCountPlusConstant) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    const usize empty = SavedSize(allocator, types, 0);
    REQUIRE(empty != 0);
    CHECK(SavedSize(allocator, types, 1) == empty + 1);
    CHECK(SavedSize(allocator, types, 1000) == empty + 1000);
    CHECK(SavedSize(allocator, types, 65536) == empty + 65536);
}

// 한도: 필드 MaxElements(8)는 저장에서 걸고(9 byte CapacityExceeded), 한도 없는 필드는
// MaxArrayElements 까지 왕복하고 하나 넘으면 저장이 CapacityExceeded다.
TEST(BytesGraph_LimitsMatchArrayRules) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x425A, 1});
        ObjectRegistry target(allocator, types, {0x425A, 2});
        {
            RootRef<BytesHolder> holder = New(source);
            With(source, holder.Handle(),
                 [](BytesHolder& value) noexcept { REQUIRE(Fill(value.small, 8, 4)); });
            REQUIRE(SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle()));
            With(source, holder.Handle(),
                 [](BytesHolder& value) noexcept { REQUIRE(Fill(value.small, 9, 4)); });
            auto overSmall =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            CHECK(!overSmall && overSmall.Error() == ErrorCode::CapacityExceeded);
            With(source, holder.Handle(), [](BytesHolder& value) noexcept {
                value.small.Clear();
                REQUIRE(Fill(value.payload, MaxArrayElements, 5));
            });
            auto atLimit =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(atLimit);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          atLimit.Value().View());
            REQUIRE(loaded);
            With(target, loaded.Value().Handle(), [](BytesHolder& value) noexcept {
                CHECK(SamePattern(value.payload, MaxArrayElements, 5));
            });
            With(source, holder.Handle(),
                 [](BytesHolder& value) noexcept { REQUIRE(value.payload.TryPushBack(uint8{0})); });
            auto over = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            CHECK(!over && over.Error() == ErrorCode::CapacityExceeded);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }
}

// 블록 길이를 바꾼 로드는 거부된다: 남은 byte보다 길면 잘린 입력, 짧으면 필드 payload 안에 남는
// byte라 둘 다 InvalidArgument이고 아무것도 게시하지 않는다.
TEST(BytesGraph_TamperedLengthIsRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x425B, 1});
        ObjectRegistry target(allocator, types, {0x425B, 2});
        {
            RootRef<BytesHolder> holder = New(source);
            With(source, holder.Handle(), [](BytesHolder& value) noexcept {
                value.payload.Clear();
                for (int32 index = 0; index < 37; ++index) {
                    REQUIRE(value.payload.TryPushBack(uint8{0xAB}));
                }
            });
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            // 블록은 u64 길이 37(리틀 엔디언) + 0xAB × 37이다.
            uint8 block[8 + 37] = {37, 0, 0, 0, 0, 0, 0, 0};
            for (usize index = 8; index < sizeof(block); ++index) {
                block[index] = 0xAB;
            }

            const usize at = Find(saved.Value().View(), Span<const uint8>(block, sizeof(block)));
            REQUIRE(at != IndexNone);
            const uint32 before = target.Stats().liveObjects;
            const uint8 lengths[] = {38, 36, 0xFF};
            for (const uint8 length : lengths) {
                Array<uint8> tampered(allocator);
                REQUIRE(tampered.TryAppend(saved.Value().View()));
                tampered[at] = length;
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              tampered.View());
                CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
                CHECK(target.Stats().liveObjects == before);
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }
}

// 경로 원소 연산은 지금 그대로다(`payload[3]` 읽기·쓰기, 끝에 원소 넣기·지우기, 원소 수).
TEST(BytesGraph_PathElementAccess) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x425C, 1});
        {
            RootRef<BytesHolder> holder = New(objects);
            const ObjectHandle handle = holder.Handle();
            With(objects, handle,
                 [](BytesHolder& value) noexcept { REQUIRE(Fill(value.payload, 6, 0)); });
            PropertyPathBuffer element;
            REQUIRE(
                ParsePropertyPath(objects, objects.OwnerAccess(), handle, "payload[3]", element));
            FieldValue input(allocator);
            input.kind = ValueKind::UInt8;
            input.unsignedInteger = 200;
            REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, element.View(), input));
            FieldValue read(allocator);
            REQUIRE(PathRead(objects, objects.OwnerAccess(), handle, element.View(), read));
            CHECK(read.kind == ValueKind::UInt8 && read.unsignedInteger == 200);
            input.unsignedInteger = 256;
            CHECK(!PathWrite(objects, objects.OwnerAccess(), handle, element.View(), input));

            PropertyPathBuffer tail;
            REQUIRE(ParsePropertyPath(objects, objects.OwnerAccess(), handle, "payload[6]", tail));
            input.unsignedInteger = 0;
            REQUIRE(PathInsertElement(objects, objects.OwnerAccess(), handle, tail.View(), &input));
            PropertyPathBuffer whole;
            REQUIRE(ParsePropertyPath(objects, objects.OwnerAccess(), handle, "payload", whole));
            auto count = PathCount(objects, objects.OwnerAccess(), handle, whole.View());
            REQUIRE(count);
            CHECK(count.Value() == 7);
            PropertyPathBuffer first;
            REQUIRE(ParsePropertyPath(objects, objects.OwnerAccess(), handle, "payload[0]", first));
            REQUIRE(PathRemoveElement(objects, objects.OwnerAccess(), handle, first.View()));
            With(objects, handle, [](BytesHolder& value) noexcept {
                REQUIRE(value.payload.Size() == 6);
                CHECK(value.payload[2] == 200 && value.payload[5] == 0);
            });
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }
}

// 로드의 모든 할당 실패(2단계 블록 복사 포함)는 OutOfMemory이고 부분 객체·할당이 남지 않는다.
TEST(BytesGraph_EveryLoadAllocationFailureRollsBack) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x425D, 1});
        {
            RootRef<BytesHolder> holder = New(objects);
            With(objects, holder.Handle(), [&](BytesHolder& value) noexcept {
                REQUIRE(Fill(value.payload, 300, 1));
                REQUIRE(Fill(value.blob.data, 20, 2));
                REQUIRE(value.blobs.TryPushBack(BytesBlob(allocator)));
                REQUIRE(Fill(value.blobs[0].data, 10, 3));
            });
            auto bytes =
                SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
            REQUIRE(bytes);
            test::NFailAllocator failure(allocator);
            uint64 attempts = 0;
            REQUIRE(LoadWithFailure(failure, types, bytes.Value().View(), &attempts));
            const auto loadStats = allocator.Stats();
            bool clean = true;
            for (uint64 index = 1; index <= attempts; ++index) {
                failure.FailOnAttempt(index);
                auto loaded = LoadWithFailure(failure, types, bytes.Value().View());
                clean = clean && !loaded && loaded.Error() == ErrorCode::OutOfMemory &&
                        allocator.Stats().liveAllocations == loadStats.liveAllocations;
            }

            CHECK(clean);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }
}

namespace {

// value를 count개 채운 BytesBlob이다.
BytesBlob BlobOf(Allocator& allocator, usize count, uint8 value) noexcept {
    BytesBlob blob(allocator);
    if (!blob.data.TryReserve(count)) {
        platform::Fatal("test blob reserve failed");
    }

    for (usize index = 0; index < count; ++index) {
        static_cast<void>(blob.data.TryPushBack(value));
    }

    return blob;
}

// u64 길이 count + value × count 블록의 위치다(없으면 IndexNone).
usize FindBlock(Span<const uint8> bytes, usize count, uint8 value, Allocator& allocator) noexcept {
    Array<uint8> block(allocator);
    if (!block.TryResize(8 + count)) {
        return IndexNone;
    }

    for (usize index = 0; index < 8; ++index) {
        block[index] = static_cast<uint8>((uint64{count} >> (8 * index)) & 0xFF);
    }

    for (usize index = 0; index < count; ++index) {
        block[8 + index] = value;
    }

    return Find(bytes, block.View());
}

} // namespace

// Optional·Map 값·InstancedStruct 내용 안 struct 멤버의 Array<uint8>도 Bytes 블록으로 왕복하고(다시
// 저장 byte 같음), 그 블록 길이를 바꾼 입력은 로드가 InvalidArgument이며 아무것도 남기지 않는다(CV4
// 리뷰 7).
TEST(BytesGraph_NestedPositionsRoundTripAndRejectTampering) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x425D, 1});
        ObjectRegistry target(allocator, types, {0x425D, 2});
        {
            RootRef<BytesHolder> holder = New(source);
            With(source, holder.Handle(), [&](BytesHolder& value) noexcept {
                value.maybe.Emplace(BlobOf(allocator, 11, 0xC1));
                REQUIRE(value.byKey.Insert(int32{3}, BlobOf(allocator, 13, 0xC2)));
                REQUIRE(value.any.Emplace<BytesBlob>(types, allocator));
                BytesBlob* inner = value.any.Get<BytesBlob>();
                REQUIRE(inner != nullptr);
                *inner = BlobOf(allocator, 17, 0xC3);
            });
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            With(target, loaded.Value().Handle(), [](BytesHolder& value) noexcept {
                REQUIRE(value.maybe.HasValue() && value.byKey.Find(3) != nullptr &&
                        value.any.Get<BytesBlob>() != nullptr);
                CHECK(value.maybe.Value().data.Size() == 11 &&
                      value.maybe.Value().data[10] == 0xC1);
                CHECK(value.byKey.Find(3)->data.Size() == 13 &&
                      value.byKey.Find(3)->data[0] == 0xC2);
                CHECK(value.any.Get<BytesBlob>()->data.Size() == 17 &&
                      value.any.Get<BytesBlob>()->data[16] == 0xC3);
            });
            auto again =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(again);
            CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
            loaded.Value().Reset();
            REQUIRE(target.Collect(target.OwnerAccess()));

            struct Block {
                usize count;
                uint8 value;
            };
            const Block blocks[] = {{11, 0xC1}, {13, 0xC2}, {17, 0xC3}};
            const uint32 before = target.Stats().liveObjects;
            for (const Block& block : blocks) {
                const usize at =
                    FindBlock(saved.Value().View(), block.count, block.value, allocator);
                REQUIRE(at != IndexNone);
                const uint8 lengths[] = {static_cast<uint8>(block.count + 1),
                                         static_cast<uint8>(block.count - 1)};
                for (const uint8 length : lengths) {
                    Array<uint8> tampered(allocator);
                    REQUIRE(tampered.TryAppend(saved.Value().View()));
                    tampered[at] = length;
                    auto rejected = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                                    tampered.View());
                    CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
                    CHECK(target.Stats().liveObjects == before);
                }
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }
}

// 로드 쪽 한도(CV4 리뷰 7): 블록 크기가 값 byte 한도(maxValueBytes)를 넘으면 CapacityExceeded이고,
// MaxArrayElements + 1 byte 블록(손으로 만든 Record — 저장은 이런 값을 만들지 않는다)도
// CapacityExceeded다. 둘 다 객체를 남기지 않는다.
TEST(BytesGraph_LoadLimits) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x425E, 1});
        ObjectRegistry target(allocator, types, {0x425E, 2});
        {
            RootRef<BytesHolder> holder = New(source);
            With(source, holder.Handle(),
                 [](BytesHolder& value) noexcept { REQUIRE(Fill(value.payload, 2000, 1)); });
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            ObjectGraphLoadOptions small;
            small.limits.maxValueBytes = 1024;
            auto over = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                        saved.Value().View(), small);
            CHECK(!over && over.Error() == ErrorCode::CapacityExceeded);
            CHECK(target.Stats().liveObjects == 0);

            Array<uint8> block(allocator);
            REQUIRE(block.TryResize(MaxArrayElements + 1));
            for (usize index = 0; index < block.Size(); ++index) {
                block[index] = 0x5A;
            }

            ArchiveWriter writer(allocator);
            REQUIRE(writer.WriteBytes(block.View()));
            Array<uint8> payload = writer.TakeBytes();
            Guid payloadId;
            for (const FieldSchema& field : GeneratedType<BytesHolder>::Schema().fields) {
                if (StringView(field.name.Data(), field.name.Size()) == StringView("payload")) {
                    payloadId = field.id;
                }
            }

            RecordBuilder record(allocator, TypeIdOf<BytesHolder>(),
                                 GeneratedType<BytesHolder>::Schema().version);
            REQUIRE(
                record.AddField(payloadId, static_cast<uint32>(WireType::Bytes), payload.View()));
            auto built = record.Build();
            REQUIRE(built);
            auto graph =
                test::graph::Single(allocator, TypeIdOf<BytesHolder>(), built.Value().View());
            REQUIRE(graph);
            auto tooMany = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                           graph.Value().View());
            CHECK(!tooMany && tooMany.Error() == ErrorCode::CapacityExceeded);
            CHECK(target.Stats().liveObjects == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }
}
