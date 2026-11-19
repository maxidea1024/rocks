#include "TypeRegistryInternal.h"

#include <Object/TypeRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

// TypeLease·StructView(TypeLease.h)의 레코드 접근이다. 스키마·함수표·enum·struct 필드 대상, 필드
// ID 정렬 색인 조회, 상속 사슬 판정(IsA·IsNativeA)을 둔다.
namespace rocks {

TypeLease::TypeLease(TypeLease&& other) noexcept : owner_(other.owner_), record_(other.record_) {
    other.owner_ = nullptr;
    other.record_ = nullptr;
}

TypeLease& TypeLease::operator=(TypeLease&& other) noexcept {
    if (this != &other) {
        Reset();
        owner_ = other.owner_;
        record_ = other.record_;
        other.owner_ = nullptr;
        other.record_ = nullptr;
    }

    return *this;
}

TypeLease::~TypeLease() {
    Reset();
}

void TypeLease::Reset() noexcept {
    if (owner_) {
        owner_->Release(record_);
        owner_ = nullptr;
        record_ = nullptr;
    }
}

Result<TypeLease> TypeLease::Clone() const noexcept {
    if (!owner_) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    return owner_->Clone(record_);
}

const TypeSchema& TypeLease::Schema() const& noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->schema;
}

TypeId TypeLease::Id() const noexcept {
    return Schema().id;
}

TypeKind TypeLease::Kind() const noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->kind;
}

const EnumSchema& TypeLease::Enum() const& noexcept {
    if (Kind() != TypeKind::Enum) {
        platform::Fatal("TypeLease::Enum called on a non-enum lease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->enumSchema;
}

const EnumSchema* TypeLease::FieldEnum(usize fieldIndex) const& noexcept {
    const auto* record = static_cast<TypeRegistry::Record*>(record_);
    if (!record || fieldIndex >= record->schema.fields.Size()) {
        platform::Fatal("TypeLease::FieldEnum called on an empty lease or out of range");
    }

    if (!record->fieldTypes || !record->fieldTypes[fieldIndex] ||
        record->fieldTypes[fieldIndex]->kind != TypeKind::Enum) {
        return nullptr;
    }

    return &record->fieldTypes[fieldIndex]->enumSchema;
}

const EnumSchema* TypeLease::FieldKeyEnum(usize fieldIndex) const& noexcept {
    const auto* record = static_cast<TypeRegistry::Record*>(record_);
    if (!record || fieldIndex >= record->schema.fields.Size()) {
        platform::Fatal("TypeLease::FieldKeyEnum called on an empty lease or out of range");
    }

    if (!record->fieldKeyTypes || !record->fieldKeyTypes[fieldIndex] ||
        record->fieldKeyTypes[fieldIndex]->kind != TypeKind::Enum) {
        return nullptr;
    }

    return &record->fieldKeyTypes[fieldIndex]->enumSchema;
}

StructView TypeLease::FieldStruct(usize fieldIndex) const& noexcept {
    const auto* record = static_cast<TypeRegistry::Record*>(record_);
    if (!record || fieldIndex >= record->schema.fields.Size()) {
        platform::Fatal("TypeLease::FieldStruct called on an empty lease or out of range");
    }

    if (!record->fieldTypes || !record->fieldTypes[fieldIndex] ||
        record->fieldTypes[fieldIndex]->kind != TypeKind::Struct) {
        return StructView();
    }

    return StructView(record->fieldTypes[fieldIndex]);
}

StructView TypeLease::Struct() const& noexcept {
    if (Kind() != TypeKind::Struct) {
        platform::Fatal("TypeLease::Struct called on a non-struct lease");
    }

    return StructView(record_);
}

const RuntimeStructOps& TypeLease::StructOps() const& noexcept {
    if (Kind() != TypeKind::Struct) {
        platform::Fatal("TypeLease::StructOps called on a non-struct lease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->structOps;
}

namespace {

// 레코드 타입 이름(private)은 호출하는 StructView 멤버(TypeRegistry의 friend)에서 접근 검사된다.
template <class Record>
const Record& ViewAs(const void* record) noexcept {
    if (!record) {
        platform::Fatal("Access to an empty StructView");
    }

    return *static_cast<const Record*>(record);
}

} // namespace

const TypeSchema& StructView::Schema() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).schema;
}

const RuntimeStructOps& StructView::StructOps() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).structOps;
}

const RuntimeStructFieldOps& StructView::FieldOps(usize schemaIndex) const noexcept {
    const auto& record = ViewAs<TypeRegistry::Record>(record_);
    if (schemaIndex >= record.structOps.fields.Size()) {
        platform::Fatal("StructView::FieldOps out of range");
    }

    return record.structOps.fields[schemaIndex];
}

namespace {

// 필드 ID 정렬 색인(fieldOrder)의 이진 탐색이다. 없으면(색인이 없는 enum 레코드 포함) IndexNone
// (TypeLease::NoField·StructView::NoField와 같다).
template <class Record>
usize SortedFieldIndex(const Record& record, Guid fieldId) noexcept {
    if (!record.fieldOrder) {
        return IndexNone;
    }

    usize low = 0;
    usize high = record.schema.fields.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const Guid id = record.schema.fields[record.fieldOrder[middle]].id;
        if (id == fieldId) {
            return record.fieldOrder[middle];
        }

        if (id.high < fieldId.high || (id.high == fieldId.high && id.low < fieldId.low)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return IndexNone;
}

} // namespace

usize StructView::FieldIndex(Guid fieldId) const noexcept {
    return SortedFieldIndex(ViewAs<TypeRegistry::Record>(record_), fieldId);
}

const RuntimeFieldOps* TypeLease::FieldOps(usize schemaIndex) const& noexcept {
    const auto* record = static_cast<TypeRegistry::Record*>(record_);
    if (!record || record->kind != TypeKind::Class || schemaIndex >= record->schema.fields.Size()) {
        platform::Fatal("TypeLease::FieldOps called on a non-class lease or out of range");
    }

    const uint32 position = record->opsIndex[schemaIndex];
    return position == TypeRegistry::Record::NoOps ? nullptr : &record->ops.fields[position];
}

usize TypeLease::FieldIndex(Guid fieldId) const noexcept {
    if (!record_) {
        platform::Fatal("TypeLease::FieldIndex called on an empty lease");
    }

    return SortedFieldIndex(*static_cast<const TypeRegistry::Record*>(record_), fieldId);
}

const EnumSchema* StructView::FieldEnum(usize schemaIndex) const noexcept {
    const auto& record = ViewAs<TypeRegistry::Record>(record_);
    if (schemaIndex >= record.schema.fields.Size()) {
        platform::Fatal("StructView::FieldEnum out of range");
    }

    const auto* target = record.fieldTypes[schemaIndex];
    return target && target->kind == TypeKind::Enum ? &target->enumSchema : nullptr;
}

const EnumSchema* StructView::FieldKeyEnum(usize schemaIndex) const noexcept {
    const auto& record = ViewAs<TypeRegistry::Record>(record_);
    if (schemaIndex >= record.schema.fields.Size()) {
        platform::Fatal("StructView::FieldKeyEnum out of range");
    }

    const auto* target = record.fieldKeyTypes ? record.fieldKeyTypes[schemaIndex] : nullptr;
    return target && target->kind == TypeKind::Enum ? &target->enumSchema : nullptr;
}

StructView StructView::FieldStruct(usize schemaIndex) const noexcept {
    const auto& record = ViewAs<TypeRegistry::Record>(record_);
    if (schemaIndex >= record.schema.fields.Size()) {
        platform::Fatal("StructView::FieldStruct out of range");
    }

    const auto* target = record.fieldTypes[schemaIndex];
    return target && target->kind == TypeKind::Struct ? StructView(target) : StructView();
}

uint32 StructView::Depth() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).depth;
}

bool StructView::ContainsStrongReference() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).containsStrongReference;
}

bool StructView::ContainsRequired() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).containsRequired;
}

bool StructView::RequiresWorldOnlyOwner() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).requiresWorldOnlyOwner;
}

bool StructView::ContainsReferenceArray() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).containsReferenceArray;
}

bool StructView::ContainsName() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).containsName;
}

bool StructView::ContainsWeakReference() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).containsWeakReference;
}

bool StructView::ContainsInstancedStruct() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).containsInstancedStruct;
}

bool StructView::IsAtomic() const noexcept {
    return HasFlag(ViewAs<TypeRegistry::Record>(record_).schema.flags, TypeFlags::Atomic);
}

StringView TypeLease::Name() const& noexcept {
    const auto& name = Schema().name;
    return StringView(name.Data(), name.Size());
}

const RuntimeTypeOps& TypeLease::Ops() const& noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->ops;
}

usize TypeLease::ObjectOffset() const noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }

    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(
        &static_cast<TypeRegistry::Record*>(record_)->objectOffset);
}

bool TypeLease::RecordObjectOffset(usize offset) const noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }

    usize* slot = &static_cast<TypeRegistry::Record*>(record_)->objectOffset;
    usize expected = NoObjectOffset;
    if (platform::AtomicCompareExchangeStrong<platform::MemoryOrder::Relaxed,
                                              platform::MemoryOrder::Relaxed>(slot, expected,
                                                                              offset)) {
        return true;
    }

    return expected == offset;
}

bool TypeLease::IsA(TypeId expected) const noexcept {
    if (!record_) {
        platform::Fatal("TypeLease::IsA called on empty lease");
    }

    for (auto* record = static_cast<TypeRegistry::Record*>(record_); record;
         record = record->base) {
        if (record->schema.id == expected) {
            return true;
        }
    }

    return false;
}

bool TypeLease::IsNativeA(const void* nativeType) const noexcept {
    if (!record_) {
        platform::Fatal("TypeLease::IsNativeA called on empty lease");
    }

    // nullptr는 기반 Object이며 모든 등록 타입이 만족한다. 등록 시 base 레코드의 nativeType과
    // 파생 레코드의 nativeBaseType 일치를 검증했으므로 base 사슬이 곧 C++ 상속 사슬이다.
    if (nativeType == nullptr) {
        return true;
    }

    for (auto* record = static_cast<TypeRegistry::Record*>(record_); record;
         record = record->base) {
        if (record->ops.nativeType == nativeType) {
            return true;
        }
    }

    return false;
}

} // namespace rocks
