#include <Core/Assert.h>
#include <CurveAssets/ColorCurveAsset.h>

// 색 커브 에셋의 조정 값·채널이다(Docs/Design/CurveAssets 2.4절).
namespace rocks {

namespace {

constexpr usize ColorChannelCount = 4;
constexpr StringView ColorChannelNames[ColorChannelCount] = {"R", "G", "B", "A"};

} // namespace

ColorCurveAdjustments ColorCurveAsset::Adjustments() const noexcept {
    ColorCurveAdjustments adjustments;
    adjustments.hue = adjustHue;
    adjustments.saturation = adjustSaturation;
    adjustments.brightness = adjustBrightness;
    adjustments.brightnessCurve = adjustBrightnessCurve;
    adjustments.vibrance = adjustVibrance;
    adjustments.minAlpha = adjustMinAlpha;
    adjustments.maxAlpha = adjustMaxAlpha;
    return adjustments;
}

usize ColorCurveAsset::ChannelCount() const noexcept {
    return ColorChannelCount;
}

const TangentCurve& ColorCurveAsset::Channel(usize index) const noexcept {
    ROCKS_ASSERT(index < ColorChannelCount);
    return index == 0 ? r : (index == 1 ? g : (index == 2 ? b : a));
}

TangentCurve& ColorCurveAsset::MutableChannel(usize index) noexcept {
    ROCKS_ASSERT(index < ColorChannelCount);
    return index == 0 ? r : (index == 1 ? g : (index == 2 ? b : a));
}

StringView ColorCurveAsset::ChannelName(usize index) const noexcept {
    return index < ColorChannelCount ? ColorChannelNames[index] : StringView();
}

} // namespace rocks
