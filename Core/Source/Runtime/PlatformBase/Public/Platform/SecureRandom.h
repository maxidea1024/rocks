#pragma once

#include <Base/Types.h>

// OS의 암호학적으로 안전한 난수 생성기에서 byte를 읽는다. Guid 발급, 세션 키, 예측되면 안 되는
// seed처럼 재현이 필요 없는 무작위 값에 쓴다. 게임플레이·테스트처럼 같은 seed로 재현해야 하는
// 난수는 rocks::RandomStream(Core/Math/RandomStream.h)을 쓴다.
//
// 사용 예:
//   uint8 key[32];
//   if (!platform::TryFillSecureRandom(key, sizeof(key))) { /* OS 난수 실패 */ }
//
// 구현: Windows BCryptGenRandom(시스템 기본 RNG), macOS·Android arc4random_buf(커널 엔트로피).
namespace rocks::platform {

// buffer의 bytes byte를 채운다. 성공하면 true다. bytes가 0이면 아무것도 하지 않고 true다.
// buffer가 nullptr인데 bytes가 0이 아니면 false다. 실패하면 buffer 내용은 정해지지 않는다.
bool TryFillSecureRandom(void* buffer, usize bytes) noexcept;

} // namespace rocks::platform
