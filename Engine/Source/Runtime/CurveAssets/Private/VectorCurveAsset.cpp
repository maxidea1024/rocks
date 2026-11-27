#include <Core/Assert.h>
#include <CurveAssets/VectorCurveAsset.h>

// 벡터 커브 에셋의 평가·채널이다(Docs/Design/CurveAssets 2.3절).
namespace rocks {

namespace {

constexpr usize VectorChannelCount = 3;
constexpr StringView VectorChannelNames[VectorChannelCount] = {"X", "Y", "Z"};

} // namespace

Vec3d VectorCurveAsset::Evaluate(float time) const noexcept {
    return Vec3d(x.Evaluate(time), y.Evaluate(time), z.Evaluate(time));
}

usize VectorCurveAsset::ChannelCount() const noexcept {
    return VectorChannelCount;
}

const TangentCurve& VectorCurveAsset::Channel(usize index) const noexcept {
    ROCKS_ASSERT(index < VectorChannelCount);
    return index == 0 ? x : (index == 1 ? y : z);
}

TangentCurve& VectorCurveAsset::MutableChannel(usize index) noexcept {
    ROCKS_ASSERT(index < VectorChannelCount);
    return index == 0 ? x : (index == 1 ? y : z);
}

StringView VectorCurveAsset::ChannelName(usize index) const noexcept {
    return index < VectorChannelCount ? VectorChannelNames[index] : StringView();
}

} // namespace rocks
