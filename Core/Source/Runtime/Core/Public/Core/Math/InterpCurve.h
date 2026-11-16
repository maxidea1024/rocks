#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/InterpCurveMath.h>
#include <Core/Math/InterpCurvePoint.h>
#include <Core/Math/Interval.h>
#include <Core/Math/PolynomialRoots.h>

namespace rocks {

// 최근접 탐색 결과다.
//   input            최근접 점의 커브 입력값(키 사이 비율을 입력 단위로 바꾼 값).
//   distanceSquared  찾는 위치와 최근접 점 사이 거리의 제곱.
//   segment          그 점이 속한 구간 번호(구간 i는 키 i에서 다음 키까지, 루프 구간은 마지막 키
//   번호).
template <typename S>
struct CurveNearestPoint {
    S input = static_cast<S>(0);
    S distanceSquared = static_cast<S>(0);
    usize segment = 0;
};

// 키 기반 보간 커브다(UE 5.8 FInterpCurve 대응). 입력값(보통 시간)에 정렬된 키 목록과 키별 보간
// 모드(InterpCurveMode)로 값·1차·2차 미분을 평가하고, 자동 접선·루프·최근접 탐색을 제공한다.
// 의미 결정과 UE 차이의 근거는 Docs/Implementation/InterpCurve.ko.md에 있다.
//
// 소유·수명: Allocator를 빌려 키 배열을 소유하는 이동 전용 값이다. Allocator는 커브보다 오래 살아야
// 한다. 복사는 TryCopyFrom으로 명시한다. Points() view와 GetPoint 참조는 키 추가·이동·삭제·Reset·
// TryCopyFrom·이동 대입에서 무효다.
// 스레드: 내부 동기화가 없다. 변경하지 않는 동안의 const 호출은 여러 스레드에서 함께 해도 된다.
// 불변식: 키 입력값은 유한하고 오름차순이다(같은 값 허용). 입력값은 TryAddPoint·TryMovePoint로만
// 바꿀 수 있다. 루프 offset은 루프가 켜져 있으면 유한한 양수다.
//
// 사용 예:
//   SystemAllocator allocator;
//   InterpCurve<Vec3> path(allocator);
//   (void)path.TryAddPoint(0.0f, Vec3(0, 0, 0));
//   (void)path.TryAddPoint(1.0f, Vec3(1, 2, 0));
//   (void)path.TryAddPoint(3.0f, Vec3(4, 0, 0));
//   for (usize i = 0; i < path.PointCount(); ++i) {
//       path.SetMode(i, InterpCurveMode::CurveAuto);
//   }
//   path.AutoSetTangents();
//   const Vec3 position = path.Eval(2.0f);
//   const Vec3 velocity = path.EvalDerivative(2.0f);
template <typename T>
class InterpCurve {
    static_assert(InterpCurveValue<T>, "InterpCurve supports float, double, Vector2/3/4, "
                                       "Quaternion and LinearColor values");

public:
    using Traits = CurveValueTraits<T>;
    using Scalar = typename Traits::Scalar;
    using Point = InterpCurvePoint<T>;
    using NearestPoint = CurveNearestPoint<Scalar>;

    // FindSegmentIndex가 첫 키 앞·NaN·빈 커브에서 반환한다.
    static constexpr usize NoSegment = IndexNone;

    explicit InterpCurve(Allocator& allocator) noexcept : points_(allocator) {
    }

    InterpCurve(const InterpCurve&) = delete;
    InterpCurve& operator=(const InterpCurve&) = delete;
    InterpCurve(InterpCurve&&) noexcept = default;
    InterpCurve& operator=(InterpCurve&&) noexcept = default;
    ~InterpCurve() noexcept = default;

    // ---- 키 ----

    // Linear 모드·기본 접선(0, quaternion은 키 자신)의 키를 넣고 그 위치를 반환한다. 같은 입력값의
    // 기존 키들 앞에 들어간다(UE AddPoint와 같다). 실패: 비유한 inValue는 InvalidArgument, 할당
    // 실패는 그 오류다. 실패하면 커브는 바뀌지 않는다. 성공하면 기존 view·참조가 무효다.
    Result<usize> TryAddPoint(Scalar inValue, const T& outValue) noexcept {
        return TryAddPoint(Point(inValue, outValue));
    }

    // 모든 필드를 지정한 키를 넣는다. 위치·실패 규칙은 위와 같다.
    Result<usize> TryAddPoint(const Point& point) noexcept {
        if (!math::IsFinite(point.inValue)) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        const usize index = LowerBound(point.inValue, NoSegment);
        auto inserted = points_.TryInsert(point, index);
        if (!inserted) {
            return Result<usize>::Failure(inserted.Error());
        }

        return Result<usize>::Success(index);
    }

    // index 키의 입력값을 newInValue로 바꾸고 새 위치를 반환한다. 값·접선·모드는 유지한다. 새
    // 위치는 TryAddPoint와 같은 규칙(같은 입력값의 다른 키들 앞)이다. 할당하지 않는다.
    // 실패(InvalidArgument): index 범위 밖, 비유한 newInValue. 실패하면 바뀌지 않는다. UE
    // MovePoint는 잘못된 index를 그대로 반환한다.
    Result<usize> TryMovePoint(usize index, Scalar newInValue) noexcept {
        if (index >= points_.Size() || !math::IsFinite(newInValue)) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        Point moved = points_[index];
        moved.inValue = newInValue;
        const usize target = LowerBound(newInValue, index);
        if (target <= index) {
            for (usize position = index; position > target; --position) {
                points_[position] = points_[position - 1];
            }
        } else {
            for (usize position = index; position < target; ++position) {
                points_[position] = points_[position + 1];
            }
        }

        points_[target] = moved;
        return Result<usize>::Success(target);
    }

    // index 키를 지운다. 범위 밖은 InvalidArgument이며 바뀌지 않는다. 루프 상태는 유지한다.
    Result<void> TryRemovePoint(usize index) noexcept {
        return points_.TryRemoveAt(index);
    }

    // count개 키를 담을 공간을 미리 확보한다. 실패하면 바뀌지 않는다.
    Result<void> TryReserve(usize count) noexcept {
        return points_.TryReserve(count);
    }

    // 모든 키를 지운다(UE Reset). 루프 설정은 유지한다(UE와 같다).
    void Reset() noexcept {
        points_.Clear();
    }

    // other의 키와 루프 설정을 복사한다. allocator는 이 커브의 것을 계속 쓴다. 할당이 실패하면
    // 오류를 반환하고 바뀌지 않는다. 자기 자신이면 아무것도 하지 않는다.
    Result<void> TryCopyFrom(const InterpCurve& other) noexcept {
        if (this == &other) {
            return Result<void>::Success();
        }

        Array<Point> copied(points_.GetAllocator());
        auto reserved = copied.TryReserve(other.points_.Size());
        if (!reserved) {
            return reserved;
        }

        for (const Point& point : other.points_) {
            auto pushed = copied.TryPushBack(point);
            if (!pushed) {
                return pushed;
            }
        }

        points_ = Move(copied);
        looped_ = other.looped_;
        loopKeyOffset_ = other.loopKeyOffset_;
        return Result<void>::Success();
    }

    usize PointCount() const noexcept {
        return points_.Size();
    }

    bool IsEmpty() const noexcept {
        return points_.IsEmpty();
    }

    // 키 목록(입력값 오름차순)이다. 무효화 조건은 클래스 주석을 따른다.
    Span<const Point> Points() const noexcept {
        return points_.View();
    }

    // index 키다. 범위 밖은 Fatal이다.
    const Point& GetPoint(usize index) const noexcept {
        return points_[index];
    }

    // 키 목록(순서 포함, 필드별 정확 비교)과 루프 여부가 같고, 루프면 offset도 같으면 참이다(UE
    // FInterpCurve::operator==와 같다 — 루프가 꺼져 있으면 offset은 보지 않는다). allocator는 보지
    // 않는다. NaN 필드가 있는 키는 자기 자신과도 같지 않다.
    friend bool operator==(const InterpCurve& left, const InterpCurve& right) noexcept {
        if (left.points_.Size() != right.points_.Size() || left.looped_ != right.looped_) {
            return false;
        }

        if (left.looped_ && left.loopKeyOffset_ != right.loopKeyOffset_) {
            return false;
        }

        for (usize index = 0; index < left.points_.Size(); ++index) {
            if (!(left.points_[index] == right.points_[index])) {
                return false;
            }
        }

        return true;
    }

    // index 키의 출력값·접선·모드를 바꾼다. 범위 밖은 Fatal이다. 자동 모드 키의 접선은 다음
    // AutoSetTangents가 다시 계산한다.
    void SetOutValue(usize index, const T& value) noexcept {
        points_[index].outValue = value;
    }

    void SetTangents(usize index, const T& arrive, const T& leave) noexcept {
        points_[index].arriveTangent = arrive;
        points_[index].leaveTangent = leave;
    }

    void SetMode(usize index, InterpCurveMode mode) noexcept {
        points_[index].mode = mode;
    }

    // ---- 루프 ----

    // 마지막 키 → 첫 키 값의 닫는 구간을 loopKey까지 둔다(UE SetLoopKey). offset = loopKey − 마지막
    // 키로 저장하므로 키를 더하면 루프 끝도 따라 움직인다. 키가 없거나 loopKey가 마지막 키 이하이면
    // 루프를 끄고 성공한다(UE와 같다). 실패(InvalidArgument): 비유한 loopKey 또는 offset이 Scalar로
    // 유한하지 않음. 실패하면 바뀌지 않는다. 루프는 입력을 감지 않는다(Eval 주석).
    Result<void> TrySetLoopKey(Scalar loopKey) noexcept {
        if (!math::IsFinite(loopKey)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (points_.IsEmpty()) {
            looped_ = false;
            return Result<void>::Success();
        }

        const Scalar lastKey = points_[points_.Size() - 1].inValue;
        if (!(loopKey > lastKey)) {
            looped_ = false;
            return Result<void>::Success();
        }

        const Scalar offset = loopKey - lastKey;
        if (!math::IsFinite(offset)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        looped_ = true;
        loopKeyOffset_ = offset;
        return Result<void>::Success();
    }

    void ClearLoopKey() noexcept {
        looped_ = false;
    }

    bool IsLooped() const noexcept {
        return looped_;
    }

    // 마지막 키에서 루프 끝까지의 입력 길이다. 루프가 꺼져 있으면 의미가 없다.
    Scalar LoopKeyOffset() const noexcept {
        return loopKeyOffset_;
    }

    // 평가 구간 수다. 키가 없으면 0, 루프면 키 수, 아니면 키 수 − 1이다.
    usize SegmentCount() const noexcept {
        const usize count = points_.Size();
        if (count == 0) {
            return 0;
        }

        return looped_ ? count : count - 1;
    }

    // ---- 평가 ----

    // input을 포함하는 구간의 시작 키 번호다: 입력값이 input 이하인 마지막 키(UE
    // GetPointIndexForInputValue와 같은 이분 탐색). 같은 입력값 키가 여럿이면 그중 마지막이다.
    // 첫 키 앞·NaN·빈 커브는 NoSegment, 마지막 키 이상은 마지막 키 번호다.
    usize FindSegmentIndex(Scalar input) const noexcept {
        const usize count = points_.Size();
        if (count == 0 || math::IsNaN(input) || input < points_[0].inValue) {
            return NoSegment;
        }

        const usize last = count - 1;
        if (input >= points_[last].inValue) {
            return last;
        }

        usize minimum = 0;
        usize maximum = count;
        while (maximum - minimum > 1) {
            const usize middle = minimum + (maximum - minimum) / 2;
            if (points_[middle].inValue <= input) {
                minimum = middle;
            } else {
                maximum = middle;
            }
        }

        return minimum;
    }

    // input에서의 값이다(UE Eval).
    // - 빈 커브: emptyValue(기본 0, quaternion은 항등).
    // - NaN input: 모든 성분 NaN(UE는 Default를 반환한다).
    // - 첫 키 앞: 첫 키 값. 마지막 키 이상(루프 아님): 마지막 키 값. 외삽하지 않는다.
    // - 루프: [마지막 키, 마지막 키 + offset)은 마지막 키 → 첫 키 값의 구간, 그 이상은 첫 키 값.
    //   입력을 감지 않으므로 주기 평가는 호출자가 math::Wrap(input, 첫 키, 마지막 키 + offset)으로
    //   감는다.
    // - 구간 안: 시작 키 모드가 Constant면 시작 키 값, Linear면 성분별 math::Lerp(quaternion은
    // Slerp),
    //   곡선이면 Hermite(quaternion은 SQUAD).
    T Eval(Scalar input, const T& emptyValue = Traits::DefaultValue()) const noexcept {
        const usize count = points_.Size();
        if (count == 0) {
            return emptyValue;
        }

        if (math::IsNaN(input)) {
            return Traits::NaNValue();
        }

        const usize index = FindSegmentIndex(input);
        if (index == NoSegment) {
            return points_[0].outValue;
        }

        const usize last = count - 1;
        if (index == last && (!looped_ || input >= points_[last].inValue + loopKeyOffset_)) {
            return looped_ ? points_[0].outValue : points_[last].outValue;
        }

        const Segment segment = MakeSegment(index);
        if (!(segment.difference > static_cast<Scalar>(0)) ||
            segment.start->mode == InterpCurveMode::Constant) {
            return segment.start->outValue;
        }

        const Scalar alpha = SegmentAlpha(input, segment);
        if constexpr (Traits::IsRotation) {
            if (segment.start->mode == InterpCurveMode::Linear) {
                return Slerp(segment.start->outValue, segment.end->outValue, alpha);
            }

            return Squad(segment.start->outValue, segment.start->leaveTangent,
                         segment.end->outValue, segment.end->arriveTangent, alpha);
        } else {
            if (segment.start->mode == InterpCurveMode::Linear) {
                return LerpComponents(segment.start->outValue, segment.end->outValue, alpha);
            }

            return HermiteComponents(segment, alpha, HermiteOrder::Value);
        }
    }

    // input에서의 입력 1단위당 1차 미분이다(UE EvalDerivative). quaternion 커브에는 없다.
    // - 빈 커브: emptyValue(기본 0). NaN input: 모든 성분 NaN.
    // - 첫 키 앞: 첫 키의 leaveTangent. 마지막 키 이상(루프 아님): 마지막 키의 arriveTangent.
    //   루프 끝 이상: 첫 키의 arriveTangent. 상수 외삽의 미분 0이 아니라 끝 방향이다(UE 규약).
    // - 구간 안: Constant는 0, Linear는 (끝 값 − 시작 값) / 구간 길이, 곡선은 Hermite 미분 / 길이.
    T EvalDerivative(Scalar input, const T& emptyValue = Traits::DefaultValue()) const noexcept
        requires(!Traits::IsRotation)
    {
        const usize count = points_.Size();
        if (count == 0) {
            return emptyValue;
        }

        if (math::IsNaN(input)) {
            return Traits::NaNValue();
        }

        const usize index = FindSegmentIndex(input);
        if (index == NoSegment) {
            return points_[0].leaveTangent;
        }

        const usize last = count - 1;
        if (index == last && (!looped_ || input >= points_[last].inValue + loopKeyOffset_)) {
            return looped_ ? points_[0].arriveTangent : points_[last].arriveTangent;
        }

        const Segment segment = MakeSegment(index);
        if (!(segment.difference > static_cast<Scalar>(0)) ||
            segment.start->mode == InterpCurveMode::Constant) {
            return Traits::DefaultValue();
        }

        if (segment.start->mode == InterpCurveMode::Linear) {
            return LinearSlope(segment);
        }

        return HermiteComponents(segment, SegmentAlpha(input, segment), HermiteOrder::First);
    }

    // input에서의 입력 1단위당 2차 미분이다(UE EvalSecondDerivative). quaternion 커브에는 없다.
    // 빈 커브는 emptyValue, NaN input은 NaN, 키 범위 밖·Constant·Linear 구간은 0, 곡선은
    // Hermite 2차 미분 / 구간 길이².
    T EvalSecondDerivative(Scalar input,
                           const T& emptyValue = Traits::DefaultValue()) const noexcept
        requires(!Traits::IsRotation)
    {
        const usize count = points_.Size();
        if (count == 0) {
            return emptyValue;
        }

        if (math::IsNaN(input)) {
            return Traits::NaNValue();
        }

        const usize index = FindSegmentIndex(input);
        const usize last = count - 1;
        if (index == NoSegment ||
            (index == last && (!looped_ || input >= points_[last].inValue + loopKeyOffset_))) {
            return Traits::DefaultValue();
        }

        const Segment segment = MakeSegment(index);
        if (!(segment.difference > static_cast<Scalar>(0)) || !segment.start->IsCurveKey()) {
            return Traits::DefaultValue();
        }

        return HermiteComponents(segment, SegmentAlpha(input, segment), HermiteOrder::Second);
    }

    // 키 입력값의 범위 [첫 키, 마지막 키]다(UE GetKeyInterval). 루프 구간은 포함하지 않는다.
    // 빈 커브는 NotFound다.
    Result<Interval<Scalar>> TryGetKeyInterval() const noexcept {
        if (points_.IsEmpty()) {
            return Result<Interval<Scalar>>::Failure(ErrorCode::NotFound);
        }

        return Result<Interval<Scalar>>::Success(
            Interval<Scalar>(points_[0].inValue, points_[points_.Size() - 1].inValue));
    }

    // ---- 자동 접선 ----

    // CurveAuto·CurveAutoClamped·Linear·Constant 키의 접선을 이웃 키로 다시 계산한다(UE
    // AutoSetTangents와 같은 순회 — 키 순서대로 제자리에서 바꾸므로 앞 키의 새 접선을 읽는다).
    // CurveUser·CurveBreak 키는 바꾸지 않는다.
    // - 이웃: 첫 키의 이전 키와 마지막 키의 다음 키는 루프면 반대쪽 끝 키(시간은 ∓ offset), 아니면
    //   자기 자신이다.
    // - stationaryEndpoints: 참이면 첫 키와 (루프가 아닐 때) 마지막 키의 자동 접선을 0(quaternion은
    //   키 자신)으로 둔다. 루프에서도 첫 키는 0이 된다(UE) — 이음매가 매끄러우려면 false를 준다.
    // - 자동 곡선 키: 이전 키가 곡선 키면 (1 − tension)·(다음 − 이전) / (다음 시간 − 이전 시간)이고
    //   CurveAutoClamped는 성분별 math::ClampCurveTangent에 (1 − tension)을 곱한다. 이전 키가
    //   Linear· Constant면 그 키의 접선을 복사해 이음매의 1차 미분을 잇는다.
    // - Linear 키: 출발 접선은 다음 구간 기울기, 도착 접선은 이전 키가 곡선 키면 출발 접선, 아니면
    //   이전 구간 기울기다. UE는 기울기 대신 값 차를 저장한다(Docs/Implementation/InterpCurve.ko.md
    //   4절).
    // - Constant 키: 접선 0.
    // - quaternion: 자동 곡선 키는 SquadControlPoint(이전, 현재, 다음, tension), Linear·Constant
    // 키는
    //   키 자신이다.
    // 시간 차는 math::CurveTangentTimeDifference로 1e-4 아래를 막는다. tension은 검사하지 않는다.
    void AutoSetTangents(Scalar tension = static_cast<Scalar>(0),
                         bool stationaryEndpoints = true) noexcept {
        const usize count = points_.Size();
        if (count == 0) {
            return;
        }

        const usize last = count - 1;
        for (usize index = 0; index < count; ++index) {
            const usize previousIndex = index == 0 ? (looped_ ? last : 0) : index - 1;
            const usize nextIndex = index == last ? (looped_ ? 0 : last) : index + 1;
            Point& current = points_[index];
            const Point& previous = points_[previousIndex];
            const Point& next = points_[nextIndex];
            const Scalar previousTime =
                (looped_ && index == 0) ? current.inValue - loopKeyOffset_ : previous.inValue;
            const Scalar nextTime =
                (looped_ && index == last) ? current.inValue + loopKeyOffset_ : next.inValue;
            const bool isEndpoint = index == 0 || (index == last && !looped_);

            if (current.mode == InterpCurveMode::CurveAuto ||
                current.mode == InterpCurveMode::CurveAutoClamped) {
                if (stationaryEndpoints && isEndpoint) {
                    current.arriveTangent = Traits::DefaultTangent(current.outValue);
                    current.leaveTangent = current.arriveTangent;
                } else if constexpr (Traits::IsRotation) {
                    current.arriveTangent = SquadControlPoint(previous.outValue, current.outValue,
                                                              next.outValue, tension);
                    current.leaveTangent = current.arriveTangent;
                } else if (previous.IsCurveKey()) {
                    current.arriveTangent = AutoTangentComponents(
                        previous.outValue, previousTime, current, next.outValue, nextTime, tension,
                        current.mode == InterpCurveMode::CurveAutoClamped);
                    current.leaveTangent = current.arriveTangent;
                } else {
                    // Linear·Constant 뒤: 그 키의 접선과 같게 해 이음매에서 끊기지 않게 한다.
                    current.arriveTangent = previous.arriveTangent;
                    current.leaveTangent = previous.leaveTangent;
                }
            } else if (current.mode == InterpCurveMode::Linear) {
                if constexpr (Traits::IsRotation) {
                    current.arriveTangent = current.outValue;
                    current.leaveTangent = current.outValue;
                } else {
                    current.leaveTangent =
                        SlopeComponents(current.outValue, current.inValue, next.outValue, nextTime);
                    current.arriveTangent =
                        previous.IsCurveKey() ? current.leaveTangent
                                              : SlopeComponents(previous.outValue, previousTime,
                                                                current.outValue, current.inValue);
                }
            } else if (current.mode == InterpCurveMode::Constant) {
                current.arriveTangent = Traits::DefaultTangent(current.outValue);
                current.leaveTangent = current.arriveTangent;
            }
        }
    }

    // ---- 최근접 탐색 ----

    // position에 가장 가까운 커브 위 점이다(UE FindNearest). 거리 있는
    // 형식(float·double·Vector2/3/4)만 제공한다. 구간을 0번부터 차례로 보고 거리가 더 작을
    // 때만(엄격한 <) 바꾸므로 동률은 앞 구간이다. 키가 하나면 그 키(구간 0), 빈 커브는 NotFound다.
    // position에 NaN이 있으면 거리가 NaN인 구간 0의 결과다. 루프 구간도 포함한다. 비용은 구간 수에
    // 비례한다.
    Result<NearestPoint> TryFindNearest(const T& position) const noexcept
        requires(Traits::HasDistance)
    {
        const usize count = points_.Size();
        if (count == 0) {
            return Result<NearestPoint>::Failure(ErrorCode::NotFound);
        }

        if (count == 1) {
            NearestPoint single;
            single.input = points_[0].inValue;
            single.distanceSquared =
                static_cast<Scalar>(ComponentDistanceSquared(points_[0].outValue, position));
            single.segment = 0;
            return Result<NearestPoint>::Success(single);
        }

        NearestPoint best = FindNearestOnSegment(position, 0);
        const usize segments = SegmentCount();
        for (usize segment = 1; segment < segments; ++segment) {
            const NearestPoint local = FindNearestOnSegment(position, segment);
            if (local.distanceSquared < best.distanceSquared) {
                best = local;
            }
        }

        return Result<NearestPoint>::Success(best);
    }

    // segment 구간(키 segment에서 다음 키까지, 루프면 마지막 구간은 첫 키 값까지) 위의 최근접
    // 점이다 (UE FindNearestOnSegment). segment >= SegmentCount()는 Fatal이다.
    // - Constant: 두 끝 키 값 중 가까운 쪽(같으면 다음 키)의 입력값.
    // - Linear: 선분 사영을 [0, 1]로 자른 점. 길이 0 선분은 끝 키다(UE와 같다).
    // - 곡선: 끝점 두 개, (C(t) − p)·C'(t) = 0의 근, 그 식의 임계점(접하는 근 후보)을 차례로
    // 비교한다. 계수·근·거리는 double로 계산한 뒤 Scalar로 반올림한다. 같은 입력값 키 사이(길이 0)
    // 구간은 입력값이 그 키다.
    NearestPoint FindNearestOnSegment(const T& position, usize segment) const noexcept
        requires(Traits::HasDistance)
    {
        if (segment >= SegmentCount()) {
            platform::Fatal("InterpCurve::FindNearestOnSegment segment out of range");
        }

        const Segment range = MakeSegment(segment);
        const Point& start = *range.start;
        const Point& end = *range.end;
        const Scalar endInput = start.inValue + range.difference;
        NearestPoint result;
        result.segment = segment;

        if (start.mode == InterpCurveMode::Constant) {
            const double startDistance = ComponentDistanceSquared(start.outValue, position);
            const double endDistance = ComponentDistanceSquared(end.outValue, position);
            if (startDistance < endDistance) {
                result.input = start.inValue;
                result.distanceSquared = static_cast<Scalar>(startDistance);
            } else {
                result.input = endInput;
                result.distanceSquared = static_cast<Scalar>(endDistance);
            }

            return result;
        }

        double startComponents[Traits::ComponentCount] = {};
        double endComponents[Traits::ComponentCount] = {};
        double positionComponents[Traits::ComponentCount] = {};
        ToDouble(start.outValue, startComponents);
        ToDouble(end.outValue, endComponents);
        ToDouble(position, positionComponents);

        if (start.mode == InterpCurveMode::Linear) {
            double projection = 0.0;
            double lengthSquared = 0.0;
            for (usize component = 0; component < Traits::ComponentCount; ++component) {
                const double toEnd = endComponents[component] - startComponents[component];
                projection += (startComponents[component] - positionComponents[component]) * toEnd;
                lengthSquared += toEnd * toEnd;
            }

            // UE는 길이 0에서 Clamp(NaN, 0, 1) = 1이 된다. 같은 결과(끝 키)를 명시적으로 고른다.
            const double ratio =
                lengthSquared > 0.0 ? math::Clamp(-projection / lengthSquared, 0.0, 1.0) : 1.0;
            double distance = 0.0;
            for (usize component = 0; component < Traits::ComponentCount; ++component) {
                const double point =
                    math::Lerp(startComponents[component], endComponents[component], ratio);
                distance += math::Square(point - positionComponents[component]);
            }

            result.input = static_cast<Scalar>(ratio) * range.difference + start.inValue;
            result.distanceSquared = static_cast<Scalar>(distance);
            return result;
        }

        return NearestOnCubic(range, startComponents, endComponents, positionComponents, segment);
    }

private:
    enum class HermiteOrder : uint8 {
        Value,
        First,
        Second,
    };

    // 평가 구간이다. difference는 입력 길이(루프 구간은 offset)다.
    struct Segment {
        const Point* start;
        const Point* end;
        Scalar difference;
    };

    // 키 배열에서 key 입력값보다 작은 키의 수(= 새 키의 위치)다. skip은 셈에서 뺄 키 번호다
    // (TryMovePoint가 자기 자신을 뺀 배열 기준 위치를 구할 때 쓴다).
    usize LowerBound(Scalar key, usize skip) const noexcept {
        usize position = 0;
        for (usize index = 0; index < points_.Size(); ++index) {
            if (index != skip && points_[index].inValue < key) {
                ++position;
            }
        }

        return position;
    }

    Segment MakeSegment(usize index) const noexcept {
        const usize last = points_.Size() - 1;
        const bool loopSegment = looped_ && index == last;
        const Point& start = points_[index];
        const Point& end = points_[loopSegment ? 0 : index + 1];
        const Scalar difference = loopSegment ? loopKeyOffset_ : end.inValue - start.inValue;
        return Segment{&start, &end, difference};
    }

    // 구간 안 비율이다. 루프 구간 끝(마지막 키 + offset)의 반올림으로 1을 조금 넘을 수 있어 1로
    // 자른다(UE는 checkf로 막는다). 비율은 0 이상이다(input >= 시작 키).
    static Scalar SegmentAlpha(Scalar input, const Segment& segment) noexcept {
        const Scalar alpha = (input - segment.start->inValue) / segment.difference;
        return math::Min(alpha, static_cast<Scalar>(1));
    }

    static T LerpComponents(const T& from, const T& to, Scalar alpha) noexcept {
        Scalar a[Traits::ComponentCount];
        Scalar b[Traits::ComponentCount];
        Traits::Store(from, a);
        Traits::Store(to, b);
        for (usize component = 0; component < Traits::ComponentCount; ++component) {
            a[component] = math::Lerp(a[component], b[component], alpha);
        }

        return Traits::Load(a);
    }

    static T LinearSlope(const Segment& segment) noexcept {
        Scalar a[Traits::ComponentCount];
        Scalar b[Traits::ComponentCount];
        Traits::Store(segment.start->outValue, a);
        Traits::Store(segment.end->outValue, b);
        for (usize component = 0; component < Traits::ComponentCount; ++component) {
            a[component] = (b[component] - a[component]) / segment.difference;
        }

        return Traits::Load(a);
    }

    static T HermiteComponents(const Segment& segment, Scalar alpha, HermiteOrder order) noexcept {
        Scalar p0[Traits::ComponentCount];
        Scalar t0[Traits::ComponentCount];
        Scalar p1[Traits::ComponentCount];
        Scalar t1[Traits::ComponentCount];
        Traits::Store(segment.start->outValue, p0);
        Traits::Store(segment.start->leaveTangent, t0);
        Traits::Store(segment.end->outValue, p1);
        Traits::Store(segment.end->arriveTangent, t1);
        const Scalar difference = segment.difference;
        for (usize component = 0; component < Traits::ComponentCount; ++component) {
            const Scalar leave = t0[component] * difference;
            const Scalar arrive = t1[component] * difference;
            if (order == HermiteOrder::Value) {
                p0[component] =
                    math::CurveHermite(p0[component], leave, p1[component], arrive, alpha);
            } else if (order == HermiteOrder::First) {
                p0[component] = math::CurveHermiteDerivative(p0[component], leave, p1[component],
                                                             arrive, alpha) /
                                difference;
            } else {
                p0[component] = math::CurveHermiteSecondDerivative(p0[component], leave,
                                                                   p1[component], arrive, alpha) /
                                (difference * difference);
            }
        }

        return Traits::Load(p0);
    }

    // (to − from) / max(1e-4, toTime − fromTime). Linear 키 자동 접선(기울기)이다.
    static T SlopeComponents(const T& from, Scalar fromTime, const T& to, Scalar toTime) noexcept {
        Scalar a[Traits::ComponentCount];
        Scalar b[Traits::ComponentCount];
        Traits::Store(from, a);
        Traits::Store(to, b);
        const Scalar time = math::CurveTangentTimeDifference(fromTime, toTime);
        for (usize component = 0; component < Traits::ComponentCount; ++component) {
            a[component] = (b[component] - a[component]) / time;
        }

        return Traits::Load(a);
    }

    static T AutoTangentComponents(const T& previous, Scalar previousTime, const Point& current,
                                   const T& next, Scalar nextTime, Scalar tension,
                                   bool clamp) noexcept {
        Scalar p[Traits::ComponentCount];
        Scalar c[Traits::ComponentCount];
        Scalar n[Traits::ComponentCount];
        Traits::Store(previous, p);
        Traits::Store(current.outValue, c);
        Traits::Store(next, n);
        for (usize component = 0; component < Traits::ComponentCount; ++component) {
            if (clamp) {
                const Scalar clamped =
                    math::ClampCurveTangent(p[component], previousTime, c[component],
                                            current.inValue, n[component], nextTime);
                p[component] = (static_cast<Scalar>(1) - tension) * clamped;
            } else {
                p[component] = math::AutoCurveTangent(p[component], previousTime, c[component],
                                                      n[component], nextTime, tension);
            }
        }

        return Traits::Load(p);
    }

    static void ToDouble(const T& value, double (&output)[Traits::ComponentCount]) noexcept {
        Scalar components[Traits::ComponentCount];
        Traits::Store(value, components);
        for (usize component = 0; component < Traits::ComponentCount; ++component) {
            output[component] = static_cast<double>(components[component]);
        }
    }

    static double ComponentDistanceSquared(const T& a, const T& b) noexcept {
        double first[Traits::ComponentCount];
        double second[Traits::ComponentCount];
        ToDouble(a, first);
        ToDouble(b, second);
        double sum = 0.0;
        for (usize component = 0; component < Traits::ComponentCount; ++component) {
            sum += math::Square(first[component] - second[component]);
        }

        return sum;
    }

    // 곡선 구간의 최근접 점이다. position을 원점으로 옮긴 3차식 C(t) = c0 + c1 t + c2 t² + c3
    // t³에서 C·C' = 0의 5차식(UE와 같은 계수)의 근과 임계점을 후보로 한다. 후보 비교 순서는 t = 0,
    // t = 1, 근(오름차순), 임계점(오름차순)이고 엄격한 <로 바꾼다(UE는 끝점과 근만 본다).
    static NearestPoint NearestOnCubic(const Segment& range,
                                       const double (&start)[Traits::ComponentCount],
                                       const double (&end)[Traits::ComponentCount],
                                       const double (&position)[Traits::ComponentCount],
                                       usize segment) noexcept {
        double leave[Traits::ComponentCount] = {};
        double arrive[Traits::ComponentCount] = {};
        ToDouble(range.start->leaveTangent, leave);
        ToDouble(range.end->arriveTangent, arrive);
        const double difference = static_cast<double>(range.difference);

        double cubic[4][Traits::ComponentCount] = {};
        for (usize component = 0; component < Traits::ComponentCount; ++component) {
            const double p0 = start[component] - position[component];
            const double p1 = end[component] - position[component];
            const double t0 = leave[component] * difference;
            const double t1 = arrive[component] * difference;
            cubic[0][component] = p0;
            cubic[1][component] = t0;
            cubic[2][component] = -3.0 * p0 - 2.0 * t0 - t1 + 3.0 * p1;
            cubic[3][component] = 2.0 * p0 + t0 + t1 - 2.0 * p1;
        }

        auto dot = [&cubic](usize a, usize b) noexcept {
            double sum = 0.0;
            for (usize component = 0; component < Traits::ComponentCount; ++component) {
                sum += cubic[a][component] * cubic[b][component];
            }

            return sum;
        };

        const double quintic[6] = {
            dot(0, 1),
            dot(1, 1) + 2.0 * dot(0, 2),
            3.0 * dot(2, 1) + 3.0 * dot(3, 0),
            4.0 * dot(3, 1) + 2.0 * dot(2, 2),
            5.0 * dot(3, 2),
            3.0 * dot(3, 3),
        };

        auto distanceAt = [&cubic](double t) noexcept {
            double sum = 0.0;
            for (usize component = 0; component < Traits::ComponentCount; ++component) {
                const double value =
                    ((cubic[3][component] * t + cubic[2][component]) * t + cubic[1][component]) *
                        t +
                    cubic[0][component];
                sum += value * value;
            }

            return sum;
        };

        double bestParameter = 0.0;
        double bestDistance = distanceAt(0.0);
        const double endDistance = distanceAt(1.0);
        if (endDistance < bestDistance) {
            bestParameter = 1.0;
            bestDistance = endDistance;
        }

        double roots[5] = {};
        const usize rootCount = math::FindPolynomialRootsInRange<5>(quintic, 0.0, 1.0, roots);
        double criticalPoints[4] = {};
        const usize criticalCount =
            math::FindPolynomialCriticalPointsInRange<5>(quintic, 0.0, 1.0, criticalPoints);
        for (usize index = 0; index < rootCount + criticalCount; ++index) {
            const double candidate =
                index < rootCount ? roots[index] : criticalPoints[index - rootCount];
            const double candidateDistance = distanceAt(candidate);
            if (candidateDistance < bestDistance) {
                bestParameter = candidate;
                bestDistance = candidateDistance;
            }
        }

        NearestPoint result;
        result.input = static_cast<Scalar>(bestParameter) * range.difference + range.start->inValue;
        result.distanceSquared = static_cast<Scalar>(bestDistance);
        result.segment = segment;
        return result;
    }

    Array<Point> points_;
    bool looped_ = false;
    Scalar loopKeyOffset_ = static_cast<Scalar>(0);
};

using InterpCurveFloat = InterpCurve<float>;
using InterpCurveDouble = InterpCurve<double>;
using InterpCurveVector2 = InterpCurve<Vec2>;
using InterpCurveVector2d = InterpCurve<Vec2d>;
using InterpCurveVector = InterpCurve<Vec3>;
using InterpCurveVectord = InterpCurve<Vec3d>;
using InterpCurveVector4 = InterpCurve<Vec4>;
using InterpCurveVector4d = InterpCurve<Vec4d>;
using InterpCurveQuat = InterpCurve<Quat>;
using InterpCurveQuatd = InterpCurve<Quatd>;
using InterpCurveLinearColor = InterpCurve<LinearColor>;

} // namespace rocks
