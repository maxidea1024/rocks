#include <Base/Compiler.h>
#include <Platform/Cpu.h>
#include <Platform/SecureRandom.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>
#include <Platform/PlatformMath.h>

using namespace rocks;

TEST(Platform_CpuFeaturesAreConsistent) {
    const platform::CpuFeatures& features = platform::QueryCpuFeatures();
    // 두 번째 호출은 같은 객체를 돌려준다.
    CHECK(&features == &platform::QueryCpuFeatures());
    if constexpr (IsArm64) {
        CHECK(features.neon && !features.avx && !features.sse42);
        CHECK(!features.aes && !features.pclmulqdq && !features.sha);
        // FEAT_PMULL은 FEAT_AES가 있어야 정의된다(Arm ARM). 반대 방향은 보장하지 않는다.
        CHECK(!features.armPmull || features.armAes);
        if constexpr (IsMac) {
            // Apple Silicon은 모두 AES·PMULL·SHA2를 갖춘다.
            CHECK(features.armAes && features.armPmull && features.armSha2);
        }
    } else {
        CHECK(!features.neon);
        CHECK(!features.armAes && !features.armPmull && !features.armSha2);
        // AVX2·FMA는 OS가 YMM 상태를 저장할 때만 true이므로 AVX 없이 켜질 수 없다.
        CHECK(!features.avx2 || features.avx);
        CHECK(!features.fma || features.avx);
    }

    printf("  cpu: sse4.2=%d popcnt=%d avx=%d avx2=%d fma=%d bmi2=%d aes=%d pclmulqdq=%d sha=%d "
           "neon=%d "
           "armAes=%d armPmull=%d armSha2=%d\n",
           features.sse42, features.popcnt, features.avx, features.avx2, features.fma,
           features.bmi2, features.aes, features.pclmulqdq, features.sha, features.neon,
           features.armAes, features.armPmull, features.armSha2);
}

TEST(Platform_SecureRandomFillsBuffers) {
    CHECK(platform::TryFillSecureRandom(nullptr, 0));
    CHECK(!platform::TryFillSecureRandom(nullptr, 1));

    uint8 first[64] = {};
    uint8 second[64] = {};
    REQUIRE(platform::TryFillSecureRandom(first, sizeof(first)));
    REQUIRE(platform::TryFillSecureRandom(second, sizeof(second)));
    // 512 bit가 우연히 같거나 모두 0일 확률은 무시할 수 있다.
    CHECK(memcmp(first, second, sizeof(first)) != 0);
    bool nonZero = false;
    for (uint8 value : first) {
        nonZero = nonZero || value != 0;
    }

    CHECK(nonZero);

    // byte 분포가 극단적으로 치우치지 않는지 대략 확인한다(4096 byte의 bit 1 비율이 40–60%).
    uint8 large[4096];
    REQUIRE(platform::TryFillSecureRandom(large, sizeof(large)));
    usize ones = 0;
    for (uint8 value : large) {
        ones += static_cast<usize>(platform::PlatformMath::PopCount32(value));
    }

    CHECK(ones > sizeof(large) * 8 * 4 / 10 && ones < sizeof(large) * 8 * 6 / 10);
}
