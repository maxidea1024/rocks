#include <CurveAssets/CurveAsset.h>

// 커브 에셋 기반의 범위·초기화다(Docs/Design/CurveAssets 2.1절).
namespace rocks {

namespace {

// 키 있는 채널들의 범위 합집합이다. rangeOf는 채널 하나의 범위(Result<Interval<float>>)다.
template <class RangeOf>
Result<Interval<float>> UnionOfChannels(const CurveAsset& asset, RangeOf&& rangeOf) noexcept {
    bool any = false;
    Interval<float> merged;
    for (usize index = 0; index < asset.ChannelCount(); ++index) {
        const TangentCurve& channel = asset.Channel(index);
        if (channel.KeyCount() == 0) {
            continue;
        }

        auto range = rangeOf(channel);
        if (!range) {
            return range;
        }

        if (!any) {
            merged = range.Value();
            any = true;
            continue;
        }

        merged.min = range.Value().min < merged.min ? range.Value().min : merged.min;
        merged.max = range.Value().max > merged.max ? range.Value().max : merged.max;
    }

    if (!any) {
        return Result<Interval<float>>::Failure(ErrorCode::NotFound);
    }

    return Result<Interval<float>>::Success(merged);
}

} // namespace

Result<Interval<float>> CurveAsset::TryGetTimeRange() const noexcept {
    return UnionOfChannels(
        *this, [](const TangentCurve& channel) noexcept { return channel.TryGetTimeRange(); });
}

Result<Interval<float>> CurveAsset::TryGetValueRange() const noexcept {
    return UnionOfChannels(
        *this, [](const TangentCurve& channel) noexcept { return channel.TryGetValueRange(); });
}

void CurveAsset::ResetCurve() noexcept {
    for (usize index = 0; index < ChannelCount(); ++index) {
        MutableChannel(index).Reset();
    }
}

} // namespace rocks
