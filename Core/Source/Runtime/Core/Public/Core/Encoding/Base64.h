#pragma once

#include <Base/Checked.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

namespace rocks {

// RFC 4648 표준 Base64(알파벳 A–Z a–z 0–9 + /, '=' 패딩)다. 할당하지 않고 호출자 버퍼에 쓴다.
// 사용 예:
//   char text[64];
//   auto written = TryBase64Encode(bytes, Span<char>(text));
//   uint8 decoded[48];
//   auto count = TryBase64Decode(StringView(text, written.Value()), Span<uint8>(decoded));
// 계약:
// - 인코딩 결과는 NUL로 끝나지 않는다. 필요한 크기는 Base64EncodedSize로 미리 구한다.
// - 디코딩은 엄격하다: 길이는 4의 배수, '='는 끝의 1~2자만, 공백·줄바꿈·URL-safe 알파벳·
//   패딩 생략은 거부한다. 마지막 문자의 사용하지 않는 bit가 0이 아니면(비정규 인코딩) 거부한다.
// - 실패하면 출력 버퍼를 전혀 바꾸지 않는다. 입력 검증과 필요 크기 확인을 쓰기 전에 끝낸다.
// - 오류: 출력 버퍼 부족은 CapacityExceeded, 잘못된 입력은 InvalidArgument, 크기 overflow는
// Overflow.
namespace detail {

inline constexpr char Base64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr int32 Base64Value(char character) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

} // namespace detail

// byteCount 바이트를 인코딩한 문자 수(패딩 포함)다. overflow하면 Overflow다.
constexpr Result<usize> Base64EncodedSize(usize byteCount) noexcept {
    usize groups = byteCount / 3 + (byteCount % 3 != 0 ? 1 : 0);
    usize characters = 0;
    if (!TryMultiply(groups, 4, characters)) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }
    return Result<usize>::Success(characters);
}

inline Result<usize> TryBase64Encode(Span<const uint8> input, Span<char> output) noexcept {
    auto required = Base64EncodedSize(input.Size());
    if (!required) {
        return Result<usize>::Failure(required.Error());
    }
    if (output.Size() < required.Value()) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }
    usize written = 0;
    usize index = 0;
    for (; index + 3 <= input.Size(); index += 3) {
        const uint32 group = (static_cast<uint32>(input[index]) << 16) |
                             (static_cast<uint32>(input[index + 1]) << 8) | input[index + 2];
        output[written++] = detail::Base64Alphabet[(group >> 18) & 0x3fu];
        output[written++] = detail::Base64Alphabet[(group >> 12) & 0x3fu];
        output[written++] = detail::Base64Alphabet[(group >> 6) & 0x3fu];
        output[written++] = detail::Base64Alphabet[group & 0x3fu];
    }
    const usize remaining = input.Size() - index;
    if (remaining != 0) {
        uint32 group = static_cast<uint32>(input[index]) << 16;
        if (remaining == 2) {
            group |= static_cast<uint32>(input[index + 1]) << 8;
        }
        output[written++] = detail::Base64Alphabet[(group >> 18) & 0x3fu];
        output[written++] = detail::Base64Alphabet[(group >> 12) & 0x3fu];
        output[written++] = remaining == 2 ? detail::Base64Alphabet[(group >> 6) & 0x3fu] : '=';
        output[written++] = '=';
    }
    return Result<usize>::Success(written);
}

// 디코딩한 byte 수를 반환한다. 실패 시 output은 바뀌지 않는다.
inline Result<usize> TryBase64Decode(StringView text, Span<uint8> output) noexcept {
    const usize length = text.ByteCount();
    const char* characters = text.Data();
    if (length % 4 != 0) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    usize padding = 0;
    if (length != 0 && characters[length - 1] == '=') {
        padding = characters[length - 2] == '=' ? 2 : 1;
    }
    // 1단계: 전체 입력 검증. 패딩 앞의 모든 문자는 알파벳이어야 한다.
    for (usize index = 0; index < length - padding; ++index) {
        if (detail::Base64Value(characters[index]) < 0) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }
    }
    if (padding != 0) {
        // 사용하지 않는 하위 bit가 0이어야 정규 인코딩이다.
        const int32 last = detail::Base64Value(characters[length - padding - 1]);
        const int32 unusedMask = padding == 2 ? 0x0f : 0x03;
        if ((last & unusedMask) != 0) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }
    }
    const usize decodedSize = length / 4 * 3 - padding;
    if (output.Size() < decodedSize) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }
    // 2단계: 검증이 끝난 입력만 쓴다.
    usize written = 0;
    for (usize index = 0; index < length; index += 4) {
        uint32 group = 0;
        for (usize offset = 0; offset < 4; ++offset) {
            const char character = characters[index + offset];
            const int32 value = character == '=' ? 0 : detail::Base64Value(character);
            group = (group << 6) | static_cast<uint32>(value);
        }
        const usize bytes = index + 4 == length ? 3 - padding : 3;
        output[written++] = static_cast<uint8>(group >> 16);
        if (bytes > 1) {
            output[written++] = static_cast<uint8>(group >> 8);
        }
        if (bytes > 2) {
            output[written++] = static_cast<uint8>(group);
        }
    }
    return Result<usize>::Success(written);
}

} // namespace rocks
