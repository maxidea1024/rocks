#include "../Common/CpuInternal.h"

#if ROCKS_ARCH_ARM64

#include <sys/sysctl.h>
#include <sys/types.h>

namespace rocks::platform::detail {
namespace {

// hw.optional.arm.FEAT_* 는 macOS 12·iOS 15부터 있다. 값이 1이면 지원한다. 이름이 없으면(구버전 OS)
// 실패한다.
bool QueryFeature(const char* name, bool& supported) noexcept {
    int value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0 || size != sizeof(value)) {
        return false;
    }

    supported = value == 1;
    return true;
}

} // namespace

void DetectArm64CryptoFeatures(CpuFeatures& features) noexcept {
    bool aes = false;
    bool pmull = false;
    bool sha2 = false;
    const bool queried = QueryFeature("hw.optional.arm.FEAT_AES", aes) &&
                         QueryFeature("hw.optional.arm.FEAT_PMULL", pmull) &&
                         QueryFeature("hw.optional.arm.FEAT_SHA256", sha2);
    if (!queried) {
        // sysctl 이름이 없는 OS에서는 대상 triple이 보장하는 기준 기능으로 판정한다. arm64-apple
        // 대상은 Apple Silicon 전부가 갖춘 AES·PMULL·SHA2를 컴파일러가 기준 기능으로 정의한다.
#if defined(__ARM_FEATURE_AES)
        aes = true;
        pmull = true;
#endif
#if defined(__ARM_FEATURE_SHA2)
        sha2 = true;
#endif
    }

    features.armAes = aes;
    features.armPmull = pmull;
    features.armSha2 = sha2;
}

} // namespace rocks::platform::detail

#endif
