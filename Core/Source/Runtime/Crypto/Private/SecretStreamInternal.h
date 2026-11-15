#pragma once

#include <Base/Types.h>

// secretstream 상태(libsodium crypto_secretstream_xchacha20poly1305_state)의 byte 배치다. 구현
// (SecretStream.cpp)이 libsodium 정의와 같은지 static_assert로 고정하고, 시험 접근점
// (SecretStreamTestAccess.h)이 Wycheproof 상태 벡터를 넣고 읽을 때 쓴다. 배치: 하위 키 32 byte ‖
// 카운터 4 byte(little-endian) ‖ inonce 8 byte ‖ 채움 8 byte.
namespace rocks::crypto::detail {

inline constexpr usize SecretStreamStateBytes = 52;
inline constexpr usize SecretStreamStateKeyOffset = 0;
inline constexpr usize SecretStreamStateKeyBytes = 32;
inline constexpr usize SecretStreamStateCounterOffset = 32;
inline constexpr usize SecretStreamStateCounterBytes = 4;
inline constexpr usize SecretStreamStateInonceOffset = 36;
inline constexpr usize SecretStreamStateInonceBytes = 8;

} // namespace rocks::crypto::detail
