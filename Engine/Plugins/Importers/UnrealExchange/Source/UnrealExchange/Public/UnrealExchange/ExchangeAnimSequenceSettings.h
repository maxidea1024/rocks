#pragma once

#include <UnrealExchange/ExchangeSettings.h>

// 애니메이션 클립 설정 번역 표다(Docs/Design/UnrealExchange 7.2절 4번). UAnimSequence
// 덤프(settings)의 최상위 키마다 행 하나다. 경고 detail은 이 표의 행 번호다.
namespace rocks {

// 표의 행 수다(명세로 옮김 15, 버림 25, Rocks 필드 없음 8).
inline constexpr usize ExchangeAnimSequenceSettingRowCount = 48;

// 표에서 명세로 옮기는 행의 번호다.
enum class ExchangeAnimSequenceSettingRow : uint32 {
    Skeleton = 0,
    AdditiveAnimType = 1,
    RefPoseType = 2,
    RefFrameIndex = 3,
    RefPoseSeq = 4,
    RetargetSource = 5,
    Interpolation = 6,
    EnableRootMotion = 7,
    RootMotionRootLock = 8,
    ForceRootLock = 9,
    UseNormalizedRootMotionScale = 10,
    RateScale = 11,
    Loop = 12,
    PlatformTargetFrameRate = 13,
    AttributeCurves = 14
};

ROCKS_UNREALEXCHANGE_API Span<const ExchangeSettingRow> ExchangeAnimSequenceSettingRows() noexcept;

} // namespace rocks
