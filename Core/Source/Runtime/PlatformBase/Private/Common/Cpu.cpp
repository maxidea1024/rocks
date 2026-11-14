#include <Platform/Cpu.h>

#if ROCKS_ARCH_X64
#include <cpuid.h>
#endif

namespace rocks::platform {
namespace {

CpuFeatures Detect() noexcept {
    CpuFeatures features;
#if ROCKS_ARCH_X64
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return features;
    }

    features.sse42 = (ecx & (1u << 20)) != 0;
    features.popcnt = (ecx & (1u << 23)) != 0;
    const bool cpuAvx = (ecx & (1u << 28)) != 0;
    const bool cpuFma = (ecx & (1u << 12)) != 0;
    const bool osSaves = (ecx & (1u << 27)) != 0; // OSXSAVE

    // OS가 XMM·YMM 상태를 문맥 전환 때 저장해야 AVX를 쓸 수 있다(XCR0 bit 1·2).
    bool ymmEnabled = false;
    if (osSaves) {
        unsigned low = 0;
        unsigned high = 0;
        __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
        ymmEnabled = (low & 0x6u) == 0x6u;
    }

    // FMA·AVX2는 VEX 인코딩과 YMM 레지스터를 쓰므로 AVX를 쓸 수 있어야 한다. 일부 가상 머신은 AVX
    // bit만 가리고 AVX2·FMA bit를 남기므로 AVX 판정에 묶는다(avx2·fma가 켜지면 avx도 켜져 있다).
    features.avx = cpuAvx && ymmEnabled;
    features.fma = cpuFma && features.avx;
    unsigned maxLeaf = __get_cpuid_max(0, nullptr);
    if (maxLeaf >= 7) {
        __cpuid_count(7, 0, eax, ebx, ecx, edx);
        features.avx2 = (ebx & (1u << 5)) != 0 && features.avx;
        features.bmi2 = (ebx & (1u << 8)) != 0;
    }
#else
    // ARM64는 Advanced SIMD(NEON)가 기본 명령이다.
    features.neon = true;
#endif
    return features;
}

} // namespace

const CpuFeatures& QueryCpuFeatures() noexcept {
    // 함수 안 static은 C++11부터 스레드 안전하게 한 번만 초기화된다.
    static const CpuFeatures features = Detect();
    return features;
}

} // namespace rocks::platform
