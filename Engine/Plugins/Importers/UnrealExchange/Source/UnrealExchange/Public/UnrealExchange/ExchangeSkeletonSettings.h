#pragma once

#include <UnrealExchange/ExchangeSettings.h>

// 스켈레톤 설정 번역 표다(Docs/Design/UnrealExchange 6.4절 1번). USkeleton 덤프(settings)의 최상위
// 키마다 행 하나다. 경고 detail은 이 표의 행 번호다.
namespace rocks {

// 표의 행 수다(명세로 옮김 2, 코드가 봄 1, 버림 6, Rocks 필드 없음 6).
inline constexpr usize ExchangeSkeletonSettingRowCount = 15;

// 표에서 명세로 옮기거나 코드가 보는 행의 번호다.
enum class ExchangeSkeletonSettingRow : uint32 {
    VirtualBones = 0,
    Sockets = 1,
    AssetUserData = 2
};

ROCKS_UNREALEXCHANGE_API Span<const ExchangeSettingRow> ExchangeSkeletonSettingRows() noexcept;

} // namespace rocks
