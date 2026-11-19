#include <Curves/TangentCurve.h>

#include "CurveKeyEdit.h"

// TangentCurve 중복 키 제거다(UE FRichCurve::RemoveRedundantKeysInternal, RC 966–1102 —
// 설계 7.2절). UE와 다른 점: 범위판의 비교 기준이 시작 보존 키에서 시작하고(15절 8), 제거 뒤
// AutoSetTangents를 부르지 않는다(D10), 표본 프레임 번호가 int32 끝에 닿으면 표본을 멈춘다(UE는
// 감싸 돌아 끝나지 않는다), 호출 하나의 표본 수가 2^24를 넘으면 CapacityExceeded다(Bake와 같은
// 상한 — UE는 상한이 없다), 차가 NaN인 키(NaN 값)는 남긴다(UE `>` 비교는 NaN을 지운다).
namespace rocks {

namespace {

bool IsUserTangentKey(const TangentCurveKey& key) noexcept {
    return key.tangentMode == CurveTangentMode::User || key.tangentMode == CurveTangentMode::Break;
}

// 호출 하나의 표본 수 상한이다(Bake와 같다).
inline constexpr usize MaxRedundantSamples = curves_detail::MaxBakeSamples;

// mostRecent(남긴 직전 키)·test·test + 1 세 키로 test 키를 남겨야 하는지다(RC 1022–1069). 표본판은
// samples에 표본 수를 더하고, 상한을 넘으면 CapacityExceeded다.
Result<bool> KeepKey(Span<const TangentCurveKey> keys, usize mostRecent, usize test,
                     float tolerance, const Optional<FrameRate>& sampleRate,
                     usize& samples) noexcept {
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
            if (++samples > MaxRedundantSamples) {
                return Result<bool>::Failure(ErrorCode::CapacityExceeded);
            }

            const float seconds = static_cast<float>(rate.AsSeconds(sample));
            const float without = math::EvalCurveSegment(previous, next, seconds);
            const TangentCurveKey& first = seconds <= removedTime ? previous : candidate;
            const TangentCurveKey& second = seconds <= removedTime ? candidate : next;
            const float with = math::EvalCurveSegment(first, second, seconds);
            if (!(math::Abs(without - with) <= tolerance)) { // NaN 차는 남긴다
                return Result<bool>::Success(true);
            }

            const Result<FrameNumber> advanced = sample.Frame().TryAdd(FrameNumber(1));
            if (!advanced) {
                break;
            }

            sample.SetFrame(advanced.Value());
        }

        return Result<bool>::Success(false);
    }

    if (IsUserTangentKey(previous) || IsUserTangentKey(next) || IsUserTangentKey(candidate)) {
        return Result<bool>::Success(true);
    }

    const float without = math::EvalCurveSegment(previous, next, candidate.time);
    // NaN 차는 남긴다.
    return Result<bool>::Success(!(math::Abs(without - candidate.value) <= tolerance));
}

// [start, end] 보존 키 사이의 키를 고른다(RC 966–1102). start < end다.
// keptIndices가 있으면 남긴 키의 원래 index를 담는다(실패하면 그대로).
Result<void> RemoveRedundant(Array<TangentCurveKey>& keys, float tolerance, usize start, usize end,
                             const Optional<FrameRate>& sampleRate,
                             Array<usize>* keptIndices) noexcept {
    const usize count = keys.Size();
    if (count < 3 || end - start < 2) {
        return curves_detail::FillIdentityIndices(keptIndices, count);
    }

    Array<TangentCurveKey> kept(keys.GetAllocator());
    if (auto reserved = kept.TryReserve(count); !reserved) {
        return reserved;
    }

    Array<usize> indices(keptIndices != nullptr ? keptIndices->GetAllocator()
                                                : keys.GetAllocator());
    if (keptIndices != nullptr) {
        if (auto reserved = indices.TryReserve(count); !reserved) {
            return reserved;
        }
    }

    const Span<const TangentCurveKey> view = keys.View();
    auto keep = [&](usize index) noexcept {
        static_cast<void>(kept.TryPushBack(view[index])); // 예약했으므로 실패하지 않는다
        if (keptIndices != nullptr) {
            static_cast<void>(indices.TryPushBack(index));
        }
    };

    for (usize index = 0; index <= start; ++index) {
        keep(index);
    }

    usize mostRecent = start; // UE는 0(15절 8)
    usize samples = 0;
    for (usize test = start + 1; test < end; ++test) {
        auto keepTest = KeepKey(view, mostRecent, test, tolerance, sampleRate, samples);
        if (!keepTest) {
            return Result<void>::Failure(keepTest.Error());
        }

        if (keepTest.Value()) {
            mostRecent = test;
            keep(test);
        }
    }

    for (usize index = end; index < count; ++index) {
        keep(index);
    }

    keys = Move(kept);
    if (keptIndices != nullptr) {
        *keptIndices = Move(indices);
    }

    return Result<void>::Success();
}

// 전체판·범위판의 공통 진입이다(keptIndices는 nullptr일 수 있다).
Result<void> RemoveRedundantAll(Array<TangentCurveKey>& keys, float tolerance,
                                const Optional<FrameRate>& sampleRate,
                                Array<usize>* keptIndices) noexcept {
    if (!curves_detail::IsUsableTolerance(tolerance)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (keys.Size() < 3) {
        return curves_detail::FillIdentityIndices(keptIndices, keys.Size());
    }

    return RemoveRedundant(keys, tolerance, 0, keys.Size() - 1, sampleRate, keptIndices);
}

Result<void> RemoveRedundantRange(Array<TangentCurveKey>& keys, float tolerance, float first,
                                  float last, const Optional<FrameRate>& sampleRate,
                                  Array<usize>* keptIndices) noexcept {
    if (!curves_detail::IsUsableTolerance(tolerance) || math::IsNaN(first) || math::IsNaN(last)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize start = 0;
    usize end = 0;
    if (!curves_detail::FindRedundantRange(keys.View(), first, last, start, end)) {
        return curves_detail::FillIdentityIndices(keptIndices, keys.Size());
    }

    return RemoveRedundant(keys, tolerance, start, end, sampleRate, keptIndices);
}

} // namespace

Result<void> TangentCurve::TryRemoveRedundantKeys(float tolerance,
                                                  Optional<FrameRate> sampleRate) noexcept {
    return RemoveRedundantAll(keys_, tolerance, sampleRate, nullptr);
}

Result<void> TangentCurve::TryRemoveRedundantKeys(float tolerance, float first, float last,
                                                  Optional<FrameRate> sampleRate) noexcept {
    return RemoveRedundantRange(keys_, tolerance, first, last, sampleRate, nullptr);
}

Result<void> TangentCurve::TryRemoveRedundantKeys(Array<usize>& keptIndices, float tolerance,
                                                  Optional<FrameRate> sampleRate) noexcept {
    return RemoveRedundantAll(keys_, tolerance, sampleRate, &keptIndices);
}

Result<void> TangentCurve::TryRemoveRedundantKeys(Array<usize>& keptIndices, float tolerance,
                                                  float first, float last,
                                                  Optional<FrameRate> sampleRate) noexcept {
    return RemoveRedundantRange(keys_, tolerance, first, last, sampleRate, &keptIndices);
}

} // namespace rocks
