#include <AssetDatabase/ImportRecord.h>
#include <Base/Endian.h>
#include <Core/Algorithms.h>
#include <Serialization/Record.h>
#include <Serialization/RecordList.h>

// ImportRecord 값 스키마의 쓰기·읽기·ImportOwner 대조다(Docs/Design/AssetSystem 5.4절, 7.1절
// 15 — A2c2a). 유니티 빌드에서 다른 파일과 이름이 겹치지 않게 도우미는 Import 접두를 단다.
namespace rocks {

namespace {

constexpr usize ImportGuidBytes = 16;
constexpr usize ImportU32Bytes = 4;
constexpr usize ImportU64Bytes = 8;
// 소유 기록의 필드 수(options 제외)와 참조 기록의 필드 수, 항목 Record의 필드 수다.
constexpr usize ImportOwnerRecordFields = 6;
constexpr usize ImportReferenceRecordFields = 4;
constexpr usize ImportSourceFields = 5;
constexpr usize ImportOutputFields = 3;
constexpr usize ImportTagFields = 2;

// Record 계층 오류를 읽기 오류로 바꾼다: 할당 실패·지원 범위 밖 버전은 그대로, 그 밖은 형식 위반
// CorruptData다(PackageTags와 같은 규칙 — AssetSystem 19장).
ErrorCode ImportReadError(ErrorCode error) noexcept {
    if (error == ErrorCode::OutOfMemory || error == ErrorCode::Unsupported) {
        return error;
    }

    return ErrorCode::CorruptData;
}

// 상한·하한 안의 유효 UTF-8이고 NUL이 없는지다.
bool ImportValidText(StringView text, usize minBytes, usize maxBytes) noexcept {
    if (text.ByteCount() < minBytes || text.ByteCount() > maxBytes || !text.IsValidUtf8()) {
        return false;
    }

    for (usize index = 0; index < text.ByteCount(); ++index) {
        if (text.Data()[index] == '\0') {
            return false;
        }
    }

    return true;
}

// 원본 루트 이름이다(빈 값은 루트 없음, 경로 구분자 금지).
bool ImportValidRoot(StringView root) noexcept {
    if (!ImportValidText(root, 0, MaxImportRootBytes)) {
        return false;
    }

    for (usize index = 0; index < root.ByteCount(); ++index) {
        const char c = root.Data()[index];
        if (c == '/' || c == '\\') {
            return false;
        }
    }

    return true;
}

// `/`로 나눈 성분들이다: 빈 성분(앞·뒤·연속 `/`), `.`·`..` 성분, `\`를 거부한다.
bool ImportValidComponents(StringView path) noexcept {
    const char* data = path.Data();
    usize start = 0;
    for (usize index = 0; index <= path.ByteCount(); ++index) {
        if (index < path.ByteCount() && data[index] == '\\') {
            return false;
        }

        if (index == path.ByteCount() || data[index] == '/') {
            const usize length = index - start;
            if (length == 0 || (length == 1 && data[start] == '.') ||
                (length == 2 && data[start] == '.' && data[start + 1] == '.')) {
                return false;
            }

            start = index + 1;
        }
    }

    return true;
}

// 원본 경로다(4.5절, 7.1절 18): 루트가 있으면 그 아래 상대 경로, 빈 루트면 `/` 구분 절대 경로 —
// POSIX `/a`, UNC `//server/share/a`, 드라이브 `C:/a`(기록은 호스트와 무관한 문법만 본다). 뒤는
// 같은 성분 규칙이다.
bool ImportValidSourcePath(StringView root, StringView path) noexcept {
    if (!ImportValidText(path, 1, MaxImportRelativePathBytes)) {
        return false;
    }

    if (!root.IsEmpty()) {
        return ImportValidComponents(path);
    }

    const char* data = path.Data();
    const usize bytes = path.ByteCount();
    constexpr usize DrivePrefixBytes = 3;
    constexpr usize UncPrefixBytes = 2;
    usize prefix = 0;
    if (bytes > DrivePrefixBytes && data[1] == ':' && data[2] == '/' &&
        ((data[0] >= 'A' && data[0] <= 'Z') || (data[0] >= 'a' && data[0] <= 'z'))) {
        prefix = DrivePrefixBytes;
    } else if (bytes > UncPrefixBytes && data[0] == '/' && data[1] == '/') {
        prefix = UncPrefixBytes;
    } else if (bytes > 1 && data[0] == '/') {
        prefix = 1;
    } else {
        return false;
    }

    const StringView rest(data + prefix, bytes - prefix);
    if (!ImportValidComponents(rest)) {
        return false;
    }

    // UNC는 서버·공유 다음에 성분이 하나 이상 있어야 한다.
    if (prefix == UncPrefixBytes) {
        usize separators = 0;
        for (usize index = 0; index < rest.ByteCount(); ++index) {
            separators += rest.Data()[index] == '/' ? 1 : 0;
        }

        return separators >= UncPrefixBytes;
    }

    return true;
}

bool ImportValidSource(StringView role, StringView root, StringView path) noexcept {
    return ImportValidText(role, 1, MaxImportRoleBytes) && ImportValidRoot(root) &&
           ImportValidSourcePath(root, path);
}

bool ImportValidOutput(StringView key, AssetId asset, TypeId type) noexcept {
    return ImportValidText(key, 0, MaxImportOutputKeyBytes) && !asset.IsNull() &&
           !(type == TypeId{});
}

// 태그 (key, value) byte 순이다.
bool ImportTagLess(StringView leftKey, StringView leftValue, StringView rightKey,
                   StringView rightValue) noexcept {
    const int byKey = StringView::CompareBytes(leftKey, rightKey);
    return byKey != 0 ? byKey < 0 : StringView::CompareBytes(leftValue, rightValue) < 0;
}

Span<const uint8> ImportBytesOf(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

StringView ImportTextOf(const FieldView& field) noexcept {
    return StringView(reinterpret_cast<const char*>(field.payload.Data()), field.payload.Size());
}

void ImportStoreGuid(uint8 (&out)[ImportGuidBytes], Guid value) noexcept {
    StoreLittleEndian<uint64>(out, value.high);
    StoreLittleEndian<uint64>(out + ImportU64Bytes, value.low);
}

// 필드가 기대 wire·크기인지 본다(크기 0은 가변).
bool ImportFieldIs(const FieldView& field, WireType type, usize size) noexcept {
    return field.wireType == static_cast<uint32>(type) &&
           (size == 0 || field.payload.Size() == size);
}

Guid ImportLoadGuid(const FieldView& field) noexcept {
    return Guid{LoadLittleEndian<uint64>(field.payload.Data()),
                LoadLittleEndian<uint64>(field.payload.Data() + ImportU64Bytes)};
}

// index 표를 만들어 less 순으로 정렬한다. 같은 원소의 순서는 정하지 않는다(쓰는 곳은 모두 같은
// 이웃을 중복으로 거부한다).
template <class Less>
Result<Array<usize>> ImportSortedOrder(Allocator& allocator, usize count, Less less) noexcept {
    Array<usize> order(allocator);
    if (auto reserved = order.TryReserve(count); !reserved) {
        return Result<Array<usize>>::Failure(reserved.Error());
    }

    for (usize index = 0; index < count; ++index) {
        (void)order.TryPushBack(index);
    }

    algo::Sort(order.View(), less);
    return Result<Array<usize>>::Success(Move(order));
}

// 정렬된 order에서 이웃이 같은(less가 어느 쪽으로도 참이 아닌) 쌍이 있는지다.
template <class Less>
bool ImportHasNeighborDuplicate(const Array<usize>& order, Less less) noexcept {
    for (usize position = 1; position < order.Size(); ++position) {
        if (!less(order[position - 1], order[position])) {
            return true;
        }
    }

    return false;
}

// 출력 asset이 유일하고 self를 포함하는지다(쓰기·읽기 공통).
template <class AssetAt>
Result<bool> ImportOutputsValid(Allocator& allocator, usize count, AssetId self,
                                AssetAt assetAt) noexcept {
    auto less = [&](usize left, usize right) noexcept {
        return GuidLess(assetAt(left).value, assetAt(right).value);
    };
    auto order = ImportSortedOrder(allocator, count, less);
    if (!order) {
        return Result<bool>::Failure(order.Error());
    }

    if (ImportHasNeighborDuplicate(order.Value(), less)) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; index < count; ++index) {
        if (assetAt(index) == self) {
            return Result<bool>::Success(true);
        }
    }

    return Result<bool>::Success(false);
}

// 원본 role이 유일한지다(쓰기·읽기 공통).
template <class RoleAt>
Result<bool> ImportRolesUnique(Allocator& allocator, usize count, RoleAt roleAt) noexcept {
    auto less = [&](usize left, usize right) noexcept {
        return StringView::CompareBytes(roleAt(left), roleAt(right)) < 0;
    };
    auto order = ImportSortedOrder(allocator, count, less);
    if (!order) {
        return Result<bool>::Failure(order.Error());
    }

    return Result<bool>::Success(!ImportHasNeighborDuplicate(order.Value(), less));
}

Result<void> ImportAddGuid(RecordBuilder& builder, Guid id, Guid value) noexcept {
    uint8 bytes[ImportGuidBytes];
    ImportStoreGuid(bytes, value);
    return builder.AddField(id, WireType::Guid, bytes);
}

Result<Array<uint8>> ImportBuildSource(Allocator& allocator,
                                       const ImportSourceInput& source) noexcept {
    RecordBuilder item(allocator, ImportSourceSchema, ImportRecordVersion);
    uint8 size[ImportU64Bytes];
    StoreLittleEndian<uint64>(size, source.bytes);
    Result<void> step =
        item.AddField(ImportSourceRoleField, WireType::String, ImportBytesOf(source.role));
    if (step) {
        step = item.AddField(ImportSourceRootField, WireType::String, ImportBytesOf(source.root));
    }

    if (step) {
        step = item.AddField(ImportSourcePathField, WireType::String,
                             ImportBytesOf(source.relativePath));
    }

    if (step) {
        step = item.AddField(ImportSourceSha256Field, WireType::Bytes, source.sha256.bytes);
    }

    if (step) {
        step = item.AddField(ImportSourceBytesField, WireType::U64, size);
    }

    if (!step) {
        return Result<Array<uint8>>::Failure(step.Error());
    }

    return item.Build();
}

Result<Array<uint8>> ImportBuildOutput(Allocator& allocator,
                                       const ImportOutputInput& output) noexcept {
    RecordBuilder item(allocator, ImportOutputSchema, ImportRecordVersion);
    Result<void> step =
        item.AddField(ImportOutputKeyField, WireType::String, ImportBytesOf(output.outputKey));
    if (step) {
        step = ImportAddGuid(item, ImportOutputAssetField, output.asset.value);
    }

    if (step) {
        step = ImportAddGuid(item, ImportOutputTypeField, output.type);
    }

    if (!step) {
        return Result<Array<uint8>>::Failure(step.Error());
    }

    return item.Build();
}

Result<Array<uint8>> ImportBuildTag(Allocator& allocator, const ImportTagInput& tag) noexcept {
    RecordBuilder item(allocator, ImportTagSchema, ImportRecordVersion);
    Result<void> step = item.AddField(ImportTagKeyField, WireType::String, ImportBytesOf(tag.key));
    if (step) {
        step = item.AddField(ImportTagValueField, WireType::String, ImportBytesOf(tag.value));
    }

    if (!step) {
        return Result<Array<uint8>>::Failure(step.Error());
    }

    return item.Build();
}

// 소유 기록 입력의 규칙을 본다(참이면 유효). 할당 실패는 그 오류다.
Result<bool> ImportOwnerInputValid(Allocator& allocator, AssetId self,
                                   const ImportRecordInput& input) noexcept {
    if (input.importerId == Guid{} ||
        !ImportValidText(input.importerName, 1, MaxImporterNameBytes) || input.sources.IsEmpty() ||
        input.sources.Size() > MaxImportSources || input.outputs.IsEmpty() ||
        input.outputs.Size() > MaxImportOutputs || input.tags.Size() > MaxImportTags ||
        (!input.options.IsEmpty() && !ReadRecordHeader(input.options))) {
        return Result<bool>::Success(false);
    }

    for (const ImportSourceInput& source : input.sources) {
        if (!ImportValidSource(source.role, source.root, source.relativePath)) {
            return Result<bool>::Success(false);
        }
    }

    for (const ImportOutputInput& output : input.outputs) {
        if (!ImportValidOutput(output.outputKey, output.asset, output.type)) {
            return Result<bool>::Success(false);
        }
    }

    for (const ImportTagInput& tag : input.tags) {
        if (!IsValidImportTag(input.importerName, tag.key, tag.value)) {
            return Result<bool>::Success(false);
        }
    }

    auto roles = ImportRolesUnique(allocator, input.sources.Size(),
                                   [&](usize index) noexcept { return input.sources[index].role; });
    if (!roles || !roles.Value()) {
        return roles;
    }

    return ImportOutputsValid(allocator, input.outputs.Size(), self,
                              [&](usize index) noexcept { return input.outputs[index].asset; });
}

// 소유 기록의 원본·옵션·출력 필드를 더한다(outputs는 outputKey 순 — 같은 key는 InvalidArgument).
Result<void> ImportAddOwnerFields(Allocator& allocator, RecordBuilder& record,
                                  const ImportRecordInput& input) noexcept {
    RecordListBuilder sources(allocator);
    for (const ImportSourceInput& source : input.sources) {
        auto built = ImportBuildSource(allocator, source);
        if (!built) {
            return Result<void>::Failure(built.Error());
        }

        if (auto added = sources.AddItem(built.Value().View()); !added) {
            return added;
        }
    }

    if (auto finished = sources.Finish(record, ImportRecordSourcesField); !finished) {
        return finished;
    }

    if (!input.options.IsEmpty()) {
        if (auto added = record.AddField(ImportRecordOptionsField, WireType::Record, input.options);
            !added) {
            return added;
        }
    }

    auto keyLess = [&](usize left, usize right) noexcept {
        return StringView::CompareBytes(input.outputs[left].outputKey,
                                        input.outputs[right].outputKey) < 0;
    };
    auto order = ImportSortedOrder(allocator, input.outputs.Size(), keyLess);
    if (!order) {
        return Result<void>::Failure(order.Error());
    }

    if (ImportHasNeighborDuplicate(order.Value(), keyLess)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    RecordListBuilder outputs(allocator);
    for (usize index : order.Value()) {
        auto built = ImportBuildOutput(allocator, input.outputs[index]);
        if (!built) {
            return Result<void>::Failure(built.Error());
        }

        if (auto added = outputs.AddItem(built.Value().View()); !added) {
            return added;
        }
    }

    if (auto finished = outputs.Finish(record, ImportRecordOutputsField); !finished) {
        return finished;
    }

    // 태그는 있을 때만 쓴다(빈 목록 필드는 읽기가 거부한다) — (key, value) 순, 같은 쌍은
    // InvalidArgument.
    if (input.tags.IsEmpty()) {
        return Result<void>::Success();
    }

    auto tagLess = [&](usize left, usize right) noexcept {
        return ImportTagLess(input.tags[left].key, input.tags[left].value, input.tags[right].key,
                             input.tags[right].value);
    };
    auto tagOrder = ImportSortedOrder(allocator, input.tags.Size(), tagLess);
    if (!tagOrder) {
        return Result<void>::Failure(tagOrder.Error());
    }

    if (ImportHasNeighborDuplicate(tagOrder.Value(), tagLess)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    RecordListBuilder tags(allocator);
    for (usize index : tagOrder.Value()) {
        auto built = ImportBuildTag(allocator, input.tags[index]);
        if (!built) {
            return Result<void>::Failure(built.Error());
        }

        if (auto added = tags.AddItem(built.Value().View()); !added) {
            return added;
        }
    }

    return tags.Finish(record, ImportRecordTagsField);
}

// 원본 목록 필드를 읽는다(1–256개, role 유일).
Result<void> ImportReadSources(Allocator& allocator, const RecordReader& record,
                               const FieldView& field, ImportRecord& out) noexcept {
    auto list = RecordListReader::OpenField(record, field, ImportSourceSchema, ImportRecordVersion,
                                            ImportRecordVersion);
    if (!list) {
        return Result<void>::Failure(ImportReadError(list.Error()));
    }

    const usize count = list.Value().ItemCount();
    if (count == 0 || count > MaxImportSources) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto reserved = out.sources.TryReserve(count); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < count; ++index) {
        auto item = list.Value().OpenItem(index);
        if (!item) {
            return Result<void>::Failure(ImportReadError(item.Error()));
        }

        auto role = item.Value().Find(ImportSourceRoleField);
        auto root = item.Value().Find(ImportSourceRootField);
        auto path = item.Value().Find(ImportSourcePathField);
        auto sha = item.Value().Find(ImportSourceSha256Field);
        auto size = item.Value().Find(ImportSourceBytesField);
        if (!role || !root || !path || !sha || !size ||
            item.Value().FieldCount() != ImportSourceFields ||
            !ImportFieldIs(role.Value(), WireType::String, 0) ||
            !ImportFieldIs(root.Value(), WireType::String, 0) ||
            !ImportFieldIs(path.Value(), WireType::String, 0) ||
            !ImportFieldIs(sha.Value(), WireType::Bytes, Sha256Digest::Bytes) ||
            !ImportFieldIs(size.Value(), WireType::U64, ImportU64Bytes) ||
            !ImportValidSource(ImportTextOf(role.Value()), ImportTextOf(root.Value()),
                               ImportTextOf(path.Value()))) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        ImportSource source(allocator);
        Result<void> step = source.role.TryAssign(ImportTextOf(role.Value()));
        if (step) {
            step = source.root.TryAssign(ImportTextOf(root.Value()));
        }

        if (step) {
            step = source.relativePath.TryAssign(ImportTextOf(path.Value()));
        }

        if (!step) {
            return step;
        }

        for (usize at = 0; at < Sha256Digest::Bytes; ++at) {
            source.sha256.bytes[at] = sha.Value().payload[at];
        }

        source.bytes = LoadLittleEndian<uint64>(size.Value().payload.Data());
        (void)out.sources.TryPushBack(Move(source));
    }

    auto roles = ImportRolesUnique(allocator, out.sources.Size(), [&](usize index) noexcept {
        return out.sources[index].role.View();
    });
    if (!roles) {
        return Result<void>::Failure(roles.Error());
    }

    return roles.Value() ? Result<void>::Success() : Result<void>::Failure(ErrorCode::CorruptData);
}

// 출력 목록 필드를 읽는다(1–1024개, outputKey 엄격 오름차순, asset 유일·self 포함).
Result<void> ImportReadOutputs(Allocator& allocator, AssetId self, const RecordReader& record,
                               const FieldView& field, ImportRecord& out) noexcept {
    auto list = RecordListReader::OpenField(record, field, ImportOutputSchema, ImportRecordVersion,
                                            ImportRecordVersion);
    if (!list) {
        return Result<void>::Failure(ImportReadError(list.Error()));
    }

    const usize count = list.Value().ItemCount();
    if (count == 0 || count > MaxImportOutputs) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto reserved = out.outputs.TryReserve(count); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < count; ++index) {
        auto item = list.Value().OpenItem(index);
        if (!item) {
            return Result<void>::Failure(ImportReadError(item.Error()));
        }

        auto key = item.Value().Find(ImportOutputKeyField);
        auto asset = item.Value().Find(ImportOutputAssetField);
        auto type = item.Value().Find(ImportOutputTypeField);
        if (!key || !asset || !type || item.Value().FieldCount() != ImportOutputFields ||
            !ImportFieldIs(key.Value(), WireType::String, 0) ||
            !ImportFieldIs(asset.Value(), WireType::Guid, ImportGuidBytes) ||
            !ImportFieldIs(type.Value(), WireType::Guid, ImportGuidBytes)) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        const StringView keyText = ImportTextOf(key.Value());
        const AssetId assetId{ImportLoadGuid(asset.Value())};
        const TypeId typeId = ImportLoadGuid(type.Value());
        if (!ImportValidOutput(keyText, assetId, typeId) ||
            (index != 0 &&
             StringView::CompareBytes(out.outputs[index - 1].outputKey.View(), keyText) >= 0)) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        ImportOutput output(allocator);
        if (auto assigned = output.outputKey.TryAssign(keyText); !assigned) {
            return assigned;
        }

        output.asset = assetId;
        output.type = typeId;
        (void)out.outputs.TryPushBack(Move(output));
    }

    auto valid = ImportOutputsValid(allocator, out.outputs.Size(), self,
                                    [&](usize index) noexcept { return out.outputs[index].asset; });
    if (!valid) {
        return Result<void>::Failure(valid.Error());
    }

    return valid.Value() ? Result<void>::Success() : Result<void>::Failure(ErrorCode::CorruptData);
}

// 태그 목록 필드를 읽는다(1–MaxImportTags개 — 빈 목록은 필드를 쓰지 않는다, (key, value) 엄격
// 오름차순, 쌍마다 IsValidImportTag).
Result<void> ImportReadTags(Allocator& allocator, const RecordReader& record,
                            const FieldView& field, ImportRecord& out) noexcept {
    auto list = RecordListReader::OpenField(record, field, ImportTagSchema, ImportRecordVersion,
                                            ImportRecordVersion);
    if (!list) {
        return Result<void>::Failure(ImportReadError(list.Error()));
    }

    const usize count = list.Value().ItemCount();
    if (count == 0 || count > MaxImportTags) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto reserved = out.tags.TryReserve(count); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < count; ++index) {
        auto item = list.Value().OpenItem(index);
        if (!item) {
            return Result<void>::Failure(ImportReadError(item.Error()));
        }

        auto key = item.Value().Find(ImportTagKeyField);
        auto value = item.Value().Find(ImportTagValueField);
        if (!key || !value || item.Value().FieldCount() != ImportTagFields ||
            !ImportFieldIs(key.Value(), WireType::String, 0) ||
            !ImportFieldIs(value.Value(), WireType::String, 0)) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        const StringView keyText = ImportTextOf(key.Value());
        const StringView valueText = ImportTextOf(value.Value());
        if (!IsValidImportTag(out.importerName.View(), keyText, valueText) ||
            (index != 0 && !ImportTagLess(out.tags[index - 1].key.View(),
                                          out.tags[index - 1].value.View(), keyText, valueText))) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        ImportTag tag(allocator);
        Result<void> step = tag.key.TryAssign(keyText);
        if (step) {
            step = tag.value.TryAssign(valueText);
        }

        if (!step) {
            return step;
        }

        (void)out.tags.TryPushBack(Move(tag));
    }

    return Result<void>::Success();
}

} // namespace

bool IsValidImportSourceLocation(StringView root, StringView relativePath) noexcept {
    return ImportValidRoot(root) && ImportValidSourcePath(root, relativePath);
}

bool IsValidImportTag(StringView importerName, StringView key, StringView value) noexcept {
    // 접두 `<importerName>.` 뒤에 한 byte 이상이 있어야 한다.
    if (importerName.IsEmpty() || key.ByteCount() <= importerName.ByteCount() + 1 ||
        !key.StartsWith(importerName, StringComparison::CaseSensitive) ||
        key.Data()[importerName.ByteCount()] != '.') {
        return false;
    }

    return ImportValidText(key, 1, MaxPackageTagKeyBytes) &&
           ImportValidText(value, 0, PackageMaxStringBytes);
}

Result<Array<uint8>> WriteImportRecord(Allocator& allocator, AssetId self,
                                       const ImportRecordInput& input) noexcept {
    using Out = Array<uint8>;
    if (self.IsNull() || input.owner.IsNull()) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    const bool owned = input.owner == self;
    if (owned) {
        auto valid = ImportOwnerInputValid(allocator, self, input);
        if (!valid) {
            return Result<Out>::Failure(valid.Error());
        }

        if (!valid.Value()) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }
    } else if (!(input.importerId == Guid{}) || !input.importerName.IsEmpty() ||
               input.importerVersion != 0 || !input.sources.IsEmpty() || !input.options.IsEmpty() ||
               !input.outputs.IsEmpty() || !input.tags.IsEmpty()) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    RecordBuilder record(allocator, ImportRecordSchema, ImportRecordVersion);
    uint8 version[ImportU32Bytes];
    StoreLittleEndian<uint32>(version, input.importerVersion);
    Result<void> step = ImportAddGuid(record, ImportRecordImporterIdField, input.importerId);
    if (step) {
        step = record.AddField(ImportRecordImporterNameField, WireType::String,
                               ImportBytesOf(input.importerName));
    }

    if (step) {
        step = record.AddField(ImportRecordImporterVersionField, WireType::U32, version);
    }

    if (step && owned) {
        step = ImportAddOwnerFields(allocator, record, input);
    }

    if (step) {
        step = ImportAddGuid(record, ImportRecordOwnerField, input.owner.value);
    }

    if (!step) {
        return Result<Out>::Failure(step.Error());
    }

    return record.Build();
}

Result<ImportRecord> ReadImportRecord(Allocator& allocator, AssetId self,
                                      Span<const uint8> bytes) noexcept {
    using Out = ImportRecord;
    if (self.IsNull() || bytes.IsEmpty()) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    // envelope 버전이 더 새면 Unsupported, 그 밖의 header 위반은 InvalidArgument다.
    auto header = ReadRecordHeader(bytes);
    if (!header) {
        return Result<Out>::Failure(ImportReadError(header.Error()));
    }

    if (!(header.Value().schema == ImportRecordSchema)) {
        return Result<Out>::Failure(ErrorCode::Unsupported);
    }

    auto record = RecordReader::Open(allocator, bytes, ImportRecordSchema, ImportRecordVersion,
                                     ImportRecordVersion);
    if (!record) {
        return Result<Out>::Failure(ImportReadError(record.Error()));
    }

    RecordReader& reader = record.Value();
    auto importerId = reader.Find(ImportRecordImporterIdField);
    auto importerName = reader.Find(ImportRecordImporterNameField);
    auto importerVersion = reader.Find(ImportRecordImporterVersionField);
    auto owner = reader.Find(ImportRecordOwnerField);
    if (!importerId || !importerName || !importerVersion || !owner ||
        !ImportFieldIs(importerId.Value(), WireType::Guid, ImportGuidBytes) ||
        !ImportFieldIs(importerName.Value(), WireType::String, 0) ||
        !ImportFieldIs(importerVersion.Value(), WireType::U32, ImportU32Bytes) ||
        !ImportFieldIs(owner.Value(), WireType::Guid, ImportGuidBytes)) {
        return Result<Out>::Failure(ErrorCode::CorruptData);
    }

    ImportRecord out(allocator);
    out.owner = AssetId{ImportLoadGuid(owner.Value())};
    out.importerId = ImportLoadGuid(importerId.Value());
    out.importerVersion = LoadLittleEndian<uint32>(importerVersion.Value().payload.Data());
    const StringView name = ImportTextOf(importerName.Value());
    if (out.owner.IsNull()) {
        return Result<Out>::Failure(ErrorCode::CorruptData);
    }

    auto sources = reader.Find(ImportRecordSourcesField);
    auto options = reader.Find(ImportRecordOptionsField);
    auto outputs = reader.Find(ImportRecordOutputsField);
    auto tags = reader.Find(ImportRecordTagsField);
    if (!out.OwnedBy(self)) {
        // 참조 기록: owner만 의미가 있다.
        if (!(out.importerId == Guid{}) || !name.IsEmpty() || out.importerVersion != 0 ||
            reader.FieldCount() != ImportReferenceRecordFields) {
            return Result<Out>::Failure(ErrorCode::CorruptData);
        }

        return Result<Out>::Success(Move(out));
    }

    const usize expectedFields = ImportOwnerRecordFields + (options ? 1 : 0) + (tags ? 1 : 0);
    if (out.importerId == Guid{} || !ImportValidText(name, 1, MaxImporterNameBytes) || !sources ||
        !outputs || reader.FieldCount() != expectedFields ||
        !ImportFieldIs(sources.Value(), WireType::RecordList, 0) ||
        !ImportFieldIs(outputs.Value(), WireType::RecordList, 0)) {
        return Result<Out>::Failure(ErrorCode::CorruptData);
    }

    if (auto assigned = out.importerName.TryAssign(name); !assigned) {
        return Result<Out>::Failure(assigned.Error());
    }

    if (auto read = ImportReadSources(allocator, reader, sources.Value(), out); !read) {
        return Result<Out>::Failure(read.Error());
    }

    if (options) {
        const FieldView& field = options.Value();
        if (!ImportFieldIs(field, WireType::Record, 0) || field.payload.IsEmpty() ||
            !ReadRecordHeader(field.payload)) {
            return Result<Out>::Failure(ErrorCode::CorruptData);
        }

        if (auto copied = out.options.TryAppend(field.payload); !copied) {
            return Result<Out>::Failure(copied.Error());
        }
    }

    if (auto read = ImportReadOutputs(allocator, self, reader, outputs.Value(), out); !read) {
        return Result<Out>::Failure(read.Error());
    }

    if (tags) {
        if (!ImportFieldIs(tags.Value(), WireType::RecordList, 0)) {
            return Result<Out>::Failure(ErrorCode::CorruptData);
        }

        if (auto read = ImportReadTags(allocator, reader, tags.Value(), out); !read) {
            return Result<Out>::Failure(read.Error());
        }
    }

    return Result<Out>::Success(Move(out));
}

Result<void> CheckImportOwner(AssetId self, const ImportRecord* record,
                              Span<const PackageDependencyEntry> dependencies) noexcept {
    if (self.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize count = 0;
    AssetId found{};
    for (const PackageDependencyEntry& entry : dependencies) {
        if (entry.kind == PackageDependencyKind::ImportOwner) {
            ++count;
            found = entry.asset;
        }
    }

    const bool reference = record != nullptr && !record->OwnedBy(self);
    if (reference ? (count != 1 || !(found == record->owner)) : count != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks
