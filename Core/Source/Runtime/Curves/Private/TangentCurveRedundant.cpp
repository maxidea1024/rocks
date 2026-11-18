#include <Curves/TangentCurve.h>

#include "CurveKeyEdit.h"

// TangentCurve 중복 키 제거다(UE FRichCurve::RemoveRedundantKeysInternal, RC 966–1102 —
// 설계 7.2절). UE와 다른 점: 범위판의 비교 기준이 시작 보존 키에서 시작하고(15절 8), 제거 뒤
// AutoSetTangents를 부르지 않는다(D10), 표본 프레임 번호가 int32 끝에 닿으면 표본을 멈춘다(UE는
// 감싸 돌아 끝나지 않는다).
namespace rocks {

namespace {

bool IsUserTangentKey(const TangentCurveKey& key) noexcept {
    return key.tangentMode == CurveTangentMode::User || key.tangentMode == CurveTangentMode::Break;
}

// mostRecent(남긴 직전 키)·test·test + 1 세 키로 test 키를 남겨야 하는지다(RC 1022–1069).
bool KeepKey(Span<const TangentCurveKey> keys, usize mostRecent, usize test, float tolerance,
             const Optional<FrameRate>& sampleRate) noexcept {
    const TangentCurveKey& previous = keys[mostRecent];
    const TangentCurveKey& candidate = keys[test];
    const TangentCurveKey& next = keys[test + 1];
    if (sampleRate.HasValue()) {
        const FrameRate& rate = sampleRate.Value();
        // 키 시간은 유한하므로(불변식) AsFrameTime이 Fatal로 가지 않는다.
        const FrameTime last = rate.AsFrameTime(next.time);
        FrameTime sample = rate.AsFrameTime(previous.time);
        const float removedTime = candidate.time;
        while (sample <= last) {
            const float seconds = static_cast<float>(rate.AsSeconds(sample));
            const float without = math::EvalCurveSegment(previous, next, seconds);
            const TangentCurveKey& first = seconds <= removedTime ? previous : candidate;
            const TangentCurveKey& second = seconds <= removedTime ? candidate : next;
            const float with = math::EvalCurveSegment(first, second, seconds);
            if (math::Abs(without - with) > tolerance) {
                return true;
            }

            const Result<FrameNumber> advanced = sample.Frame().TryAdd(FrameNumber(1));
            if (!advanced) {
                break;
            }

            sample.SetFrame(advanced.Value());
        }

        return false;
    }

    if (IsUserTangentKey(previous) || IsUserTangentKey(next) || IsUserTangentKey(candidate)) {
        return true;
    }

    const float without = math::EvalCurveSegment(previous, next, candidate.time);
    return math::Abs(without - candidate.value) > tolerance;
}

// [start, end] 보존 키 사이의 키를 고른다(RC 966–1102). start < end다.
Result<void> RemoveRedundant(Array<TangentCurveKey>& keys, float tolerance, usize start, usize end,
                             const Optional<FrameRate>& sampleRate) noexcept {
    const usize count = keys.Size();
    if (count < 3 || end - start < 2) {
        return Result<void>::Success();
    }

    Array<TangentCurveKey> kept(keys.GetAllocator());
    if (auto reserved = kept.TryReserve(count); !reserved) {
        return reserved;
    }

    const Span<const TangentCurveKey> view = keys.View();
    for (usize index = 0; index <= start; ++index) {
        static_cast<void>(kept.TryPushBack(view[index])); // 예약했으므로 실패하지 않는다
    }

    usize mostRecent = start; // UE는 0(15절 8)
    for (usize test = start + 1; test < end; ++test) {
        if (KeepKey(view, mostRecent, test, tolerance, sampleRate)) {
            mostRecent = test;
            static_cast<void>(kept.TryPushBack(view[test]));
        }
    }

    for (usize index = end; index < count; ++index) {
        static_cast<void>(kept.TryPushBack(view[index]));
    }

    keys = Move(kept);
    return Result<void>::Success();
}

} // namespace

Result<void> TangentCurve::TryRemoveRedundantKeys(float tolerance,
                                                  Optional<FrameRate> sampleRate) noexcept {
    if (!curves_detail::IsUsableTolerance(tolerance)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (keys_.Size() < 3) {
        return Result<void>::Success();
    }

    return RemoveRedundant(keys_, tolerance, 0, keys_.Size() - 1, sampleRate);
}

Result<void> TangentCurve::TryRemoveRedundantKeys(float tolerance, float first, float last,
                                                  Optional<FrameRate> sampleRate) noexcept {
    if (!curves_detail::IsUsableTolerance(tolerance) || math::IsNaN(first) || math::IsNaN(last)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize start = 0;
    usize end = 0;
    if (!curves_detail::FindRedundantRange(keys_.View(), first, last, start, end)) {
        return Result<void>::Success();
    }

    return RemoveRedundant(keys_, tolerance, start, end, sampleRate);
}

} // namespace rocks
