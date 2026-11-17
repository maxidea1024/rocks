#pragma once

#include <ReflectionTestFixtures/InstancedStructFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <Serialization/ValueArray.h>
#include <Serialization/WireSlot.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9j InstancedStruct PROPERTY 시험(InstancedStructGraphTests.cpp·
// InstancedStructPathTests.cpp)의 공용 도우미다: 생성 fixture(InstancedStructFixture.h) 등록, 객체
// 생성·값 편집, 필드 ID, Record·그래프 조립, 진단 기록 hook.
namespace rocks::test::instanced {

using reflection_fixture::InstancedDepth1;
using reflection_fixture::InstancedDepth15;
using reflection_fixture::InstancedDepth16;
using reflection_fixture::InstancedHolder;
using reflection_fixture::InstancedHost;
using reflection_fixture::InstancedLocked;
using reflection_fixture::InstancedNested;
using reflection_fixture::InstancedPoint;
using reflection_fixture::InstancedRefArrayHolder;
using reflection_fixture::InstancedRefHolder;
using reflection_fixture::InstancedRequiredLink;
using reflection_fixture::InstancedScale;
using reflection_fixture::InstancedVersioned;
using reflection_fixture::InstancedWorldBound;

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

template <class T>
Guid FieldOf(StringView name) noexcept {
    return SchemaField(GeneratedType<T>::Schema(), name).id;
}

// 깊이 사슬(InstancedDepth1..16)을 등록한다.
inline bool RegisterDepthChain(TypeRegistry& types) noexcept {
    using namespace reflection_fixture;
    return GeneratedType<InstancedDepth1>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth2>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth3>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth4>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth5>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth6>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth7>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth8>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth9>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth10>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth11>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth12>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth13>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth14>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth15>::Register(types).IsOk() &&
           GeneratedType<InstancedDepth16>::Register(types).IsOk();
}

// fixture를 등록한다(point = false면 InstancedPoint를 등록하지 않는다 — 미등록 struct 정책 시험).
// 기록은 Shutdown이 정리한다.
inline bool RegisterFixture(TypeRegistry& types, bool point = true) noexcept {
    return (!point || GeneratedType<InstancedPoint>::Register(types).IsOk()) &&
           GeneratedType<InstancedScale>::Register(types).IsOk() &&
           GeneratedType<InstancedVersioned>::Register(types).IsOk() &&
           GeneratedType<InstancedRefHolder>::Register(types).IsOk() &&
           GeneratedType<InstancedRequiredLink>::Register(types).IsOk() &&
           GeneratedType<InstancedNested>::Register(types).IsOk() &&
           GeneratedType<InstancedRefArrayHolder>::Register(types).IsOk() &&
           GeneratedType<InstancedWorldBound>::Register(types).IsOk() &&
           GeneratedType<InstancedLocked>::Register(types).IsOk() &&
           GeneratedType<InstancedHost>::Register(types).IsOk() && RegisterDepthChain(types) &&
           GeneratedType<InstancedHolder>::Register(types).IsOk();
}

// root로 쥔 새 객체다(실패하면 시험 중단).
inline RootRef<InstancedHolder> New(ObjectRegistry& objects) noexcept {
    auto created =
        objects.NewObject<InstancedHolder>(objects.OwnerAccess(), TypeIdOf<InstancedHolder>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

// 객체 값을 edit로 바꾸거나 본다(Borrow 동안).
template <class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<InstancedHolder>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

// value에 S의 기본 값을 넣고 그 주소를 돌려준다(실패하면 시험 중단).
template <class S>
S& Make(ObjectRegistry& objects, InstancedStruct<>& value) noexcept {
    if (!value.Emplace<S>(objects.Types(), objects.ObjectAllocator())) {
        platform::Fatal("test instanced emplace failed");
    }

    return *value.Get<S>();
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

// I64 값 하나의 payload다.
inline Array<uint8> I64Payload(Allocator& allocator, int64 value) noexcept {
    ArchiveWriter writer(allocator);
    if (!writer.WriteI64(value)) {
        platform::Fatal("test payload failed");
    }

    return writer.TakeBytes();
}

// InstancedHolder 노드 하나짜리 그래프다.
inline Array<uint8> HolderGraph(Allocator& allocator, Span<const FieldBytes> fields) noexcept {
    auto record =
        RecordOf(allocator, TypeIdOf<InstancedHolder>(), TypeOf<InstancedHolder>().version, fields);
    auto graph = test::graph::Single(allocator, TypeIdOf<InstancedHolder>(), record.View());
    if (!graph) {
        platform::Fatal("test graph build failed");
    }

    return Move(graph.Value());
}

// 진단을 기록한다.
struct Recorder final : FieldCodecHooks {
    static constexpr uint32 Capacity = 16;

    struct Entry {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        TypeId valueType{};
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
        entry.valueType = diagnostic.valueType;
        entry.steps = static_cast<uint32>(diagnostic.path.Size());
        for (usize index = 0; index < diagnostic.path.Size(); ++index) {
            entry.path[index] = diagnostic.path[index];
        }

        return Result<void>::Success();
    }

    Entry entries[Capacity];
    uint32 count = 0;
};

} // namespace rocks::test::instanced
