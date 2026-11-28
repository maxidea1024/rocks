#pragma once

#include <Base/Types.h>

// 감시 변경 묶기(Docs/Design/ShaderSystem 16.6절 SSD100)다. 저장 한 번이 사건을 여럿 내고("모두
// 저장"은 파일 여럿), 사건마다 재컴파일하면 같은 저장으로 재컴파일이 여러 번 돈다. 감시 스레드는 첫
// 사건 뒤 조용한 구간까지 사건을 모았다가 한 번 재컴파일한다.
//
// 사용 예(감시 스레드):
//   if (collecting && ShaderWatchSettled(first, last, platform::MonotonicNanoseconds(),
//                                        ShaderWatchSettleMilliseconds,
//                                        ShaderWatchSettleLimitMilliseconds)) {
//       Reload(pending);
//   }
namespace rocks::shader {

// 마지막 사건 뒤 이만큼 새 사건이 없으면 재컴파일한다.
inline constexpr uint32 ShaderWatchSettleMilliseconds = 50;
// 첫 사건 뒤 이만큼 지나면 사건이 계속 와도 재컴파일한다(무한히 미루지 않는다).
inline constexpr uint32 ShaderWatchSettleLimitMilliseconds = 500;
// 사건을 모으는 동안의 Poll 간격이다.
inline constexpr uint32 ShaderWatchSettlePollMilliseconds = 10;

// 모은 사건으로 재컴파일을 시작해도 되는가다: 마지막 사건 뒤 settleMs 동안 조용했거나, 첫 사건 뒤
// limitMs가 지났다. 시각은 같은 단조 시계의 ns이고 first <= last <= now여야 한다(어기면 바로
// 시작해도 된다고 본다 — 기다림이 끝나지 않는 일을 막는다).
constexpr bool ShaderWatchSettled(uint64 firstChangeNanoseconds, uint64 lastChangeNanoseconds,
                                  uint64 nowNanoseconds, uint32 settleMilliseconds,
                                  uint32 limitMilliseconds) noexcept {
    constexpr uint64 NanosecondsPerMillisecond = 1000000;
    if (firstChangeNanoseconds > lastChangeNanoseconds || lastChangeNanoseconds > nowNanoseconds) {
        return true;
    }

    return nowNanoseconds - lastChangeNanoseconds >=
               static_cast<uint64>(settleMilliseconds) * NanosecondsPerMillisecond ||
           nowNanoseconds - firstChangeNanoseconds >=
               static_cast<uint64>(limitMilliseconds) * NanosecondsPerMillisecond;
}

} // namespace rocks::shader
