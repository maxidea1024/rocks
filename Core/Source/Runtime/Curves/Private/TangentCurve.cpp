#include <Curves/TangentCurve.h>

#include "CurveKeyEdit.h"

// TangentCurve의 평가·자동 접선·키 편집이다(Docs/Design/CurveData.ko.md 5–7절). 중복 키 제거는
// TangentCurveRedundant.cpp, 값 범위는 TangentCurveValueRange.cpp다.
namespace rocks {

namespace {

TangentCurveKey MakeKey(float time, float value) noexcept {
    TangentCurveKey key;
    key.time = time;
    key.value = value;
    return key;
}

} // namespace

Result<void> TangentCurve::TryCopyFrom(const TangentCurve& other) noexcept {
    if (this == &other) {
        return Result<void>::Success();
    }

    Array<TangentCurveKey> keys(keys_.GetAllocator());
    if (auto reserved = keys.TryReserve(other.keys_.Size()); !reserved) {
        return reserved;
    }

    static_cast<void>(keys.TryAppend(other.keys_.View())); // 예약했으므로 실패하지 않는다
    defaultValue = other.defaultValue;
    preExtrapolation = other.preExtrapolation;
    postExtrapolation = other.postExtrapolation;
    keys_ = Move(keys);
    return Result<void>::Success();
}

float TangentCurve::Evaluate(float time, float fallback) const noexcept {
    return curves_detail::EvaluateCurve(
        *this, keys_.View(), time, fallback, false,
        [](const TangentCurveKey& first, const TangentCurveKey& second, float at) noexcept {
            return math::EvalCurveSegment(first, second, at);
        });
}

// UE FRichCurve::IsConstant(RC 485–502).
bool TangentCurve::IsConstant(float tolerance) const noexcept {
    if (keys_.Size() <= 1) {
        return true;
    }

    const float reference = keys_[0].value;
    for (const TangentCurveKey& key : keys_.View()) {
        if (!math::IsNearlyEqual(key.value, reference, tolerance) ||
            key.interp == CurveInterp::Cubic) {
            return false;
        }
    }

    return true;
}

usize TangentCurve::FindKey(float time, float tolerance) const noexcept {
    return curves_detail::FindWithinTolerance(keys_.View(), time, tolerance);
}

Result<Interval<float>> TangentCurve::TryGetTimeRange() const noexcept {
    return curves_detail::TimeRange(keys_.View());
}

// UE FRichCurve::AutoSetTangents(RC 680–755)를 줄 단위로 옮겼다. 키 값은 바꾸지 않으므로 앞 키를
// 이미 고친 뒤에 읽어도 결과가 같다.
void TangentCurve::AutoSetTangents(float tension) noexcept {
    const usize count = keys_.Size();
    for (usize index = 0; index < count; ++index) {
        TangentCurveKey& key = keys_[index];
        float arriveTangent = key.arriveTangent;
        float leaveTangent = key.leaveTangent;
        if (index == 0) {
            if (index + 1 < count && key.tangentMode == CurveTangentMode::Auto) {
                leaveTangent = 0.0f;
            }
        } else if (index + 1 < count) {
            const TangentCurveKey& previous = keys_[index - 1];
            if (key.interp == CurveInterp::Cubic && key.tangentMode == CurveTangentMode::Auto) {
                const TangentCurveKey& next = keys_[index + 1];
                arriveTangent = math::ComputeCurveTangent(previous.time, previous.value, key.value,
                                                          next.time, next.value, tension);
                leaveTangent = arriveTangent;
            } else if (previous.interp == CurveInterp::Constant ||
                       key.interp == CurveInterp::Constant) {
                leaveTangent = 0.0f;
                if (previous.interp != CurveInterp::Cubic) {
                    arriveTangent = 0.0f;
                }
            }
        } else if (key.interp == CurveInterp::Cubic && key.tangentMode == CurveTangentMode::Auto) {
            arriveTangent = 0.0f;
        }

        key.arriveTangent = arriveTangent;
        key.leaveTangent = leaveTangent;
    }
}

Result<usize> TangentCurve::TryAddKey(float time, float value, bool unwindRotation) noexcept {
    return curves_detail::AddKey(keys_, MakeKey(time, value), unwindRotation);
}

Result<usize> TangentCurve::TryUpdateOrAddKey(float time, float value, bool unwindRotation,
                                              float tolerance) noexcept {
    return curves_detail::UpdateOrAddKey(keys_, MakeKey(time, value), unwindRotation, tolerance);
}

Result<void> TangentCurve::TryRemoveKey(usize index) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return keys_.TryRemoveAt(index);
}

Result<usize> TangentCurve::TrySetKeyTime(usize index, float time) noexcept {
    return curves_detail::SetKeyTime(keys_, index, time);
}

Result<void> TangentCurve::TrySetKeyValue(usize index, float value) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    keys_[index].value = value;
    return Result<void>::Success();
}

Result<void> TangentCurve::TrySetKeyInterp(usize index, CurveInterp interp) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    keys_[index].interp = interp;
    return Result<void>::Success();
}

Result<void> TangentCurve::TrySetKeyTangentMode(usize index, CurveTangentMode mode) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    keys_[index].tangentMode = mode;
    return Result<void>::Success();
}

Result<void> TangentCurve::TrySetKeyTangentWeight(usize index, CurveTangentWeight weight) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    keys_[index].tangentWeight = weight;
    return Result<void>::Success();
}

Result<void> TangentCurve::TrySetKeyTangents(usize index, float arriveTangent,
                                             float leaveTangent) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    keys_[index].arriveTangent = arriveTangent;
    keys_[index].leaveTangent = leaveTangent;
    return Result<void>::Success();
}

Result<void> TangentCurve::TrySetKeyTangentWeights(usize index, float arriveWeight,
                                                   float leaveWeight) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    keys_[index].arriveTangentWeight = arriveWeight;
    keys_[index].leaveTangentWeight = leaveWeight;
    return Result<void>::Success();
}

Result<void> TangentCurve::TrySetKeys(Span<const TangentCurveKey> keys) noexcept {
    return curves_detail::SetKeys(keys_, keys);
}

void TangentCurve::Reset() noexcept {
    keys_.Clear();
}

Result<void> TangentCurve::TryShiftTime(float delta) noexcept {
    return curves_detail::ShiftTime(keys_, delta);
}

Result<void> TangentCurve::TryScaleTime(float origin, float factor) noexcept {
    return curves_detail::ScaleTime(keys_, origin, factor);
}

Result<void> TangentCurve::TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept {
    return curves_detail::ShiftKeyTimes(keys_, indices, delta);
}

Result<void> TangentCurve::TryBake(float interval, float first, float last) noexcept {
    return curves_detail::Bake(
        keys_, interval, first, last, [this](float time) noexcept { return Evaluate(time); },
        MakeKey);
}

Result<void> TangentCurve::TryBake(float interval) noexcept {
    if (keys_.Size() == 0) {
        return TryBake(interval, 0.0f, 0.0f);
    }

    return TryBake(interval, keys_[0].time, keys_[keys_.Size() - 1].time);
}

Result<void> TangentCurve::Validate(const TangentCurve& curve) noexcept {
    return curves_detail::ValidateKeys(curve.keys_.View());
}

} // namespace rocks
