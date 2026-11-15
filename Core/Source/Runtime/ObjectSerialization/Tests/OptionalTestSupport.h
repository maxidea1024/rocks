#pragma once

#include <ReflectionTestFixtures/OptionalFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Object/ConstructionContext.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/OptionalWire.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9e Optional PROPERTY 시험(OptionalGraphTests.cpp·OptionalFieldTests.cpp)의 공용
// 도우미다: 생성 fixture(OptionalFixture.h) 등록, 객체 생성·값 편집, 필드 ID·FieldValue, Optional
// payload·Record 조립.
namespace rocks::test::optional {

using reflection_fixture::OptionalEmpty;
using reflection_fixture::OptionalHolder;
using reflection_fixture::OptionalLink;
using reflection_fixture::OptionalMode;
using reflection_fixture::OptionalPoint;
using reflection_fixture::OptionalSlot;
using reflection_fixture::OptionalTag;
using reflection_fixture::OptionalVersioned;

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

// OptionalHolder 필드 ID다.
inline Guid HolderField(StringView name) noexcept {
    return SchemaField(TypeOf<OptionalHolder>(), name).id;
}

// OptionalPoint 멤버 ID다.
inline Guid PointField(StringView name) noexcept {
    return SchemaField(TypeOf<OptionalPoint>(), name).id;
}

// fixture를 쓰는 순서대로 등록한다. 기록은 Shutdown이 정리한다.
inline bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<OptionalMode>::Register(types).IsOk() &&
           GeneratedType<OptionalPoint>::Register(types).IsOk() &&
           GeneratedType<OptionalSlot>::Register(types).IsOk() &&
           GeneratedType<OptionalVersioned>::Register(types).IsOk() &&
           GeneratedType<OptionalTag>::Register(types).IsOk() &&
           GeneratedType<OptionalLink>::Register(types).IsOk() &&
           GeneratedType<OptionalEmpty>::Register(types).IsOk() &&
           GeneratedType<OptionalHolder>::Register(types).IsOk();
}

inline RegistryOptions WithNames(NamePool* names) noexcept {
    RegistryOptions options;
    options.names = names;
    return options;
}

// root로 쥔 새 객체다(실패하면 시험 중단).
inline RootRef<OptionalHolder> New(ObjectRegistry& objects) noexcept {
    auto created =
        objects.NewObject<OptionalHolder>(objects.OwnerAccess(), TypeIdOf<OptionalHolder>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

// 객체 값을 edit로 바꾸거나 본다(Borrow 동안).
template <class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<OptionalHolder>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

inline FieldValue U32Value(Allocator& allocator, uint64 value, bool present = true) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::UInt32;
    result.unsignedInteger = value;
    result.present = present;
    return result;
}

inline FieldValue TextValue(Allocator& allocator, StringView text, bool present = true) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Utf8String;
    result.present = present;
    if (present && !result.string.TryAssign(text)) {
        platform::Fatal("test string assign failed");
    }

    return result;
}

// Optional payload다: present 0, 또는 present 1 + innerWire + write가 쓰는 내부 값.
template <class Write>
Array<uint8> OptionalPayload(Allocator& allocator, bool present, WireType innerWire,
                             Write&& write) noexcept {
    ArchiveWriter payload(allocator);
    if (!present) {
        if (!OptionalWireWriter::WriteUnset(payload)) {
            platform::Fatal("test optional unset failed");
        }

        return payload.TakeBytes();
    }

    auto set = OptionalWireWriter::BeginSet(payload, innerWire);
    if (!set || !write(payload) || !set.Value().End()) {
        platform::Fatal("test optional payload failed");
    }

    return payload.TakeBytes();
}

// 1단계(Validate)가 성공하는 가장 작은 maxWork다(이분 탐색 — 기본 한도로도 실패하면 시험 실패와
// 0). options의 다른 칸(hooks 등)은 그대로 쓴다.
inline uint64 MinimumValidateWork(Allocator& allocator, ObjectRegistry& registry,
                                  TypeRegistry& types, Span<const uint8> graph,
                                  ObjectGraphLoadOptions options = {}) noexcept {
    const auto validates = [&](uint64 work) noexcept {
        ObjectGraphLoadOptions limited = options;
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

// OptionalHolder 노드 하나짜리 그래프다.
inline Array<uint8> HolderGraph(Allocator& allocator, Span<const FieldBytes> fields) noexcept {
    auto record =
        RecordOf(allocator, TypeIdOf<OptionalHolder>(), TypeOf<OptionalHolder>().version, fields);
    auto graph = test::graph::Single(allocator, TypeIdOf<OptionalHolder>(), record.View());
    if (!graph) {
        platform::Fatal("test graph build failed");
    }

    return Move(graph.Value());
}

} // namespace rocks::test::optional
