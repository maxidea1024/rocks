#pragma once

#include <Base/Guid.h>
#include <Core/Result.h>

// 키 식별자다(Docs/Design/Crypto.ko.md 10.3절 KeyRing, 10.4절 KeyBlob). 형식은 키 ID를 기록하고
// 키를 ID로 찾는다(2절, 시도 복호를 하지 않는다).
//
// - 형식에는 RFC 9562 byte 순서(BE64(high) ‖ BE64(low))로 쓴다. Core Guid 표기·세그먼트 헤더와
//   같다.
// - 모든 bit가 0인 Nil은 "없음"이다. 기본 생성한 KeyId를 실수로 쓰면 모든 키가 같은 ID가 되므로
//   형식(KeyBlob 등)은 Nil을 받지 않는다.
//
// 사용 예:
//   auto id = crypto::TryNewKeyId();
//   if (!id) { return id.Error(); }
namespace rocks::crypto {

struct KeyId {
    Guid value;

    [[nodiscard]] constexpr bool IsNil() const noexcept {
        return value.high == 0 && value.low == 0;
    }

    friend constexpr bool operator==(const KeyId&, const KeyId&) noexcept = default;
};

// 무작위 KeyId(UUID v4, Nil이 나오지 않는다)다. OS 난수 실패는 DependencyFailed다.
[[nodiscard]] Result<KeyId> TryNewKeyId() noexcept;

} // namespace rocks::crypto
