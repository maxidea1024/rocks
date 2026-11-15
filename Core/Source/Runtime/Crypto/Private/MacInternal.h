#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Crypto/Mac.h>
#include <Crypto/Secret.h>

// MAC 키 규칙을 다른 키 파생 경로(Kdf·KeyBlob 등)와 나누는 모듈 내부 함수다. 길이·알고리즘 규칙의
// 출처를 Mac.cpp 하나로 둔다.
namespace rocks::crypto::detail {

// 열거값으로 정의된 알고리즘인가.
bool IsDefinedMacAlgorithm(MacAlgorithm algorithm) noexcept;

// 알고리즘 검사다. 열거값이 아니면 InvalidArgument, 이 기기에서 쓸 수 없으면(AES HW가 없는
// AES-CMAC) Unsupported다.
[[nodiscard]] Result<void> CheckMacUsable(MacAlgorithm algorithm) noexcept;

// 알고리즘의 권장 키 길이(HMAC-SHA-256 32, HMAC-SHA-512 64, BLAKE2b keyed 32, AES-CMAC 32 byte)다.
// 정의되지 않은 값은 0이다.
usize DefaultMacKeyBytes(MacAlgorithm algorithm) noexcept;

// MacKey::TryImport가 받는 키 길이인가(HMAC 16–4096, BLAKE2b keyed 16–64, AES-CMAC 16·32).
// 정의되지 않은 알고리즘은 false다.
bool IsValidMacKeyBytes(MacAlgorithm algorithm, usize bytes) noexcept;

// 길이를 맞춘 키 byte로 MacKey를 만든다(AES-CMAC은 AES 키 스케줄을 키 저장소에 둔다). 호출자가
// CheckMacUsable과 키 길이를 먼저 검사한다(어기면 Fatal). 키 저장소가 찼으면 OutOfMemory다.
[[nodiscard]] Result<MacKey> TryMakeMacKey(MacAlgorithm algorithm, SecretBytes&& key) noexcept;

} // namespace rocks::crypto::detail
