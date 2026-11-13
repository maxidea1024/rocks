#include "CompressionInternal.h"

#include <Core/Assert.h>
#include <Core/Move.h>
#include <Platform/Sync.h>

// 사용자 등록 형식 표와 등록 코덱 호출이다(Docs/Design/Compression.ko.md 14절). 표는 고정 16칸이며
// 할당하지 않는다. 내장 형식 경로는 이 표를 읽지 않는다.
namespace rocks {
namespace detail {
namespace {

// 등록 한 건이다. users는 살아 있는 CodecLease 수, closing은 해제가 시작됐음(새 사용 거부)이다.
struct Slot {
    bool used = false;
    bool closing = false;
    uint8 nameSize = 0;
    char name[MaxCompressionFormatNameBytes] = {};
    CompressionFormat format = CompressionFormat::Lz4;
    uint32 version = 0;
    CompressionLevelRange levels;
    const CompressionCodec* codec = nullptr;
    usize users = 0;
};

struct Registry {
    platform::Mutex mutex;
    Slot slots[MaxCompressionCodecs];
    // 지금까지 발급한 형식 값 수다. 다음 값은 FirstPluginFormatValue + issued이며 해제한 값을 다시
    // 쓰지 않는다.
    uint64 issued = 0;
};

// 표다. 해제하지 않는다: 프로세스 종료 중 다른 스레드(정적 소멸자·detach된 스레드)가 압축해도
// 소멸된 mutex를 쓰지 않는다. 첫 호출에서 thread-safe하게 만든다(정적 초기화 순서와 무관하다).
Registry& Codecs() noexcept {
    alignas(Registry) static unsigned char storage[sizeof(Registry)];
    static Registry* registry = ConstructAt<Registry>(storage);
    return *registry;
}

// 잠금을 쥔 채 부른다. includeClosing이면 해제 중인 등록도 찾는다.
Slot* FindSlot(Registry& registry, CompressionFormat format, bool includeClosing) noexcept {
    for (Slot& slot : registry.slots) {
        if (slot.used && slot.format == format && (includeClosing || !slot.closing)) {
            return &slot;
        }
    }
    return nullptr;
}

bool SameName(const Slot& slot, StringView name) noexcept {
    if (slot.nameSize != name.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < name.ByteCount(); ++index) {
        if (slot.name[index] != name.Data()[index]) {
            return false;
        }
    }
    return true;
}

// 형식 이름 문법(소문자 ASCII [a-z0-9_-]로 된 비지 않은 조각을 '.'로 이은 1–32byte)을 확인하고 조각
// 수를 돌려준다. 위반이면 0이다. 내장 형식 이름은 조각 하나, 사용자 등록 이름은 둘 이상이다.
uint32 CountNameSegments(StringView name) noexcept {
    if (name.ByteCount() == 0 || name.ByteCount() > MaxCompressionFormatNameBytes) {
        return 0;
    }

    uint32 segments = 1;
    bool segmentEmpty = true;
    for (usize index = 0; index < name.ByteCount(); ++index) {
        const char c = name.Data()[index];
        if (c == '.') {
            if (segmentEmpty) {
                return 0;
            }
            ++segments;
            segmentEmpty = true;
            continue;
        }

        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) {
            return 0;
        }
        segmentEmpty = false;
    }
    return segmentEmpty ? 0 : segments;
}

// 내장 형식 이름이다. 모르는 값이면 nullptr다. 새 형식을 덧붙이면 여기도 덧붙인다(enum switch라
// 빠뜨리면 경고로 빌드가 실패한다).
const char* BuiltinFormatName(CompressionFormat format) noexcept {
    switch (format) {
    case CompressionFormat::Lz4:
        return "lz4";
    case CompressionFormat::Zstd:
        return "zstd";
    case CompressionFormat::Deflate:
        return "deflate";
    case CompressionFormat::Zlib:
        return "zlib";
    case CompressionFormat::Gzip:
        return "gzip";
    case CompressionFormat::Lz4Frame:
        return "lz4frame";
    case CompressionFormat::Brotli:
        return "brotli";
    }
    return nullptr;
}

void CopyName(StringView name, char (&bytes)[MaxCompressionFormatNameBytes], uint8& size) noexcept {
    for (usize index = 0; index < name.ByteCount(); ++index) {
        bytes[index] = name.Data()[index];
    }
    size = static_cast<uint8>(name.ByteCount());
}

bool IsAllowedCodecError(ErrorCode error, bool decompressing) noexcept {
    switch (error) {
    case ErrorCode::InvalidArgument:
    case ErrorCode::CapacityExceeded:
    case ErrorCode::OutOfMemory:
    case ErrorCode::Overflow:
    case ErrorCode::DependencyFailed:
        return true;
    case ErrorCode::Unsupported:
        return decompressing;
    default:
        return false;
    }
}

} // namespace

CodecLease::~CodecLease() noexcept {
    Release();
}

CodecLease::CodecLease(CodecLease&& other) noexcept
    : codec_(other.codec_), format_(other.format_), slot_(other.slot_), levels_(other.levels_) {
    other.codec_ = nullptr;
}

CodecLease& CodecLease::operator=(CodecLease&& other) noexcept {
    if (this != &other) {
        Release();
        codec_ = other.codec_;
        format_ = other.format_;
        slot_ = other.slot_;
        levels_ = other.levels_;
        other.codec_ = nullptr;
    }
    return *this;
}

Result<CodecLease> CodecLease::TryAcquire(CompressionFormat format) noexcept {
    if (!IsPluginFormat(format)) {
        return Result<CodecLease>::Failure(ErrorCode::InvalidArgument);
    }

    Registry& registry = Codecs();
    platform::LockGuard lock(registry.mutex);
    Slot* slot = FindSlot(registry, format, false);
    if (slot == nullptr) {
        return Result<CodecLease>::Failure(ErrorCode::InvalidArgument);
    }

    ++slot->users;
    CodecLease lease;
    lease.codec_ = slot->codec;
    lease.format_ = format;
    lease.slot_ = static_cast<uint32>(slot - registry.slots);
    lease.levels_ = slot->levels;
    return Result<CodecLease>::Success(Move(lease));
}

Result<CodecLease> CodecLease::TryAcquireIfPlugin(CompressionFormat format) noexcept {
    if (!IsPluginFormat(format)) {
        return Result<CodecLease>::Success(CodecLease());
    }
    return TryAcquire(format);
}

void CodecLease::Release() noexcept {
    if (codec_ == nullptr) {
        return;
    }

    Registry& registry = Codecs();
    platform::LockGuard lock(registry.mutex);
    Slot& slot = registry.slots[slot_];
    // 사용 중인 칸은 해제되지 않으므로(Busy) 같은 등록이다.
    ROCKS_ASSERT(slot.used && slot.users != 0 && slot.codec == codec_);
    --slot.users;
    codec_ = nullptr;
}

Result<CompressionLevelRange> PluginLevelRange(CompressionFormat format) noexcept {
    Registry& registry = Codecs();
    platform::LockGuard lock(registry.mutex);
    const Slot* slot = FindSlot(registry, format, false);
    if (slot == nullptr) {
        return Result<CompressionLevelRange>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<CompressionLevelRange>::Success(slot->levels);
}

namespace plugin {

Result<usize> Bound(const CodecLease& plugin, usize sourceBytes) noexcept {
    ROCKS_ASSERT(plugin.Codec() != nullptr);
    auto bound = plugin.Codec()->TryGetCompressBound(sourceBytes);
    if (!bound && bound.Error() != ErrorCode::InvalidArgument &&
        bound.Error() != ErrorCode::Overflow) {
        return Result<usize>::Failure(ErrorCode::DependencyFailed);
    }
    return bound;
}

Result<usize> Compress(const CodecLease& plugin, int32 level, Span<const uint8> source,
                       Span<uint8> destination, Allocator& scratch,
                       const CompressionProbe& probe) noexcept {
    ROCKS_ASSERT(plugin.Codec() != nullptr);
    if (level < plugin.Levels().minimum || level > plugin.Levels().maximum) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    if (probe.IsCancelled()) {
        return Result<usize>::Failure(ErrorCode::Cancelled);
    }

    const CompressionCodec& codec = *plugin.Codec();
    auto written = codec.TryCompress(
        level, Span<const uint8>(NonNullSource(source), source.Size()),
        Span<uint8>(NonNullDestination(destination), destination.Size()), scratch);
    if (written) {
        if (written.Value() > destination.Size()) {
            return Result<usize>::Failure(ErrorCode::DependencyFailed);
        }
        return written;
    }

    const ErrorCode error = written.Error();
    if (!IsAllowedCodecError(error, false)) {
        return Result<usize>::Failure(ErrorCode::DependencyFailed);
    }
    if (error == ErrorCode::CapacityExceeded) {
        // bound 이상의 출력에서 용량 부족은 계약 위반이다(CompressAppend가 bound만큼만 잡는다).
        auto bound = codec.TryGetCompressBound(source.Size());
        if (bound && destination.Size() >= bound.Value()) {
            return Result<usize>::Failure(ErrorCode::DependencyFailed);
        }
    }
    return written;
}

Result<usize> Decompress(const CodecLease& plugin, Span<const uint8> source,
                         Span<uint8> destination, Allocator& scratch,
                         const CompressionProbe& probe) noexcept {
    ROCKS_ASSERT(plugin.Codec() != nullptr);
    if (probe.IsCancelled()) {
        return Result<usize>::Failure(ErrorCode::Cancelled);
    }

    auto written = plugin.Codec()->TryDecompress(
        Span<const uint8>(NonNullSource(source), source.Size()),
        Span<uint8>(NonNullDestination(destination), destination.Size()), scratch);
    if (written) {
        if (written.Value() > destination.Size()) {
            return Result<usize>::Failure(ErrorCode::DependencyFailed);
        }
        return written;
    }
    if (!IsAllowedCodecError(written.Error(), true)) {
        return Result<usize>::Failure(ErrorCode::DependencyFailed);
    }
    return written;
}

Result<usize> DecompressAppend(const CodecLease& plugin, Span<const uint8> source,
                               usize maxOutputBytes, Array<uint8>& output, Allocator& scratch,
                               const CompressionProbe& probe) noexcept {
    const usize base = output.Size();
    // 첫 크기는 64KiB와 입력 × 4 중 큰 값이다. 입력 × 4가 상한을 넘으면 상한에서 시작한다(상한이
    // 입력의 4배보다 작으므로 큰 할당이 아니고, 작은 크기부터 여러 번 다시 해제하지 않는다).
    usize capacity = DecodeOutputInitialBytes;
    if (source.Size() > maxOutputBytes / 4) {
        capacity = maxOutputBytes;
    } else if (source.Size() * 4 > capacity) {
        capacity = source.Size() * 4;
    }
    if (capacity > maxOutputBytes) {
        capacity = maxOutputBytes;
    }

    while (true) {
        // 재시도 전 길이는 base다(축소는 저장소를 유지한다). 늘린 구간은 0으로 채우지 않는다.
        auto grown = output.TryAppendUninitialized(capacity);
        if (!grown) {
            RestoreLength(output, base);
            return Result<usize>::Failure(grown.Error());
        }

        // 취소는 Decompress가 코덱 호출 전마다(곧 재시도 사이에도) 확인한다.
        auto written =
            Decompress(plugin, source, Span<uint8>(output.Data() + base, capacity), scratch, probe);
        if (written) {
            RestoreLength(output, base + written.Value());
            return written;
        }

        RestoreLength(output, base);
        if (written.Error() != ErrorCode::CapacityExceeded || capacity == maxOutputBytes) {
            return written;
        }
        capacity = capacity > maxOutputBytes - capacity ? maxOutputBytes : capacity * 2;
    }
}

} // namespace plugin
} // namespace detail

Result<CompressionFormat>
TryRegisterCompressionCodec(const CompressionCodecDescription& description,
                            const CompressionCodec& codec) noexcept {
    if (detail::CountNameSegments(description.name) < 2) {
        return Result<CompressionFormat>::Failure(ErrorCode::InvalidArgument);
    }

    const CompressionLevelRange& levels = description.levels;
    if (levels.minimum > levels.defaultLevel || levels.defaultLevel > levels.maximum) {
        return Result<CompressionFormat>::Failure(ErrorCode::InvalidArgument);
    }

    detail::Registry& registry = detail::Codecs();
    platform::LockGuard lock(registry.mutex);
    detail::Slot* free = nullptr;
    for (detail::Slot& slot : registry.slots) {
        if (!slot.used) {
            free = free != nullptr ? free : &slot;
            continue;
        }
        if (detail::SameName(slot, description.name)) {
            return Result<CompressionFormat>::Failure(ErrorCode::AlreadyExists);
        }
    }
    constexpr uint64 MaxIssued = uint64{0xFFFFFFFF} - detail::FirstPluginFormatValue;
    if (free == nullptr || registry.issued > MaxIssued) {
        return Result<CompressionFormat>::Failure(ErrorCode::CapacityExceeded);
    }

    const auto format =
        static_cast<CompressionFormat>(detail::FirstPluginFormatValue + registry.issued);
    ++registry.issued;
    *free = detail::Slot{};
    free->used = true;
    detail::CopyName(description.name, free->name, free->nameSize);
    free->format = format;
    free->version = description.version;
    free->levels = levels;
    free->codec = &codec;
    return Result<CompressionFormat>::Success(format);
}

Result<void> TryUnregisterCompressionCodec(CompressionFormat format) noexcept {
    if (!detail::IsPluginFormat(format)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::Registry& registry = detail::Codecs();
    platform::LockGuard lock(registry.mutex);
    detail::Slot* slot = detail::FindSlot(registry, format, true);
    if (slot == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 처음 부른 순간부터 새 사용을 막는다. 남은 사용이 끝난 뒤 다시 부르면 칸을 비운다.
    slot->closing = true;
    if (slot->users != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    *slot = detail::Slot{};
    return Result<void>::Success();
}

Result<CompressionFormatInfo> TryGetCompressionFormatInfo(CompressionFormat format) noexcept {
    CompressionFormatInfo info;
    if (!detail::IsPluginFormat(format)) {
        const char* name = detail::BuiltinFormatName(format);
        if (name == nullptr) {
            return Result<CompressionFormatInfo>::Failure(ErrorCode::InvalidArgument);
        }

        detail::CopyName(StringView(name), info.nameBytes, info.nameSize);
        info.revision = CompressionOutputRevision;
        info.compresses = detail::ResolveFormat(format).compresses;
        return Result<CompressionFormatInfo>::Success(info);
    }

    detail::Registry& registry = detail::Codecs();
    platform::LockGuard lock(registry.mutex);
    const detail::Slot* slot = detail::FindSlot(registry, format, false);
    if (slot == nullptr) {
        return Result<CompressionFormatInfo>::Failure(ErrorCode::InvalidArgument);
    }

    detail::CopyName(StringView(slot->name, slot->nameSize), info.nameBytes, info.nameSize);
    info.revision = slot->version;
    info.plugin = true;
    info.compresses = true;
    return Result<CompressionFormatInfo>::Success(info);
}

Result<CompressionFormat> TryFindCompressionFormat(StringView name) noexcept {
    const uint32 segments = detail::CountNameSegments(name);
    if (segments == 0) {
        return Result<CompressionFormat>::Failure(ErrorCode::InvalidArgument);
    }

    if (segments == 1) {
        // 내장 형식 값은 0부터 빈틈없이 이어진다(끝에 덧붙인다).
        for (uint32 value = 0; value < detail::FirstPluginFormatValue; ++value) {
            const auto format = static_cast<CompressionFormat>(value);
            const char* builtin = detail::BuiltinFormatName(format);
            if (builtin == nullptr) {
                break;
            }
            if (StringView(builtin) == name) {
                return Result<CompressionFormat>::Success(format);
            }
        }
        return Result<CompressionFormat>::Failure(ErrorCode::NotFound);
    }

    detail::Registry& registry = detail::Codecs();
    platform::LockGuard lock(registry.mutex);
    for (const detail::Slot& slot : registry.slots) {
        if (slot.used && !slot.closing && detail::SameName(slot, name)) {
            return Result<CompressionFormat>::Success(slot.format);
        }
    }
    return Result<CompressionFormat>::Failure(ErrorCode::NotFound);
}

} // namespace rocks
