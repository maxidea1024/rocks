#pragma once

#include <UnrealExchange/ExchangeSettings.h>

// 정적 메시 설정 번역 표다(Docs/Design/UnrealExchange 5.5절 3–5번). UStaticMesh 덤프(settings)의
// 최상위 키마다 행 하나이고, LOD(SourceModels)·슬롯(StaticMaterials)·섹션 정보·소켓·Nanite 설정은
// 멤버 표로 재귀한다. 번역 결과는 정적 메시 변환(ExchangeStaticMesh.h)이 명세로 옮긴다: 값
// struct(빌드·축소·Nanite·UV 밀도·섹션 정보)는 ImportJson, 나머지는 코드로 읽는다. 경고 detail은 이
// 표의 행 번호다(멤버의 경고는 그 최상위 행 번호).
namespace rocks {

// 표의 행 수다(5.5절 5번 — 명세로 옮김 15, 버림 12, Rocks 필드 없음 21).
inline constexpr usize ExchangeStaticMeshSettingRowCount = 48;

// 표에서 명세로 옮기는 행의 번호다(경고 detail·시험).
enum class ExchangeStaticMeshSettingRow : uint32 {
    SourceModels = 0,
    StaticMaterials = 1,
    SectionInfoMap = 2,
    Sockets = 3,
    LightMapResolution = 4,
    LightMapCoordinateIndex = 5,
    PositiveBoundsExtension = 6,
    NegativeBoundsExtension = 7,
    AutoComputeLodScreenSize = 8,
    LodGroup = 9,
    MinLod = 10,
    NumStreamedLods = 11,
    NaniteSettings = 12,
    LodForCollision = 13,
    ComplexCollisionMesh = 14
};

// 정적 메시 표다(정적 수명).
ROCKS_UNREALEXCHANGE_API Span<const ExchangeSettingRow> ExchangeStaticMeshSettingRows() noexcept;

} // namespace rocks
