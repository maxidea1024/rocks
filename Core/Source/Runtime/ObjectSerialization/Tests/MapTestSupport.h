#pragma once

#include <ReflectionTestFixtures/MapFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Object/ConstructionContext.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/MapWire.h>
#include <Serialization/Record.h>
#include <Serialization/SetWire.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9f2 Map·Set PROPERTY 시험(MapGraphTests.cpp·MapGraphLoadTests.cpp)의 공용 도우미다:
// 생성 fixture(MapFixture.h) 등록, 객체 생성·값 편집, 필드 ID, Map·Set payload·Record·그래프 조립,
// 진단 기록 hook.
namespace rocks::test::map {

using reflection_fixture::MapAccess;
using reflection_fixture::MapBag;
using reflection_fixture::MapColor;
using reflection_fixture::MapHolder;
using reflection_fixture::MapKeyBase;
using reflection_fixture::MapKeyDerived;
using reflection_fixture::MapKeyLeaf;
using reflection_fixture::MapLink;
using reflection_fixture::MapLockedHolder;
using reflection_fixture::MapPathHolder;
using reflection_fixture::MapPerPlatform;
using reflection_fixture::MapPoint;
using reflection_fixture::MapTagBag;
using reflection_fixture::MapVersioned;

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

// MapHolder 필드 ID다.
inline Guid HolderField(StringView name) noexcept {
    return SchemaField(TypeOf<MapHolder>(), name).id;
}

// MapPathHolder 필드 ID다.
inline Guid PathHolderField(StringView name) noexcept {
    return SchemaField(TypeOf<MapPathHolder>(), name).id;
}

// fixture를 쓰는 순서대로 등록한다(MapKeyLeaf·MapLockedHolder는 쓰는 시험이 따로 등록한다).
// 기록은 Shutdown이 정리한다.
inline bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<MapColor>::Register(types).IsOk() &&
           GeneratedType<MapKeyBase>::Register(types).IsOk() &&
           GeneratedType<MapKeyDerived>::Register(types).IsOk() &&
           GeneratedType<MapPoint>::Register(types).IsOk() &&
           GeneratedType<MapVersioned>::Register(types).IsOk() &&
           GeneratedType<MapLink>::Register(types).IsOk() &&
           GeneratedType<MapBag>::Register(types).IsOk() &&
           GeneratedType<MapTagBag>::Register(types).IsOk() &&
           GeneratedType<MapPerPlatform>::Register(types).IsOk() &&
           GeneratedType<MapHolder>::Register(types).IsOk() &&
           GeneratedType<MapAccess>::Register(types).IsOk() &&
           GeneratedType<MapPathHolder>::Register(types).IsOk();
}

inline RegistryOptions WithNames(NamePool* names) noexcept {
    RegistryOptions options;
    options.names = names;
    return options;
}

// root로 쥔 새 T 객체다(T는 MapHolder가 기본 — 실패하면 시험 중단).
template <class T = MapHolder>
RootRef<T> New(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<T>(objects.OwnerAccess(), TypeIdOf<T>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

// T 객체 값을 edit로 바꾸거나 본다(Borrow 동안 — T는 MapHolder가 기본).
template <class T = MapHolder, class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<T>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

// map의 key 항목을 넣는다(실패하면 시험 중단).
template <class Map, class Key, class Value>
void Put(Map& map, Key&& key, Value&& value) noexcept {
    auto inserted = map.Insert(Forward<Key>(key), Forward<Value>(value));
    if (!inserted || inserted.Value() != InsertOutcome::Inserted) {
        platform::Fatal("test map insert failed");
    }
}

// allocator로 만든 String이다.
inline String Text(Allocator& allocator, StringView text) noexcept {
    String value(allocator);
    if (!value.TryAssign(text)) {
        platform::Fatal("test string assign failed");
    }

    return value;
}

// 필드 하나(id, wire, payload)다.
struct FieldBytes {
    Guid id;
    uint32 wire = 0;
    Span<const uint8> payload;
};

// type의 Record에 fields를 순서대로 담는다.
inline Array<uint8> RecordOf(Allocator& allocator, TypeId type, uint32 version,
                             Span<const FieldBytes> fields) noexcept {
    RecordBuilder record(allocator, type, version);
    for (const FieldBytes& field : fields) {
        if (!record.AddField(field.id, field.wire, field.payload)) {
            platform::Fatal("test record field add failed");
        }
    }

    auto built = record.Build();
    if (!built) {
        platform::Fatal("test record build failed");
    }

    return Move(built.Value());
}

// MapHolder 노드 하나짜리 그래프다.
inline Array<uint8> HolderGraph(Allocator& allocator, Span<const FieldBytes> fields) noexcept {
    auto record = RecordOf(allocator, TypeIdOf<MapHolder>(), TypeOf<MapHolder>().version, fields);
    auto graph = test::graph::Single(allocator, TypeIdOf<MapHolder>(), record.View());
    if (!graph) {
        platform::Fatal("test graph build failed");
    }

    return Move(graph.Value());
}

// Map payload다: header(keyWire, valueWire, count) 뒤에 write가 항목을 쓴다(형식 검사 없이 — 손상
// 입력 조립용).
template <class Write>
Array<uint8> MapPayload(Allocator& allocator, WireType keyWire, WireType valueWire, uint32 count,
                        Write&& write) noexcept {
    ArchiveWriter payload(allocator);
    if (!payload.WriteU32(static_cast<uint32>(keyWire)) ||
        !payload.WriteU32(static_cast<uint32>(valueWire)) || !payload.WriteU32(count) ||
        !write(payload)) {
        platform::Fatal("test map payload failed");
    }

    return payload.TakeBytes();
}

// Set payload다: header(elementWire, count) 뒤에 write가 원소를 쓴다.
template <class Write>
Array<uint8> SetPayload(Allocator& allocator, WireType elementWire, uint32 count,
                        Write&& write) noexcept {
    ArchiveWriter payload(allocator);
    if (!payload.WriteU32(static_cast<uint32>(elementWire)) || !payload.WriteU32(count) ||
        !write(payload)) {
        platform::Fatal("test set payload failed");
    }

    return payload.TakeBytes();
}

// graph를 한 번 복원한 결과 코드다(성공이면 None — 만든 객체는 바로 놓는다).
inline ErrorCode LoadCode(Allocator& allocator, ObjectRegistry& objects, TypeRegistry& types,
                          Span<const uint8> graph,
                          const ObjectGraphLoadOptions& options = {}) noexcept {
    auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph, options);
    return loaded ? ErrorCode::None : loaded.Error();
}

// 1단계(Validate)가 성공하는 가장 작은 maxWork다(이분 탐색 — 기본 한도로도 실패하면 시험 실패와
// 0).
inline uint64 MinimumValidateWork(Allocator& allocator, ObjectRegistry& registry,
                                  TypeRegistry& types, Span<const uint8> graph) noexcept {
    const auto validates = [&](uint64 work) noexcept {
        ObjectGraphLoadOptions limited;
        limited.limits.maxWork = work;
        ObjectGraphStage stage(allocator, registry, types, registry.OwnerAccess(), limited);
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

// lease 스키마로 해석한 문자열 경로다(실패하면 시험 중단 — key 단계 경로 시험, 리플렉션 B9f3).
inline PropertyPathBuffer PathText(const TypeLease& lease, StringView text) noexcept {
    PropertyPathBuffer path;
    if (!ParsePropertyPath(lease, text, path)) {
        platform::Fatal("test property path parse failed");
    }

    return path;
}

// 종류 kind의 단일 값이다(정수·실수는 해당 칸 — 부호 있는 정수·enum은 signedInteger).
inline FieldValue Scalar(Allocator& allocator, ValueKind kind, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = kind;
    result.signedInteger = value;
    result.unsignedInteger = static_cast<uint64>(value);
    result.float32 = static_cast<float>(value);
    result.boolean = value != 0;
    return result;
}

// String 값이다.
inline FieldValue TextField(Allocator& allocator, StringView text) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Utf8String;
    if (!result.string.TryAssign(text)) {
        platform::Fatal("test string value assign failed");
    }

    return result;
}

// 한 단계 key 경로(field[key] — 키 wire는 wire가 담는다)다. 키 wire를 바로 만들 수 없는 종류(Soft·
// ClassRef)나 정규형이 아닌 wire를 시험할 때 쓴다.
struct KeyPath {
    explicit KeyPath(Allocator& allocator) noexcept : wire(allocator) {
    }

    Array<uint8> wire;
    PathKey keys[1];
    PathStep steps[1];

    PropertyPath View() const noexcept {
        return PropertyPath{Span<const PathStep>(steps, 1), Span<const PathKey>(keys, 1)};
    }
};

// field[key] 경로를 path에 만든다(AppendPathKey가 실패하면 시험 중단).
inline void MakeKeyPath(KeyPath& path, Guid field, const PathKeyValue& key) noexcept {
    path.wire.Clear();
    if (!AppendPathKey(key, path.wire)) {
        platform::Fatal("test path key encode failed");
    }

    path.keys[0] = PathKey{key.kind, path.wire.View()};
    path.steps[0] = PathStep{field, 0, PathSelector::Key};
}

// 진단과 소프트 참조 수집을 기록한다(진단은 경로 전체, 소프트 참조는 경로의 첫 단계만 둔다).
struct Recorder final : FieldCodecHooks {
    static constexpr uint32 Capacity = 32;

    struct Entry {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        Guid field{};
        int64 value = 0;
        uint32 steps = 0;
        PathStep path[MaxPropertyPathSteps] = {};
    };

    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count >= Capacity || diagnostic.path.Size() > MaxPropertyPathSteps) {
            CHECK(false);
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        Entry& entry = entries[count++];
        entry.kind = diagnostic.kind;
        entry.field = diagnostic.field;
        entry.value = diagnostic.value;
        entry.steps = static_cast<uint32>(diagnostic.path.Size());
        for (usize index = 0; index < diagnostic.path.Size(); ++index) {
            entry.path[index] = diagnostic.path[index];
        }

        return Result<void>::Success();
    }

    Result<void> CollectSoftReference(const SoftReferenceInfo& info) noexcept override {
        if (softCount >= Capacity || info.path.Size() == 0) {
            CHECK(false);
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        softPaths[softCount++] = info.path[0];
        return Result<void>::Success();
    }

    Entry entries[Capacity] = {};
    uint32 count = 0;
    PathStep softPaths[Capacity] = {};
    uint32 softCount = 0;
};

} // namespace rocks::test::map
