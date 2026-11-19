#include <Core/Math/Intersection.h>
#include <Core/Math/RandomStream.h>
#include <TestSupport/ExactArithmetic.h>
#include <TestSupport/GeometryOracles.h>
#include <TestSupport/Test.h>
#include <Platform/PlatformMath.h>

using namespace rocks;

// Cylinder 형상(CY1)과 Capsule 평면·볼록 영역 판정 보강 시험이다(Docs/Design/CylinderShape.ko.md
// 3.6절). 경계 포함은 표본 점이 아니라 정확 산술(정수) oracle로 판정한다.

namespace {

using test::BigAdd;
using test::BigCompare;
using test::BigFromDouble;
using test::BigMultiply;
using test::BigSigned;
using test::BigSignedSubtract;
using test::BigSquare;
using test::BigUnsigned;
using test::OracleHit;

// ---- 공용 도우미 ----
template <typename T>
T Infinity() noexcept {
    if constexpr (IsSameV<T, float>) {
        return platform::PlatformMath::Infinity<float>();
    } else {
        return platform::PlatformMath::Infinity<double>();
    }
}

template <typename T>
T QuietNaN() noexcept {
    if constexpr (IsSameV<T, float>) {
        return platform::PlatformMath::QuietNaN<float>();
    } else {
        return platform::PlatformMath::QuietNaN<double>();
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
bool IsPositiveZero(T value) noexcept {
    return value == static_cast<T>(0) && math::CopySign(static_cast<T>(1), value) > 0;
}

template <typename T>
bool IsZeroVector(const Vector3<T>& vector) noexcept {
    return vector == Vector3<T>::Zero();
}

template <typename T>
Vec3d Widen(const Vector3<T>& value) noexcept {
    return Vec3d(static_cast<double>(value.x), static_cast<double>(value.y),
                 static_cast<double>(value.z));
}

// 정밀도별 비교 허용 오차(좌표 규모 약 10 기준)다.
template <typename T>
double Loose() noexcept {
    return IsSameV<T, float> ? 2.0e-3 : 1.0e-9;
}

double MaxD(double a, double b) noexcept {
    return a > b ? a : b;
}

double MinD(double a, double b) noexcept {
    return a < b ? a : b;
}

// ---- 결정적 난수(Core RandomStream, 고정 seed) ----
// 시험용 편의 함수만 더한다. 여러 호출은 지역 변수로 순서를 고정한다.
struct Random {
    RandomStream stream;

    explicit Random(uint64 seed) noexcept : stream(seed) {
    }

    // [0, bound) 균등 정수다.
    uint32 Below(uint32 bound) noexcept {
        return stream.NextUInt32Below(bound);
    }

    // [0, 1)
    double Unit() noexcept {
        return stream.NextDouble01();
    }

    double Range(double low, double high) noexcept {
        return low + (high - low) * Unit();
    }

    Vec3d Point(double extent) noexcept {
        const double x = Range(-extent, extent);
        const double y = Range(-extent, extent);
        const double z = Range(-extent, extent);
        return Vec3d{x, y, z};
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

    // [-limit, limit] 정수를 2^-fractionBits 단위로 만든 dyadic 값이다.
    double Dyadic(uint32 limit, int fractionBits) noexcept {
        const int64 integer = static_cast<int64>(Below(2 * limit + 1)) - static_cast<int64>(limit);
        double scale = 1.0;
        for (int bit = 0; bit < fractionBits; ++bit) {
            scale *= 0.5;
        }
        return static_cast<double>(integer) * scale;
    }
};

constexpr uint64 CylinderSeed = 0xC71D0E5EEDull;

// ---- 독립 double oracle(검사 대상과 다른 식: 외적으로 수직 거리, 정규화하지 않은 내적) ----
struct OracleCylinder {
    Vec3d start;
    Vec3d end;
    double radius;
};

template <typename T>
OracleCylinder ToOracle(const Cylinder<T>& cylinder) noexcept {
    return OracleCylinder{Widen(cylinder.start), Widen(cylinder.end),
                          static_cast<double>(cylinder.radius)};
}

// 축 방향 초과(끝면 밖이면 양수)와 수직 초과(반지름 밖이면 양수)다.
struct OracleExcess {
    double axial;
    double radial;
};

OracleExcess OracleCylinderExcess(const OracleCylinder& cylinder, const Vec3d& point) noexcept {
    const Vec3d axis = cylinder.end - cylinder.start;
    const double length = axis.Length();
    const Vec3d offset = point - cylinder.start;
    if (length == 0.0) {
        return OracleExcess{0.0, offset.Length() - cylinder.radius};
    }
    const double along = Dot(offset, axis) / length;
    const double radial = Cross(offset, axis).Length() / length;
    return OracleExcess{MaxD(-along, along - length), radial - cylinder.radius};
}

// 볼록 집합의 부호 거리(밖 양수)다. 축 방향 slab과 무한 원기둥의 교집합 SDF다.
double OracleCylinderSdf(const OracleCylinder& cylinder, const Vec3d& point) noexcept {
    const OracleExcess excess = OracleCylinderExcess(cylinder, point);
    const double axial = MaxD(excess.axial, 0.0);
    const double radial = MaxD(excess.radial, 0.0);
    const double outside = math::Sqrt(axial * axial + radial * radial);
    return outside + MinD(MaxD(excess.axial, excess.radial), 0.0);
}

// normal 방향 선형 함수 n·x - distance의 cylinder 위 최솟값·최댓값이다. 중심과 √ 꼴 반폭으로
// 계산한다(검사 대상은 끝점 거리와 외적).
struct OracleRange {
    double lowest;
    double highest;
};

OracleRange OraclePlaneRange(const OracleCylinder& cylinder, const Vec3d& normal, double distance,
                             bool capsule) noexcept {
    const Vec3d center = (cylinder.start + cylinder.end) * 0.5;
    const Vec3d axis = cylinder.end - cylinder.start;
    const double length = axis.Length();
    const double normalLengthSquared = normal.LengthSquared();
    double along = 0.0;
    double disc = cylinder.radius * math::Sqrt(normalLengthSquared);
    if (length > 0.0) {
        along = Dot(normal, axis) / length;
        if (!capsule) {
            disc = cylinder.radius * math::Sqrt(MaxD(normalLengthSquared - along * along, 0.0));
        }
    }
    const double support = 0.5 * length * math::Abs(along) + disc;
    const double middle = Dot(normal, center) - distance;
    return OracleRange{middle - support, middle + support};
}

OracleHit OracleCylinderRaycast(const OracleCylinder& cylinder, const Vec3d& origin,
                                const Vec3d& direction, double maxDistance,
                                double margin) noexcept {
    return test::OracleRaycast(origin, direction, maxDistance, margin, [&](const Vec3d& point) {
        return OracleCylinderSdf(cylinder, point);
    });
}

// 무작위 cylinder다. 일부는 좌표축과 평행한 축, 거의 평행한 축(1e-7 기울기), radius 0이다.
OracleCylinder RandomCylinder(Random& random, bool allowSegment) noexcept {
    const Vec3d start = random.Point(6.0);
    const uint32 shape = random.Below(8);
    Vec3d axis = random.UnitVector();
    if (shape == 0) {
        axis = Vec3d(0, 0, 1);
    } else if (shape == 1) {
        axis = Vec3d(-1, 0, 0);
    } else if (shape == 3) {
        const Vec3d tilt = random.UnitVector() * 1.0e-7;
        axis = Vec3d(0, 1, 0) + tilt;
        axis = axis / axis.Length();
    }
    const double length = random.Range(0.1, 8.0);
    double radius = random.Range(0.05, 3.0);
    if (allowSegment && shape == 2) {
        radius = 0.0;
    }
    return OracleCylinder{start, start + axis * length, radius};
}

template <typename T>
Cylinder<T> Narrow(const OracleCylinder& cylinder) noexcept {
    return Cylinder<T>(Vector3<T>(cylinder.start), Vector3<T>(cylinder.end),
                       static_cast<T>(cylinder.radius));
}

// ---- 정확 산술(경계 포함 oracle, TestSupport/ExactArithmetic.h) ----
// 정확 산술 판정 결과다. overflow는 시험 도우미의 용량 부족이다(실패로 센다).
struct ExactBoundsResult {
    bool contains = true;
    bool overflow = false;
};

// bounds가 cylinder {start, end, radius} 전체를 덮는지 정확히 판정한다. 축 i의 실제 반폭은
// e_i = r·√(D_j² + D_k²)/|D|(D = end - start, 정확)이고, 경계 간격 g = min(start_i, end_i) - low_i
// (또는 high_i - max)가 g >= 0이며 g²·|D|² >= r²·(D_j² + D_k²)이면 그 쪽이 덮인다.
ExactBoundsResult ExactBoundsContainCylinder(const Vec3d& start, const Vec3d& end, double radius,
                                             const Vec3d& low, const Vec3d& high) noexcept {
    const double values[13] = {start.x, start.y, start.z, end.x,  end.y,  end.z, radius,
                               low.x,   low.y,   low.z,   high.x, high.y, high.z};
    const int scale = test::DyadicScale(values);

    const double starts[3] = {start.x, start.y, start.z};
    const double ends[3] = {end.x, end.y, end.z};
    const double lows[3] = {low.x, low.y, low.z};
    const double highs[3] = {high.x, high.y, high.z};
    BigUnsigned squares[3];
    for (int axis = 0; axis < 3; ++axis) {
        const BigSigned difference =
            BigSignedSubtract(BigFromDouble(ends[axis], scale), BigFromDouble(starts[axis], scale));
        squares[axis] = BigSquare(difference);
    }
    const BigUnsigned lengthSquared = BigAdd(BigAdd(squares[0], squares[1]), squares[2]);
    const BigUnsigned radiusSquared = BigSquare(BigFromDouble(radius, scale));

    ExactBoundsResult result;
    for (int axis = 0; axis < 3; ++axis) {
        const BigUnsigned across = BigAdd(squares[(axis + 1) % 3], squares[(axis + 2) % 3]);
        const BigUnsigned required = BigMultiply(radiusSquared, across);
        const double lowest = MinD(starts[axis], ends[axis]);
        const double highest = MaxD(starts[axis], ends[axis]);
        const BigSigned gaps[2] = {
            BigSignedSubtract(BigFromDouble(lowest, scale), BigFromDouble(lows[axis], scale)),
            BigSignedSubtract(BigFromDouble(highs[axis], scale), BigFromDouble(highest, scale))};
        for (const BigSigned& gap : gaps) {
            if (gap.negative) {
                result.contains = false;
                continue;
            }
            const BigUnsigned covered = BigMultiply(BigSquare(gap), lengthSquared);
            if (covered.overflow || required.overflow) {
                result.overflow = true;
                continue;
            }
            if (BigCompare(covered, required) < 0) {
                result.contains = false;
            }
        }
    }
    return result;
}

// T 정밀도에서 value 크기의 ulp(double로)다.
template <typename T>
double UlpOf(double value) noexcept {
    const T magnitude = static_cast<T>(math::Abs(value));
    return static_cast<double>(math::NextUp(magnitude)) - static_cast<double>(magnitude);
}

// 경계가 정확히 포함하고(정확 산술) 꽉 맞는 경계보다 상대 32ε와 몇 ulp 이상 넓지 않은지 본다.
template <typename T>
void CheckBoundsAgainstOracle(const Cylinder<T>& cylinder) {
    const Result<Box<T>> bounds = TryBounds(cylinder);
    REQUIRE(bounds);
    const Vec3d start = Widen(cylinder.start);
    const Vec3d end = Widen(cylinder.end);
    const double radius = static_cast<double>(cylinder.radius);
    const Vec3d low = Widen(bounds.Value().min);
    const Vec3d high = Widen(bounds.Value().max);
    const ExactBoundsResult exact = ExactBoundsContainCylinder(start, end, radius, low, high);
    CHECK(!exact.overflow);
    CHECK(exact.contains);

    const Vec3d delta = end - start;
    const double scaleLength =
        MaxD(MaxD(math::Abs(delta.x), math::Abs(delta.y)), math::Abs(delta.z));
    const double slack = 32.0 * static_cast<double>(math::MachineEpsilon<T>);
    // 축 비율 하한(정규 값의 2배)이 더하는 절대 여유다. 축 비율이 비정규로 작을 때만 보인다.
    const double floorSlack = radius * 4.0 * static_cast<double>(NumericLimits<T>::Min());
    const double starts[3] = {start.x, start.y, start.z};
    const double ends[3] = {end.x, end.y, end.z};
    const double lows[3] = {low.x, low.y, low.z};
    const double highs[3] = {high.x, high.y, high.z};
    for (int axis = 0; axis < 3; ++axis) {
        double half = 0.0;
        if (radius > 0.0 && scaleLength > 0.0) {
            const Vec3d scaled = delta / scaleLength;
            const double comps[3] = {scaled.x, scaled.y, scaled.z};
            const double across0 = comps[(axis + 1) % 3];
            const double across1 = comps[(axis + 2) % 3];
            half = radius * math::Sqrt(across0 * across0 + across1 * across1) / scaled.Length();
        }
        const double lowest = MinD(starts[axis], ends[axis]);
        const double highest = MaxD(starts[axis], ends[axis]);
        const double lowAllowance = half * (1.0 + slack) +
                                    4.0 * UlpOf<T>(MaxD(math::Abs(lowest), math::Abs(lows[axis]))) +
                                    4.0 * UlpOf<T>(half) + floorSlack;
        const double highAllowance =
            half * (1.0 + slack) +
            4.0 * UlpOf<T>(MaxD(math::Abs(highest), math::Abs(highs[axis]))) +
            4.0 * UlpOf<T>(half) + floorSlack;
        CHECK(lowest - lows[axis] <= lowAllowance);
        CHECK(highs[axis] - highest <= highAllowance);
    }
}

// =================================================================================================
// 형상
// =================================================================================================

template <typename T>
void CheckCylinderConstruction() {
    const Cylinder<T> defaultCylinder;
    CHECK(defaultCylinder.IsValid());
    CHECK(defaultCylinder.radius == 0);

    auto made = Cylinder<T>::TryMake(V<T>(0, 0, 0), V<T>(0, 0, 4), S<T>(1));
    REQUIRE(made);
    CHECK(made.Value().IsValid());
    CHECK(made.Value().Center() == V<T>(0, 0, 2));

    // -0 반경은 +0으로 정리하고 선분·점(radius 0)은 허용한다.
    auto negativeZero = Cylinder<T>::TryMake(V<T>(1, 1, 1), V<T>(1, 1, 2), S<T>(-0.0));
    REQUIRE(negativeZero);
    CHECK(IsPositiveZero(negativeZero.Value().radius));
    CHECK(Cylinder<T>(V<T>(1, 1, 1), V<T>(1, 1, 2), S<T>(-0.0)).IsValid());
    CHECK(Cylinder<T>::TryMake(V<T>(3, 3, 3), V<T>(3, 3, 3), S<T>(0)));

    // 입력 오류: 비유한·음수 반경, 비유한 끝점, 원판(radius > 0인데 축 없음), 축 차이 overflow.
    CHECK(!Cylinder<T>::TryMake(V<T>(0, 0, 0), V<T>(0, 0, 4), S<T>(-0.5)));
    CHECK(!Cylinder<T>::TryMake(V<T>(0, 0, 0), V<T>(0, 0, 4), QuietNaN<T>()));
    CHECK(!Cylinder<T>::TryMake(V<T>(0, 0, 0), V<T>(0, 0, 4), Infinity<T>()));
    CHECK(!Cylinder<T>::TryMake(V<T>(0, 0, 0), V<T>(0, 0, Infinity<T>()), S<T>(1)));
    CHECK(!Cylinder<T>::TryMake(V<T>(0, QuietNaN<T>(), 0), V<T>(0, 0, 1), S<T>(1)));
    CHECK(!Cylinder<T>::TryMake(V<T>(2, 2, 2), V<T>(2, 2, 2), S<T>(1)));
    CHECK(!Cylinder<T>(V<T>(2, 2, 2), V<T>(2, 2, 2), S<T>(1)).IsValid());
    const T huge = NumericLimits<T>::Max();
    CHECK(!Cylinder<T>::TryMake(Vector3<T>(-huge, 0, 0), Vector3<T>(huge, 0, 0), S<T>(1)));
    CHECK(Cylinder<T>::TryMake(Vector3<T>(-huge, 0, 0), Vector3<T>(huge, 0, 0), S<T>(0)));

    // 축 성분이 아주 작아도(|d|²가 underflow) TryNormalize가 가장 큰 성분으로 먼저 나누므로
    // 축이 정해지고 유효하다. 가장 작은 비정규 차이도 같다.
    const T tiny = IsSameV<T, float> ? S<T>(1.0e-30) : S<T>(1.0e-200);
    CHECK(Cylinder<T>::TryMake(V<T>(0, 0, 0), Vector3<T>(tiny, tiny, 0), S<T>(1)));
    const T smallest = math::NextUp(S<T>(0));
    CHECK(Cylinder<T>::TryMake(V<T>(0, 0, 0), Vector3<T>(smallest, 0, 0), S<T>(1)));

    // 판정은 모두 잘못된 값을 InvalidInput·InvalidArgument로 거부한다.
    const Cylinder<T> disc(V<T>(0, 0, 0), V<T>(0, 0, 0), S<T>(1));
    const Cylinder<T> negative(V<T>(0, 0, 0), V<T>(0, 0, 1), S<T>(-1));
    const Cylinder<T> invalids[] = {disc, negative};
    const Ray<T> ray(V<T>(-5, 0, 0), Vector3<T>::Forward());
    const Plane<T> plane;
    const Sphere<T> sphere(V<T>(0, 0, 0), S<T>(1));
    const Segment<T> segment(V<T>(-1, 0, 0), V<T>(1, 0, 0));
    const Plane<T> cubePlanes[] = {Plane<T>(V<T>(0, 0, 1), S<T>(-10))};
    const ConvexVolume<T> volume = ConvexVolume<T>::TryMake(cubePlanes).Value();
    for (const Cylinder<T>& invalid : invalids) {
        CHECK(!TryBounds(invalid));
        CHECK(ContainsPoint(invalid, V<T>(0, 0, 0)) == QueryResult::InvalidInput);
        CHECK(!TryClosestPoint(invalid, V<T>(0, 0, 0)));
        CHECK(!TryDistanceSquared(invalid, V<T>(0, 0, 0)));
        CHECK(Raycast(ray, invalid).result == QueryResult::InvalidInput);
        CHECK(SegmentCast(segment, invalid).result == QueryResult::InvalidInput);
        CHECK(Overlap(invalid, sphere) == QueryResult::InvalidInput);
        CHECK(Overlap(sphere, invalid) == QueryResult::InvalidInput);
        CHECK(Overlap(invalid, plane) == QueryResult::InvalidInput);
        CHECK(Overlap(plane, invalid) == QueryResult::InvalidInput);
        CHECK(Overlap(segment, invalid) == QueryResult::InvalidInput);
        CHECK(Overlap(invalid, segment) == QueryResult::InvalidInput);
        CHECK(ClassifyPlaneSide(plane, invalid) == PlaneSide::InvalidInput);
        CHECK(ClassifyContainment(volume, invalid) == Containment::InvalidInput);
        CHECK(Overlap(volume, invalid) == QueryResult::InvalidInput);
        CHECK(Overlap(invalid, volume) == QueryResult::InvalidInput);
    }

    // 다른 입력의 오류도 같다.
    const Cylinder<T> valid = made.Value();
    CHECK(ContainsPoint(valid, V<T>(0, QuietNaN<T>(), 0)) == QueryResult::InvalidInput);
    CHECK(!TryClosestPoint(valid, V<T>(Infinity<T>(), 0, 0)));
    CHECK(Raycast(Ray<T>(V<T>(-5, 0, 0), V<T>(2, 0, 0)), valid).result ==
          QueryResult::InvalidInput);
    CHECK(Raycast(ray, valid, S<T>(-1)).result == QueryResult::InvalidInput);
    CHECK(Raycast(ray, valid, QuietNaN<T>()).result == QueryResult::InvalidInput);
    CHECK(SegmentCast(Segment<T>(V<T>(0, 0, 0), V<T>(QuietNaN<T>(), 0, 0)), valid).result ==
          QueryResult::InvalidInput);
    CHECK(Overlap(valid, Sphere<T>(V<T>(0, 0, 0), S<T>(-1))) == QueryResult::InvalidInput);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 2), 0), valid) == PlaneSide::InvalidInput);
    CHECK(ClassifyContainment(ConvexVolume<T>{}, valid) == Containment::InvalidInput);
}

// =================================================================================================
// 경계(정확 산술 oracle)
// =================================================================================================

template <typename T>
void CheckBoundsFixedCases() {
    // radius 0은 두 끝점의 box(정확)다.
    const Result<Box<T>> segment = TryBounds(Cylinder<T>(V<T>(1, -2, 3), V<T>(-4, 5, 0), S<T>(0)));
    REQUIRE(segment);
    CHECK(segment.Value().min == V<T>(-4, -2, 0));
    CHECK(segment.Value().max == V<T>(1, 5, 3));

    // 좌표축과 평행한 축은 축 방향 좌표가 끝점 그대로이고 나머지 축은 r보다 조금 넓다.
    const Cylinder<T> upright(V<T>(1, 2, 0), V<T>(1, 2, 10), S<T>(3));
    const Result<Box<T>> uprightBounds = TryBounds(upright);
    REQUIRE(uprightBounds);
    CHECK(uprightBounds.Value().min.z == 0 && uprightBounds.Value().max.z == 10);
    CHECK(uprightBounds.Value().min.x < -2 && uprightBounds.Value().min.x > S<T>(-2.00001));
    CheckBoundsAgainstOracle(upright);

    // 리뷰 반례: 축이 x축과 거의 평행하면 a_x가 1로 반올림되지만 실제 x 반폭은 1e-5다.
    const Cylinder<T> review(V<T>(0, 0, 0), V<T>(1000, 1.0e-6, 0), S<T>(1.0e4));
    CheckBoundsAgainstOracle(review);
    const Result<Box<T>> reviewBounds = TryBounds(review);
    REQUIRE(reviewBounds);
    CHECK(reviewBounds.Value().min.x < S<T>(-0.9e-5));
    CHECK(reviewBounds.Value().max.x > S<T>(1000));

    // 원점에서 1e9 cm, 큰 반경, 대각 축, 음의 방향 축, −0 성분.
    CheckBoundsAgainstOracle(Cylinder<T>(V<T>(1.0e9, 1.0e9, 1.0e9),
                                         V<T>(1.0e9 + 64, 1.0e9 - 128, 1.0e9 + 256), S<T>(32)));
    CheckBoundsAgainstOracle(
        Cylinder<T>(V<T>(-1.0e9, 3.0e8, 1.0e9), V<T>(-1.0e9 + 512, 3.0e8, 1.0e9 + 1), S<T>(0.75)));
    CheckBoundsAgainstOracle(Cylinder<T>(V<T>(5, 5, 5), V<T>(6, 6, 6), S<T>(1.0e6)));
    CheckBoundsAgainstOracle(Cylinder<T>(V<T>(3, -1, 2), V<T>(-7, 4, -9), S<T>(0.125)));
    CheckBoundsAgainstOracle(Cylinder<T>(V<T>(-0.0, 0, -0.0), V<T>(0, -0.0, -3), S<T>(2)));
    CheckBoundsAgainstOracle(Cylinder<T>(V<T>(0, 0, 0), V<T>(1, 1.0e-3, 1.0e-4), S<T>(1.0e5)));

    // 축 비율이 비정규 범위로 작아지는 경우와 축 성분이 아주 작은 경우(double만 의미가 있다).
    if constexpr (IsSameV<T, double>) {
        CheckBoundsAgainstOracle(Cylinder<T>(V<T>(0, 0, 0), V<T>(1, 1.0e-310, 0), S<T>(1.0e30)));
        CheckBoundsAgainstOracle(
            Cylinder<T>(V<T>(0, 0, 0), V<T>(1.0e-200, 3.0e-201, 0), S<T>(1.0e-180)));
        CheckBoundsAgainstOracle(
            Cylinder<T>(V<T>(0, 0, 0), V<T>(1, 1.0e-300, 0), math::NextUp(0.0)));
        CheckBoundsAgainstOracle(
            Cylinder<T>(V<T>(1.0e9, 0, 0), V<T>(1.0e9, 1.0e-9, 1.0e-9), S<T>(1.0e9)));
    } else {
        CheckBoundsAgainstOracle(Cylinder<T>(V<T>(0, 0, 0), V<T>(1, 1.0e-40, 0), S<T>(1.0e9)));
        CheckBoundsAgainstOracle(
            Cylinder<T>(V<T>(0, 0, 0), V<T>(1.0e-30, 3.0e-31, 0), S<T>(1.0e-20)));
    }

    // 경계 좌표 overflow는 실패다.
    const T huge = NumericLimits<T>::Max();
    CHECK(!TryBounds(Cylinder<T>(Vector3<T>(huge, 0, 0), Vector3<T>(huge, 0, 1), huge)));
}

template <typename T>
void PropertyBoundsExact() {
    Random random(CylinderSeed);
    for (int iteration = 0; iteration < 1500; ++iteration) {
        // 정수·dyadic 좌표(2^-6 단위)와 반경, 가끔 큰 좌표 오프셋.
        const int fractionBits = static_cast<int>(random.Below(7));
        const double offsetScale = random.Below(4) == 0 ? 1.0e8 : 0.0;
        const Vec3d offset{random.Dyadic(8, 0) * offsetScale, random.Dyadic(8, 0) * offsetScale,
                           random.Dyadic(8, 0) * offsetScale};
        const Vec3d start{offset.x + random.Dyadic(4096, fractionBits),
                          offset.y + random.Dyadic(4096, fractionBits),
                          offset.z + random.Dyadic(4096, fractionBits)};
        // 축 성분 하나를 아주 작게 만들어 거의 평행한 축도 넣는다.
        Vec3d axis{random.Dyadic(4096, fractionBits), random.Dyadic(4096, fractionBits),
                   random.Dyadic(4096, fractionBits)};
        const uint32 squash = random.Below(5);
        if (squash == 0) {
            axis.y = axis.y * 1.0e-9;
        } else if (squash == 1) {
            axis.z = 0.0;
            axis.x = axis.x * 1.0e-7;
        }
        const double radius = MaxD(math::Abs(random.Dyadic(1 << 20, fractionBits + 4)), 0.0625);
        const Cylinder<T> cylinder(Vector3<T>(start), Vector3<T>(start + axis),
                                   static_cast<T>(radius));
        if (!cylinder.IsValid()) {
            continue;
        }
        CheckBoundsAgainstOracle(cylinder);
    }
}

// =================================================================================================
// 점 포함·최근접점·거리
// =================================================================================================

template <typename T>
void CheckContainsAndClosest() {
    const Cylinder<T> upright(V<T>(0, 0, 0), V<T>(0, 0, 2), S<T>(1));

    // 끝면 중심·모서리·옆면 위의 점은 정확히 Hit, 한 ulp 밖은 Miss다.
    CHECK(ContainsPoint(upright, V<T>(0, 0, 0)) == QueryResult::Hit);
    CHECK(ContainsPoint(upright, V<T>(0, 0, 2)) == QueryResult::Hit);
    CHECK(ContainsPoint(upright, V<T>(1, 0, 2)) == QueryResult::Hit);
    CHECK(ContainsPoint(upright, V<T>(0, -1, 1)) == QueryResult::Hit);
    CHECK(ContainsPoint(upright, V<T>(0.5, 0.5, 1)) == QueryResult::Hit);
    CHECK(ContainsPoint(upright, Vector3<T>(0, 0, math::NextUp(S<T>(2)))) == QueryResult::Miss);
    CHECK(ContainsPoint(upright, Vector3<T>(0, 0, math::NextDown(S<T>(0)))) == QueryResult::Miss);
    CHECK(ContainsPoint(upright, Vector3<T>(math::NextUp(S<T>(1)), 0, 1)) == QueryResult::Miss);
    // 곱집합이라 끝면 모서리 바깥(캡슐이면 포함될 점)은 Miss다.
    CHECK(ContainsPoint(upright, V<T>(0.9, 0, 2.1)) == QueryResult::Miss);

    // 최근접점·거리: 끝면 영역, 옆면 영역, 모서리 영역, 안.
    CHECK(TryClosestPoint(upright, V<T>(0.5, 0, 5)).Value() == V<T>(0.5, 0, 2));
    CHECK(TryDistanceSquared(upright, V<T>(0.5, 0, 5)).Value() == 9);
    CHECK(TryClosestPoint(upright, V<T>(4, 0, 1)).Value() == V<T>(1, 0, 1));
    CHECK(TryDistanceSquared(upright, V<T>(4, 0, 1)).Value() == 9);
    CHECK(TryClosestPoint(upright, V<T>(0, -4, -4)).Value() == V<T>(0, -1, 0));
    CHECK(TryDistanceSquared(upright, V<T>(0, -4, -4)).Value() == 25);
    CHECK(TryClosestPoint(upright, V<T>(0.25, 0.5, 1.5)).Value() == V<T>(0.25, 0.5, 1.5));
    CHECK(TryDistanceSquared(upright, V<T>(0.25, 0.5, 1.5)).Value() == 0);

    // radius 0은 선분이다.
    const Cylinder<T> segment(V<T>(0, 0, 0), V<T>(4, 0, 0), S<T>(0));
    CHECK(ContainsPoint(segment, V<T>(2, 0, 0)) == QueryResult::Hit);
    CHECK(ContainsPoint(segment, V<T>(2, 0.5, 0)) == QueryResult::Miss);
    CHECK(TryClosestPoint(segment, V<T>(6, 3, 0)).Value() == V<T>(4, 0, 0));
    CHECK(TryDistanceSquared(segment, V<T>(2, 3, 0)).Value() == 9);

    // 긴 축의 수직 거리: |p - s|² - t² 꼴은 H = 1e9, r = 1에서 소거로 망가진다(오차 약 100 > r²).
    if constexpr (IsSameV<T, double>) {
        const Cylinder<T> longAxis(V<T>(0, 0, 0), V<T>(1.0e9, 1.0e9, 0), S<T>(1));
        const Vec3d middle(5.0e8, 5.0e8, 0.0);
        const Vec3d inPlane = Vec3d(-1, 1, 0) / math::Sqrt(2.0);
        CHECK(ContainsPoint(longAxis, middle + Vec3d(0, 0, 0.999)) == QueryResult::Hit);
        CHECK(ContainsPoint(longAxis, middle + Vec3d(0, 0, 1.001)) == QueryResult::Miss);
        CHECK(ContainsPoint(longAxis, middle + inPlane * 0.999) == QueryResult::Hit);
        CHECK(ContainsPoint(longAxis, middle + inPlane * 1.001) == QueryResult::Miss);
        const double distance = TryDistanceSquared(longAxis, middle + Vec3d(0, 0, 3)).Value();
        CHECK(math::Abs(distance - 4.0) <= 1.0e-6);
        // 원점에서 1e9 cm 떨어진 끝점은 정확히 포함이다.
        const Cylinder<T> far(V<T>(1.0e9, -1.0e9, 1.0e9), V<T>(1.0e9 + 3, -1.0e9 + 4, 1.0e9 + 12),
                              S<T>(0.5));
        CHECK(ContainsPoint(far, far.start) == QueryResult::Hit);
        CHECK(ContainsPoint(far, far.end) == QueryResult::Hit);
        CHECK(TryDistanceSquared(far, far.end).Value() == 0);
    }
}

template <typename T>
void PropertyContainsAndClosest() {
    Random random(CylinderSeed ^ 0x11u);
    const double margin = Loose<T>() * 10.0;
    for (int iteration = 0; iteration < 3000; ++iteration) {
        const Cylinder<T> cylinder = Narrow<T>(RandomCylinder(random, true));
        REQUIRE(cylinder.IsValid());
        const OracleCylinder oracle = ToOracle(cylinder);
        const Vector3<T> point(random.Point(12.0));
        const Vec3d widePoint = Widen(point);
        const double sdf = OracleCylinderSdf(oracle, widePoint);

        const QueryResult contained = ContainsPoint(cylinder, point);
        if (cylinder.radius > 0 && sdf < -margin) {
            CHECK(contained == QueryResult::Hit);
        }
        if (sdf > margin) {
            CHECK(contained == QueryResult::Miss);
        }

        const Vector3<T> closest = TryClosestPoint(cylinder, point).Value();
        const double distanceSquared =
            static_cast<double>(TryDistanceSquared(cylinder, point).Value());
        const double oracleDistance = MaxD(sdf, 0.0);
        CHECK(math::Abs(math::Sqrt(distanceSquared) - oracleDistance) <= margin);
        CHECK(math::Abs(DistanceSquared(Widen(closest), widePoint) - distanceSquared) <=
              margin * (1.0 + distanceSquared));
        CHECK(OracleCylinderSdf(oracle, Widen(closest)) <= margin);
        if (contained == QueryResult::Hit) {
            CHECK(closest == point);
            CHECK(distanceSquared == 0.0);
        }

        // 구 겹침은 거리와 같은 식이다(대칭 포함).
        const T sphereRadius = static_cast<T>(random.Range(0.0, 3.0));
        const Sphere<T> sphere(point, sphereRadius);
        const QueryResult overlap = Overlap(cylinder, sphere);
        CHECK(overlap == Overlap(sphere, cylinder));
        const double gap = sdf - static_cast<double>(sphereRadius);
        if (gap < -margin) {
            CHECK(overlap == QueryResult::Hit);
        } else if (gap > margin) {
            CHECK(overlap == QueryResult::Miss);
        }
    }
}

// =================================================================================================
// 광선·선분
// =================================================================================================

template <typename T>
void CheckRayCylinder() {
    const Cylinder<T> upright(V<T>(0, 0, 0), V<T>(0, 0, 2), S<T>(1));

    const RayHit<T> side = Raycast(Ray<T>(V<T>(-5, 0, 1), Vector3<T>::Forward()), upright);
    REQUIRE(side.IsHit());
    CHECK(side.distance == 4);
    CHECK(side.point == V<T>(-1, 0, 1));
    CHECK(side.normal == V<T>(-1, 0, 0));
    CHECK(!side.startedInside);

    // maxDistance가 정확히 진입 거리면 Hit, 한 ulp 짧으면 Miss다.
    CHECK(Raycast(Ray<T>(V<T>(-5, 0, 1), Vector3<T>::Forward()), upright, S<T>(4)).IsHit());
    CHECK(Raycast(Ray<T>(V<T>(-5, 0, 1), Vector3<T>::Forward()), upright, math::NextDown(S<T>(4)))
              .result == QueryResult::Miss);

    // 축 평행 ray는 끝면으로 들어오며 normal은 축 방향이다.
    const RayHit<T> cap = Raycast(Ray<T>(V<T>(0.5, 0, 7), Vector3<T>::Down()), upright);
    REQUIRE(cap.IsHit());
    CHECK(cap.distance == 5);
    CHECK(cap.point == V<T>(0.5, 0, 2));
    CHECK(cap.normal == V<T>(0, 0, 1));
    const RayHit<T> bottom = Raycast(Ray<T>(V<T>(0, -0.5, -3), Vector3<T>::Up()), upright);
    REQUIRE(bottom.IsHit());
    CHECK(bottom.distance == 3);
    CHECK(bottom.normal == V<T>(0, 0, -1));

    // 옆면 위를 축과 평행하게 스치는 ray와 끝면 평면 안을 스치는 ray는 Hit(닫힌 집합)다.
    const RayHit<T> alongSide = Raycast(Ray<T>(V<T>(1, 0, -3), Vector3<T>::Up()), upright);
    REQUIRE(alongSide.IsHit());
    CHECK(alongSide.distance == 3);
    CHECK(alongSide.normal == V<T>(0, 0, -1));
    const RayHit<T> inCapPlane = Raycast(Ray<T>(V<T>(-5, 0, 2), Vector3<T>::Forward()), upright);
    REQUIRE(inCapPlane.IsHit());
    CHECK(inCapPlane.distance == 4);
    CHECK(inCapPlane.point == V<T>(-1, 0, 2));
    CHECK(inCapPlane.normal == V<T>(-1, 0, 0));
    CHECK(Raycast(Ray<T>(Vector3<T>(-5, 0, math::NextUp(S<T>(2))), Vector3<T>::Forward()), upright)
              .result == QueryResult::Miss);
    CHECK(Raycast(Ray<T>(Vector3<T>(math::NextUp(S<T>(1)), 0, -3), Vector3<T>::Up()), upright)
              .result == QueryResult::Miss);

    // 옆면 접선은 Hit, 한 ulp 밖은 Miss다.
    const RayHit<T> tangent = Raycast(Ray<T>(V<T>(-5, 1, 1), Vector3<T>::Forward()), upright);
    REQUIRE(tangent.IsHit());
    CHECK(tangent.distance == 5);
    CHECK(tangent.point == V<T>(0, 1, 1));
    CHECK(Raycast(Ray<T>(Vector3<T>(-5, math::NextUp(S<T>(1)), 1), Vector3<T>::Forward()), upright)
              .result == QueryResult::Miss);

    // 시작점이 안이거나 경계 위면 거리 0, startedInside, normal 0이다.
    const RayHit<T> inside = Raycast(Ray<T>(V<T>(0.5, 0, 1), Vector3<T>::Up()), upright);
    CHECK(inside.IsHit() && inside.startedInside && IsZeroVector(inside.normal));
    CHECK(IsPositiveZero(inside.distance));
    const RayHit<T> onCap = Raycast(Ray<T>(V<T>(0, 0, 2), Vector3<T>::Up()), upright);
    CHECK(onCap.IsHit() && onCap.startedInside);
    const RayHit<T> onRim = Raycast(Ray<T>(V<T>(1, 0, 0), Vector3<T>::Forward()), upright);
    CHECK(onRim.IsHit() && onRim.startedInside);

    // 뒤쪽·멀어짐·끝면 평면 밖에서 축과 수직으로 지남은 Miss다.
    CHECK(Raycast(Ray<T>(V<T>(-5, 0, 1), Vector3<T>::Backward()), upright).result ==
          QueryResult::Miss);
    CHECK(Raycast(Ray<T>(V<T>(0, 0, 3), Vector3<T>::Up()), upright).result == QueryResult::Miss);
    CHECK(Raycast(Ray<T>(V<T>(-5, 0, 3), Vector3<T>::Forward()), upright).result ==
          QueryResult::Miss);

    // radius 0(선분)은 정확히 지날 때만 Hit이고 normal은 -direction이다.
    const Cylinder<T> segment(V<T>(0, -2, 0), V<T>(0, 2, 0), S<T>(0));
    const RayHit<T> through = Raycast(Ray<T>(V<T>(-3, 1, 0), Vector3<T>::Forward()), segment);
    REQUIRE(through.IsHit());
    CHECK(through.distance == 3);
    CHECK(through.normal == V<T>(-1, 0, 0));
    CHECK(Raycast(Ray<T>(V<T>(-3, 1, 0.25), Vector3<T>::Forward()), segment).result ==
          QueryResult::Miss);

    // 회전된 cylinder의 해석해: 축 (1, 1, 0)/√2 위의 반지름 1 원기둥에 축과 수직인 +Z ray.
    const Cylinder<T> diagonal(V<T>(0, 0, 0), V<T>(4, 4, 0), S<T>(1));
    const RayHit<T> fromBelow = Raycast(Ray<T>(V<T>(2, 2, -5), Vector3<T>::Up()), diagonal);
    REQUIRE(fromBelow.IsHit());
    CHECK(math::Abs(static_cast<double>(fromBelow.distance) - 4.0) <= Loose<T>());
    CHECK(Widen(fromBelow.normal).Equals(Vec3d(0, 0, -1), Loose<T>()));
}

template <typename T>
void CheckSegmentCylinder() {
    const Cylinder<T> upright(V<T>(0, 0, 0), V<T>(0, 0, 2), S<T>(1));

    // 모서리 동률: 끝면과 옆면에 같은 t로 진입하면 끝면 normal이다.
    const SegmentHit<T> rim = SegmentCast(Segment<T>(V<T>(-2, 0, 3), V<T>(0, 0, 1)), upright);
    REQUIRE(rim.IsHit());
    CHECK(rim.t == S<T>(0.5));
    CHECK(rim.point == V<T>(-1, 0, 2));
    CHECK(rim.normal == V<T>(0, 0, 1));

    // 끝점 접촉은 t = 1 Hit, 짧으면 Miss다.
    const SegmentHit<T> endTouch = SegmentCast(Segment<T>(V<T>(-5, 0, 1), V<T>(-1, 0, 1)), upright);
    REQUIRE(endTouch.IsHit());
    CHECK(endTouch.t == 1);
    CHECK(endTouch.normal == V<T>(-1, 0, 0));
    CHECK(SegmentCast(Segment<T>(V<T>(-5, 0, 1), V<T>(-1.5, 0, 1)), upright).result ==
          QueryResult::Miss);

    // 0 길이 segment는 점 포함과 같다.
    CHECK(SegmentCast(Segment<T>(V<T>(0.5, 0, 1), V<T>(0.5, 0, 1)), upright).IsHit());
    CHECK(SegmentCast(Segment<T>(V<T>(1.5, 0, 1), V<T>(1.5, 0, 1)), upright).result ==
          QueryResult::Miss);

    // 시작점이 안이면 t = 0 시작 접촉이다.
    const SegmentHit<T> startInside =
        SegmentCast(Segment<T>(V<T>(0, 0, 1), V<T>(9, 0, 1)), upright);
    CHECK(startInside.IsHit() && startInside.startedInside && startInside.t == 0);

    // radius 0 cylinder와 교차하는 segment.
    const Cylinder<T> line(V<T>(0, -2, 0), V<T>(0, 2, 0), S<T>(0));
    CHECK(SegmentCast(Segment<T>(V<T>(-1, 0, 0), V<T>(1, 0, 0)), line).IsHit());
    CHECK(SegmentCast(Segment<T>(V<T>(-1, 0, 0), V<T>(0, 0, 0)), line).IsHit());
    CHECK(SegmentCast(Segment<T>(V<T>(-1, 0, 1), V<T>(1, 0, 1)), line).result == QueryResult::Miss);

    // segment 겹침은 SegmentCast와 같고 대칭이다.
    const Segment<T> crossing(V<T>(-3, 0, 1), V<T>(3, 0, 1));
    CHECK(Overlap(crossing, upright) == QueryResult::Hit);
    CHECK(Overlap(upright, crossing) == QueryResult::Hit);
    CHECK(Overlap(Segment<T>(V<T>(-3, 0, 5), V<T>(3, 0, 5)), upright) == QueryResult::Miss);
}

template <typename T>
void PropertyRayAndSegment() {
    Random random(CylinderSeed ^ 0x22u);
    const double margin = Loose<T>() * 10.0;
    int hits = 0;
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const Cylinder<T> cylinder = Narrow<T>(RandomCylinder(random, false));
        REQUIRE(cylinder.IsValid());
        const OracleCylinder oracle = ToOracle(cylinder);

        // 대부분 cylinder 근처를 향하게 한다.
        const Vector3<T> origin(random.Point(15.0));
        const Vec3d target = (oracle.start + oracle.end) * 0.5 + random.Point(3.0);
        const Result<Ray<T>> made = Ray<T>::TryMake(origin, Vector3<T>(target - Widen(origin)));
        if (!made) {
            continue;
        }
        const Ray<T> ray = made.Value();
        const double maxDistance = random.Range(5.0, 40.0);
        const RayHit<T> hit = Raycast(ray, cylinder, static_cast<T>(maxDistance));
        const OracleHit expected = OracleCylinderRaycast(oracle, Widen(ray.origin),
                                                         Widen(ray.direction), maxDistance, margin);
        switch (expected.kind) {
        case OracleHit::Miss:
            CHECK(hit.result == QueryResult::Miss);
            break;
        case OracleHit::Inside:
            CHECK(hit.IsHit() && hit.startedInside && hit.distance == 0);
            break;
        case OracleHit::Hit: {
            if (math::Abs(maxDistance - expected.distance) <= margin) {
                break;
            }
            REQUIRE(hit.IsHit());
            ++hits;
            CHECK(!hit.startedInside);
            CHECK(math::Abs(static_cast<double>(hit.distance) - expected.distance) <= margin);
            CHECK(math::Abs(OracleCylinderSdf(oracle, Widen(hit.point))) <= margin);
            CHECK(math::Abs(Widen(hit.normal).Length() - 1.0) <= Loose<T>());
            // normal은 진입한 경계의 바깥 방향이라 ray 방향과 마주본다.
            CHECK(Dot(Widen(hit.normal), Widen(ray.direction)) <= Loose<T>());
            break;
        }
        case OracleHit::Ambiguous:
            break;
        }

        // segment는 같은 구간 교집합을 [0, 1] 매개변수로 계산한다.
        const Vector3<T> segmentEnd = ray.PointAt(static_cast<T>(maxDistance));
        const Segment<T> segment(ray.origin, segmentEnd);
        const SegmentHit<T> segmentHit = SegmentCast(segment, cylinder);
        CHECK(Overlap(segment, cylinder) == segmentHit.result);
        CHECK(Overlap(cylinder, segment) == segmentHit.result);
        if (expected.kind == OracleHit::Miss) {
            CHECK(segmentHit.result == QueryResult::Miss);
        } else if (expected.kind == OracleHit::Hit &&
                   math::Abs(maxDistance - expected.distance) > margin) {
            REQUIRE(segmentHit.IsHit());
            CHECK(math::Abs(static_cast<double>(segmentHit.t) * maxDistance - expected.distance) <=
                  margin * 4.0);
        }

        // 끝점이 안(ContainsPoint)이면 계산된 t가 1을 넘어도 Hit다(끝점 접촉 보존).
        const Vec3d surface = oracle.start + (oracle.end - oracle.start) * random.Unit();
        const Vector3<T> nearSurface(surface + random.UnitVector() * random.Range(0.0, 3.0));
        const Segment<T> toNear(ray.origin, nearSurface);
        if (ContainsPoint(cylinder, nearSurface) == QueryResult::Hit) {
            const SegmentHit<T> nearHit = SegmentCast(toNear, cylinder);
            CHECK(nearHit.IsHit());
            CHECK(math::Abs(Widen(nearHit.normal).Length() - 1.0) <= Loose<T>() ||
                  nearHit.startedInside);
        }
    }
    CHECK(hits > 200);
}

// =================================================================================================
// 평면·볼록 영역(Cylinder와 Capsule 보강)
// =================================================================================================

template <typename T>
void CheckPlaneClassification() {
    const Cylinder<T> upright(V<T>(0, 0, 0), V<T>(0, 0, 2), S<T>(1));
    const Capsule<T> capsule(V<T>(0, 0, 0), V<T>(0, 0, 2), S<T>(1));

    // 끝면 평면 접촉(끝점 거리 정확히 0, |n × a| = 0)과 한 ulp 밖.
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 1), S<T>(2)), upright) == PlaneSide::Intersecting);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 1), math::NextUp(S<T>(2))), upright) ==
          PlaneSide::Back);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 1), math::NextDown(S<T>(0))), upright) ==
          PlaneSide::Front);
    // 옆면 접촉(원판 support r·|n × a| = 1).
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(1, 0, 0), S<T>(1)), upright) == PlaneSide::Intersecting);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(1, 0, 0), math::NextUp(S<T>(1))), upright) ==
          PlaneSide::Back);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(-1, 0, 0), math::NextUp(S<T>(1))), upright) ==
          PlaneSide::Back);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 1, 0), S<T>(-1)), upright) == PlaneSide::Intersecting);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 1, 0), math::NextDown(S<T>(-1))), upright) ==
          PlaneSide::Front);
    CHECK(Overlap(upright, Plane<T>(V<T>(1, 0, 0), S<T>(1))) == QueryResult::Hit);
    CHECK(Overlap(Plane<T>(V<T>(1, 0, 0), S<T>(1.5)), upright) == QueryResult::Miss);

    // capsule은 끝 반구만큼 더 나온다.
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 1), S<T>(3)), capsule) == PlaneSide::Intersecting);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 1), math::NextUp(S<T>(3))), capsule) ==
          PlaneSide::Back);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 1), S<T>(-1)), capsule) == PlaneSide::Intersecting);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 1), math::NextDown(S<T>(-1))), capsule) ==
          PlaneSide::Front);
    CHECK(Overlap(capsule, Plane<T>(V<T>(0, 0, 1), S<T>(3))) == QueryResult::Hit);
    CHECK(Overlap(Plane<T>(V<T>(0, 0, 1), S<T>(3.5)), capsule) == QueryResult::Miss);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(0, 0, 1), 0),
                            Capsule<T>(V<T>(0, 0, 0), V<T>(0, 0, 1), S<T>(-1))) ==
          PlaneSide::InvalidInput);
    CHECK(Overlap(Capsule<T>(V<T>(0, 0, 0), V<T>(0, 0, 1), S<T>(-1)), Plane<T>()) ==
          QueryResult::InvalidInput);

    // 거의 평행한 normal(리뷰 반례): 원판 support는 r·|n × a| = 1e4·1e-9 = 1e-5다. √(1 - (n·a)²)
    // 꼴이면 n·a가 1로 반올림되어 0이 된다.
    if constexpr (IsSameV<T, double>) {
        const Cylinder<T> review(V<T>(0, 0, 0), V<T>(1000, 1.0e-6, 0), S<T>(1.0e4));
        CHECK(ClassifyPlaneSide(Plane<T>(V<T>(1, 0, 0), S<T>(1000 + 0.9e-5)), review) ==
              PlaneSide::Intersecting);
        CHECK(ClassifyPlaneSide(Plane<T>(V<T>(1, 0, 0), S<T>(1000 + 1.1e-5)), review) ==
              PlaneSide::Back);
        CHECK(ClassifyPlaneSide(Plane<T>(V<T>(-1, 0, 0), S<T>(0.9e-5)), review) ==
              PlaneSide::Intersecting);
        CHECK(ClassifyPlaneSide(Plane<T>(V<T>(-1, 0, 0), S<T>(1.1e-5)), review) == PlaneSide::Back);
    }

    // 비단위 normal(허용 오차 안): support는 그 normal에 대해 정확하다(|n|·r).
    const T stretched = S<T>(1) + S<T>(1.0 / 131072.0);
    const Plane<T> loose(Vector3<T>(stretched, 0, 0), stretched - S<T>(1.0 / 1048576.0));
    REQUIRE(loose.IsValid());
    CHECK(ClassifyPlaneSide(loose, upright) == PlaneSide::Intersecting);
    const Plane<T> looseFar(Vector3<T>(stretched, 0, 0), stretched + S<T>(1.0 / 1048576.0));
    CHECK(ClassifyPlaneSide(looseFar, upright) == PlaneSide::Back);

    // radius 0은 선분 분류다.
    const Cylinder<T> segment(V<T>(0, 0, 0), V<T>(0, 0, 2), S<T>(0));
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(1, 0, 0), S<T>(0)), segment) == PlaneSide::Intersecting);
    CHECK(ClassifyPlaneSide(Plane<T>(V<T>(1, 0, 0), math::NextUp(S<T>(0))), segment) ==
          PlaneSide::Back);

    // 0 길이 capsule은 sphere 분류와 같다.
    const Capsule<T> point(V<T>(1, 2, 3), V<T>(1, 2, 3), S<T>(0.75));
    const Sphere<T> sphere(V<T>(1, 2, 3), S<T>(0.75));
    for (double distance = -1.0; distance <= 5.0; distance += 0.125) {
        const Plane<T> plane(V<T>(0, 0, 1), S<T>(distance));
        CHECK(ClassifyPlaneSide(plane, point) == ClassifyPlaneSide(plane, sphere));
    }
}

template <typename T>
ConvexVolume<T> UnitCube() noexcept {
    const Plane<T> planes[] = {
        Plane<T>(V<T>(1, 0, 0), 0),   Plane<T>(V<T>(-1, 0, 0), -1), Plane<T>(V<T>(0, 1, 0), 0),
        Plane<T>(V<T>(0, -1, 0), -1), Plane<T>(V<T>(0, 0, 1), 0),   Plane<T>(V<T>(0, 0, -1), -1),
    };
    return ConvexVolume<T>::TryMake(planes).Value();
}

template <typename T>
void CheckVolumeClassification() {
    const ConvexVolume<T> cube = UnitCube<T>();

    // 네 옆면과 두 끝면에 닿는 cylinder는 닫힌 포함이라 Inside다.
    const Cylinder<T> fitted(V<T>(0.5, 0.5, 0), V<T>(0.5, 0.5, 1), S<T>(0.5));
    CHECK(ClassifyContainment(cube, fitted) == Containment::Inside);
    CHECK(ClassifyContainment(cube, Cylinder<T>(V<T>(0.5, 0.5, 0.25), V<T>(0.5, 0.5, 0.75),
                                                S<T>(0.25))) == Containment::Inside);
    CHECK(ClassifyContainment(cube, Cylinder<T>(V<T>(0.5, 0.5, 0.5), V<T>(0.5, 0.5, 1.5),
                                                S<T>(0.25))) == Containment::Intersecting);
    // 옆면 접촉은 교차(Outside는 엄격), 떨어지면 Outside다.
    CHECK(ClassifyContainment(cube, Cylinder<T>(V<T>(1.5, 0.5, 0), V<T>(1.5, 0.5, 1), S<T>(0.5))) ==
          Containment::Intersecting);
    CHECK(ClassifyContainment(cube, Cylinder<T>(V<T>(1.75, 0.5, 0), V<T>(1.75, 0.5, 1),
                                                S<T>(0.5))) == Containment::Outside);
    // 끝면 접촉도 교차다.
    CHECK(ClassifyContainment(cube, Cylinder<T>(V<T>(0.5, 0.5, 1), V<T>(0.5, 0.5, 3),
                                                S<T>(0.25))) == Containment::Intersecting);
    CHECK(ClassifyContainment(cube, Cylinder<T>(V<T>(0.5, 0.5, 1.25), V<T>(0.5, 0.5, 3),
                                                S<T>(0.25))) == Containment::Outside);
    CHECK(Overlap(cube, fitted) == QueryResult::Hit);
    CHECK(Overlap(fitted, cube) == QueryResult::Hit);
    CHECK(Overlap(cube, Cylinder<T>(V<T>(3, 3, 3), V<T>(4, 4, 4), S<T>(0.5))) == QueryResult::Miss);

    // capsule: 반구까지 들어가면 Inside, 반구가 면에 닿으면 교차.
    CHECK(ClassifyContainment(cube, Capsule<T>(V<T>(0.5, 0.5, 0.25), V<T>(0.5, 0.5, 0.75),
                                               S<T>(0.25))) == Containment::Inside);
    CHECK(ClassifyContainment(cube, Capsule<T>(V<T>(0.5, 0.5, 0.5), V<T>(0.5, 0.5, 1.5),
                                               S<T>(0.25))) == Containment::Intersecting);
    CHECK(ClassifyContainment(cube, Capsule<T>(V<T>(0.5, 0.5, 1.5), V<T>(0.5, 0.5, 3),
                                               S<T>(0.5))) == Containment::Intersecting);
    CHECK(ClassifyContainment(cube, Capsule<T>(V<T>(0.5, 0.5, 1.75), V<T>(0.5, 0.5, 3),
                                               S<T>(0.5))) == Containment::Outside);
    CHECK(Overlap(cube, Capsule<T>(V<T>(2, 0.5, 0.5), V<T>(3, 0.5, 0.5), S<T>(1))) ==
          QueryResult::Hit);
    CHECK(Overlap(Capsule<T>(V<T>(2.5, 0.5, 0.5), V<T>(3, 0.5, 0.5), S<T>(1)), cube) ==
          QueryResult::Miss);
    CHECK(ClassifyContainment(ConvexVolume<T>{}, Capsule<T>()) == Containment::InvalidInput);
    CHECK(ClassifyContainment(cube, Capsule<T>(V<T>(0, 0, 0), V<T>(0, 0, 1), S<T>(-1))) ==
          Containment::InvalidInput);
    CHECK(Overlap(cube, Capsule<T>(V<T>(0, 0, 0), V<T>(0, 0, 1), S<T>(-1))) ==
          QueryResult::InvalidInput);

    // 0 길이 capsule은 sphere 분류와 같다.
    const Capsule<T> point(V<T>(1.25, 0.5, 0.5), V<T>(1.25, 0.5, 0.5), S<T>(0.25));
    CHECK(ClassifyContainment(cube, point) ==
          ClassifyContainment(cube, Sphere<T>(V<T>(1.25, 0.5, 0.5), S<T>(0.25))));
}

template <typename T>
void PropertyPlaneAndVolume() {
    Random random(CylinderSeed ^ 0x33u);
    const double margin = Loose<T>() * 10.0;
    const ConvexVolume<T> cube = UnitCube<T>();
    for (int iteration = 0; iteration < 3000; ++iteration) {
        const OracleCylinder source = RandomCylinder(random, true);
        const Cylinder<T> cylinder = Narrow<T>(source);
        const Capsule<T> capsule(cylinder.start, cylinder.end, cylinder.radius);
        const OracleCylinder oracle = ToOracle(cylinder);
        const Vector3<T> normal(random.UnitVector());
        const T distance = static_cast<T>(random.Range(-12.0, 12.0));
        const Plane<T> plane(normal, distance);
        REQUIRE(plane.IsValid());

        const bool isCapsule[] = {false, true};
        for (const bool asCapsule : isCapsule) {
            const PlaneSide side =
                asCapsule ? ClassifyPlaneSide(plane, capsule) : ClassifyPlaneSide(plane, cylinder);
            const OracleRange range = OraclePlaneRange(
                oracle, Widen(plane.normal), static_cast<double>(plane.distance), asCapsule);
            if (range.lowest > margin) {
                CHECK(side == PlaneSide::Front);
            } else if (range.highest < -margin) {
                CHECK(side == PlaneSide::Back);
            } else if (range.lowest < -margin && range.highest > margin) {
                CHECK(side == PlaneSide::Intersecting);
            }
        }
        CHECK(Overlap(cylinder, plane) == Overlap(plane, cylinder));
        CHECK(Overlap(capsule, plane) == Overlap(plane, capsule));

        // 볼록 영역(작은 cylinder로 단위 cube 근처를 덮는다).
        const Vec3d center = random.Point(1.5) + Vec3d(0.5, 0.5, 0.5);
        const Vec3d axis = random.UnitVector() * random.Range(0.0, 0.8);
        const OracleCylinder small{center, center + axis, random.Range(0.0, 0.5)};
        const Cylinder<T> smallCylinder = Narrow<T>(small);
        if (!smallCylinder.IsValid()) {
            continue;
        }
        const Capsule<T> smallCapsule(smallCylinder.start, smallCylinder.end, smallCylinder.radius);
        const OracleCylinder smallOracle = ToOracle(smallCylinder);
        for (const bool asCapsule : isCapsule) {
            const Containment containment = asCapsule ? ClassifyContainment(cube, smallCapsule)
                                                      : ClassifyContainment(cube, smallCylinder);
            bool surelyOutside = false;
            bool surelyInside = true;
            bool ambiguous = false;
            for (const Plane<T>& face : cube.Planes()) {
                const OracleRange range = OraclePlaneRange(
                    smallOracle, Widen(face.normal), static_cast<double>(face.distance), asCapsule);
                surelyOutside = surelyOutside || range.highest < -margin;
                surelyInside = surelyInside && range.lowest > margin;
                ambiguous = ambiguous || math::Abs(range.highest) <= margin ||
                            math::Abs(range.lowest) <= margin;
            }
            if (surelyOutside) {
                CHECK(containment == Containment::Outside);
            } else if (!ambiguous) {
                CHECK(containment ==
                      (surelyInside ? Containment::Inside : Containment::Intersecting));
            }
        }
        CHECK(Overlap(cube, smallCylinder) == Overlap(smallCylinder, cube));
        CHECK(Overlap(cube, smallCapsule) == Overlap(smallCapsule, cube));
    }
}

// =================================================================================================
// 판정 사이의 일관성(리뷰 반영)과 넓은 분포
// =================================================================================================

// 한 점에 대한 판정들이 같은 안·밖 판단을 내는지 본다: ContainsPoint ⇔ 거리² == 0 ⇔ 반지름 0 sphere
// 겹침 ⇔ ray·segment의 startedInside. radius > 0이면 최근접점은 항상 ContainsPoint Hit다.
template <typename T>
void CheckPointConsistency(const Cylinder<T>& cylinder, const Vector3<T>& point,
                           const Vector3<T>& direction) {
    const bool inside = ContainsPoint(cylinder, point) == QueryResult::Hit;
    const T distanceSquared = TryDistanceSquared(cylinder, point).Value();
    CHECK((distanceSquared == 0) == inside);
    CHECK((Overlap(cylinder, Sphere<T>(point, S<T>(0))) == QueryResult::Hit) == inside);
    CHECK((Overlap(Sphere<T>(point, S<T>(0)), cylinder) == QueryResult::Hit) == inside);

    const Result<Ray<T>> ray = Ray<T>::TryMake(point, direction);
    if (ray) {
        const RayHit<T> hit = Raycast(ray.Value(), cylinder);
        CHECK(hit.startedInside == inside);
        if (hit.IsHit() && !hit.startedInside) {
            CHECK(math::Abs(static_cast<double>(Widen(hit.normal).Length()) - 1.0) <= Loose<T>());
        }
    }
    const SegmentHit<T> segmentHit =
        SegmentCast(Segment<T>(point, point + direction * S<T>(3)), cylinder);
    CHECK(segmentHit.startedInside == inside);

    const Vector3<T> closest = TryClosestPoint(cylinder, point).Value();
    if (cylinder.radius > 0) {
        CHECK(ContainsPoint(cylinder, closest) == QueryResult::Hit);
    }
    if (inside) {
        CHECK(closest == point);
    }
}

// 경계 근처 점(최근접점에서 반올림 수준으로 흔든 점, 끝면·옆면·모서리 위 점)을 많이 만들어 일관성을
// 본다. 큰 좌표(double 1e9 cm, float 1e4 cm)와 좌표축에 평행·거의 평행한 축을 섞는다.
template <typename T>
void PropertyJudgementConsistency() {
    Random random(CylinderSeed ^ 0x44u);
    const double largeOffset = IsSameV<T, float> ? 1.0e4 : 1.0e9;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        OracleCylinder source = RandomCylinder(random, true);
        const Vec3d offset =
            random.Below(3) == 0 ? Vec3d(largeOffset, -largeOffset, largeOffset) : Vec3d();
        source.start = source.start + offset;
        source.end = source.end + offset;
        const Cylinder<T> cylinder = Narrow<T>(source);
        if (!cylinder.IsValid()) {
            continue;
        }
        const OracleCylinder oracle = ToOracle(cylinder);
        const Vec3d axis = (oracle.end - oracle.start) / (oracle.end - oracle.start).Length();
        const Vec3d across = Cross(axis, random.UnitVector());
        const Vec3d radialDirection =
            across.Length() > 0 ? across / across.Length() : Vec3d(0, 0, 1);

        // 0: 경계 위 표본(옆면·끝면·모서리), 1: 무작위 점, 2: 무작위 점의 최근접점.
        const uint32 kind = random.Below(3);
        Vec3d wide;
        if (kind == 0) {
            const uint32 feature = random.Below(3);
            const double along = feature == 0 ? random.Unit() : static_cast<double>(feature - 1);
            const double radial = feature == 0 ? oracle.radius : oracle.radius * random.Unit();
            wide = oracle.start + (oracle.end - oracle.start) * along + radialDirection * radial;
        } else {
            wide = (oracle.start + oracle.end) * 0.5 + random.Point(8.0);
        }
        Vector3<T> point(wide);
        if (kind == 2) {
            point = TryClosestPoint(cylinder, point).Value();
        }
        const Vector3<T> direction(random.UnitVector());
        CheckPointConsistency(cylinder, point, direction);
    }
}

// 접선 ray와 maxDistance = 정확한 진입 거리. 옆면 접선에 가까운 ray는 진입하면 hit point가 표면
// 근처이고, 같은 ray를 진입 거리로 자르면 같은 hit, 한 ulp 짧게 자르면 Miss다.
template <typename T>
void PropertyTangentAndExactMaxDistance() {
    Random random(CylinderSeed ^ 0x55u);
    const double largeOffset = IsSameV<T, float> ? 1.0e4 : 1.0e9;
    int exactChecks = 0;
    for (int iteration = 0; iteration < 3000; ++iteration) {
        OracleCylinder source = RandomCylinder(random, false);
        const bool far = random.Below(3) == 0;
        const Vec3d offset = far ? Vec3d(-largeOffset, largeOffset, largeOffset) : Vec3d();
        source.start = source.start + offset;
        source.end = source.end + offset;
        const Cylinder<T> cylinder = Narrow<T>(source);
        if (!cylinder.IsValid()) {
            continue;
        }
        const OracleCylinder oracle = ToOracle(cylinder);
        const Vec3d axis = (oracle.end - oracle.start) / (oracle.end - oracle.start).Length();
        const Vec3d across = Cross(axis, random.UnitVector());
        if (across.Length() == 0) {
            continue;
        }
        const Vec3d radialDirection = across / across.Length();
        const Vec3d tangent = Cross(axis, radialDirection);
        const Vec3d surface = oracle.start + (oracle.end - oracle.start) * random.Unit() +
                              radialDirection * oracle.radius;
        // 접선 방향에 축 성분과 아주 작은 안쪽·바깥쪽 기울기를 섞는다.
        const double tilt = random.Range(-1.0e-6, 1.0e-6);
        const Vec3d mixed = tangent + axis * random.Range(-0.5, 0.5) + radialDirection * tilt;
        const Vec3d direction = mixed / mixed.Length();
        const double back = random.Range(1.0, 10.0);
        const Result<Ray<T>> made =
            Ray<T>::TryMake(Vector3<T>(surface - direction * back), Vector3<T>(direction));
        if (!made) {
            continue;
        }
        const Ray<T> ray = made.Value();
        const RayHit<T> hit = Raycast(ray, cylinder);
        CHECK(hit.startedInside == (ContainsPoint(cylinder, ray.origin) == QueryResult::Hit));
        if (!hit.IsHit() || hit.startedInside) {
            continue;
        }

        const double margin = Loose<T>() * 10.0 + 64.0 *
                                                      static_cast<double>(math::MachineEpsilon<T>) *
                                                      (far ? largeOffset : 20.0);
        CHECK(math::Abs(OracleCylinderSdf(oracle, Widen(hit.point))) <= margin);
        CHECK(math::Abs(Widen(hit.normal).Length() - 1.0) <= Loose<T>());
        // 접선 진입 normal은 근사다: direction과 같은 쪽이어도 좌표 반올림 / 반지름 정도로 아주
        // 조금이다.
        CHECK(Dot(Widen(hit.normal), Widen(ray.direction)) <=
              margin / static_cast<double>(cylinder.radius));

        if (hit.distance > 0) {
            const RayHit<T> exact = Raycast(ray, cylinder, hit.distance);
            CHECK(exact.IsHit() && exact.distance == hit.distance && exact.point == hit.point);
            CHECK(Raycast(ray, cylinder, math::NextDown(hit.distance)).result == QueryResult::Miss);
            ++exactChecks;
        }
    }
    CHECK(exactChecks > 500);
}

// 리뷰 반례 1: 축이 x 방향 1 ulp인 아주 얇은 축, 큰 반경. ContainsPoint가 Miss인데 거리²가 0이던
// 점이다(포함은 |radial|² <= r², 거리는 |radial| - r로 식이 달랐다).
void CheckReviewDistanceZeroMatchesContains() {
    const Vec3d start(0x1.926fbea9853e0p-4, -0x1.2035b6001f1b4p-2, 0x1.1567edf93a928p-2);
    const Vec3d end(math::NextUp(start.x), start.y, start.z);
    const Cylinder<double> cylinder(start, end, 0x1.e73cff691c7bbp+20);
    REQUIRE(cylinder.IsValid());
    const Vec3d point(0x1.926fbea9853e0p-4, -0x1.dd21c2cd963f2p+20, -0x1.8ae50178a3ed2p+18);
    CheckPointConsistency(cylinder, point, Vec3d(0, 0, 1));
}

// 리뷰 반례 2: radius 0(z축 평행 선분) 위의 점에서 출발한 ray가 startedInside인데 ContainsPoint가
// Miss였다(CastCapsule은 두 식, ContainsPoint는 한 식). radius 0 포함 판정을 CastCapsule 시작
// 판정과 같은 식으로 맞췄다.
template <typename T>
void CheckReviewSegmentStartMatchesContains() {
    const Cylinder<T> segment(V<T>(-6.04e-4, 5.38e-4, -4.67e-4), V<T>(-6.04e-4, 5.38e-4, 1.20e-3),
                              S<T>(0));
    REQUIRE(segment.IsValid());
    const Vector3<T> origin = V<T>(-6.04e-4, 5.38e-4, 3.67e-4);
    const Ray<T> ray(origin, V<T>(-0.0, -0.0, -1));
    REQUIRE(ray.IsValid());
    const RayHit<T> hit = Raycast(ray, segment);
    CHECK(hit.startedInside == (ContainsPoint(segment, origin) == QueryResult::Hit));
    CheckPointConsistency(segment, origin, V<T>(-0.0, -0.0, -1));

    // radius 0 축에 놓인 점들(선분 위 보간점)도 같다.
    Random random(CylinderSeed ^ 0x66u);
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const OracleCylinder source{random.Point(1.0e-2), random.Point(1.0e-2), 0.0};
        const Cylinder<T> line = Narrow<T>(source);
        const Vector3<T> on = Lerp(line.start, line.end, static_cast<T>(random.Unit()));
        CheckPointConsistency(line, on, Vector3<T>(random.UnitVector()));
    }
}

// 밖에서 출발했는데 진입 시각이 0으로 underflow하면 startedInside가 아니라 거리 0 접촉이다.
// 끝면에서 가장 작은 비정규 거리만큼 떨어진 점에서 축 방향 길이 4의 segment를 쏘면 진입 t = dmin /
// 4가 0이 된다.
void CheckUnderflowEntryIsSurfaceContact() {
    const Cylinder<double> upright(Vec3d(0, 0, 0), Vec3d(0, 0, 2), 1.0);
    const Vec3d origin(0.5, 0, -math::NextUp(0.0));
    REQUIRE(ContainsPoint(upright, origin) == QueryResult::Miss);
    const SegmentHit<double> contact =
        SegmentCast(Segment<double>(origin, origin + Vec3d(0, 0, 4)), upright);
    REQUIRE(contact.IsHit());
    CHECK(!contact.startedInside);
    CHECK(IsPositiveZero(contact.t));
    CHECK(contact.normal == Vec3d(0, 0, -1));
    const RayHit<double> hit = Raycast(Ray<double>(origin, Vec3d(0, 0, 1)), upright);
    REQUIRE(hit.IsHit());
    CHECK(!hit.startedInside);
    CHECK(hit.normal == Vec3d(0, 0, -1));
    CHECK(TryDistanceSquared(upright, origin).Value() > 0);
}

} // namespace

TEST(CylinderConstructionAndValidity) {
    CheckCylinderConstruction<float>();
    CheckCylinderConstruction<double>();
}

TEST(CylinderPrecisionConversionIsExplicit) {
    const Cylinder<double> wide(Cylinder<float>(Vec3(0, 0, 0), Vec3(0, 0, 1), 0.25f));
    CHECK(wide.IsValid());
    CHECK(wide.radius == 0.25);
    // double에서만 구분되는 끝점은 float로 바꾸면 원판이 되어 무효다.
    const Cylinder<float> collapsed(Cylinder<double>(Vec3d(1, 1, 1), Vec3d(1, 1, 1 + 1.0e-12), 1));
    CHECK(!collapsed.IsValid());
}

TEST(CylinderBoundsFixedCasesContainExactly) {
    CheckBoundsFixedCases<float>();
    CheckBoundsFixedCases<double>();
}

// 정확 산술 oracle 자체가 포함 위반을 잡는지 본다(음성 대조). 리뷰 반례의 실제 x 반폭은
// 1e-5·(1 - 5e-19)이므로 1e-5·(1 + 1e-10)은 덮고 1e-5·(1 - 1e-10)과 √(1 - a_x²) 꼴의 0은 못 덮는다.
TEST(CylinderBoundsExactOracleDetectsViolations) {
    const Vec3d start(0, 0, 0);
    const Vec3d end(1000, 1.0e-6, 0);
    const double radius = 1.0e4;
    // 높은 쪽은 넉넉히 둔다(1000 + 1e-5·(1 + 1e-10)은 1000의 ulp에서 1e-15 항이 사라진다).
    const Vec3d high(1001, 2.0e4, 2.0e4);
    const ExactBoundsResult covered = ExactBoundsContainCylinder(
        start, end, radius, Vec3d(-1.0e-5 * (1 + 1.0e-10), -2.0e4, -2.0e4), high);
    CHECK(!covered.overflow && covered.contains);
    const ExactBoundsResult tooTight = ExactBoundsContainCylinder(
        start, end, radius, Vec3d(-1.0e-5 * (1 - 1.0e-10), -2.0e4, -2.0e4), high);
    CHECK(!tooTight.overflow && !tooTight.contains);
    const ExactBoundsResult naive =
        ExactBoundsContainCylinder(start, end, radius, Vec3d(0, -2.0e4, -2.0e4), high);
    CHECK(!naive.overflow && !naive.contains);
    // y 반폭은 r·1000/|d|로 r보다 아주 조금 작다. 정확히 r이면 덮고, r보다 한 ulp 작으면
    // 1000/|d|의 부족분(약 5e-19 비율)보다 크게 줄어 못 덮는다.
    const ExactBoundsResult exactY =
        ExactBoundsContainCylinder(start, end, radius, Vec3d(-1, -radius, -2.0e4), high);
    CHECK(!exactY.overflow && exactY.contains);
    const ExactBoundsResult shortY = ExactBoundsContainCylinder(
        start, end, radius, Vec3d(-1, -math::NextDown(radius), -2.0e4), high);
    CHECK(!shortY.overflow && !shortY.contains);
}

TEST(CylinderBoundsRandomDyadicContainExactly) {
    PropertyBoundsExact<float>();
    PropertyBoundsExact<double>();
}

TEST(CylinderContainsClosestAndDistance) {
    CheckContainsAndClosest<float>();
    CheckContainsAndClosest<double>();
}

TEST(CylinderPropertyContainsClosestAndSphere) {
    PropertyContainsAndClosest<float>();
    PropertyContainsAndClosest<double>();
}

TEST(CylinderRaycastRules) {
    CheckRayCylinder<float>();
    CheckRayCylinder<double>();
}

TEST(CylinderSegmentCastRules) {
    CheckSegmentCylinder<float>();
    CheckSegmentCylinder<double>();
}

TEST(CylinderPropertyRayAndSegment) {
    PropertyRayAndSegment<float>();
    PropertyRayAndSegment<double>();
}

TEST(CylinderAndCapsulePlaneClassification) {
    CheckPlaneClassification<float>();
    CheckPlaneClassification<double>();
}

TEST(CylinderAndCapsuleVolumeClassification) {
    CheckVolumeClassification<float>();
    CheckVolumeClassification<double>();
}

TEST(CylinderAndCapsulePropertyPlaneAndVolume) {
    PropertyPlaneAndVolume<float>();
    PropertyPlaneAndVolume<double>();
}

TEST(CylinderReviewJudgementConsistency) {
    PropertyJudgementConsistency<float>();
    PropertyJudgementConsistency<double>();
}

TEST(CylinderReviewTangentRaysAndExactMaxDistance) {
    PropertyTangentAndExactMaxDistance<float>();
    PropertyTangentAndExactMaxDistance<double>();
}

TEST(CylinderReviewDistanceZeroMatchesContains) {
    CheckReviewDistanceZeroMatchesContains();
}

TEST(CylinderReviewSegmentStartMatchesContains) {
    CheckReviewSegmentStartMatchesContains<float>();
    CheckReviewSegmentStartMatchesContains<double>();
}

TEST(CylinderReviewUnderflowEntryIsSurfaceContact) {
    CheckUnderflowEntryIsSurfaceContact();
}
