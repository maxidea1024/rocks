#pragma once

#include <Base/NumericLimits.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/MathUtility.h>
#include <Core/Result.h>
#include <Curves/CurveTypes.h>
#include <Curves/PlainCurve.h>
#include <Curves/StepCurves.h>
#include <Curves/TangentCurve.h>
#include <Platform/Memory.h>

// 커브 키 핸들 표다(Docs/Design/CurveData.ko.md 8절, D3 — CV7, UE 5.8 FKeyHandle·FKeyHandleMap
// 대응). 편집 도구·undo가 키를 index 대신 안정된 핸들로 가리키게 한다. 핸들은 커브 값의 일부가
// 아니다 — 표는 값 밖의 편집 상태이고 커브를 쥐지 않는다(커브는 객체 필드·배열 원소 안에서 옮겨
// 다니므로 주소를 붙잡을 수 없다). 표의 모든 함수가 커브를 인자로 받는다.
namespace rocks {

// 키 하나를 가리키는 편집용 핸들이다(UE FKeyHandle). 값은 표마다 1부터 단조 증가하는 uint32이고 0은
// 무효다(Invalid). 핸들은 그 표 안에서만 뜻이 있다(다른 표의 핸들과 비교하지 않는다).
struct CurveKeyHandle {
    uint32 value = 0;

    static constexpr CurveKeyHandle Invalid() noexcept {
        return {};
    }

    constexpr bool IsValid() const noexcept {
        return value != 0;
    }

    friend constexpr bool operator==(CurveKeyHandle left, CurveKeyHandle right) noexcept {
        return left.value == right.value;
    }
};

namespace curves_detail {

struct CurveKeyHandleTableTestAccess;

inline bool SameFloatBits(float left, float right) noexcept {
    return platform::BitCast<uint32>(left) == platform::BitCast<uint32>(right);
}

// 표 사본과 커브 키가 비트까지 같은지다(칸마다 — 구조체 padding은 보지 않는다). Name은 모든 칸(같음
// 규칙이 아니라 토큰 그대로), String은 byte다.
inline bool SameKeyBits(const TangentCurveKey& left, const TangentCurveKey& right) noexcept {
    return SameFloatBits(left.time, right.time) && SameFloatBits(left.value, right.value) &&
           left.interp == right.interp && left.tangentMode == right.tangentMode &&
           left.tangentWeight == right.tangentWeight &&
           SameFloatBits(left.arriveTangent, right.arriveTangent) &&
           SameFloatBits(left.arriveTangentWeight, right.arriveTangentWeight) &&
           SameFloatBits(left.leaveTangent, right.leaveTangent) &&
           SameFloatBits(left.leaveTangentWeight, right.leaveTangentWeight);
}

inline bool SameKeyBits(const PlainCurveKey& left, const PlainCurveKey& right) noexcept {
    return SameFloatBits(left.time, right.time) && SameFloatBits(left.value, right.value);
}

inline bool SameKeyBits(const IntegerStepKey& left, const IntegerStepKey& right) noexcept {
    return SameFloatBits(left.time, right.time) && left.value == right.value;
}

inline bool SameKeyBits(const NameStepKey& left, const NameStepKey& right) noexcept {
    return SameFloatBits(left.time, right.time) && left.value.domain == right.value.domain &&
           left.value.id == right.value.id && left.value.displayId == right.value.displayId &&
           left.value.number == right.value.number && left.value.kind == right.value.kind;
}

inline bool SameKeyBits(const StringStepKey& left, const StringStepKey& right) noexcept {
    return SameFloatBits(left.time, right.time) && left.value.View() == right.value.View();
}

// 표 사본용 키 복제다(비트 복사 가능한 키는 그대로, String 키는 allocator로 값을 복제한다).
template <class Key>
Result<Key> CloneTableKey(const Key& key, Allocator& allocator) noexcept {
    if constexpr (IsTriviallyCopyableV<Key>) {
        static_cast<void>(allocator);
        return Result<Key>::Success(key);
    } else {
        Key cloned(allocator);
        cloned.time = key.time;
        if (auto assigned = cloned.value.TryAssign(key.value.View()); !assigned) {
            return Result<Key>::Failure(assigned.Error());
        }

        return Result<Key>::Success(Move(cloned));
    }
}

} // namespace curves_detail

// Curve(TangentCurve·PlainCurve·IntegerStepCurve·NameStepCurve·StringStepCurve)의 키 핸들 표다(UE
// FKeyHandleMap — 설계 8절, D3).
//
// - 표는 index 순서의 핸들 목록과 마지막으로 맞춘 키 배열 전체의 사본을 쥔다. 모든 함수는 먼저
// 인자로
//   받은 커브의 키 배열을 사본과 칸마다 비트 비교한다(O(N)). 다르면(표를 거치지 않은 수정 — C++
//   API·리플렉션 경로 쓰기·대입 묶음·undo) 표 전체를 무효로 표시하고 StaleHandle을 낸다. 무효가 된
//   표는 TryRebuild가 새 핸들을 발급할 때까지 모든 함수가 StaleHandle이다(UE처럼 개수만 맞춰 다른
//   키를 가리키게 두지 않는다). 새로 만든 표는 빈 사본이라 빈 커브에만 맞고, 키가 있는 커브에는
//   먼저 TryRebuild를 부른다.
// - 핸들은 표마다 1부터 단조 증가하는 uint32이고 0은 무효다. 마지막 핸들이 uint32 최댓값이면 새
// 핸들
//   발급이 Overflow다(UE는 전역 atomic이고 쿡 중에는 0을 건너뛰며 순환한다).
// - 편집 함수는 커브와 표를 함께 바꾸고 키 순서가 바뀌면 핸들이 키를 따라간다. 실패하면 커브와 표가
//   모두 그대로다(String 값은 커브를 바꾸기 전에 사본 키를 먼저 만든다). 예외는
//   TryEditKeys다(실패한 편집 함수가 앞서 바꾼 칸은 남고 표는 그 상태에 맞춘다).
// - 저장하지 않는다(UE와 같다 — undo 기록은 CU4 단계가 정한다). 복사는 TryCopyFrom(핸들·사본·발급
//   카운터 그대로), 이동은 noexcept다. 동기화는 없다.
// - 핸들 → index 조회는 핸들 목록의 선형 탐색이다(사본 비교와 같은 O(N) — 편집기 빈도).
//
// 사용 예:
//   CurveKeyHandleTable<TangentCurve> table(allocator);
//   if (auto rebuilt = table.TryRebuild(curve); !rebuilt) { return rebuilt; }
//   auto handle = table.TryAddKey(curve, 1.0f, 3.0f);          // 새 키와 그 핸들
//   if (!handle) { return Result<void>::Failure(handle.Error()); }
//   auto moved = table.TrySetKeyTime(curve, handle.Value(), 4.0f);   // 핸들은 키를 따라간다
template <class Curve>
class CurveKeyHandleTable {
public:
    using Key = RemoveCvRefT<decltype(DeclVal<const Curve&>().Keys()[0])>;

    // 빈 표다(빈 사본 — 빈 커브에 맞는다). allocator는 핸들 목록과 사본이 쓴다.
    explicit CurveKeyHandleTable(Allocator& allocator) noexcept
        : handles_(allocator), snapshot_(allocator) {
    }

    CurveKeyHandleTable(const CurveKeyHandleTable&) = delete;
    CurveKeyHandleTable& operator=(const CurveKeyHandleTable&) = delete;
    CurveKeyHandleTable(CurveKeyHandleTable&&) noexcept = default;
    CurveKeyHandleTable& operator=(CurveKeyHandleTable&&) noexcept = default;

    // other의 핸들·사본·발급 카운터·무효 표시를 그대로 복사한다(복제된 편집 세션이 같은 키를 같은
    // 핸들로 가리킨다 — 설계 8절). 실패(OutOfMemory)면 이 표는 그대로다.
    Result<void> TryCopyFrom(const CurveKeyHandleTable& other) noexcept {
        if (this == &other) {
            return Result<void>::Success();
        }

        Array<CurveKeyHandle> handles(handles_.GetAllocator());
        Array<Key> snapshot(snapshot_.GetAllocator());
        if (auto reserved = handles.TryReserve(other.handles_.Size()); !reserved) {
            return reserved;
        }

        if (auto reserved = snapshot.TryReserve(other.snapshot_.Size()); !reserved) {
            return reserved;
        }

        static_cast<void>(handles.TryAppend(other.handles_.View())); // 예약했으므로 실패하지 않는다
        for (const Key& key : other.snapshot_.View()) {
            auto cloned = curves_detail::CloneTableKey(key, snapshot.GetAllocator());
            if (!cloned) {
                return Result<void>::Failure(cloned.Error());
            }

            static_cast<void>(snapshot.TryPushBack(Move(cloned.Value())));
        }

        handles_ = Move(handles);
        snapshot_ = Move(snapshot);
        lastIssued_ = other.lastIssued_;
        stale_ = other.stale_;
        return Result<void>::Success();
    }

    // 표가 무효인지다(표를 거치지 않은 수정을 감지한 뒤 TryRebuild 전까지 참).
    bool IsStale() const noexcept {
        return stale_;
    }

    // 핸들 수(= 맞춘 키 수)다.
    usize Count() const noexcept {
        return handles_.Size();
    }

    // 커브의 모든 키에 새 핸들을 발급하고 사본을 다시 맞춘다(무효 표시를 지운다). 옛 핸들은 더는
    // 찾을 수 없다(발급 카운터는 이어진다 — 옛 핸들 값이 다른 키를 가리키지 않는다).
    // 실패: 핸들이 uint32를 넘으면 Overflow, 할당 실패면 OutOfMemory(표는 그대로).
    Result<void> TryRebuild(const Curve& curve) noexcept {
        const Span<const Key> keys = curve.Keys();
        if (keys.Size() > static_cast<usize>(NumericLimits<uint32>::Max() - lastIssued_)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        Array<CurveKeyHandle> handles(handles_.GetAllocator());
        Array<Key> snapshot(snapshot_.GetAllocator());
        if (auto reserved = handles.TryReserve(keys.Size()); !reserved) {
            return reserved;
        }

        if (auto reserved = snapshot.TryReserve(keys.Size()); !reserved) {
            return reserved;
        }

        uint32 issued = lastIssued_;
        for (const Key& key : keys) {
            auto cloned = curves_detail::CloneTableKey(key, snapshot.GetAllocator());
            if (!cloned) {
                return Result<void>::Failure(cloned.Error());
            }

            static_cast<void>(snapshot.TryPushBack(Move(cloned.Value())));
            static_cast<void>(handles.TryPushBack(CurveKeyHandle{++issued}));
        }

        handles_ = Move(handles);
        snapshot_ = Move(snapshot);
        lastIssued_ = issued;
        stale_ = false;
        return Result<void>::Success();
    }

    // index 키의 핸들이다(UE GetKeyHandle). 실패: 표가 무효이거나 커브가 사본과 다르면 StaleHandle,
    // index가 범위 밖이면 InvalidArgument.
    Result<CurveKeyHandle> TryGetHandle(const Curve& curve, usize index) noexcept {
        if (auto checked = Check(curve); !checked) {
            return Result<CurveKeyHandle>::Failure(checked.Error());
        }

        if (index >= handles_.Size()) {
            return Result<CurveKeyHandle>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<CurveKeyHandle>::Success(handles_[index]);
    }

    // 핸들이 가리키는 키의 지금 index다(UE GetIndex). 실패: StaleHandle(위와 같다), 핸들이 이 표에
    // 없으면(지운 키·무효 0·다른 표) NotFound.
    Result<usize> TryFindIndex(const Curve& curve, CurveKeyHandle handle) noexcept {
        if (auto checked = Check(curve); !checked) {
            return Result<usize>::Failure(checked.Error());
        }

        return Find(handle);
    }

    // curve.TryAddKey(time, arguments...)로 키를 더하고 새 핸들을 돌려준다(UE AddKey가 핸들을
    // 돌려주는 것과 같다). arguments는 커브의 TryAddKey 나머지 인자다(값, TangentCurve·PlainCurve는
    // unwindRotation도). 실패: StaleHandle, 핸들 발급 Overflow, 커브 TryAddKey의 오류(비유한 시간
    // InvalidArgument·OutOfMemory) — 실패하면 커브와 표가 그대로다.
    template <class Value, class... Rest>
    Result<CurveKeyHandle> TryAddKey(Curve& curve, float time, Value&& value,
                                     Rest&&... rest) noexcept {
        if (auto checked = Check(curve); !checked) {
            return Result<CurveKeyHandle>::Failure(checked.Error());
        }

        if (lastIssued_ == NumericLimits<uint32>::Max()) {
            return Result<CurveKeyHandle>::Failure(ErrorCode::Overflow);
        }

        auto pending = PrepareInsert(time, value);
        if (!pending) {
            return Result<CurveKeyHandle>::Failure(pending.Error());
        }

        auto added = curve.TryAddKey(time, Forward<Value>(value), Forward<Rest>(rest)...);
        if (!added) {
            return Result<CurveKeyHandle>::Failure(added.Error());
        }

        const CurveKeyHandle handle{++lastIssued_};
        Commit(curve, Move(pending.Value()), added.Value(), handle);
        return Result<CurveKeyHandle>::Success(handle);
    }

    // curve.TryUpdateOrAddKey(time, arguments...)다(UE UpdateOrAddKey). 허용오차 안의 키 값을
    // 바꿨으면 그 키의 핸들, 키를 더했으면 새 핸들을 돌려준다. 실패는 TryAddKey와 같다(값만 바꾸는
    // 경우에는 Overflow가 아니다). 더할지는 커브 연산이 정한다 — 발급 카운터가 끝인데 키가
    // 더해졌으면 그 키를 지워 되돌리고 Overflow다(허용오차 인자의 형식과 무관하다).
    template <class Value, class... Rest>
    Result<CurveKeyHandle> TryUpdateOrAddKey(Curve& curve, float time, Value&& value,
                                             Rest&&... rest) noexcept {
        if (auto checked = Check(curve); !checked) {
            return Result<CurveKeyHandle>::Failure(checked.Error());
        }

        const usize before = curve.KeyCount();
        auto pending = PrepareInsert(time, value);
        if (!pending) {
            return Result<CurveKeyHandle>::Failure(pending.Error());
        }

        auto updated = curve.TryUpdateOrAddKey(time, Forward<Value>(value), Forward<Rest>(rest)...);
        if (!updated) {
            return Result<CurveKeyHandle>::Failure(updated.Error());
        }

        const usize index = updated.Value();
        if (curve.KeyCount() == before) {
            Refresh(curve, Move(pending.Value()), index);
            return Result<CurveKeyHandle>::Success(handles_[index]);
        }

        if (lastIssued_ == NumericLimits<uint32>::Max()) {
            static_cast<void>(curve.TryRemoveKey(index)); // 더한 키를 지우면 커브가 그대로다
            return Result<CurveKeyHandle>::Failure(ErrorCode::Overflow);
        }

        const CurveKeyHandle handle{++lastIssued_};
        Commit(curve, Move(pending.Value()), index, handle);
        return Result<CurveKeyHandle>::Success(handle);
    }

    // 핸들의 키를 지운다(UE DeleteKey). 다른 키의 핸들은 그대로 같은 키를 가리킨다. 실패:
    // StaleHandle, NotFound.
    Result<void> TryRemoveKey(Curve& curve, CurveKeyHandle handle) noexcept {
        auto index = Locate(curve, handle);
        if (!index) {
            return Result<void>::Failure(index.Error());
        }

        if (auto removed = curve.TryRemoveKey(index.Value()); !removed) {
            return removed;
        }

        static_cast<void>(handles_.TryRemoveAt(index.Value())); // 줄이기라 실패하지 않는다
        static_cast<void>(snapshot_.TryRemoveAt(index.Value()));
        return Result<void>::Success();
    }

    // 핸들의 키 시간을 바꾸고 새 index를 돌려준다(UE SetKeyTime — 핸들이 키를 따라간다). 할당하지
    // 않는다. 실패: StaleHandle, NotFound, 커브 TrySetKeyTime의 오류(비유한 시간 InvalidArgument).
    Result<usize> TrySetKeyTime(Curve& curve, CurveKeyHandle handle, float time) noexcept {
        auto index = Locate(curve, handle);
        if (!index) {
            return Result<usize>::Failure(index.Error());
        }

        auto moved = curve.TrySetKeyTime(index.Value(), time);
        if (!moved) {
            return moved;
        }

        MoveEntry(index.Value(), moved.Value());
        snapshot_[moved.Value()].time = curve.Keys()[moved.Value()].time;
        return moved;
    }

    // 핸들의 키 값을 바꾼다(UE SetKeyValue). 실패: StaleHandle, NotFound, 커브의 오류(String 할당
    // 실패 OutOfMemory — 커브와 표 그대로).
    template <class Value>
    Result<void> TrySetKeyValue(Curve& curve, CurveKeyHandle handle, Value&& value) noexcept {
        auto index = Locate(curve, handle);
        if (!index) {
            return Result<void>::Failure(index.Error());
        }

        const float time = curve.Keys()[index.Value()].time;
        auto pending = PrepareInsert(time, value);
        if (!pending) {
            return Result<void>::Failure(pending.Error());
        }

        if (auto set = curve.TrySetKeyValue(index.Value(), Forward<Value>(value)); !set) {
            return set;
        }

        Refresh(curve, Move(pending.Value()), index.Value());
        return Result<void>::Success();
    }

    // 고른 핸들의 키만 delta만큼 옮긴다(UE ShiftCurve(Delta, KeyHandles) — 편집기 선택 이동). 커브
    // TryShiftKeyTimes와 같은 규칙으로 다시 넣고 핸들이 키를 따라간다. 실패: StaleHandle, NotFound,
    // 중복 핸들·비유한 delta·결과 시간 InvalidArgument, 할당 실패 OutOfMemory(커브와 표 그대로).
    Result<void> TryShiftKeyTimes(Curve& curve, Span<const CurveKeyHandle> handles,
                                  float delta) noexcept {
        if (auto checked = Check(curve); !checked) {
            return checked;
        }

        Allocator& allocator = handles_.GetAllocator();
        Array<usize> indices(allocator);
        if (auto reserved = indices.TryReserve(handles.Size()); !reserved) {
            return reserved;
        }

        for (const CurveKeyHandle handle : handles) {
            auto index = Find(handle);
            if (!index) {
                return Result<void>::Failure(index.Error());
            }

            static_cast<void>(indices.TryPushBack(index.Value()));
        }

        // 커브와 같은 규칙으로 새 순서(새 자리마다 옛 index)를 먼저 구한다.
        Array<usize> order(allocator);
        Array<CurveKeyHandle> nextHandles(allocator);
        Array<Key> nextSnapshot(snapshot_.GetAllocator());
        if (auto reserved = order.TryReserve(handles_.Size()); !reserved) {
            return reserved;
        }

        if (auto reserved = nextHandles.TryReserve(handles_.Size()); !reserved) {
            return reserved;
        }

        if (auto reserved = nextSnapshot.TryReserve(snapshot_.Size()); !reserved) {
            return reserved;
        }

        Array<uint8> selected(allocator);
        if (auto sized = selected.TryResize(handles_.Size()); !sized) {
            return sized;
        }

        for (usize index = 0; index < selected.Size(); ++index) {
            selected[index] = 0;
        }

        for (const usize index : indices.View()) {
            selected[index] = 1; // 중복·비유한은 아래 커브 연산이 거부한다(표는 그대로)
        }

        auto shifted = curve.TryShiftKeyTimes(indices.View(), delta);
        if (!shifted) {
            return shifted;
        }

        // 남은 키를 원래 순서로 둔 뒤, 고른 키를 원래 index 오름차순으로 "같은 시간 키 앞"에 넣는다
        // (curves_detail::ShiftKeyTimes와 같은 규칙).
        for (usize index = 0; index < selected.Size(); ++index) {
            if (selected[index] == 0) {
                static_cast<void>(order.TryPushBack(index));
            }
        }

        for (usize index = 0; index < selected.Size(); ++index) {
            if (selected[index] != 0) {
                const float time = snapshot_[index].time + delta;
                usize at = 0;
                while (at < order.Size() && TimeAfterShift(order[at], selected, delta) < time) {
                    ++at;
                }

                static_cast<void>(order.TryInsert(index, at));
            }
        }

        for (usize position = 0; position < order.Size(); ++position) {
            static_cast<void>(nextHandles.TryPushBack(handles_[order[position]]));
            static_cast<void>(nextSnapshot.TryPushBack(Move(snapshot_[order[position]])));
            nextSnapshot[position].time = curve.Keys()[position].time;
        }

        handles_ = Move(nextHandles);
        snapshot_ = Move(nextSnapshot);
        return Result<void>::Success();
    }

    // 모든 키 시간을 delta만큼 옮긴다(UE FIndexedCurve::ShiftCurve(DeltaTime) — IndexedCurve.cpp
    // 89–117, 핸들 유지). 키 순서가 그대로라 모든 핸들이 같은 키를 가리킨다(UE는 키마다
    // SetKeyTime이라 같은 시간이 된 키의 순서가 바뀔 수 있다 — 설계 15절 29). 할당하지 않는다.
    // 실패: StaleHandle, 커브 TryShiftTime의 오류(비유한 delta·결과 시간 InvalidArgument) —
    // 실패하면 커브와 표가 그대로다.
    Result<void> TryShiftTime(Curve& curve, float delta) noexcept {
        if (auto checked = Check(curve); !checked) {
            return checked;
        }

        if (auto shifted = curve.TryShiftTime(delta); !shifted) {
            return shifted;
        }

        SyncTimes(curve);
        return Result<void>::Success();
    }

    // origin을 중심으로 키 시간을 factor배 한다(UE ScaleCurve(ScaleOrigin, ScaleFactor) —
    // IndexedCurve.cpp 119–146). 핸들은
    // TryShiftTime과 같이 그대로다. 실패: StaleHandle, 커브 TryScaleTime의 오류(0 이하·비유한
    // factor, 비유한 origin·결과 시간 InvalidArgument) — 실패하면 커브와 표가 그대로다.
    Result<void> TryScaleTime(Curve& curve, float origin, float factor) noexcept {
        if (auto checked = Check(curve); !checked) {
            return checked;
        }

        if (auto scaled = curve.TryScaleTime(origin, factor); !scaled) {
            return scaled;
        }

        SyncTimes(curve);
        return Result<void>::Success();
    }

    // 커브의 중복 키 제거를 하고 남은 키의 핸들을 그대로 둔다(UE RemoveRedundantKeys의 핸들 유지 —
    // RichCurve.cpp 984–1100, IntegralCurve.cpp 228–257, StringCurve.cpp 209–238). arguments는 커브
    // TryRemoveRedundantKeys의 인자다(TangentCurve는 허용오차·표본 속도, 범위판은 first·last도,
    // PlainCurve는 허용오차, 계단 커브는 없음). 커브가 남긴 키의 원래 index 목록(순서를 보존하는
    // 부분열)으로 핸들·사본을 앞으로 당긴다. 지운 키의 핸들은 NotFound가 된다. 실패: StaleHandle,
    // 커브의 오류(허용오차 InvalidArgument, 표본 상한 CapacityExceeded, 할당 실패 OutOfMemory) —
    // 실패하면 커브와 표가 그대로다.
    template <class... Arguments>
    Result<void> TryRemoveRedundantKeys(Curve& curve, Arguments&&... arguments) noexcept {
        if (auto checked = Check(curve); !checked) {
            return checked;
        }

        Array<usize> kept(handles_.GetAllocator());
        if (auto removed = curve.TryRemoveRedundantKeys(kept, Forward<Arguments>(arguments)...);
            !removed) {
            return removed;
        }

        for (usize position = 0; position < kept.Size(); ++position) {
            const usize from = kept[position]; // 오름차순이라 from ≥ position
            if (from != position) {
                handles_[position] = handles_[from];
                snapshot_[position] = Move(snapshot_[from]);
            }
        }

        if (kept.Size() < handles_.Size()) {
            const usize dropped = handles_.Size() - kept.Size();
            static_cast<void>(
                handles_.TryRemoveAt(kept.Size(), dropped)); // 줄이기라 실패하지 않는다
            static_cast<void>(snapshot_.TryRemoveAt(kept.Size(), dropped));
        }

        return Result<void>::Success();
    }

    // TryEditKeys가 편집 함수에 넘기는 키별 칸 편집자다(TangentCurve 표 전용). 핸들로 키를 골라
    // 보간·접선 방식·가중 방식·접선·가중치만 바꾼다(커브의 같은 이름 함수 — 접선 재계산 없음). 키
    // 시간·값·수·순서를 바꾸는 길은 없다(그런 편집은 표의 다른 함수로 한다). 각 함수는 할당하지
    // 않고, 핸들이 이 표에 없으면 NotFound다(그 키는 그대로). 복사할 수 없고 TryEditKeys 안에서만
    // 쓴다.
    class KeyFieldEditor {
    public:
        KeyFieldEditor(const KeyFieldEditor&) = delete;
        KeyFieldEditor& operator=(const KeyFieldEditor&) = delete;

        // 지금 키들이다(읽기 전용 — 앞선 편집이 바로 보인다).
        Span<const Key> Keys() const noexcept {
            return curve_.Keys();
        }

        // 핸들 키의 지금 index다. 실패: NotFound.
        Result<usize> TryFindIndex(CurveKeyHandle handle) const noexcept {
            return table_.Find(handle);
        }

        Result<void> TrySetInterp(CurveKeyHandle handle, CurveInterp interp) noexcept {
            auto index = table_.Find(handle);
            return index ? curve_.TrySetKeyInterp(index.Value(), interp)
                         : Result<void>::Failure(index.Error());
        }

        Result<void> TrySetTangentMode(CurveKeyHandle handle, CurveTangentMode mode) noexcept {
            auto index = table_.Find(handle);
            return index ? curve_.TrySetKeyTangentMode(index.Value(), mode)
                         : Result<void>::Failure(index.Error());
        }

        Result<void> TrySetTangentWeight(CurveKeyHandle handle,
                                         CurveTangentWeight weight) noexcept {
            auto index = table_.Find(handle);
            return index ? curve_.TrySetKeyTangentWeight(index.Value(), weight)
                         : Result<void>::Failure(index.Error());
        }

        Result<void> TrySetTangents(CurveKeyHandle handle, float arriveTangent,
                                    float leaveTangent) noexcept {
            auto index = table_.Find(handle);
            return index ? curve_.TrySetKeyTangents(index.Value(), arriveTangent, leaveTangent)
                         : Result<void>::Failure(index.Error());
        }

        Result<void> TrySetTangentWeights(CurveKeyHandle handle, float arriveWeight,
                                          float leaveWeight) noexcept {
            auto index = table_.Find(handle);
            return index ? curve_.TrySetKeyTangentWeights(index.Value(), arriveWeight, leaveWeight)
                         : Result<void>::Failure(index.Error());
        }

    private:
        friend class CurveKeyHandleTable;

        KeyFieldEditor(const CurveKeyHandleTable& table, Curve& curve) noexcept
            : table_(table), curve_(curve) {
        }

        const CurveKeyHandleTable& table_;
        Curve& curve_;
    };

    // 키 시간·값·수·순서를 바꾸지 않는 키별 칸 편집을 표를 거쳐 한다(TangentCurve 전용):
    // edit(KeyFieldEditor&)를 부른 뒤 사본을 다시 맞추고 edit의 결과(Result<void>)를 돌려준다.
    // - edit가 실패를 돌려주면 그 전에 편집자로 바꾼 칸은 남는다(표는 그 상태에 맞춰 다시 맞추므로
    //   핸들은 계속 쓸 수 있다 — 실패하면 그대로인 다른 함수와 다른 점이다).
    // - 안전장치: edit가 편집자 밖에서 커브를 바꿔 키 수·시간·값이 사본과 비트까지 다르면(같은 시간
    //   키의 순서 바뀜 포함) 표를 무효로 하고 StaleHandle이다.
    // 사용 예:
    //   auto edited = table.TryEditKeys(curve, [&](auto& keys) noexcept {
    //       return keys.TrySetInterp(handle, CurveInterp::Cubic);
    //   });
    template <class Edit>
        requires IsSameV<Curve, TangentCurve>
    Result<void> TryEditKeys(Curve& curve, Edit&& edit) noexcept {
        if (auto checked = Check(curve); !checked) {
            return checked;
        }

        KeyFieldEditor editor(*this, curve);
        Result<void> edited = edit(editor);
        const Span<const Key> keys = curve.Keys();
        bool same = keys.Size() == snapshot_.Size();
        for (usize index = 0; same && index < keys.Size(); ++index) {
            same = curves_detail::SameFloatBits(keys[index].time, snapshot_[index].time) &&
                   curves_detail::SameFloatBits(keys[index].value, snapshot_[index].value);
        }

        if (!same) {
            stale_ = true;
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        for (usize index = 0; index < keys.Size(); ++index) {
            snapshot_[index] = keys[index];
        }

        return edited;
    }

    // Auto 키의 접선을 다시 계산한다(TangentCurve::AutoSetTangents — 키 시간·값·순서가 그대로라
    // 핸들도 그대로다). 할당하지 않는다. 실패: StaleHandle.
    Result<void> TryAutoSetTangents(Curve& curve, float tension = 0.0f) noexcept
        requires IsSameV<Curve, TangentCurve>
    {
        if (auto checked = Check(curve); !checked) {
            return checked;
        }

        curve.AutoSetTangents(tension);
        const Span<const Key> keys = curve.Keys();
        for (usize index = 0; index < keys.Size(); ++index) {
            snapshot_[index] = keys[index];
        }

        return Result<void>::Success();
    }

private:
    friend struct curves_detail::CurveKeyHandleTableTestAccess;

    // 커브가 사본과 같은지 본다. 다르면 표를 무효로 표시한다.
    Result<void> Check(const Curve& curve) noexcept {
        if (stale_) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        const Span<const Key> keys = curve.Keys();
        bool same = keys.Size() == snapshot_.Size();
        for (usize index = 0; same && index < keys.Size(); ++index) {
            same = curves_detail::SameKeyBits(keys[index], snapshot_[index]);
        }

        if (!same) {
            stale_ = true;
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        return Result<void>::Success();
    }

    Result<usize> Find(CurveKeyHandle handle) const noexcept {
        if (handle.IsValid()) {
            for (usize index = 0; index < handles_.Size(); ++index) {
                if (handles_[index] == handle) {
                    return Result<usize>::Success(index);
                }
            }
        }

        return Result<usize>::Failure(ErrorCode::NotFound);
    }

    Result<usize> Locate(const Curve& curve, CurveKeyHandle handle) noexcept {
        if (auto checked = Check(curve); !checked) {
            return Result<usize>::Failure(checked.Error());
        }

        return Find(handle);
    }

    // 새 키·바뀐 키의 사본 자리를 미리 만든다(String 값은 커브를 바꾸기 전에 복제한다 — 실패해도
    // 커브가 그대로다). 핸들 목록·사본의 용량도 하나 더 잡아 둔다.
    template <class Value>
    Result<Key> PrepareInsert(float time, const Value& value) noexcept {
        if (auto reserved = handles_.TryEnsureCapacity(handles_.Size() + 1); !reserved) {
            return Result<Key>::Failure(reserved.Error());
        }

        if (auto reserved = snapshot_.TryEnsureCapacity(snapshot_.Size() + 1); !reserved) {
            return Result<Key>::Failure(reserved.Error());
        }

        if constexpr (IsTriviallyCopyableV<Key>) {
            static_cast<void>(time);
            static_cast<void>(value);
            return Result<Key>::Success(Key{});
        } else {
            Key key(snapshot_.GetAllocator());
            key.time = time;
            if (auto assigned = key.value.TryAssign(StringView(value)); !assigned) {
                return Result<Key>::Failure(assigned.Error());
            }

            return Result<Key>::Success(Move(key));
        }
    }

    // 커브 연산이 바꾼 뒤의 키로 사본 키를 마무리한다(비트 복사 키는 그대로 복사, String 키는
    // 시간만 — 값은 PrepareInsert가 이미 복제했다).
    void Finish(Key& snapshotKey, const Key& curveKey) noexcept {
        if constexpr (IsTriviallyCopyableV<Key>) {
            snapshotKey = curveKey;
        } else {
            snapshotKey.time = curveKey.time;
        }
    }

    void Commit(const Curve& curve, Key&& pending, usize index, CurveKeyHandle handle) noexcept {
        Finish(pending, curve.Keys()[index]);
        static_cast<void>(handles_.TryInsert(handle, index)); // 용량을 잡아 두었다
        static_cast<void>(snapshot_.TryInsert(Move(pending), index));
    }

    void Refresh(const Curve& curve, Key&& pending, usize index) noexcept {
        Finish(pending, curve.Keys()[index]);
        snapshot_[index] = Move(pending);
    }

    // 핸들·사본의 한 자리를 from에서 to로 옮긴다(사이는 한 칸씩 민다).
    void MoveEntry(usize from, usize to) noexcept {
        if (from == to) {
            return;
        }

        const CurveKeyHandle handle = handles_[from];
        Key key = Move(snapshot_[from]);
        if (to < from) {
            for (usize position = from; position > to; --position) {
                handles_[position] = handles_[position - 1];
                snapshot_[position] = Move(snapshot_[position - 1]);
            }
        } else {
            for (usize position = from; position < to; ++position) {
                handles_[position] = handles_[position + 1];
                snapshot_[position] = Move(snapshot_[position + 1]);
            }
        }

        handles_[to] = handle;
        snapshot_[to] = Move(key);
    }

    // 순서를 그대로 두는 시간 연산(Shift·Scale) 뒤 사본 시간을 커브에 맞춘다.
    void SyncTimes(const Curve& curve) noexcept {
        const Span<const Key> keys = curve.Keys();
        for (usize index = 0; index < keys.Size(); ++index) {
            snapshot_[index].time = keys[index].time;
        }
    }

    float TimeAfterShift(usize index, const Array<uint8>& selected, float delta) const noexcept {
        return selected[index] != 0 ? snapshot_[index].time + delta : snapshot_[index].time;
    }

    Array<CurveKeyHandle> handles_;
    Array<Key> snapshot_;
    uint32 lastIssued_ = 0;
    bool stale_ = false;
};

} // namespace rocks
