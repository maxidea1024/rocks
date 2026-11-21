#include "AssetDatabaseInternal.h"
#include "AtomicFileWrite.h"

#include <AssetDatabase/AssetAliasTable.h>
#include <AssetDatabase/LogicalPath.h>
#include <Core/Algorithms.h>
#include <Core/FileError.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Time/DateTime.h>

// 경로 별칭 표다(Docs/Design/AssetSystem.ko.md 4.3절, 7.1절 17 A2e1).
namespace rocks {

namespace {

using asset_database_detail::AssignDisplayName;
using asset_database_detail::EditorTempStem;
using asset_database_detail::WriteFileAtomically;

// 별칭 항목 하나의 JSON 노드 수 상한(객체 + 키 넷 + 값 넷)과 최상위 여유다 — 읽기 옵션의 노드 상한
// (항목 수 상한까지 읽을 수 있게).
constexpr uint32 AliasEntryNodes = 9;
constexpr uint32 AliasRootNodes = 8;
// 하루의 tick 수다.
constexpr int64 TicksPerDay = TimeSpan::TicksPerDay;
// id(36자)·ISO 8601 시각(최대 28자)을 쓰는 고정 버퍼 크기다.
constexpr usize AliasFieldBytes = 64;

constexpr StringView VersionKey = "version";
constexpr StringView AliasesKey = "aliases";
constexpr StringView PathKey = "path";
constexpr StringView AssetKey = "asset";
constexpr StringView CreatedKey = "created";
constexpr StringView DaysKey = "days";

template <class T>
Result<T> FailAlias(AssetDiagnostic* failure, ErrorCode error, AssetAliasField field,
                    uint64 detail) noexcept {
    if (failure != nullptr) {
        *failure = AssetDiagnostic{};
        failure->code = AssetDiagnosticCode::CorruptAliasTable;
        failure->error = error;
        failure->section = static_cast<uint32>(field);
        failure->detail = detail;
    }

    return Result<T>::Failure(error);
}

template <class T>
Result<T> FailIo(AssetDiagnostic* failure, ErrorCode error) noexcept {
    if (failure != nullptr) {
        *failure = AssetDiagnostic{};
        failure->error = error;
    }

    return Result<T>::Failure(error);
}

// 객체의 키가 정확히 keys인지다(모르는 키·빠진 키 없음 — 중복 키는 읽기가 거부한다).
bool HasExactKeys(JsonValue object, Span<const StringView> keys) noexcept {
    if (!object.IsObject() || object.Size() != keys.Size()) {
        return false;
    }

    for (StringView key : keys) {
        if (!object.Find(key).IsValid()) {
            return false;
        }
    }

    return true;
}

// path(Config 루트 기준 절대 경로)를 읽는다. 없으면 existed 거짓·빈 bytes.
Result<void> ReadWhole(platform::FileSystem& files, StringView path, uint64 limit,
                       Array<uint8>& bytes, bool& existed, bool& tooLarge) noexcept {
    existed = false;
    tooLarge = false;
    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        return opened.code == platform::FileCode::NotFound
                   ? Result<void>::Success()
                   : Result<void>::Failure(FileErrorCode(opened));
    }

    existed = true;
    uint64 size = 0;
    if (auto sized = file.Size(size); !sized) {
        return Result<void>::Failure(FileErrorCode(sized));
    }

    if (size > limit) {
        tooLarge = true;
        return Result<void>::Success();
    }

    if (auto resized = bytes.TryResize(static_cast<usize>(size)); !resized) {
        return resized;
    }

    // 크기를 잰 뒤 줄었으면 EndOfFile — IO 오류로 보고 다시 시도하게 한다(쓰는 중인 파일).
    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    if (!read.status) {
        return Result<void>::Failure(FileErrorCode(read.status));
    }

    return Result<void>::Success();
}

bool ByKey(const AssetAlias& left, const AssetAlias& right) noexcept {
    return StringView::CompareBytes(left.key.View(), right.key.View()) < 0;
}

// path 텍스트의 끝 경계 문자다(prune 대조 — 객체 경로의 `.`, 인용·구분·괄호·공백).
bool IsBoundary(uint8 value) noexcept {
    switch (value) {
    case '.':
    case '"':
    case '\'':
    case ' ':
    case '\t':
    case '\r':
    case '\n':
    case ',':
    case ')':
    case ']':
        return true;
    default:
        return false;
    }
}

uint8 AsciiLower(uint8 value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<uint8>(value - 'A' + 'a') : value;
}

// bytes에서 needle(ASCII 대소문자 무시, 뒤 경계)이 나오는 줄 번호들을 lines에 붙인다.
Result<void> FindLines(Span<const uint8> bytes, StringView needle, Array<uint64>& lines) noexcept {
    const usize length = needle.ByteCount();
    uint64 line = 1;
    for (usize at = 0; at < bytes.Size(); ++at) {
        if (bytes[at] == '\n') {
            ++line;
            continue;
        }

        if (bytes.Size() - at < length) {
            continue;
        }

        bool same = true;
        for (usize offset = 0; offset < length && same; ++offset) {
            same = AsciiLower(bytes[at + offset]) ==
                   AsciiLower(static_cast<uint8>(needle.Data()[offset]));
        }

        if (!same || (at + length < bytes.Size() && !IsBoundary(bytes[at + length]))) {
            continue;
        }

        if (auto pushed = lines.TryPushBack(line); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// prune 검색의 디렉터리 항목이다(OS 이름 byte 그대로).
struct SearchChild {
    explicit SearchChild(Allocator& allocator) noexcept : name(allocator) {
    }

    Array<char> name;
    platform::FileInfo info;
};

struct SearchContext {
    Array<SearchChild>* children;
    ErrorCode error = ErrorCode::None;
};

bool CollectSearchChild(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& context = *static_cast<SearchContext*>(raw);
    SearchChild child(context.children->GetAllocator());
    if (auto appended = child.name.TryAppend(Span<const char>(entry.name, entry.nameBytes));
        !appended) {
        context.error = appended.Error();
        return false;
    }

    child.info = entry.info;
    if (auto pushed = context.children->TryPushBack(Move(child)); !pushed) {
        context.error = pushed.Error();
        return false;
    }

    return true;
}

} // namespace

int64 AssetAliasExpiryTicks(const AssetAlias& alias) noexcept {
    const int64 span = static_cast<int64>(alias.days) * TicksPerDay;
    const int64 maximum = DateTime::MaxValue().Ticks();
    const int64 created = alias.created.Ticks();
    return created > maximum - span ? maximum : created + span;
}

AssetAliasTable::AssetAliasTable(Allocator& allocator, const ProjectLayout& layout,
                                 usize maxEntries) noexcept
    : allocator_(&allocator), layout_(&layout), maxEntries_(maxEntries), entries_(allocator),
      currentBytes_(allocator), previousBytes_(allocator), previousEntries_(allocator) {
}

usize AssetAliasTable::Find(StringView key) const noexcept {
    const usize at = algo::LowerBound(
        entries_.View(), key, [](const AssetAlias& alias, StringView probe) noexcept {
            return StringView::CompareBytes(alias.key.View(), probe) < 0;
        });
    return at != entries_.Size() && entries_[at].key.View() == key ? at : AssetAliasNone;
}

Result<void> AssetAliasTable::Load(platform::FileSystem& files, AssetDiagnostic* failure) noexcept {
    entries_.Clear();
    currentBytes_.Clear();
    currentExisted_ = false;
    loaded_ = false;
    restorable_ = false;
    String path(*allocator_);
    const StringView pieces[] = {layout_->ConfigDirectory(), "/", AssetAliasFileName};
    for (StringView piece : pieces) {
        if (auto appended = path.TryAppend(piece); !appended) {
            return appended;
        }
    }

    Array<uint8> bytes(*allocator_);
    bool existed = false;
    bool tooLarge = false;
    if (auto read = ReadWhole(files, path.View(), MaxAssetAliasFileBytes, bytes, existed, tooLarge);
        !read) {
        return read.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(read.Error())
                                                      : FailIo<void>(failure, read.Error());
    }

    if (tooLarge) {
        return FailAlias<void>(failure, ErrorCode::CapacityExceeded, AssetAliasField::File,
                               AssetAliasNoEntry);
    }

    Array<AssetAlias> entries(*allocator_);
    if (existed) {
        JsonDocument document(*allocator_);
        JsonReadOptions options;
        options.maxNodes = static_cast<uint32>(MaxAssetAliases) * AliasEntryNodes + AliasRootNodes;
        const JsonError error = document.Parse(
            StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()), options);
        if (!error.Succeeded()) {
            if (error.code == JsonErrorCode::OutOfMemory) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            return FailAlias<void>(failure, ErrorCode::InvalidArgument, AssetAliasField::Json,
                                   (static_cast<uint64>(error.position.line) << 32) |
                                       error.position.column);
        }

        const JsonValue root = document.Root();
        const StringView rootKeys[] = {VersionKey, AliasesKey};
        if (!HasExactKeys(root, rootKeys)) {
            return FailAlias<void>(failure, ErrorCode::InvalidArgument, AssetAliasField::Root,
                                   AssetAliasNoEntry);
        }

        auto version = root.Find(VersionKey).TryGetInt64();
        if (!root.Find(VersionKey).IsNumber() || !version ||
            version.Value() != AssetAliasFormatVersion) {
            return FailAlias<void>(failure, ErrorCode::InvalidArgument, AssetAliasField::Version,
                                   AssetAliasNoEntry);
        }

        const JsonValue list = root.Find(AliasesKey);
        if (!list.IsArray()) {
            return FailAlias<void>(failure, ErrorCode::InvalidArgument, AssetAliasField::Aliases,
                                   AssetAliasNoEntry);
        }

        if (list.Size() > maxEntries_) {
            return FailAlias<void>(failure, ErrorCode::CapacityExceeded, AssetAliasField::Aliases,
                                   AssetAliasNoEntry);
        }

        if (auto reserved = entries.TryReserve(list.Size()); !reserved) {
            return reserved;
        }

        const StringView entryKeys[] = {PathKey, AssetKey, CreatedKey, DaysKey};
        for (usize at = 0; at < list.Size(); ++at) {
            const JsonValue item = list.At(at);
            if (!HasExactKeys(item, entryKeys)) {
                return FailAlias<void>(failure, ErrorCode::InvalidArgument, AssetAliasField::Entry,
                                       at);
            }

            const JsonValue pathValue = item.Find(PathKey);
            if (!pathValue.IsString()) {
                return FailAlias<void>(failure, ErrorCode::InvalidArgument, AssetAliasField::Path,
                                       at);
            }

            auto logical = ParseLogicalPath(*allocator_, pathValue.GetString());
            if (!logical) {
                return logical.Error() == ErrorCode::OutOfMemory
                           ? Result<void>::Failure(logical.Error())
                           : FailAlias<void>(failure, ErrorCode::InvalidArgument,
                                             AssetAliasField::Path, at);
            }

            const JsonValue assetValue = item.Find(AssetKey);
            auto asset = assetValue.IsString()
                             ? AssetId::TryParse(assetValue.GetString())
                             : Result<AssetId>::Failure(ErrorCode::InvalidArgument);
            if (!asset || asset.Value().IsNull()) {
                return FailAlias<void>(failure, ErrorCode::InvalidArgument, AssetAliasField::Asset,
                                       at);
            }

            const JsonValue createdValue = item.Find(CreatedKey);
            auto created = createdValue.IsString()
                               ? TryParseIso8601(createdValue.GetString())
                               : Result<DateTime>::Failure(ErrorCode::InvalidArgument);
            if (!created) {
                return FailAlias<void>(failure, ErrorCode::InvalidArgument,
                                       AssetAliasField::Created, at);
            }

            const JsonValue daysValue = item.Find(DaysKey);
            auto days = daysValue.IsNumber() ? daysValue.TryGetInt64()
                                             : Result<int64>::Failure(ErrorCode::InvalidArgument);
            if (!days || days.Value() < 1 || days.Value() > AssetAliasMaxDays) {
                return FailAlias<void>(failure, ErrorCode::InvalidArgument, AssetAliasField::Days,
                                       at);
            }

            AssetAlias alias(*allocator_);
            alias.asset = asset.Value();
            alias.created = created.Value();
            alias.days = static_cast<uint32>(days.Value());
            if (auto assigned = alias.path.TryAssign(logical.Value().Text()); !assigned) {
                return assigned;
            }

            if (auto assigned = alias.key.TryAssign(logical.Value().Key()); !assigned) {
                return assigned;
            }

            (void)entries.TryPushBack(Move(alias));
        }

        // 정렬이 어긋난 파일(손으로 병합)도 받는다 — 키 순으로 정렬해 중복을 본다(위치는 정렬 전
        // 번호가 아니라 첫 중복의 뒤쪽 항목이다).
        Array<usize> order(*allocator_);
        if (auto sized = order.TryResize(entries.Size()); !sized) {
            return sized;
        }

        for (usize at = 0; at < order.Size(); ++at) {
            order[at] = at;
        }

        algo::Sort(order.View(), [&](usize left, usize right) noexcept {
            const int compared =
                StringView::CompareBytes(entries[left].key.View(), entries[right].key.View());
            return compared != 0 ? compared < 0 : left < right;
        });
        for (usize at = 1; at < order.Size(); ++at) {
            if (entries[order[at]].key.View() == entries[order[at - 1]].key.View()) {
                return FailAlias<void>(failure, ErrorCode::InvalidArgument,
                                       AssetAliasField::DuplicatePath, order[at]);
            }
        }

        algo::Sort(entries.View(), ByKey);
    }

    entries_ = Move(entries);
    currentBytes_ = Move(bytes);
    currentExisted_ = existed;
    loaded_ = true;
    return Result<void>::Success();
}

Result<void> AssetAliasTable::EncodeTo(const Array<AssetAlias>& entries,
                                       Array<uint8>& out) const noexcept {
    // writer는 한 번 실패하면 이후 호출이 모두 실패한다(Finish는 InvalidArgument) — 첫 오류만
    // 남긴다(할당 실패를 OutOfMemory로 돌려준다). id·시각 텍스트는 고정 용량 버퍼에 쓴다(36자·최대
    // 28자 — 할당·실패가 없다).
    StringBuilder<1024> text(*allocator_);
    JsonWriter writer(text);
    Result<void> first = Result<void>::Success();
    auto put = [&](Result<void> step) noexcept {
        if (first && !step) {
            first = Move(step);
        }
    };
    put(writer.BeginObject());
    put(writer.Key(VersionKey));
    (void)writer.UInt64(AssetAliasFormatVersion);
    put(writer.Key(AliasesKey));
    put(writer.BeginArray());
    char fieldBytes[AliasFieldBytes];
    FixedStringBuilder field(Span<char>(fieldBytes, AliasFieldBytes));
    for (const AssetAlias& alias : entries) {
        put(writer.BeginObject());
        put(writer.Key(PathKey));
        put(writer.String(alias.path.View()));
        put(writer.Key(AssetKey));
        field.Clear();
        (void)field.TryAppendFormat("{}", alias.asset);
        put(writer.String(field.View()));
        put(writer.Key(CreatedKey));
        field.Clear();
        (void)field.TryAppendFormat("{}", alias.created);
        put(writer.String(field.View()));
        put(writer.Key(DaysKey));
        (void)writer.UInt64(alias.days);
        put(writer.EndObject());
    }

    put(writer.EndArray());
    put(writer.EndObject());
    put(writer.Finish());
    if (!first) {
        return first;
    }

    out.Clear();
    return out.TryAppend(Span<const uint8>(reinterpret_cast<const uint8*>(text.View().Data()),
                                           text.View().ByteCount()));
}

Result<void> AssetAliasTable::Write(platform::FileSystem& files, const AssetDatabaseLock& lock,
                                    Span<const StringView> removeKeys,
                                    Span<const AssetAliasInput> add, bool& notDurable,
                                    AssetDiagnostic* failure) noexcept {
    notDurable = false;
    if (!loaded_ || !lock.IsHeldFor(*layout_)) {
        return FailIo<void>(failure, ErrorCode::InvalidArgument);
    }

    // 새 표: 지울 키를 빼고(정렬 키 대조), add를 더하거나 바꾼다(같은 키는 뒤가 이긴다).
    asset_database_detail::NameSet removed(*allocator_);
    for (StringView key : removeKeys) {
        if (auto added = removed.Add(key); !added) {
            return added;
        }
    }

    removed.Finish();
    Array<AssetAlias> next(*allocator_);
    if (auto reserved = next.TryReserve(entries_.Size() + add.Size()); !reserved) {
        return reserved;
    }

    for (const AssetAlias& alias : entries_) {
        if (removed.Contains(alias.key.View())) {
            continue;
        }

        AssetAlias copy(*allocator_);
        if (auto assigned = copy.path.TryAssign(alias.path.View()); !assigned) {
            return assigned;
        }

        if (auto assigned = copy.key.TryAssign(alias.key.View()); !assigned) {
            return assigned;
        }

        copy.asset = alias.asset;
        copy.created = alias.created;
        copy.days = alias.days;
        (void)next.TryPushBack(Move(copy));
    }

    for (const AssetAliasInput& input : add) {
        if (input.asset.IsNull() || input.days < 1 || input.days > AssetAliasMaxDays) {
            return FailIo<void>(failure, ErrorCode::InvalidArgument);
        }

        auto logical = ParseLogicalPath(*allocator_, input.path);
        if (!logical) {
            if (logical.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(logical.Error());
            }

            if (failure != nullptr) {
                *failure = AssetDiagnostic{};
                failure->code = AssetDiagnosticCode::InvalidPath;
                failure->error = logical.Error();
            }

            return Result<void>::Failure(logical.Error());
        }

        AssetAlias alias(*allocator_);
        if (auto assigned = alias.path.TryAssign(logical.Value().Text()); !assigned) {
            return assigned;
        }

        if (auto assigned = alias.key.TryAssign(logical.Value().Key()); !assigned) {
            return assigned;
        }

        alias.asset = input.asset;
        alias.created = input.created;
        alias.days = input.days;
        (void)next.TryPushBack(Move(alias));
    }

    // 키 순으로 정렬하고 같은 키는 뒤(나중에 더한 것)를 남긴다 — 위치 순서가 우선순위다.
    Array<usize> order(*allocator_);
    if (auto sized = order.TryResize(next.Size()); !sized) {
        return sized;
    }

    for (usize at = 0; at < order.Size(); ++at) {
        order[at] = at;
    }

    algo::Sort(order.View(), [&](usize left, usize right) noexcept {
        const int compared =
            StringView::CompareBytes(next[left].key.View(), next[right].key.View());
        return compared != 0 ? compared < 0 : left < right;
    });
    Array<AssetAlias> merged(*allocator_);
    if (auto reserved = merged.TryReserve(next.Size()); !reserved) {
        return reserved;
    }

    for (usize at = 0; at < order.Size(); ++at) {
        if (at + 1 < order.Size() && next[order[at]].key.View() == next[order[at + 1]].key.View()) {
            continue;
        }

        (void)merged.TryPushBack(Move(next[order[at]]));
    }

    next = Move(merged);
    if (next.Size() > maxEntries_) {
        return FailIo<void>(failure, ErrorCode::CapacityExceeded);
    }
    Array<uint8> bytes(*allocator_);
    if (auto encoded = EncodeTo(next, bytes); !encoded) {
        return encoded;
    }

    auto written = WriteFileAtomically(files, *allocator_, layout_->ConfigDirectory(),
                                       AssetAliasFileName, bytes.View());
    if (!written) {
        return written.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(written.Error())
                                                         : FailIo<void>(failure, written.Error());
    }

    notDurable = written.Value().notDurable;
    previousBytes_ = Move(currentBytes_);
    previousExisted_ = currentExisted_;
    previousEntries_ = Move(entries_);
    currentBytes_ = Move(bytes);
    currentExisted_ = true;
    entries_ = Move(next);
    restorable_ = true;
    return Result<void>::Success();
}

Result<void> AssetAliasTable::Restore(platform::FileSystem& files,
                                      const AssetDatabaseLock& lock) noexcept {
    if (!lock.IsHeldFor(*layout_)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!restorable_) {
        return Result<void>::Success();
    }

    if (previousExisted_) {
        auto written = WriteFileAtomically(files, *allocator_, layout_->ConfigDirectory(),
                                           AssetAliasFileName, previousBytes_.View());
        if (!written) {
            return Result<void>::Failure(written.Error());
        }
    } else {
        String path(*allocator_);
        const StringView pieces[] = {layout_->ConfigDirectory(), "/", AssetAliasFileName};
        for (StringView piece : pieces) {
            if (auto appended = path.TryAppend(piece); !appended) {
                return appended;
            }
        }

        if (auto deleted = files.Delete(path.View().Data(), path.View().ByteCount());
            !deleted && deleted.code != platform::FileCode::NotFound) {
            return Result<void>::Failure(FileErrorCode(deleted));
        }
    }

    currentBytes_ = Move(previousBytes_);
    currentExisted_ = previousExisted_;
    entries_ = Move(previousEntries_);
    restorable_ = false;
    return Result<void>::Success();
}

Result<void> AssetAliasTable::Prune(platform::FileSystem& files, const AssetDatabaseLock& lock,
                                    int64 nowTicks, const AssetAliasSearchLimits& limits,
                                    AssetAliasPruneReport& report,
                                    AssetDiagnostic* failure) noexcept {
    report.removed = 0;
    report.inUse = 0;
    report.kept = 0;
    report.truncated = false;
    report.notDurable = false;
    report.uses.Clear();
    if (!loaded_ || !lock.IsHeldFor(*layout_)) {
        return FailIo<void>(failure, ErrorCode::InvalidArgument);
    }

    Array<usize> expired(*allocator_);
    for (usize at = 0; at < entries_.Size(); ++at) {
        if (AssetAliasExpiryTicks(entries_[at]) <= nowTicks) {
            if (auto pushed = expired.TryPushBack(at); !pushed) {
                return pushed;
            }
        }
    }

    report.kept = entries_.Size() - expired.Size();
    if (expired.IsEmpty()) {
        return Result<void>::Success();
    }

    // Config 아래 텍스트 참조 검색(깊이 우선, 링크는 따라가지 않는다).
    const StringView config = layout_->ConfigDirectory();
    Array<Array<char>> pending(*allocator_);
    if (auto pushed = pending.TryPushBack(Array<char>(*allocator_)); !pushed) {
        return pushed;
    }

    Array<bool> used(*allocator_);
    if (auto sized = used.TryResize(expired.Size()); !sized) {
        return sized;
    }

    for (bool& flag : used) {
        flag = false;
    }

    usize filesSeen = 0;
    uint64 bytesSeen = 0;
    Array<uint64> lines(*allocator_);
    while (!pending.IsEmpty()) {
        Array<char> relative = Move(pending[pending.Size() - 1]);
        (void)pending.TryPopBack();
        Array<char> directory(*allocator_);
        const StringView directoryPieces[] = {config, relative.IsEmpty() ? "" : "/",
                                              StringView(relative.Data(), relative.Size())};
        for (StringView piece : directoryPieces) {
            if (auto appended =
                    directory.TryAppend(Span<const char>(piece.Data(), piece.ByteCount()));
                !appended) {
                return appended;
            }
        }

        Array<SearchChild> children(*allocator_);
        SearchContext context{&children};
        if (auto listed = files.EnumerateDirectory(directory.Data(), directory.Size(),
                                                   &CollectSearchChild, &context);
            !listed) {
            if (listed.code == platform::FileCode::NotFound) {
                continue;
            }

            return FailIo<void>(failure, FileErrorCode(listed));
        }

        if (context.error != ErrorCode::None) {
            return Result<void>::Failure(context.error);
        }

        for (SearchChild& child : children) {
            if (child.info.isLink) {
                continue;
            }

            const StringView name(child.name.Data(), child.name.Size());
            Array<char> childRelative(*allocator_);
            const StringView childPieces[] = {StringView(relative.Data(), relative.Size()),
                                              relative.IsEmpty() ? "" : "/", name};
            for (StringView piece : childPieces) {
                if (auto appended =
                        childRelative.TryAppend(Span<const char>(piece.Data(), piece.ByteCount()));
                    !appended) {
                    return appended;
                }
            }

            if (child.info.type == platform::FileType::Directory) {
                if (auto pushed = pending.TryPushBack(Move(childRelative)); !pushed) {
                    return pushed;
                }

                continue;
            }

            // 별칭 표 자신과 그 쓰기 임시 파일은 검색하지 않는다(Config 바로 아래).
            if (child.info.type != platform::FileType::File ||
                (relative.IsEmpty() &&
                 (name == AssetAliasFileName || EditorTempStem(name) == AssetAliasFileName))) {
                continue;
            }

            ++filesSeen;
            bytesSeen += child.info.size;
            if (filesSeen > limits.maxFiles || bytesSeen > limits.maxTotalBytes ||
                child.info.size > limits.maxFileBytes) {
                report.truncated = true;
                break;
            }

            Array<char> full(*allocator_);
            const StringView fullPieces[] = {
                config, "/", StringView(childRelative.Data(), childRelative.Size())};
            for (StringView piece : fullPieces) {
                if (auto appended =
                        full.TryAppend(Span<const char>(piece.Data(), piece.ByteCount()));
                    !appended) {
                    return appended;
                }
            }

            Array<uint8> bytes(*allocator_);
            bool existed = false;
            bool tooLarge = false;
            if (auto read = ReadWhole(files, StringView(full.Data(), full.Size()),
                                      limits.maxFileBytes, bytes, existed, tooLarge);
                !read) {
                return read.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(read.Error())
                                                              : FailIo<void>(failure, read.Error());
            }

            // 열거 뒤 사라진 파일은 건너뛰고, 열거 뒤 커진 파일은 상한에 닿은 것이다.
            if (tooLarge) {
                report.truncated = true;
                break;
            }

            for (usize slot = 0; slot < expired.Size(); ++slot) {
                lines.Clear();
                if (auto found =
                        FindLines(bytes.View(), entries_[expired[slot]].path.View(), lines);
                    !found) {
                    return found;
                }

                for (uint64 line : lines) {
                    AssetAliasUse use(*allocator_);
                    use.line = line;
                    Result<void> step = use.alias.TryAssign(entries_[expired[slot]].path.View());
                    if (step) {
                        step = AssignDisplayName(
                            use.file, StringView(childRelative.Data(), childRelative.Size()));
                    }

                    if (step) {
                        // 프로젝트 루트 상대 경로로 보고한다.
                        String prefixed(*allocator_);
                        step = prefixed.TryAppend("Config/");
                        if (step) {
                            step = prefixed.TryAppend(use.file.View());
                        }

                        if (step) {
                            use.file = Move(prefixed);
                        }
                    }

                    if (step) {
                        step = report.uses.TryPushBack(Move(use));
                    }

                    if (!step) {
                        return step;
                    }

                    used[slot] = true;
                }
            }
        }

        if (report.truncated) {
            break;
        }
    }

    if (report.truncated) {
        // 검색을 멈췄다(남은 디렉터리는 보지 않는다) — 아무것도 지우지 않는다.
        report.kept = entries_.Size();
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->code = AssetDiagnosticCode::AliasSearchTruncated;
        }

        return Result<void>::Success();
    }

    Array<StringView> removeKeys(*allocator_);
    for (usize slot = 0; slot < expired.Size(); ++slot) {
        if (used[slot]) {
            ++report.inUse;
        } else if (auto pushed = removeKeys.TryPushBack(entries_[expired[slot]].key.View());
                   !pushed) {
            return pushed;
        }
    }

    if (removeKeys.IsEmpty()) {
        return Result<void>::Success();
    }

    bool notDurable = false;
    if (auto written = Write(files, lock, removeKeys.View(), {}, notDurable, failure); !written) {
        return written;
    }

    report.removed = removeKeys.Size();
    report.notDurable = notDurable;
    return Result<void>::Success();
}

} // namespace rocks
