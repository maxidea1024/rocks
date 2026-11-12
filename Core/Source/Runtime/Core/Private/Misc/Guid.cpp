#include <Base/Endian.h>
#include <Core/Guid.h>
#include <Platform/SecureRandom.h>

namespace rocks {
namespace {

int HexValue(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }

    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }

    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }

    return -1;
}

} // namespace

Result<Guid> TryNewGuid() noexcept {
    uint8 bytes[16];
    if (!platform::TryFillSecureRandom(bytes, sizeof(bytes))) {
        return Result<Guid>::Failure(ErrorCode::Unsupported);
    }

    // RFC 9562 version 4(무작위)·variant 10xx를 표시한다.
    bytes[6] = static_cast<uint8>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8>((bytes[8] & 0x3f) | 0x80);
    Guid value;
    value.high = LoadBigEndian<uint64>(bytes);
    value.low = LoadBigEndian<uint64>(bytes + 8);
    return Result<Guid>::Success(value);
}

Result<Guid> TryParseGuid(StringView text) noexcept {
    const usize length = text.ByteCount();
    const bool hyphenated = length == 36;
    if (!hyphenated && length != 32) {
        return Result<Guid>::Failure(ErrorCode::InvalidArgument);
    }

    uint64 halves[2] = {0, 0};
    usize digits = 0;
    for (usize index = 0; index < length; ++index) {
        const char character = text.Data()[index];
        // 표준 표기의 구분자 위치(8·13·18·23)에는 '-'만, 그 밖에는 16진 숫자만 받는다.
        if (hyphenated && (index == 8 || index == 13 || index == 18 || index == 23)) {
            if (character != '-') {
                return Result<Guid>::Failure(ErrorCode::InvalidArgument);
            }

            continue;
        }

        const int value = HexValue(character);
        if (value < 0) {
            return Result<Guid>::Failure(ErrorCode::InvalidArgument);
        }

        uint64& half = halves[digits / 16];
        half = (half << 4) | static_cast<uint64>(value);
        ++digits;
    }

    Guid result;
    result.high = halves[0];
    result.low = halves[1];
    return Result<Guid>::Success(result);
}

Result<void> Formatter<Guid>::Format(StringBuilderBase& out, const Guid& value,
                                     StringView spec) noexcept {
    if (!spec.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    constexpr char Digits[] = "0123456789abcdef";
    char text[36];
    usize cursor = 0;
    for (usize digit = 0; digit < 32; ++digit) {
        if (digit == 8 || digit == 12 || digit == 16 || digit == 20) {
            text[cursor++] = '-';
        }

        const uint64 half = digit < 16 ? value.high : value.low;
        const uint32 shift = static_cast<uint32>((15 - digit % 16) * 4);
        text[cursor++] = Digits[(half >> shift) & 0xf];
    }

    return out.TryAppend(StringView(text, sizeof(text)));
}

} // namespace rocks
