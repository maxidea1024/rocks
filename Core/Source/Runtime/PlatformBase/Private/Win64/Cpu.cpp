#include "../Common/CpuInternal.h"

#include <Base/Compiler.h>

#if ROCKS_ARCH_ARM64

#include <Windows.h>

namespace rocks::platform::detail {

// Windows ARM64는 Crypto Extensions를 한 비트로 알려 준다(AES·PMULL·SHA1·SHA2를 함께 뜻한다). 개별
// 비트가 없으므로 셋을 같은 값으로 둔다. Windows ARM64는 아직 지원 대상이 아니라 빌드·실행 검증
// 전이다.
void DetectArm64CryptoFeatures(CpuFeatures& features) noexcept {
    const bool crypto = IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) != 0;
    features.armAes = crypto;
    features.armPmull = crypto;
    features.armSha2 = crypto;
}

} // namespace rocks::platform::detail

#endif
