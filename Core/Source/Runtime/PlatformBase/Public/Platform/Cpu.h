#pragma once

#include <Base/Compiler.h>

// 실행 중인 CPU의 명령어 확장 지원 여부다. 빌드는 기준 명령(x64: SSE2, ARM64: NEON)만 쓰고, 더 넓은
// 명령(AVX2 등)을 쓰는 경로는 실행 시 이 값으로 고른다.
//
// 사용 예:
//   const platform::CpuFeatures& cpu = platform::QueryCpuFeatures();
//   if (cpu.avx2 && cpu.fma) { RunAvx2Path(); } else { RunBaselinePath(); }
namespace rocks::platform {

struct CpuFeatures {
    // x64
    bool sse42 = false;
    bool popcnt = false;
    // AVX 계열은 CPU 지원과 OS의 YMM 상태 저장 지원(XGETBV)이 모두 있을 때만 true다.
    bool avx = false;
    bool avx2 = false;
    bool fma = false;
    bool bmi2 = false;
    // ARM64 기준 SIMD다(ARM64에서는 항상 true).
    bool neon = false;
};

// 처음 호출할 때 한 번 조사하고 이후 같은 값을 돌려준다. 여러 스레드에서 동시에 불러도 된다.
const CpuFeatures& QueryCpuFeatures() noexcept;

} // namespace rocks::platform
