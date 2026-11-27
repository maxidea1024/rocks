#include <Core/Assert.h>
#include <CurveAssets/FloatCurveAsset.h>

// 스칼라 커브 에셋의 채널이다(Docs/Design/CurveAssets 2.2절).
namespace rocks {

namespace {

// 채널 이름이다(UE GetCurves — Rocks 에셋 객체에는 이름이 없어 고정 이름이다).
constexpr StringView ScalarChannelName = "Value";

} // namespace

usize FloatCurveAsset::ChannelCount() const noexcept {
    return 1;
}

const TangentCurve& FloatCurveAsset::Channel(usize index) const noexcept {
    ROCKS_ASSERT(index == 0);
    (void)index;
    return curve;
}

TangentCurve& FloatCurveAsset::MutableChannel(usize index) noexcept {
    ROCKS_ASSERT(index == 0);
    (void)index;
    return curve;
}

StringView FloatCurveAsset::ChannelName(usize index) const noexcept {
    return index == 0 ? ScalarChannelName : StringView();
}

} // namespace rocks
