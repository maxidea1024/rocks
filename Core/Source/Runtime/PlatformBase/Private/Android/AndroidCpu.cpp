#include "../Common/CpuInternal.h"

#if ROCKS_ARCH_ARM64

#include <asm/hwcap.h>
#include <sys/auxv.h>

namespace rocks::platform::detail {

// Linux 커널이 알려 주는 AT_HWCAP bit다(arch/arm64 uapi hwcap.h: AES bit 3, PMULL bit 4, SHA2 bit
// 6).
void DetectArm64CryptoFeatures(CpuFeatures& features) noexcept {
    const unsigned long hwcap = getauxval(AT_HWCAP);
    features.armAes = (hwcap & HWCAP_AES) != 0;
    features.armPmull = (hwcap & HWCAP_PMULL) != 0;
    features.armSha2 = (hwcap & HWCAP_SHA2) != 0;
}

} // namespace rocks::platform::detail

#endif
