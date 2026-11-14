#include <Crypto/Bytes.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

int SodiumVariant(Base64Variant variant) noexcept {
    switch (variant) {
    case Base64Variant::Standard:
        return sodium_base64_VARIANT_ORIGINAL;
    case Base64Variant::StandardNoPadding:
        return sodium_base64_VARIANT_ORIGINAL_NO_PADDING;
    case Base64Variant::UrlSafe:
        return sodium_base64_VARIANT_URLSAFE;
    case Base64Variant::UrlSafeNoPadding:
        return sodium_base64_VARIANT_URLSAFE_NO_PADDING;
    }

    return -1;
}

bool HasPadding(Base64Variant variant) noexcept {
    return variant == Base64Variant::Standard || variant == Base64Variant::UrlSafe;
}

// 올바른 base64 텍스트라면 풀린 byte 수다. 텍스트가 형식상 불가능한 길이면 InvalidArgument다. 내용(알파벳·끝 bit) 검사는
// libsodium이 한다. 출력 Span 부족을 형식 오류와 구분하려고 먼저 계산한다.
Result<usize> DecodedBytes(StringView text, Base64Variant variant) noexcept {
    const usize length = text.ByteCount();
    if (HasPadding(variant)) {
        if (length % 4 != 0) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        usize padding = 0;
        if (length != 0 && text.Data()[length - 1] == '=') {
            padding = length >= 2 && text.Data()[length - 2] == '=' ? 2 : 1;
        }

        return Result<usize>::Success(length / 4 * 3 - padding);
    }

    if (length % 4 == 1) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(length / 4 * 3 + (length % 4 == 0 ? 0 : length % 4 - 1));
}

} // namespace

bool ConstantTimeEquals(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    return left.IsEmpty() || sodium_memcmp(left.Data(), right.Data(), left.Size()) == 0;
}

bool IsAllZero(Span<const uint8> bytes) noexcept {
    return bytes.IsEmpty() || sodium_is_zero(bytes.Data(), bytes.Size()) == 1;
}

void SecureZero(Span<uint8> bytes) noexcept {
    if (!bytes.IsEmpty()) {
        sodium_memzero(bytes.Data(), bytes.Size());
    }
}

Result<usize> TryGetHexChars(usize bytes) noexcept {
    // NUL 하나를 더 쓸 자리도 있어야 하므로 2 * bytes + 1이 넘치지 않아야 한다.
    if (bytes > (MaxSize - 1) / 2) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    return Result<usize>::Success(bytes * 2);
}

Result<usize> TryGetBase64Chars(usize bytes, Base64Variant variant) noexcept {
    if (SodiumVariant(variant) < 0) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // 4 * ceil(bytes / 3) + NUL이 넘치지 않아야 한다.
    if (bytes / 3 >= (MaxSize - 5) / 4) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    const usize whole = bytes / 3 * 4;
    const usize rest = bytes % 3;
    if (rest == 0) {
        return Result<usize>::Success(whole);
    }

    return Result<usize>::Success(whole + (HasPadding(variant) ? 4 : rest + 1));
}

Result<usize> TryToHex(Span<const uint8> bytes, Span<char> text) noexcept {
    auto chars = TryGetHexChars(bytes.Size());
    if (!chars) {
        return chars;
    }

    if (text.Size() < chars.Value() + 1) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    sodium_bin2hex(text.Data(), text.Size(), bytes.Data(), bytes.Size());
    return chars;
}

Result<usize> TryFromHex(StringView text, Span<uint8> bytes) noexcept {
    const usize length = text.ByteCount();
    if (length % 2 != 0) {
        SecureZero(bytes);
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const usize needed = length / 2;
    if (bytes.Size() < needed) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    if (length == 0) {
        return Result<usize>::Success(0);
    }

    size_t written = 0;
    const char* end = nullptr;
    const int status = sodium_hex2bin(bytes.Data(), bytes.Size(), text.Data(), length, nullptr, &written, &end);
    if (status != 0 || end != text.Data() + length || written != needed) {
        SecureZero(bytes);
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(written);
}

Result<usize> TryToBase64(Span<const uint8> bytes, Base64Variant variant, Span<char> text) noexcept {
    auto chars = TryGetBase64Chars(bytes.Size(), variant);
    if (!chars) {
        return chars;
    }

    if (text.Size() < chars.Value() + 1) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    sodium_bin2base64(text.Data(), text.Size(), bytes.Data(), bytes.Size(), SodiumVariant(variant));
    return chars;
}

Result<usize> TryFromBase64(StringView text, Base64Variant variant, Span<uint8> bytes) noexcept {
    if (SodiumVariant(variant) < 0) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto needed = DecodedBytes(text, variant);
    if (!needed) {
        SecureZero(bytes);
        return needed;
    }

    if (bytes.Size() < needed.Value()) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    if (text.IsEmpty()) {
        return Result<usize>::Success(0);
    }

    size_t written = 0;
    const char* end = nullptr;
    const int status = sodium_base642bin(bytes.Data(), bytes.Size(), text.Data(), text.ByteCount(), nullptr, &written,
                                         &end, SodiumVariant(variant));
    if (status != 0 || end != text.Data() + text.ByteCount() || written != needed.Value()) {
        SecureZero(bytes);
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(written);
}

} // namespace rocks::crypto
