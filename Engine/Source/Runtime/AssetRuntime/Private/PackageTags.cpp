#include <AssetRuntime/PackageFormat.h>
#include <AssetRuntime/PackageTags.h>
#include <Serialization/Record.h>
#include <Serialization/RecordList.h>

// 패키지 Tags 값 스키마의 쓰기·읽기다(Docs/Design/AssetSystem.ko.md 7.1절 13 — A2b).
namespace rocks {

namespace {

// (키, 값) 쌍의 순서다(UTF-8 byte 순).
int ComparePair(StringView leftKey, StringView leftValue, StringView rightKey,
                StringView rightValue) noexcept {
    const int key = StringView::CompareBytes(leftKey, rightKey);
    return key != 0 ? key : StringView::CompareBytes(leftValue, rightValue);
}

// Record 계층 오류를 Tags 읽기 오류로 바꾼다: 할당 실패는 그대로, 지원 범위 밖 버전은 Unsupported
// (더 새 Tags — 색인은 UnsupportedVersion), 그 밖은 형식 위반 InvalidArgument다.
ErrorCode TagsError(ErrorCode error) noexcept {
    if (error == ErrorCode::OutOfMemory || error == ErrorCode::Unsupported) {
        return error;
    }

    return ErrorCode::InvalidArgument;
}

bool ValidKey(StringView key) noexcept {
    return !key.IsEmpty() && key.ByteCount() <= MaxPackageTagKeyBytes && key.IsValidUtf8();
}

bool ValidValue(StringView value) noexcept {
    return value.ByteCount() <= PackageMaxStringBytes && value.IsValidUtf8();
}

Span<const uint8> BytesOf(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

StringView TextOf(const FieldView& field) noexcept {
    return StringView(reinterpret_cast<const char*>(field.payload.Data()), field.payload.Size());
}

} // namespace

Result<Array<uint8>> WritePackageTags(Allocator& allocator,
                                      Span<const PackageTagInput> tags) noexcept {
    using Out = Array<uint8>;
    if (tags.IsEmpty()) {
        return Result<Out>::Success(Array<uint8>(allocator));
    }

    // 순서를 정하는 index 표(삽입 정렬 — 태그 수는 작다).
    Array<usize> order(allocator);
    if (auto reserved = order.TryReserve(tags.Size()); !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    for (usize index = 0; index < tags.Size(); ++index) {
        if (!ValidKey(tags[index].key) || !ValidValue(tags[index].value)) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }

        usize at = order.Size();
        (void)order.TryPushBack(index);
        while (at > 0) {
            const PackageTagInput& before = tags[order[at - 1]];
            const int compared =
                ComparePair(before.key, before.value, tags[index].key, tags[index].value);
            if (compared == 0) {
                return Result<Out>::Failure(ErrorCode::InvalidArgument);
            }

            if (compared < 0) {
                break;
            }

            order[at] = order[at - 1];
            --at;
        }

        order[at] = index;
    }

    RecordListBuilder list(allocator);
    for (usize position : order) {
        RecordBuilder item(allocator, PackageTagItemSchema, PackageTagsVersion);
        if (auto added =
                item.AddField(PackageTagKeyField, WireType::String, BytesOf(tags[position].key));
            !added) {
            return Result<Out>::Failure(added.Error());
        }

        if (auto added = item.AddField(PackageTagValueField, WireType::String,
                                       BytesOf(tags[position].value));
            !added) {
            return Result<Out>::Failure(added.Error());
        }

        auto built = item.Build();
        if (!built) {
            return Result<Out>::Failure(built.Error());
        }

        if (auto appended = list.AddItem(built.Value().View()); !appended) {
            return Result<Out>::Failure(appended.Error());
        }
    }

    RecordBuilder record(allocator, PackageTagsSchema, PackageTagsVersion);
    if (auto finished = list.Finish(record, PackageTagsListField); !finished) {
        return Result<Out>::Failure(finished.Error());
    }

    return record.Build();
}

Result<Array<PackageTag>> ReadPackageTags(Allocator& allocator, Span<const uint8> bytes) noexcept {
    using Out = Array<PackageTag>;
    Array<PackageTag> tags(allocator);
    if (bytes.IsEmpty()) {
        return Result<Out>::Success(Move(tags));
    }

    auto header = ReadRecordHeader(bytes);
    if (!header) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    if (!(header.Value().schema == PackageTagsSchema)) {
        return Result<Out>::Failure(ErrorCode::Unsupported);
    }

    auto record = RecordReader::Open(allocator, bytes, PackageTagsSchema, PackageTagsVersion,
                                     PackageTagsVersion);
    if (!record) {
        return Result<Out>::Failure(TagsError(record.Error()));
    }

    auto listField = record.Value().Find(PackageTagsListField);
    if (!listField) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    auto list = RecordListReader::OpenField(record.Value(), listField.Value(), PackageTagItemSchema,
                                            PackageTagsVersion, PackageTagsVersion);
    if (!list) {
        return Result<Out>::Failure(TagsError(list.Error()));
    }

    if (auto reserved = tags.TryReserve(list.Value().ItemCount()); !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    for (usize index = 0; index < list.Value().ItemCount(); ++index) {
        auto item = list.Value().OpenItem(index);
        if (!item) {
            return Result<Out>::Failure(TagsError(item.Error()));
        }

        auto key = item.Value().Find(PackageTagKeyField);
        auto value = item.Value().Find(PackageTagValueField);
        if (!key || !value || key.Value().wireType != static_cast<uint32>(WireType::String) ||
            value.Value().wireType != static_cast<uint32>(WireType::String)) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }

        const StringView keyText = TextOf(key.Value());
        const StringView valueText = TextOf(value.Value());
        if (!ValidKey(keyText) || !ValidValue(valueText)) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }

        if (index != 0 && ComparePair(tags[index - 1].key.View(), tags[index - 1].value.View(),
                                      keyText, valueText) >= 0) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }

        PackageTag tag(allocator);
        if (auto assigned = tag.key.TryAssign(keyText); !assigned) {
            return Result<Out>::Failure(assigned.Error());
        }

        if (auto assigned = tag.value.TryAssign(valueText); !assigned) {
            return Result<Out>::Failure(assigned.Error());
        }

        (void)tags.TryPushBack(Move(tag));
    }

    return Result<Out>::Success(Move(tags));
}

} // namespace rocks
