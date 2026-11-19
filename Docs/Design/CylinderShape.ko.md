# Rocks Cylinder 형상 설계

기준일: 2026-11-19, 개정 4.

이 문서는 기본 형상에 cylinder(원기둥)를 더하는 설계다. Core 수학의 형상·판정(CY1)과 물리 어댑터의 충돌·질의 형상(CY2) 두 층을 정한다. 문서 전체가 설계이며 구현된 항목은 없다.

- 사용자 결정(2026-11-19): cylinder를 넣는다. UE 5.8.4는 엔진 쪽 기본 충돌·질의 형상에 cylinder가 없으므로(2절), Rocks가 UE보다 형상을 하나 더 갖는다.
- 관련 문서: [수학 구현](../Implementation/Math.ko.md) 2.1절(기본 도형과 판정 계약), [Core 상세 명세](CoreDetailedSpec.ko.md) G04·G05·G06, [물리 어댑터 경계](PhysicsJoltAdapter.ko.md) 2·4·5·8절, [전체 구현 계획](EngineImplementationPlan.ko.md) 16절.
- 좌표·단위는 UE 5.8과 같다(왼손, +Z 위, cm).

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 표현 | `Cylinder<T>{start, end, radius}` — 두 끝면 중심과 반경. float/double 템플릿 | Rocks `Capsule`과 같은 표현. Chaos `FCylinder(x1, x2, Radius)`와 같다(2절). 대안은 결정 3 |
| 퇴화 | radius > 0이면 축을 정규화할 수 있어야 한다(원판 거부). radius == 0이면 선분·점으로 허용한다 | 두 끝점으로는 원판의 normal을 담을 수 없다(3.3절) |
| Core 판정(CY1) | 해석적으로 정확하게 계산할 수 있는 조합: AABB 경계, 점 포함, 최근접점·거리, ray·선분, 구·평면·선분 겹침, 평면 분류, 볼록 영역(`ConvexVolume`) 3분류 | Core 판정은 숨은 허용치 없이 닫힌 집합 계약을 따른다(수학 구현 2.1절) |
| 계산 꼴 | 경계 반폭과 support는 소거가 없는 꼴(`√(d_j² + d_k²)`, `\|n × a\|`)로 계산하고, 경계는 바깥쪽 보정까지 계산으로 보장한다 | `√(1 − x²)` 꼴은 축이 좌표축과 거의 평행할 때 형상을 빠뜨린다(리뷰 반례, 3.4절) |
| Capsule 보강 | 같은 단계에서 Capsule에도 `ClassifyPlaneSide`·`ConvexVolume` 판정과 평면 겹침을 더한다 | 지금 Capsule에는 없다. support가 (H/2)\|n·a\| + r로 쉽다 |
| Core 판정 후속 | cylinder 대 Box·OrientedBox·Capsule·Cylinder·Triangle 겹침, 구 sweep, 선분·캡슐과의 거리 | 유한 분리축으로 끝나지 않거나 4차식이다. 결정 1 |
| 물리(CY2) | 에셋 단순 충돌 형상과 씬 질의 형상에 cylinder를 더하고 Jolt `CylinderShape`로 사상한다 | Jolt가 cylinder를 기본 형상으로 지원한다(2절) |
| 물리 주의 | 질의·trigger·정적 충돌에는 권장, 동적 rigid body에는 경고 | Jolt 문서: cylinder는 가장 불안정한 형상 |
| 테이퍼드 cylinder | 후속(결정 2) | Chaos `FTaperedCylinder`·Jolt `TaperedCylinderShape`는 있으나 요구가 아직 없다 |

## 2. 참조

**UE 5.8.4**(commit `f45729c0b6f0`, 소스 읽기로 확인, 경로는 `Engine/Source/Runtime/` 기준)

| 층 | 형상 | cylinder |
|---|---|---|
| 씬 질의 형상 `FCollisionShape` | Line, Box, Sphere, Capsule(PhysicsCore/Public/CollisionShape.h:8-16) | 없음 |
| 에셋 단순 충돌 `FKAggregateGeom` | Sphere, Box, Sphyl(capsule), Convex, TaperedCapsule, LevelSet 등(Engine/Classes/PhysicsEngine/AggregateGeom.h:28-52) | 없음 |
| Chaos implicit 형상 | Sphere, Box, Capsule, Convex, Cylinder, TaperedCylinder 등(Experimental/Chaos/Public/Chaos/ImplicitObjectType.h:22-23) | 있음 |
| Core 수학 `Math/` | — | 없음 |

- Chaos `FCylinder(x1, x2, Radius)`는 두 끝면 중심과 반경으로 만들고, 안쪽을 향하는 끝면 평면 둘과 높이를 저장한다(Experimental/Chaos/Public/Chaos/Cylinder.h:20-25). `TCylinder`는 4.27부터 deprecated 별칭이다(:481). 판정은 `PhiWithNormal`(:104-)이 한다(:92-103의 부호 거리 코드는 `#if 0`으로 꺼져 있다).
- Chaos의 bounds는 AABB(x1, x2) ± Radius의 보수적 상자다(:26-29). Rocks는 꽉 맞는 경계를 쓴다(3.2절).
- `FTaperedCylinder(x1, x2, Radius1, Radius2)`(TaperedCylinder.h:20).
- Chaos cylinder는 직렬화 경로(Experimental/Chaos/Private/Chaos/ImplicitObject.cpp:464) 외에 엔진 충돌 에셋에서 만드는 경로를 찾지 못했다.

**Jolt**(jrouwe/JoltPhysics master 사본, 최신 릴리스 v5.7.0. master와 태그가 같은지는 미확인)

- `CylinderShape(halfHeight, radius, convexRadius = cDefaultConvexRadius)`, 로컬 Y축, 원점 중심, 끝면은 (0, ±halfHeight, 0)이다(Jolt/Physics/Collision/Shape/CylinderShape.h).
- convex radius와 반높이·반경의 관계(잘림인지 거부인지)는 조사 결과가 엇갈려 미확인이다(J18). 0을 허용하며, 형상을 convex radius만큼 줄였다가 둥글리므로 body는 지정한 cylinder 안쪽에 있다(문서 Convex Radius 절).
- support 함수는 해석적이다(정확한 원기둥). 면·삼각형 출력은 8각형 근사다(CylinderShape.cpp).
- `CastRay`는 `RayCylinder`를 쓴다(본문 미확인). 점 포함은 높이·반경 검사로 정확하다.
- scale은 XZ 균일만 허용한다(`IsValidScale` → `IsUniformScaleXZ`).
- `TaperedCylinderShape`(v5.2.0 도입, 원문 미확인)는 반높이·윗반경·아랫반경을 받는다.
- 공식 문서: "cylinders are the least stable of all shapes, so use another shape if possible."
- 얇은 cylinder의 supporting face 결함이 v5.3.0에서 고쳐졌다.

## 3. Core 수학(CY1)

### 3.1 타입

```cpp
template <typename T>
struct Cylinder {
    Vector3<T> start;   // 한 끝면 중심
    Vector3<T> end;     // 다른 끝면 중심
    T radius = 0;

    static Result<Cylinder> TryMake(const Vector3<T>& start, const Vector3<T>& end, T radius) noexcept;
    bool IsValid() const noexcept;
    Vector3<T> Center() const noexcept;
};
```

- 형상은 축 선분 [start, end] 위 점에서 축에 수직인 거리 ≤ radius이고, 축 방향 투영이 [0, H](H = |end − start|) 안인 점의 집합(닫힌 집합)이다. 선분 × 원판의 곱집합이다.
- `TryMake`와 `IsValid`는 같은 규칙을 쓴다: 좌표 유한, 반경 유한·0 이상, 그리고 radius > 0이면 d = end − start의 정규화(`TryNormalize`)가 성공해야 한다. `TryMake`는 `-0` 반경을 `+0`으로 정리한다(`Capsule`과 같다). 값 생성자로 만든 무효 값은 모든 판정이 `InvalidInput`으로 거부한다.
- 축 단위 벡터 a는 판정 안에서 d를 정규화해 얻는다. 한 판정 안에서는 같은 a를 쓴다.

### 3.2 판정 목록

| 분류 | 함수 | 계산 방식 |
|---|---|---|
| 경계 | `TryBounds(cylinder)` → `Result<Box<T>>`(잘못된 입력과 좌표 overflow를 실패로 구분) | 축별 반폭 e_i = r·√(d_j² + d_k²) / \|d\|(i, j, k는 서로 다른 축)을 최대 성분으로 크기를 맞춰 계산한다. 반폭에 상대 여유를 곱하고 끝점 ± 반폭을 바깥쪽 한 ulp 보정한다(3.4절). radius 0이면 두 끝점의 AABB |
| 점 포함 | `ContainsPoint(cylinder, point)` | t = (p − s)·a ∈ [0, H], 수직 벡터 v = (p − s) − t·a에 대해 \|v\|² ≤ r² |
| 최근접점·거리 | `TryClosestPoint(cylinder, point)`, `TryDistanceSquared(cylinder, point)` | t를 [0, H]로 자르고, v의 길이를 r로 자른다(곱집합의 성분별 투영이라 끝면 모서리 영역에서도 정확) |
| 광선 | `Raycast(ray, cylinder, maxDistance)` → `RayHit` | 구간 교집합(3.5절) |
| 선분 | `SegmentCast(segment, cylinder)` → `SegmentHit` | 광선 판정을 선분 매개변수로 |
| 겹침 | `Overlap(cylinder, sphere)`, `Overlap(cylinder, plane)`, `Overlap(segment, cylinder)`과 반대 순서 | 구는 최근접점 거리² ≤ r_s², 평면은 support 반폭, 선분은 `SegmentCast` 적중 |
| 평면 분류 | `ClassifyPlaneSide(plane, cylinder)` | 두 끝면 중심의 부호 거리 최소·최대에 원판 support r·\|n × a\|를 빼고 더한 범위로 판정한다(수학적으로 중심 ± (H/2)·\|n·a\| + r·\|n × a\|와 같다. 중심 반올림이 없고 끝점이 평면 위면 거리가 정확히 0) |
| 볼록 영역 | `ClassifyContainment(volume, cylinder)`, `Overlap(volume, cylinder)`과 반대 순서 | 평면마다 위 support로 최저·최고 거리를 구해 Box·Sphere와 같은 규칙(밖은 엄격, 접촉은 교차, 안은 경계 포함) |

모든 겹침 판정은 양방향으로 둔다(기존 관례).

**Capsule 보강(같은 단계).** `ClassifyPlaneSide(plane, capsule)`, `ClassifyContainment(volume, capsule)`, `Overlap(volume, capsule)`, `Overlap(capsule, plane)`과 반대 순서를 더한다. support 반폭은 (H/2)·\|n·a\| + r이다(H = 0이면 r).

### 3.3 퇴화

- **원판(radius > 0이고 축 정규화 실패)**: 거부한다. 두 끝점 표현으로는 원판의 normal이 없기 때문이다. `TryNormalize`가 가장 큰 성분으로 먼저 나누므로 0이 아닌 유한 차이는 성분이 1e−200이어도 정규화된다(IEEE 점진적 언더플로에서 서로 다른 끝점의 차는 0이 되지 않는다). 그래서 실제로 거부되는 것은 `start == end`와 차의 overflow뿐이며, 아주 짧은 축은 유효하고 모든 계산을 크기 조정으로 안전하게 한다. 원판이 필요하면 결정 3의 대안 표현을 다시 본다.
- **선분·점(radius == 0)**: 허용한다. 축을 정규화하지 않고 `Capsule` radius 0과 같은 선분 경로로 계산한다(정규화 실패를 피함).
- 축이 아주 짧거나 반경이 아주 작은 경우는 일반 경로이며 3.4절 정밀도 계약을 따른다.

### 3.4 정밀도 계약

- 다른 Core 판정과 같이 닫힌 집합, 접촉 = Hit, 숨은 허용치 없음을 원칙으로 하고, "접촉 = Hit은 반올림 한계 안에서 성립한다"는 단서를 단다(Intersection.h 기존 판정 주석과 같은 문구).
- **`√(1 − x²)` 꼴을 쓰지 않는다.** 축 a의 성분이나 n·a가 ±1에 가까우면 정규화·내적 반올림으로 값이 정확히 1이 되어 정보가 사라지고, 1을 넘으면 NaN이 된다. 리뷰 반례: start = (0, 0, 0), end = (1000, 1e−6, 0), r = 1e4 cm이면 a_x = 1이 되어 x 반폭이 0으로 계산되지만 실제 반폭은 1e−5 cm다. `√((1 − x)(1 + x))`도 x가 이미 1로 반올림된 뒤라 막지 못한다.
- **경계 반폭**: e_i = r·√(d_j² + d_k²) / |d|. 구현은 가장 큰 성분 L로 나눈 비율로 계산해 성분 제곱의 underflow·overflow를 없앤다. d 성분의 뺄셈 오차는 결과 대비 ε 규모라 큰 좌표에서도 방향 오차가 ε 규모다. 계산 단계의 상대 오차 합은 약 12.5u(u = ε/2)이며, 반폭에 여유 M = 1 + 8ε(= 1 + 16u, 이름 있는 상수에서 유도)를 곱해 덮는다. 축 비율이 비정규 범위로 작아질 때의 절대 오차는 비율 하한(2 × 형식 최소 정규값)으로, 마지막 곱의 비정규 절대 오차는 반폭 자체의 `NextUp` 한 번으로 덮는다. 최종 min·max는 `NextDown`·`NextUp`으로 한 ulp 바깥쪽 보정한다(원점에서 10⁹ cm에서는 덧셈의 반 ulp 약 6e−8 cm만으로도 포함이 깨진다). 반폭이 정확히 0인 축은 끝점 좌표를 그대로 쓴다.
- **경계 포함은 계산으로 보장한다.** 표면 표본 점도 반올림되므로 표본 시험으로는 포함 위반을 검출할 수 없다. 시험은 정확 산술(정수·dyadic 좌표) oracle로 반폭 하한을 대조한다(3.6절).
- **support 반폭**: h_n = (H/2)·|n·a| + r·|n × a|. 구현은 두 끝면 중심의 부호 거리 최소·최대 ∓ r·|n × a|로 같은 값을 계산한다. 비단위 normal에도 정확한 support이고, 평행에 가까워도 소거가 없다(위 예에서 |n × a| = 1e−9). |n × a|는 성분 제곱 underflow를 막는 안정한 길이 계산으로 구한다.
- **수직 거리**: v = (p − s) − t·a(`Capsule` 코드 방식) 또는 |(p − s) × a|로 구한다. |p − s|² − t²은 긴 cylinder에서 소거로 망가진다(H = 1e9, r = 1이면 오차 약 100 > r²).
- **공간 인덱스와의 관계**: cylinder는 `LooseOctree`·`FlatBoundsSet`의 질의 형상이 아니므로 그쪽 보수성 논증의 대상이 아니다. 대신 `Bounds`가 공간 인덱스 등록 경계로 쓰이므로, 위 포함 보장이 컬링 누락을 막는 역할을 한다.

### 3.5 광선 판정

`Box`의 `CastBox`처럼 구간 교집합으로 계산한다. o는 원점, w는 방향, s0 = (o − start)·a, sw = w·a.

1. 축 slab: sw ≠ 0이면 [(0 − s0)/sw, (H − s0)/sw]를 정렬한 구간. sw = 0이면 0 ≤ s0 ≤ H일 때만 전체 구간, 아니면 Miss.
2. 옆면: 수직 성분 o⊥ = (o − start) − s0·a, w⊥ = w − sw·a에 대해 |o⊥ + t·w⊥|² ≤ r²의 2차식 구간. |w⊥|² = 0이면 |o⊥|² ≤ r²일 때만 전체 구간. 근은 `Capsule`·`Sphere`와 같은 안정 근 공식으로 구한다.
3. 진입 = max(두 하한, 0), 이탈 = min(두 상한, maxDistance). 진입 ≤ 이탈이면 Hit.

규칙:

- 축 평행 ray와 끝면 평면 안을 스치는 ray는 닫힌 집합이므로 Hit이다.
- 시작점이 안이면 `startedInside`이고 `ContainsPoint`와 같은 식으로 판정한다. 두 식의 반올림이 엇갈리면 시작 접촉으로 본다(`Capsule` 주석과 같다).
- 법선은 진입을 결정한 경계의 바깥쪽이다. 끝면과 옆면 진입 거리가 같은 모서리 동률은 끝면 법선을 쓴다.
- radius 0은 `Sphere` radius 0과 같이 정확히 지날 때만 Hit이고 법선은 −direction이다.

### 3.6 시험

- 독립 double oracle과 고정 seed 무작위 사례 대조(기존 수학 시험 관례: 조밀 표본, 해석해, SDF 근 찾기).
- **경계 포함**: 정확 산술 oracle(정수·dyadic 좌표로 반폭 하한 계산)과 대조. 고정 사례로 축이 좌표축과 거의 평행한 경우(리뷰 반례), 원점에서 10⁹ cm, 큰 반경.
- **support**: 꼭짓점·모서리를 지나는 평면, 거의 평행한 normal, 비단위 normal(허용 오차 안).
- 경계 사례: 축 평행·대각 축, −0 성분, 끝면·옆면·모서리 접촉, 선분·점 퇴화, 거부되는 원판(언더플로 포함), 아주 짧은 축과 아주 긴 축(수직 거리 소거), 시작점이 안·경계 위인 ray, maxDistance가 정확히 진입 거리, 끝면 안을 스치는 ray.
- 모든 겹침 쌍의 대칭. Capsule 보강 판정도 같은 기준.
- 입력 오류: 비유한·음수 반경, 무효 값 생성자 입력.
- 판정 사이 일관성: 거리 0 ⇔ `ContainsPoint`(반경 0 구 겹침 포함), Raycast `startedInside` ⇔ `ContainsPoint`(시작점), 최근접점이 형상 안.

**판정 사이 일관성 계약(CY1 리뷰 반영).**

- `ContainsPoint` Hit ⇔ `TryDistanceSquared` == 0 ⇔ `Overlap(cylinder, Sphere(p, 0))` Hit ⇔ 그 점에서 시작한 `Raycast`·`SegmentCast`의 `startedInside`. 모두 하나의 포함 식(축 좌표 두 부호 판정 + 수직 거리² ≤ r²)을 쓴다. 포함 식이 밖이면 거리²는 형식 최소 정규값(`NumericLimits<T>::Min()`) 이상으로 올려 0이 되지 않게 한다(실제 거리²가 그보다 작은 점은 결과가 실제보다 크다).
- radius 0의 포함 판정은 `CastCapsule` 시작 판정과 같은 두 식(선분 거리² ≤ r², 또는 수직 성분 판정 + 축 범위)이다. 그래서 `ContainsPoint(Capsule radius 0)`(선분 거리 식 하나)과는 반올림 경계에서 다를 수 있다.
- `TryClosestPoint`는 자른 점이 포함 식 밖이면 안쪽 기준점(중심, 중심이 밖이면 start)과의 이분 탐색(상한 64단계, 이름 있는 상수)으로 안으로 판단되는 가장 바깥 점을 돌려준다. radius > 0이면 결과에 대한 `ContainsPoint`가 항상 Hit이다. radius 0은 선분 위 근사점이다.
- 밖에서 출발했는데 진입 거리가 0으로 언더플로하면 거리 0 접촉(`startedInside` 아님)이다.
- 접선에 가까운 옆면 진입의 법선은 근사다(normal·direction이 1e−9 … 7e−8 정도 양수일 수 있다).
- 시험의 정확 산술과 기하 oracle 도우미는 공용 시험 지원(`Core/Source/Developer/TestSupport/Public/TestSupport/ExactArithmetic.h`, `GeometryOracles.h`)으로 옮겼고 기존 Intersection 시험도 이를 쓴다.

**CY1 검증(2026-11-19).** 구현 시험 13개(float·double)에 더해 독립 리뷰가 Python 정확 산술(`Fraction`, 90자리 `Decimal`) 판정기로 대조했다: `TryBounds` 포함 double 30만 + float 20만 cylinder(축 117만 개, 축 성분 1e−6 … 4e−320 비정규, 좌표 1e−3 … 1e9) 위반 0, Raycast 17만 ray(축 평행·접선·끝면 위 시작·maxDistance = 진입 거리) 누락 0, 평면 분류 여유 밖 불일치 0. 판정 사이 일관성 결함(거리 0인데 포함 Miss, 반경 0 시작점, 최근접점의 포함)은 리뷰에서 찾아 고쳤다.

## 4. 물리 어댑터(CY2)

물리는 아직 구현되지 않았으므로 이 절은 계약이다. 구현은 물리 어댑터 단계에서 한다.

### 4.1 형상 사상

- 에셋 단순 충돌 형상(UE `FKAggregateGeom` 대응)에 cylinder 원소를 더한다. 원소는 로컬 중심·회전·반높이·반경을 가지며 축은 로컬 +Z다(Rocks 캡슐과 같다, 물리 어댑터 경계 4절).
- 씬 질의 형상(UE `FCollisionShape` 대응)을 Line, Box, Sphere, Capsule, Cylinder로 둔다.
- Jolt 사상: 물리 어댑터 경계의 기준 사상 A가 Rocks +Z를 Jolt +Y로 옮기는 경우(J2·J6 결과에 따름)에만 형상 회전이 필요 없다. 아니면 고정 회전으로 감싼다(물리 어댑터 경계 2·4절 문구와 같다). `Cylinder{start, end, radius}`는 중심 (start + end)/2, 반높이 |end − start|/2, 축 회전으로 바꾸고, 단위는 어댑터 경계에서 cm → m.
- scale: Jolt의 XZ 균일 조건은 사상 A를 거치면 Rocks의 축 수직 두 축(XY) 균일이다. 위반하면 쿠킹이 거부한다. 음수(거울) scale은 J8 결과를 따른다.
- 반높이·반경이 Jolt 기본 convex radius(문서상 기본값, 크기는 J12)보다 작은 cylinder는 J18 결과에 따라 convex radius를 줄이거나 쿠킹이 거부한다.

### 4.2 convex radius 정책

- 질의 형상과 trigger(sensor)는 convex radius 0으로 만들어 판정을 정확하게 한다.
- 정적·동적 body의 충돌 형상은 Jolt 기본 convex radius를 쓴다. 이때 body는 지정한 cylinder보다 모서리만큼 안쪽에 있으므로, Core `Bounds`·컬링은 보수적으로 남는다. 둥근 모서리만큼 형상이 달라짐을 에셋 도구에 표시한다.
- 같은 cylinder라도 질의 형상(convex radius 0)과 body 형상(기본값)이 달라, sweep 결과와 실제 접촉이 모서리 근처에서 어긋날 수 있다. 이 차이를 어댑터 문서에 적는다.

### 4.3 안정성 경고

동적 rigid body에 cylinder 충돌 형상을 쓰면 에셋 검증이 경고를 낸다(Jolt 문서의 안정성 주의). 굴러가는 바퀴처럼 꼭 필요한 경우는 허용하되, 캡슐·convex로 대신할 수 있으면 그쪽을 권한다.

### 4.4 검증 항목(물리 어댑터 경계 8절 표에 추가)

| ID | 확인할 것 |
|---|---|
| J16 | `NarrowPhaseQuery::CastShape`·`CollideShape`가 cylinder를 질의 형상으로 받는지, 결과 법선·거리 의미(J11과 연동), cylinder 대 mesh·heightfield의 활성 모서리 처리 |
| J17 | `RayCylinder`의 정확성(끝면·모서리·시작점이 안인 경우), convex radius가 ray 판정에 주는 영향 |
| J18 | 반높이·반경과 convex radius의 관계(잘림인지 거부인지), 0 근처 동작, 얇은 cylinder 접촉 품질(v5.3.0 이후), convex radius 0 sensor의 침투 깊이 안정성 |
| J19 | 질량·관성 계산식과 단위 변환, 부력(`GetSubmergedVolume`) 근사, 캐릭터(`CharacterVirtual`) 형상으로 쓸 수 있는지 |

## 5. 단계

| 단계 | 내용 | 시점 | 완료 기준 |
|---|---|---|---|
| CY0 | 이 문서 | 지금 | 교차검증 반영(개정 2) |
| CY1 | Core `Cylinder<T>`와 3.2절 판정, Capsule 보강, 시험, `Intersection.h` 기능별 분리 — **구현**(2026-11-19) | 지금(별도 브랜치 `feature/cylinder-shape`) | 시험 통과, Foundation·format·policy, 수학 구현 2.1절·Core 상세 명세 G04·G05·G06 갱신 |
| CY2 | 물리 어댑터 cylinder 충돌·질의 형상 | 물리 어댑터 구현 단계 | J16–J19 확인, 어댑터 시험 |

## 6. 결정 사항

1. **일반 볼록 판정(GJK 등)의 Core 도입.** 결정(권장안, 2026-11-19): CY1에 넣지 않는다. cylinder 대 Box·OrientedBox·Capsule·Cylinder·Triangle 겹침은 유한 분리축으로 끝나지 않고, 선분·캡슐 대 cylinder 거리와 구 sweep은 4차식이다. 게임플레이 형상 질의는 Jolt가 담당하므로(전체 구현 계획 10.4절) Core에서 당장 필요하지 않다. Core 소비자가 생길 때 반복 판정의 종료 조건과 허용치 계약을 따로 설계한다.
2. **테이퍼드 cylinder.** 결정(권장안): 요구가 생길 때 같은 표현(두 끝면 중심 + 반경 둘)으로 Core와 어댑터에 더한다.
3. **표현과 원판.** 결정(권장안): `{start, end, radius}`를 유지하고 원판을 거부한다. 대안 `{center, unitAxis, halfHeight, radius}`(Jolt·`OrientedBox` 쪽 표현)는 원판을 유효한 형상으로 만들고 판정마다 다시 정규화하지 않는 장점이 있으나, 단위 축 허용치를 계약에 넣어야 하고 `Capsule`과 표현이 달라진다. 원판 형상이 필요해지면 이 결정을 다시 본다.

## 7. 미확인 사항

- Jolt `RayCylinder` 본문, 질의 경로의 cylinder 지원 세부, convex radius와 크기의 관계, master와 v5.7.0 태그의 일치(J16–J19).
- UE에서 Chaos cylinder를 엔진 충돌 에셋으로 만드는 경로는 찾지 못했다(직렬화 경로만 확인).

## 8. 파일 배치

`Intersection.h`는 기능별 헤더의 묶음 헤더다(2026-11-19 분리, 동작 불변 이동): `IntersectionTypes.h`(결과 형식·공통 계약), `IntersectionDetail.h`·`IntersectionCastDetail.h`·`IntersectionCylinderDetail.h`(내부 구현), `IntersectionCast.h`(광선·선분), `IntersectionContainment.h`(점 포함·평면 분류·볼록 영역), `IntersectionOverlap.h`(겹침), `IntersectionDistance.h`(최근접점·거리), `ShapeBounds.h`(경계). 각 헤더는 단독 include로 컴파일된다.

## 9. 개정 이력

- 개정 1(2026-11-19): 초안.
- 개정 2(2026-11-19): 교차검증(치명 2, 중요 6, 경미 6) 반영. 경계 반폭과 support를 소거 없는 꼴로 바꾸고 경계 포함을 계산(상대 여유 + 한 ulp 바깥 보정)과 정확 산술 시험으로 보장하게 했다. 퇴화 규칙(radius > 0이면 축 정규화 필수, radius 0은 선분 경로)과 `IsValid`·`TryMake` 일치, 수직 거리 계산 꼴, 광선 구간 교집합 알고리즘과 규칙, 평면·선분 겹침, Capsule 평면·볼록 영역 판정 보강, UE 인용 정정(`FCylinder`, `#if 0` 블록, 보수적 bounds), 물리 계약 조건부 문구와 J18 수정·J19 추가, 결정 1–3 확정.
- 개정 3(2026-11-19): CY1 구현을 반영했다. `TryBounds`(Result 반환)와 크기 조정·비정규 보정을 포함한 반폭 오차 예산, 끝면 중심 거리 기반 support, 원판 거부가 실제로는 `start == end`와 overflow뿐이라는 점, 판정 사이 일관성 시험, 정확 산술 독립 검증 결과, `Intersection.h` 파일 배치(8절)를 적었다.
- 개정 4(2026-11-19): CY1 리뷰 반영 결과로 판정 사이 일관성 계약, radius 0 포함 식, 최근접점 안쪽 보정, 언더플로 진입, 접선 법선 근사, 공용 시험 도구 위치를 3.6절에 적었다.
