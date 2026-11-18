#include <Curves/PlainCurve.h>

#include "CurveKeyEdit.h"

// PlainCurve의 평가·키 편집·중복 키 제거다(Docs/Design/CurveData.ko.md 5.7·7절, UE
// SimpleCurve.cpp).
namespace rocks {

namespace {

PlainCurveKey MakeKey(float time, float value) noexcept {
    PlainCurveKey key;
    key.time = time;
    key.value = value;
    return key;
}

// UE FSimpleCurve::EvalForTwoKeys(SC 493–509): 길이가 양수이고 Linear면 Lerp, 아니면 앞 키 값.
float PlainSegment(PlainCurveInterp interp, const PlainCurveKey& first, const PlainCurveKey& second,
                   float time) noexcept {
    const float difference = second.time - first.time;
    if (difference > 0.0f && interp != PlainCurveInterp::Constant) {
        const float alpha = (time - first.time) / difference;
        return math::Lerp(first.value, second.value, alpha);
    }

    return first.value;
}

// UE FSimpleCurve::RemoveRedundantKeysInternal(SC 511–643): [start, end] 보존 키 사이에서, 남긴
// 키를 되짚어 다시 지우며 고른다(두 번 불러도 더 지우지 않는다 — UE 쿡 결정성 주석).
// keptIndices가 있으면 남긴 키의 원래 index를 담는다(실패하면 그대로).
Result<void> RemoveRedundant(Array<PlainCurveKey>& keys, PlainCurveInterp interp, float tolerance,
                             usize start, usize end, Array<usize>* keptIndices) noexcept {
    const usize count = keys.Size();
    if (count < 3 || end - start < 2) {
        return curves_detail::FillIdentityIndices(keptIndices, count);
    }

    Allocator& allocator = keys.GetAllocator();
    Array<usize> keep(keptIndices != nullptr ? keptIndices->GetAllocator() : allocator);
    if (auto reserved = keep.TryReserve(count); !reserved) {
        return reserved;
    }

    const Span<const PlainCurveKey> view = keys.View();
    auto keepMiddle = [&](usize first, usize middle, usize last) noexcept {
        const float without = PlainSegment(interp, view[first], view[last], view[middle].time);
        return !(math::Abs(without - view[middle].value) <= tolerance); // NaN 차는 남긴다
    };

    usize next = 0;
    for (; next <= start; ++next) {
        static_cast<void>(keep.TryPushBack(next)); // 예약했으므로 실패하지 않는다
    }

    for (;;) {
        bool skipped = false;
        usize evaluateStart = keep[keep.Size() - 1];
        while (next < end) {
            if (keepMiddle(evaluateStart, next, next + 1)) {
                break;
            }

            ++next;
            skipped = true;
        }

        if (skipped) {
            bool removed = false;
            while (keep[keep.Size() - 1] > start) {
                const usize kept = keep.Size();
                if (keepMiddle(keep[kept - 2], keep[kept - 1], next)) {
                    break;
                }

                static_cast<void>(keep.TryPopBack());
                removed = true;
            }

            if (removed) {
                continue;
            }
        }

        if (next >= end) {
            break;
        }

        static_cast<void>(keep.TryPushBack(next));
        ++next;
    }

    for (; next < count; ++next) {
        static_cast<void>(keep.TryPushBack(next));
    }

    Array<PlainCurveKey> kept(allocator);
    if (auto reserved = kept.TryReserve(keep.Size()); !reserved) {
        return reserved;
    }

    for (const usize index : keep.View()) {
        static_cast<void>(kept.TryPushBack(view[index]));
    }

    keys = Move(kept);
    if (keptIndices != nullptr) {
        *keptIndices = Move(keep);
    }

    return Result<void>::Success();
}

Result<void> RemoveRedundantAll(Array<PlainCurveKey>& keys, PlainCurveInterp interp,
                                float tolerance, Array<usize>* keptIndices) noexcept {
    if (!curves_detail::IsUsableTolerance(tolerance)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (keys.Size() < 3) {
        return curves_detail::FillIdentityIndices(keptIndices, keys.Size());
    }

    return RemoveRedundant(keys, interp, tolerance, 0, keys.Size() - 1, keptIndices);
}

Result<void> RemoveRedundantRange(Array<PlainCurveKey>& keys, PlainCurveInterp interp,
                                  float tolerance, float first, float last,
                                  Array<usize>* keptIndices) noexcept {
    if (!curves_detail::IsUsableTolerance(tolerance) || math::IsNaN(first) || math::IsNaN(last)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize start = 0;
    usize end = 0;
    if (!curves_detail::FindRedundantRange(keys.View(), first, last, start, end)) {
        return curves_detail::FillIdentityIndices(keptIndices, keys.Size());
    }

    return RemoveRedundant(keys, interp, tolerance, start, end, keptIndices);
}

} // namespace

Result<void> PlainCurve::TryCopyFrom(const PlainCurve& other) noexcept {
    if (this == &other) {
        return Result<void>::Success();
    }

    Array<PlainCurveKey> keys(keys_.GetAllocator());
    if (auto reserved = keys.TryReserve(other.keys_.Size()); !reserved) {
        return reserved;
    }

    static_cast<void>(keys.TryAppend(other.keys_.View())); // 예약했으므로 실패하지 않는다
    defaultValue = other.defaultValue;
    preExtrapolation = other.preExtrapolation;
    postExtrapolation = other.postExtrapolation;
    interp = other.interp;
    keys_ = Move(keys);
    return Result<void>::Success();
}

float PlainCurve::Evaluate(float time, float fallback) const noexcept {
    const PlainCurveInterp mode = interp;
    return curves_detail::EvaluateCurve(
        defaultValue, preExtrapolation, postExtrapolation, keys_.View(), time, fallback, true,
        [mode](const PlainCurveKey& first, const PlainCurveKey& second, float at) noexcept {
            return PlainSegment(mode, first, second, at);
        });
}

bool PlainCurve::IsConstant(float tolerance) const noexcept {
    if (keys_.Size() <= 1) {
        return true;
    }

    const float reference = keys_[0].value;
    for (const PlainCurveKey& key : keys_.View()) {
        if (!math::IsNearlyEqual(key.value, reference, tolerance)) {
            return false;
        }
    }

    return true;
}

usize PlainCurve::FindKey(float time, float tolerance) const noexcept {
    return curves_detail::FindWithinTolerance(keys_.View(), time, tolerance);
}

Result<Interval<float>> PlainCurve::TryGetTimeRange() const noexcept {
    return curves_detail::TimeRange(keys_.View());
}

Result<Interval<float>> PlainCurve::TryGetValueRange() const noexcept {
    if (keys_.Size() == 0) {
        return Result<Interval<float>>::Failure(ErrorCode::NotFound);
    }

    float minimum = keys_[0].value;
    float maximum = keys_[0].value;
    for (const PlainCurveKey& key : keys_.View()) {
        if (!math::IsFinite(key.value)) {
            return Result<Interval<float>>::Failure(ErrorCode::InvalidArgument);
        }
    }

    curves_detail::IncludeKeyValues(keys_.View(), minimum, maximum);
    return curves_detail::FiniteRange(minimum, maximum);
}

Result<usize> PlainCurve::TryAddKey(float time, float value, bool unwindRotation) noexcept {
    return curves_detail::AddKey(keys_, MakeKey(time, value), unwindRotation);
}

Result<usize> PlainCurve::TryUpdateOrAddKey(float time, float value, bool unwindRotation,
                                            float tolerance) noexcept {
    return curves_detail::UpdateOrAddKey(keys_, MakeKey(time, value), unwindRotation, tolerance);
}

Result<void> PlainCurve::TryRemoveKey(usize index) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return keys_.TryRemoveAt(index);
}

Result<usize> PlainCurve::TrySetKeyTime(usize index, float time) noexcept {
    return curves_detail::SetKeyTime(keys_, index, time);
}

Result<void> PlainCurve::TrySetKeyValue(usize index, float value) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    keys_[index].value = value;
    return Result<void>::Success();
}

Result<void> PlainCurve::TrySetKeys(Span<const PlainCurveKey> keys) noexcept {
    return curves_detail::SetKeys(keys_, keys);
}

void PlainCurve::Reset() noexcept {
    keys_.Clear();
}

Result<void> PlainCurve::TryShiftTime(float delta) noexcept {
    return curves_detail::ShiftTime(keys_, delta);
}

Result<void> PlainCurve::TryScaleTime(float origin, float factor) noexcept {
    return curves_detail::ScaleTime(keys_, origin, factor);
}

Result<void> PlainCurve::TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept {
    return curves_detail::ShiftKeyTimes(keys_, indices, delta);
}

Result<void> PlainCurve::TryBake(float interval, float first, float last) noexcept {
    return curves_detail::Bake(
        keys_, interval, first, last, [this](float time) noexcept { return Evaluate(time); },
        MakeKey);
}

Result<void> PlainCurve::TryBake(float interval) noexcept {
    if (keys_.Size() == 0) {
        return TryBake(interval, 0.0f, 0.0f);
    }

    return TryBake(interval, keys_[0].time, keys_[keys_.Size() - 1].time);
}

Result<void> PlainCurve::TryRemoveRedundantKeys(float tolerance) noexcept {
    return RemoveRedundantAll(keys_, interp, tolerance, nullptr);
}

Result<void> PlainCurve::TryRemoveRedundantKeys(float tolerance, float first, float last) noexcept {
    return RemoveRedundantRange(keys_, interp, tolerance, first, last, nullptr);
}

Result<void> PlainCurve::TryRemoveRedundantKeys(Array<usize>& keptIndices,
                                                float tolerance) noexcept {
    return RemoveRedundantAll(keys_, interp, tolerance, &keptIndices);
}

Result<void> PlainCurve::TryRemoveRedundantKeys(Array<usize>& keptIndices, float tolerance,
                                                float first, float last) noexcept {
    return RemoveRedundantRange(keys_, interp, tolerance, first, last, &keptIndices);
}

Result<void> PlainCurve::Validate(const PlainCurve& curve) noexcept {
    return curves_detail::ValidateKeys(curve.keys_.View());
}

} // namespace rocks
