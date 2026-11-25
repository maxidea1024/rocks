#include <Base/NumericLimits.h>
#include <Core/Move.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <Serialization/Archive.h>
#include <Serialization/ValueArray.h>

// 노드 속성 Record의 필드 읽기다(Docs/Design/MaterialSystem 19.13.3절).
namespace rocks::material {

namespace {

// schema에서 이름이 field인 필드의 id를 찾는다(도구 이름이 아니라 C++ 필드 이름 — 정적 이름이다).
Result<Guid> FieldId(const TypeSchema& schema, StringView field) noexcept {
    for (const FieldSchema& entry : schema.fields) {
        if (StringView(entry.name.Data(), entry.name.Size()) == field) {
            return Result<Guid>::Success(entry.id);
        }
    }

    return Result<Guid>::Failure(ErrorCode::NotFound);
}

// 배열 필드의 원소 수 상한이다(schema의 maxElements — 0이면 배열 필드가 아니다).
uint32 FieldMaxElements(const TypeSchema& schema, StringView field) noexcept {
    for (const FieldSchema& entry : schema.fields) {
        if (StringView(entry.name.Data(), entry.name.Size()) == field) {
            return entry.maxElements;
        }
    }

    return 0;
}

} // namespace

Result<NodePropertyReader> NodePropertyReader::Open(Allocator& allocator, Span<const uint8> bytes,
                                                    const TypeSchema& schema) noexcept {
    NodePropertyReader reader(allocator, schema);
    if (bytes.IsEmpty()) {
        return Result<NodePropertyReader>::Success(Move(reader));
    }

    auto record = RecordReader::Open(allocator, bytes, schema.id, 1, schema.version);
    if (!record) {
        return Result<NodePropertyReader>::Failure(record.Error());
    }

    reader.record_.Emplace(Move(record.Value()));
    return Result<NodePropertyReader>::Success(Move(reader));
}

Result<Optional<FieldView>> NodePropertyReader::Find(StringView field) noexcept {
    auto id = FieldId(*schema_, field);
    if (!id) {
        return Result<Optional<FieldView>>::Failure(id.Error());
    }

    if (!record_) {
        return Result<Optional<FieldView>>::Success(Optional<FieldView>());
    }

    auto found = record_->Find(id.Value());
    if (!found) {
        if (found.Error() == ErrorCode::NotFound) {
            return Result<Optional<FieldView>>::Success(Optional<FieldView>());
        }

        return Result<Optional<FieldView>>::Failure(found.Error());
    }

    return Result<Optional<FieldView>>::Success(Optional<FieldView>(found.Value()));
}

Result<int64> NodePropertyReader::ReadInteger(StringView field, int64 fallback) noexcept {
    auto found = Find(field);
    if (!found) {
        return Result<int64>::Failure(found.Error());
    }

    if (!found.Value()) {
        return Result<int64>::Success(fallback);
    }

    const FieldView& view = *found.Value();
    auto value = record_->OpenValue(view);
    if (!value) {
        return Result<int64>::Failure(value.Error());
    }

    switch (static_cast<WireType>(view.wireType)) {
    case WireType::U32: {
        auto read = value.Value().ReadU32();
        if (!read) {
            return Result<int64>::Failure(read.Error());
        }

        return Result<int64>::Success(static_cast<int64>(read.Value()));
    }
    case WireType::U64: {
        auto read = value.Value().ReadU64();
        if (!read) {
            return Result<int64>::Failure(read.Error());
        }

        if (read.Value() > static_cast<uint64>(NumericLimits<int64>::Max())) {
            return Result<int64>::Failure(ErrorCode::Overflow);
        }

        return Result<int64>::Success(static_cast<int64>(read.Value()));
    }
    case WireType::I64:
        return value.Value().ReadI64();
    default:
        return Result<int64>::Failure(ErrorCode::TypeMismatch);
    }
}

Result<bool> NodePropertyReader::ReadBool(StringView field, bool fallback) noexcept {
    auto found = Find(field);
    if (!found) {
        return Result<bool>::Failure(found.Error());
    }

    if (!found.Value()) {
        return Result<bool>::Success(fallback);
    }

    if (found.Value()->wireType != static_cast<uint32>(WireType::Bool)) {
        return Result<bool>::Failure(ErrorCode::TypeMismatch);
    }

    auto value = record_->OpenValue(*found.Value());
    if (!value) {
        return Result<bool>::Failure(value.Error());
    }

    return value.Value().ReadBool();
}

Result<float> NodePropertyReader::ReadFloat(StringView field, float fallback) noexcept {
    auto found = Find(field);
    if (!found) {
        return Result<float>::Failure(found.Error());
    }

    if (!found.Value()) {
        return Result<float>::Success(fallback);
    }

    if (found.Value()->wireType != static_cast<uint32>(WireType::F32)) {
        return Result<float>::Failure(ErrorCode::TypeMismatch);
    }

    auto value = record_->OpenValue(*found.Value());
    if (!value) {
        return Result<float>::Failure(value.Error());
    }

    return value.Value().ReadF32();
}

Result<Guid> NodePropertyReader::ReadGuid(StringView field, Guid fallback) noexcept {
    auto found = Find(field);
    if (!found) {
        return Result<Guid>::Failure(found.Error());
    }

    if (!found.Value()) {
        return Result<Guid>::Success(fallback);
    }

    if (found.Value()->wireType != static_cast<uint32>(WireType::Guid)) {
        return Result<Guid>::Failure(ErrorCode::TypeMismatch);
    }

    auto value = record_->OpenValue(*found.Value());
    if (!value) {
        return Result<Guid>::Failure(value.Error());
    }

    return value.Value().ReadGuid();
}

Result<StringView> NodePropertyReader::ReadText(StringView field) noexcept {
    auto found = Find(field);
    if (!found) {
        return Result<StringView>::Failure(found.Error());
    }

    if (!found.Value()) {
        return Result<StringView>::Success(StringView());
    }

    if (found.Value()->wireType != static_cast<uint32>(WireType::String)) {
        return Result<StringView>::Failure(ErrorCode::TypeMismatch);
    }

    auto value = record_->OpenValue(*found.Value());
    if (!value) {
        return Result<StringView>::Failure(value.Error());
    }

    return value.Value().ReadStringView();
}

Result<NodePropertyList> NodePropertyReader::OpenStructArray(StringView field,
                                                             const TypeSchema& element) noexcept {
    auto found = Find(field);
    if (!found) {
        return Result<NodePropertyList>::Failure(found.Error());
    }

    NodePropertyList list(*allocator_, element);
    if (!found.Value()) {
        return Result<NodePropertyList>::Success(Move(list));
    }

    auto opened =
        RecordListReader::OpenField(*record_, *found.Value(), element.id, 1, element.version);
    if (!opened) {
        return Result<NodePropertyList>::Failure(opened.Error());
    }

    list.list_.Emplace(Move(opened.Value()));
    return Result<NodePropertyList>::Success(Move(list));
}

Result<NodePropertyReader> NodePropertyReader::OpenStruct(StringView field,
                                                          const TypeSchema& element) noexcept {
    auto found = Find(field);
    if (!found) {
        return Result<NodePropertyReader>::Failure(found.Error());
    }

    NodePropertyReader reader(*allocator_, element);
    if (!found.Value()) {
        return Result<NodePropertyReader>::Success(Move(reader));
    }

    auto nested = record_->OpenRecordField(*allocator_, *found.Value(), element.id, 1,
                                           element.version, RecordLimits{}.maxFields);
    if (!nested) {
        return Result<NodePropertyReader>::Failure(nested.Error());
    }

    reader.record_.Emplace(Move(nested.Value()));
    return Result<NodePropertyReader>::Success(Move(reader));
}

Result<void> NodePropertyReader::ReadGuidArray(StringView field, Array<Guid>& out) noexcept {
    auto found = Find(field);
    if (!found) {
        return Result<void>::Failure(found.Error());
    }

    if (!found.Value()) {
        return Result<void>::Success();
    }

    auto array = ValueArrayReader::OpenField(*record_, *found.Value(), WireType::Guid,
                                             FieldMaxElements(*schema_, field));
    if (!array) {
        return Result<void>::Failure(array.Error());
    }

    for (uint32 index = 0; index < array.Value().Count(); ++index) {
        auto value = array.Value().Elements().ReadGuid();
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        if (auto pushed = out.TryPushBack(value.Value()); !pushed) {
            return pushed;
        }
    }

    return array.Value().Finish();
}

Result<void> NodePropertyReader::ReadTextArray(StringView field, Array<StringView>& out) noexcept {
    auto found = Find(field);
    if (!found) {
        return Result<void>::Failure(found.Error());
    }

    if (!found.Value()) {
        return Result<void>::Success();
    }

    auto array = ValueArrayReader::OpenField(*record_, *found.Value(), WireType::String,
                                             FieldMaxElements(*schema_, field));
    if (!array) {
        return Result<void>::Failure(array.Error());
    }

    for (uint32 index = 0; index < array.Value().Count(); ++index) {
        auto value = array.Value().Elements().ReadStringView();
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        if (auto pushed = out.TryPushBack(value.Value()); !pushed) {
            return pushed;
        }
    }

    return array.Value().Finish();
}

usize NodePropertyList::Count() const noexcept {
    return list_ ? list_->ItemCount() : 0;
}

Result<NodePropertyReader> NodePropertyList::Element(usize index) noexcept {
    if (index >= Count()) {
        return Result<NodePropertyReader>::Failure(ErrorCode::InvalidArgument);
    }

    auto item = list_->OpenItem(index);
    if (!item) {
        return Result<NodePropertyReader>::Failure(item.Error());
    }

    NodePropertyReader reader(*allocator_, *element_);
    reader.record_.Emplace(Move(item.Value()));
    return Result<NodePropertyReader>::Success(Move(reader));
}

} // namespace rocks::material
