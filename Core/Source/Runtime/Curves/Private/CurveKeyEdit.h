#pragma once

#include <Base/NumericLimits.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Interval.h>
#include <Core/Math/KeyCurveMath.h>
#include <Core/Math/MathUtility.h>
#include <Core/Result.h>
#include <Curves/CurveTypes.h>
#include <Platform/PlatformMath.h>

// 커브 값 타입(TangentCurve·PlainCurve·계단 커브 셋)이 함께 쓰는 키 배열 연산이다(Docs/Design/
// CurveData.ko.md 5·7절). 키 형식은 float time 칸과 value 칸을 가진 struct이고 배열은 시간
// 오름차순·유한 시간이다. 키는 옮기기(Move)로 다루고 복사가 필요한 곳은 CloneCurveKey를 쓴다
// (String 키는 할당하는 복사다). 함수마다 실패하면 배열을 바꾸지 않는다.
namespace rocks {

struct StringStepKey;

} // namespace rocks

namespace rocks::curves_detail {

// 키 하나의 복사본이다. 비트 복사 가능한 키는 그대로, StringStepKey는 값 문자열을 allocator로
// 복제한다(StepCurves.cpp — 실패하면 OutOfMemory).
template <class Key>
    requires IsTriviallyCopyableV<Key>
Result<Key> CloneCurveKey(const Key& key, Allocator&) noexcept {
    return Result<Key>::Success(key);
}

Result<StringStepKey> CloneCurveKey(const StringStepKey& key, Allocator& allocator) noexcept;

// source의 키를 모두 복제해 out 끝에 더한다(out은 source.Size()만큼 미리 예약돼 있어야 한다).
template <class Key>
Result<void> AppendCloned(Array<Key>& out, Span<const Key> source) noexcept {
    for (const Key& key : source) {
        auto cloned = CloneCurveKey(key, out.GetAllocator());
        if (!cloned) {
            return Result<void>::Failure(cloned.Error());
        }

        if (auto pushed = out.TryPushBack(Move(cloned.Value())); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 허용오차 인자가 쓸 수 있는 값인지다(0 이상 — NaN·음수는 거부한다).
inline bool IsUsableTolerance(float tolerance) noexcept {
    return tolerance >= 0.0f;
}

// 첫 keys[i].time >= time인 i다(없으면 Size). 같은 시간 키들 앞 자리다(UE AddKey 삽입 위치).
template <class Key>
usize LowerBound(Span<Key> keys, float time) noexcept {
    usize first = 0;
    usize count = keys.Size();
    while (count > 0) {
        const usize step = count / 2;
        const usize middle = first + step;
        if (keys[middle].time < time) {
            first = middle + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }

    return first;
}

// |keys[i].time − time| ≤ tolerance인 가장 앞 i다(없으면 IndexNone). 정렬된 배열에서 "키 시간 >
// time 또는 허용오차 안"은 거짓 다음 참인 단조 조건이라(키 시간 ≤ time 쪽에서 float 차 |키 −
// time|은 키 시간에 대해 늘지 않는다) 이진 탐색으로 그 경계를 찾는다. UE UpdateOrAddKey의
// 앞에서부터 훑기와 결과가 같다.
template <class Key>
usize FindWithinTolerance(Span<Key> keys, float time, float tolerance) noexcept {
    if (math::IsNaN(time) || !IsUsableTolerance(tolerance)) {
        return IndexNone;
    }

    usize first = 0;
    usize count = keys.Size();
    while (count > 0) {
        const usize step = count / 2;
        const usize middle = first + step;
        const float keyTime = keys[middle].time;
        if (!(keyTime > time || math::IsNearlyEqual(keyTime, time, tolerance))) {
            first = middle + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }

    if (first < keys.Size() && math::IsNearlyEqual(keys[first].time, time, tolerance)) {
        return first;
    }

    return IndexNone;
}

// UE AddKey의 회전 풀기다(RC 319–334): 앞 키 값과의 차가 (−180, 180] 쪽에 들도록 360씩 옮긴다. UE
// 반복 뺄셈을 그대로 해서 UE가 끝나는 모든 입력에서 비트가 같다(360을 더하고 빼도 값이 바뀌는 크기
// — 약 2^33 아래 — 는 2^26번 안에 끝난다). 어느 값이 비유한이면 옮기지 않고, 360을 더하고 빼도 값이
// 그대로인 큰 값(UE는 끝나지 않는다)이면 double 나머지로 한 번에 옮긴다. 2^26번 상한은 안전장치다.
inline float UnwindRotation(float previous, float value) noexcept {
    if (!math::IsFinite(previous) || !math::IsFinite(value)) {
        return value;
    }

    constexpr uint32 MaxSteps = 1u << 26;
    float moved = value;
    uint32 steps = 0;
    bool stuck = false;
    while (!stuck && moved - previous > 180.0f) {
        const float next = moved - 360.0f;
        stuck = next == moved || ++steps > MaxSteps;
        moved = next;
    }

    while (!stuck && moved - previous < -180.0f) {
        const float next = moved + 360.0f;
        stuck = next == moved || ++steps > MaxSteps;
        moved = next;
    }

    if (!stuck) {
        return moved;
    }

    double remainder =
        math::Fmod(static_cast<double>(value) - static_cast<double>(previous), 360.0);
    if (remainder > 180.0) {
        remainder -= 360.0;
    } else if (remainder < -180.0) {
        remainder += 360.0;
    }

    return static_cast<float>(static_cast<double>(previous) + remainder);
}

// 키 시간이 모두 유한하고 오름차순인지다(검증 hook·TrySetKeys).
template <class Key>
Result<void> ValidateKeys(Span<Key> keys) noexcept {
    for (usize index = 0; index < keys.Size(); ++index) {
        const float time = keys[index].time;
        if (!math::IsFinite(time) || (index > 0 && keys[index - 1].time > time)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

// key를 같은 시간 키들 앞에 넣고 index를 돌려준다(UE AddKey). unwind면 앞 키 기준으로 값을 푼다.
template <class Key>
Result<usize> AddKey(Array<Key>& keys, Key key, bool unwindRotation) noexcept {
    if (!math::IsFinite(key.time)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const usize index = LowerBound(keys.View(), key.time);
    if constexpr (IsSameV<decltype(key.value), float>) {
        if (unwindRotation && index > 0) {
            key.value = UnwindRotation(keys[index - 1].value, key.value);
        }
    } else {
        static_cast<void>(unwindRotation); // 회전 풀기는 실수 값 커브만이다
    }

    if (auto inserted = keys.TryInsert(Move(key), index); !inserted) {
        return Result<usize>::Failure(inserted.Error());
    }

    return Result<usize>::Success(index);
}

// 허용오차 안의 가장 앞 키 값을 바꾸거나 새 키를 넣는다(UE UpdateOrAddKey).
template <class Key>
Result<usize> UpdateOrAddKey(Array<Key>& keys, Key key, bool unwindRotation,
                             float tolerance) noexcept {
    if (!math::IsFinite(key.time) || !IsUsableTolerance(tolerance)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const usize found = FindWithinTolerance(keys.View(), key.time, tolerance);
    if (found != IndexNone) {
        keys[found].value = Move(key.value);
        return Result<usize>::Success(found);
    }

    return AddKey(keys, Move(key), unwindRotation);
}

// index 키를 새 시간 자리(같은 시간 키들 앞)로 옮긴다. 다른 칸은 그대로다. 할당하지 않는다.
template <class Key>
Result<usize> SetKeyTime(Array<Key>& keys, usize index, float time) noexcept {
    if (index >= keys.Size() || !math::IsFinite(time)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    Key moved = Move(keys[index]);
    // 그 키를 뺀 배열에서 time보다 앞선 키 수가 새 자리다(옮긴 자리의 시간 칸은 그대로 남아 있다).
    usize target = LowerBound(keys.View(), time);
    if (moved.time < time) {
        --target;
    }

    moved.time = time;
    if (target < index) {
        for (usize position = index; position > target; --position) {
            keys[position] = Move(keys[position - 1]);
        }
    } else {
        for (usize position = index; position < target; ++position) {
            keys[position] = Move(keys[position + 1]);
        }
    }

    keys[target] = Move(moved);
    return Result<usize>::Success(target);
}

// 키 전체를 source로 바꾼다(정렬·유한 시간만).
template <class Key>
Result<void> SetKeys(Array<Key>& keys, Span<const Key> source) noexcept {
    if (auto valid = ValidateKeys(source); !valid) {
        return valid;
    }

    Array<Key> next(keys.GetAllocator());
    if (auto reserved = next.TryReserve(source.Size()); !reserved) {
        return reserved;
    }

    if (auto appended = AppendCloned(next, source); !appended) {
        return appended;
    }

    keys = Move(next);
    return Result<void>::Success();
}

// 모든 키 시간을 transform(time)으로 바꾼다. 결과가 하나라도 비유한이면 바꾸지 않는다. transform은
// 단조 증가라 순서가 그대로다.
template <class Key, class Transform>
Result<void> TransformTimes(Array<Key>& keys, Transform&& transform) noexcept {
    for (usize index = 0; index < keys.Size(); ++index) {
        if (!math::IsFinite(transform(keys[index].time))) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    for (usize index = 0; index < keys.Size(); ++index) {
        keys[index].time = transform(keys[index].time);
    }

    return Result<void>::Success();
}

template <class Key>
Result<void> ShiftTime(Array<Key>& keys, float delta) noexcept {
    if (!math::IsFinite(delta)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return TransformTimes(keys, [delta](float time) noexcept { return time + delta; });
}

// UE ScaleCurve 식: (time − origin)·factor + origin.
template <class Key>
Result<void> ScaleTime(Array<Key>& keys, float origin, float factor) noexcept {
    if (!math::IsFinite(origin) || !math::IsFinite(factor) || !(factor > 0.0f)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return TransformTimes(
        keys, [origin, factor](float time) noexcept { return (time - origin) * factor + origin; });
}

// 고른 키만 delta만큼 옮긴다: 고른 키를 빼고 원래 index 오름차순으로 "같은 시간 키 앞"에 다시
// 넣는다.
template <class Key>
Result<void> ShiftKeyTimes(Array<Key>& keys, Span<const usize> indices, float delta) noexcept {
    if (!math::IsFinite(delta)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (indices.Size() == 0) {
        return Result<void>::Success();
    }

    Allocator& allocator = keys.GetAllocator();
    Array<uint8> selected(allocator);
    if (auto sized = selected.TryResize(keys.Size()); !sized) {
        return sized;
    }

    for (usize position = 0; position < selected.Size(); ++position) {
        selected[position] = 0;
    }

    for (const usize index : indices) {
        if (index >= keys.Size() || selected[index] != 0 ||
            !math::IsFinite(keys[index].time + delta)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        selected[index] = 1;
    }

    Array<Key> next(allocator);
    if (auto reserved = next.TryReserve(keys.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < keys.Size(); ++index) {
        if (selected[index] == 0) {
            static_cast<void>(next.TryPushBack(Move(keys[index]))); // 예약했으므로 실패하지 않는다
        }
    }

    for (usize index = 0; index < keys.Size(); ++index) {
        if (selected[index] != 0) {
            Key moved = Move(keys[index]);
            moved.time = moved.time + delta;
            const usize at = LowerBound(next.View(), moved.time);
            static_cast<void>(next.TryInsert(Move(moved), at));
        }
    }

    keys = Move(next);
    return Result<void>::Success();
}

// Bake의 표본 수 상한이다(float i가 정확한 범위 — i·interval의 i는 2^24까지 정확하다).
inline constexpr usize MaxBakeSamples = usize{1} << 24;

// first + i·interval(i ≥ 1, < last) 시간마다 evaluate(time)를 먼저 모두 구한 뒤
// UpdateOrAddKey(허용오차 1e-4 — UE BakeCurve 기본값)로 넣는다. makeKey(time, value)가 새 키를
// 만든다.
template <class Key, class Evaluate, class MakeKey>
Result<void> Bake(Array<Key>& keys, float interval, float first, float last, Evaluate&& evaluate,
                  MakeKey&& makeKey) noexcept {
    if (!math::IsFinite(interval) || !(interval > 0.0f) || !math::IsFinite(first) ||
        !math::IsFinite(last)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (keys.Size() == 0) {
        return Result<void>::Success();
    }

    usize count = 0;
    while (first + static_cast<float>(count + 1) * interval < last) {
        if (count == MaxBakeSamples) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        ++count;
    }

    Allocator& allocator = keys.GetAllocator();
    Array<float> values(allocator);
    if (auto reserved = values.TryReserve(count); !reserved) {
        return reserved;
    }

    for (usize sample = 1; sample <= count; ++sample) {
        const float time = first + static_cast<float>(sample) * interval;
        static_cast<void>(values.TryPushBack(evaluate(time))); // 예약했으므로 실패하지 않는다
    }

    Array<Key> next(allocator);
    if (auto reserved = next.TryReserve(keys.Size() + count); !reserved) {
        return reserved;
    }

    static_cast<void>(next.TryAppend(keys.View()));
    constexpr float BakeTolerance = 1.0e-4f; // UE UpdateOrAddKey 기본값(UE_KINDA_SMALL_NUMBER)
    for (usize sample = 1; sample <= count; ++sample) {
        const float time = first + static_cast<float>(sample) * interval;
        // 예약했고 시간이 유한하므로 실패하지 않는다.
        static_cast<void>(
            UpdateOrAddKey(next, makeKey(time, values[sample - 1]), false, BakeTolerance));
    }

    keys = Move(next);
    return Result<void>::Success();
}

// 범위판 중복 제거의 시작·끝 보존 키를 찾는다(UE RemoveRedundantKeys(…, First, Last, …)): 시간이
// first 이하인 마지막 키와 last 이상인 첫 키. 찾으면 참이다. first ≥ last면 거짓이다.
template <class Key>
bool FindRedundantRange(Span<Key> keys, float first, float last, usize& start,
                        usize& end) noexcept {
    if (first >= last) {
        return false;
    }

    start = IndexNone;
    end = IndexNone;
    for (usize index = 0; index < keys.Size(); ++index) {
        const float time = keys[index].time;
        if (time <= first) {
            start = index;
        }

        if (time >= last) {
            end = index;
            break;
        }
    }

    return start != IndexNone && end != IndexNone;
}

// 첫 keys[i].time > time인 i다(없으면 Size).
template <class Key>
usize UpperBound(Span<Key> keys, float time) noexcept {
    usize first = 0;
    usize count = keys.Size();
    while (count > 0) {
        const usize step = count / 2;
        const usize middle = first + step;
        if (!(time < keys[middle].time)) {
            first = middle + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }

    return first;
}

// 시간이 time 이하인 마지막 키다(없거나 time이 NaN이면 IndexNone — UE FIntegralCurve::
// FindKeyBeforeOrAt). 같은 시간 키가 여럿이면 그 시간의 마지막 키다.
template <class Key>
usize FindAtOrBefore(Span<Key> keys, float time) noexcept {
    if (keys.Size() == 0 || math::IsNaN(time) || time < keys[0].time) {
        return IndexNone;
    }

    return UpperBound(keys, time) - 1;
}

// 계단 커브 평가의 키 index다(UE FIntegralCurve::Evaluate·FStringCurve::Eval — 설계 5.8절).
// 기본값을 써야 하면 IndexNone이다: 키가 없을 때, defaultBeforeFirst이고 첫 키 시간보다 앞일 때,
// 시간이 NaN일 때(UE는 마지막 키 — 17.5절). 키 하나이거나 첫 키 앞이면 첫 키, 마지막 키 시간보다
// 앞이면 시간 이하인 마지막 키(UE의 앞에서부터 훑기와 같은 결과), 그 밖은 마지막 키다.
template <class Key>
usize StepKeyIndex(Span<Key> keys, float time, bool defaultBeforeFirst) noexcept {
    const usize count = keys.Size();
    if (count == 0 || math::IsNaN(time) || (defaultBeforeFirst && time < keys[0].time)) {
        return IndexNone;
    }

    if (count < 2 || time < keys[0].time) {
        return 0;
    }

    if (time < keys[count - 1].time) {
        return UpperBound(keys, time) - 1;
    }

    return count - 1;
}

// 계단 커브의 중복 키 제거다(UE FIntegralCurve·FStringCurve::RemoveRedundantKeys — 원래 배열에서
// 앞·뒤 키와 값이 같은 안쪽 키를 지운다). UE와 달리 남은 키 하나를 기본값으로 바꾸지 않는다(설계
// 15절 9). same(a, b)가 값 같음이다.
// 키를 지우지 않은 중복 제거의 남은 index(0..count − 1)를 keptIndices에 채운다(nullptr면 할 일이
// 없다). 할당 실패면 OutOfMemory이고 keptIndices는 그대로다.
inline Result<void> FillIdentityIndices(Array<usize>* keptIndices, usize count) noexcept {
    if (keptIndices == nullptr) {
        return Result<void>::Success();
    }

    Array<usize> indices(keptIndices->GetAllocator());
    if (auto reserved = indices.TryReserve(count); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < count; ++index) {
        static_cast<void>(indices.TryPushBack(index)); // 예약했으므로 실패하지 않는다
    }

    *keptIndices = Move(indices);
    return Result<void>::Success();
}

template <class Key, class Same>
Result<void> RemoveStepRedundantKeys(Array<Key>& keys, Same&& same,
                                     Array<usize>* keptIndices = nullptr) noexcept {
    const usize count = keys.Size();
    if (count < 3) {
        return FillIdentityIndices(keptIndices, count);
    }

    Allocator& allocator = keys.GetAllocator();
    Array<uint8> removed(allocator);
    if (auto sized = removed.TryResize(count); !sized) {
        return sized;
    }

    Array<usize> indices(keptIndices != nullptr ? keptIndices->GetAllocator() : allocator);
    if (keptIndices != nullptr) {
        if (auto reserved = indices.TryReserve(count); !reserved) {
            return reserved;
        }
    }

    usize kept = count;
    for (usize index = 0; index < count; ++index) {
        const bool middle = index > 0 && index + 1 < count &&
                            same(keys[index - 1].value, keys[index].value) &&
                            same(keys[index].value, keys[index + 1].value);
        removed[index] = middle ? 1 : 0;
        kept -= middle ? 1 : 0;
    }

    if (kept == count) {
        if (keptIndices != nullptr) {
            for (usize index = 0; index < count; ++index) {
                static_cast<void>(indices.TryPushBack(index)); // 예약했으므로 실패하지 않는다
            }

            *keptIndices = Move(indices);
        }

        return Result<void>::Success();
    }

    Array<Key> next(allocator);
    if (auto reserved = next.TryReserve(kept); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < count; ++index) {
        if (removed[index] == 0) {
            static_cast<void>(next.TryPushBack(Move(keys[index]))); // 예약했으므로 실패하지 않는다
            if (keptIndices != nullptr) {
                static_cast<void>(indices.TryPushBack(index));
            }
        }
    }

    keys = Move(next);
    if (keptIndices != nullptr) {
        *keptIndices = Move(indices);
    }

    return Result<void>::Success();
}

template <class Key>
Result<Interval<float>> TimeRange(Span<Key> keys) noexcept {
    if (keys.Size() == 0) {
        return Result<Interval<float>>::Failure(ErrorCode::NotFound);
    }

    return Result<Interval<float>>::Success(
        Interval<float>(keys[0].time, keys[keys.Size() - 1].time));
}

// 키 값만으로 정한 범위다(PlainCurve, TangentCurve의 시작 값).
template <class Key>
void IncludeKeyValues(Span<Key> keys, float& minimum, float& maximum) noexcept {
    for (const Key& key : keys) {
        minimum = math::Min(minimum, key.value);
        maximum = math::Max(maximum, key.value);
    }
}

// 범위 결과다: 두 끝이 유한하면 Success, 아니면 InvalidArgument.
inline Result<Interval<float>> FiniteRange(float minimum, float maximum) noexcept {
    return Interval<float>::TryMake(minimum, maximum);
}

// 키 목록의 index 키 시간이다. 목록이 시간만 읽는 TimeAt(index)를 주면(PackedTangentCurve의 키 보기
// — 키 전체를 풀지 않는다) 그것을 쓰고, 아니면 keys[index].time이다.
template <class Keys>
float KeyTimeAt(const Keys& keys, usize index) noexcept {
    if constexpr (requires { keys.TimeAt(index); }) {
        return keys.TimeAt(index);
    } else {
        return keys[index].time;
    }
}

// 스칼라 커브의 공통 평가 틀이다(UE FRichCurve::Eval·FSimpleCurve::Eval, RemapTimeValue — 설계
// 5절). keys는 Size()와 operator[](키 — 참조나 값)를 가진 시간 오름차순 키 목록이다(Span, 또는
// PackedTangentCurve가 풀어 내는 키 보기). plainBounds면 PlainCurve 경계(첫 키 이하는 앞 갈래,
// 마지막 키 이상은 뒤 갈래), 아니면 TangentCurve 경계다. segment(k1, k2, time)가 구간 값이다.
template <class Keys, class Segment>
float EvaluateCurve(const Optional<float>& defaultValue, CurveExtrapolation preExtrapolation,
                    CurveExtrapolation postExtrapolation, const Keys& keys, float time,
                    float fallback, bool plainBounds, Segment&& segment) noexcept {
    const usize count = keys.Size();
    float value = defaultValue.HasValue() ? defaultValue.Value() : fallback;
    float offset = 0.0f;
    if (count == 0) {
        return value + offset; // UE와 같이 0을 더한다(−0은 +0이 된다)
    }

    if (math::IsNaN(time)) {
        return platform::PlatformMath::QuietNaN<float>();
    }

    const auto& firstKey = keys[0];
    const auto& lastKey = keys[count - 1];
    auto cyclic = [](CurveExtrapolation mode) noexcept {
        return mode != CurveExtrapolation::Linear && mode != CurveExtrapolation::Constant;
    };

    // 시간 재배치(UE RemapTimeValue, RC 1104–1161).
    if (count >= 2) {
        const bool before = time <= firstKey.time;
        const bool after = !before && time >= lastKey.time;
        const CurveExtrapolation mode = before ? preExtrapolation : postExtrapolation;
        if ((before || after) && cyclic(mode)) {
            const float minimum = firstKey.time;
            const float maximum = lastKey.time;
            const math::CurveCycle cycle = math::CycleCurveTime(minimum, maximum, time);
            time = cycle.time;
            if (mode == CurveExtrapolation::CycleWithOffset) {
                const float delta =
                    before ? firstKey.value - lastKey.value : lastKey.value - firstKey.value;
                offset = math::CurveCycleValueOffset(cycle, delta);
            } else if (mode == CurveExtrapolation::Oscillate && math::IsOddCurveCycle(cycle)) {
                time = minimum + (maximum - time);
            }
        }
    }

    constexpr float SlopeTimeTolerance = 1.0e-8f; // UE FMath::IsNearlyZero 기본값(UE_SMALL_NUMBER)
    const bool front = plainBounds ? time <= firstKey.time : time < firstKey.time;
    const bool inside = plainBounds ? time < lastKey.time : time <= lastKey.time;
    if (count < 2 || front) {
        if (preExtrapolation == CurveExtrapolation::Linear && count > 1) {
            const float timeDifference = keys[1].time - firstKey.time;
            if (math::IsNearlyZero(timeDifference, SlopeTimeTolerance)) {
                value = firstKey.value;
            } else {
                const float valueDifference = keys[1].value - firstKey.value;
                const float slope = valueDifference / timeDifference;
                value = slope * (time - firstKey.time) + firstKey.value;
            }
        } else {
            value = firstKey.value;
        }
    } else if (inside) {
        // UE와 같은 이진 탐색(first = 1, count = N − 2): time 이하인 마지막 키에서 시작하는 구간.
        usize first = 1;
        usize remaining = count - 2;
        while (remaining > 0) {
            const usize step = remaining / 2;
            const usize middle = first + step;
            if (time >= KeyTimeAt(keys, middle)) {
                first = middle + 1;
                remaining -= step + 1;
            } else {
                remaining = step;
            }
        }

        value = segment(keys[first - 1], keys[first], time);
    } else {
        if (postExtrapolation == CurveExtrapolation::Linear) {
            const auto& previousKey = keys[count - 2];
            const float timeDifference = previousKey.time - lastKey.time;
            if (math::IsNearlyZero(timeDifference, SlopeTimeTolerance)) {
                value = lastKey.value;
            } else {
                const float valueDifference = previousKey.value - lastKey.value;
                const float slope = valueDifference / timeDifference;
                value = slope * (time - lastKey.time) + lastKey.value;
            }
        } else {
            value = lastKey.value;
        }
    }

    return value + offset;
}

} // namespace rocks::curves_detail
