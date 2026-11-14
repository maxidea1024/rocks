#include <Platform/DataIndependentTiming.h>

#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Platform/Cpu.h>

namespace rocks::platform {

#if ROCKS_ARCH_ARM64

namespace {

// 기능 확장(+dit) 없이 어셈블되도록 읽기는 범용 시스템 레지스터 이름(S3_3_C4_C2_5 = DIT), 쓰기는
// MSR(immediate) 명령 인코딩을 쓴다. FEAT_DIT가 없는 CPU에서는 부르지 않는다(정의되지 않은 명령).
bool ReadDit() noexcept {
    uint64 value = 0;
    __asm__ volatile("mrs %0, s3_3_c4_c2_5" : "=r"(value));
    return value != 0;
}

} // namespace

bool IsDataIndependentTimingEnabled() noexcept {
    return QueryCpuFeatures().armDit && ReadDit();
}

bool BeginDataIndependentTiming() noexcept {
    if (!QueryCpuFeatures().armDit || ReadDit()) {
        return false;
    }

    __asm__ volatile(".inst 0xd503415f" ::: "memory"); // msr DIT, #1
    return true;
}

void EndDataIndependentTiming() noexcept {
    if (QueryCpuFeatures().armDit) {
        __asm__ volatile(".inst 0xd503405f" ::: "memory"); // msr DIT, #0
    }
}

#else

bool IsDataIndependentTimingEnabled() noexcept {
    return false;
}

bool BeginDataIndependentTiming() noexcept {
    return false;
}

void EndDataIndependentTiming() noexcept {
}

#endif

} // namespace rocks::platform
