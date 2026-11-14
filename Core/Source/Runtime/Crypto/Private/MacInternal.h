#pragma once

#include <Base/Types.h>
#include <Crypto/Mac.h>

// MAC 키 규칙을 다른 키 파생 경로(Kdf 등)와 나누는 모듈 내부 함수다. 길이·알고리즘 규칙의 출처를
// Mac.cpp 하나로 둔다.
namespace rocks::crypto::detail {

// 열거값으로 정의된 알고리즘인가.
bool IsDefinedMacAlgorithm(MacAlgorithm algorithm) noexcept;

// 알고리즘의 권장 키 길이(HMAC-SHA-256 32, HMAC-SHA-512 64, BLAKE2b keyed 32 byte)다. 정의되지 않은
// 값은 0이다.
usize DefaultMacKeyBytes(MacAlgorithm algorithm) noexcept;

} // namespace rocks::crypto::detail
