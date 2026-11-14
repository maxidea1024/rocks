#include <Crypto/Bytes.h>

#include "Sodium/SodiumRuntime.h"

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

// 올바른 base64 텍스트라면 풀린 byte 수다. 길이·끝 '='만 보고 불가능한 텍스트면 InvalidArgument다.
// 내용(알파벳·끝 bit) 검사는 libsodium이 한다. 출력 Span 부족을 형식 오류와 구분하려고 먼저
// 계산한다.
Result<usize> DecodedBytes(StringView text, Base64Variant variant) noexcept {
    const usize length = text.ByteCount();
    const char* data = text.Data();
    if (HasPadding(variant)) {
        // '='는 끝에 둘까지다. 셋 이상이면 아래 계산이 실제보다 큰 크기를 내어 형식 오류가
        // CapacityExceeded로 보인다.
        if (length % 4 != 0 || (length != 0 && data[length - 3] == '=')) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        usize padding = 0;
        if (length != 0 && data[length - 1] == '=') {
            padding = data[length - 2] == '=' ? 2 : 1;
        }

        return Result<usize>::Success(length / 4 * 3 - padding);
    }

    // 패딩 없는 variant에 패딩을 붙인 입력('='를 데이터로 세면 크기가 틀린다)도 여기서 거부한다.
    if (length % 4 == 1 || (length != 0 && data[length - 1] == '=')) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(length / 4 * 3 + (length % 4 == 0 ? 0 : length % 4 - 1));
}

} // namespace

// 비교·0 검사·소거는 sodium_init과 무관하지만, 모든 libsodium 진입점이 초기화(misuse handler 설치
// 포함)를 거친다는 계약을 한 가지로 유지한다(Docs/Design/Crypto.ko.md 5.1절). 초기화 뒤의 비용은
// 원자적 읽기 하나다.
bool ConstantTimeEquals(Span<const uint8> left, Span<const uint8> right) noexcept {
    detail::EnsureSodium();
    if (left.Size() != right.Size()) {
        return false;
    }

    return left.IsEmpty() || sodium_memcmp(left.Data(), right.Data(), left.Size()) == 0;
}

bool IsAllZero(Span<const uint8> bytes) noexcept {
    detail::EnsureSodium();
    return bytes.IsEmpty() || sodium_is_zero(bytes.Data(), bytes.Size()) == 1;
}

void SecureZero(Span<uint8> bytes) noexcept {
    detail::EnsureSodium();
    if (!bytes.IsEmpty()) {
        sodium_memzero(bytes.Data(), bytes.Size());
    }
}

Result<usize> TryGetHexChars(usize bytes) noexcept {
    // NUL 하나를 더 쓸 자리도 있어야 하므로 2 * bytes + 1이 넘치지 않아야 한다. libsodium은
    // bytes >= SIZE_MAX / 2를 misuse로 보므로 같은 경계를 쓴다.
    if (bytes >= MaxSize / 2) {
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

    // 앞 문자를 쓰는 동안 아직 읽지 않은 입력을 덮으면 결과가 깨진다. 쓸 범위(문자 + NUL)와 입력이
    // 겹치면 거부한다.
    const Span<char> written = text.Left(chars.Value() + 1);
    if (Overlaps(bytes, written)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // 최대 길이로 쓸 범위만 넘긴다. libsodium은 NUL 뒤 남은 공간도 0으로 채우므로 text 전체를
    // 넘기면 뒤를 덮는다.
    detail::EnsureSodium();
    sodium_bin2hex(written.Data(), written.Size(), bytes.Data(), bytes.Size());
    return chars;
}

Result<usize> TryFromHex(StringView text, Span<uint8> bytes) noexcept {
    const usize length = text.ByteCount();
    if (length % 2 != 0) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const usize needed = length / 2;
    if (bytes.Size() < needed) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    if (length == 0) {
        return Result<usize>::Success(0);
    }

    // 실패하면 출력 전체를 지우므로 입력이 출력 어디와도 겹치면 안 된다.
    if (Overlaps(bytes, Span<const char>(text.Data(), length))) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    size_t written = 0;
    const char* end = nullptr;
    const int status =
        sodium_hex2bin(bytes.Data(), needed, text.Data(), length, nullptr, &written, &end);
    if (status != 0 || end != text.Data() + length || written != needed) {
        SecureZero(bytes);
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(written);
}

Result<usize> TryToBase64(Span<const uint8> bytes, Base64Variant variant,
                          Span<char> text) noexcept {
    auto chars = TryGetBase64Chars(bytes.Size(), variant);
    if (!chars) {
        return chars;
    }

    if (text.Size() < chars.Value() + 1) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    // TryToHex와 같은 이유로 쓸 범위만 넘기고, 그 범위와 입력이 겹치면 거부한다.
    const Span<char> written = text.Left(chars.Value() + 1);
    if (Overlaps(bytes, written)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    sodium_bin2base64(written.Data(), written.Size(), bytes.Data(), bytes.Size(),
                      SodiumVariant(variant));
    return chars;
}

Result<usize> TryFromBase64(StringView text, Base64Variant variant, Span<uint8> bytes) noexcept {
    if (SodiumVariant(variant) < 0) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto needed = DecodedBytes(text, variant);
    if (!needed) {
        return needed;
    }

    if (bytes.Size() < needed.Value()) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    if (text.IsEmpty()) {
        return Result<usize>::Success(0);
    }

    // 실패하면 출력 전체를 지우므로 입력이 출력 어디와도 겹치면 안 된다.
    if (Overlaps(bytes, Span<const char>(text.Data(), text.ByteCount()))) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    size_t written = 0;
    const char* end = nullptr;
    const int status =
        sodium_base642bin(bytes.Data(), needed.Value(), text.Data(), text.ByteCount(), nullptr,
                          &written, &end, SodiumVariant(variant));
    if (status != 0 || end != text.Data() + text.ByteCount() || written != needed.Value()) {
        SecureZero(bytes);
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(written);
}

} // namespace rocks::crypto
