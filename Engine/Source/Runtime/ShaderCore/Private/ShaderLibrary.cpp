#include <ShaderCore/ShaderLibrary.h>

#include "ShaderCoreInternal.h"

#include <Base/Endian.h>
#include <Base/NumericLimits.h>
#include <Core/Algorithms.h>
#include <Core/Encoding/ByteWriter.h>
#include <string.h>

// 셰이더 라이브러리 v1(Docs/Design/ShaderSystem 12.2.1절 SSD7·SSD8)이다. byte 형식과 규칙은 공개
// 헤더 주석을 따른다.
namespace rocks::shader {

namespace library_detail {

// 머리 칸 오프셋이다(공개 헤더 표와 같다).
constexpr usize VersionAt = 4;
constexpr usize KindAt = 8;
constexpr usize KindPadAt = 9;
constexpr usize FormatIdAt = 16;
constexpr usize FormatShaAt = 48;
constexpr usize TypeSetShaAt = 80;
constexpr usize BackendAt = 112;
constexpr usize ShaderModelMajorAt = 113;
constexpr usize ShaderModelMinorAt = 114;
constexpr usize FactsPadAt = 115;
constexpr usize FeaturesAt = 116;
constexpr usize DeviceRequirementsAt = 120;
constexpr usize LimitsAt = 124;
constexpr usize LimitCount = 13;
constexpr usize EntryCountAt = 176;
constexpr usize CodeCountAt = 180;
constexpr usize NameCountAt = 184;
constexpr usize CountPadAt = 188;
constexpr usize EntryOffsetAt = 192;
constexpr usize CodeOffsetAt = 200;
constexpr usize NameOffsetAt = 208;
constexpr usize BlobOffsetAt = 216;
constexpr usize BlobBytesAt = 224;
constexpr usize FileShaAt = 232;

static_assert(LimitsAt + LimitCount * sizeof(uint32) == EntryCountAt);
static_assert(FileShaAt + Sha256Digest::Bytes == ShaderLibraryHeaderBytes);

// 항목 칸 오프셋이다.
constexpr usize EntryBundleAt = 0;
constexpr usize EntryNameAt = 8;
constexpr usize EntryVertexInputAt = 16;
constexpr usize EntryPermutationAt = 24;
constexpr usize EntryStageAt = 28;
constexpr usize EntryDevelopmentAt = 29;
constexpr usize EntryPadAt = 30;
constexpr usize EntryFeaturesAt = 32;
constexpr usize EntryCodeAt = 36;
constexpr usize EntrySignatureAt = 40;

static_assert(EntrySignatureAt + Sha256Digest::Bytes == ShaderLibraryEntryStride);

// 코드 칸 오프셋이다.
constexpr usize CodeShaAt = 0;
constexpr usize CodeOffsetFieldAt = 32;
constexpr usize CodeSizeAt = 40;

static_assert(CodeSizeAt + sizeof(uint64) == ShaderLibraryCodeStride);

// 이름 칸 오프셋이다.
constexpr usize NameHashAt = 0;
constexpr usize NameLengthAt = 8;
constexpr usize NamePadAt = 9;
constexpr usize NameBytesAt = 16;

static_assert(NameBytesAt + MaxShaderTypeNameBytes == ShaderLibraryNameStride);

// 코드 틀: u32 값 수, 값마다 u32 id·u32 byte 수.
constexpr usize CodeCountBytes = sizeof(uint32);
constexpr usize CodeValueHeaderBytes = 2 * sizeof(uint32);

constexpr uint8 LastBackend = static_cast<uint8>(ShaderBackend::OpenGL);
constexpr uint8 LastKind = static_cast<uint8>(ShaderLibraryKind::Project);

constexpr uint64 AlignUp(uint64 value) noexcept {
    return (value + ShaderLibraryAlignment - 1) & ~static_cast<uint64>(ShaderLibraryAlignment - 1);
}

bool DigestLess(const Sha256Digest& a, const Sha256Digest& b) noexcept {
    return memcmp(a.bytes, b.bytes, Sha256Digest::Bytes) < 0;
}

Sha256Digest LoadDigest(const uint8* at) noexcept {
    Sha256Digest digest;
    memcpy(digest.bytes, at, Sha256Digest::Bytes);
    return digest;
}

bool IsZero(const uint8* at, usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        if (at[index] != 0) {
            return false;
        }
    }

    return true;
}

ShaderLibraryKey LoadKey(const uint8* entry) noexcept {
    ShaderLibraryKey key;
    key.bundleId = LoadLittleEndian<uint64>(entry + EntryBundleAt);
    key.nameHash = LoadLittleEndian<uint64>(entry + EntryNameAt);
    key.vertexInputType = LoadLittleEndian<uint64>(entry + EntryVertexInputAt);
    key.permutationId = LoadLittleEndian<uint32>(entry + EntryPermutationAt);
    return key;
}

Result<void> Reject(const char** reason, const char* why,
                    ErrorCode code = ErrorCode::InvalidArgument) noexcept {
    if (reason != nullptr) {
        *reason = why;
    }

    return Result<void>::Failure(code);
}

// 항목 값의 코드 틀 byte 수다.
uint64 FramedBytes(const ShaderLibraryBuildEntry& entry) noexcept {
    uint64 bytes = CodeCountBytes;
    for (const ShaderLibraryValue& value : entry.values) {
        bytes += CodeValueHeaderBytes + value.bytes.Size();
    }

    return bytes;
}

void HashU32(Sha256& hasher, uint32 value) noexcept {
    uint8 bytes[sizeof(uint32)];
    StoreLittleEndian<uint32>(bytes, value);
    hasher.Update(Span<const uint8>(bytes, sizeof(bytes)));
}

// 코드 틀 byte의 SHA-256이다(틀을 만들지 않고 계산한다).
Sha256Digest FramedSha(const ShaderLibraryBuildEntry& entry) noexcept {
    Sha256 hasher;
    HashU32(hasher, static_cast<uint32>(entry.values.Size()));
    for (const ShaderLibraryValue& value : entry.values) {
        HashU32(hasher, value.id);
        HashU32(hasher, static_cast<uint32>(value.bytes.Size()));
        hasher.Update(value.bytes);
    }

    return hasher.Finish();
}

const char* CheckBuildEntry(const ShaderLibraryBuildEntry& entry) noexcept {
    if (!core_detail::IsShaderIdentifier(entry.typeName, MaxShaderTypeNameBytes) ||
        entry.key.nameHash != ShaderTypeNameHash(entry.typeName)) {
        return "shader library entry type name must be a shader type name whose hash is the key "
               "name hash";
    }

    if (!core_detail::IsSingleShaderStage(entry.stage)) {
        return "shader library entry stage must be exactly one stage bit";
    }

    if ((entry.runtimeFeatures & ~RuntimeCheckedShaderFeatures) != 0) {
        return "shader library entry runtime features must be runtime-checked features only";
    }

    if (entry.values.IsEmpty() || entry.values.Size() > MaxShaderLibraryValues ||
        entry.values[0].id != ShaderLibraryValueCode) {
        return "shader library entry needs value 0 (Code) and at most values 0, 1 and 2";
    }

    for (usize index = 0; index < entry.values.Size(); ++index) {
        const ShaderLibraryValue& value = entry.values[index];
        if (value.id > ShaderLibraryValueGlesCopyTable ||
            (index > 0 && entry.values[index - 1].id >= value.id)) {
            return "shader library entry values must be ascending ids among 0, 1 and 2";
        }

        if (value.bytes.Size() > NumericLimits<uint32>::Max()) {
            return "shader library value is larger than 4 GiB";
        }
    }

    return nullptr;
}

struct CodeRef {
    Sha256Digest sha;
    // 이 코드를 담은 첫 입력 항목이다.
    uint32 entry = 0;
    uint64 bytes = 0;
    uint64 offset = 0;
};

struct NameRef {
    uint64 hash = 0;
    StringView name;
};

// view가 연 byte의 칸 읽기다.
struct Cursor {
    const uint8* data;

    uint8 U8(usize at) const noexcept {
        return data[at];
    }

    uint32 U32(usize at) const noexcept {
        return LoadLittleEndian<uint32>(data + at);
    }

    uint64 U64(usize at) const noexcept {
        return LoadLittleEndian<uint64>(data + at);
    }
};

// 코드 틀을 검사한다(길이 정확·값 오름차순·0 있음·0·1·2만).
bool IsValidFrame(Span<const uint8> code) noexcept {
    if (code.Size() < CodeCountBytes) {
        return false;
    }

    const uint32 count = LoadLittleEndian<uint32>(code.Data());
    if (count == 0 || count > MaxShaderLibraryValues) {
        return false;
    }

    uint64 at = CodeCountBytes;
    int64 previous = -1;
    for (uint32 index = 0; index < count; ++index) {
        if (code.Size() - at < CodeValueHeaderBytes) {
            return false;
        }

        const uint32 id = LoadLittleEndian<uint32>(code.Data() + at);
        const uint32 bytes = LoadLittleEndian<uint32>(code.Data() + at + sizeof(uint32));
        if ((index == 0 && id != ShaderLibraryValueCode) || id > ShaderLibraryValueGlesCopyTable ||
            static_cast<int64>(id) <= previous) {
            return false;
        }

        at += CodeValueHeaderBytes;
        if (code.Size() - at < bytes) {
            return false;
        }

        at += bytes;
        previous = id;
    }

    return at == code.Size();
}

} // namespace library_detail

const char* ShaderLibraryKindName(ShaderLibraryKind kind) noexcept {
    switch (kind) {
    case ShaderLibraryKind::Engine:
        return "Engine";
    case ShaderLibraryKind::Project:
        return "Project";
    }

    return "unknown";
}

Result<void> BuildShaderLibrary(const ShaderLibraryBuildDesc& desc, Allocator& allocator,
                                Array<uint8>& out, const char** reason) noexcept {
    using namespace library_detail;
    out.Clear();
    if (static_cast<uint8>(desc.kind) > LastKind ||
        static_cast<uint8>(desc.facts.backend) > LastBackend ||
        !core_detail::IsShaderFormatId(desc.facts.FormatId())) {
        return Reject(reason, "shader library kind, backend or format id is invalid");
    }

    const Span<const ShaderLibraryBuildEntry> entries = desc.entries;
    if (entries.Size() > MaxShaderLibraryEntries) {
        return Reject(reason, "shader library has too many entries", ErrorCode::CapacityExceeded);
    }

    const uint32 entryCount = static_cast<uint32>(entries.Size());
    Array<uint32> order(allocator);
    Array<Sha256Digest> entryShas(allocator);
    Array<CodeRef> codes(allocator);
    Array<NameRef> names(allocator);
    if (!order.TryReserve(entryCount) || !entryShas.TryReserve(entryCount) ||
        !codes.TryReserve(entryCount) || !names.TryReserve(entryCount)) {
        return Reject(reason, "out of memory", ErrorCode::OutOfMemory);
    }

    for (uint32 index = 0; index < entryCount; ++index) {
        const ShaderLibraryBuildEntry& entry = entries[index];
        if (const char* broken = CheckBuildEntry(entry); broken != nullptr) {
            return Reject(reason, broken);
        }

        const Sha256Digest sha = FramedSha(entry);
        (void)order.TryPushBack(index);
        (void)entryShas.TryPushBack(sha);
        (void)codes.TryPushBack(CodeRef{sha, index, FramedBytes(entry), 0});
        (void)names.TryPushBack(NameRef{entry.key.nameHash, entry.typeName});
    }

    // 항목은 키 순, 코드는 SHA 순(같은 코드는 하나), 이름은 해시 순(같은 해시는 같은 이름)이다.
    algo::Sort(order.View(), [&](uint32 a, uint32 b) noexcept {
        return ShaderLibraryKeyLess(entries[a].key, entries[b].key);
    });
    for (usize index = 1; index < order.Size(); ++index) {
        if (entries[order[index - 1]].key == entries[order[index]].key) {
            return Reject(reason, "shader library has two entries with the same key");
        }
    }

    algo::Sort(codes.View(), [](const CodeRef& a, const CodeRef& b) noexcept {
        return DigestLess(a.sha, b.sha) || (a.sha == b.sha && a.entry < b.entry);
    });
    usize uniqueCodes = 0;
    for (usize index = 0; index < codes.Size(); ++index) {
        if (uniqueCodes == 0 || !(codes[uniqueCodes - 1].sha == codes[index].sha)) {
            codes[uniqueCodes++] = codes[index];
        }
    }

    (void)codes.TryResize(uniqueCodes);
    algo::Sort(names.View(),
               [](const NameRef& a, const NameRef& b) noexcept { return a.hash < b.hash; });
    usize uniqueNames = 0;
    for (usize index = 0; index < names.Size(); ++index) {
        if (uniqueNames > 0 && names[uniqueNames - 1].hash == names[index].hash) {
            if (names[uniqueNames - 1].name != names[index].name) {
                return Reject(reason, "two shader type names in the library share a name hash");
            }

            continue;
        }

        names[uniqueNames++] = names[index];
    }

    (void)names.TryResize(uniqueNames);

    // 섹션 배치다.
    const uint64 entryOffset = ShaderLibraryHeaderBytes;
    const uint64 codeOffset =
        entryOffset + static_cast<uint64>(entryCount) * ShaderLibraryEntryStride;
    const uint64 nameOffset =
        codeOffset + static_cast<uint64>(codes.Size()) * ShaderLibraryCodeStride;
    const uint64 blobOffset =
        nameOffset + static_cast<uint64>(names.Size()) * ShaderLibraryNameStride;
    uint64 blobBytes = 0;
    for (CodeRef& code : codes) {
        code.offset = blobBytes;
        blobBytes += AlignUp(code.bytes);
    }

    const uint64 totalBytes = blobOffset + blobBytes;
    if (totalBytes > NumericLimits<usize>::Max() ||
        !out.TryReserve(static_cast<usize>(totalBytes))) {
        return Reject(reason, "out of memory", ErrorCode::OutOfMemory);
    }

    ByteWriter writer(out);
    writer.WriteBytes(Span<const uint8>(reinterpret_cast<const uint8*>(ShaderLibraryMagic),
                                        sizeof(ShaderLibraryMagic)));
    writer.WriteU32LE(ShaderLibraryVersion);
    writer.WriteU8(static_cast<uint8>(desc.kind));
    writer.WriteZeros(FormatIdAt - KindPadAt);
    // 형식 id 칸은 NUL 뒤까지 0이다(SetShaderFormatFactsId 계약과 무관하게 정규화한다).
    const StringView formatId = desc.facts.FormatId();
    writer.WriteBytes(
        Span<const uint8>(reinterpret_cast<const uint8*>(formatId.Data()), formatId.ByteCount()));
    writer.WriteZeros(MaxShaderFormatIdBytes - formatId.ByteCount());
    writer.WriteBytes(Span<const uint8>(desc.formatDefinitionSha256.bytes));
    writer.WriteBytes(Span<const uint8>(desc.typeSetSha256.bytes));
    writer.WriteU8(static_cast<uint8>(desc.facts.backend));
    writer.WriteU8(desc.facts.shaderModel.major);
    writer.WriteU8(desc.facts.shaderModel.minor);
    writer.WriteU8(0);
    writer.WriteU32LE(desc.facts.features);
    writer.WriteU32LE(desc.facts.deviceRequirements);
    const ShaderFormatLimits& limits = desc.facts.limits;
    const uint32 limitValues[LimitCount] = {
        limits.constantBuffers,   limits.sampledTextures,   limits.storageBuffers,
        limits.storageImages,     limits.samplers,          limits.uavTotal,
        limits.uavStages,         limits.renderTargets,     limits.interStageVectors,
        limits.rootConstantBytes, limits.uniformVectors[0], limits.uniformVectors[1],
        limits.uniformVectors[2]};
    for (const uint32 value : limitValues) {
        writer.WriteU32LE(value);
    }

    writer.WriteU32LE(entryCount);
    writer.WriteU32LE(static_cast<uint32>(codes.Size()));
    writer.WriteU32LE(static_cast<uint32>(names.Size()));
    writer.WriteU32LE(0);
    writer.WriteU64LE(entryOffset);
    writer.WriteU64LE(codeOffset);
    writer.WriteU64LE(nameOffset);
    writer.WriteU64LE(blobOffset);
    writer.WriteU64LE(blobBytes);
    // 파일 SHA-256 자리(0으로 두고 마지막에 채운다)다.
    writer.WriteZeros(Sha256Digest::Bytes);

    const Span<const CodeRef> codeView = codes.View();
    for (const uint32 index : order) {
        const ShaderLibraryBuildEntry& entry = entries[index];
        const usize code = algo::LowerBound(
            codeView, entryShas[index],
            [](const CodeRef& a, const Sha256Digest& b) noexcept { return DigestLess(a.sha, b); });
        writer.WriteU64LE(entry.key.bundleId);
        writer.WriteU64LE(entry.key.nameHash);
        writer.WriteU64LE(entry.key.vertexInputType);
        writer.WriteU32LE(entry.key.permutationId);
        writer.WriteU8(static_cast<uint8>(entry.stage));
        writer.WriteU8(entry.developmentOnly ? 1 : 0);
        writer.WriteZeros(EntryFeaturesAt - EntryPadAt);
        writer.WriteU32LE(entry.runtimeFeatures);
        writer.WriteU32LE(static_cast<uint32>(code));
        writer.WriteBytes(Span<const uint8>(entry.layoutSignature.bytes));
    }

    for (const CodeRef& code : codes) {
        writer.WriteBytes(Span<const uint8>(code.sha.bytes));
        writer.WriteU64LE(code.offset);
        writer.WriteU64LE(code.bytes);
    }

    for (const NameRef& name : names) {
        writer.WriteU64LE(name.hash);
        writer.WriteU8(static_cast<uint8>(name.name.ByteCount()));
        writer.WriteZeros(NameBytesAt - NamePadAt);
        writer.WriteBytes(Span<const uint8>(reinterpret_cast<const uint8*>(name.name.Data()),
                                            name.name.ByteCount()));
        writer.WriteZeros(MaxShaderTypeNameBytes - name.name.ByteCount());
    }

    for (const CodeRef& code : codes) {
        const ShaderLibraryBuildEntry& entry = entries[code.entry];
        writer.WriteU32LE(static_cast<uint32>(entry.values.Size()));
        for (const ShaderLibraryValue& value : entry.values) {
            writer.WriteU32LE(value.id);
            writer.WriteU32LE(static_cast<uint32>(value.bytes.Size()));
            writer.WriteBytes(value.bytes);
        }

        writer.WriteZeros(static_cast<usize>(AlignUp(code.bytes) - code.bytes));
    }

    if (auto written = writer.Status(); !written) {
        out.Clear();
        return Reject(reason, "out of memory", written.Error());
    }

    const Sha256Digest fileSha = Sha256::Hash(out.View());
    memcpy(out.Data() + FileShaAt, fileSha.bytes, Sha256Digest::Bytes);
    return Result<void>::Success();
}

void ShaderLibraryView::Close() noexcept {
    *this = ShaderLibraryView{};
}

ShaderReadStatus ShaderLibraryView::Open(Span<const uint8> bytes) noexcept {
    using namespace library_detail;
    Close();
    if (bytes.Size() < ShaderLibraryHeaderBytes) {
        return ShaderReadStatus::Corrupt("shader library is shorter than its header");
    }

    const Cursor file{bytes.Data()};
    if (memcmp(bytes.Data(), ShaderLibraryMagic, sizeof(ShaderLibraryMagic)) != 0) {
        return ShaderReadStatus::Corrupt("shader library magic is not RSHL");
    }

    if (file.U32(VersionAt) != ShaderLibraryVersion) {
        return ShaderReadStatus::Unsupported("shader library version is not 1");
    }

    // 파일 SHA-256(자기 칸은 0으로 본다)이다.
    {
        const uint8 zeros[Sha256Digest::Bytes] = {};
        Sha256 hasher;
        hasher.Update(bytes.Left(FileShaAt));
        hasher.Update(Span<const uint8>(zeros));
        hasher.Update(bytes.RightChop(ShaderLibraryHeaderBytes));
        if (!(hasher.Finish() == LoadDigest(bytes.Data() + FileShaAt))) {
            return ShaderReadStatus::Corrupt("shader library file SHA-256 does not match");
        }
    }

    if (file.U8(KindAt) > LastKind || !IsZero(bytes.Data() + KindPadAt, FormatIdAt - KindPadAt) ||
        file.U8(BackendAt) > LastBackend || file.U8(FactsPadAt) != 0 || file.U32(CountPadAt) != 0) {
        return ShaderReadStatus::Corrupt("shader library header has an invalid kind, backend or "
                                         "non-zero padding");
    }

    const uint8* idField = bytes.Data() + FormatIdAt;
    const void* terminator = memchr(idField, 0, MaxShaderFormatIdBytes);
    if (terminator == nullptr) {
        return ShaderReadStatus::Corrupt("shader library format id is not NUL terminated");
    }

    const usize idBytes = static_cast<usize>(static_cast<const uint8*>(terminator) - idField);
    const StringView id(reinterpret_cast<const char*>(idField), idBytes);
    if (!core_detail::IsShaderFormatId(id) ||
        !IsZero(idField + idBytes, MaxShaderFormatIdBytes - idBytes)) {
        return ShaderReadStatus::Corrupt("shader library format id is invalid");
    }

    const uint32 entryCount = file.U32(EntryCountAt);
    const uint32 codeCount = file.U32(CodeCountAt);
    const uint32 nameCount = file.U32(NameCountAt);
    if (entryCount > MaxShaderLibraryEntries || codeCount > entryCount || nameCount > entryCount ||
        (entryCount > 0 && (codeCount == 0 || nameCount == 0))) {
        return ShaderReadStatus::Corrupt("shader library table counts are out of range");
    }

    const uint64 entryOffset = ShaderLibraryHeaderBytes;
    const uint64 codeOffset =
        entryOffset + static_cast<uint64>(entryCount) * ShaderLibraryEntryStride;
    const uint64 nameOffset = codeOffset + static_cast<uint64>(codeCount) * ShaderLibraryCodeStride;
    const uint64 blobOffset = nameOffset + static_cast<uint64>(nameCount) * ShaderLibraryNameStride;
    const uint64 blobBytes = file.U64(BlobBytesAt);
    if (file.U64(EntryOffsetAt) != entryOffset || file.U64(CodeOffsetAt) != codeOffset ||
        file.U64(NameOffsetAt) != nameOffset || file.U64(BlobOffsetAt) != blobOffset ||
        blobOffset > bytes.Size() || blobBytes != bytes.Size() - blobOffset) {
        return ShaderReadStatus::Corrupt("shader library section offsets or file size do not "
                                         "match the tables");
    }

    // 코드 표와 코드 부분이다.
    const uint8* blob = bytes.Data() + blobOffset;
    uint64 expectedOffset = 0;
    for (uint32 index = 0; index < codeCount; ++index) {
        const uint8* row =
            bytes.Data() + codeOffset + static_cast<uint64>(index) * ShaderLibraryCodeStride;
        const uint64 offset = LoadLittleEndian<uint64>(row + CodeOffsetFieldAt);
        const uint64 size = LoadLittleEndian<uint64>(row + CodeSizeAt);
        if (offset != expectedOffset || size < CodeCountBytes || size > blobBytes - offset ||
            AlignUp(size) > blobBytes - offset) {
            return ShaderReadStatus::Corrupt("shader library code range is not canonical");
        }

        if (index > 0 && !DigestLess(LoadDigest(row - ShaderLibraryCodeStride + CodeShaAt),
                                     LoadDigest(row + CodeShaAt))) {
            return ShaderReadStatus::Corrupt("shader library code table is not in strict SHA-256 "
                                             "order");
        }

        const Span<const uint8> code(blob + offset, static_cast<usize>(size));
        if (!(Sha256::Hash(code) == LoadDigest(row + CodeShaAt)) || !IsValidFrame(code) ||
            !IsZero(blob + offset + size, static_cast<usize>(AlignUp(size) - size))) {
            return ShaderReadStatus::Corrupt("shader library code bytes do not match their SHA-256 "
                                             "or value framing");
        }

        expectedOffset = offset + AlignUp(size);
    }

    if (expectedOffset != blobBytes) {
        return ShaderReadStatus::Corrupt("shader library code section has bytes no code owns");
    }

    // 이름 표다(CP7 — 이름에서 해시를 다시 계산한다).
    for (uint32 index = 0; index < nameCount; ++index) {
        const uint8* row =
            bytes.Data() + nameOffset + static_cast<uint64>(index) * ShaderLibraryNameStride;
        const uint64 hash = LoadLittleEndian<uint64>(row + NameHashAt);
        const uint8 length = row[NameLengthAt];
        if (length > MaxShaderTypeNameBytes || !IsZero(row + NamePadAt, NameBytesAt - NamePadAt) ||
            !IsZero(row + NameBytesAt + length, MaxShaderTypeNameBytes - length)) {
            return ShaderReadStatus::Corrupt("shader library name row has an invalid length or "
                                             "non-zero padding");
        }

        const StringView name(reinterpret_cast<const char*>(row + NameBytesAt), length);
        if (!core_detail::IsShaderIdentifier(name, MaxShaderTypeNameBytes)) {
            return ShaderReadStatus::Corrupt("shader library type name breaks the shader type name "
                                             "rule");
        }

        if (ShaderTypeNameHash(name) != hash) {
            return ShaderReadStatus::Corrupt("shader library type name hash does not match the "
                                             "name");
        }

        if (index > 0 &&
            LoadLittleEndian<uint64>(row - ShaderLibraryNameStride + NameHashAt) >= hash) {
            return ShaderReadStatus::Corrupt("shader library name table is not in strict hash "
                                             "order");
        }
    }

    // 항목 표다. 이름 조회에 이름 표 칸을 먼저 채운다.
    bytes_ = bytes;
    nameOffset_ = static_cast<usize>(nameOffset);
    nameCount_ = nameCount;
    for (uint32 index = 0; index < entryCount; ++index) {
        const uint8* row =
            bytes.Data() + entryOffset + static_cast<uint64>(index) * ShaderLibraryEntryStride;
        const ShaderLibraryKey key = LoadKey(row);
        const bool broken =
            (index > 0 && !ShaderLibraryKeyLess(LoadKey(row - ShaderLibraryEntryStride), key)) ||
            !core_detail::IsSingleShaderStage(row[EntryStageAt]) || row[EntryDevelopmentAt] > 1 ||
            !IsZero(row + EntryPadAt, EntryFeaturesAt - EntryPadAt) ||
            (LoadLittleEndian<uint32>(row + EntryFeaturesAt) & ~RuntimeCheckedShaderFeatures) !=
                0 ||
            LoadLittleEndian<uint32>(row + EntryCodeAt) >= codeCount ||
            TypeName(key.nameHash).IsEmpty();
        if (broken) {
            Close();
            return ShaderReadStatus::Corrupt("shader library entry is out of order, out of range "
                                             "or names a type missing from the name table");
        }
    }

    kind_ = static_cast<ShaderLibraryKind>(file.U8(KindAt));
    memcpy(facts_.formatId, idField, MaxShaderFormatIdBytes);
    facts_.backend = static_cast<ShaderBackend>(file.U8(BackendAt));
    facts_.shaderModel.major = file.U8(ShaderModelMajorAt);
    facts_.shaderModel.minor = file.U8(ShaderModelMinorAt);
    facts_.features = file.U32(FeaturesAt);
    facts_.deviceRequirements = file.U32(DeviceRequirementsAt);
    uint32 limitValues[LimitCount];
    for (usize index = 0; index < LimitCount; ++index) {
        limitValues[index] = file.U32(LimitsAt + index * sizeof(uint32));
    }

    ShaderFormatLimits& limits = facts_.limits;
    limits.constantBuffers = limitValues[0];
    limits.sampledTextures = limitValues[1];
    limits.storageBuffers = limitValues[2];
    limits.storageImages = limitValues[3];
    limits.samplers = limitValues[4];
    limits.uavTotal = limitValues[5];
    limits.uavStages = limitValues[6];
    limits.renderTargets = limitValues[7];
    limits.interStageVectors = limitValues[8];
    limits.rootConstantBytes = limitValues[9];
    limits.uniformVectors[0] = limitValues[10];
    limits.uniformVectors[1] = limitValues[11];
    limits.uniformVectors[2] = limitValues[12];
    formatDefinitionSha256_ = LoadDigest(bytes.Data() + FormatShaAt);
    typeSetSha256_ = LoadDigest(bytes.Data() + TypeSetShaAt);
    fileSha256_ = LoadDigest(bytes.Data() + FileShaAt);
    entryCount_ = entryCount;
    codeCount_ = codeCount;
    entryOffset_ = static_cast<usize>(entryOffset);
    codeOffset_ = static_cast<usize>(codeOffset);
    blobOffset_ = static_cast<usize>(blobOffset);
    return ShaderReadStatus::Success();
}

ShaderLibraryEntry ShaderLibraryView::Entry(uint32 index) const noexcept {
    using namespace library_detail;
    const uint8* row =
        bytes_.Data() + entryOffset_ + static_cast<usize>(index) * ShaderLibraryEntryStride;
    ShaderLibraryEntry entry;
    entry.key = LoadKey(row);
    entry.stage = row[EntryStageAt];
    entry.developmentOnly = row[EntryDevelopmentAt] != 0;
    entry.runtimeFeatures = LoadLittleEndian<uint32>(row + EntryFeaturesAt);
    entry.codeIndex = LoadLittleEndian<uint32>(row + EntryCodeAt);
    entry.layoutSignature = LoadDigest(row + EntrySignatureAt);
    return entry;
}

bool ShaderLibraryView::Find(const ShaderLibraryKey& key, ShaderLibraryEntry& out) const noexcept {
    using namespace library_detail;
    uint32 low = 0;
    uint32 high = entryCount_;
    while (low < high) {
        const uint32 middle = low + (high - low) / 2;
        const ShaderLibraryKey probe = LoadKey(
            bytes_.Data() + entryOffset_ + static_cast<usize>(middle) * ShaderLibraryEntryStride);
        if (ShaderLibraryKeyLess(probe, key)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low == entryCount_) {
        return false;
    }

    const ShaderLibraryEntry entry = Entry(low);
    if (!(entry.key == key)) {
        return false;
    }

    out = entry;
    return true;
}

Span<const uint8> ShaderLibraryView::CodeBytes(uint32 codeIndex) const noexcept {
    using namespace library_detail;
    const uint8* row =
        bytes_.Data() + codeOffset_ + static_cast<usize>(codeIndex) * ShaderLibraryCodeStride;
    const uint64 offset = LoadLittleEndian<uint64>(row + CodeOffsetFieldAt);
    const uint64 size = LoadLittleEndian<uint64>(row + CodeSizeAt);
    return Span<const uint8>(bytes_.Data() + blobOffset_ + offset, static_cast<usize>(size));
}

Span<const uint8> ShaderLibraryView::Value(const ShaderLibraryEntry& entry,
                                           uint32 valueId) const noexcept {
    using namespace library_detail;
    if (!IsOpen() || entry.codeIndex >= codeCount_) {
        return {};
    }

    // Open이 틀을 검사했다.
    const Span<const uint8> code = CodeBytes(entry.codeIndex);
    const uint32 count = LoadLittleEndian<uint32>(code.Data());
    usize at = CodeCountBytes;
    for (uint32 index = 0; index < count; ++index) {
        const uint32 id = LoadLittleEndian<uint32>(code.Data() + at);
        const uint32 bytes = LoadLittleEndian<uint32>(code.Data() + at + sizeof(uint32));
        at += CodeValueHeaderBytes;
        if (id == valueId) {
            return code.Slice(at, bytes);
        }

        at += bytes;
    }

    return {};
}

Sha256Digest ShaderLibraryView::CodeSha256(uint32 codeIndex) const noexcept {
    using namespace library_detail;
    return LoadDigest(bytes_.Data() + codeOffset_ +
                      static_cast<usize>(codeIndex) * ShaderLibraryCodeStride + CodeShaAt);
}

StringView ShaderLibraryView::TypeName(uint64 nameHash) const noexcept {
    using namespace library_detail;
    uint32 low = 0;
    uint32 high = nameCount_;
    while (low < high) {
        const uint32 middle = low + (high - low) / 2;
        const uint8* row =
            bytes_.Data() + nameOffset_ + static_cast<usize>(middle) * ShaderLibraryNameStride;
        const uint64 hash = LoadLittleEndian<uint64>(row + NameHashAt);
        if (hash == nameHash) {
            return StringView(reinterpret_cast<const char*>(row + NameBytesAt), row[NameLengthAt]);
        }

        if (hash < nameHash) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return StringView();
}

} // namespace rocks::shader
