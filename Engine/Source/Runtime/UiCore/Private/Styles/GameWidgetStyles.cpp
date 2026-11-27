#include <UiCore/Styles/GameWidgetStyles.h>

#include <UiCore/Styles/CoreStyles.h>

// UE 5.8 UMGCoreStyle.cpp 값이다(Docs/Design/UiToolkit 20.11절). 줄 번호는 그 파일 기준이다.
namespace rocks::ui {

namespace {

StyleBrush GameButtonBrush(LinearColor fill, LinearColor outline) noexcept {
    StyleBrush brush =
        StyleBrush::RoundedBox(fill, GameButtonCornerRadius, outline, ControlOutlineThickness);
    brush.imageSize = GameButtonImageSize;
    return brush;
}

} // namespace

const ButtonStyle& GameButtonStyle() noexcept {
    // 407–409행: RB(Secondary, 4, InputOutline, 1)·RB(Hover, 4, Hover, 1)·RB(Header, 4, Hover, 1),
    // 416–425행: 전경 ForegroundHover(비활성 Foreground), 두 여백 ButtonMargins.
    static const ButtonStyle style{GameButtonBrush(GameSecondaryColor, GameInputOutlineColor),
                                   GameButtonBrush(GameHoverColor, GameHoverColor),
                                   GameButtonBrush(GameHeaderColor, GameHoverColor),
                                   StyleBrush::None(),
                                   ButtonPadding,
                                   ButtonPadding,
                                   ColorRef::Specified(GameForegroundHoverColor),
                                   ColorRef::Specified(GameForegroundHoverColor),
                                   ColorRef::Specified(GameForegroundHoverColor),
                                   ColorRef::Specified(GameForegroundColor)};
    return style;
}

const ButtonStyle& GameNoBorderButtonStyle() noexcept {
    // 307–312행: brush 없음, 여백 (0,0,0,1)·(0,1,0,0), 전경은 상속이다.
    static const ButtonStyle style = [] {
        ButtonStyle noBorder;
        noBorder.normalPadding = {0.0f, 0.0f, 0.0f, 1.0f};
        noBorder.pressedPadding = {0.0f, 1.0f, 0.0f, 0.0f};
        return noBorder;
    }();
    return style;
}

const TextStyle& GameNormalTextStyle() noexcept {
    static const TextStyle style{{Typeface::Regular, GameRegularTextSize},
                                 {ColorSource::Foreground}};
    return style;
}

const TextStyle& GameSmallTextStyle() noexcept {
    static const TextStyle style{{Typeface::Regular, GameSmallTextSize}, {ColorSource::Foreground}};
    return style;
}

const SliderStyle& GameSliderStyle() noexcept {
    // 724–730행: 막대 흰색(비활성 회색), 두께 2, 손잡이 8×14.
    static const SliderStyle style = [] {
        SliderStyle slider = DefaultSliderStyle();
        slider.bar = StyleBrush::Color(GameSliderBarColor);
        slider.barHovered = slider.bar;
        slider.barDisabled = StyleBrush::Color(GameSliderDisabledBarColor);
        slider.barThickness = GameSliderBarThickness;
        slider.thumbSize = GameSliderThumbSize;
        return slider;
    }();
    return style;
}

} // namespace rocks::ui
