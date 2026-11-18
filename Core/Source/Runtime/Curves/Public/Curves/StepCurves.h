#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Interval.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Meta/Annotations.h>

// 계단 커브 셋이다(Docs/Design/CurveData.ko.md 2·4·5.8절 — CV6): int32 값 IntegerStepCurve(UE 5.8
// FIntegralCurve), Name 값 NameStepCurve(UE FNameCurve — Rocks가 기본값과 평가를 더한다, D9),
// String 값 StringStepCurve(UE FStringCurve). 키 사이를 보간하지 않고 시간 이하인 마지막 키 값을
// 낸다.
//
// 불변식·검증 hook·편집 연산의 실패 규약·스레드 규칙은 TangentCurve와 같다(TangentCurve.h): 키
// 시간은 유한·오름차순(같은 시간 키 허용)이고 키 배열은 private이라 C++ API와 검증 hook Validate가
// 함께 지킨다. 편집 연산은 실패하면 커브를 바꾸지 않는다. 시간 단위는 커브를 쓰는 쪽이 정한다(대개
// 초).
namespace rocks {

// IntegerStepCurve의 키다(UE FIntegralKey): 시간과 int32 값.
ROCKS_STRUCT(Atomic)
struct IntegerStepKey {
    ROCKS_PROPERTY() float time = 0.0f;
    ROCKS_PROPERTY() int32 value = 0;
};

// NameStepCurve의 키다(UE FNameCurveKey): 시간과 Name 값. Name 같음은 Rocks Name 규칙(ASCII
// 대소문자 무시 — Name.h)이고 표시 철자는 wire에 보존된다(설계 9절).
ROCKS_STRUCT(Atomic)
struct NameStepKey {
    ROCKS_PROPERTY() float time = 0.0f;
    ROCKS_PROPERTY() Name value;
};

// StringStepCurve의 키다(UE FStringCurveKey): 시간과 String 값(UTF-8). 값 문자열은 키를 담은 배열의
// allocator를 쓴다. 이동만 되고 복사는 CloneCurveKey(내부)·커브의 TryCopyFrom이 한다.
ROCKS_STRUCT(Atomic)
struct StringStepKey {
    explicit StringStepKey(Allocator& allocator) noexcept : value(allocator) {
    }

    ROCKS_PROPERTY() float time = 0.0f;
    ROCKS_PROPERTY() String value;
};

// int32 값 계단 커브다(UE FIntegralCurve).
//
// 평가(Evaluate, 설계 5.8절 — UE FIntegralCurve::Evaluate):
// - 키가 없거나, useDefaultBeforeFirstKey이고 첫 키 시간보다 앞이면 defaultValue(없으면 fallback).
// - 키가 하나이거나 첫 키 시간보다 앞이면 첫 키 값.
// - 마지막 키 시간보다 앞이면 시간 이하인 마지막 키 값(같은 시간 키가 여럿이면 그 시간의 마지막
// 키).
// - 그 밖은 마지막 키 값.
// - NaN 시간은 키가 있어도 defaultValue(없으면 fallback)다(UE는 마지막 키 값 — 설계 17.5절).
// 기본값의 "없음"은 Optional이다(UE DefaultValue = MAX_int32 표지 대신 — D6).
//
// 사용 예:
//   IntegerStepCurve curve(allocator);
//   if (auto added = curve.TryAddKey(1.0f, 3); !added) { return added.Error(); }
//   const int32 value = curve.Evaluate(2.0f);   // 3
ROCKS_STRUCT()
struct IntegerStepCurve {
    ROCKS_GENERATED_BODY()
public:
    // 키 없는 커브다(기본값 없음, useDefaultBeforeFirstKey 거짓). allocator는 키 배열이 쓴다.
    explicit IntegerStepCurve(Allocator& allocator) noexcept : keys_(allocator) {
    }

    // 키가 없거나(또는 아래 조건에서) 평가가 내는 값이다. 없으면 Evaluate의 fallback이다.
    ROCKS_PROPERTY() Optional<int32> defaultValue;
    // 참이면 첫 키 시간보다 앞의 평가가 첫 키 값 대신 기본값이다(UE
    // bUseDefaultValueBeforeFirstKey).
    ROCKS_PROPERTY() bool useDefaultBeforeFirstKey = false;

    // other의 칸과 키를 복사한다. 실패(OutOfMemory)면 이 커브는 그대로다.
    Result<void> TryCopyFrom(const IntegerStepCurve& other) noexcept;

    // 시간 오름차순 키다. 다음 편집 연산까지 유효하다.
    Span<const IntegerStepKey> Keys() const noexcept {
        return keys_.View();
    }

    usize KeyCount() const noexcept {
        return keys_.Size();
    }

    // time의 값이다(위 평가 규칙). 할당하지 않는다.
    int32 Evaluate(float time, int32 fallback = 0) const noexcept;

    // |키 시간 − time| ≤ tolerance인 가장 앞 키의 index다(없거나 NaN·음수 tolerance면 IndexNone —
    // UE FindKey는 이진 탐색 경로에 따라 정해진다, 설계 15절 6).
    usize FindKey(float time, float tolerance = 1.0e-4f) const noexcept;

    // 시간이 time 이하인 마지막 키의 index다(없거나 NaN이면 IndexNone — UE FindKeyBeforeOrAt).
    usize FindKeyAtOrBefore(float time) const noexcept;

    // [첫 키 시간, 마지막 키 시간]이다. 실패: 키가 없으면 NotFound.
    Result<Interval<float>> TryGetTimeRange() const noexcept;

    // (time, value) 키를 같은 시간 키들 앞에 넣고 index를 돌려준다(UE AddKey). 실패: time이
    // 비유한이면 InvalidArgument, 할당 실패면 OutOfMemory.
    Result<usize> TryAddKey(float time, int32 value) noexcept;

    // |키 시간 − time| ≤ tolerance인 가장 앞 키가 있으면 그 값만 바꾸고 없으면 TryAddKey다(UE
    // UpdateOrAddKey). 실패: time이 비유한이거나 tolerance가 NaN·음수면 InvalidArgument, 할당
    // 실패면 OutOfMemory.
    Result<usize> TryUpdateOrAddKey(float time, int32 value, float tolerance = 1.0e-4f) noexcept;

    // index 키를 지운다. 실패: index가 범위 밖이면 InvalidArgument.
    Result<void> TryRemoveKey(usize index) noexcept;

    // index 키의 시간을 바꾸고(같은 시간 키들 앞 자리로) 새 index를 돌려준다. 할당하지 않는다.
    // 실패: index가 범위 밖이거나 time이 비유한이면 InvalidArgument.
    Result<usize> TrySetKeyTime(usize index, float time) noexcept;

    // index 키의 값을 바꾼다. 실패: index가 범위 밖이면 InvalidArgument.
    Result<void> TrySetKeyValue(usize index, int32 value) noexcept;

    // 키 전체를 keys로 바꾼다. 실패: 시간이 비유한이거나 오름차순이 아니면 InvalidArgument, 할당
    // 실패면 OutOfMemory.
    Result<void> TrySetKeys(Span<const IntegerStepKey> keys) noexcept;

    // 키만 비운다(기본값·useDefaultBeforeFirstKey는 그대로).
    void Reset() noexcept;

    // 모든 키 시간을 옮기거나(delta) origin 기준 factor배 한다(TangentCurve와 같은 규칙).
    Result<void> TryShiftTime(float delta) noexcept;
    Result<void> TryScaleTime(float origin, float factor) noexcept;

    // 고른 키만 delta만큼 옮긴다(TangentCurve::TryShiftKeyTimes와 같은 규칙).
    Result<void> TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept;

    // 앞·뒤 키와 값이 같은 안쪽 키를 지운다(UE RemoveRedundantKeys — 평가 결과가 바뀌지 않는다).
    // UE와 달리 남은 키가 하나여도 그 키를 기본값으로 바꾸지 않는다(설계 15절 9 — UE는 키를 지우고
    // DefaultValue로 옮겨 useDefaultBeforeFirstKey일 때 평가가 바뀐다). 실패: 할당 실패면
    // OutOfMemory.
    Result<void> TryRemoveRedundantKeys() noexcept;

    // 위와 같고, 남긴 키의 원래 index(오름차순 — 순서를 보존하는 부분열)를 keptIndices에 담는다(키
    // 핸들 표 CurveKeyHandleTable::TryRemoveRedundantKeys가 남은 키의 핸들을 맞추는 데 쓴다). 지운
    // 키가 없으면 0..N−1이다. keptIndices는 그 allocator로 다시 채운다. 실패하면 커브와
    // keptIndices가 그대로다.
    Result<void> TryRemoveRedundantKeys(Array<usize>& keptIndices) noexcept;

    // 리플렉션 경로의 검증 hook이다: 키 시간이 모두 유한하고 오름차순이면 Success, 아니면
    // InvalidArgument.
    static Result<void> Validate(const IntegerStepCurve& curve) noexcept;

private:
    ROCKS_PROPERTY(Name = "keys") Array<IntegerStepKey> keys_;
};

// Name 값 계단 커브다(UE FNameCurve). UE에는 평가 함수도 기본값 칸도 없다. Rocks는 기본값을 두고
// StringStepCurve와 같은 규칙으로 평가한다(D9):
// - 키가 없거나 첫 키 시간보다 앞이면(늘) defaultValue(없으면 fallback).
// - 마지막 키 시간보다 앞이면 시간 이하인 마지막 키 값, 그 밖은 마지막 키 값.
// - NaN 시간은 defaultValue(없으면 fallback)다.
// 값 같음(중복 키 제거)은 Rocks Name 규칙이다(ASCII 대소문자만 다른 이름은 같다).
//
// 사용 예:
//   NameStepCurve curve(allocator);
//   if (auto added = curve.TryAddKey(0.5f, pool.TryIntern("Walk").Value()); !added) { ... }
//   const Name state = curve.Evaluate(1.0f);   // Walk
ROCKS_STRUCT()
struct NameStepCurve {
    ROCKS_GENERATED_BODY()
public:
    explicit NameStepCurve(Allocator& allocator) noexcept : keys_(allocator) {
    }

    // 키가 없거나 첫 키 시간보다 앞일 때의 값이다. 없으면 Evaluate의 fallback이다.
    ROCKS_PROPERTY() Optional<Name> defaultValue;

    Result<void> TryCopyFrom(const NameStepCurve& other) noexcept;

    Span<const NameStepKey> Keys() const noexcept {
        return keys_.View();
    }

    usize KeyCount() const noexcept {
        return keys_.Size();
    }

    // time의 값이다(위 평가 규칙). 할당하지 않는다.
    Name Evaluate(float time, Name fallback = {}) const noexcept;

    // 편집·조회 규칙은 IntegerStepCurve의 같은 이름 연산과 같다.
    usize FindKey(float time, float tolerance = 1.0e-4f) const noexcept;
    usize FindKeyAtOrBefore(float time) const noexcept;
    Result<Interval<float>> TryGetTimeRange() const noexcept;
    Result<usize> TryAddKey(float time, Name value) noexcept;
    Result<usize> TryUpdateOrAddKey(float time, Name value, float tolerance = 1.0e-4f) noexcept;
    Result<void> TryRemoveKey(usize index) noexcept;
    Result<usize> TrySetKeyTime(usize index, float time) noexcept;
    Result<void> TrySetKeyValue(usize index, Name value) noexcept;
    Result<void> TrySetKeys(Span<const NameStepKey> keys) noexcept;
    void Reset() noexcept;
    Result<void> TryShiftTime(float delta) noexcept;
    Result<void> TryScaleTime(float origin, float factor) noexcept;
    Result<void> TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept;

    // 앞·뒤 키와 값(Name 규칙)이 같은 안쪽 키를 지운다. UE FNameCurve에는 없는 연산이다(String과
    // 같은 규칙으로 더한다 — 남은 키를 기본값으로 바꾸지 않는다).
    Result<void> TryRemoveRedundantKeys() noexcept;

    // 위와 같고, 남긴 키의 원래 index(오름차순 — 순서를 보존하는 부분열)를 keptIndices에 담는다(키
    // 핸들 표 CurveKeyHandleTable::TryRemoveRedundantKeys가 남은 키의 핸들을 맞추는 데 쓴다). 지운
    // 키가 없으면 0..N−1이다. keptIndices는 그 allocator로 다시 채운다. 실패하면 커브와
    // keptIndices가 그대로다.
    Result<void> TryRemoveRedundantKeys(Array<usize>& keptIndices) noexcept;

    static Result<void> Validate(const NameStepCurve& curve) noexcept;

private:
    ROCKS_PROPERTY(Name = "keys") Array<NameStepKey> keys_;
};

// String 값 계단 커브다(UE FStringCurve). 평가는 NameStepCurve와 같은 규칙이다(첫 키 이전은 늘
// 기본값 — UE 그대로). 기본값의 "없음"은 Optional이다(UE는 빈 문자열이 "없음"이라 빈 문자열을
// 기본값으로 둘 수 없다 — Rocks는 빈 문자열도 기본값이다). 값 같음(중복 키 제거)은 byte 비교다(UE
// FString ==는 대소문자를 무시한다 — 설계 15절 10).
//
// Evaluate가 돌려주는 StringView는 키 값·기본값(다음 편집까지) 또는 fallback(호출자 수명)을
// 가리킨다.
//
// 사용 예:
//   StringStepCurve curve(allocator);
//   if (auto added = curve.TryAddKey(0.0f, "Intro"); !added) { return added.Error(); }
//   const StringView text = curve.Evaluate(3.0f);   // "Intro"
ROCKS_STRUCT()
struct StringStepCurve {
    ROCKS_GENERATED_BODY()
public:
    explicit StringStepCurve(Allocator& allocator) noexcept : keys_(allocator) {
    }

    // 키가 없거나 첫 키 시간보다 앞일 때의 값이다. 없으면 Evaluate의 fallback이다. 값 문자열의
    // allocator는 채우는 쪽이 정한다(TryCopyFrom은 이 커브 키 배열의 allocator로 복제한다).
    ROCKS_PROPERTY() Optional<String> defaultValue;

    // other의 기본값·키를 이 커브의 allocator로 복제한다. 실패(OutOfMemory)면 이 커브는 그대로다.
    Result<void> TryCopyFrom(const StringStepCurve& other) noexcept;

    Span<const StringStepKey> Keys() const noexcept {
        return keys_.View();
    }

    usize KeyCount() const noexcept {
        return keys_.Size();
    }

    // time의 값이다(위 평가 규칙). 할당하지 않는다.
    StringView Evaluate(float time, StringView fallback = {}) const noexcept;

    // 편집·조회 규칙은 IntegerStepCurve의 같은 이름 연산과 같다. 값은 키 배열의 allocator로 복사해
    // 넣는다(할당 실패는 OutOfMemory이고 커브는 그대로다).
    usize FindKey(float time, float tolerance = 1.0e-4f) const noexcept;
    usize FindKeyAtOrBefore(float time) const noexcept;
    Result<Interval<float>> TryGetTimeRange() const noexcept;
    Result<usize> TryAddKey(float time, StringView value) noexcept;
    Result<usize> TryUpdateOrAddKey(float time, StringView value,
                                    float tolerance = 1.0e-4f) noexcept;
    Result<void> TryRemoveKey(usize index) noexcept;
    Result<usize> TrySetKeyTime(usize index, float time) noexcept;
    Result<void> TrySetKeyValue(usize index, StringView value) noexcept;
    Result<void> TrySetKeys(Span<const StringStepKey> keys) noexcept;
    void Reset() noexcept;
    Result<void> TryShiftTime(float delta) noexcept;
    Result<void> TryScaleTime(float origin, float factor) noexcept;
    Result<void> TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept;

    // 앞·뒤 키와 값(byte 비교)이 같은 안쪽 키를 지운다. 남은 키를 기본값으로 바꾸지 않는다(설계
    // 15절 9 — UE는 바꿔 첫 키 이전 평가가 늘 달라진다).
    Result<void> TryRemoveRedundantKeys() noexcept;

    // 위와 같고, 남긴 키의 원래 index(오름차순 — 순서를 보존하는 부분열)를 keptIndices에 담는다(키
    // 핸들 표 CurveKeyHandleTable::TryRemoveRedundantKeys가 남은 키의 핸들을 맞추는 데 쓴다). 지운
    // 키가 없으면 0..N−1이다. keptIndices는 그 allocator로 다시 채운다. 실패하면 커브와
    // keptIndices가 그대로다.
    Result<void> TryRemoveRedundantKeys(Array<usize>& keptIndices) noexcept;

    static Result<void> Validate(const StringStepCurve& curve) noexcept;

private:
    ROCKS_PROPERTY(Name = "keys") Array<StringStepKey> keys_;
};

} // namespace rocks
