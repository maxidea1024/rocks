#pragma once

#include <UnrealExchange/ExchangeSettings.h>

// 스킨 메시 설정 번역 표다(Docs/Design/UnrealExchange 6.4절 2–4번). USkeletalMesh 덤프(settings)의
// 최상위 키마다 행 하나이고, LOD(SourceModels)·소켓·가중치 프로필·Nanite 설정은 멤버 표로
// 재귀한다. 경고 detail은 이 표의 행 번호다(멤버의 경고는 그 최상위 행 번호). data.materials(6.3절
// 13번)의 슬롯 번역 경고는 ExchangeSkeletalMeshMaterialsDetail이다.
namespace rocks {

// 표의 행 수다(명세로 옮김 8, 버림 16, Rocks 필드 없음 30).
inline constexpr usize ExchangeSkeletalMeshSettingRowCount = 54;

// data.materials 번역 경고의 detail이다(settings 표 밖 — 행 수와 같은 값).
inline constexpr uint64 ExchangeSkeletalMeshMaterialsDetail = ExchangeSkeletalMeshSettingRowCount;

// 표에서 명세로 옮기는 행의 번호다.
enum class ExchangeSkeletalMeshSettingRow : uint32 {
    SourceModels = 0,
    Skeleton = 1,
    PositiveBoundsExtension = 2,
    NegativeBoundsExtension = 3,
    MinLod = 4,
    NaniteSettings = 5,
    Sockets = 6,
    SkinWeightProfiles = 7
};

ROCKS_UNREALEXCHANGE_API Span<const ExchangeSettingRow> ExchangeSkeletalMeshSettingRows() noexcept;

// data 객체에서 materials만 슬롯으로 번역하는 표다(bones·lods는 버림).
ROCKS_UNREALEXCHANGE_API Span<const ExchangeSettingRow> ExchangeSkeletalMeshDataRows() noexcept;

} // namespace rocks
