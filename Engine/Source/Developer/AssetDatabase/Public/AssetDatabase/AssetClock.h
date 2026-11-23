#pragma once

#include <Base/Types.h>

// AssetDatabase가 쓰는 시각의 원천이다(Docs/Design/AssetSystem 7.1절 9 — A2a). 감시 루트
// 재확인(1초), 쓰는 중 파일 대기(최대 10초), 원본 debounce(500 ms), 별칭 생성 UTC·만료가 모두 이
// 시계를 쓴다. 시험은 고정 시각을 주는 구현을 끼워 시간 규칙을 결정적으로 단언한다(sleep 없음).
//
// 사용 예:
//   SystemAssetClock clock;
//   const int64 now = clock.NowTicks();   // 0001-01-01 UTC부터의 100 ns tick
namespace rocks {

// 1초의 tick 수다(100 ns 단위).
inline constexpr int64 AssetClockTicksPerSecond = 10000000;
// 1 ms의 tick 수다.
inline constexpr int64 AssetClockTicksPerMillisecond = AssetClockTicksPerSecond / 1000;

class AssetClock {
public:
    // 지금 UTC 시각이다(0001-01-01 00:00:00 UTC부터의 100 ns tick — rocks::DateTime·
    // platform::UtcTicks와 같은 표현). 벽시계라 뒤로 갈 수 있다.
    virtual int64 NowTicks() noexcept = 0;

protected:
    ~AssetClock() = default;
};

// platform::UtcTicks를 읽는 시계다.
class ROCKS_ASSETDATABASE_API SystemAssetClock final : public AssetClock {
public:
    int64 NowTicks() noexcept override;
};

} // namespace rocks
