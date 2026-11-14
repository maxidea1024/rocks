#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 비밀 byte를 다루는 기반 연산이다(Docs/Design/Crypto.ko.md 8절). 키·tag·토큰처럼 비밀을 담은 byte는 일반 비교
// (memcmp, operator==)·일반 hex/base64(Core Base64)를 쓰지 않는다. 일반 구현은 첫 다른 byte에서 멈추거나 입력에 따라
// 표를 다르게 읽어 실행 시간이 비밀을 흘린다. 이 헤더의 함수는 libsodium의 상수 시간 구현을 쓴다.
//
// 사용 예:
//   if (!crypto::ConstantTimeEquals(expectedTag, receivedTag)) { return AuthenticationFailed; }
//   crypto::SecureZero(Span<uint8>(scratchKey));             // 컴파일러가 지우지 않는 소거
//   char text[2 * 32 + 1];
//   auto written = crypto::TryToHex(key.View(), Span<char>(text));   // 64자 + NUL
namespace rocks::crypto {

// 두 byte 범위가 같으면 true다. 길이가 다르면 false다(길이는 비밀이 아니다). 같은 길이면 내용과 무관하게 같은 시간이
// 걸린다(sodium_memcmp). 사전순 비교는 하지 않는다.
[[nodiscard]] bool ConstantTimeEquals(Span<const uint8> left, Span<const uint8> right) noexcept;

// 모든 byte가 0이면 true다(상수 시간, sodium_is_zero). 빈 범위는 true다.
[[nodiscard]] bool IsAllZero(Span<const uint8> bytes) noexcept;

// 범위를 0으로 지운다. 최적화로 없어지지 않는 소거다(sodium_memzero). 비밀이 지나간 버퍼를 수명 끝에 지울 때 쓴다.
void SecureZero(Span<uint8> bytes) noexcept;

// ---- 상수 시간 hex·base64 ----
// 출력은 호출자가 준 Span이다. text 출력은 끝에 NUL 하나를 더 쓰므로 필요한 크기는 Get…Chars() + 1이다. 반환값은 쓴
// 문자 수(NUL 제외) 또는 byte 수다. 출력 Span이 작으면 CapacityExceeded이고 아무것도 쓰지 않는다.

enum class Base64Variant : uint8 {
    Standard,           // RFC 4648 기본 알파벳, '=' 패딩
    StandardNoPadding,  // 기본 알파벳, 패딩 없음
    UrlSafe,            // RFC 4648 URL·파일 이름 안전 알파벳('-', '_'), '=' 패딩
    UrlSafeNoPadding,   // URL 안전 알파벳, 패딩 없음(JWT 등)
};

// bytes를 소문자 hex로 쓴 문자 수다(2 * bytes). overflow면 Overflow다.
[[nodiscard]] Result<usize> TryGetHexChars(usize bytes) noexcept;

// bytes를 base64로 쓴 문자 수다(NUL 제외). overflow면 Overflow다.
[[nodiscard]] Result<usize> TryGetBase64Chars(usize bytes, Base64Variant variant) noexcept;

[[nodiscard]] Result<usize> TryToHex(Span<const uint8> bytes, Span<char> text) noexcept;

// hex 문자열(대소문자 무관, 구분자 없음)을 byte로 쓴다. 홀수 길이·hex가 아닌 문자는 InvalidArgument, 출력이 모자라면
// CapacityExceeded다. 실패하면 bytes 전체를 0으로 지운다(일부만 풀린 비밀이 남지 않게).
[[nodiscard]] Result<usize> TryFromHex(StringView text, Span<uint8> bytes) noexcept;

[[nodiscard]] Result<usize> TryToBase64(Span<const uint8> bytes, Base64Variant variant, Span<char> text) noexcept;

// variant와 다른 알파벳·패딩, 공백 같은 다른 문자, 잘못된 끝 bit는 InvalidArgument다. 출력이 모자라면 CapacityExceeded다.
// 실패하면 bytes 전체를 0으로 지운다.
[[nodiscard]] Result<usize> TryFromBase64(StringView text, Base64Variant variant, Span<uint8> bytes) noexcept;

} // namespace rocks::crypto
