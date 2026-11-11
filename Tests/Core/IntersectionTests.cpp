#include <Core/Math/Intersection.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

// ---- 공용 도우미 ----
template <typename T>
T Infinity() noexcept {
    if constexpr (IsSameV<T, float>) {
        return __builtin_huge_valf();
    } else {
        return __builtin_huge_val();
    }
}

template <typename T>
T QuietNaN() noexcept {
    if constexpr (IsSameV<T, float>) {
        return __builtin_nanf("");
    } else {
        return __builtin_nan("");
    }
}

// 0이 아닌 유한값에서 0에서 멀어지는/가까워지는 쪽으로 한 ulp 이동한다. +0의 UlpAway는 최소
// subnormal이다.
template <typename T>
T UlpAway(T value) noexcept {
    if constexpr (IsSameV<T, float>) {
        return __builtin_bit_cast(float, __builtin_bit_cast(uint32, value) + 1u);
    } else {
        return __builtin_bit_cast(double, __builtin_bit_cast(uint64, value) + 1ull);
    }
}

template <typename T>
T UlpToward(T value) noexcept {
    if constexpr (IsSameV<T, float>) {
        return __builtin_bit_cast(float, __builtin_bit_cast(uint32, value) - 1u);
    } else {
        return __builtin_bit_cast(double, __builtin_bit_cast(uint64, value) - 1ull);
    }
}

template <typename T>
Vector3<T> V(double x, double y, double z) noexcept {
    return Vector3<T>(static_cast<T>(x), static_cast<T>(y), static_cast<T>(z));
}

template <typename T>
T S(double value) noexcept {
    return static_cast<T>(value);
}

template <typename T>
Ray<T> MakeRay(const Vector3<T>& origin, const Vector3<T>& direction) noexcept {
    return Ray<T>::TryMake(origin, direction).Value();
}

template <typename T>
bool IsZeroVector(const Vector3<T>& vector) noexcept {
    return vector == Vector3<T>::Zero();
}

// 정밀도별 비교 허용 오차(좌표 규모 약 10 기준)다.
template <typename T>
T Loose() noexcept {
    return IsSameV<T, float> ? static_cast<T>(1.0e-4) : static_cast<T>(1.0e-10);
}

// ---- 결정적 난수(xorshift64*, 고정 seed) ----
// 주의: 한 식 안에서 여러 번 호출하면 함수 인자·연산자 피연산자의 평가 순서가 정해져 있지 않아
// 플랫폼(ABI)마다 다른 사례가 만들어진다. 여러 호출은 중괄호 초기화(왼쪽부터 평가 보장)나 지역
// 변수로 순서를 고정한다.
struct Random {
    uint64 state;

    explicit Random(uint64 seed) noexcept : state(seed) {
    }

    uint64 Next() noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ull;
    }

    // [0, 1)
    double Unit() noexcept {
        return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
    }

    double Range(double low, double high) noexcept {
        return low + (high - low) * Unit();
    }

    Vec3d Point(double extent) noexcept {
        return Vec3d{Range(-extent, extent), Range(-extent, extent), Range(-extent, extent)};
    }

    Vec3d UnitVector() noexcept {
        for (;;) {
            const Vec3d candidate = Point(1.0);
            const double lengthSquared = candidate.LengthSquared();
            if (lengthSquared > 1.0e-4 && lengthSquared <= 1.0) {
                return candidate / math::Sqrt(lengthSquared);
            }
        }
    }

    Quatd Rotation() noexcept {
        for (;;) {
            const Quatd candidate{Range(-1, 1), Range(-1, 1), Range(-1, 1), Range(-1, 1)};
            const double lengthSquared = candidate.LengthSquared();
            if (lengthSquared > 1.0e-4 && lengthSquared <= 1.0) {
                const double inverse = 1.0 / math::Sqrt(lengthSquared);
                return Quatd(candidate.x * inverse, candidate.y * inverse, candidate.z * inverse,
                             candidate.w * inverse);
            }
        }
    }
};

constexpr uint64 PropertySeed = 0x5EED6E0D00C0FFEEull;

template <typename T>
Vector3<T> Narrow(const Vec3d& value) noexcept {
    return Vector3<T>(value);
}

template <typename T>
Vec3d Widen(const Vector3<T>& value) noexcept {
    return Vec3d(static_cast<double>(value.x), static_cast<double>(value.y),
                 static_cast<double>(value.z));
}

template <typename T>
Quaternion<T> NarrowRotation(const Quatd& value) noexcept {
    // float로 줄인 뒤 다시 정규화해 단위 허용 오차 안에 둔다.
    const Quaternion<T> narrowed(static_cast<T>(value.x), static_cast<T>(value.y),
                                 static_cast<T>(value.z), static_cast<T>(value.w));
    return narrowed.TryNormalize().Value();
}

// ---- double 기준 oracle. 검사 대상 구현과 다른 방식으로 계산한다. ----
double ClampD(double value, double low, double high) noexcept {
    return value < low ? low : (value > high ? high : value);
}

double MaxD(double a, double b) noexcept {
    return a > b ? a : b;
}

double MinD(double a, double b) noexcept {
    return a < b ? a : b;
}

// 볼록 함수의 최솟값을 황금분할 탐색으로 찾는다. f(m1) == f(m2)이면 [m1, m2]에 최소가 있으므로
// high를 줄여도 된다. 90회 축소 후 구간 폭은 처음의 1e-18 수준이다.
template <typename Function>
double MinimizeConvex(double low, double high, Function function, double& argument) noexcept {
    const double ratio = 0.61803398874989484820;
    double m1 = high - (high - low) * ratio;
    double m2 = low + (high - low) * ratio;
    double f1 = function(m1);
    double f2 = function(m2);
    for (int iteration = 0; iteration < 90; ++iteration) {
        if (f1 <= f2) {
            high = m2;
            m2 = m1;
            f2 = f1;
            m1 = high - (high - low) * ratio;
            f1 = function(m1);
        } else {
            low = m1;
            m1 = m2;
            f1 = f2;
            m2 = low + (high - low) * ratio;
            f2 = function(m2);
        }
    }
    argument = (low + high) * 0.5;
    return function(argument);
}

// 무게중심 좌표 (u, v)에 대한 볼록 2차 함수를 중첩 삼분 탐색으로
// 최소화한다(삼각형 위 최근접 oracle).
double OracleTriangleDistanceSquared(const Vec3d& point, const Vec3d& a, const Vec3d& b,
                                     const Vec3d& c) noexcept {
    const Vec3d ab = b - a;
    const Vec3d ac = c - a;
    auto inner = [&](double u) {
        auto along = [&](double v) { return DistanceSquared(point, a + ab * u + ac * v); };
        double ignored = 0.0;
        return MinimizeConvex(0.0, 1.0 - u, along, ignored);
    };
    double ignored = 0.0;
    return MinimizeConvex(0.0, 1.0, inner, ignored);
}

double OracleSegmentPointDistanceSquared(const Vec3d& point, const Vec3d& start,
                                         const Vec3d& end) noexcept {
    auto along = [&](double t) { return DistanceSquared(point, start + (end - start) * t); };
    double ignored = 0.0;
    return MinimizeConvex(0.0, 1.0, along, ignored);
}

double OracleSegmentSegmentDistanceSquared(const Vec3d& a0, const Vec3d& a1, const Vec3d& b0,
                                           const Vec3d& b1) noexcept {
    auto outer = [&](double s) {
        const Vec3d onA = a0 + (a1 - a0) * s;
        auto inner = [&](double t) { return DistanceSquared(onA, b0 + (b1 - b0) * t); };
        double ignored = 0.0;
        return MinimizeConvex(0.0, 1.0, inner, ignored);
    };
    double ignored = 0.0;
    return MinimizeConvex(0.0, 1.0, outer, ignored);
}

// 축별로 독립 최소화한 box 거리²(분리 가능한 볼록 함수)다.
double OracleBoxDistanceSquared(const Vec3d& point, const Vec3d& minimum,
                                const Vec3d& maximum) noexcept {
    const double p[3] = {point.x, point.y, point.z};
    const double low[3] = {minimum.x, minimum.y, minimum.z};
    const double high[3] = {maximum.x, maximum.y, maximum.z};
    double sum = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        auto along = [&](double x) { return (p[axis] - x) * (p[axis] - x); };
        double ignored = 0.0;
        sum += MinimizeConvex(low[axis], high[axis], along, ignored);
    }
    return sum;
}

// 엔진 Quaternion을 거치지 않고 회전 행렬 열(local 축)을 계산한다.
void OracleAxes(const Quatd& q, Vec3d (&axes)[3]) noexcept {
    const double xx = q.x * q.x;
    const double yy = q.y * q.y;
    const double zz = q.z * q.z;
    const double xy = q.x * q.y;
    const double xz = q.x * q.z;
    const double yz = q.y * q.z;
    const double wx = q.w * q.x;
    const double wy = q.w * q.y;
    const double wz = q.w * q.z;
    axes[0] = Vec3d(1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy));
    axes[1] = Vec3d(2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx));
    axes[2] = Vec3d(2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy));
}

double BoxSignedDistance(const Vec3d& local, const Vec3d& half) noexcept {
    const Vec3d q(math::Abs(local.x) - half.x, math::Abs(local.y) - half.y,
                  math::Abs(local.z) - half.z);
    const Vec3d outside(MaxD(q.x, 0.0), MaxD(q.y, 0.0), MaxD(q.z, 0.0));
    const double inside = MinD(MaxD(q.x, MaxD(q.y, q.z)), 0.0);
    return outside.Length() + inside;
}

struct OracleHit {
    enum Kind {
        Miss,
        Hit,
        Inside,
        Ambiguous
    };
    Kind kind;
    double distance;
};

// signed distance 함수가 볼록이면 ray 위에서도 볼록이다. 최소값의 부호로 Hit/Miss를 정하고 첫 근을
// 이분법으로 찾는다. 경계에서 margin 이내인 경우는 판정하지 않는다(Ambiguous).
template <typename Sdf>
OracleHit OracleRaycast(const Vec3d& origin, const Vec3d& direction, double maxDistance,
                        double margin, Sdf sdf) noexcept {
    auto along = [&](double t) { return sdf(origin + direction * t); };
    const double startValue = along(0.0);
    if (startValue < -margin) {
        return {OracleHit::Inside, 0.0};
    }
    if (startValue <= margin) {
        return {OracleHit::Ambiguous, 0.0};
    }

    double nearest = 0.0;
    const double minimum = MinimizeConvex(0.0, maxDistance, along, nearest);
    if (minimum > margin) {
        return {OracleHit::Miss, 0.0};
    }
    if (minimum >= -margin) {
        return {OracleHit::Ambiguous, 0.0};
    }

    double low = 0.0;
    double high = nearest;
    for (int iteration = 0; iteration < 100; ++iteration) {
        const double middle = (low + high) * 0.5;
        if (along(middle) > 0.0) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return {OracleHit::Hit, (low + high) * 0.5};
}

// 두 OrientedBox의 15개 후보 축(면 6, 모서리 외적 9)에 꼭짓점을 직접 투영해 최대 간격을 구한다.
// > 0이면 분리, < 0이면 모든 축에서 겹친다. 길이가 거의 0인 외적 축은 면 축이 대신하므로 건너뛴다.
double OracleObbGap(const Vec3d& centerA, const Quatd& rotationA, const Vec3d& halfA,
                    const Vec3d& centerB, const Quatd& rotationB, const Vec3d& halfB) noexcept {
    Vec3d axesA[3];
    Vec3d axesB[3];
    OracleAxes(rotationA, axesA);
    OracleAxes(rotationB, axesB);

    Vec3d cornersA[8];
    Vec3d cornersB[8];
    for (int index = 0; index < 8; ++index) {
        const double sx = (index & 1) != 0 ? 1.0 : -1.0;
        const double sy = (index & 2) != 0 ? 1.0 : -1.0;
        const double sz = (index & 4) != 0 ? 1.0 : -1.0;
        cornersA[index] = centerA + axesA[0] * (sx * halfA.x) + axesA[1] * (sy * halfA.y) +
                          axesA[2] * (sz * halfA.z);
        cornersB[index] = centerB + axesB[0] * (sx * halfB.x) + axesB[1] * (sy * halfB.y) +
                          axesB[2] * (sz * halfB.z);
    }

    Vec3d candidates[15];
    int count = 0;
    for (int i = 0; i < 3; ++i) {
        candidates[count++] = axesA[i];
        candidates[count++] = axesB[i];
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const Vec3d axis = Cross(axesA[i], axesB[j]);
            if (axis.LengthSquared() > 1.0e-10) {
                candidates[count++] = axis;
            }
        }
    }

    double largestGap = -1.0e300;
    for (int index = 0; index < count; ++index) {
        const Vec3d axis = candidates[index] / candidates[index].Length();
        double lowA = 1.0e300;
        double highA = -1.0e300;
        double lowB = 1.0e300;
        double highB = -1.0e300;
        for (int corner = 0; corner < 8; ++corner) {
            const double projectedA = Dot(cornersA[corner], axis);
            const double projectedB = Dot(cornersB[corner], axis);
            lowA = MinD(lowA, projectedA);
            highA = MaxD(highA, projectedA);
            lowB = MinD(lowB, projectedB);
            highB = MaxD(highB, projectedB);
        }
        largestGap = MaxD(largestGap, MaxD(lowB - highA, lowA - highB));
    }
    return largestGap;
}

// =================================================================================================
// Ray 질의
// =================================================================================================

template <typename T>
void CheckRayPlane() {
    const Plane<T> ground;
    const T zero = S<T>(0);

    // 위에서 아래로: normal은 출발한 쪽(+Z)을 향한다.
    const RayHit<T> down = Raycast(Ray<T>(V<T>(1, 2, 5), Vector3<T>::Down()), ground);
    REQUIRE(down.IsHit());
    CHECK(down.distance == 5);
    CHECK(down.point == V<T>(1, 2, 0));
    CHECK(down.normal == Vector3<T>::Up());
    CHECK(!down.startedInside);

    const RayHit<T> up = Raycast(Ray<T>(V<T>(0, 0, -5), Vector3<T>::Up()), ground);
    REQUIRE(up.IsHit());
    CHECK(up.normal == Vector3<T>::Down());

    // 뒤쪽 교차와 평면 밖 평행 ray는 Miss다.
    CHECK(Raycast(Ray<T>(V<T>(0, 0, 5), Vector3<T>::Up()), ground).result == QueryResult::Miss);
    CHECK(Raycast(Ray<T>(V<T>(0, 0, 5), Vector3<T>::Forward()), ground).result ==
          QueryResult::Miss);

    // 평면 위 시작은 방향과 무관하게 거리 0 Hit다. 평행하면 normal은 plane.normal 그대로다.
    const RayHit<T> onPlaneParallel = Raycast(Ray<T>(V<T>(1, 2, 0), Vector3<T>::Forward()), ground);
    REQUIRE(onPlaneParallel.IsHit());
    CHECK(onPlaneParallel.distance == 0);
    CHECK(onPlaneParallel.normal == Vector3<T>::Up());
    const RayHit<T> onPlaneLeaving = Raycast(Ray<T>(V<T>(1, 2, 0), Vector3<T>::Up()), ground);
    REQUIRE(onPlaneLeaving.IsHit());
    CHECK(onPlaneLeaving.normal == Vector3<T>::Down());

    // signed zero: z = -0도 평면 위다.
    const RayHit<T> negativeZero = Raycast(Ray<T>(V<T>(0, 0, -0.0), Vector3<T>::Forward()), ground);
    CHECK(negativeZero.IsHit() && negativeZero.distance == 0);

    // maxDistance 경계는 닫혀 있다.
    const Ray<T> fromAbove(V<T>(0, 0, 5), Vector3<T>::Down());
    CHECK(Raycast(fromAbove, ground, S<T>(5)).IsHit());
    CHECK(Raycast(fromAbove, ground, UlpToward(S<T>(5))).result == QueryResult::Miss);
    CHECK(Raycast(fromAbove, ground, S<T>(-0.0)).result == QueryResult::Miss);
    CHECK(Raycast(Ray<T>(V<T>(0, 0, 0), Vector3<T>::Down()), ground, S<T>(-0.0)).IsHit());

    // 비스듬한 ray.
    const RayHit<T> oblique = Raycast(MakeRay(V<T>(0, 0, 4), V<T>(3, 0, -4)), ground);
    REQUIRE(oblique.IsHit());
    CHECK(math::Abs(oblique.distance - S<T>(5)) <= Loose<T>());
    CHECK(oblique.point.Equals(V<T>(3, 0, 0), Loose<T>()));

    // 입력 오류는 Miss가 아니라 InvalidInput이다.
    CHECK(Raycast(fromAbove, ground, QuietNaN<T>()).result == QueryResult::InvalidInput);
    CHECK(Raycast(fromAbove, ground, S<T>(-1)).result == QueryResult::InvalidInput);
    CHECK(Raycast(Ray<T>(V<T>(0, 0, 5), V<T>(0, 0, -2)), ground).result ==
          QueryResult::InvalidInput);
    CHECK(Raycast(fromAbove, Plane<T>(V<T>(0, 0, 2), zero)).result == QueryResult::InvalidInput);
    CHECK(Raycast(Ray<T>(V<T>(QuietNaN<T>(), 0, 5), Vector3<T>::Down()), ground).result ==
          QueryResult::InvalidInput);

    // Miss 결과의 나머지 필드는 기본값이다.
    const RayHit<T> miss = Raycast(Ray<T>(V<T>(0, 0, 5), Vector3<T>::Up()), ground);
    CHECK(miss.distance == 0 && IsZeroVector(miss.point) && IsZeroVector(miss.normal));
}

template <typename T>
void CheckRaySphere() {
    const Sphere<T> unit(Vector3<T>::Zero(), S<T>(1));
    const Ray<T> fromLeft(V<T>(-5, 0, 0), Vector3<T>::Forward());

    const RayHit<T> front = Raycast(fromLeft, unit);
    REQUIRE(front.IsHit());
    CHECK(front.distance == 4);
    CHECK(front.point == V<T>(-1, 0, 0));
    CHECK(front.normal == V<T>(-1, 0, 0));
    CHECK(!front.startedInside);

    // 접선 접촉은 Hit이고 반지름을 한 ulp 줄이면 Miss다.
    const Ray<T> tangent(V<T>(-5, 1, 0), Vector3<T>::Forward());
    const RayHit<T> touch = Raycast(tangent, unit);
    REQUIRE(touch.IsHit());
    CHECK(touch.distance == 5);
    CHECK(touch.point == V<T>(0, 1, 0));
    CHECK(touch.normal == V<T>(0, 1, 0));
    CHECK(Raycast(tangent, Sphere<T>(Vector3<T>::Zero(), UlpToward(S<T>(1)))).result ==
          QueryResult::Miss);

    // 시작점 내부·경계: 거리 0, normal 영벡터, point = origin.
    const RayHit<T> inside = Raycast(Ray<T>(V<T>(0.25, 0, 0), Vector3<T>::Forward()), unit);
    REQUIRE(inside.IsHit());
    CHECK(inside.startedInside);
    CHECK(inside.distance == 0);
    CHECK(inside.point == V<T>(0.25, 0, 0));
    CHECK(IsZeroVector(inside.normal));
    const RayHit<T> onSurface = Raycast(Ray<T>(V<T>(1, 0, 0), Vector3<T>::Forward()), unit);
    CHECK(onSurface.IsHit() && onSurface.startedInside);

    // 뒤쪽, maxDistance 경계.
    CHECK(Raycast(Ray<T>(V<T>(-5, 0, 0), Vector3<T>::Backward()), unit).result ==
          QueryResult::Miss);
    CHECK(Raycast(fromLeft, unit, S<T>(4)).IsHit());
    CHECK(Raycast(fromLeft, unit, UlpToward(S<T>(4))).result == QueryResult::Miss);
    CHECK(Raycast(fromLeft, unit, 10).IsHit());

    // 반지름 0은 점이며 정확히 지나는 ray만 Hit, normal은 -direction이다.
    const RayHit<T> point = Raycast(fromLeft, Sphere<T>(V<T>(2, 0, 0), S<T>(0)));
    REQUIRE(point.IsHit());
    CHECK(point.distance == 7);
    CHECK(point.normal == V<T>(-1, 0, 0));
    CHECK(Raycast(tangent, Sphere<T>(V<T>(2, 0, 0), S<T>(0))).result == QueryResult::Miss);

    CHECK(Raycast(fromLeft, Sphere<T>(Vector3<T>::Zero(), S<T>(-1))).result ==
          QueryResult::InvalidInput);
    CHECK(Raycast(fromLeft, Sphere<T>(V<T>(0, QuietNaN<T>(), 0), S<T>(1))).result ==
          QueryResult::InvalidInput);
    CHECK(Raycast(fromLeft, Sphere<T>(Vector3<T>::Zero(), Infinity<T>())).result ==
          QueryResult::InvalidInput);
}

template <typename T>
void CheckRayBox() {
    const Box<T> box(V<T>(0, 0, 0), V<T>(1, 1, 1));

    const RayHit<T> fromLeft = Raycast(Ray<T>(V<T>(-2, 0.5, 0.5), Vector3<T>::Forward()), box);
    REQUIRE(fromLeft.IsHit());
    CHECK(fromLeft.distance == 2);
    CHECK(fromLeft.point == V<T>(0, 0.5, 0.5));
    CHECK(fromLeft.normal == V<T>(-1, 0, 0));

    const RayHit<T> fromAbove = Raycast(Ray<T>(V<T>(0.5, 0.5, 3), Vector3<T>::Down()), box);
    REQUIRE(fromAbove.IsHit());
    CHECK(fromAbove.distance == 2);
    CHECK(fromAbove.point.z == 1);
    CHECK(fromAbove.normal == V<T>(0, 0, 1));

    // 면 위를 평행하게 스치면 Hit, 한 ulp 밖이면 Miss다.
    const RayHit<T> grazing = Raycast(Ray<T>(V<T>(-2, 1, 0.5), Vector3<T>::Forward()), box);
    REQUIRE(grazing.IsHit());
    CHECK(grazing.distance == 2);
    CHECK(grazing.point == V<T>(0, 1, 0.5));
    CHECK(Raycast(Ray<T>(V<T>(-2, UlpAway(S<T>(1)), 0.5), Vector3<T>::Forward()), box).result ==
          QueryResult::Miss);
    CHECK(Raycast(Ray<T>(V<T>(-2, 1, 1), Vector3<T>::Forward()), box).IsHit());

    // -0 방향 성분은 평행 축으로 처리한다.
    const RayHit<T> signedZero = Raycast(Ray<T>(V<T>(-2, 0.5, 0.5), V<T>(1, -0.0, -0.0)), box);
    CHECK(signedZero.IsHit() && signedZero.distance == 2);

    // 시작점 내부·경계.
    const RayHit<T> inside = Raycast(Ray<T>(V<T>(0.5, 0.5, 0.5), Vector3<T>::Forward()), box);
    CHECK(inside.IsHit() && inside.startedInside && inside.distance == 0);
    CHECK(IsZeroVector(inside.normal));
    const RayHit<T> onFace = Raycast(Ray<T>(V<T>(0, 0.5, 0.5), Vector3<T>::Backward()), box);
    CHECK(onFace.IsHit() && onFace.startedInside);

    // 뒤쪽과 maxDistance 경계.
    CHECK(Raycast(Ray<T>(V<T>(-2, 0.5, 0.5), Vector3<T>::Backward()), box).result ==
          QueryResult::Miss);
    const Ray<T> toward(V<T>(-2, 0.5, 0.5), Vector3<T>::Forward());
    CHECK(Raycast(toward, box, S<T>(2)).IsHit());
    CHECK(Raycast(toward, box, UlpToward(S<T>(2))).result == QueryResult::Miss);

    // 대각선 ray는 꼭짓점에 닿는다. point는 box 밖으로 나가지 않는다.
    const RayHit<T> corner = Raycast(MakeRay(V<T>(-1, -1, -1), V<T>(1, 1, 1)), box);
    REQUIRE(corner.IsHit());
    CHECK(math::Abs(corner.distance - math::Sqrt(S<T>(3))) <= Loose<T>());
    CHECK(corner.point.Equals(Vector3<T>::Zero(), Loose<T>()));
    CHECK(ContainsPoint(box, corner.point) == QueryResult::Hit);

    // 0 부피 box(면)도 유효하다.
    const Box<T> flat(V<T>(0, 0, 0), V<T>(1, 1, 0));
    const RayHit<T> flatHit = Raycast(Ray<T>(V<T>(0.5, 0.5, 1), Vector3<T>::Down()), flat);
    CHECK(flatHit.IsHit() && flatHit.distance == 1 && flatHit.normal == V<T>(0, 0, 1));
    const Box<T> signedZeroBox(V<T>(-0.0, -0.0, -0.0), V<T>(0, 0, 0));
    CHECK(Raycast(Ray<T>(V<T>(-1, 0, 0), Vector3<T>::Forward()), signedZeroBox).IsHit());

    CHECK(Raycast(toward, Box<T>(V<T>(1, 0, 0), V<T>(0, 1, 1))).result ==
          QueryResult::InvalidInput);
    CHECK(Raycast(toward, Box<T>::Empty()).result == QueryResult::InvalidInput);
    CHECK(Raycast(toward, box, QuietNaN<T>()).result == QueryResult::InvalidInput);
}

template <typename T>
void CheckRayTriangle() {
    const Triangle<T> triangle(V<T>(0, 0, 0), V<T>(1, 0, 0), V<T>(0, 1, 0));

    const RayHit<T> front = Raycast(Ray<T>(V<T>(0.25, 0.25, 1), Vector3<T>::Down()), triangle);
    REQUIRE(front.IsHit());
    CHECK(front.distance == 1);
    CHECK(front.point == V<T>(0.25, 0.25, 0));
    CHECK(front.normal == Vector3<T>::Up());
    CHECK(!front.startedInside);

    // 기본은 양면이며 normal은 ray가 온 쪽을 향한다. FrontOnly는 뒷면을 놓친다.
    const Ray<T> fromBelow(V<T>(0.25, 0.25, -1), Vector3<T>::Up());
    const RayHit<T> back = Raycast(fromBelow, triangle);
    REQUIRE(back.IsHit());
    CHECK(back.normal == Vector3<T>::Down());
    CHECK(Raycast(fromBelow, triangle, Infinity<T>(), TriangleSides::FrontOnly).result ==
          QueryResult::Miss);
    CHECK(Raycast(Ray<T>(V<T>(0.25, 0.25, 1), Vector3<T>::Down()), triangle, Infinity<T>(),
                  TriangleSides::FrontOnly)
              .IsHit());

    // 변·꼭짓점 위는 Hit, 변에서 최소 subnormal만큼 밖이면 Miss다.
    const RayHit<T> edge = Raycast(Ray<T>(V<T>(0.5, 0, 1), Vector3<T>::Down()), triangle);
    CHECK(edge.IsHit() && edge.point == V<T>(0.5, 0, 0));
    CHECK(Raycast(Ray<T>(V<T>(0, 0, 1), Vector3<T>::Down()), triangle).IsHit());
    CHECK(Raycast(Ray<T>(V<T>(0.5, -UlpAway(S<T>(0)), 1), Vector3<T>::Down()), triangle).result ==
          QueryResult::Miss);

    // 공면 ray·평행 ray·뒤쪽은 Miss다.
    CHECK(Raycast(Ray<T>(V<T>(-1, 0.25, 0), Vector3<T>::Forward()), triangle).result ==
          QueryResult::Miss);
    CHECK(Raycast(Ray<T>(V<T>(-1, 0.25, 1), Vector3<T>::Forward()), triangle).result ==
          QueryResult::Miss);
    CHECK(Raycast(Ray<T>(V<T>(0.25, 0.25, 1), Vector3<T>::Up()), triangle).result ==
          QueryResult::Miss);

    // 퇴화 삼각형은 입력 오류가 아니라 Miss다.
    const Triangle<T> degenerate(V<T>(-1, 0, 0), V<T>(0, 0, 0), V<T>(1, 0, 0));
    CHECK(Raycast(Ray<T>(V<T>(0, 0, 1), Vector3<T>::Down()), degenerate).result ==
          QueryResult::Miss);

    const Ray<T> down(V<T>(0.25, 0.25, 1), Vector3<T>::Down());
    CHECK(Raycast(down, triangle, S<T>(1)).IsHit());
    CHECK(Raycast(down, triangle, UlpToward(S<T>(1))).result == QueryResult::Miss);
    CHECK(Raycast(down, Triangle<T>(V<T>(0, 0, 0), V<T>(1, 0, 0), V<T>(0, QuietNaN<T>(), 0)))
              .result == QueryResult::InvalidInput);
}

template <typename T>
void CheckRayCapsule() {
    const Capsule<T> capsule(V<T>(0, 0, -2), V<T>(0, 0, 2), S<T>(1));

    const RayHit<T> side = Raycast(Ray<T>(V<T>(-5, 0, 0), Vector3<T>::Forward()), capsule);
    REQUIRE(side.IsHit());
    CHECK(side.distance == 4);
    CHECK(side.point == V<T>(-1, 0, 0));
    CHECK(side.normal == V<T>(-1, 0, 0));

    const RayHit<T> cap = Raycast(Ray<T>(V<T>(0, 0, 10), Vector3<T>::Down()), capsule);
    REQUIRE(cap.IsHit());
    CHECK(cap.distance == 7);
    CHECK(cap.point == V<T>(0, 0, 3));
    CHECK(cap.normal == V<T>(0, 0, 1));

    // 끝 반구에 비스듬히 닿는 경우: 진입점은 x = -sqrt(0.75).
    const RayHit<T> shoulder = Raycast(Ray<T>(V<T>(-5, 0, 2.5), Vector3<T>::Forward()), capsule);
    REQUIRE(shoulder.IsHit());
    const T shoulderX = -math::Sqrt(S<T>(0.75));
    CHECK(math::Abs(shoulder.distance - (S<T>(5) + shoulderX)) <= Loose<T>());
    CHECK(shoulder.normal.Equals(V<T>(shoulderX, 0, 0.5), Loose<T>()));

    // 옆면 접선은 Hit, 한 ulp 밖은 Miss다.
    const RayHit<T> tangent = Raycast(Ray<T>(V<T>(-5, 1, 0), Vector3<T>::Forward()), capsule);
    REQUIRE(tangent.IsHit());
    CHECK(tangent.distance == 5);
    CHECK(tangent.point == V<T>(0, 1, 0));
    CHECK(Raycast(Ray<T>(V<T>(-5, UlpAway(S<T>(1)), 0), Vector3<T>::Forward()), capsule).result ==
          QueryResult::Miss);

    const RayHit<T> inside = Raycast(Ray<T>(V<T>(0, 0.5, 2.5), Vector3<T>::Up()), capsule);
    CHECK(inside.IsHit() && inside.startedInside && IsZeroVector(inside.normal));
    const RayHit<T> onSurface = Raycast(Ray<T>(V<T>(1, 0, 0), Vector3<T>::Forward()), capsule);
    CHECK(onSurface.IsHit() && onSurface.startedInside);
    CHECK(Raycast(Ray<T>(V<T>(-5, 0, 0), Vector3<T>::Backward()), capsule).result ==
          QueryResult::Miss);
    CHECK(Raycast(Ray<T>(V<T>(-5, 0, 0), Vector3<T>::Forward()), capsule, S<T>(3.5)).result ==
          QueryResult::Miss);

    // 축 방향 ray가 축 범위 밖의 무한 원기둥 안에서 출발하면 끝 반구가 첫 진입이다.
    const RayHit<T> axial = Raycast(Ray<T>(V<T>(0.5, 0, -10), Vector3<T>::Up()), capsule);
    REQUIRE(axial.IsHit());
    CHECK(math::Abs(axial.distance - (S<T>(8) - math::Sqrt(S<T>(0.75)))) <= Loose<T>());

    // 0 길이 capsule은 sphere와 비트 단위로 같다.
    const Capsule<T> point(V<T>(3, 1, 0), V<T>(3, 1, 0), S<T>(1.5));
    const Sphere<T> sphere(V<T>(3, 1, 0), S<T>(1.5));
    const Ray<T> probe = MakeRay(V<T>(-4, -2, 1), V<T>(7, 3, -1));
    const RayHit<T> fromCapsule = Raycast(probe, point);
    const RayHit<T> fromSphere = Raycast(probe, sphere);
    CHECK(fromCapsule.IsHit() && fromSphere.IsHit());
    CHECK(fromCapsule.distance == fromSphere.distance);
    CHECK(fromCapsule.point == fromSphere.point);
    CHECK(fromCapsule.normal == fromSphere.normal);

    CHECK(Raycast(probe, Capsule<T>(V<T>(0, 0, 0), V<T>(0, 0, 1), S<T>(-1))).result ==
          QueryResult::InvalidInput);
}

template <typename T>
void CheckRayOrientedBox() {
    const OrientedBox<T> aligned(V<T>(5, 0, 0), Quaternion<T>::Identity(), V<T>(1, 1, 1));
    const Ray<T> forward(Vector3<T>::Zero(), Vector3<T>::Forward());

    // 항등 회전은 같은 AABB와 정확히 같다.
    const RayHit<T> alignedHit = Raycast(forward, aligned);
    const RayHit<T> boxHit = Raycast(forward, Box<T>(V<T>(4, -1, -1), V<T>(6, 1, 1)));
    REQUIRE(alignedHit.IsHit());
    CHECK(alignedHit.distance == boxHit.distance);
    CHECK(alignedHit.point == boxHit.point);
    CHECK(alignedHit.normal == boxHit.normal);

    // Z축 90도 회전, local x 반경 2 → world y 반경 2, world x 반경 1.
    const Quaternion<T> quarterTurn =
        Quaternion<T>::FromUnitAxisAngle(Vector3<T>::Up(), math::HalfPi<T>);
    const OrientedBox<T> turned(V<T>(5, 0, 0), quarterTurn, V<T>(2, 1, 1));
    const RayHit<T> turnedHit = Raycast(forward, turned);
    REQUIRE(turnedHit.IsHit());
    CHECK(math::Abs(turnedHit.distance - S<T>(4)) <= Loose<T>());
    CHECK(turnedHit.normal.Equals(V<T>(-1, 0, 0), Loose<T>()));
    CHECK(turnedHit.point.Equals(V<T>(4, 0, 0), Loose<T>()));
    CHECK(Raycast(Ray<T>(V<T>(0, 1.5, 0), Vector3<T>::Forward()), turned).IsHit());
    CHECK(Raycast(Ray<T>(V<T>(0, 2.5, 0), Vector3<T>::Forward()), turned).result ==
          QueryResult::Miss);

    const RayHit<T> inside = Raycast(Ray<T>(V<T>(5, 0, 0), Vector3<T>::Up()), turned);
    CHECK(inside.IsHit() && inside.startedInside && inside.point == V<T>(5, 0, 0));

    CHECK(Raycast(forward, OrientedBox<T>(V<T>(5, 0, 0), Quaternion<T>(0, 0, 0, 2), V<T>(1, 1, 1)))
              .result == QueryResult::InvalidInput);
    CHECK(Raycast(forward, OrientedBox<T>(V<T>(5, 0, 0), Quaternion<T>::Identity(), V<T>(1, -1, 1)))
              .result == QueryResult::InvalidInput);
}

// =================================================================================================
// Segment 질의
// =================================================================================================

template <typename T>
void CheckSegmentPlane() {
    const Plane<T> ground;

    const SegmentHit<T> crossing = SegmentCast(Segment<T>(V<T>(0, 0, 2), V<T>(0, 0, -2)), ground);
    REQUIRE(crossing.IsHit());
    CHECK(crossing.t == S<T>(0.5));
    CHECK(crossing.point == V<T>(0, 0, 0));
    CHECK(crossing.normal == Vector3<T>::Up());

    // 끝점 접촉은 t가 정확히 0/1이다.
    const SegmentHit<T> endOn = SegmentCast(Segment<T>(V<T>(0, 0, 2), V<T>(1, 0, 0)), ground);
    CHECK(endOn.IsHit() && endOn.t == 1 && endOn.point == V<T>(1, 0, 0));
    const SegmentHit<T> startOn = SegmentCast(Segment<T>(V<T>(1, 0, 0), V<T>(0, 0, -2)), ground);
    CHECK(startOn.IsHit() && startOn.t == 0 && startOn.normal == Vector3<T>::Up());

    CHECK(SegmentCast(Segment<T>(V<T>(0, 0, 2), V<T>(0, 0, 1)), ground).result ==
          QueryResult::Miss);

    // 평면 위 segment와 0 길이 segment.
    CHECK(SegmentCast(Segment<T>(V<T>(0, 0, 0), V<T>(5, 0, 0)), ground).IsHit());
    CHECK(SegmentCast(Segment<T>(V<T>(3, 3, 0), V<T>(3, 3, 0)), ground).IsHit());
    CHECK(SegmentCast(Segment<T>(V<T>(3, 3, 1), V<T>(3, 3, 1)), ground).result ==
          QueryResult::Miss);

    CHECK(SegmentCast(Segment<T>(V<T>(0, 0, QuietNaN<T>()), V<T>(0, 0, 1)), ground).result ==
          QueryResult::InvalidInput);
    CHECK(SegmentCast(Segment<T>(V<T>(0, 0, 2), V<T>(0, 0, -2)), Plane<T>(V<T>(0, 0, 0.5), S<T>(0)))
              .result == QueryResult::InvalidInput);
}

template <typename T>
void CheckSegmentSphere() {
    const Sphere<T> unit(Vector3<T>::Zero(), S<T>(1));

    const SegmentHit<T> through = SegmentCast(Segment<T>(V<T>(-5, 0, 0), V<T>(5, 0, 0)), unit);
    REQUIRE(through.IsHit());
    CHECK(through.t == S<T>(0.4));
    CHECK(through.point == V<T>(-1, 0, 0));
    CHECK(through.normal == V<T>(-1, 0, 0));

    const SegmentHit<T> endOn = SegmentCast(Segment<T>(V<T>(-5, 0, 0), V<T>(-1, 0, 0)), unit);
    CHECK(endOn.IsHit() && endOn.t == 1);

    // 끝점이 표면에서 한 ulp 밖이면 Miss다. start = -2이면 end - start = 1 - ulp가 정확히 표현된다.
    CHECK(SegmentCast(Segment<T>(V<T>(-2, 0, 0), V<T>(UlpAway(S<T>(-1)), 0, 0)), unit).result ==
          QueryResult::Miss);

    // 반대로 start = -5이면 end - start = 4 - ulp(1)/2가 짝수 반올림으로 정확히 4가 된다. 계산은
    // start + (end - start) * t로 하므로 이 segment는 표면(-1)에 닿는 것으로
    // 판정된다(문서화된 한계).
    const SegmentHit<T> rounded =
        SegmentCast(Segment<T>(V<T>(-5, 0, 0), V<T>(UlpAway(S<T>(-1)), 0, 0)), unit);
    CHECK(rounded.IsHit() && rounded.t == 1);

    const SegmentHit<T> startInside = SegmentCast(Segment<T>(V<T>(0, 0, 0), V<T>(5, 0, 0)), unit);
    CHECK(startInside.IsHit() && startInside.startedInside && startInside.t == 0);

    // 0 길이 segment는 점 포함 판정이다.
    CHECK(SegmentCast(Segment<T>(V<T>(0.5, 0, 0), V<T>(0.5, 0, 0)), unit).IsHit());
    CHECK(SegmentCast(Segment<T>(V<T>(1, 0, 0), V<T>(1, 0, 0)), unit).IsHit());
    CHECK(SegmentCast(Segment<T>(V<T>(2, 0, 0), V<T>(2, 0, 0)), unit).result == QueryResult::Miss);

    CHECK(SegmentCast(Segment<T>(V<T>(-5, 0, 0), V<T>(5, 0, 0)),
                      Sphere<T>(Vector3<T>::Zero(), S<T>(-1)))
              .result == QueryResult::InvalidInput);
}

template <typename T>
void CheckSegmentBox() {
    const Box<T> box(V<T>(0, 0, 0), V<T>(1, 1, 1));

    const SegmentHit<T> through =
        SegmentCast(Segment<T>(V<T>(-1, 0.5, 0.5), V<T>(3, 0.5, 0.5)), box);
    REQUIRE(through.IsHit());
    CHECK(through.t == S<T>(0.25));
    CHECK(through.point == V<T>(0, 0.5, 0.5));
    CHECK(through.normal == V<T>(-1, 0, 0));

    const SegmentHit<T> endOn = SegmentCast(Segment<T>(V<T>(-1, 0.5, 0.5), V<T>(0, 0.5, 0.5)), box);
    CHECK(endOn.IsHit() && endOn.t == 1);
    CHECK(SegmentCast(Segment<T>(V<T>(-1, 0.5, 0.5), V<T>(-0.5, 0.5, 0.5)), box).result ==
          QueryResult::Miss);

    const SegmentHit<T> pointInside = SegmentCast(Segment<T>(V<T>(1, 1, 1), V<T>(1, 1, 1)), box);
    CHECK(pointInside.IsHit() && pointInside.startedInside);
    CHECK(SegmentCast(Segment<T>(V<T>(2, 1, 1), V<T>(2, 1, 1)), box).result == QueryResult::Miss);
    CHECK(SegmentCast(Segment<T>(V<T>(-1, 0.5, 0.5), V<T>(3, 0.5, 0.5)), Box<T>::Empty()).result ==
          QueryResult::InvalidInput);
}

template <typename T>
void CheckSegmentTriangle() {
    const Triangle<T> triangle(V<T>(0, 0, 0), V<T>(1, 0, 0), V<T>(0, 1, 0));

    const SegmentHit<T> through =
        SegmentCast(Segment<T>(V<T>(0.25, 0.25, 1), V<T>(0.25, 0.25, -1)), triangle);
    REQUIRE(through.IsHit());
    CHECK(through.t == S<T>(0.5));
    CHECK(through.normal == Vector3<T>::Up());

    const SegmentHit<T> endOn =
        SegmentCast(Segment<T>(V<T>(0.25, 0.25, 1), V<T>(0.25, 0.25, 0)), triangle);
    CHECK(endOn.IsHit() && endOn.t == 1);
    CHECK(SegmentCast(Segment<T>(V<T>(0.25, 0.25, 1), V<T>(0.25, 0.25, 0.5)), triangle).result ==
          QueryResult::Miss);

    // 아래에서 올라오는 segment는 FrontOnly에서 Miss다.
    const Segment<T> upward(V<T>(0.25, 0.25, -1), V<T>(0.25, 0.25, 1));
    CHECK(SegmentCast(upward, triangle).IsHit());
    CHECK(SegmentCast(upward, triangle, TriangleSides::FrontOnly).result == QueryResult::Miss);

    // 0 길이 segment: 삼각형 위(변 포함)면 Hit.
    const SegmentHit<T> onFace =
        SegmentCast(Segment<T>(V<T>(0.25, 0.25, 0), V<T>(0.25, 0.25, 0)), triangle);
    CHECK(onFace.IsHit() && onFace.t == 0 && onFace.normal == Vector3<T>::Up());
    CHECK(SegmentCast(Segment<T>(V<T>(0.5, 0.5, 0), V<T>(0.5, 0.5, 0)), triangle).IsHit());
    CHECK(SegmentCast(Segment<T>(V<T>(0.75, 0.75, 0), V<T>(0.75, 0.75, 0)), triangle).result ==
          QueryResult::Miss);
    CHECK(SegmentCast(Segment<T>(V<T>(0.25, 0.25, 0.5), V<T>(0.25, 0.25, 0.5)), triangle).result ==
          QueryResult::Miss);

    const Triangle<T> degenerate(V<T>(-1, 0, 0), V<T>(0, 0, 0), V<T>(1, 0, 0));
    CHECK(SegmentCast(Segment<T>(V<T>(0, 0, 1), V<T>(0, 0, -1)), degenerate).result ==
          QueryResult::Miss);
    CHECK(SegmentCast(Segment<T>(V<T>(0, 0, 0), V<T>(0, 0, 0)), degenerate).result ==
          QueryResult::Miss);
    CHECK(SegmentCast(Segment<T>(V<T>(0, 0, 1), V<T>(0, 0, -1)),
                      Triangle<T>(V<T>(0, 0, 0), V<T>(Infinity<T>(), 0, 0), V<T>(0, 1, 0)))
              .result == QueryResult::InvalidInput);
}

// =================================================================================================
// 포함·겹침
// =================================================================================================

template <typename T>
void CheckContainsPoint() {
    const Box<T> box(V<T>(0, 0, 0), V<T>(1, 1, 1));
    CHECK(ContainsPoint(box, V<T>(1, 0.5, 0.5)) == QueryResult::Hit);
    CHECK(ContainsPoint(box, V<T>(UlpAway(S<T>(1)), 0.5, 0.5)) == QueryResult::Miss);
    CHECK(ContainsPoint(box, V<T>(-0.0, 0, 0)) == QueryResult::Hit);
    CHECK(ContainsPoint(box, V<T>(QuietNaN<T>(), 0, 0)) == QueryResult::InvalidInput);
    CHECK(ContainsPoint(Box<T>::Empty(), V<T>(0, 0, 0)) == QueryResult::InvalidInput);

    const Sphere<T> unit(Vector3<T>::Zero(), S<T>(1));
    CHECK(ContainsPoint(unit, V<T>(0, 1, 0)) == QueryResult::Hit);
    CHECK(ContainsPoint(unit, V<T>(0, UlpAway(S<T>(1)), 0)) == QueryResult::Miss);
    CHECK(ContainsPoint(Sphere<T>(V<T>(2, 2, 2), S<T>(0)), V<T>(2, 2, 2)) == QueryResult::Hit);
    CHECK(ContainsPoint(unit, V<T>(0, Infinity<T>(), 0)) == QueryResult::InvalidInput);

    const Capsule<T> capsule(V<T>(0, 0, 0), V<T>(0, 0, 4), S<T>(1));
    CHECK(ContainsPoint(capsule, V<T>(1, 0, 2)) == QueryResult::Hit);
    CHECK(ContainsPoint(capsule, V<T>(0, 0, 5)) == QueryResult::Hit);
    CHECK(ContainsPoint(capsule, V<T>(0, 0, UlpAway(S<T>(5)))) == QueryResult::Miss);
    CHECK(ContainsPoint(capsule, V<T>(0, 0, -1)) == QueryResult::Hit);
    CHECK(ContainsPoint(Capsule<T>(V<T>(0, 0, 0), V<T>(0, 0, 4), S<T>(-1)), V<T>(0, 0, 0)) ==
          QueryResult::InvalidInput);

    const OrientedBox<T> aligned(V<T>(0, 0, 0), Quaternion<T>::Identity(), V<T>(1, 2, 3));
    CHECK(ContainsPoint(aligned, V<T>(1, -2, 3)) == QueryResult::Hit);
    CHECK(ContainsPoint(aligned, V<T>(UlpAway(S<T>(1)), 0, 0)) == QueryResult::Miss);
    const OrientedBox<T> turned(V<T>(0, 0, 0),
                                Quaternion<T>::FromUnitAxisAngle(Vector3<T>::Up(), math::HalfPi<T>),
                                V<T>(2, 1, 1));
    CHECK(ContainsPoint(turned, V<T>(0, 1.9, 0)) == QueryResult::Hit);
    CHECK(ContainsPoint(turned, V<T>(1.1, 0, 0)) == QueryResult::Miss);
    CHECK(ContainsPoint(OrientedBox<T>(V<T>(0, 0, 0), Quaternion<T>(0, 0, 0, 0), V<T>(1, 1, 1)),
                        V<T>(0, 0, 0)) == QueryResult::InvalidInput);
}

template <typename T>
void CheckSphereAndBoxOverlaps() {
    const T one = S<T>(1);
    const Sphere<T> origin(Vector3<T>::Zero(), one);

    // 정확히 닿으면 Hit, 한 ulp 멀어지면 Miss다(숨은 허용 오차 없음).
    CHECK(Overlap(origin, Sphere<T>(V<T>(2, 0, 0), one)) == QueryResult::Hit);
    CHECK(Overlap(origin, Sphere<T>(V<T>(UlpAway(S<T>(2)), 0, 0), one)) == QueryResult::Miss);
    CHECK(Overlap(origin, Sphere<T>(V<T>(0.1, 0, 0), S<T>(0.25))) == QueryResult::Hit);
    CHECK(Overlap(Sphere<T>(V<T>(3, 3, 3), S<T>(0)), Sphere<T>(V<T>(3, 3, 3), S<T>(0))) ==
          QueryResult::Hit);
    CHECK(Overlap(origin, Sphere<T>(V<T>(2, 0, 0), S<T>(-1))) == QueryResult::InvalidInput);
    CHECK(Overlap(Sphere<T>(V<T>(QuietNaN<T>(), 0, 0), one), origin) == QueryResult::InvalidInput);

    const Box<T> box(V<T>(0, 0, 0), V<T>(1, 1, 1));
    const Sphere<T> beside(V<T>(2, 0.5, 0.5), one);
    CHECK(Overlap(beside, box) == QueryResult::Hit);
    CHECK(Overlap(box, beside) == QueryResult::Hit);
    const Sphere<T> justOut(V<T>(UlpAway(S<T>(2)), 0.5, 0.5), one);
    CHECK(Overlap(justOut, box) == QueryResult::Miss);
    CHECK(Overlap(box, justOut) == QueryResult::Miss);
    CHECK(Overlap(Sphere<T>(V<T>(0.5, 0.5, 0.5), S<T>(0)), box) == QueryResult::Hit);
    CHECK(Overlap(Sphere<T>(V<T>(1, 1, 1), S<T>(0)), Box<T>(V<T>(1, 1, 1), V<T>(1, 1, 1))) ==
          QueryResult::Hit);
    CHECK(Overlap(beside, Box<T>(V<T>(1, 0, 0), V<T>(0, 1, 1))) == QueryResult::InvalidInput);

    // box 쌍: 면 접촉 Hit, 한 ulp 틈 Miss, 포함 Hit.
    CHECK(Overlap(box, Box<T>(V<T>(1, 0, 0), V<T>(2, 1, 1))) == QueryResult::Hit);
    CHECK(Overlap(box, Box<T>(V<T>(UlpAway(S<T>(1)), 0, 0), V<T>(2, 1, 1))) == QueryResult::Miss);
    CHECK(Overlap(box, Box<T>(V<T>(1, 1, 1), V<T>(2, 2, 2))) == QueryResult::Hit);
    CHECK(Overlap(box, Box<T>(V<T>(0.25, 0.25, 0.25), V<T>(0.5, 0.5, 0.5))) == QueryResult::Hit);
    CHECK(Overlap(box, Box<T>(V<T>(0.5, 0.5, 0.5), V<T>(0.5, 0.5, 0.5))) == QueryResult::Hit);
    CHECK(Overlap(box, Box<T>::Empty()) == QueryResult::InvalidInput);
}

template <typename T>
void CheckPlaneClassification() {
    const Plane<T> ground;
    const Box<T> above(V<T>(0, 0, 1), V<T>(1, 1, 2));
    const Box<T> touching(V<T>(0, 0, 0), V<T>(1, 1, 2));
    const Box<T> straddling(V<T>(0, 0, -1), V<T>(1, 1, 2));
    const Box<T> below(V<T>(0, 0, -3), V<T>(1, 1, -1));

    CHECK(ClassifyPlaneSide(ground, above) == PlaneSide::Front);
    CHECK(ClassifyPlaneSide(ground, touching) == PlaneSide::Intersecting);
    CHECK(ClassifyPlaneSide(ground, straddling) == PlaneSide::Intersecting);
    CHECK(ClassifyPlaneSide(ground, below) == PlaneSide::Back);
    CHECK(ClassifyPlaneSide(ground, Box<T>(V<T>(0, 0, UlpAway(S<T>(0))), V<T>(1, 1, 1))) ==
          PlaneSide::Front);
    CHECK(ClassifyPlaneSide(ground, Box<T>(V<T>(5, 5, 0), V<T>(5, 5, 0))) ==
          PlaneSide::Intersecting);

    CHECK(Overlap(above, ground) == QueryResult::Miss);
    CHECK(Overlap(ground, touching) == QueryResult::Hit);
    CHECK(Overlap(touching, ground) == QueryResult::Hit);
    CHECK(Overlap(Box<T>::Empty(), ground) == QueryResult::InvalidInput);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 3), S<T>(0)), above) == PlaneSide::InvalidInput);

    // 기울어진 평면: 가장 가까운 꼭짓점이 평면 위에 정확히 있으면 Intersecting.
    auto tilted = Plane<T>::TryFromPointNormal(V<T>(1, 1, 1), V<T>(1, 1, 1));
    REQUIRE(tilted);
    CHECK(ClassifyPlaneSide(tilted.Value(), Box<T>(V<T>(-1, -1, -1), V<T>(0, 0, 0))) ==
          PlaneSide::Back);
    CHECK(ClassifyPlaneSide(tilted.Value(), Box<T>(V<T>(0, 0, 0), V<T>(4, 4, 4))) ==
          PlaneSide::Intersecting);

    const Sphere<T> resting(V<T>(0, 0, 1), S<T>(1));
    CHECK(ClassifyPlaneSide(ground, resting) == PlaneSide::Intersecting);
    CHECK(ClassifyPlaneSide(ground, Sphere<T>(V<T>(0, 0, UlpAway(S<T>(1))), S<T>(1))) ==
          PlaneSide::Front);
    CHECK(ClassifyPlaneSide(ground, Sphere<T>(V<T>(0, 0, -3), S<T>(1))) == PlaneSide::Back);
    CHECK(Overlap(resting, ground) == QueryResult::Hit);
    CHECK(Overlap(ground, resting) == QueryResult::Hit);
    CHECK(Overlap(ground, Sphere<T>(V<T>(0, 0, 3), S<T>(1))) == QueryResult::Miss);
    CHECK(Overlap(ground, Sphere<T>(V<T>(0, 0, 3), S<T>(-1))) == QueryResult::InvalidInput);
}

template <typename T>
void CheckCapsuleOverlaps() {
    const Capsule<T> capsule(V<T>(0, 0, 0), V<T>(0, 0, 4), S<T>(1));

    const Sphere<T> touching(V<T>(3, 0, 2), S<T>(2));
    CHECK(Overlap(capsule, touching) == QueryResult::Hit);
    CHECK(Overlap(touching, capsule) == QueryResult::Hit);
    CHECK(Overlap(capsule, Sphere<T>(V<T>(UlpAway(S<T>(3)), 0, 2), S<T>(2))) == QueryResult::Miss);
    CHECK(Overlap(capsule, Sphere<T>(V<T>(0, 0, 7), S<T>(2))) == QueryResult::Hit);
    CHECK(Overlap(capsule, Sphere<T>(V<T>(0, 0, UlpAway(S<T>(7))), S<T>(2))) == QueryResult::Miss);
    CHECK(Overlap(capsule, Sphere<T>(V<T>(3, 0, 2), S<T>(-2))) == QueryResult::InvalidInput);

    // 평행 capsule 접촉과 한 ulp 분리.
    const Capsule<T> parallel(V<T>(3, 0, 1), V<T>(3, 0, 5), S<T>(2));
    CHECK(Overlap(capsule, parallel) == QueryResult::Hit);
    CHECK(Overlap(parallel, capsule) == QueryResult::Hit);
    const Capsule<T> parallelOut(V<T>(UlpAway(S<T>(3)), 0, 1), V<T>(UlpAway(S<T>(3)), 0, 5),
                                 S<T>(2));
    CHECK(Overlap(capsule, parallelOut) == QueryResult::Miss);
    CHECK(Overlap(parallelOut, capsule) == QueryResult::Miss);

    // 엇갈린 축.
    const Capsule<T> crossing(V<T>(-5, 3, 2), V<T>(5, 3, 2), S<T>(2));
    CHECK(Overlap(capsule, crossing) == QueryResult::Hit);
    const Capsule<T> crossingOut(V<T>(-5, UlpAway(S<T>(3)), 2), V<T>(5, UlpAway(S<T>(3)), 2),
                                 S<T>(2));
    CHECK(Overlap(capsule, crossingOut) == QueryResult::Miss);

    // 0 길이 capsule 두 개는 sphere 쌍과 같다.
    CHECK(Overlap(Capsule<T>(V<T>(0, 0, 0), V<T>(0, 0, 0), S<T>(1)),
                  Capsule<T>(V<T>(2, 0, 0), V<T>(2, 0, 0), S<T>(1))) == QueryResult::Hit);
    CHECK(Overlap(capsule, Capsule<T>(V<T>(0, 0, 0), V<T>(0, 0, 1), QuietNaN<T>())) ==
          QueryResult::InvalidInput);
}

template <typename T>
void CheckSphereTriangleOverlap() {
    const Triangle<T> triangle(V<T>(0, 0, 0), V<T>(4, 0, 0), V<T>(0, 4, 0));

    // 면·변·꼭짓점 접촉.
    CHECK(Overlap(Sphere<T>(V<T>(1, 1, 2), S<T>(2)), triangle) == QueryResult::Hit);
    CHECK(Overlap(triangle, Sphere<T>(V<T>(1, 1, 2), S<T>(2))) == QueryResult::Hit);
    CHECK(Overlap(Sphere<T>(V<T>(1, 1, 2), UlpToward(S<T>(2))), triangle) == QueryResult::Miss);
    CHECK(Overlap(Sphere<T>(V<T>(2, -1, 0), S<T>(1)), triangle) == QueryResult::Hit);
    CHECK(Overlap(Sphere<T>(V<T>(2, -1, 0), UlpToward(S<T>(1))), triangle) == QueryResult::Miss);
    CHECK(Overlap(Sphere<T>(V<T>(-3, 0, 0), S<T>(3)), triangle) == QueryResult::Hit);
    CHECK(Overlap(Sphere<T>(V<T>(-3, 0, 0), UlpToward(S<T>(3))), triangle) == QueryResult::Miss);

    // 퇴화 삼각형은 선분처럼 판정한다.
    const Triangle<T> collinear(V<T>(0, 0, 0), V<T>(2, 0, 0), V<T>(4, 0, 0));
    CHECK(Overlap(Sphere<T>(V<T>(3, 1, 0), S<T>(1)), collinear) == QueryResult::Hit);
    CHECK(Overlap(Sphere<T>(V<T>(3, 1, 0), UlpToward(S<T>(1))), collinear) == QueryResult::Miss);
    CHECK(Overlap(Sphere<T>(V<T>(5, 0, 0), S<T>(1)), collinear) == QueryResult::Hit);

    CHECK(Overlap(Sphere<T>(V<T>(1, 1, 2), S<T>(2)),
                  Triangle<T>(V<T>(0, 0, 0), V<T>(QuietNaN<T>(), 0, 0), V<T>(0, 4, 0))) ==
          QueryResult::InvalidInput);
}

template <typename T>
void CheckOrientedBoxOverlap() {
    const T one = S<T>(1);
    const OrientedBox<T> a(Vector3<T>::Zero(), Quaternion<T>::Identity(), Vector3<T>::Splat(one));

    // 축 정렬 접촉은 Hit, 명확한 틈은 Miss다.
    const OrientedBox<T> touching(V<T>(2, 0, 0), Quaternion<T>::Identity(), Vector3<T>::Splat(one));
    CHECK(Overlap(a, touching) == QueryResult::Hit);
    CHECK(Overlap(touching, a) == QueryResult::Hit);
    const OrientedBox<T> apart(V<T>(2.5, 0, 0), Quaternion<T>::Identity(), Vector3<T>::Splat(one));
    CHECK(Overlap(a, apart) == QueryResult::Miss);

    // Z축 45도 회전: 꼭짓점까지 거리 sqrt(2).
    const Quaternion<T> eighth =
        Quaternion<T>::FromUnitAxisAngle(Vector3<T>::Up(), math::Pi<T> / S<T>(4));
    const T reach = S<T>(1) + math::Sqrt(S<T>(2));
    CHECK(Overlap(a, OrientedBox<T>(V<T>(reach - S<T>(0.01), 0, 0), eighth,
                                    Vector3<T>::Splat(one))) == QueryResult::Hit);
    CHECK(Overlap(a, OrientedBox<T>(V<T>(reach + S<T>(0.01), 0, 0), eighth,
                                    Vector3<T>::Splat(one))) == QueryResult::Miss);

    // 같은 회전(모든 모서리 평행)에서도 외적 축 잡음으로 잘못 분리하지 않는다.
    const Quaternion<T> tilt = Quaternion<T>::TryFromAxisAngle(V<T>(1, 2, 3), S<T>(0.7)).Value();
    const OrientedBox<T> tiltedA(V<T>(1, 1, 1), tilt, V<T>(1, 2, 0.5));
    const OrientedBox<T> tiltedB(tiltedA.ToWorld(V<T>(1.9, 0, 0)), tilt, V<T>(1, 2, 0.5));
    const OrientedBox<T> tiltedFar(tiltedA.ToWorld(V<T>(2.1, 0, 0)), tilt, V<T>(1, 2, 0.5));
    CHECK(Overlap(tiltedA, tiltedB) == QueryResult::Hit);
    CHECK(Overlap(tiltedA, tiltedFar) == QueryResult::Miss);

    // 0 부피 box와 명시적 epsilon 검증.
    const OrientedBox<T> pointBox(V<T>(1, 1, 1), Quaternion<T>::Identity(), Vector3<T>::Zero());
    CHECK(Overlap(a, pointBox) == QueryResult::Hit);
    CHECK(Overlap(a, touching, S<T>(0)) == QueryResult::Hit);
    CHECK(Overlap(a, touching, S<T>(-1)) == QueryResult::InvalidInput);
    CHECK(Overlap(a, touching, QuietNaN<T>()) == QueryResult::InvalidInput);
    CHECK(Overlap(a, touching, Infinity<T>()) == QueryResult::InvalidInput);
    CHECK(Overlap(a, OrientedBox<T>(V<T>(2, 0, 0), Quaternion<T>(0, 0, 0, 2), Vector3<T>::One())) ==
          QueryResult::InvalidInput);
}

// =================================================================================================
// 최근접점·거리
// =================================================================================================

template <typename T>
void CheckClosestPoints() {
    // 평면
    const Plane<T> raised(Vector3<T>::Up(), S<T>(5));
    auto onPlane = TryClosestPoint(raised, V<T>(1, 2, 7));
    REQUIRE(onPlane);
    CHECK(onPlane.Value() == V<T>(1, 2, 5));
    auto signedDistance = TrySignedDistance(raised, V<T>(1, 2, 3));
    REQUIRE(signedDistance);
    CHECK(signedDistance.Value() == -2);
    CHECK(!TryClosestPoint(Plane<T>(V<T>(0, 0, 2), S<T>(0)), V<T>(0, 0, 0)));
    CHECK(!TrySignedDistance(raised, V<T>(QuietNaN<T>(), 0, 0)));

    // segment: 끝점 밖은 끝점을 정확히, 안쪽은 투영점.
    const Segment<T> segment(V<T>(0, 0, 0), V<T>(4, 0, 0));
    CHECK(TryClosestPoint(segment, V<T>(-2, 3, 0)).Value() == V<T>(0, 0, 0));
    CHECK(TryClosestPoint(segment, V<T>(9, 3, 0)).Value() == V<T>(4, 0, 0));
    CHECK(TryClosestPoint(segment, V<T>(1, 3, 0)).Value() == V<T>(1, 0, 0));
    CHECK(TryDistanceSquared(segment, V<T>(1, 3, 0)).Value() == 9);
    const Segment<T> pointSegment(V<T>(1, 1, 1), V<T>(1, 1, 1));
    CHECK(TryClosestPoint(pointSegment, V<T>(5, 5, 5)).Value() == V<T>(1, 1, 1));
    CHECK(!TryClosestPoint(segment, V<T>(0, Infinity<T>(), 0)));

    // box: 내부 점은 그대로, 밖은 축별 clamp.
    const Box<T> box(V<T>(0, 0, 0), V<T>(1, 2, 3));
    CHECK(TryClosestPoint(box, V<T>(0.5, 0.5, 0.5)).Value() == V<T>(0.5, 0.5, 0.5));
    CHECK(TryClosestPoint(box, V<T>(5, -1, 1)).Value() == V<T>(1, 0, 1));
    CHECK(TryDistanceSquared(box, V<T>(5, -1, 1)).Value() == 17);
    CHECK(TryDistanceSquared(box, V<T>(1, 2, 3)).Value() == 0);
    CHECK(!TryClosestPoint(Box<T>::Empty(), V<T>(0, 0, 0)));

    // 삼각형 7개 Voronoi 영역.
    const Triangle<T> triangle(V<T>(0, 0, 0), V<T>(4, 0, 0), V<T>(0, 4, 0));
    CHECK(TryClosestPoint(triangle, V<T>(-1, -1, 3)).Value() == V<T>(0, 0, 0));
    CHECK(TryClosestPoint(triangle, V<T>(5, -1, 0)).Value() == V<T>(4, 0, 0));
    CHECK(TryClosestPoint(triangle, V<T>(-1, 5, 0)).Value() == V<T>(0, 4, 0));
    CHECK(TryClosestPoint(triangle, V<T>(2, -3, 1)).Value() == V<T>(2, 0, 0));
    CHECK(TryClosestPoint(triangle, V<T>(-3, 2, 1)).Value() == V<T>(0, 2, 0));
    CHECK(TryClosestPoint(triangle, V<T>(3, 3, 1)).Value() == V<T>(2, 2, 0));
    CHECK(TryClosestPoint(triangle, V<T>(1, 1, 5)).Value() == V<T>(1, 1, 0));
    CHECK(TryDistanceSquared(triangle, V<T>(1, 1, 5)).Value() == 25);

    // 퇴화 삼각형: 일직선(가운데 꼭짓점이 끝이 아님), 중복 꼭짓점.
    const Triangle<T> collinear(V<T>(0, 0, 0), V<T>(4, 0, 0), V<T>(2, 0, 0));
    CHECK(TryClosestPoint(collinear, V<T>(3, 1, 0)).Value() == V<T>(3, 0, 0));
    CHECK(TryClosestPoint(collinear, V<T>(5, 1, 0)).Value() == V<T>(4, 0, 0));
    const Triangle<T> pinched(V<T>(0, 0, 0), V<T>(0, 0, 0), V<T>(0, 4, 0));
    CHECK(TryClosestPoint(pinched, V<T>(1, 2, 0)).Value() == V<T>(0, 2, 0));
    const Triangle<T> single(V<T>(1, 1, 1), V<T>(1, 1, 1), V<T>(1, 1, 1));
    CHECK(TryClosestPoint(single, V<T>(5, 5, 5)).Value() == V<T>(1, 1, 1));
    CHECK(!TryClosestPoint(triangle, V<T>(QuietNaN<T>(), 0, 0)));

    // OrientedBox: 내부 점은 그대로, 밖은 local clamp 후 world.
    const OrientedBox<T> aligned(V<T>(0, 0, 0), Quaternion<T>::Identity(), V<T>(1, 1, 1));
    CHECK(TryClosestPoint(aligned, V<T>(3, 0.5, 0)).Value() == V<T>(1, 0.5, 0));
    CHECK(TryClosestPoint(aligned, V<T>(0.5, 0.25, -0.5)).Value() == V<T>(0.5, 0.25, -0.5));
    CHECK(TryDistanceSquared(aligned, V<T>(3, 0.5, 0)).Value() == 4);
    const OrientedBox<T> turned(V<T>(0, 0, 0),
                                Quaternion<T>::FromUnitAxisAngle(Vector3<T>::Up(), math::HalfPi<T>),
                                V<T>(2, 1, 1));
    CHECK(TryClosestPoint(turned, V<T>(0, 5, 0)).Value().Equals(V<T>(0, 2, 0), Loose<T>()));
    CHECK(math::Abs(TryDistanceSquared(turned, V<T>(0, 5, 0)).Value() - S<T>(9)) <=
          Loose<T>() * 10);
    CHECK(!TryClosestPoint(OrientedBox<T>(V<T>(0, 0, 0), Quaternion<T>(0, 0, 0, 3), V<T>(1, 1, 1)),
                           V<T>(0, 0, 0)));
}

template <typename T>
void CheckSegmentSegment() {
    const Segment<T> alongX(V<T>(-1, 0, 0), V<T>(1, 0, 0));

    auto crossing = TryClosestPoints(alongX, Segment<T>(V<T>(0, -1, 0), V<T>(0, 1, 0)));
    REQUIRE(crossing);
    CHECK(crossing.Value().distanceSquared == 0);
    CHECK(crossing.Value().s == S<T>(0.5) && crossing.Value().t == S<T>(0.5));

    auto skew = TryClosestPoints(alongX, Segment<T>(V<T>(0, -1, 2), V<T>(0, 1, 2)));
    REQUIRE(skew);
    CHECK(skew.Value().distanceSquared == 4);
    CHECK(skew.Value().pointA == V<T>(0, 0, 0));
    CHECK(skew.Value().pointB == V<T>(0, 0, 2));

    // 평행: 겹치는 구간이 있으면 수직 거리, 없으면 끝점 사이 거리.
    auto overlapping = TryClosestPoints(Segment<T>(V<T>(0, 0, 0), V<T>(4, 0, 0)),
                                        Segment<T>(V<T>(1, 0, 3), V<T>(6, 0, 3)));
    REQUIRE(overlapping);
    CHECK(overlapping.Value().distanceSquared == 9);
    auto disjoint = TryClosestPoints(Segment<T>(V<T>(0, 0, 0), V<T>(1, 0, 0)),
                                     Segment<T>(V<T>(3, 0, 0), V<T>(5, 0, 0)));
    REQUIRE(disjoint);
    CHECK(disjoint.Value().distanceSquared == 4);
    CHECK(disjoint.Value().pointA == V<T>(1, 0, 0));
    CHECK(disjoint.Value().pointB == V<T>(3, 0, 0));
    auto antiParallel = TryClosestPoints(Segment<T>(V<T>(0, 0, 0), V<T>(4, 0, 0)),
                                         Segment<T>(V<T>(6, 1, 0), V<T>(5, 1, 0)));
    REQUIRE(antiParallel);
    CHECK(antiParallel.Value().distanceSquared == 2);

    // 0 길이 segment.
    auto points = TryClosestPoints(Segment<T>(V<T>(1, 1, 1), V<T>(1, 1, 1)),
                                   Segment<T>(V<T>(1, 3, 1), V<T>(1, 3, 1)));
    REQUIRE(points);
    CHECK(points.Value().distanceSquared == 4);
    auto pointToSegment = TryClosestPoints(Segment<T>(V<T>(2, 5, 0), V<T>(2, 5, 0)), alongX);
    REQUIRE(pointToSegment);
    CHECK(pointToSegment.Value().pointB == V<T>(1, 0, 0));
    CHECK(pointToSegment.Value().distanceSquared == 26);

    // 순서를 바꾸면 s/t와 점만 바뀌고 거리는 비트 단위로 같다.
    const Segment<T> first(V<T>(0.3, -1.7, 2.1), V<T>(4.2, 0.6, -0.9));
    const Segment<T> second(V<T>(-2.2, 3.1, 0.4), V<T>(1.9, -0.8, 1.3));
    const SegmentClosestPoints<T> forward = TryClosestPoints(first, second).Value();
    const SegmentClosestPoints<T> backward = TryClosestPoints(second, first).Value();
    CHECK(forward.distanceSquared == backward.distanceSquared);
    CHECK(forward.s == backward.t && forward.t == backward.s);
    CHECK(forward.pointA == backward.pointB && forward.pointB == backward.pointA);

    // 거의 평행한 segment도 유한하고 올바른 거리다.
    auto nearlyParallel = TryClosestPoints(Segment<T>(V<T>(0, 0, 0), V<T>(10, 0, 0)),
                                           Segment<T>(V<T>(0, 1, 0), V<T>(10, 1.000001, 0)));
    REQUIRE(nearlyParallel);
    CHECK(math::Abs(nearlyParallel.Value().distanceSquared - S<T>(1)) <= S<T>(1.0e-4));

    CHECK(!TryClosestPoints(alongX, Segment<T>(V<T>(0, 0, 0), V<T>(0, QuietNaN<T>(), 0))));
}

// =================================================================================================
// 큰 좌표(double 1e7)
// =================================================================================================

void CheckLargeCoordinates() {
    const double base = 1.0e7;
    const RayHit<double> sphereHit =
        Raycast(Ray<double>(Vec3d(base - 100, base, 0), Vec3d(1, 0, 0)),
                Sphere<double>(Vec3d(base, base, 0), 1.0));
    REQUIRE(sphereHit.IsHit());
    CHECK(sphereHit.distance == 99);
    CHECK(sphereHit.point == Vec3d(base - 1, base, 0));

    const Box<double> box(Vec3d(base, base, 0), Vec3d(base + 1, base + 1, 1));
    const RayHit<double> boxHit =
        Raycast(Ray<double>(Vec3d(base - 10, base + 0.5, 0.5), Vec3d(1, 0, 0)), box);
    REQUIRE(boxHit.IsHit());
    CHECK(boxHit.distance == 10);
    CHECK(boxHit.point.x == base);
    CHECK(Overlap(box, Box<double>(Vec3d(base + 1, base, 0), Vec3d(base + 2, base + 1, 1))) ==
          QueryResult::Hit);

    const Triangle<double> triangle(Vec3d(base, base, 0), Vec3d(base + 4, base, 0),
                                    Vec3d(base, base + 4, 0));
    CHECK(TryClosestPoint(triangle, Vec3d(base + 1, base + 1, 2)).Value() ==
          Vec3d(base + 1, base + 1, 0));

    auto closest = TryClosestPoints(
        Segment<double>(Vec3d(base, base, 0), Vec3d(base + 4, base, 0)),
        Segment<double>(Vec3d(base + 2, base - 1, 3), Vec3d(base + 2, base + 1, 3)));
    REQUIRE(closest);
    CHECK(closest.Value().distanceSquared == 9);

    const OrientedBox<double> obbA(Vec3d(base, base, base), Quatd::Identity(), Vec3d(1, 1, 1));
    const OrientedBox<double> obbB(Vec3d(base + 2, base, base), Quatd::Identity(), Vec3d(1, 1, 1));
    CHECK(Overlap(obbA, obbB) == QueryResult::Hit);
}

// =================================================================================================
// seeded property 검증
// =================================================================================================

template <typename T>
void PropertyClosestPointOnBox() {
    Random random(PropertySeed ^ sizeof(T));
    const double tolerance = IsSameV<T, float> ? 1.0e-4 : 1.0e-10;
    for (int iteration = 0; iteration < 300; ++iteration) {
        const Vec3d cornerA = random.Point(5.0);
        const Vec3d extent{random.Range(0, 3), random.Range(0, 3), random.Range(0, 3)};
        const Box<T> box(Narrow<T>(cornerA), Narrow<T>(cornerA + extent));
        if (!box.IsValid()) {
            continue;
        }
        const Vector3<T> point = Narrow<T>(random.Point(10.0));
        const Vec3d minimum = Widen(box.min);
        const Vec3d maximum = Widen(box.max);
        const Vec3d query = Widen(point);

        const Vec3d closest = Widen(TryClosestPoint(box, point).Value());
        const double distanceSquared = DistanceSquared(query, closest);
        CHECK(ContainsPoint(box, Narrow<T>(closest)) == QueryResult::Hit);
        CHECK(math::Abs(distanceSquared - OracleBoxDistanceSquared(query, minimum, maximum)) <=
              tolerance * (1.0 + distanceSquared));

        // 표본 상한: 어느 box 위 표본보다 멀 수 없다.
        double sampled = 1.0e300;
        for (int i = 0; i <= 8; ++i) {
            for (int j = 0; j <= 8; ++j) {
                for (int k = 0; k <= 8; ++k) {
                    const Vec3d sample(minimum.x + (maximum.x - minimum.x) * i / 8.0,
                                       minimum.y + (maximum.y - minimum.y) * j / 8.0,
                                       minimum.z + (maximum.z - minimum.z) * k / 8.0);
                    sampled = MinD(sampled, DistanceSquared(query, sample));
                }
            }
        }
        CHECK(distanceSquared <= sampled + tolerance * (1.0 + sampled));
    }
}

template <typename T>
void PropertyClosestPointOnTriangle() {
    Random random(PropertySeed + 17 * sizeof(T));
    const double tolerance = IsSameV<T, float> ? 2.0e-4 : 1.0e-9;
    for (int iteration = 0; iteration < 400; ++iteration) {
        const Vec3d a = random.Point(4.0);
        const Vec3d b = random.Point(4.0);
        Vec3d c = random.Point(4.0);
        const int kind = iteration % 4;
        if (kind == 1) {
            // 거의 일직선인 sliver.
            // 난수 호출 순서를 플랫폼과 무관하게 고정한다(연산자 피연산자 평가 순서는 미지정).
            const double along = random.Range(-0.5, 1.5);
            const Vec3d jitter = random.Point(1.0e-5);
            c = a + (b - a) * along + jitter;
        } else if (kind == 2) {
            // 정확히 일직선(가운데 또는 바깥 꼭짓점).
            c = a + (b - a) * (iteration % 8 == 2 ? 0.5 : 2.0);
        }
        const Triangle<T> triangle(Narrow<T>(a), Narrow<T>(b), Narrow<T>(c));
        const Vector3<T> point = Narrow<T>(random.Point(8.0));
        const Vec3d wa = Widen(triangle.a);
        const Vec3d wb = Widen(triangle.b);
        const Vec3d wc = Widen(triangle.c);
        const Vec3d query = Widen(point);

        const Vec3d closest = Widen(TryClosestPoint(triangle, point).Value());
        const double ours = DistanceSquared(query, closest);
        const double oracle = OracleTriangleDistanceSquared(query, wa, wb, wc);
        const double scale = 1.0 + oracle;
        CHECK(math::Abs(ours - oracle) <= tolerance * scale * 10.0);

        // 결과 점은 삼각형 위에 있어야 한다.
        CHECK(OracleTriangleDistanceSquared(closest, wa, wb, wc) <= tolerance * scale);

        // 조밀한 표본 상한.
        double sampled = 1.0e300;
        const int steps = 24;
        for (int i = 0; i <= steps; ++i) {
            for (int j = 0; j <= steps - i; ++j) {
                const Vec3d sample =
                    wa + (wb - wa) * (i / double(steps)) + (wc - wa) * (j / double(steps));
                sampled = MinD(sampled, DistanceSquared(query, sample));
            }
        }
        CHECK(ours <= sampled + tolerance * scale * 10.0);
        CHECK(TryDistanceSquared(triangle, point).Value() ==
              DistanceSquared(point, TryClosestPoint(triangle, point).Value()));
    }
}

template <typename T>
void PropertySegmentSegment() {
    Random random(PropertySeed + 31 * sizeof(T));
    const double tolerance = IsSameV<T, float> ? 2.0e-4 : 1.0e-9;
    for (int iteration = 0; iteration < 300; ++iteration) {
        const Vec3d a0 = random.Point(5.0);
        Vec3d a1 = random.Point(5.0);
        Vec3d b0 = random.Point(5.0);
        Vec3d b1 = random.Point(5.0);
        const int kind = iteration % 5;
        if (kind == 1) {
            // 평행(같은 방향 벡터).
            b1 = b0 + (a1 - a0) * random.Range(-1.5, 1.5);
        } else if (kind == 2) {
            a1 = a0;
        } else if (kind == 3) {
            // 같은 직선 위.
            b0 = a0 + (a1 - a0) * random.Range(-1, 2);
            b1 = a0 + (a1 - a0) * random.Range(-1, 2);
        }
        const Segment<T> first(Narrow<T>(a0), Narrow<T>(a1));
        const Segment<T> second(Narrow<T>(b0), Narrow<T>(b1));

        const SegmentClosestPoints<T> result = TryClosestPoints(first, second).Value();
        const double oracle = OracleSegmentSegmentDistanceSquared(
            Widen(first.start), Widen(first.end), Widen(second.start), Widen(second.end));
        const double scale = 1.0 + oracle;
        CHECK(math::Abs(static_cast<double>(result.distanceSquared) - oracle) <=
              tolerance * scale * 10.0);
        CHECK(result.s >= 0 && result.s <= 1 && result.t >= 0 && result.t <= 1);
        CHECK(result.pointA == first.PointAt(result.s));
        CHECK(result.pointB == second.PointAt(result.t));

        const SegmentClosestPoints<T> swapped = TryClosestPoints(second, first).Value();
        CHECK(result.distanceSquared == swapped.distanceSquared);
        CHECK(result.s == swapped.t && result.t == swapped.s);

        // capsule 겹침은 같은 거리로 판정하며 순서와 무관하다.
        const double distance = math::Sqrt(oracle);
        const double radiusA = random.Range(0, 1.5);
        const double radiusB = random.Range(0, 1.5);
        const Capsule<T> capsuleA(first.start, first.end, static_cast<T>(radiusA));
        const Capsule<T> capsuleB(second.start, second.end, static_cast<T>(radiusB));
        const QueryResult overlap = Overlap(capsuleA, capsuleB);
        CHECK(overlap == Overlap(capsuleB, capsuleA));
        const double gap = distance - (radiusA + radiusB);
        if (gap > 1.0e-3) {
            CHECK(overlap == QueryResult::Miss);
        } else if (gap < -1.0e-3) {
            CHECK(overlap == QueryResult::Hit);
        }
    }
}

template <typename T>
void PropertySphereOverlaps() {
    Random random(PropertySeed + 43 * sizeof(T));
    const double margin = IsSameV<T, float> ? 1.0e-3 : 1.0e-8;
    for (int iteration = 0; iteration < 500; ++iteration) {
        const Vec3d corner = random.Point(4.0);
        const Box<T> box{
            Narrow<T>(corner),
            Narrow<T>(corner + Vec3d{random.Range(0, 3), random.Range(0, 3), random.Range(0, 3)})};
        const Sphere<T> sphere{Narrow<T>(random.Point(7.0)), static_cast<T>(random.Range(0, 3))};
        if (!box.IsValid()) {
            continue;
        }

        const double distance = math::Sqrt(
            OracleBoxDistanceSquared(Widen(sphere.center), Widen(box.min), Widen(box.max)));
        const QueryResult sphereBox = Overlap(sphere, box);
        CHECK(sphereBox == Overlap(box, sphere));
        if (distance > sphere.radius + margin) {
            CHECK(sphereBox == QueryResult::Miss);
        } else if (distance < sphere.radius - margin) {
            CHECK(sphereBox == QueryResult::Hit);
        }

        const Sphere<T> other{Narrow<T>(random.Point(7.0)), static_cast<T>(random.Range(0, 3))};
        CHECK(Overlap(sphere, other) == Overlap(other, sphere));

        const Box<T> otherBox(sphere.center - Vector3<T>::Splat(other.radius),
                              sphere.center + Vector3<T>::Splat(other.radius));
        CHECK(Overlap(box, otherBox) == Overlap(otherBox, box));

        auto plane = Plane<T>::TryFromPointNormal(Narrow<T>(random.Point(3.0)),
                                                  Narrow<T>(random.UnitVector()));
        REQUIRE(plane);
        CHECK(Overlap(box, plane.Value()) == Overlap(plane.Value(), box));
        CHECK(Overlap(sphere, plane.Value()) == Overlap(plane.Value(), sphere));

        const Capsule<T> capsule{Narrow<T>(random.Point(5.0)), Narrow<T>(random.Point(5.0)),
                                 static_cast<T>(random.Range(0, 1.5))};
        const QueryResult capsuleSphere = Overlap(capsule, sphere);
        CHECK(capsuleSphere == Overlap(sphere, capsule));
        const double capsuleDistance = math::Sqrt(OracleSegmentPointDistanceSquared(
            Widen(sphere.center), Widen(capsule.start), Widen(capsule.end)));
        const double capsuleGap = capsuleDistance - (static_cast<double>(capsule.radius) +
                                                     static_cast<double>(sphere.radius));
        if (capsuleGap > margin) {
            CHECK(capsuleSphere == QueryResult::Miss);
        } else if (capsuleGap < -margin) {
            CHECK(capsuleSphere == QueryResult::Hit);
        }

        const Triangle<T> triangle{Narrow<T>(random.Point(4.0)), Narrow<T>(random.Point(4.0)),
                                   Narrow<T>(random.Point(4.0))};
        const QueryResult sphereTriangle = Overlap(sphere, triangle);
        CHECK(sphereTriangle == Overlap(triangle, sphere));
        const double triangleDistance = math::Sqrt(OracleTriangleDistanceSquared(
            Widen(sphere.center), Widen(triangle.a), Widen(triangle.b), Widen(triangle.c)));
        if (triangleDistance > sphere.radius + margin) {
            CHECK(sphereTriangle == QueryResult::Miss);
        } else if (triangleDistance < sphere.radius - margin) {
            CHECK(sphereTriangle == QueryResult::Hit);
        }
    }
}

template <typename T>
void PropertyRaySphere() {
    Random random(PropertySeed + 59 * sizeof(T));
    const double tolerance = IsSameV<T, float> ? 1.0e-3 : 1.0e-8;
    for (int iteration = 0; iteration < 500; ++iteration) {
        const Sphere<T> sphere{Narrow<T>(random.Point(5.0)), static_cast<T>(random.Range(0, 3))};
        const Vec3d origin = random.Point(15.0);
        const Vec3d target = Widen(sphere.center) + random.Point(4.0);
        auto ray = Ray<T>::TryMake(Narrow<T>(origin), Narrow<T>(target - origin));
        if (!ray) {
            continue;
        }

        // 계산 대상과 같은 입력(정밀도 축소 후)으로 고전 2차식을 double로 푼다.
        const Vec3d o = Widen(ray.Value().origin);
        const Vec3d d = Widen(ray.Value().direction);
        const Vec3d m = o - Widen(sphere.center);
        const double radius = sphere.radius;
        const double a = Dot(d, d);
        const double b = Dot(m, d);
        const double c = Dot(m, m) - radius * radius;
        const double discriminant = b * b - a * c;
        const RayHit<T> hit = Raycast(ray.Value(), sphere);
        const double scale = 1.0 + Dot(m, m);
        if (c < -tolerance * scale) {
            CHECK(hit.IsHit() && hit.startedInside && hit.distance == 0);
        } else if (c > tolerance * scale) {
            if (b >= 0.0 || discriminant < -tolerance * scale) {
                CHECK(hit.result == QueryResult::Miss);
            } else if (discriminant > tolerance * scale) {
                const double expected = (-b - math::Sqrt(discriminant)) / a;
                REQUIRE(hit.IsHit());
                CHECK(math::Abs(static_cast<double>(hit.distance) - expected) <=
                      tolerance * (1.0 + expected));
                CHECK(!hit.startedInside);
                if (radius > 0.5) {
                    CHECK(hit.normal.IsNormalized(static_cast<T>(1.0e-3)));
                }
            }
        }
    }
}

template <typename T>
void PropertyRayConvexShapes() {
    Random random(PropertySeed + 71 * sizeof(T));
    const double margin = IsSameV<T, float> ? 1.0e-3 : 1.0e-7;
    const double tolerance = IsSameV<T, float> ? 2.0e-3 : 1.0e-6;
    int compared = 0;
    // 판정 가능한 Hit 비교가 기준(200)을 여유 있게 넘도록 400회를 돈다.
    for (int iteration = 0; iteration < 400; ++iteration) {
        const Vec3d origin = random.Point(12.0);

        // AABB
        {
            const Vec3d corner = random.Point(4.0);
            const Box<T> box{Narrow<T>(corner),
                             Narrow<T>(corner + Vec3d{random.Range(0, 3), random.Range(0, 3),
                                                      random.Range(0, 3)})};
            const Vec3d center = (Widen(box.min) + Widen(box.max)) * 0.5;
            const Vec3d half = (Widen(box.max) - Widen(box.min)) * 0.5;
            const Vec3d target = center + random.Point(3.0);
            const Ray<T> ray = MakeRay(Narrow<T>(origin), Narrow<T>(target - origin));
            const Vec3d o = Widen(ray.origin);
            const Vec3d d = Widen(ray.direction);
            const OracleHit oracle = OracleRaycast(o, d, 100.0, margin, [&](const Vec3d& p) {
                return BoxSignedDistance(p - center, half);
            });
            const RayHit<T> hit = Raycast(ray, box);
            if (oracle.kind == OracleHit::Miss) {
                CHECK(hit.result == QueryResult::Miss);
            } else if (oracle.kind == OracleHit::Inside) {
                CHECK(hit.IsHit() && hit.startedInside);
            } else if (oracle.kind == OracleHit::Hit) {
                CHECK(hit.IsHit() && !hit.startedInside);
                CHECK(math::Abs(static_cast<double>(hit.distance) - oracle.distance) <=
                      tolerance * (1.0 + oracle.distance));
                CHECK(ContainsPoint(box, hit.point) == QueryResult::Hit);
                ++compared;
            }
        }

        // Capsule
        {
            const Vec3d start = random.Point(4.0);
            const Vec3d end = iteration % 7 == 0 ? start : random.Point(4.0);
            const Capsule<T> capsule(Narrow<T>(start), Narrow<T>(end),
                                     static_cast<T>(random.Range(0.05, 2)));
            const Vec3d s0 = Widen(capsule.start);
            const Vec3d s1 = Widen(capsule.end);
            const double radius = capsule.radius;
            const Vec3d target = (s0 + s1) * 0.5 + random.Point(3.0);
            const Ray<T> ray = MakeRay(Narrow<T>(origin), Narrow<T>(target - origin));
            const OracleHit oracle = OracleRaycast(
                Widen(ray.origin), Widen(ray.direction), 100.0, margin, [&](const Vec3d& p) {
                    const Vec3d axis = s1 - s0;
                    const double lengthSquared = Dot(axis, axis);
                    const double t =
                        lengthSquared > 0 ? ClampD(Dot(p - s0, axis) / lengthSquared, 0, 1) : 0;
                    return Distance(p, s0 + axis * t) - radius;
                });
            const RayHit<T> hit = Raycast(ray, capsule);
            if (oracle.kind == OracleHit::Miss) {
                CHECK(hit.result == QueryResult::Miss);
            } else if (oracle.kind == OracleHit::Inside) {
                CHECK(hit.IsHit() && hit.startedInside);
            } else if (oracle.kind == OracleHit::Hit) {
                CHECK(hit.IsHit() && !hit.startedInside);
                CHECK(math::Abs(static_cast<double>(hit.distance) - oracle.distance) <=
                      tolerance * (1.0 + oracle.distance));
                CHECK(hit.normal.IsNormalized(static_cast<T>(1.0e-3)));
                ++compared;
            }
        }

        // OrientedBox
        {
            const Quatd rotationD = random.Rotation();
            const OrientedBox<T> box{
                Narrow<T>(random.Point(4.0)), NarrowRotation<T>(rotationD),
                Narrow<T>(Vec3d{random.Range(0, 2), random.Range(0, 2), random.Range(0, 2)})};
            Vec3d axes[3];
            const Quatd wideRotation(box.rotation.x, box.rotation.y, box.rotation.z,
                                     box.rotation.w);
            OracleAxes(wideRotation, axes);
            const Vec3d center = Widen(box.center);
            const Vec3d half = Widen(box.halfExtents);
            const Vec3d target = center + random.Point(3.0);
            const Ray<T> ray = MakeRay(Narrow<T>(origin), Narrow<T>(target - origin));
            const OracleHit oracle = OracleRaycast(
                Widen(ray.origin), Widen(ray.direction), 100.0, margin, [&](const Vec3d& p) {
                    const Vec3d offset = p - center;
                    const Vec3d local(Dot(offset, axes[0]), Dot(offset, axes[1]),
                                      Dot(offset, axes[2]));
                    return BoxSignedDistance(local, half);
                });
            const RayHit<T> hit = Raycast(ray, box);
            if (oracle.kind == OracleHit::Miss) {
                CHECK(hit.result == QueryResult::Miss);
            } else if (oracle.kind == OracleHit::Inside) {
                CHECK(hit.IsHit() && hit.startedInside);
            } else if (oracle.kind == OracleHit::Hit) {
                CHECK(hit.IsHit() && !hit.startedInside);
                CHECK(math::Abs(static_cast<double>(hit.distance) - oracle.distance) <=
                      tolerance * (1.0 + oracle.distance));
                CHECK(hit.normal.IsNormalized(static_cast<T>(1.0e-3)));
                ++compared;
            }
        }
    }
    // 판정 가능한 Hit 사례가 충분히 비교되었는지 확인한다(oracle이 모두 Ambiguous로 빠지는 회귀
    // 방지).
    CHECK(compared > 200);
}

template <typename T>
void PropertyRayTriangle() {
    Random random(PropertySeed + 83 * sizeof(T));
    const double margin = IsSameV<T, float> ? 1.0e-3 : 1.0e-8;
    int hits = 0;
    int misses = 0;
    for (int iteration = 0; iteration < 600; ++iteration) {
        const Triangle<T> triangle{Narrow<T>(random.Point(4.0)), Narrow<T>(random.Point(4.0)),
                                   Narrow<T>(random.Point(4.0))};
        const Vec3d a = Widen(triangle.a);
        const Vec3d b = Widen(triangle.b);
        const Vec3d c = Widen(triangle.c);
        const double u = random.Range(-0.3, 1.0);
        const double v = random.Range(-0.3, 1.0);
        const Vec3d target = a + (b - a) * u + (c - a) * v;
        const Vec3d origin = random.Point(12.0);
        const Ray<T> ray = MakeRay(Narrow<T>(origin), Narrow<T>(target - origin));
        const Vec3d o = Widen(ray.origin);
        const Vec3d d = Widen(ray.direction);

        const Vec3d normal = Cross(b - a, c - a);
        const double normalLength = normal.Length();
        if (normalLength < 1.0e-2) {
            continue;
        }
        const double rate = Dot(normal, d) / normalLength;
        if (math::Abs(rate) < 0.05) {
            continue;
        }

        // 평면 교차 후 부호 있는 넓이 비율로 무게중심 좌표를 구한다.
        const double t = Dot(normal, a - o) / Dot(normal, d);
        const Vec3d p = o + d * t;
        const double area = Dot(normal, normal);
        const double wa = Dot(Cross(b - p, c - p), normal) / area;
        const double wb = Dot(Cross(c - p, a - p), normal) / area;
        const double wc = Dot(Cross(a - p, b - p), normal) / area;
        const double smallest = MinD(wa, MinD(wb, wc));

        const RayHit<T> hit = Raycast(ray, triangle);
        const RayHit<T> front = Raycast(ray, triangle, Infinity<T>(), TriangleSides::FrontOnly);
        if (t < -margin || smallest < -margin) {
            CHECK(hit.result == QueryResult::Miss);
            ++misses;
        } else if (t > margin && smallest > margin) {
            REQUIRE(hit.IsHit());
            CHECK(math::Abs(static_cast<double>(hit.distance) - t) <= margin * (1.0 + t));
            CHECK(Dot(Widen(hit.normal), d) < 0.0);
            CHECK((front.IsHit()) == (Dot(normal, d) < 0.0));
            ++hits;
        }
    }
    CHECK(hits > 50 && misses > 50);
}

template <typename T>
void PropertyOrientedBoxOverlap() {
    Random random(PropertySeed + 97 * sizeof(T));
    const double margin = IsSameV<T, float> ? 1.0e-3 : 1.0e-7;
    int separated = 0;
    int overlapping = 0;
    for (int iteration = 0; iteration < 600; ++iteration) {
        const Quatd rotationA = random.Rotation();
        Quatd rotationB = random.Rotation();
        const int kind = iteration % 4;
        if (kind == 1) {
            rotationB = rotationA;
        } else if (kind == 2) {
            // A를 local 축 하나에 대해 90도 돌린 회전(모서리가 서로 평행).
            const double half = 0.70710678118654752440;
            const Quatd quarter = iteration % 3 == 0   ? Quatd(half, 0, 0, half)
                                  : iteration % 3 == 1 ? Quatd(0, half, 0, half)
                                                       : Quatd(0, 0, half, half);
            rotationB = rotationA * quarter;
        }

        const OrientedBox<T> a{
            Narrow<T>(random.Point(2.0)), NarrowRotation<T>(rotationA),
            Narrow<T>(Vec3d{random.Range(0.1, 2), random.Range(0.1, 2), random.Range(0.1, 2)})};
        const OrientedBox<T> b{
            Narrow<T>(random.Point(2.5)), NarrowRotation<T>(rotationB),
            Narrow<T>(Vec3d{random.Range(0, 2), random.Range(0, 2), random.Range(0, 2)})};
        const Quatd wideA(a.rotation.x, a.rotation.y, a.rotation.z, a.rotation.w);
        const Quatd wideB(b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w);
        const double gap = OracleObbGap(Widen(a.center), wideA, Widen(a.halfExtents),
                                        Widen(b.center), wideB, Widen(b.halfExtents));

        const QueryResult result = Overlap(a, b);
        CHECK(result == Overlap(b, a));
        if (gap > margin) {
            CHECK(result == QueryResult::Miss);
            ++separated;
        } else if (gap < -margin) {
            CHECK(result == QueryResult::Hit);
            ++overlapping;
        }
    }
    CHECK(separated > 100 && overlapping > 100);
}

// end가 닫힌 공 안이고 start가 밖이면 반올림과 무관하게 t <= 1 Hit여야 한다.
template <typename T>
void PropertySegmentSphereEndInside() {
    Random random(PropertySeed + 101 * sizeof(T));
    int checked = 0;
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const Sphere<T> sphere{Narrow<T>(random.Point(3.0)), static_cast<T>(random.Range(0.1, 3))};
        const Vec3d direction = random.UnitVector();
        const Vector3<T> end =
            Narrow<T>(Widen(sphere.center) + direction * (sphere.radius * random.Range(0.999, 1)));
        const Vector3<T> start = Narrow<T>(random.Point(10.0));
        const T radiusSquared = sphere.radius * sphere.radius;
        if (DistanceSquared(end, sphere.center) > radiusSquared ||
            DistanceSquared(start, sphere.center) <= radiusSquared) {
            continue;
        }

        const SegmentHit<T> hit = SegmentCast(Segment<T>(start, end), sphere);
        CHECK(hit.IsHit() && hit.t >= 0 && hit.t <= 1 && !hit.startedInside);
        ++checked;
    }
    CHECK(checked > 500);
}

} // namespace

TEST(IntersectionRayPlane) {
    CheckRayPlane<float>();
    CheckRayPlane<double>();
}

TEST(IntersectionRaySphere) {
    CheckRaySphere<float>();
    CheckRaySphere<double>();
}

TEST(IntersectionRayBox) {
    CheckRayBox<float>();
    CheckRayBox<double>();
}

TEST(IntersectionRayTriangle) {
    CheckRayTriangle<float>();
    CheckRayTriangle<double>();
}

TEST(IntersectionRayCapsule) {
    CheckRayCapsule<float>();
    CheckRayCapsule<double>();
}

TEST(IntersectionRayOrientedBox) {
    CheckRayOrientedBox<float>();
    CheckRayOrientedBox<double>();
}

TEST(IntersectionSegmentPlane) {
    CheckSegmentPlane<float>();
    CheckSegmentPlane<double>();
}

TEST(IntersectionSegmentSphere) {
    CheckSegmentSphere<float>();
    CheckSegmentSphere<double>();
}

TEST(IntersectionSegmentBox) {
    CheckSegmentBox<float>();
    CheckSegmentBox<double>();
}

TEST(IntersectionSegmentTriangle) {
    CheckSegmentTriangle<float>();
    CheckSegmentTriangle<double>();
}

TEST(IntersectionContainsPoint) {
    CheckContainsPoint<float>();
    CheckContainsPoint<double>();
}

TEST(IntersectionSphereAndBoxOverlaps) {
    CheckSphereAndBoxOverlaps<float>();
    CheckSphereAndBoxOverlaps<double>();
}

TEST(IntersectionPlaneClassification) {
    CheckPlaneClassification<float>();
    CheckPlaneClassification<double>();
}

TEST(IntersectionCapsuleOverlaps) {
    CheckCapsuleOverlaps<float>();
    CheckCapsuleOverlaps<double>();
}

TEST(IntersectionSphereTriangleOverlap) {
    CheckSphereTriangleOverlap<float>();
    CheckSphereTriangleOverlap<double>();
}

TEST(IntersectionOrientedBoxOverlap) {
    CheckOrientedBoxOverlap<float>();
    CheckOrientedBoxOverlap<double>();
}

TEST(IntersectionClosestPoints) {
    CheckClosestPoints<float>();
    CheckClosestPoints<double>();
}

TEST(IntersectionSegmentSegment) {
    CheckSegmentSegment<float>();
    CheckSegmentSegment<double>();
}

TEST(IntersectionLargeCoordinates) {
    CheckLargeCoordinates();
}

TEST(IntersectionPropertyClosestPointOnBox) {
    PropertyClosestPointOnBox<float>();
    PropertyClosestPointOnBox<double>();
}

TEST(IntersectionPropertyClosestPointOnTriangle) {
    PropertyClosestPointOnTriangle<float>();
    PropertyClosestPointOnTriangle<double>();
}

TEST(IntersectionPropertySegmentSegmentAndCapsule) {
    PropertySegmentSegment<float>();
    PropertySegmentSegment<double>();
}

TEST(IntersectionPropertySphereOverlapsAndSymmetry) {
    PropertySphereOverlaps<float>();
    PropertySphereOverlaps<double>();
}

TEST(IntersectionPropertyRaySphere) {
    PropertyRaySphere<float>();
    PropertyRaySphere<double>();
}

TEST(IntersectionPropertyRayConvexShapes) {
    PropertyRayConvexShapes<float>();
    PropertyRayConvexShapes<double>();
}

TEST(IntersectionPropertyRayTriangle) {
    PropertyRayTriangle<float>();
    PropertyRayTriangle<double>();
}

TEST(IntersectionPropertyOrientedBoxOverlap) {
    PropertyOrientedBoxOverlap<float>();
    PropertyOrientedBoxOverlap<double>();
}

TEST(IntersectionPropertySegmentSphereEndInside) {
    PropertySegmentSphereEndInside<float>();
    PropertySegmentSphereEndInside<double>();
}

// ---- 리뷰 회귀(2026-11-11) ----
namespace {

// IsValid가 허용하는 길이 오차(|q|² = 1 + 1.8e-5) 안의 비단위 회전도 단위 회전과 같은 box로
// 취급해야 한다. 180도 Z 회전은 정규화하면 정확히 (0,0,1,0)이므로 경계 판정이 정확하다.
template <typename T>
void CheckOrientedBoxNonUnitRotation() {
    const T one = static_cast<T>(1);
    const Quaternion<T> rotation(static_cast<T>(0), static_cast<T>(0), one + static_cast<T>(9.0e-6),
                                 static_cast<T>(0));
    const OrientedBox<T> box(Vector3<T>::Zero(), rotation, Vector3<T>::Splat(static_cast<T>(100)));
    REQUIRE(box.IsValid());
    CHECK(box.UnitRotation() == Quaternion<T>(0, 0, 1, 0));
    CHECK(box.AxisX() == Vector3<T>(-1, 0, 0));

    // 면 위의 점(world x = -100)은 포함이다. 정규화 전에는 local x가 100.0036으로 늘어나 Miss였다.
    CHECK(ContainsPoint(box, Vector3<T>(-100, 0, 0)) == QueryResult::Hit);
    CHECK(ContainsPoint(box, Vector3<T>(-100, 0, 0) - Vector3<T>(static_cast<T>(0.01), 0, 0)) ==
          QueryResult::Miss);

    const RayHit<T> hit = Raycast(Ray<T>(Vector3<T>(-300, 0, 0), Vector3<T>(1, 0, 0)), box);
    REQUIRE(hit.IsHit());
    CHECK(hit.distance == static_cast<T>(200));
    CHECK(hit.point == Vector3<T>(-100, 0, 0));
    CHECK(hit.normal == Vector3<T>(-1, 0, 0));

    // x = 100에서 면 접촉하는 identity box와 겹침이다.
    const OrientedBox<T> touching(Vector3<T>(150, 0, 0), Quaternion<T>(),
                                  Vector3<T>::Splat(static_cast<T>(50)));
    CHECK(Overlap(box, touching) == QueryResult::Hit);
    CHECK(Overlap(touching, box) == QueryResult::Hit);
    const OrientedBox<T> apart(Vector3<T>(static_cast<T>(150.5), 0, 0), Quaternion<T>(),
                               Vector3<T>::Splat(static_cast<T>(50)));
    CHECK(Overlap(box, apart) == QueryResult::Miss);

    auto closest = TryClosestPoint(box, Vector3<T>(-150, 0, 0));
    REQUIRE(closest);
    CHECK(closest.Value() == Vector3<T>(-100, 0, 0));
    auto distanceSquared = TryDistanceSquared(box, Vector3<T>(-150, 0, 0));
    REQUIRE(distanceSquared);
    CHECK(distanceSquared.Value() == static_cast<T>(2500));
}

} // namespace

TEST(IntersectionReviewOrientedBoxNonUnitRotation) {
    CheckOrientedBoxNonUnitRotation<float>();
    CheckOrientedBoxNonUnitRotation<double>();
}

TEST(IntersectionReviewCapsuleStartOnSideSurface) {
    // 리뷰 재현 입력: origin이 옆면 위(선분 거리 판정은 밖, 수직 성분 판정은 안)였다.
    const Capsule<double> capsule(
        Vec3d(1.8938331700276845, -0.99557382955938856, -1.4237027711157406),
        Vec3d(-5.4929955063443963, 0.88585323315807618, 0.085849490307394527), 1.2556979115178017);
    const Ray<double> ray(Vec3d(-2.1603267803619088, -1.2141742229034214, -0.98928748735074445),
                          Vec3d(0.28728694130541987, 0.9232426956109735, 0.25512573048667503));
    REQUIRE(ray.IsValid());
    REQUIRE(capsule.IsValid());
    const RayHit<double> hit = Raycast(ray, capsule);
    REQUIRE(hit.IsHit());
    CHECK(hit.distance == 0.0);
    CHECK(hit.startedInside);

    // 축 방향으로 정확히 표현되는 옆면 시작점: 바깥으로 나가는 방향도 거리 0 Hit다.
    const Capsule<double> axisAligned(Vec3d(0.0, 0.0, -2.0), Vec3d(0.0, 0.0, 2.0), 1.0);
    const RayHit<double> outward =
        Raycast(Ray<double>(Vec3d(1.0, 0.0, 0.5), Vec3d(1.0, 0.0, 0.0)), axisAligned);
    REQUIRE(outward.IsHit());
    CHECK(outward.distance == 0.0);
    // 축 범위 밖의 무한 원기둥 안에서 출발하면 끝 구가 첫 진입이다.
    const RayHit<double> beyond =
        Raycast(Ray<double>(Vec3d(0.5, 0.0, 5.0), Vec3d(0.0, 0.0, -1.0)), axisAligned);
    REQUIRE(beyond.IsHit());
    CHECK(!beyond.startedInside);
    CHECK(math::Abs(beyond.distance - (5.0 - 2.0 - math::Sqrt(0.75))) <= 1.0e-12);
}

TEST(IntersectionReviewSegmentEndOnTriangle) {
    // 리뷰 재현 입력(float): end가 z = c 평면의 삼각형 내부에 정확히 있지만 계산된 t가 1을 넘었다.
    const float c = -0x1.e03f9ep-1f;
    const Triangle<float> triangle(Vec3(-0x1.c24e62p+1f, -0x1.4d920cp+0f, c),
                                   Vec3(0x1.e27fcap+1f, -0x1.edd632p+0f, c),
                                   Vec3(-0x1.c6ffe0p-2f, 0x1.855cfap+0f, c));
    const Vec3 start(-0x1.de849cp+1f, 0x1.045576p+1f, 0x1.22cce8p+2f);
    const Vec3 end(0x1.502454p-1f, -0x1.8f859cp-1f, c);
    const SegmentHit<float> hit = SegmentCast(Segment<float>(start, end), triangle);
    REQUIRE(hit.IsHit());
    CHECK(hit.t <= 1.0f);
    CHECK(hit.normal.z > 0.0f); // start는 z > c(앞면 normal +Z 쪽)에 있다.
    // FrontOnly: 앞면 쪽에서 오므로 Hit, 반대 순서(뒤에서 시작하는 경우)는 아래에서 확인한다.
    CHECK(SegmentCast(Segment<float>(start, end), triangle, TriangleSides::FrontOnly).IsHit());
    const Vec3 below(start.x, start.y, c - 3.0f);
    const SegmentHit<float> back = SegmentCast(Segment<float>(below, end), triangle);
    if (!back.IsHit()) {
        CHECK(back.IsHit());
    } else {
        CHECK(back.normal.z < 0.0f);
    }
    CHECK(!SegmentCast(Segment<float>(below, end), triangle, TriangleSides::FrontOnly).IsHit());
    // 반대 방향(시작이 삼각형 위)은 t = 0 Hit다.
    const SegmentHit<float> reversed = SegmentCast(Segment<float>(end, start), triangle);
    REQUIRE(reversed.IsHit());
    CHECK(reversed.t == 0.0f && !platform::PlatformMath::SignBit(reversed.t));
    // 공면 segment는 Raycast와 같이 Miss다.
    const Vec3 coplanarStart(start.x, start.y, c);
    CHECK(!SegmentCast(Segment<float>(coplanarStart, end), triangle).IsHit());
}

TEST(IntersectionReviewRayPlaneOverflowIsMiss) {
    // 거의 평행한 ray는 거리 계산이 overflow한다. Inf 거리·NaN 점을 Hit로 내지 않는다.
    const RayHit<float> overflow =
        Raycast(Ray<float>(Vec3(0.0f, 0.0f, 1.0e9f), Vec3(1.0f, 0.0f, -1.0e-30f)), Plane<float>());
    CHECK(overflow.result == QueryResult::Miss);
    const RayHit<float> subnormal =
        Raycast(Ray<float>(Vec3(0.0f, 0.0f, 1.0f), Vec3(1.0f, 0.0f, -1.0e-40f)), Plane<float>());
    CHECK(subnormal.result == QueryResult::Miss);
    const RayHit<double> subnormalDouble =
        Raycast(Ray<double>(Vec3d(0.0, 0.0, 1.0), Vec3d(1.0, 0.0, -1.0e-310)), Plane<double>());
    CHECK(subnormalDouble.result == QueryResult::Miss);
    // 유한한 거리는 그대로 Hit다.
    const RayHit<float> shallow =
        Raycast(Ray<float>(Vec3(0.0f, 0.0f, 1.0f), Vec3(1.0f, 0.0f, -1.0e-3f)), Plane<float>());
    REQUIRE(shallow.IsHit());
    CHECK(math::IsFinite(shallow.distance) && shallow.point.IsFinite());
}

TEST(IntersectionReviewBoxExpandKeepsNaN) {
    const float nan = __builtin_nanf("");
    Box<float> box = Box<float>::Empty().ExpandedToInclude(Vec3(nan, 0.0f, 0.0f));
    CHECK(!box.IsValid());
    box = box.ExpandedToInclude(Vec3(1.0f, 1.0f, 1.0f));
    CHECK(!box.IsValid()); // 뒤이은 확장에서도 NaN이 사라지지 않는다.
    CHECK(math::IsNaN(box.min.x) && math::IsNaN(box.max.x));
    const Box<double> valid = Box<double>::Empty()
                                  .ExpandedToInclude(Vec3d(1.0, -2.0, 3.0))
                                  .ExpandedToInclude(Vec3d::Zero());
    REQUIRE(valid.IsValid());
    CHECK(valid.min == Vec3d(0.0, -2.0, 0.0) && valid.max == Vec3d(1.0, 0.0, 3.0));
}

TEST(IntersectionRereviewTriangleStartContact) {
    // 재검토 재현 입력 1(float): 회전된 삼각형 위의 공면 segment. 수정 전에는 t = 1 Hit였다.
    const Triangle<float> rotated(Vec3(0x1.f4b976p-1f, -0x1.2c7d54p-1f, 0x1.1041a8p-4f),
                                  Vec3(0x1.8450e6p-1f, 0x1.19a924p-2f, 0x1.d64ae2p-4f),
                                  Vec3(-0x1.749292p-2f, -0x1.1e5e02p-1f, 0x1.dd9ba6p-2f));
    const Vec3 onA(0x1.66850ep-1f, 0x1.a4a22cp-3f, 0x1.11904cp-3f);
    const Vec3 onB(-0x1.ea15ep-3f, -0x1.fd4ec4p-2f, 0x1.b6479cp-2f);
    REQUIRE(detail::PointOnTriangle(rotated, onA) && detail::PointOnTriangle(rotated, onB));
    // 시작점이 삼각형 위면 방향과 무관하게 t = 0 접촉이다(0 길이 segment와 같은 규칙).
    const SegmentHit<float> forward = SegmentCast(Segment<float>(onA, onB), rotated);
    REQUIRE(forward.IsHit());
    CHECK(forward.t == 0.0f && forward.point == onA);
    const SegmentHit<float> backward = SegmentCast(Segment<float>(onB, onA), rotated);
    REQUIRE(backward.IsHit());
    CHECK(backward.t == 0.0f && backward.point == onB);

    // 재검토 재현 입력 4(float): 시작점이 회전된 삼각형 위인데 scaledT가 음수로 반올림되어 놓쳤다.
    const Triangle<float> small(Vec3(-0x1.eb09bep-4f, -0x1.db5ap-14f, 0x1.b50922p-4f),
                                Vec3(0x1.110528p-6f, -0x1.d4c564p-8f, 0x1.8bc744p-6f),
                                Vec3(-0x1.76d1c2p-5f, -0x1.3a6e08p-4f, 0x1.5513bap-4f));
    const Vec3 start(-0x1.6c046cp-5f, -0x1.01b4e6p-4f, 0x1.41139ap-4f);
    const Vec3 end(-0x1.8a5c6p-6f, -0x1.9acbd4p-5f, 0x1.124bc6p-4f);
    REQUIRE(detail::PointOnTriangle(small, start));
    const SegmentHit<float> fromStart = SegmentCast(Segment<float>(start, end), small);
    REQUIRE(fromStart.IsHit());
    CHECK(fromStart.t == 0.0f && fromStart.point == start);
    CHECK(SegmentCast(Segment<float>(end, start), small).IsHit());
    // Raycast도 같은 시작 접촉 규칙이다.
    auto direction = (end - start).TryNormalize();
    REQUIRE(direction);
    const RayHit<float> ray = Raycast(Ray<float>(start, direction.Value()), small);
    REQUIRE(ray.IsHit());
    CHECK(ray.distance == 0.0f && !ray.startedInside);
    const RayHit<float> away = Raycast(Ray<float>(start, -direction.Value()), small);
    REQUIRE(away.IsHit());
    CHECK(away.distance == 0.0f);
    // 시작 접촉의 normal은 진행 방향의 반대쪽 면이다.
    CHECK(Dot(ray.normal, direction.Value()) <= 0.0f);
    CHECK(Dot(away.normal, -direction.Value()) <= 0.0f);

    // 축 정렬 삼각형: 평면 위 시작점이 삼각형 밖이면 공면 ray는 여전히 Miss다.
    const Triangle<double> flat(Vec3d(0.0, 0.0, 0.0), Vec3d(1.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0));
    CHECK(Raycast(Ray<double>(Vec3d(-1.0, 0.25, 0.0), Vec3d::Forward()), flat).result ==
          QueryResult::Miss);
    // 삼각형 위에서 공면으로 출발하면 거리 0 Hit, normal은 앞면이다.
    const RayHit<double> coplanar =
        Raycast(Ray<double>(Vec3d(0.25, 0.25, 0.0), Vec3d::Forward()), flat);
    REQUIRE(coplanar.IsHit());
    CHECK(coplanar.distance == 0.0 && coplanar.normal == Vec3d::Up());
    // FrontOnly는 앞면 진입일 때만 시작 접촉을 Hit로 본다.
    CHECK(Raycast(Ray<double>(Vec3d(0.25, 0.25, 0.0), Vec3d::Down()), flat,
                  detail::GeometryInfinity<double>(), TriangleSides::FrontOnly)
              .IsHit());
    CHECK(!Raycast(Ray<double>(Vec3d(0.25, 0.25, 0.0), Vec3d::Up()), flat,
                   detail::GeometryInfinity<double>(), TriangleSides::FrontOnly)
               .IsHit());
}
