#pragma once

// libsodium 초기화 지점이다. libsodium을 부르는 모든 어댑터 함수는 처음에 EnsureSodium()을
// 부른다(Crypto.h 초기화 계약).
namespace rocks::crypto::detail {

// 처음 부를 때 libsodium을 초기화하고, 이후에는 이미 끝난 초기화를 기다리지 않고 바로 돌아온다.
// 여러 스레드에서 동시에 불러도 된다(GetCryptoRuntimeInfo의 함수 안 static 초기화가 blocking
// once다).
void EnsureSodium() noexcept;

} // namespace rocks::crypto::detail
