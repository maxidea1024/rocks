#pragma once

#include <Core/Result.h>
#include <Crypto/PasswordHash.h>

// 비밀번호 비용 검사를 다른 어댑터(KeyBlob 등)와 함께 쓰는 모듈 내부 진입점이다. 구현은
// PasswordHash.cpp다.
namespace rocks::crypto::detail {

// 반복·메모리·반복 × 메모리 상한(MaxPassword*)을 넘는가.
[[nodiscard]] bool IsPasswordCostOverCap(PasswordCost cost) noexcept;

// 범위 밖(0회·8 KiB 미만)은 InvalidArgument, 상한 초과는 CapacityExceeded다.
[[nodiscard]] Result<void> CheckPasswordCost(PasswordCost cost) noexcept;

// 비밀번호 길이가 Argon2id 상한(libsodium PASSWD_MAX) 이하인가. 넘으면 호출자 인자 오류다.
[[nodiscard]] bool IsPasswordLengthValid(Span<const uint8> password) noexcept;

} // namespace rocks::crypto::detail
