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
    // Apple은 DIT를 켠 뒤 speculation barrier를 요구한다("Writing ARM64 code for Apple platforms"의
    // DIT 절). 없으면 뒤따르는 비밀 의존 명령이 DIT가 꺼진 상태로 투기 실행될 수 있다. Arm ARM은
    // PSTATE 쓰기에 barrier를 요구하지 않지만 감지가 macOS뿐이라 언제나 넣는다. 끌 때는 넣지
    // 않는다.
    if (QueryCpuFeatures().armSb) {
        __asm__ volatile(".inst 0xd50330ff" ::: "memory"); // sb
    } else {
        __asm__ volatile("dsb nsh\n\tisb" ::: "memory");
    }

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
