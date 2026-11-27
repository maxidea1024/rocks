#pragma once

#include <UiCore/Styles/ButtonStyle.h>
#include <UiCore/Styles/SliderStyle.h>
#include <UiCore/Styles/TextStyle.h>

namespace rocks::ui {

// UE 5.8 게임 위젯 스타일(UMGCoreStyle.cpp — FUMGCoreStyle) 중 Rocks 형식으로 옮길 수 있는 값이다
// (Docs/Design/UiToolkit 20.11절). 색은 테마를 따르지 않는 고정 선형 색(FUMGColor, 42–91행)이다.
// 게임 Widget(ButtonWidget·SliderWidget)의 기본값이다(UE DefaultStyleCache 25–75행). UE PNG
// 자원으로만 정의된 키(Checkbox·ProgressBar·Scrollbar 등)는 없다 — 부품이 코어 값을 쓴다.

// FUMGColor 값이다(원문 그대로 — White·White25·ForegroundHover가 (0, 1, 1)인 것도 같다).
inline constexpr LinearColor GameSecondaryColor{0.495466f, 0.495466f, 0.495466f, 1.0f};
inline constexpr LinearColor GameInputOutlineColor{0.695111f, 0.695111f, 0.695111f, 1.0f};
inline constexpr LinearColor GameHeaderColor{0.384266f, 0.384266f, 0.384266f, 1.0f};
inline constexpr LinearColor GameHoverColor{0.724268f, 0.724268f, 0.724268f, 1.0f};
inline constexpr LinearColor GameForegroundColor{0.527115f, 0.527115f, 0.527115f, 1.0f};
inline constexpr LinearColor GameForegroundHoverColor{0.0f, 1.0f, 1.0f, 1.0f};
// 슬라이더 막대 색이다(FColor::White, FLinearColor::Gray — Color.cpp 18행).
inline constexpr LinearColor GameSliderBarColor{1.0f, 1.0f, 1.0f, 1.0f};
inline constexpr LinearColor GameSliderDisabledBarColor{0.5f, 0.5f, 0.5f, 1.0f};
// NoBorder 보통·눌림 여백이다(310–311행).
inline constexpr Vec4 GameNoBorderPadding{0.0f, 0.0f, 0.0f, 1.0f};
inline constexpr Vec4 GameNoBorderPressedPadding{0.0f, 1.0f, 0.0f, 0.0f};
// 버튼 brush 크기(Icon32x32)·반경, 슬라이더 막대 두께·손잡이 크기다(407–409·722–730행).
inline constexpr Vec2 GameButtonImageSize{32.0f, 32.0f};
inline constexpr float GameButtonCornerRadius = 4.0f;
inline constexpr float GameSliderBarThickness = 2.0f;
inline constexpr Vec2 GameSliderThumbSize{8.0f, 14.0f};
// 글자 크기다(UMGCoreStyle.h 38–39행 RegularTextSize·SmallTextSize).
inline constexpr float GameRegularTextSize = 10.0f;
inline constexpr float GameSmallTextSize = 8.0f;

// "Button"(407–427행). 비활성 brush는 없다(FSlateNoResource).
ROCKS_UICORE_API const ButtonStyle& GameButtonStyle() noexcept;
// "NoBorder"(307–323행).
ROCKS_UICORE_API const ButtonStyle& GameNoBorderButtonStyle() noexcept;
// "NormalText"(238–243행)·"SmallText"(266–267행). 그림자·강조는 Rocks TextStyle에 칸이 없다.
ROCKS_UICORE_API const TextStyle& GameNormalTextStyle() noexcept;
ROCKS_UICORE_API const TextStyle& GameSmallTextStyle() noexcept;
// "Slider"(722–731행). 손잡이 그림(PNG) 대신 코어 손잡이 brush를 쓴다.
ROCKS_UICORE_API const SliderStyle& GameSliderStyle() noexcept;

} // namespace rocks::ui
