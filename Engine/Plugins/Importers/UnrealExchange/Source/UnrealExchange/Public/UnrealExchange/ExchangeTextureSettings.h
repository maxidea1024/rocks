#pragma once

#include <UnrealExchange/ExchangeSettings.h>

// 텍스처 설정 번역 표다(Docs/Design/UnrealExchange 4.6절 3번 — UE 5.8
// UTexture·UStreamableRenderAsset· 차원 클래스 프로퍼티). 행 번호(표 순서, 0부터)는
// UnsupportedSetting 경고의 detail이다.
namespace rocks {

// 텍스처 표의 행 수다(4.6절 3번 표 — 0–72).
inline constexpr usize ExchangeTextureSettingRowCount = 73;

// 텍스처 다섯 클래스가 함께 쓰는 표다(차원 클래스 키는 행의 classes로 가른다). 정적 수명이다.
ROCKS_UNREALEXCHANGE_API Span<const ExchangeSettingRow> ExchangeTextureSettingRows() noexcept;

} // namespace rocks
