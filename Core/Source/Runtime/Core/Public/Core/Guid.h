#pragma once

#include <Base/Guid.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>

// Guid(128-bit 고유 식별자) 발급과 텍스트 표기다(UE FGuid::NewGuid·ToString·Parse 대응).
//
// 사용 예:
//   auto id = TryNewGuid();                                   // 무작위 UUID v4
//   auto text = TryFormat(allocator, "{}", id.Value());
//   // text: "3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7b"
//   auto parsed = TryParseGuid("3F2A9C1E-7B4D-4E8F-9A01-2C3D4E5F6A7B");
//
// 표기: RFC 9562(UUID) 표준 문자열 "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"(16진 소문자). Guid의
// high가 앞 16자리, low가 뒤 16자리다. byte 순서는 표준 문자열의 순서와 같다(앞 byte가 high의
// 최상위 byte).
namespace rocks {

// 무작위 UUID v4를 발급한다(122 bit 무작위, version 4·variant 10xx 표시). OS 보안 난수를 쓰며
// 실패하면 Unsupported다. 같은 값이 다시 나올 확률은 무시할 수 있지만 중복 검사는 하지 않는다.
Result<Guid> TryNewGuid() noexcept;

// 문자열을 Guid로 읽는다. 받는 형식(16진은 대소문자 무관):
//   "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"(36자, 서식 출력과 같음)
//   "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"(32자, 구분자 없음 — UE EGuidFormats::Digits)
// 그 밖(중괄호·공백·다른 길이)은 InvalidArgument다.
Result<Guid> TryParseGuid(StringView text) noexcept;

// "{}"는 36자 소문자 표준 표기다. spec("{:...}")은 받지 않는다.
template <>
struct Formatter<Guid> {
    static Result<void> Format(StringBuilderBase& out, const Guid& value, StringView spec) noexcept;
};

} // namespace rocks
