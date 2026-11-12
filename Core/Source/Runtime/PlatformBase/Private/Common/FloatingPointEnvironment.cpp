#include <Platform/FloatingPointEnvironment.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace rocks::platform {
namespace detail {

#if defined(__x86_64__) || defined(_M_X64)

// MXCSR: bit 0–5 상태 플래그, 6 DAZ, 7–12 예외 mask(1 = trap 꺼짐), 13–14 반올림 모드, 15 FTZ.
// 기대 값은 상태 플래그를 뺀 0x1F80(모든 mask, nearest, DAZ·FTZ 끔)이다.
constexpr uint64 StatusFlags = 0x3F;
constexpr uint64 Expected = 0x1F80;

uint64 ReadFloatingPointControl() noexcept {
    return _mm_getcsr();
}

void WriteFloatingPointControl(uint64 value) noexcept {
    _mm_setcsr(static_cast<unsigned int>(value));
}

uint64 FlushToZeroBit() noexcept {
    return 0x8000;
}

uint64 DenormalsAreZeroBit() noexcept {
    return 0x40;
}

uint64 RoundTowardZeroBits() noexcept {
    return 0x6000;
}

uint64 StatusFlagBits() noexcept {
    return StatusFlags;
}

bool IsExpected(uint64 control) noexcept {
    return (control & ~StatusFlags) == Expected;
}

// 제어 비트는 target에서, 상태 플래그는 current에서 가져온다.
uint64 MergeControl(uint64 target, uint64 current) noexcept {
    return (target & ~StatusFlags) | (current & StatusFlags);
}

uint64 MakeExpected(uint64 control) noexcept {
    return Expected | (control & StatusFlags);
}

#elif defined(__aarch64__) || defined(_M_ARM64)

// FPCR: bit 0 FIZ, 1 AH, 2 NEP, 8–12·15 예외 trap enable, 13 EBF, 19 FZ16, 22–23 RMode, 24 FZ, 25
// DN, 26 AHP. 모두 0이어야 한다(nearest, 비정규 수 보존, 기본 NaN 끔, 대체 처리·IEEE half 대체 형식
// 끔, trap 꺼짐). 기능이 없는 CPU에서 이 비트들은 RES0이라 0으로 써도 안전하다. 그 밖의 비트는
// 보존한다. 상태 플래그는 FPSR에 따로 있어 FPCR 복원이 건드리지 않는다.
constexpr uint64 Controlled = (1ull << 0) | (1ull << 1) | (1ull << 2) | (0x1Full << 8) |
                              (1ull << 13) | (1ull << 15) | (1ull << 19) | (3ull << 22) |
                              (1ull << 24) | (1ull << 25) | (1ull << 26);

uint64 ReadFloatingPointControl() noexcept {
    uint64 value = 0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

void WriteFloatingPointControl(uint64 value) noexcept {
    __asm__ volatile("msr fpcr, %0" : : "r"(value));
}

uint64 FlushToZeroBit() noexcept {
    return 1ull << 24;
}

uint64 DenormalsAreZeroBit() noexcept {
    return 1ull << 24;
}

uint64 RoundTowardZeroBits() noexcept {
    return 3ull << 22;
}

uint64 StatusFlagBits() noexcept {
    return 0;
}

bool IsExpected(uint64 control) noexcept {
    return (control & Controlled) == 0;
}

uint64 MergeControl(uint64 target, uint64 /*current*/) noexcept {
    return target;
}

uint64 MakeExpected(uint64 control) noexcept {
    return control & ~Controlled;
}

#else
#error "FloatingPointEnvironment: unsupported CPU architecture"
#endif

} // namespace detail

bool IsDeterministicFloatingPointEnvironment() noexcept {
    return detail::IsExpected(detail::ReadFloatingPointControl());
}

bool RunWithDeterministicFloatingPoint(void (*function)(void* context) noexcept,
                                       void* context) noexcept {
    const uint64 saved = detail::ReadFloatingPointControl();
    const bool changed = !detail::IsExpected(saved);
    if (changed) {
        detail::WriteFloatingPointControl(detail::MakeExpected(saved));
    }

    // 포인터를 compiler가 추적할 수 없게 세탁한다. 대상을 모르는 호출은 inline되지 않고(ThinLTO
    // 포함), 그 안의 부동소수점 연산이 위 제어 레지스터 쓰기보다 앞으로 옮겨질 수 없다.
    __asm__ volatile("" : "+r"(function)::"memory");
    function(context);
    __asm__ volatile("" ::: "memory");

    // 제어 설정은 호출 전 값으로(안에서 바뀌었어도), 상태 플래그는 실행 중 쌓인 값으로 둔다.
    const uint64 current = detail::ReadFloatingPointControl();
    const uint64 restored = detail::MergeControl(saved, current);
    if (restored != current) {
        detail::WriteFloatingPointControl(restored);
    }

    return changed;
}

} // namespace rocks::platform
