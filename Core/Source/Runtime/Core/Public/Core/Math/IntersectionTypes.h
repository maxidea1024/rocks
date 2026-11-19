#pragma once

#include <Core/Math/Shapes.h>

namespace rocks {

// 기본 shape의 교차·포함·거리 질의(Intersection.h 묶음)의 결과 형식과 공통 계약이다. 모든 함수는
// 상태 없는 순수 함수이며 어느 스레드에서나 호출할 수 있다.
//
// 결과 계약
// - 입력 오류와 접촉 결과를 구분한다. shape·ray·point가 IsValid가 아니거나(비유한값, 음수 반지름,
//   역전된 box, 단위가 아닌 방향·회전) maxDistance가 NaN·음수이면 QueryResult::InvalidInput을
//   반환하며 Miss나 Hit로 대신하지 않는다. 최근접점·거리 함수는 같은 경우 Result
//   실패(InvalidArgument)다.
// - shape는 닫힌 집합이다. 경계에 정확히 닿으면 Hit다. 비교에 숨은 epsilon이 없으며 허용 오차가
//   필요한 질의(OrientedBox 쌍)는 명시적 인자로 받는다. 따라서 경계 근처 결과는 T 반올림에 따라
//   달라질 수 있다.
// - 대칭: Overlap(a, b)와 Overlap(b, a)는 항상 같은 결과다. 서로 다른 타입 쌍은 두 순서를 모두
//   제공하고 같은 구현을 호출한다. 같은 타입 쌍(Capsule, OrientedBox, segment 최근접점)은 인자를
//   정해진 사전식 순서로 바꿔 계산하므로 순서를 바꿔도 비트 단위로 같은 값이다.
// - Ray 질의: direction은 단위 벡터여야 하고 distance는 origin에서 잰 실제 거리로
//   [0, maxDistance]에 있다. maxDistance는 +Inf일 수 있으며 maxDistance와 같은 거리의 접촉도 Hit다.
//   -0은 0으로 취급한다.
// - 시작점 내부: origin(segment는 start)이 solid(Sphere, Box, Capsule, Cylinder, OrientedBox)의
//   내부나 경계 위면 거리 0, point = origin, normal = 영벡터, startedInside = true인 Hit다.
//   표면에서 바깥으로 쏘는 ray도 경계 위이므로 거리 0 Hit가 된다. UE LineExtentBoxIntersection은
//   같은 상황에서 normal (0,0,1)을 돌려주고 LineBoxIntersection은 bool만 돌려준다. Rocks는 의미
//   없는 방향 대신 영벡터로 표시한다.
// - normal: solid 표면 hit은 바깥 방향 단위 normal(반올림 수준)이다. Plane·Triangle hit은 진행
//   방향의 반대쪽, 즉 ray가 출발한 쪽을 향한다. Plane·Triangle은 두께가 없으므로 startedInside는
//   항상 false다.
// - Segment 질의: t는 [0,1] 매개변수다(거리 아님). Sphere·Box·Triangle은
//   start + (end - start) * t로 계산하므로 end - start가 반올림되면 t = 1의 점이 end와 1 ulp
//   수준으로 달라질 수 있다(예: start=-5, end=-1-ulp이면 차이가 정확히 4로 반올림되어 표면 -1에
//   닿는다). Plane은 끝점별 거리로 계산해 끝점이 평면 위면 t를 정확히 0/1로 낸다. 0 길이 segment는
//   점 판정이다. Plane, Sphere, Box, Triangle, Cylinder만 지원한다.
// - 볼록 영역(프러스텀 포함, ConvexVolume)은 Box·Sphere·Capsule·Cylinder와의 3분류
//   (ClassifyContainment)와 보수적 겹침(Overlap)만 지원한다. ray·segment와 볼록 영역의 교차는 없다.
// - 지원하지 않는 범위: swept shape(sweep), 접촉 깊이·다중 접촉점, mesh 가속 구조. 물리 응답은 별도
//   계층에서 다룬다.

// 겹침·포함 판정 결과다. 입력 오류(InvalidInput)는 접촉 여부(Miss/Hit)와 구분한다.
enum class QueryResult : uint8 {
    Miss,
    Hit,
    InvalidInput
};

// 평면에 대한 위치 분류다. Front/Back은 shape 전체가 엄격히 앞(SignedDistance > 0)/뒤(< 0)에 있을
// 때만이며 평면에 닿기만 해도 Intersecting이다.
enum class PlaneSide : uint8 {
    Front,
    Back,
    Intersecting,
    InvalidInput
};

// 삼각형 ray·segment 질의의 면 선택이다. TwoSided(기본)는 양면 모두 Hit, FrontOnly는 기하
// 앞면(Cross(b - a, c - a) 쪽 — UE FPlane·Chaos FTriangle과 같은 식)에서 들어오는 경우만 Hit다.
// UE 렌더 메시의 앞면(Cross(c - a, b - a), 화면 반시계)과 반대이므로 렌더 메시 삼각형은 b와 c를
// 바꿔 넘긴다(Triangle 설명 참고).
enum class TriangleSides : uint8 {
    TwoSided,
    FrontOnly
};

// Ray 질의 결과다. IsHit()이 false면 나머지 필드는 기본값(0)이며 의미가 없다.
template <typename T>
struct RayHit {
    QueryResult result = QueryResult::Miss;
    T distance = static_cast<T>(0);
    Vector3<T> point;
    Vector3<T> normal;
    bool startedInside = false;

    constexpr bool IsHit() const noexcept {
        return result == QueryResult::Hit;
    }
};

// Segment 질의 결과다. t는 [0,1] 매개변수이며 거리 = t * segment 길이다. IsHit()이 false면 나머지
// 필드는 기본값이다.
template <typename T>
struct SegmentHit {
    QueryResult result = QueryResult::Miss;
    T t = static_cast<T>(0);
    Vector3<T> point;
    Vector3<T> normal;
    bool startedInside = false;

    constexpr bool IsHit() const noexcept {
        return result == QueryResult::Hit;
    }
};

// 두 segment의 최근접점이다. s는 첫 segment, t는 두 번째 segment의 [0,1] 매개변수다.
// 최소 거리를 주는 점 쌍이 여러 개(평행 구간)면 그중 하나를 돌려준다.
template <typename T>
struct SegmentClosestPoints {
    T s = static_cast<T>(0);
    T t = static_cast<T>(0);
    Vector3<T> pointA;
    Vector3<T> pointB;
    T distanceSquared = static_cast<T>(0);
};

// OrientedBox 쌍 분리축 검사의 기본 평행 보정값이다. 두 box의 회전 행렬 원소 절댓값에 더해서, 거의
// 평행한 모서리의 외적 축이 반올림 잡음만으로 분리축이 되는 것을 막는다. 판정은 이 값 * extent
// 정도만큼 겹침 쪽으로 보수적이다(아주 작은 틈은 Hit일 수 있다).
template <typename T>
    requires math::IsFloatingPointV<T>
inline constexpr T ObbParallelEpsilon =
    IsSameV<T, float> ? static_cast<T>(1.0e-6) : static_cast<T>(1.0e-12);

// ---- 볼록 영역·포함 3분류(ConvexVolume, sphere의 box 포함) ----
//
// ClassifyContainment는 shape이 영역 밖(Outside)·안(Inside)·걸침(Intersecting) 중 어디인지
// 돌려준다.
// - Outside는 엄격하다: 어느 평면(또는 sphere 표면)에 대해 shape 전체가 엄격히 바깥일 때만이다.
//   경계에 닿기만 해도 Outside가 아니다(닫힌 집합, 접촉 = 교차).
// - Inside는 닫힌 포함이다: shape 전체가 영역 안(경계 포함)이다.
// - 그 밖은 Intersecting이다. 볼록 영역의 Box 판정은 평면별 판정이라 모서리 근처에서 실제로는
//   영역과 겹치지 않는 box도 Intersecting일 수 있다(UE FConvexVolume::IntersectBox와 같은 보수적
//   술어).
// - 단조성: 평면별 거리(가장 앞·뒤 꼭짓점의 SignedDistance)와 sphere–box 거리는 box 좌표에 대해
//   단조인 고정 순서의 식으로 계산한다. 그래서 box A ⊆ B이면 반올림을 거쳐도 B가 Outside가 아닌 한
//   A의 판정이 B보다 바깥쪽으로 가지 않고, B가 Inside면 A도 Outside가 아니다. 공간 인덱스가 노드
//   경계로 가지를 칠 때 이 성질에 기대어 프록시를 놓치지 않는다(Docs/Design/SpatialIndex.ko.md
//   6.8절).
// - 입력이 유효하지 않으면(평면 없는·잘못된 ConvexVolume, 역전·비유한 box, 음수 반지름)
// InvalidInput이다.
enum class Containment : uint8 {
    Outside,
    Inside,
    Intersecting,
    InvalidInput
};

} // namespace rocks
