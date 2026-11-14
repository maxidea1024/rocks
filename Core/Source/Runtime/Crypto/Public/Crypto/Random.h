#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>

// 암호학적으로 안전한 난수다(Docs/Design/Crypto.ko.md 5.1절 "난수 단일 경로"). 키·nonce·salt처럼
// 예측되면 안 되는 값에 쓴다. 재현이 필요한 게임플레이 난수는 rocks::RandomStream을 쓴다.
//
// 구현은 OS CSPRNG 하나다(PlatformBase SecureRandom: Windows BCryptGenRandom, macOS·Android
// arc4random_buf). libsodium 내부의 난수(키 생성 등)도 같은 경로로 바꿔 둔다. 사용자 공간 DRBG는
// 두지 않는다.
//
// 사용 예:
//   uint8 salt[32];
//   if (auto filled = crypto::TryFillRandom(Span<uint8>(salt)); !filled) { return filled.Error(); }
//   auto die = crypto::TryRandomUniform(6);   // 0–5 균등
namespace rocks::crypto {

// output을 난수로 채운다. 빈 Span은 성공이다. OS 난수가 실패하면 DependencyFailed이고 output 전체를
// 0으로 지운다. (libsodium 내부 난수 경로는 실패를 돌려줄 수 없어 Fatal이다. 이 함수는 호출자가
// 처리할 수 있게 오류로 돌려준다.)
[[nodiscard]] Result<void> TryFillRandom(Span<uint8> output) noexcept;

// [0, upperBound) 균등 분포 정수다(나머지 편향 없음, 거부 표본추출). upperBound가 0이면
// InvalidArgument다.
[[nodiscard]] Result<uint32> TryRandomUniform(uint32 upperBound) noexcept;

} // namespace rocks::crypto
