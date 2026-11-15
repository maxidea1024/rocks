#pragma once

#include <ReflectionTestFixtures/NameFieldFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Object/ConstructionContext.h>
#include <Object/NameField.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <Serialization/ValueArray.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9c2 Name 필드 그래프 시험(NameGraphTests.cpp·NameGraphLoadTests.cpp)의 공용 도우미다:
// 생성 fixture(NameFieldFixture.h) 등록, pool·토큰 도우미, 단일 필드 그래프 조립(손상 입력 포함),
// 편의 복원, 1단계 최소 작업량 탐색, 관찰 hook.
namespace rocks::test::names {

using reflection_fixture::NameFields;
using reflection_fixture::NameGroup;
using reflection_fixture::NameMigrated;
using reflection_fixture::NamePlain;
using reflection_fixture::NameSocket;
using reflection_fixture::NameVersioned;
using reflection_fixture::NameWrapper;

inline ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

inline const FieldSchema& SchemaField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field;
        }
    }

    platform::Fatal("fixture field missing");
}

// fixture struct·클래스를 쓰는 순서대로 등록한다. 기록은 Shutdown이 정리한다.
inline bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<NamePlain>::Register(types).IsOk() &&
           GeneratedType<NameSocket>::Register(types).IsOk() &&
           GeneratedType<NameGroup>::Register(types).IsOk() &&
           GeneratedType<NameWrapper>::Register(types).IsOk() &&
           GeneratedType<NameVersioned>::Register(types).IsOk() &&
           GeneratedType<NameFields>::Register(types).IsOk() &&
           GeneratedType<NameMigrated>::Register(types).IsOk();
}

// 저장 Record를 targetVersion으로 올린다(필드를 그대로 옮긴다 — 노드·struct 마이그레이션 공용).
// user는 호출 수다.
inline Result<Array<uint8>> CopyFields(Allocator& allocator, TypeId type, uint32 sourceVersion,
                                       uint32 targetVersion, Span<const uint8> source,
                                       void* user) noexcept {
    ++*static_cast<uint32*>(user);
    auto reader = RecordReader::Open(allocator, source, type, sourceVersion, sourceVersion);
    if (!reader) {
        return Result<Array<uint8>>::Failure(reader.Error());
    }

    RecordBuilder builder(allocator, type, targetVersion);
    for (usize index = 0; index < reader.Value().FieldCount(); ++index) {
        auto field = reader.Value().Next();
        if (!field) {
            return Result<Array<uint8>>::Failure(field.Error());
        }

        if (auto added =
                builder.AddField(field.Value().id, field.Value().wireType, field.Value().payload);
            !added) {
            return Result<Array<uint8>>::Failure(added.Error());
        }
    }

    return builder.Build();
}

inline RegistryOptions WithNames(NamePool* names) noexcept {
    RegistryOptions options;
    options.names = names;
    return options;
}

inline Name Intern(NamePool& pool, StringView text) noexcept {
    auto interned = pool.TryIntern(text);
    if (!interned) {
        platform::Fatal("test name intern failed");
    }

    return interned.Value();
}

// pool에서 token의 표시 문자열이 expected인지다(lease는 바로 놓는다).
inline bool DisplayIs(NamePool& pool, Name token, StringView expected) noexcept {
    auto lease = pool.Acquire(token);
    return lease && lease.Value().View() == expected;
}

inline FieldValue NameValue(Allocator& allocator, Name token) noexcept {
    FieldValue value(allocator);
    value.kind = ValueKind::Name;
    value.name = token;
    return value;
}

// 새 NameFields 객체의 값을 fill로 채운다.
template <class Fill>
Result<RootRef<NameFields>> NewFields(ObjectRegistry& objects, Fill&& fill) noexcept {
    auto created = objects.NewObject<NameFields>(objects.OwnerAccess(), TypeIdOf<NameFields>());
    if (!created) {
        return created;
    }

    auto borrowed = objects.Resolve<NameFields>(objects.OwnerAccess(), created.Value().Handle());
    if (!borrowed) {
        return Result<RootRef<NameFields>>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<RootRef<NameFields>>::Failure(value.Error());
    }

    if (!fill(*value.Value())) {
        return Result<RootRef<NameFields>>::Failure(ErrorCode::OutOfMemory);
    }

    return created;
}

// 객체의 값을 inspect로 본다(Borrow 동안).
template <class Inspect>
void Look(ObjectRegistry& objects, ObjectHandle handle, Inspect&& inspect) noexcept {
    auto borrowed = objects.Resolve<NameFields>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    inspect(*value.Value());
}

// 필드 field 하나만 담은 NameFields Record의 단일 노드 그래프다.
inline Array<uint8> SingleFieldGraph(Allocator& allocator, StringView field, WireType wire,
                                     Span<const uint8> payload) noexcept {
    RecordBuilder record(allocator, TypeIdOf<NameFields>(), 1);
    if (!record.AddField(SchemaField(TypeOf<NameFields>(), field).id, static_cast<uint32>(wire),
                         payload)) {
        platform::Fatal("test record field add failed");
    }

    auto recordBytes = record.Build();
    if (!recordBytes) {
        platform::Fatal("test record build failed");
    }

    auto graph = test::graph::Single(allocator, TypeIdOf<NameFields>(), recordBytes.Value().View());
    if (!graph) {
        platform::Fatal("test graph build failed");
    }

    return Move(graph.Value());
}

// 그래프를 편의 복원으로 로드한 결과다. 실패하면 객체가 공개·예약되지 않았는지 본다.
inline ErrorCode LoadGraph(Allocator& allocator, TypeRegistry& types, ObjectRegistry& target,
                           Span<const uint8> graph, ObjectGraphLoadOptions options = {}) noexcept {
    const auto before = target.Stats().liveObjects;
    auto loaded =
        LoadObjectGraph(allocator, target, types, target.OwnerAccess(), graph, Move(options));
    if (!loaded) {
        CHECK(target.Stats().liveObjects == before);
        CHECK(target.Stats().reservedObjects == 0);
        return loaded.Error();
    }

    return ErrorCode::None;
}

// String wire 값 하나(u64 길이 + byte)를 검사 없이 쓴다(WriteString은 잘못된 UTF-8을 거부하므로
// 손상 입력은 직접 조립한다).
inline void WriteRawString(ArchiveWriter& out, StringView text) noexcept {
    if (!out.WriteU64(text.ByteCount()) ||
        !out.WriteRaw({reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()})) {
        platform::Fatal("test payload write failed");
    }
}

// String 값 하나의 payload다.
inline Array<uint8> StringPayload(Allocator& allocator, StringView text) noexcept {
    ArchiveWriter payload(allocator);
    WriteRawString(payload, text);
    return payload.TakeBytes();
}

// String 원소 ValueArray payload다(원소 count개 — 모두 text).
inline Array<uint8> StringArrayPayload(Allocator& allocator, StringView text,
                                       uint32 count) noexcept {
    ArchiveWriter payload(allocator);
    auto writer =
        ValueArrayWriter::Begin(payload, WireType::String, count, NumericLimits<usize>::Max());
    if (!writer) {
        platform::Fatal("test value array begin failed");
    }

    for (uint32 index = 0; index < count; ++index) {
        WriteRawString(payload, text);
    }

    if (!writer.Value().End()) {
        platform::Fatal("test value array end failed");
    }

    return payload.TakeBytes();
}

// unit을 count번 이은 문자열이다.
inline Array<char> Repeat(Allocator& allocator, StringView unit, usize count) noexcept {
    Array<char> text(allocator);
    for (usize index = 0; index < count; ++index) {
        for (usize offset = 0; offset < unit.ByteCount(); ++offset) {
            if (!text.TryPushBack(unit.Data()[offset])) {
                platform::Fatal("test text allocation failed");
            }
        }
    }

    return text;
}

inline StringView ViewOf(const Array<char>& text) noexcept {
    return StringView(text.Data(), text.Size());
}

// 1단계(Validate)가 성공하는 가장 작은 maxWork다(이분 탐색 — 기본 한도로도 실패하면 시험 실패와
// 0).
inline uint64 MinimumValidateWork(Allocator& allocator, ObjectRegistry& registry,
                                  TypeRegistry& types, Span<const uint8> graph) noexcept {
    const auto validates = [&](uint64 work) noexcept {
        ObjectGraphLoadOptions options;
        options.limits.maxWork = work;
        ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess(), options);
        ObjectGraphInput input;
        input.bytes = graph;
        return stage.Validate(input).IsOk();
    };

    uint64 low = 0;
    uint64 high = ObjectGraphLimits{}.maxWork;
    if (!validates(high)) {
        CHECK(false);
        return 0;
    }

    while (low + 1 < high) {
        const uint64 middle = low + (high - low) / 2;
        if (validates(middle)) {
            high = middle;
        } else {
            low = middle;
        }
    }

    return high;
}

// 관찰 대상 struct 하나를 요청하는 hook이다.
struct ObserveOne final : FieldCodecHooks {
    explicit ObserveOne(TypeId type) noexcept : id(type) {
    }

    Span<const TypeId> ObservedStructs() noexcept override {
        return Span<const TypeId>(&id, 1);
    }

    Result<void> ObserveStruct(const FieldCodecFrame&, TypeId type, const void*,
                               const void* value) noexcept override {
        if (type == TypeIdOf<NamePlain>()) {
            observedValue = static_cast<const NamePlain*>(value)->value;
        }

        ++observed;
        return Result<void>::Success();
    }

    TypeId id;
    uint32 observed = 0;
    uint32 observedValue = 0;
};

} // namespace rocks::test::names
