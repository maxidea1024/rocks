#include "MiniaudioSupport.h"

#include <Base/NumericLimits.h>

#include <Core/Memory/Allocator.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <miniaudio.h>

namespace rocks::audio::miniaudio_support {

static_assert(MaSuccess == MA_SUCCESS && MaOutOfMemory == MA_OUT_OF_MEMORY &&
                  MaDoesNotExist == MA_DOES_NOT_EXIST && MaBusy == MA_BUSY &&
                  MaNoBackend == MA_NO_BACKEND && MaNoDevice == MA_NO_DEVICE,
              "MiniaudioSupport.h ma_result values must match miniaudio.h");

namespace {

constexpr char32_t ReplacementCharacter = 0xFFFD;
constexpr char32_t HighSurrogateFirst = 0xD800;
constexpr char32_t LowSurrogateFirst = 0xDC00;
constexpr char32_t SurrogateLast = 0xDFFF;
constexpr char32_t SupplementaryFirst = 0x10000;
constexpr uint32 SurrogateShift = 10;
constexpr uint32 MaxUtf8Bytes = 4;
constexpr uint32 MaxDecimalDigits = 20;
constexpr uint32 DecimalBase = 10;

// 코드 포인트 하나를 UTF-8로 쓴다. 쓴 byte 수다(1–4).
uint32 EncodeUtf8(char32_t codePoint, char* out) noexcept {
    if (codePoint < 0x80) {
        out[0] = static_cast<char>(codePoint);
        return 1;
    }

    if (codePoint < 0x800) {
        out[0] = static_cast<char>(0xC0 | (codePoint >> 6));
        out[1] = static_cast<char>(0x80 | (codePoint & 0x3F));
        return 2;
    }

    if (codePoint < SupplementaryFirst) {
        out[0] = static_cast<char>(0xE0 | (codePoint >> 12));
        out[1] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (codePoint & 0x3F));
        return 3;
    }

    out[0] = static_cast<char>(0xF0 | (codePoint >> 18));
    out[1] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (codePoint & 0x3F));
    return 4;
}

bool IsHighSurrogate(char32_t unit) noexcept {
    return unit >= HighSurrogateFirst && unit < LowSurrogateFirst;
}

bool IsLowSurrogate(char32_t unit) noexcept {
    return unit >= LowSurrogateFirst && unit <= SurrogateLast;
}

usize Fail(Span<char> out) noexcept {
    if (out.Size() != 0) {
        out[0] = '\0';
    }

    return 0;
}

// out에 bytes를 이어 쓸 자리(NUL 한 칸 포함)가 있는지다.
bool Fits(Span<char> out, usize used, usize bytes) noexcept {
    return out.Size() != 0 && bytes <= out.Size() - 1 - used;
}

// 머리를 포함한 블록 시작이다.
unsigned char* BlockOf(void* address) noexcept {
    return static_cast<unsigned char*>(address) - MiniaudioAllocationHeader;
}

usize SizeOf(void* address) noexcept {
    usize bytes = 0;
    platform::MemoryCopy(&bytes, BlockOf(address), sizeof(bytes));
    return bytes;
}

} // namespace

ErrorCode ErrorCodeFromMaResult(int32 result) noexcept {
    switch (result) {
    case MaSuccess:
        return ErrorCode::None;
    case MaOutOfMemory:
        return ErrorCode::OutOfMemory;
    case MaDoesNotExist:
    case MaNoBackend:
    case MaNoDevice:
        return ErrorCode::NotFound;
    case MaBusy:
        return ErrorCode::Busy;
    default:
        return ErrorCode::Unsupported;
    }
}

uint32 CallbackFramesUpperBound(uint32 period, uint32 clientRate, uint32 internalRate) noexcept {
    if (clientRate == 0 || internalRate == 0 || clientRate == internalRate) {
        return period;
    }

    const uint64 converted = (uint64{period} * clientRate + internalRate - 1) / internalRate + 1;
    return converted > NumericLimits<uint32>::Max() ? NumericLimits<uint32>::Max()
                                                    : static_cast<uint32>(converted);
}

usize EncodeUtf16DeviceId(const char16_t* text, usize units, Span<char> out) noexcept {
    usize used = 0;
    usize index = 0;
    while (text != nullptr && index < units && text[index] != 0) {
        char32_t codePoint = static_cast<char32_t>(text[index]);
        ++index;
        if (IsHighSurrogate(codePoint) && index < units &&
            IsLowSurrogate(static_cast<char32_t>(text[index]))) {
            const char32_t low = static_cast<char32_t>(text[index]);
            ++index;
            codePoint = SupplementaryFirst + ((codePoint - HighSurrogateFirst) << SurrogateShift) +
                        (low - LowSurrogateFirst);
        } else if (IsHighSurrogate(codePoint) || IsLowSurrogate(codePoint)) {
            codePoint = ReplacementCharacter;
        }

        char encoded[MaxUtf8Bytes];
        const uint32 bytes = EncodeUtf8(codePoint, encoded);
        if (!Fits(out, used, bytes)) {
            return Fail(out);
        }

        platform::MemoryCopy(out.Data() + used, encoded, bytes);
        used += bytes;
    }

    if (used == 0) {
        return Fail(out);
    }

    out[used] = '\0';
    return used;
}

usize EncodeTextDeviceId(const char* text, usize bytes, Span<char> out) noexcept {
    usize length = 0;
    while (text != nullptr && length < bytes && text[length] != '\0') {
        ++length;
    }

    if (length == 0 || !Fits(out, 0, length)) {
        return Fail(out);
    }

    platform::MemoryCopy(out.Data(), text, length);
    out[length] = '\0';
    return length;
}

usize EncodeIntegerDeviceId(int64 value, Span<char> out) noexcept {
    char digits[MaxDecimalDigits];
    uint32 count = 0;
    // 음수의 크기는 uint64로 계산한다(INT64_MIN도 넘치지 않게).
    uint64 magnitude = value < 0 ? 0 - static_cast<uint64>(value) : static_cast<uint64>(value);
    do {
        digits[count++] = static_cast<char>('0' + magnitude % DecimalBase);
        magnitude /= DecimalBase;
    } while (magnitude != 0);

    const usize length = count + (value < 0 ? 1 : 0);
    if (!Fits(out, 0, length)) {
        return Fail(out);
    }

    usize used = 0;
    if (value < 0) {
        out[used++] = '-';
    }

    while (count > 0) {
        out[used++] = digits[--count];
    }

    out[used] = '\0';
    return used;
}

usize CopyDeviceName(const char* name, usize bytes, Span<char> out) noexcept {
    if (out.Size() == 0) {
        return 0;
    }

    usize length = 0;
    while (name != nullptr && length < bytes && name[length] != '\0') {
        ++length;
    }

    if (length > out.Size() - 1) {
        length = out.Size() - 1;
        // 잘린 자리가 다중 byte 문자 가운데면 그 문자 시작까지 물린다.
        while (length > 0 && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80) {
            --length;
        }
    }

    if (length != 0) {
        platform::MemoryCopy(out.Data(), name, length);
    }

    out[length] = '\0';
    return length;
}

void* AllocateForMiniaudio(usize bytes, void* allocator) noexcept {
    if (bytes > MaxSize - MiniaudioAllocationHeader) {
        return nullptr;
    }

    auto memory = static_cast<Allocator*>(allocator)->Allocate(bytes + MiniaudioAllocationHeader,
                                                               MiniaudioAllocationHeader);
    if (!memory) {
        return nullptr;
    }

    auto* block = static_cast<unsigned char*>(memory.Value());
    platform::MemoryCopy(block, &bytes, sizeof(bytes));
    return block + MiniaudioAllocationHeader;
}

void* ReallocateForMiniaudio(void* address, usize bytes, void* allocator) noexcept {
    if (address == nullptr) {
        return AllocateForMiniaudio(bytes, allocator);
    }

    void* moved = AllocateForMiniaudio(bytes, allocator);
    if (moved == nullptr) {
        return nullptr;
    }

    const usize old = SizeOf(address);
    platform::MemoryCopy(moved, address, old < bytes ? old : bytes);
    FreeForMiniaudio(address, allocator);
    return moved;
}

void FreeForMiniaudio(void* address, void* allocator) noexcept {
    if (address == nullptr) {
        return;
    }

    if (!static_cast<Allocator*>(allocator)->Deallocate(BlockOf(address))) {
        platform::Fatal("miniaudio allocation could not be returned to its allocator");
    }
}

} // namespace rocks::audio::miniaudio_support
