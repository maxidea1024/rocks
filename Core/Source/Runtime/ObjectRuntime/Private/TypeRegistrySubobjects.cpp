#include "TypeRegistrySubobjects.h"

#include <Base/PlacementNew.h>

// 선언 subobject 표의 등록 검사·복사다(Docs/Design/Prefab 10.2절). TypeRegistry::Register가 잠금
// 밖에서 ValidateSubobjectList로 모양을, 잠금 안에서 InheritsSubobjects로 base 사슬을 보고,
// 레코드를 채울 때 CopySubobjects를 부른다.
namespace rocks::detail {

namespace {

constexpr usize AlignSubobjectOffset(usize value, usize alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// 선언 subobject 소유 필드에 둘 수 없는 flag다(Transient·EditorOnly·Required·Notify — 정체성은
// graph 노드 종류가 정하고 생성은 instancing이 한다).
constexpr uint32 ForbiddenSubobjectFieldFlags =
    static_cast<uint32>(FieldFlags::Transient) | static_cast<uint32>(FieldFlags::EditorOnly) |
    static_cast<uint32>(FieldFlags::Required) | static_cast<uint32>(FieldFlags::Notify);

const FieldSchema* FindField(Span<const FieldSchema> fields, Guid id) noexcept {
    for (const FieldSchema& field : fields) {
        if (field.id == id) {
            return &field;
        }
    }

    return nullptr;
}

bool IsSingleObjectReference(const FieldSchema& field) noexcept {
    return field.kind == FieldKind::ObjectReference &&
           field.value.kind == ValueKind::ObjectReference;
}

bool IsObjectReferenceArray(const FieldSchema& field) noexcept {
    return field.kind == FieldKind::ObjectReference && field.value.kind == ValueKind::Array &&
           field.element.kind == ValueKind::ObjectReference;
}

bool IsWeakObjectReference(const FieldSchema& field) noexcept {
    return field.value.kind == ValueKind::WeakObjectReference;
}

bool SameSubobject(const SubobjectSchema& left, const SubobjectSchema& right) noexcept {
    return left.id == right.id && left.type == right.type &&
           left.attachParent == right.attachParent && left.flags == right.flags;
}

} // namespace

usize SubobjectBlockLayout::Place(usize offset) noexcept {
    schemaOffset = AlignSubobjectOffset(offset, alignof(SubobjectSchema));
    opsOffset = AlignSubobjectOffset(schemaOffset + count * sizeof(SubobjectSchema),
                                     alignof(RuntimeSubobjectOps));
    return opsOffset + count * sizeof(RuntimeSubobjectOps);
}

Result<SubobjectBlockLayout> ValidateSubobjectList(const TypeSchema& schema,
                                                   const RuntimeTypeOps& ops) noexcept {
    SubobjectBlockLayout layout;
    layout.count = schema.subobjects.Size();
    if (layout.count > MaxClassSubobjects) {
        return Result<SubobjectBlockLayout>::Failure(ErrorCode::CapacityExceeded);
    }

    if (ops.subobjects.Size() != layout.count) {
        return Result<SubobjectBlockLayout>::Failure(ErrorCode::InvalidArgument);
    }

    // 소유 구조 클래스 지정자는 이 클래스(상속 포함) 필드를 가리키고 종류가 맞아야 한다.
    const FieldSchema* list = nullptr;
    const FieldSchema* root = nullptr;
    if (schema.subobjectListField != Guid{}) {
        list = FindField(schema.fields, schema.subobjectListField);
        if (list == nullptr || !IsObjectReferenceArray(*list)) {
            return Result<SubobjectBlockLayout>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (schema.subobjectRootField != Guid{}) {
        root = FindField(schema.fields, schema.subobjectRootField);
        if (root == nullptr || !IsSingleObjectReference(*root)) {
            return Result<SubobjectBlockLayout>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (schema.subobjectAttachField != Guid{}) {
        const FieldSchema* attach = FindField(schema.fields, schema.subobjectAttachField);
        if (attach == nullptr || !IsWeakObjectReference(*attach)) {
            return Result<SubobjectBlockLayout>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (layout.count != 0 && list == nullptr) {
        return Result<SubobjectBlockLayout>::Failure(ErrorCode::InvalidArgument);
    }

    usize roots = 0;
    for (usize index = 0; index < layout.count; ++index) {
        const SubobjectSchema& entry = schema.subobjects[index];
        const RuntimeSubobjectOps& entryOps = ops.subobjects[index];
        const bool rootEntry = (entry.flags & static_cast<uint32>(SubobjectFlags::Root)) != 0;
        if (entry.id == Guid{} || entry.type == TypeId{} || entryOps.id != entry.id ||
            entryOps.field == nullptr || (entry.flags & ~KnownSubobjectFlags) != 0 ||
            entry.attachParent < -1 || entry.attachParent >= static_cast<int32>(index) ||
            (rootEntry && (root == nullptr || entry.attachParent != -1))) {
            return Result<SubobjectBlockLayout>::Failure(ErrorCode::InvalidArgument);
        }

        roots += rootEntry ? 1 : 0;
        const FieldSchema* field = FindField(schema.fields, entry.id);
        if (field == nullptr || !IsSingleObjectReference(*field) || field == list ||
            field == root || !HasFlag(field->flags, FieldFlags::ReadOnly) ||
            (field->flags & ForbiddenSubobjectFieldFlags) != 0) {
            return Result<SubobjectBlockLayout>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize previous = 0; previous < index; ++previous) {
            if (schema.subobjects[previous].id == entry.id) {
                return Result<SubobjectBlockLayout>::Failure(ErrorCode::AlreadyExists);
            }
        }
    }

    if (roots > 1) {
        return Result<SubobjectBlockLayout>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<SubobjectBlockLayout>::Success(layout);
}

bool InheritsSubobjects(const TypeSchema& base, const TypeSchema& derived) noexcept {
    const auto inherits = [](Guid baseValue, Guid derivedValue) noexcept {
        return baseValue == Guid{} || baseValue == derivedValue;
    };
    if (!inherits(base.subobjectListField, derived.subobjectListField) ||
        !inherits(base.subobjectRootField, derived.subobjectRootField) ||
        !inherits(base.subobjectAttachField, derived.subobjectAttachField) ||
        base.subobjects.Size() > derived.subobjects.Size()) {
        return false;
    }

    for (usize index = 0; index < base.subobjects.Size(); ++index) {
        if (!SameSubobject(base.subobjects[index], derived.subobjects[index])) {
            return false;
        }
    }

    return true;
}

void CopySubobjects(const SubobjectBlockLayout& layout, uint8* block, const TypeSchema& schema,
                    const RuntimeTypeOps& ops, Span<const SubobjectSchema>& subobjects,
                    Span<const RuntimeSubobjectOps>& subobjectOps) noexcept {
    auto* copiedSchema = reinterpret_cast<SubobjectSchema*>(block + layout.schemaOffset);
    auto* copiedOps = reinterpret_cast<RuntimeSubobjectOps*>(block + layout.opsOffset);
    for (usize index = 0; index < layout.count; ++index) {
        ::new (copiedSchema + index) SubobjectSchema(schema.subobjects[index]);
        ::new (copiedOps + index) RuntimeSubobjectOps(ops.subobjects[index]);
    }

    subobjects = {copiedSchema, layout.count};
    subobjectOps = {copiedOps, layout.count};
}

} // namespace rocks::detail
