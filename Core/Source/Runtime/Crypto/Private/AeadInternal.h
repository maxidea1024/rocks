#pragma once

#include <Core/Result.h>
#include <Crypto/Aead.h>
#include <Crypto/Crypto.h>
#include <Crypto/Secret.h>
#include <Platform/Cpu.h>

// 다른 어댑터(KeyExchange 등)가 AeadKey를 만들 때 쓰는 모듈 내부 진입점이다. 구현은 Aead.cpp다.
namespace rocks::crypto::detail {

// 알고리즘 검사다. 열거값이 아니면 InvalidArgument, 이 기기에서 쓸 수 없으면 Unsupported다.
[[nodiscard]] Result<void> CheckAeadUsable(AeadAlgorithm algorithm) noexcept;

// 길이를 맞춘 키 byte로 AeadKey를 만든다(AES-256-GCM은 키 확장 상태를 키 저장소에 둔다). 호출자가
// CheckAeadUsable과 키 길이를 먼저 검사한다(어기면 Fatal). 키 저장소가 찼으면 OutOfMemory다.
[[nodiscard]] Result<AeadKey> TryMakeAeadKey(AeadAlgorithm algorithm, SecretBytes&& key) noexcept;

// libsodium AES 경로(AES-256-GCM·AEGIS)의 HW 판정이다. 두 알고리즘이 같은 조건을 쓴다(6.1절 "HW
// 조건은 하나다"). 값을 받으므로 시험이 HW 없는 경로를 재현할 수 있다(11절 결정성). 공개 경로는
// QueryCpuFeatures()·GetCryptoRuntimeInfo()·crypto_aead_aes256gcm_is_available()을 넘긴다.
// - cpu: HasAesInstructions(Aes/AesKernels.h)를 채워야 한다. x86-64는 AES-NI·PCLMULQDQ·AVX, ARM64는
//   AES·PMULL이다. libsodium은 ARM에서 AES bit 하나만 보므로 PMULL은 CpuFeatures로 본다. PMULL 없이
//   AES-256-GCM 키를 만들면 beforenm이 PMULL 명령에서 죽는다.
// - sodium: libsodium 실행 감지(sodium_runtime_has_*)도 모두 참이어야 한다(6.2절). x86의 실행 감지
//   값은 빌드 정의가 있을 때만 켜진다.
// - sodiumAesGcmAvailable: crypto_aead_aes256gcm_is_available()이다. libsodium이 HW 구현으로
//   빌드되었을 때만 참이다. ARM에서는 HAVE_ARMCRYPTO 빌드 정의를 뜻하고, 이는 AEGIS armcrypto
//   구현의 컴파일 조건과 같다(ARM의 실행 감지는 빌드 정의와 무관해서 따로 확인해야 한다).
// 조건이 아니면 libsodium soft 경로로 내려가지 않고 Unsupported다(결정 D3).
[[nodiscard]] ROCKS_CRYPTO_API bool HasAesHardware(const platform::CpuFeatures& cpu,
                                                   const CryptoRuntimeInfo& sodium,
                                                   bool sodiumAesGcmAvailable) noexcept;

// 이 프로세스에서 AES 계열(AES-128/256-GCM, AEGIS, hazmat AES 모드, AES-CMAC)을 쓸 수 있는가.
// libsodium HW 판정(HasAesHardware를 감지 결과로 부른 값)과 AES 경량 계층 커널 표(감지·자가 시험을
// 거친 GetAesKernels())가 모두 참일 때만 참이다. 둘 중 하나만 되는 기기에서 AES 일부만 쓰이는
// 상태를 없앤다(6.1절 "HW 조건은 하나다"). 결과는 프로세스 동안 같다.
[[nodiscard]] ROCKS_CRYPTO_API bool IsAesAvailable() noexcept;

} // namespace rocks::crypto::detail
