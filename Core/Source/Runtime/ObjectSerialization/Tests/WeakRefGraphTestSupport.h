#pragma once

#include <ReflectionTestFixtures/WeakRefFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <Serialization/ValueArray.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9d1 WeakRef PROPERTY 시험(WeakRefGraphTests.cpp·WeakRefGraphLoadTests.cpp·
// WeakRefFieldTests.cpp)의 공용 도우미다: 생성 fixture(WeakRefFixture.h) 등록, 객체 생성·값 편집,
// 진단·외부 참조 허락을 기록하는 hook, U32·U32 배열 payload와 단일 필드 Record 조립.
namespace rocks::test::weak {

using reflection_fixture::WeakHolder;
using reflection_fixture::WeakLink;
using reflection_fixture::WeakLinkGroup;
using reflection_fixture::WeakOther;
using reflection_fixture::WeakTarget;

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

// WeakHolder 필드 ID다.
inline Guid HolderField(StringView name) noexcept {
    return SchemaField(TypeOf<WeakHolder>(), name).id;
}

// fixture를 쓰는 순서대로 등록한다(기록은 Shutdown이 정리한다).
inline bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<WeakTarget>::Register(types).IsOk() &&
           GeneratedType<WeakOther>::Register(types).IsOk() &&
           GeneratedType<WeakLink>::Register(types).IsOk() &&
           GeneratedType<WeakLinkGroup>::Register(types).IsOk() &&
           GeneratedType<WeakHolder>::Register(types).IsOk();
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

// 객체 값을 edit로 바꾸거나 본다(Borrow 동안). T는 객체의 정적 타입이다.
template <class T = WeakHolder, class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<T>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

// holder의 강한 참조 필드(strong·second)를 Registry로 쓴다.
inline void SetStrong(ObjectRegistry& objects, ObjectHandle owner,
                      ObjectRef<WeakTarget> WeakHolder::* member, ObjectHandle target) noexcept {
    auto borrowed = objects.Resolve<WeakHolder>(objects.OwnerAccess(), owner);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    REQUIRE(objects.SetReference(objects.OwnerAccess(), owner, value.Value()->*member, target));
}

inline FieldValue WeakValue(Allocator& allocator, ObjectHandle target) noexcept {
    FieldValue value(allocator);
    value.kind = ValueKind::WeakObjectReference;
    value.reference = target;
    return value;
}

// 진단과 외부 참조 허락 질의를 기록하고, 질의에는 참조 종류마다 정한 답을 준다. 관찰할 struct를
// 하나 줄 수 있다.
struct Recorder final : FieldCodecHooks {
    static constexpr uint32 Capacity = 64;
    static constexpr uint32 MaxSteps = MaxPropertyPathSteps;

    struct Entry {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        TypeId objectType{};
        uint32 objectIndex = 0;
        Guid field{};
        int64 value = 0;
        uint32 steps = 0;
        PathStep path[MaxSteps] = {};
    };

    struct Asked {
        ObjectHandle target{};
        TypeId expectedType{};
        uint32 slot = 0;
        bool weak = false;
        bool required = false;
    };

    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count >= Capacity || diagnostic.path.Size() > MaxSteps) {
            CHECK(false);
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        Entry& entry = entries[count++];
        entry.kind = diagnostic.kind;
        entry.objectType = diagnostic.objectType;
        entry.objectIndex = diagnostic.objectIndex;
        entry.field = diagnostic.field;
        entry.value = diagnostic.value;
        entry.steps = static_cast<uint32>(diagnostic.path.Size());
        for (usize index = 0; index < diagnostic.path.Size(); ++index) {
            entry.path[index] = diagnostic.path[index];
        }

        return Result<void>::Success();
    }

    Result<ExternalAdmission>
    AdmitExternalReference(const ExternalReferenceInfo& info) noexcept override {
        if (askedCount >= Capacity) {
            CHECK(false);
            return Result<ExternalAdmission>::Failure(ErrorCode::CapacityExceeded);
        }

        asked[askedCount++] = {info.target, info.expectedType, info.slot, info.weak, info.required};
        return Result<ExternalAdmission>::Success(info.weak ? weakAnswer : strongAnswer);
    }

    Span<const TypeId> ObservedStructs() noexcept override {
        return observed == TypeId{} ? Span<const TypeId>() : Span<const TypeId>(&observed, 1);
    }

    void Clear() noexcept {
        count = 0;
        askedCount = 0;
    }

    // kind·value가 같은 진단의 수다.
    uint32 Count(FieldDiagnosticKind kind, int64 value) const noexcept {
        uint32 found = 0;
        for (uint32 index = 0; index < count; ++index) {
            found += entries[index].kind == kind && entries[index].value == value ? 1 : 0;
        }

        return found;
    }

    // objectIndex 노드의 경로가 [first(index면 [i])] 또는 [first, second]인 weak 집합 밖 null
    // 진단(ExternalReference, value 2)이 있는지다.
    bool HasWeakNull(uint32 objectIndex, Guid first, bool indexed, uint32 index,
                     Guid second = {}) const noexcept {
        const uint32 steps = second == Guid{} ? 1 : 2;
        for (uint32 at = 0; at < count; ++at) {
            const Entry& entry = entries[at];
            if (entry.kind != FieldDiagnosticKind::ExternalReference || entry.value != 2 ||
                entry.objectIndex != objectIndex || entry.steps != steps ||
                entry.path[0].fieldId != first || entry.path[0].indexed != indexed ||
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

    // weak 질의 수다.
    uint32 WeakAsked() const noexcept {
        uint32 found = 0;
        for (uint32 index = 0; index < askedCount; ++index) {
            found += asked[index].weak ? 1 : 0;
        }

        return found;
    }

    Entry entries[Capacity] = {};
    uint32 count = 0;
    Asked asked[Capacity] = {};
    uint32 askedCount = 0;
    ExternalAdmission strongAnswer = ExternalAdmission::Import;
    ExternalAdmission weakAnswer = ExternalAdmission::Import;
    TypeId observed{};
};

// U32 값 하나의 payload다.
inline Array<uint8> U32Payload(Allocator& allocator, uint32 value) noexcept {
    ArchiveWriter payload(allocator);
    if (!payload.WriteU32(value)) {
        platform::Fatal("test payload write failed");
    }

    return payload.TakeBytes();
}

// U32 원소 ValueArray payload다.
inline Array<uint8> U32ArrayPayload(Allocator& allocator, Span<const uint32> values) noexcept {
    ArchiveWriter payload(allocator);
    auto writer = ValueArrayWriter::Begin(
        payload, WireType::U32, static_cast<uint32>(values.Size()), NumericLimits<usize>::Max());
    if (!writer) {
        platform::Fatal("test value array begin failed");
    }

    for (uint32 value : values) {
        if (!payload.WriteU32(value)) {
            platform::Fatal("test value array write failed");
        }
    }

    if (!writer.Value().End()) {
        platform::Fatal("test value array end failed");
    }

    return payload.TakeBytes();
}

// type의 Record에 (field, wire, payload) 하나를 담는다(빈 payload면 필드 없는 Record).
inline Array<uint8> OneFieldRecord(Allocator& allocator, TypeId type, uint32 version, Guid field,
                                   WireType wire, Span<const uint8> payload) noexcept {
    RecordBuilder record(allocator, type, version);
    if (field != Guid{} && !record.AddField(field, static_cast<uint32>(wire), payload)) {
        platform::Fatal("test record field add failed");
    }

    auto built = record.Build();
    if (!built) {
        platform::Fatal("test record build failed");
    }

    return Move(built.Value());
}

} // namespace rocks::test::weak
