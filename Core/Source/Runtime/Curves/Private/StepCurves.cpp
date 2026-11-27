#include <Curves/StepCurves.h>

#include "CurveKeyEdit.h"

// 계단 커브 셋의 평가·편집이다(Docs/Design/CurveData/README.ko.md 5.8·7절 — UE IntegralCurve.cpp·
// StringCurve.cpp·NameCurve.cpp). 공통 연산은 CurveKeyEdit.h다.
namespace rocks {

namespace curves_detail {

Result<StringStepKey> CloneCurveKey(const StringStepKey& key, Allocator& allocator) noexcept {
    StringStepKey cloned(allocator);
    cloned.time = key.time;
    if (auto assigned = cloned.value.TryAssign(key.value.View()); !assigned) {
        return Result<StringStepKey>::Failure(assigned.Error());
    }

    return Result<StringStepKey>::Success(Move(cloned));
}

} // namespace curves_detail

namespace {

IntegerStepKey MakeIntegerKey(float time, int32 value) noexcept {
    IntegerStepKey key;
    key.time = time;
    key.value = value;
    return key;
}

NameStepKey MakeNameKey(float time, Name value) noexcept {
    NameStepKey key;
    key.time = time;
    key.value = value;
    return key;
}

// value를 담은 String 키다(배열의 allocator로 복사한다).
Result<StringStepKey> MakeStringKey(Allocator& allocator, float time, StringView value) noexcept {
    StringStepKey key(allocator);
    key.time = time;
    if (auto assigned = key.value.TryAssign(value); !assigned) {
        return Result<StringStepKey>::Failure(assigned.Error());
    }

    return Result<StringStepKey>::Success(Move(key));
}

// 키 배열 사본(복제)으로 keys를 바꾼다. 실패하면 keys는 그대로다.
template <class Key>
Result<void> CopyKeys(Array<Key>& keys, const Array<Key>& source) noexcept {
    Array<Key> next(keys.GetAllocator());
    if (auto reserved = next.TryReserve(source.Size()); !reserved) {
        return reserved;
    }

    if (auto appended = curves_detail::AppendCloned(next, source.View()); !appended) {
        return appended;
    }

    keys = Move(next);
    return Result<void>::Success();
}

template <class Key>
Result<void> RemoveAt(Array<Key>& keys, usize index) noexcept {
    if (index >= keys.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return keys.TryRemoveAt(index);
}

} // namespace

// ---- IntegerStepCurve ----

Result<void> IntegerStepCurve::TryCopyFrom(const IntegerStepCurve& other) noexcept {
    if (this == &other) {
        return Result<void>::Success();
    }

    if (auto copied = CopyKeys(keys_, other.keys_); !copied) {
        return copied;
    }

    defaultValue = other.defaultValue;
    useDefaultBeforeFirstKey = other.useDefaultBeforeFirstKey;
    return Result<void>::Success();
}

int32 IntegerStepCurve::Evaluate(float time, int32 fallback) const noexcept {
    const usize index = curves_detail::StepKeyIndex(keys_.View(), time, useDefaultBeforeFirstKey);
    if (index != IndexNone) {
        return keys_[index].value;
    }

    return defaultValue.HasValue() ? defaultValue.Value() : fallback;
}

usize IntegerStepCurve::FindKey(float time, float tolerance) const noexcept {
    return curves_detail::FindWithinTolerance(keys_.View(), time, tolerance);
}

usize IntegerStepCurve::FindKeyAtOrBefore(float time) const noexcept {
    return curves_detail::FindAtOrBefore(keys_.View(), time);
}

Result<Interval<float>> IntegerStepCurve::TryGetTimeRange() const noexcept {
    return curves_detail::TimeRange(keys_.View());
}

Result<usize> IntegerStepCurve::TryAddKey(float time, int32 value) noexcept {
    return curves_detail::AddKey(keys_, MakeIntegerKey(time, value), false);
}

Result<usize> IntegerStepCurve::TryUpdateOrAddKey(float time, int32 value,
                                                  float tolerance) noexcept {
    return curves_detail::UpdateOrAddKey(keys_, MakeIntegerKey(time, value), false, tolerance);
}

Result<void> IntegerStepCurve::TryRemoveKey(usize index) noexcept {
    return RemoveAt(keys_, index);
}

Result<usize> IntegerStepCurve::TrySetKeyTime(usize index, float time) noexcept {
    return curves_detail::SetKeyTime(keys_, index, time);
}

Result<void> IntegerStepCurve::TrySetKeyValue(usize index, int32 value) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    keys_[index].value = value;
    return Result<void>::Success();
}

Result<void> IntegerStepCurve::TrySetKeys(Span<const IntegerStepKey> keys) noexcept {
    return curves_detail::SetKeys(keys_, keys);
}

void IntegerStepCurve::Reset() noexcept {
    keys_.Clear();
}

Result<void> IntegerStepCurve::TryShiftTime(float delta) noexcept {
    return curves_detail::ShiftTime(keys_, delta);
}

Result<void> IntegerStepCurve::TryScaleTime(float origin, float factor) noexcept {
    return curves_detail::ScaleTime(keys_, origin, factor);
}

Result<void> IntegerStepCurve::TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept {
    return curves_detail::ShiftKeyTimes(keys_, indices, delta);
}

Result<void> IntegerStepCurve::TryRemoveRedundantKeys() noexcept {
    return curves_detail::RemoveStepRedundantKeys(
        keys_, [](int32 left, int32 right) noexcept { return left == right; });
}

Result<void> IntegerStepCurve::TryRemoveRedundantKeys(Array<usize>& keptIndices) noexcept {
    return curves_detail::RemoveStepRedundantKeys(
        keys_, [](int32 left, int32 right) noexcept { return left == right; }, &keptIndices);
}

Result<void> IntegerStepCurve::Validate(const IntegerStepCurve& curve) noexcept {
    return curves_detail::ValidateKeys(curve.keys_.View());
}

// ---- NameStepCurve ----

Result<void> NameStepCurve::TryCopyFrom(const NameStepCurve& other) noexcept {
    if (this == &other) {
        return Result<void>::Success();
    }

    if (auto copied = CopyKeys(keys_, other.keys_); !copied) {
        return copied;
    }

    defaultValue = other.defaultValue;
    return Result<void>::Success();
}

Name NameStepCurve::Evaluate(float time, Name fallback) const noexcept {
    const usize index = curves_detail::StepKeyIndex(keys_.View(), time, true);
    if (index != IndexNone) {
        return keys_[index].value;
    }

    return defaultValue.HasValue() ? defaultValue.Value() : fallback;
}

usize NameStepCurve::FindKey(float time, float tolerance) const noexcept {
    return curves_detail::FindWithinTolerance(keys_.View(), time, tolerance);
}

usize NameStepCurve::FindKeyAtOrBefore(float time) const noexcept {
    return curves_detail::FindAtOrBefore(keys_.View(), time);
}

Result<Interval<float>> NameStepCurve::TryGetTimeRange() const noexcept {
    return curves_detail::TimeRange(keys_.View());
}

Result<usize> NameStepCurve::TryAddKey(float time, Name value) noexcept {
    return curves_detail::AddKey(keys_, MakeNameKey(time, value), false);
}

Result<usize> NameStepCurve::TryUpdateOrAddKey(float time, Name value, float tolerance) noexcept {
    return curves_detail::UpdateOrAddKey(keys_, MakeNameKey(time, value), false, tolerance);
}

Result<void> NameStepCurve::TryRemoveKey(usize index) noexcept {
    return RemoveAt(keys_, index);
}

Result<usize> NameStepCurve::TrySetKeyTime(usize index, float time) noexcept {
    return curves_detail::SetKeyTime(keys_, index, time);
}

Result<void> NameStepCurve::TrySetKeyValue(usize index, Name value) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    keys_[index].value = value;
    return Result<void>::Success();
}

Result<void> NameStepCurve::TrySetKeys(Span<const NameStepKey> keys) noexcept {
    return curves_detail::SetKeys(keys_, keys);
}

void NameStepCurve::Reset() noexcept {
    keys_.Clear();
}

Result<void> NameStepCurve::TryShiftTime(float delta) noexcept {
    return curves_detail::ShiftTime(keys_, delta);
}

Result<void> NameStepCurve::TryScaleTime(float origin, float factor) noexcept {
    return curves_detail::ScaleTime(keys_, origin, factor);
}

Result<void> NameStepCurve::TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept {
    return curves_detail::ShiftKeyTimes(keys_, indices, delta);
}

namespace {

// Name 중복 제거의 같음이다: 토큰의 모든 칸(domain·id·displayId·number·kind)이 같아야 한다 — 키
// 핸들 표 H2의 Name 비교(SameKeyBits)와 같은 판단이다. 대소문자만 다른 이름(표시 철자가 다르다)을
// 지우면 평가 결과의 표시 철자가 바뀌고, None 계열은 Name ==가 domain을 보지 않으므로 칸을 직접
// 비교한다.
bool SameNameSpelling(Name left, Name right) noexcept {
    return left.domain == right.domain && left.id == right.id &&
           left.displayId == right.displayId && left.number == right.number &&
           left.kind == right.kind;
}

} // namespace

Result<void> NameStepCurve::TryRemoveRedundantKeys() noexcept {
    return curves_detail::RemoveStepRedundantKeys(keys_, SameNameSpelling);
}

Result<void> NameStepCurve::TryRemoveRedundantKeys(Array<usize>& keptIndices) noexcept {
    return curves_detail::RemoveStepRedundantKeys(keys_, SameNameSpelling, &keptIndices);
}

Result<void> NameStepCurve::Validate(const NameStepCurve& curve) noexcept {
    return curves_detail::ValidateKeys(curve.keys_.View());
}

// ---- StringStepCurve ----

Result<void> StringStepCurve::TryCopyFrom(const StringStepCurve& other) noexcept {
    if (this == &other) {
        return Result<void>::Success();
    }

    Allocator& allocator = keys_.GetAllocator();
    Optional<String> nextDefault;
    if (other.defaultValue.HasValue()) {
        auto cloned = other.defaultValue.Value().TryClone(allocator);
        if (!cloned) {
            return Result<void>::Failure(cloned.Error());
        }

        nextDefault.Emplace(Move(cloned.Value()));
    }

    if (auto copied = CopyKeys(keys_, other.keys_); !copied) {
        return copied;
    }

    defaultValue = Move(nextDefault);
    return Result<void>::Success();
}

StringView StringStepCurve::Evaluate(float time, StringView fallback) const noexcept {
    const usize index = curves_detail::StepKeyIndex(keys_.View(), time, true);
    if (index != IndexNone) {
        return keys_[index].value.View();
    }

    return defaultValue.HasValue() ? defaultValue.Value().View() : fallback;
}

usize StringStepCurve::FindKey(float time, float tolerance) const noexcept {
    return curves_detail::FindWithinTolerance(keys_.View(), time, tolerance);
}

usize StringStepCurve::FindKeyAtOrBefore(float time) const noexcept {
    return curves_detail::FindAtOrBefore(keys_.View(), time);
}

Result<Interval<float>> StringStepCurve::TryGetTimeRange() const noexcept {
    return curves_detail::TimeRange(keys_.View());
}

Result<usize> StringStepCurve::TryAddKey(float time, StringView value) noexcept {
    if (!math::IsFinite(time)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto key = MakeStringKey(keys_.GetAllocator(), time, value);
    if (!key) {
        return Result<usize>::Failure(key.Error());
    }

    return curves_detail::AddKey(keys_, Move(key.Value()), false);
}

Result<usize> StringStepCurve::TryUpdateOrAddKey(float time, StringView value,
                                                 float tolerance) noexcept {
    if (!math::IsFinite(time) || !curves_detail::IsUsableTolerance(tolerance)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto key = MakeStringKey(keys_.GetAllocator(), time, value);
    if (!key) {
        return Result<usize>::Failure(key.Error());
    }

    return curves_detail::UpdateOrAddKey(keys_, Move(key.Value()), false, tolerance);
}

Result<void> StringStepCurve::TryRemoveKey(usize index) noexcept {
    return RemoveAt(keys_, index);
}

Result<usize> StringStepCurve::TrySetKeyTime(usize index, float time) noexcept {
    return curves_detail::SetKeyTime(keys_, index, time);
}

Result<void> StringStepCurve::TrySetKeyValue(usize index, StringView value) noexcept {
    if (index >= keys_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 새 문자열을 먼저 만들어 실패해도 키가 그대로다.
    String next(keys_.GetAllocator());
    if (auto assigned = next.TryAssign(value); !assigned) {
        return assigned;
    }

    keys_[index].value = Move(next);
    return Result<void>::Success();
}

Result<void> StringStepCurve::TrySetKeys(Span<const StringStepKey> keys) noexcept {
    return curves_detail::SetKeys(keys_, keys);
}

void StringStepCurve::Reset() noexcept {
    keys_.Clear();
}

Result<void> StringStepCurve::TryShiftTime(float delta) noexcept {
    return curves_detail::ShiftTime(keys_, delta);
}

Result<void> StringStepCurve::TryScaleTime(float origin, float factor) noexcept {
    return curves_detail::ScaleTime(keys_, origin, factor);
}

Result<void> StringStepCurve::TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept {
    return curves_detail::ShiftKeyTimes(keys_, indices, delta);
}

Result<void> StringStepCurve::TryRemoveRedundantKeys() noexcept {
    return curves_detail::RemoveStepRedundantKeys(
        keys_, [](const String& left, const String& right) noexcept {
            return left.View() == right.View();
        });
}

Result<void> StringStepCurve::TryRemoveRedundantKeys(Array<usize>& keptIndices) noexcept {
    return curves_detail::RemoveStepRedundantKeys(
        keys_,
        [](const String& left, const String& right) noexcept {
            return left.View() == right.View();
        },
        &keptIndices);
}

Result<void> StringStepCurve::Validate(const StringStepCurve& curve) noexcept {
    return curves_detail::ValidateKeys(curve.keys_.View());
}

} // namespace rocks
