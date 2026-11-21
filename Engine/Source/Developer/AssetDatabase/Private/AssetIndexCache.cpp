#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetIndexCache.h>
#include <Core/Algorithms.h>
#include <Core/Build/BuildInfo.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/FileError.h>
#include <Core/Text/Unicode.h>
#include <Object/AssetId.h>

// 색인 캐시의 형식·검사·읽기·쓰기다(Docs/Design/AssetSystem.ko.md 7.1절 16 A2d1).
namespace rocks {

namespace {

using asset_database_detail::EditorAppendGuidText;
using asset_database_detail::EditorTempSuffix;

// 파일 첫 4 byte다("RKAI").
constexpr uint32 CacheMagic = 0x49414b52u;
// 헤더 byte 수다: magic·버전 셋·Unicode·호환 주부·정규화 플래그와 예약 셋·customVersions·도구 빌드
// ID·항목 수·본문 byte·본문 SHA-256.
constexpr usize DigestBytes = 32;
constexpr usize ReservedBytes = 3;
constexpr usize HeaderBytes = 4 + 4 * 3 + DigestBytes + 2 * 2 + 1 + ReservedBytes + DigestBytes +
                              DigestBytes + 4 + 8 + DigestBytes;
// 항목 하나의 최소 byte(빈 이름·의존·태그 없음·요약 없음)다 — 항목 수 상한의 근거.
constexpr usize GuidBytes = 16;
constexpr usize MinEntryBytes = 4 + DigestBytes + GuidBytes * 2 + 1 + 4 + 4 + 1;
// 의존 하나·태그 하나의 최소 byte다.
constexpr usize DependencyBytes = GuidBytes + 4 + GuidBytes + 1;
constexpr usize MinTagBytes = 4 + 4;

void WriteGuid(ByteWriter& writer, Guid value) noexcept {
    writer.WriteU64LE(value.high);
    writer.WriteU64LE(value.low);
}

Guid ReadGuid(ByteReader& reader) noexcept {
    Guid value;
    value.high = reader.ReadU64LE();
    value.low = reader.ReadU64LE();
    return value;
}

void WriteDigest(ByteWriter& writer, const Sha256Digest& digest) noexcept {
    writer.WriteBytes(Span<const uint8>(digest.bytes, DigestBytes));
}

bool ReadDigest(ByteReader& reader, Sha256Digest& out) noexcept {
    return reader.ReadInto(Span<uint8>(out.bytes, DigestBytes));
}

void WriteText(ByteWriter& writer, StringView text) noexcept {
    writer.WriteU32LE(static_cast<uint32>(text.ByteCount()));
    writer.WriteBytes(
        Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
}

// 길이 붙은 UTF-8 문자열을 읽어 out에 둔다. 형식 위반은 false(reader 실패로 표시), 할당 실패는 그
// 오류다.
Result<bool> ReadText(ByteReader& reader, String& out) noexcept {
    const uint32 bytes = reader.ReadU32LE();
    const Span<const uint8> raw = reader.Take(bytes);
    if (reader.Failed()) {
        return Result<bool>::Success(false);
    }

    const StringView text(reinterpret_cast<const char*>(raw.Data()), raw.Size());
    if (!text.IsValidUtf8()) {
        reader.MarkFailed();
        return Result<bool>::Success(false);
    }

    if (auto assigned = out.TryAssign(text); !assigned) {
        return Result<bool>::Failure(assigned.Error());
    }

    return Result<bool>::Success(true);
}

bool IsKnownDependencyKind(uint32 kind) noexcept {
    return kind >= static_cast<uint32>(PackageDependencyKind::Hard) &&
           kind <= static_cast<uint32>(PackageDependencyKind::ImportOwner);
}

// 파일 자신의 진단이다(같은 id·경로 진단은 게시가 다시 정하므로 캐시를 막지 않는다).
bool IsFileDiagnostic(const AssetIndexDiagnostic& diagnostic) noexcept {
    return diagnostic.code != AssetDiagnosticCode::DuplicateAssetId &&
           diagnostic.code != AssetDiagnosticCode::DuplicatePath;
}

// 항목 하나를 읽는다(의미 검사 포함). 형식 위반은 false다.
Result<bool> ReadEntry(ByteReader& reader, AssetIndexCacheEntry& entry) noexcept {
    auto named = ReadText(reader, entry.fileName);
    if (!named || !named.Value()) {
        return named;
    }

    if (!ReadDigest(reader, entry.fingerprint.digest)) {
        return Result<bool>::Success(false);
    }

    entry.id = AssetId{ReadGuid(reader)};
    entry.type = ReadGuid(reader);
    const uint8 readOnly = reader.ReadU8();
    const uint32 dependencies = reader.ReadU32LE();
    if (reader.Failed() || readOnly > 1 || entry.fileName.IsEmpty() ||
        !entry.fileName.View().EndsWith(EditorPackageExtension) ||
        dependencies > reader.Remaining() / DependencyBytes) {
        return Result<bool>::Success(false);
    }

    entry.readOnly = readOnly == 1;
    if (auto reserved = entry.dependencies.TryReserve(dependencies); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    for (uint32 index = 0; index < dependencies; ++index) {
        PackageDependencyEntry dependency;
        dependency.asset = AssetId{ReadGuid(reader)};
        const uint32 kind = reader.ReadU32LE();
        dependency.expectedType = ReadGuid(reader);
        const uint8 required = reader.ReadU8();
        // 개수는 위에서 남은 byte로 묶었으므로 읽기는 실패하지 않는다.
        if (!IsKnownDependencyKind(kind) || required > 1) {
            return Result<bool>::Success(false);
        }

        dependency.kind = static_cast<PackageDependencyKind>(kind);
        dependency.required = required == 1;
        (void)entry.dependencies.TryPushBack(dependency);
    }

    const uint32 tags = reader.ReadU32LE();
    if (reader.Failed() || tags > reader.Remaining() / MinTagBytes) {
        return Result<bool>::Success(false);
    }

    if (auto reserved = entry.tags.TryReserve(tags); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    for (uint32 index = 0; index < tags; ++index) {
        PackageTag tag(entry.tags.GetAllocator());
        auto key = ReadText(reader, tag.key);
        if (!key || !key.Value()) {
            return key;
        }

        auto value = ReadText(reader, tag.value);
        if (!value || !value.Value()) {
            return value;
        }

        // Tags 규칙: 키는 비지 않고 (키, 값) 순으로 엄격 증가(같은 쌍 없음 — PackageTags가 쓰는
        // 순서).
        if (tag.key.IsEmpty()) {
            return Result<bool>::Success(false);
        }

        if (!entry.tags.IsEmpty()) {
            const PackageTag& previous = entry.tags[entry.tags.Size() - 1];
            int compared = StringView::CompareBytes(previous.key.View(), tag.key.View());
            if (compared == 0) {
                compared = StringView::CompareBytes(previous.value.View(), tag.value.View());
            }

            if (compared >= 0) {
                return Result<bool>::Success(false);
            }
        }

        (void)entry.tags.TryPushBack(Move(tag));
    }

    const uint8 present = reader.ReadU8();
    if (reader.Failed() || present > 1) {
        return Result<bool>::Success(false);
    }

    entry.importRecord.present = present == 1;
    if (entry.importRecord.present) {
        entry.importRecord.owner = AssetId{ReadGuid(reader)};
        entry.importRecord.importerId = ReadGuid(reader);
        auto name = ReadText(reader, entry.importRecord.importerName);
        if (!name || !name.Value()) {
            return name;
        }

        entry.importRecord.importerVersion = reader.ReadU32LE();
        entry.importRecord.outputCount = static_cast<usize>(reader.ReadU64LE());
        if (reader.Failed()) {
            return Result<bool>::Success(false);
        }
    }

    return Result<bool>::Success(true);
}

} // namespace

AssetIndexCacheKey MakeAssetIndexCacheKey(const PackageReadOptions& options) noexcept {
    AssetIndexCacheKey key;
    const StringView unicode = UnicodeVersionText();
    key.unicode = Sha256::Hash(
        Span<const uint8>(reinterpret_cast<const uint8*>(unicode.Data()), unicode.ByteCount()));
    const EngineVersion compatible = options.engineVersion.CompatibleBaseline();
    key.compatibleMajor = compatible.majorVersion;
    key.compatibleMinor = compatible.minorVersion;
    Sha256 customs;
    for (const PackageCustomVersion& custom : options.customVersions) {
        uint8 bytes[GuidBytes + 4];
        StoreLittleEndian<uint64>(bytes, custom.id.high);
        StoreLittleEndian<uint64>(bytes + 8, custom.id.low);
        StoreLittleEndian<uint32>(bytes + GuidBytes, custom.version);
        customs.Update(Span<const uint8>(bytes, sizeof(bytes)));
    }

    key.customVersions = customs.Finish();
    return key;
}

AssetIndexCache::AssetIndexCache(Allocator& allocator) noexcept
    : allocator_(&allocator), entries_(allocator) {
}

Result<bool> AssetIndexCache::Parse(Span<const uint8> bytes,
                                    const AssetIndexCacheKey& key) noexcept {
    entries_.Clear();
    toolBuildId_ = Sha256Digest{};
    auto empty = [&]() noexcept {
        entries_.Clear();
        return Result<bool>::Success(false);
    };
    if (bytes.Size() < HeaderBytes || bytes.Size() > MaxAssetIndexCacheBytes) {
        return empty();
    }

    ByteReader reader(bytes);
    AssetIndexCacheKey stored;
    const uint32 magic = reader.ReadU32LE();
    stored.formatVersion = reader.ReadU32LE();
    stored.schemaVersion = reader.ReadU32LE();
    stored.semanticVersion = reader.ReadU32LE();
    (void)ReadDigest(reader, stored.unicode);
    stored.compatibleMajor = reader.ReadU16LE();
    stored.compatibleMinor = reader.ReadU16LE();
    const uint8 normalizes = reader.ReadU8();
    stored.normalizesNames = normalizes == 1;
    bool reservedZero = true;
    for (usize index = 0; index < ReservedBytes; ++index) {
        reservedZero = reservedZero && reader.ReadU8() == 0;
    }

    (void)ReadDigest(reader, stored.customVersions);
    (void)ReadDigest(reader, toolBuildId_);
    const uint32 count = reader.ReadU32LE();
    const uint64 bodyBytes = reader.ReadU64LE();
    Sha256Digest bodyDigest;
    (void)ReadDigest(reader, bodyDigest);
    // 헤더 칸은 위 크기 검사로 모두 읽힌다(reader가 실패하지 않는다).
    if (magic != CacheMagic || normalizes > 1 || !reservedZero || !(stored == key) ||
        bodyBytes != bytes.Size() - HeaderBytes || count > bodyBytes / MinEntryBytes) {
        return empty();
    }

    const Span<const uint8> body(bytes.Data() + HeaderBytes, bytes.Size() - HeaderBytes);
    if (!(Sha256::Hash(body) == bodyDigest)) {
        return empty();
    }

    if (auto reserved = entries_.TryReserve(count); !reserved) {
        entries_.Clear();
        return Result<bool>::Failure(reserved.Error());
    }

    ByteReader items(body);
    for (uint32 index = 0; index < count; ++index) {
        AssetIndexCacheEntry entry(*allocator_);
        auto read = ReadEntry(items, entry);
        if (!read) {
            entries_.Clear();
            return Result<bool>::Failure(read.Error());
        }

        // 이름은 byte 순으로 엄격 증가한다(같은 이름 둘 없음 — Find 이진 검색).
        if (!read.Value() ||
            (!entries_.IsEmpty() &&
             StringView::CompareBytes(entries_[entries_.Size() - 1].fileName.View(),
                                      entry.fileName.View()) >= 0)) {
            return empty();
        }

        (void)entries_.TryPushBack(Move(entry));
    }

    if (items.Remaining() != 0) {
        return empty();
    }

    return Result<bool>::Success(true);
}

Result<bool> AssetIndexCache::Load(platform::FileSystem& files, const ProjectLayout& layout,
                                   const PackageReadOptions& options) noexcept {
    entries_.Clear();
    String path(*allocator_);
    const StringView pieces[] = {layout.IntermediateDirectory(), "/", AssetIndexCacheFileName};
    for (StringView piece : pieces) {
        if (auto appended = path.TryAppend(piece); !appended) {
            return Result<bool>::Failure(appended.Error());
        }
    }

    platform::File file;
    if (auto opened = file.Open(files, path.View().Data(), path.View().ByteCount(),
                                platform::FileOpenMode::ReadExisting);
        !opened) {
        return Result<bool>::Success(false);
    }

    uint64 size = 0;
    if (auto sized = file.Size(size); !sized || size > MaxAssetIndexCacheBytes) {
        (void)file.Close();
        return Result<bool>::Success(false);
    }

    Array<uint8> bytes(*allocator_);
    if (auto resized = bytes.TryResize(static_cast<usize>(size)); !resized) {
        (void)file.Close();
        return Result<bool>::Failure(resized.Error());
    }

    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    (void)file.Close();
    if (!read.status) {
        return Result<bool>::Success(false);
    }

    return Parse(bytes.View(), MakeAssetIndexCacheKey(options));
}

const AssetIndexCacheEntry*
AssetIndexCache::Find(StringView fileName, const PackageFingerprint& fingerprint) const noexcept {
    usize low = 0;
    usize high = entries_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int compared = StringView::CompareBytes(entries_[middle].fileName.View(), fileName);
        if (compared == 0) {
            return entries_[middle].fingerprint == fingerprint ? &entries_[middle] : nullptr;
        }

        if (compared < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

Result<void> EncodeAssetIndexCache(const AssetIndex& index, const AssetIndexCacheKey& key,
                                   Array<uint8>& out) noexcept {
    Allocator& allocator = out.GetAllocator();
    // 파일 자신의 진단이 있는 이름(정렬)이다 — 그 파일은 넣지 않는다(다음 시작에 다시 읽는다).
    Array<StringView> excluded(allocator);
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        if (IsFileDiagnostic(diagnostic)) {
            if (auto pushed = excluded.TryPushBack(diagnostic.fileName.View()); !pushed) {
                return pushed;
            }
        }
    }

    algo::Sort(excluded.View(), [](StringView left, StringView right) noexcept {
        return StringView::CompareBytes(left, right) < 0;
    });
    auto isExcluded = [&](StringView name) noexcept {
        usize low = 0;
        usize high = excluded.Size();
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            const int compared = StringView::CompareBytes(excluded[middle], name);
            if (compared == 0) {
                return true;
            }

            if (compared < 0) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        return false;
    };

    Array<const AssetIndexEntry*> entries(allocator);
    if (auto reserved = entries.TryReserve(index.Count()); !reserved) {
        return reserved;
    }

    for (usize position = 0; position < index.Count(); ++position) {
        if (!isExcluded(index.At(position).fileName.View())) {
            (void)entries.TryPushBack(&index.At(position));
        }
    }

    algo::Sort(
        entries.View(), [](const AssetIndexEntry* left, const AssetIndexEntry* right) noexcept {
            return StringView::CompareBytes(left->fileName.View(), right->fileName.View()) < 0;
        });

    const usize start = out.Size();
    ByteWriter writer(out);
    writer.WriteU32LE(CacheMagic);
    writer.WriteU32LE(key.formatVersion);
    writer.WriteU32LE(key.schemaVersion);
    writer.WriteU32LE(key.semanticVersion);
    WriteDigest(writer, key.unicode);
    writer.WriteU16LE(key.compatibleMajor);
    writer.WriteU16LE(key.compatibleMinor);
    writer.WriteU8(key.normalizesNames ? 1 : 0);
    writer.WriteZeros(ReservedBytes);
    WriteDigest(writer, key.customVersions);
    WriteDigest(writer, build::ToolBuildId());
    writer.WriteU32LE(static_cast<uint32>(entries.Size()));
    const usize bodyBytesAt = out.Size();
    writer.WriteU64LE(0);
    const usize digestAt = out.Size();
    writer.WriteZeros(DigestBytes);
    const usize bodyStart = out.Size();
    for (const AssetIndexEntry* entry : entries) {
        WriteText(writer, entry->fileName.View());
        WriteDigest(writer, entry->fingerprint.digest);
        WriteGuid(writer, entry->id.value);
        WriteGuid(writer, entry->type);
        writer.WriteU8(entry->readOnly ? 1 : 0);
        writer.WriteU32LE(static_cast<uint32>(entry->dependencies.Size()));
        for (const PackageDependencyEntry& dependency : entry->dependencies) {
            WriteGuid(writer, dependency.asset.value);
            writer.WriteU32LE(static_cast<uint32>(dependency.kind));
            WriteGuid(writer, dependency.expectedType);
            writer.WriteU8(dependency.required ? 1 : 0);
        }

        writer.WriteU32LE(static_cast<uint32>(entry->tags.Size()));
        for (const PackageTag& tag : entry->tags) {
            WriteText(writer, tag.key.View());
            WriteText(writer, tag.value.View());
        }

        writer.WriteU8(entry->importRecord.present ? 1 : 0);
        if (entry->importRecord.present) {
            WriteGuid(writer, entry->importRecord.owner.value);
            WriteGuid(writer, entry->importRecord.importerId);
            WriteText(writer, entry->importRecord.importerName.View());
            writer.WriteU32LE(entry->importRecord.importerVersion);
            writer.WriteU64LE(static_cast<uint64>(entry->importRecord.outputCount));
        }
    }

    if (auto status = writer.Status(); !status) {
        out.TryResize(start).IsOk();
        return status;
    }

    const Span<const uint8> body(out.Data() + bodyStart, out.Size() - bodyStart);
    const Sha256Digest digest = Sha256::Hash(body);
    writer.PatchLittleEndian<uint64>(bodyBytesAt, static_cast<uint64>(body.Size()));
    for (usize index = 0; index < DigestBytes; ++index) {
        out[digestAt + index] = digest.bytes[index];
    }

    return Result<void>::Success();
}

Result<void> WriteAssetIndexCache(platform::FileSystem& files, const ProjectLayout& layout,
                                  const AssetDatabaseLock& lock, const AssetIndex& index,
                                  const PackageReadOptions& options) noexcept {
    if (!lock.IsHeldFor(layout)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = layout.GetAllocator();
    Array<uint8> bytes(allocator);
    if (auto encoded = EncodeAssetIndexCache(index, MakeAssetIndexCacheKey(options), bytes);
        !encoded) {
        return encoded;
    }

    const StringView directory = layout.IntermediateDirectory();
    if (auto made = platform::MakeDirectoryTree(files, directory.Data(), directory.ByteCount());
        !made) {
        return Result<void>::Failure(FileErrorCode(made));
    }

    String target(allocator);
    String temp(allocator);
    const StringView targetPieces[] = {directory, "/", AssetIndexCacheFileName};
    for (StringView piece : targetPieces) {
        if (auto appended = target.TryAppend(piece); !appended) {
            return appended;
        }
    }

    auto guid = TryNewGuid();
    if (!guid) {
        return Result<void>::Failure(guid.Error());
    }

    if (auto assigned = temp.TryAssign(target.View()); !assigned) {
        return assigned;
    }

    Result<void> step = temp.TryAppend(".");
    if (step) {
        step = EditorAppendGuidText(temp, guid.Value());
    }

    if (step) {
        step = temp.TryAppend(EditorTempSuffix);
    }

    if (!step) {
        return step;
    }

    auto discard = [&]() noexcept {
        (void)files.Delete(temp.View().Data(), temp.View().ByteCount());
    };
    {
        platform::File file;
        if (auto opened = file.Open(files, temp.View().Data(), temp.View().ByteCount(),
                                    platform::FileOpenMode::CreateNew);
            !opened) {
            return Result<void>::Failure(FileErrorCode(opened));
        }

        const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
        if (!written.status) {
            (void)file.Close();
            discard();
            return Result<void>::Failure(FileErrorCode(written.status));
        }

        if (auto flushed = file.Flush(); !flushed) {
            (void)file.Close();
            discard();
            return Result<void>::Failure(FileErrorCode(flushed));
        }

        if (auto closed = file.Close(); !closed) {
            discard();
            return Result<void>::Failure(FileErrorCode(closed));
        }
    }

    const platform::FileStatus replaced =
        files.Replace(temp.View().Data(), temp.View().ByteCount(), target.View().Data(),
                      target.View().ByteCount());
    if (!replaced && replaced.code != platform::FileCode::NotDurable) {
        discard();
        return Result<void>::Failure(FileErrorCode(replaced));
    }

    return Result<void>::Success();
}

} // namespace rocks
