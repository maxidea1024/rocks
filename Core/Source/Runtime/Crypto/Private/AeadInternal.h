#pragma once

#include <Core/Result.h>
#include <Crypto/Aead.h>
#include <Crypto/Secret.h>

// 다른 어댑터(KeyExchange 등)가 AeadKey를 만들 때 쓰는 모듈 내부 진입점이다. 구현은 Aead.cpp다.
namespace rocks::crypto::detail {

// 알고리즘 검사다. 열거값이 아니면 InvalidArgument, 이 기기에서 쓸 수 없으면 Unsupported다.
[[nodiscard]] Result<void> CheckAeadUsable(AeadAlgorithm algorithm) noexcept;

// 길이를 맞춘 키 byte로 AeadKey를 만든다(AES-256-GCM은 키 확장 상태를 키 저장소에 둔다). 호출자가
// CheckAeadUsable과 키 길이를 먼저 검사한다. 키 저장소가 찼으면 OutOfMemory다.
[[nodiscard]] Result<AeadKey> TryMakeAeadKey(AeadAlgorithm algorithm, SecretBytes&& key) noexcept;

} // namespace rocks::crypto::detail
