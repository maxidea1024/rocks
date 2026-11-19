# Rocks 평면 경계 판정(FlatBoundsSet) 설계

기준일: 2026-11-19, 개정 3.

이 문서는 매 프레임 뷰 가시성 판정에 쓰는 평면 SoA 경계 집합 `FlatBoundsSet`의 설계를 정한다. 문서 전체가 설계이며 구현된 항목은 없다. 수치는 측정 전 제안값이다.

- 상위 계약: [전체 구현 계획](EngineImplementationPlan.ko.md) 10.4절(소비자 표 1행 "RenderScene의 평면 SoA bounds", 계층 공간 해시 규약 1·6·7·8, 가시성 보수성, 수용 시험 SI01·SI06).
- 같은 모듈의 다른 구조: [공간 인덱스 설계](SpatialIndex.ko.md)(`LooseOctree`). 공통 규칙(핸들, 읽기·변경 단계, 오류, 실패 시 상태)은 그 문서를 따르고 여기서는 다른 점만 적는다.
- 착수: 사용자 결정(2026-11-19). LooseOctree와 같은 별도 브랜치 `feature/spatial-index`에서 진행한다. 소비자(RenderScene)는 화면 마일스톤 뒤에 생기므로 이번 구현은 도구를 미리 만들어 두는 단계다. 메인 줄의 순서는 바꾸지 않는다.

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 구조 | 트리 없이 경계를 조밀한 SoA page에 두고 뷰마다 전수 판정한다 | UE 5.8.4 프러스텀 컬링의 기본 경로(2절), 10.4절 소비자 표 1행 |
| 위치 | `Engine/Source/Runtime/SpatialIndex`, 네임스페이스 `rocks::spatial` | 10.4절 "공통 해시·평면 판정 코드는 SpatialIndex 모듈". Core는 가볍게 둔다 |
| 정밀도 | 204,800 cm 타일마다 page를 두고 타일 원점 기준 상대 float로 저장한다. 평면은 page마다 double에서 상대 float로 옮기고 6.2절의 오차 여유 δ만큼 넓혀 판정한다 | 10.4절 규약 7. SIMD는 float 4-lane(`Register4f`)뿐이다 |
| 보수성 | 정답 술어로 보이는 원소는 반드시 보인다(누락 0). δ 띠 안의 초과는 허용한다. float 판정은 정확한(실수) 교차 집합도 포함한다 | 13.1절 V_ref ⊆ V_submit, SI06 |
| 큰 원소·범위 밖 | 타일 상대 범위를 넘는 큰 원소와 타일 key 범위 밖 원소는 별도 double 목록에서 항상 판정한다 | page의 δ가 큰 원소 하나로 커지는 것을 막는다. 10.4절 규약 1·6 |
| 거리 판정 | UE 기본값(`r.DistanceCullToSphereEdge`)과 같은 방향: 먼 쪽은 AABB까지 최근접 거리, 가까운 쪽은 가장 먼 꼭짓점 거리 | 큰 원소가 중심 거리로 일찍 사라지지 않게 한다 |
| 결과 | 뷰마다 원소 슬롯당 1 bit인 가시성 비트 집합 + 결과 세대 | UE 뷰별 `PrimitiveVisibilityMap`과 같은 형태 |
| 슬롯 변경 | 변경 함수가 옮겨진 원소의 (핸들, 새 슬롯)을 반환한다. 낡은 비트 집합은 세대로 거부한다 | UE `SetElementId` 콜백 대신 명시적 반환 |
| page 크기 | 64의 배수 설정값, 초기 64(word 1개) | 희소한 월드에서 타일당 메모리와 비트 집합을 줄인다. FB2에서 64·256·1,024 비교 |
| 병렬 | page 단위로 `ParallelFor`에 나눈다 | page마다 출력 word가 따로라 쓰기 경합이 없다 |
| 동시성 | 변경 단계·읽기 단계 분리, `AccessOverlapChecker` | SpatialIndex.ko.md 6.9절 |

## 2. 참조: UE 5.8.4의 프러스텀 컬링

UE 5.8.4(commit `f45729c0b6f0`)에서 확인한 사실이다. 경로는 `Engine/Source/Runtime/` 기준이며 소스 읽기로 얻은 사실이다.

- 기본 경로는 `Scene.PrimitiveBounds` 선형 배열을 뷰별 `PrimitiveVisibilityMap` 비트 배열의 32-bit word 단위로 판정하고 `ParallelFor`로 task를 나눈다. 옥트리 경로는 `r.Visibility.FrustumCull.UseOctree` 기본 꺼짐이다(Renderer/Private/SceneVisibility.cpp:343-349, 860-924, 4043-4050).
- 평면 8개일 때 `IntersectBox8Plane`이 순열 평면 8개를 4개씩 SoA로 읽어 앞 4평면에서 조기 탈출한다(:531-589, 3912). SIMD는 primitive 하나 대 평면 여러 개 단위다.
- task 수는 worker 수(최대 16) × 2이고, 렌더 스레드 단독이면 128 word 단위다(:3619-3626, 3655-3672).
- "항상 보이는" 프록시 타입은 배열 뒤쪽에 모으고 시작을 32의 배수로 맞춰 판정 루프에서 뺀다(:3596-3606, RendererScene.cpp:6379-6435).
- `PrimitiveBounds`는 BoxSphere bounds와 Min/MaxDrawDistance·MaxCullDistance를 담는다(ScenePrivate.h:1228-1239).
- 거리 판정은 `r.DistanceCullToSphereEdge`(기본 true, SceneVisibility.cpp:67-73)일 때 먼 쪽을 `(Dist − SphereRadius)²`, 가까운 쪽을 가장 먼 거리로 판정한다(:720-727, 1002-1025). `MaxDrawDistanceScale`, 페이드 반경, HLOD `MaxCullDistance`도 있다.
- 원소 제거는 병렬 배열 교환과 축소 없는 제거로 조밀하게 유지한다(RendererScene.cpp:5716-5785).

Rocks가 다르게 가는 점:

- SIMD 축을 원소 4개 대 평면 하나로 둔다(SoA 4-lane). OT5에서 같은 float SoA 데이터의 박스 판정이 SIMD로 3–5배 빨랐다([OT5 보고서](../Benchmarks/SpatialIndex/2026-11-18/report.ko.md) 1절, A2 대 A2s). UE 방식은 FB2 변형으로 비교한다.
- 월드 좌표가 double(cm)이므로 타일 상대 float를 쓴다.
- 거리 판정은 구 대신 AABB 거리를 쓴다(구보다 덜 넓다). 페이드는 범위 밖이다.

## 3. 책임과 범위

**하는 것**: 원소의 월드 AABB, 필터 마스크, 그리기 거리 범위, 항상 보임 여부, 사용자 값을 저장하고, 뷰(볼록 영역, 거리 기준점, 거리 배율, 필터)마다 보이는 원소의 비트 집합을 만든다.

**하지 않는 것**: LOD 선택, 오클루전(HZB), 페이드, 재질·그림자 정책, 정밀 형상 판정, 영역 질의(`LooseOctree`), 정적·대량 instance(계층 공간 해시). 비유한 좌표는 거부하며, 가시성 보수성이 필요한 소비자(RenderScene)가 자기 격리 목록에서 판정한다(SpatialIndex.ko.md 결정 4와 같다).

## 4. 공개 API(스케치)

```cpp
namespace rocks::spatial {

struct FlatBoundsHandle {
    SlotHandle slot;
};

struct FlatBoundsDesc {
    Box<double> bounds;      // 월드 AABB(cm). Box::IsValid()
    uint32 filterMask;       // 뷰 mask와 AND가 0이면 보이지 않는다
    float minDrawDistance;   // cm, 0이면 하한 없음. 유한·0 이상
    float maxDrawDistance;   // cm, 0이면 상한 없음. 유한·0 이상, 0이 아니면 min ≤ max
    bool alwaysVisible;      // 공간·거리 판정 없이 필터만 본다. 경계는 그래도 유효해야 한다
    uint64 userValue;
};

struct FlatBoundsView {
    ConvexVolume<double> volume;  // 안쪽 normal 평면(프러스텀 등)
    Vec3d distanceOrigin;         // 그리기 거리 기준점
    float distanceScale;          // 그리기 거리 배율(UE MaxDrawDistanceScale 대응), 유한·0 초과
    uint32 filterMask;            // 0이면 빈 결과(오류 아님)
};

struct FlatBoundsMove {           // 변경 함수가 옮긴 원소
    FlatBoundsHandle handle;
    uint32 newSlot;               // 큰 원소·범위 밖 목록이면 FlatBoundsListSlot 영역 상수
};

struct FlatBoundsMoves {          // 한 변경 호출이 옮긴 원소(최대 2개)
    FlatBoundsMove moves[2];
    uint32 count;
};

struct FlatVisibility {           // 호출자 소유 출력
    Span<uint64> words;           // WordCount() 이상
    uint64 epoch;                 // QueryViews가 쓴다
};

class FlatBoundsSet {
public:
    [[nodiscard]] static Result<FlatBoundsSet> TryCreate(const FlatBoundsConfig& config,
                                                         Allocator& allocator) noexcept;
    // 변경 단계. 옮겨진 원소가 있으면 moves에 담는다(nullptr이면 담지 않는다)
    [[nodiscard]] Result<FlatBoundsHandle> TryAdd(const FlatBoundsDesc& desc, FlatBoundsMoves* moves = nullptr) noexcept;
    [[nodiscard]] Result<void> TryRemove(FlatBoundsHandle handle, FlatBoundsMoves* moves = nullptr) noexcept;
    [[nodiscard]] Result<void> TryUpdateBounds(FlatBoundsHandle handle, const Box<double>& bounds,
                                               FlatBoundsMoves* moves = nullptr) noexcept;
    [[nodiscard]] Result<void> TrySetFilter(FlatBoundsHandle handle, uint32 filterMask) noexcept;
    [[nodiscard]] Result<void> TrySetDrawDistance(FlatBoundsHandle handle, float minDistance, float maxDistance) noexcept;
    [[nodiscard]] Result<void> TrySetAlwaysVisible(FlatBoundsHandle handle, bool alwaysVisible) noexcept;
    [[nodiscard]] Result<void> TrySetUserValue(FlatBoundsHandle handle, uint64 userValue) noexcept;
    [[nodiscard]] Result<void> TryReserve(uint32 count) noexcept;
    void Clear() noexcept;
};

class FlatBoundsReader {  // FlatBoundsReadScope에서만 얻는다
public:
    [[nodiscard]] uint32 SlotCapacity() const noexcept;
    [[nodiscard]] uint32 WordCount() const noexcept;
    [[nodiscard]] Result<void> QueryViews(Span<const FlatBoundsView> views, Span<FlatVisibility> out,
                                          WorkerScheduler* scheduler, Allocator& scratch,
                                          FlatQueryStats* stats = nullptr) const noexcept;
    template <typename Visitor>
    [[nodiscard]] Result<void> ForEachVisible(const FlatVisibility& bits, uint32 viewIndex, Visitor&& visitor) const noexcept;
    [[nodiscard]] Result<uint64> TryUserValueAt(uint32 slot) const noexcept;
    [[nodiscard]] Result<uint32> TrySlotOf(FlatBoundsHandle handle) const noexcept;
    [[nodiscard]] bool IsValid(FlatBoundsHandle handle) const noexcept;
    [[nodiscard]] Result<FlatBoundsDesc> TryGet(FlatBoundsHandle handle) const noexcept;
    [[nodiscard]] uint32 Count() const noexcept;
    [[nodiscard]] FlatBoundsStats Stats() const noexcept;
    [[nodiscard]] Result<FlatBoundsValidationReport> Validate() const noexcept;
};

} // namespace rocks::spatial
```

계약은 다음과 같다.

- **슬롯.** page 원소의 전역 슬롯은 `page 색인 × pageCapacity + page 안 위치`다. 비트 집합은 이 슬롯으로 색인한다. 큰 원소·범위 밖 목록의 원소는 비트 집합 뒤쪽의 목록 영역(page 슬롯 뒤에 이어지는 슬롯)에 둔다. `SlotCapacity()`는 이 둘을 합한 크기다.
- **살아 있지 않은 슬롯의 비트는 항상 0이다.** `QueryViews`는 해제된 page의 word, 사용 수 뒤의 꼬리, 마지막 word의 남는 bit를 모두 0으로 쓴다.
- **슬롯 이동.** 삭제(같은 타일 마지막 원소가 빈자리로 옴), 타일 이동, 목록 이동은 원소의 슬롯을 바꾼다. 변경 함수는 그 호출이 옮긴 원소를 `FlatBoundsMoves`로 돌려준다(최대 2개: 자기 자신과 빈자리를 채운 원소). 소비자는 이것으로 자기 배열을 고친다. 슬롯 변경 알림 콜백은 두지 않는다.
- **결과 세대.** 집합은 변경마다 프로세스 전역 단조 계수(LooseOctree의 `NextMutationEpoch`과 같은 방식)에서 새 세대를 받는다. `QueryViews`는 결과에 그 세대를 쓰고, `ForEachVisible`은 세대가 다르면 `StaleHandle`을 돌려준다. 이전 읽기 단계의 비트 집합을 다음 단계에서 잘못 해석하는 것을 막는다.
- **`QueryViews` 입력 오류**: 유효하지 않은 `ConvexVolume`, 비유한 `distanceOrigin`, `distanceScale`이 유한 양수가 아님, `views`와 `out` 개수 불일치, `out[i].words`가 `WordCount()`보다 짧음, `out` 버퍼끼리 겹침 → `InvalidArgument`, 출력 불변. 뷰 0개는 성공(no-op).
- **`scheduler`가 nullptr**이거나 worker 안에서 부르거나 helper 할당이 실패하면 호출 스레드가 전부 처리한다(`ParallelFor` 계약). 결과는 병렬 여부와 무관하게 같다. 질의가 쓰는 할당은 `scratch`뿐이며, `scratch`는 여러 스레드의 해제를 받을 수 있어야 한다.
- **범위 밖 슬롯**: `TryUserValueAt`은 살아 있지 않거나 범위 밖 슬롯에 `NotFound`를 돌려준다.
- **변경 오류·실패 시 상태·이동·수명**은 SpatialIndex.ko.md 6.1절의 LooseOctree 규칙과 같다. 그리기 거리 입력(NaN·음수·Inf·min > max)은 `InvalidArgument`다.

## 5. 저장 구조

**타일.** 타일 한 변은 `tileSize`(초기 204,800 cm, 10.4절 규약 7). 타일 key는 AABB 중심 c의 `floor((c − anchor) / tileSize)`를 축별로 한 값이다(0 방향 절삭 금지, 10.4절 규약 1). key가 int32 범위를 넘으면 그 원소는 범위 밖 목록으로 간다. 타일 원점(key × tileSize + anchor)은 double로 정확하다.

**큰 원소 판정.** AABB의 타일 상대 좌표 최대 절댓값이 `largeRelativeExtent`(초기 2 × tileSize)를 넘으면 큰 원소 목록에 둔다. 그래서 page의 상대 좌표 최대 절댓값 R은 이 값으로 묶이고, δ가 큰 원소 하나 때문에 page 전체에서 커지지 않는다. 10.4절 규약 6의 최상위 level 초과 장대 대상도 여기로 온다.

**page.** 타일 하나는 page 하나 이상을 가진다. `pageCapacity`는 64의 배수(초기 64, `TryCreate`가 검사)다.

| 배열 | 형식 | 비고 |
|---|---|---|
| minX·minY·minZ·maxX·maxY·maxZ | float × capacity | 타일 상대 좌표. double 뺄셈 결과를 float로 바깥쪽 반올림한 뒤 한 float ulp 더 바깥으로 보정한다(LooseOctree 6.3절과 같다) |
| minDraw·maxDraw | float × capacity | 그리기 거리(cm)를 desc 값 그대로 둔다(상한 없음은 +Inf). 여유는 6.2절 거리 한계식이 질의 때 더한다 |
| filter | uint32 × capacity | 항상 보임은 별도 bit 배열 또는 filter의 예약 bit로 둔다(구현에서 정한다) |
| record | uint32 × capacity | 레코드 슬롯 색인 |
| userValue | uint64 × capacity | 읽기 단계 출력용 |
| 머리 | 타일 key, 타일 원점(double), 사용 수, R(저장된 float 상대 좌표의 최대 절댓값) | R은 6.2절에 쓴다 |

- 중심 배열은 두지 않는다(거리 판정은 min·max만 쓴다).
- 원소당 page 데이터는 48 byte(경계 24, 거리 8, 필터 4, record 4, 사용자 값 8)다. 레코드(`SlotArray<FlatRecord>`: double AABB, page·위치, 거리, 필터, 항상 보임, 사용자 값)가 따로 있다.
- page는 색인 풀(`IndexPool`)에 둔다. 타일 → page 목록 대응은 변경 단계에서만 `HashMap`으로 찾고, 읽기 단계는 page 배열을 순서대로 훑는다(10.4절 규약 8).
- 같은 타일의 page는 가득 찬 page가 앞에 오도록 유지한다. 삭제는 그 타일 마지막 page의 마지막 원소로 빈자리를 채운다(swap-remove). 비게 된 page는 풀에 돌려준다.
- R은 원소가 들어올 때 늘린다. 줄이는 것은 page가 비거나 유지보수 재계산 때만 한다(크게 남는 것은 보수적이라 허용).

**큰 원소·범위 밖 목록.** double AABB와 거리·필터·사용자 값을 조밀한 SoA로 둔다. 질의마다 double로 판정한다(SIMD 없음). 목록 크기는 통계로 보고하고 FB2에서 비용을 잰다.

## 6. 판정

### 6.1 술어(정답 기준)

원소 e와 뷰 v에 대해 다음이 모두 참이면 보인다.

1. `(e.filter & v.filterMask) != 0`.
2. `e.alwaysVisible`이거나 3·4가 참.
3. 볼록 영역: `Overlap(v.volume, e.bounds)`(Core 술어, 어느 평면에 대해서도 엄격히 바깥이 아님).
4. 거리: 배율을 적용한 하한·상한 m = minDraw · scale, M = maxDraw · scale에 대해, M이 무제한이 아니면 AABB까지 최근접 거리² ≤ M², m이 0이 아니면 가장 먼 꼭짓점까지 거리² ≥ m².

정답 기준은 이 술어를 정확한 double AABB로 계산한 결과다. float 판정은 이 기준과 정확한(실수) 교차 집합을 모두 포함한다.

### 6.2 float 판정과 보수성

Core 평면 규약 `SignedDistance(p) = n·p − d`(안쪽이 양수)를 쓴다. 타일 원점 o에 대해 p = o + r이면 `n·r − (d − n·o)`이므로, page마다 w′ = n·o − d를 double로 계산해 float로 옮긴다. 원소 4개를 한 레지스터로 묶어, 평면마다 n_f 성분 부호로 min/max 배열을 골라 가장 안쪽 꼭짓점의 S_f = n_f·x + w′_f를 float로 계산하고, S_f < −δ이면 그 평면에서 바깥이다.

**오차 상한.** u = 2⁻²⁴, ε = 2⁻⁵³, R은 page에 저장된 float 상대 좌표의 최대 절댓값이다.

| 오차 | 원인 | 상한 |
|---|---|---|
| normal의 float 변환 | n → n_f | u·‖n‖₁·R |
| w′의 float 변환 | w′ → w′_f | u·\|w′\| |
| float 내적 4항 | 반올림 | γ₄·(‖n_f‖₁·R + \|w′_f\|) |
| w′의 double 계산 | n·o − d의 소거 | γ₄(double)·(‖n‖₁·‖o‖∞ + \|d\|) |
| 정답 술어의 반올림 | Core `Overlap`의 double 계산 | γ₄(double)·(‖n‖₁·(‖o‖∞ + R) + \|d\|) |
| 저장 경계 | double 뺄셈의 최근접 반올림 | 한 ulp 바깥 보정으로 덮음(5절) |

**δ 식(제안).** δ = 8u·(‖n‖₁·R + |w′|) + 12ε·(‖n‖₁·(‖o‖∞ + R) + |d|) + 4·FLT_MIN·(‖n‖₁ + 1). double로 계산한 뒤 float로 위쪽 반올림한다.

- 첫 항은 float 쪽 오차다(이론값 5u + 2차항에 여유).
- 둘째 항은 double 쪽 오차다. 타일이 원점에서 멀면 w′의 소거 오차와 정답 술어의 반올림 오차가 R·|w′|와 무관하게 커지므로 반드시 필요하다. 리뷰 probe에서 이 항이 없으면 원점에서 10⁹ cm 떨어진 타일의 꼭짓점 접촉 200만 건 중 222,907건이 누락되었고, 이 항을 넣으면 0건이었다.
- 셋째 항은 FTZ·DAZ 하한이다. 비정규 값이 0이 되는 오차는 FLT_MIN 규모인데, δ가 비정규면 δ 자체가 0이 되기 때문이다.
- FMA 사용 여부와 무관하게 성립한다.

**증명 요지.** 저장 경계 B_f는 B − o를 포함하므로, 정확한 최댓값 max_B n·r는 n_f 부호로 고른 꼭짓점의 n_f·x_f에 normal 변환 오차를 더한 값 이하다. 정답 술어가 바깥이 아니면(S_core ≥ 0) 정확한 값은 −E_core 이상이고, 위 표의 오차를 차례로 더하면 S_f ≥ −Σ오차다. δ ≥ Σ오차이면 "float가 바깥 ⇒ 정답도 바깥"이다. 같은 논증으로 float 판정은 실수 교차 집합도 포함한다. 그래서 Core 술어가 꼭짓점 평면에서 정확한 접촉을 놓치는 사례(SpatialIndex.ko.md 6.8절)를 float 경로는 물려받지 않는다.

**거리 여유.** 계산 오차는 최근접 거리²에 대해 d²(1 ± ~9.5u) ± 3.5u·R·d ± 3u²R²(기준점의 float 변환 오차, 차·제곱·합의 반올림)이고 최원 거리도 같다. 구현(FB1)은 t = 거리 한도 × 배율로 상한 판정을 t·(t·(1 + 2⁻¹⁹) + 17u·R) + c, 하한 판정을 t·(t·(1 − 2⁻¹⁹) − 17u·R) − c와 비교한다(c = 8u²R² + 64·FLT_MIN). c의 8u²R² 항은 d ≈ 0에서 제곱 오차를 덮으려고 더했다. 하한 식은 D ≥ 1.75u·R에서 단조 증가하므로 그 아래 영역을 c가 덮는다. 상대 여유(c·u·d²)만 두면 뷰 근처의 하한 판정에서 누락이 생긴다. 배율이 커서 s²·M²가 float를 넘으면 상한을 무제한으로 본다(보수적).

**초과 띠.** 초과 원소는 어떤 평면에서 −δ ≤ S_f < 0이거나, 거리 판정이 여유 띠 안에 있는 원소다. 시험은 이 정의로 초과를 판정한다.

### 6.3 순회와 병렬

- 판정 단위는 page다. `ParallelFor`(Core `Task/ParallelFor.h`)로 page 색인 전체(해제된 page 포함 — 그 word를 0으로 쓰기 위해)를 나눈다. page의 출력 word는 그 page의 슬롯 범위이고 pageCapacity가 64의 배수이므로 page마다 겹치지 않는다. 목록 영역은 별도 작업 하나가 맡는다.
- page 하나를 맡으면 뷰마다: 평면을 상대 float로 옮기고 δ를 계산하고(page당 뷰 수 × 평면 수 번), 원소 4개씩 필터 → 항상 보임 → 평면 판정(모든 lane이 바깥이면 다음 묶음) → 거리 판정 순으로 계산해 bit를 쓴다.
- page 수가 적으면(한 타일에 원소가 몰림) 병렬성이 낮다. (page, word 묶음) 단위 분할은 FB2 측정으로 필요성을 본다.
- 읽기 범위는 호출 스레드에서 한 번 열면 된다. `AccessOverlapChecker`는 객체 단위 계수라 worker가 따로 열지 않는다.

## 7. 변경

- **삽입**: 큰 원소·범위 밖 판정 → 목록이면 목록 끝에 붙인다. 아니면 중심 타일을 찾고(없으면 등록), 그 타일 마지막 page에 자리가 없으면 page를 먼저 확보한 뒤 끝에 붙인다. 실패하면 상태 불변.
- **삭제**: 레코드 → (page, 위치). 그 타일 마지막 page의 마지막 원소를 빈자리로 옮기고 레코드를 고치고 `FlatBoundsMoves`에 담는다. 마지막 page가 비면 풀에 돌려주고, 타일의 page가 없어지면 타일 등록을 지운다. 목록 원소도 같은 방식(swap-remove)이다.
- **경계 갱신**: 같은 타일이고 목록 여부가 같으면 그 자리의 상대 float만 다시 쓴다(R 갱신 포함). 그 밖에는 새 자리를 먼저 확보한 뒤 옛 자리에서 삭제하고 새 자리에 쓴다. 옮겨진 원소(자기 자신과 빈자리를 채운 원소)를 돌려준다.
- 필터·그리기 거리·항상 보임·사용자 값 변경은 그 자리만 고친다.

## 8. 동시성

LooseOctree와 같다(SpatialIndex.ko.md 6.9절). `FlatBoundsReadScope`가 `AccessOverlapChecker`의 읽기 범위를 열고 `FlatBoundsReader`를 준다. 모든 변경 함수는 변경 범위를 연다.

## 9. 시험

| ID | 시험 | 판정 |
|---|---|---|
| FB01 | 정답 대조: 무작위 원소(분포 여러 개, 크기 편차, 타일 경계 걸침)와 무작위 뷰(프러스텀 6·8평면, 축 평행, −0 성분, 퇴화), 필터·항상 보임·거리·배율 조합, 뷰 mask 0 | 누락 0, 초과는 6.2절 초과 띠 안 |
| FB02 | 정밀도 경계: 원점에서 10⁹ cm 타일, 타일 원점 근처 작은 원소(R 작음) + 꼭짓점을 지나는 평면, 음수 key, 타일 경계 ±1 ulp 중심, 큰 원소 경계값, key 넘침 | FB01 기준 |
| FB03 | 거리 경계: d가 정확히 m·M, 뷰 근처 하한(d 작음), 배율, 입력 오류(NaN·음수·Inf·min > max) | 누락 0, 정해진 오류 |
| FB04 | 변경: 삽입·삭제·같은 타일 갱신·타일 이동·목록 이동, `FlatBoundsMoves`가 실제 이동과 일치, 레코드–슬롯 일치, 빈 page 반환, `Validate()` | 위반 0 |
| FB05 | 출력 계약: 해제 page·꼬리 bit 0, 세대 낡음 → `StaleHandle`, out 짧음·겹침·개수 불일치, 뷰 0개 | 계약과 일치 |
| FB06 | 다중 뷰·병렬: 여러 뷰 = 뷰별 단독, scheduler 있음 = 없음 | 비트 단위 일치 |
| FB07 | 실패 주입(N번째 할당), 세대 retire, `Clear`·이동 뒤 핸들, pageCapacity 검증 | 정해진 오류, 논리 상태 불변 |
| FB08 | 동시성: 읽기 범위 중 변경 Fatal(자식 프로세스), 여러 스레드 동시 질의 | 일치 |
| FB09 | FTZ·DAZ 상태의 보수성 | 누락 0 |

## 10. 벤치마크

`Programs/SpatialIndexBench`에 비교군을 더한다.

- 대상: `FlatBoundsSet`, 기존 A1(scalar double 전수)·A2(float SoA SIMD 전수), D(`LooseOctree` 프러스텀 질의).
- 10.4절 SI01: 원소 10k–1M × 뷰 1·4(메인 + cascade 3), 뷰당 원소 판정 desktop 1.5 ns 이하(제안값), worker 8 확장 효율 0.7 이상.
- 변형: page 용량 64·256·1,024, UE 방식(원소 하나 대 평면 4개 묶음), 뷰 1·4의 상각 효과, (page, word 묶음) 분할.
- 메모리: 원소당 byte, 희소 타일(타일당 원소 몇 개) 분포의 메모리·비트 집합 크기, 큰 원소 목록 크기와 비용, R 팽창이 초과 비율에 주는 영향.
- 기기 부하를 기록하고, 유휴 기기가 아니면 예비 결과로 표시한다.

## 11. 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| FB0 | 이 문서 | 교차검증 반영(개정 2) |
| FB1 | `FlatBoundsSet` 저장·변경·질의·검증, 시험 FB01–FB09 — **구현**(2026-11-19) | 시험 통과, Foundation·format·policy |
| FB2 | 벤치 비교군과 측정 — **예비 측정**(2026-11-19, 부하 있는 환경) | 원시 JSON, 예비 결과 표기 |

## 12. 결정 사항

1. **page 용량 기본값.** 결정됨(FB2): 64. 원소가 많은 분포(10⁶)에서 256·1,024가 조금 빠르나 잡음 범위 안이고 원소당 메모리는 같다(48–49 B). 희소한 분포에서는 큰 용량이 감당되지 않는다(넓고 희소 10⁴의 원소당 메모리 64·256·1,024 = 3.2·12·47 kB, 비트 집합 원소당 61·245·980 bit).
2. **큰 원소 기준.** 결정됨(FB2): 2 × tileSize 유지. 원소의 0.1%·1%를 반폭 3·10⁵ cm로 키워도 시간 차이는 잡음 범위이고 초과는 0%였다. R은 원소 크기보다 원점이 타일 모서리라는 점에서 거의 정해진다(약 tile 크기). 타일 중심을 원점으로 쓰면 R과 δ가 절반이 되는 개선 여지가 있다(후속).
3. **SIMD 축.** 결정됨(FB2): 원소 4개 대 평면 하나. 10⁶ 단일 스레드에서 뷰 1·4 각각 4.4–10.4·3.8–7.7 ns로, UE 방식(원소 하나 대 평면 4개, 거리·타일 정밀도 없음) 6.9–13.2·6.4–11.7 ns보다 1.2–2배 빠르다.
4. **항상 보임 표현.** 결정됨: page별 별도 bit 배열(원소당 1 bit). filter 32 bit를 모두 사용자에게 남기고 원소 64개마다 word 하나만 더 읽는다.

## 12a. 구현 결과(FB1·FB2, 2026-11-19)

**설계와 달라진 점.**

1. `QueryViews`에 `maxHelpers`(기본 무제한)를 더했다. 인자 순서는 `stats`, `maxHelpers`.
2. `ForEachVisible`은 뷰 색인을 받지 않고 결과 하나를 순회한다. visitor는 `FlatVisibleElement{handle, slot, userValue}`를 받고 `QueryControl`을 돌려준다.
3. `TryAdd`의 `FlatBoundsMoves`에 새 원소 자신의 슬롯도 담는다. 진단·시험용 `TryPlacementOf`를 더했다.
4. 목록 슬롯은 `2³¹ + 목록 색인`으로 고정한다(page 수가 바뀌어도 그대로). 비트 집합에서는 page 영역 뒤에 놓인다. page 수 상한이 2³¹/용량이라 겹치지 않는다.
5. 그리기 거리는 desc의 float 값을 그대로 저장하고 여유는 6.2절 한계식으로 질의 때 더한다.
6. 큰 원소와 범위 밖 원소를 한 double 목록에 두고 종류 플래그로 구분한다.
7. page 단위 분류: w′ + ‖n‖₁R + δ < 0이면 page 전체가 그 평면 바깥이라 원소 판정을 건너뛰고, w′ − ‖n‖₁R − 2δ ≥ 0이면 그 평면 판정을 생략한다(δ 하나로 충분하고 2δ는 여유). 평면을 4개 묶음으로 판정해 묶음마다 조기 탈출한다. 결과는 바뀌지 않는다.
8. δ를 float로 옮길 때 최근접 변환에 (1 + 8u)를 곱한다(δ ≥ 4·FLT_MIN·(‖n‖₁ + 1)이 정규 값이라 결과가 δ 이상이 보장된다).
9. 설정 검사: tileSize·anchor는 정수, 2³¹·tileSize + |anchor| < 2⁵³, largeRelativeExtent ≤ 2⁶⁰, pageCapacity ≤ 65,536이고 64의 배수.
10. 실패 원자성을 위해 타일 등록을 `HashMap`에 확보 단계에서 먼저 넣는다(삭제 칸 정리 때문에 예약 뒤에도 할당할 수 있다). 레코드 확보가 실패하면 빈 타일 항목을 지운다.
11. 결과 세대 계수(`NextMutationEpoch`)와 FTZ에 안전한 float 비교를 LooseOctree와 함께 쓰는 내부 헤더(`Private/SpatialIndexInternal.h`)로 옮겼다.
12. (page, word 묶음) 분할은 구현하지 않았다. 용량 64에서 page 수가 충분해 병렬성이 부족하지 않았다.

**측정(FB2, 예비 결과).** 기기 CPU 75–100%, 같은 구성이 반복 사이에 1.5–2배 흔들렸다. 원시 결과는 `../Benchmarks/SpatialIndex/2026-11-19/flatbounds/`.

- 정확도: 모든 분포·구성에서 누락 0, 초과 0.000%.
- SI01(뷰당 원소 판정 1.5 ns 이하, 10⁶·용량 64): 단일 스레드 4.3–10.4 ns로 미충족, worker 8개 0.80–1.52 ns(뷰 4), 1.05–1.37 ns(뷰 1)로 대체로 충족.
- 확장 효율(worker 8): 0.42–0.95. 기준 0.7은 군집·크기 편차·넓고 희소에서 대체로 충족, 균일 뷰 1에서 미충족(0.42–0.64).
- 같은 입력의 원소·뷰당 시간(10⁶): 평면 전수 A1 16–35 ns, A2 20–35 ns, `LooseOctree` 4.9–22 ns. `FlatBoundsSet` 단일 스레드가 전수보다 3–5배 빠르고 옥트리와 비슷하거나 빠르다.
- 넓고 희소 10⁶(용량 64): 타일 124,443개, 원소당 391 B, 원소당 8 bit.

**독립 검증.** 리뷰 퍼저가 이 커밋의 소스를 직접 컴파일해 3,000 시드 × 5,000 변경(추가·삭제·갱신·타일 이동·목록 이동·이동 생성·할당 실패 주입 20%)을 정답(정확한 double AABB의 Core 술어와 double 거리식)과 대조했다. 정답 가시 판정 488,549,569건 중 누락 0, 초과 0.19%. 원거리 타일, 타일 먼 모서리, key 넘침, 꼭짓점 평면 ±1 ulp, 배율 1e−3–1e30, FTZ·DAZ를 포함했고 매 단계 `Validate`, 이동 반환과 슬롯 일치, 출력 계약(쓰레기로 채운 버퍼의 0 재기록)을 확인했다.

## 13. 미확인 사항

- δ 식의 계수(8, 12, 4)는 구현 시험(FB02는 12ε 항을 빼면 실패함을 확인)과 독립 퍼저(정답 가시 판정 4.9억 건 누락 0)로 확인했다.
- UE `IntersectBox8Plane`의 SIMD 세부와 task 크기 산정식은 일부만 확인했다.
- 이 문서의 성능 판단 전부(측정 없음).

## 14. 개정 이력

- 개정 1(2026-11-19): 초안.
- 개정 2(2026-11-19): 교차검증(치명 1, 중요 7, 경미 6) 반영. δ 식에 double 쪽 오차 항과 FTZ 하한을 더했다(원거리 타일 누락 반례). 저장 경계 한 ulp 보정, 거리 판정을 UE 기본값 방향(AABB 최근접·최원 거리)과 배율·입력 검증으로 바꿨다. 해제 page·꼬리 bit 0 계약, 슬롯 이동 반환(`FlatBoundsMoves`)과 결과 세대, page 용량 64·중심 배열 제거, key 범위 검사와 큰 원소·범위 밖 double 목록, API 보강, 시험 FB01–FB09와 벤치 항목을 더했다.
- 개정 3(2026-11-19): FB1·FB2 구현 결과를 반영했다. 설계와 달라진 점 12개(12a절), 거리 저장과 한계식(5·6.2절), 결정 1–4 확정, 예비 측정과 독립 검증 결과.
