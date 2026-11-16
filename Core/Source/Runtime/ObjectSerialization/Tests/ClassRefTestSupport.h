#pragma once

#include <ReflectionTestFixtures/ClassRefFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9d2 ClassRef PROPERTY 시험(ClassRefGraphTests.cpp·ClassRefFieldTests.cpp)의 공용
// 도우미다: 생성 fixture(ClassRefFixture.h) 등록(지운 클래스를 흉내 내려고 ClassLeaf를 빼고 등록할
// 수 있다), 객체 생성·값 편집, ClassRef FieldValue, 진단 기록 hook, 단일 필드 Record 조립.
namespace rocks::test::classref {

using reflection_fixture::ClassBase;
using reflection_fixture::ClassDerived;
using reflection_fixture::ClassHolder;
using reflection_fixture::ClassLeaf;
using reflection_fixture::ClassRenamed;
using reflection_fixture::ClassSlot;
using reflection_fixture::ClassStranger;
using reflection_fixture::ClassValue;

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

// ClassHolder 필드 ID다.
inline Guid HolderField(StringView name) noexcept {
    return SchemaField(TypeOf<ClassHolder>(), name).id;
}

// fixture를 쓰는 순서대로 등록한다. withLeaf가 거짓이면 ClassLeaf를 등록하지 않는다(그 클래스를
// 지운 실행 환경 — 저장된 ClassLeaf 값은 미등록 클래스다). 기록은 Shutdown이 정리한다.
inline bool RegisterFixture(TypeRegistry& types, bool withLeaf = true) noexcept {
    return GeneratedType<ClassBase>::Register(types).IsOk() &&
           GeneratedType<ClassDerived>::Register(types).IsOk() &&
           (!withLeaf || GeneratedType<ClassLeaf>::Register(types).IsOk()) &&
           GeneratedType<ClassStranger>::Register(types).IsOk() &&
           GeneratedType<ClassRenamed>::Register(types).IsOk() &&
           GeneratedType<ClassValue>::Register(types).IsOk() &&
           GeneratedType<ClassSlot>::Register(types).IsOk() &&
           GeneratedType<ClassHolder>::Register(types).IsOk();
}

// root로 쥔 새 객체다(실패하면 시험 중단).
template <class T>
RootRef<T> New(ObjectRegistry& objects) noexcept {
    auto created = objects.NewObject<T>(objects.OwnerAccess(), TypeIdOf<T>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

// 객체 값을 edit로 바꾸거나 본다(Borrow 동안).
template <class T = ClassHolder, class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<T>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

inline FieldValue ClassValueOf(Allocator& allocator, TypeId type) noexcept {
    FieldValue value(allocator);
    value.kind = ValueKind::ClassReference;
    value.guid = type;
    return value;
}

// 진단을 기록한다.
struct Recorder final : FieldCodecHooks {
    static constexpr uint32 Capacity = 32;

    struct Entry {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        uint32 objectIndex = 0;
        Guid field{};
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
        entry.objectIndex = diagnostic.objectIndex;
        entry.field = diagnostic.field;
        entry.valueType = diagnostic.valueType;
        entry.steps = static_cast<uint32>(diagnostic.path.Size());
        for (usize index = 0; index < diagnostic.path.Size(); ++index) {
            entry.path[index] = diagnostic.path[index];
        }

        return Result<void>::Success();
    }

    // kind 진단의 수다.
    uint32 Count(FieldDiagnosticKind kind) const noexcept {
        uint32 found = 0;
        for (uint32 index = 0; index < count; ++index) {
            found += entries[index].kind == kind ? 1 : 0;
        }

        return found;
    }

    // 경로가 [first(indexed면 [i])] 또는 [first, second]이고 valueType이 type인
    // UnknownClassReference 진단이 있는지다.
    bool HasUnknownClass(TypeId type, Guid first, bool indexed, uint32 index,
                         Guid second = {}) const noexcept {
        const uint32 steps = second == Guid{} ? 1 : 2;
        for (uint32 at = 0; at < count; ++at) {
            const Entry& entry = entries[at];
            if (entry.kind != FieldDiagnosticKind::UnknownClassReference ||
                entry.valueType != type || entry.steps != steps || entry.path[0].fieldId != first ||
                (entry.path[0].selector == PathSelector::Index) != indexed ||
                (indexed && entry.path[0].index != index)) {
                continue;
            }

            if (steps == 1 ? entry.field == first
                           : entry.path[1].fieldId == second && entry.field == second) {
                return true;
            }
        }

        return false;
    }

    Entry entries[Capacity] = {};
    uint32 count = 0;
};

// Guid 값 하나의 payload다.
inline Array<uint8> GuidPayload(Allocator& allocator, Guid value) noexcept {
    ArchiveWriter payload(allocator);
    if (!payload.WriteGuid(value)) {
        platform::Fatal("test payload write failed");
    }

    return payload.TakeBytes();
}

// type의 Record에 (field, wire, payload) 하나를 담는다.
inline Array<uint8> OneFieldRecord(Allocator& allocator, TypeId type, uint32 version, Guid field,
                                   WireType wire, Span<const uint8> payload) noexcept {
    RecordBuilder record(allocator, type, version);
    if (!record.AddField(field, static_cast<uint32>(wire), payload)) {
        platform::Fatal("test record field add failed");
    }

    auto built = record.Build();
    if (!built) {
        platform::Fatal("test record build failed");
    }

    return Move(built.Value());
}

} // namespace rocks::test::classref
