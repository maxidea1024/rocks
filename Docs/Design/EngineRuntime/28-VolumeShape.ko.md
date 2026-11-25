# 엔진 런타임 — 28. 영역 형상과 후처리 볼륨 혼합(CR17)

[목차](README.ko.md)

## 28. 영역 형상과 후처리 볼륨 혼합(CR17)

설계일 2026-11-25. [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR17, 결정 CD1(brush volume 대신 영역 형상)의 상세 설계다. 물리 엔진 없이 "점이 영역 안인가, 밖이면 얼마나 먼가"를 정확히 계산한다. 쓰는 곳은 후처리 볼륨(27.5절에서 미룬 볼륨 모으기 — ERD53), 오디오 리버브 존([오디오](../AudioSystem/03-Design/03.15-ReverbZone.ko.md) 3.15.2절), 앞으로의 물리 볼륨(CR27)이다. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`(UE 5.8.4).

### 28.1 UE 동작

- `AVolume::EncompassesPoint(Point, SphereRadius, OutDistance)`(`E/Private/Volume.cpp:84`): brush 충돌 body까지의 거리 제곱(`GetSquaredDistanceToCollision` — 안이면 0)을 재서 거리 ≤ SphereRadius면 참이다. body가 없으면 거리 −1·거짓이다.
- `UPostProcessComponent::EncompassesPoint`(`E/Private/Components/PostProcessComponent.cpp:170`):
  1. 경계가 없으면(unbound이거나 부모가 primitive가 아니다 — `GetPostProcessBounds` `:50`) 거리 0·참이다.
  2. 부모 bounds 상자를 BlendRadius + SphereRadius만큼 넓혀 점이 밖이면 거리 `FLT_MAX`·거짓이다(`:182`).
  3. 부모 primitive의 물리 객체가 있으면 그 거리를 쓴다(`:192–219`).
  4. 부모가 Shape면 `GetSquaredDistanceToCollision`이다. 실패하면 구는 bounds 구 거리, 상자·캡슐은 bounds 상자 거리로 근사한다(`:221–245`).
  5. 그 밖에는 거리 0·참이다(`:253`).
- `GetProperties`(`E/Classes/Components/PostProcessComponent.h:65`): enabled = `bEnabled && ShouldRender()`, unbound = `bUnbound || bounds 없음`, Size = bounds 상자 부피(없으면 `DBL_MAX`).
- 정렬 `PostProcessVolumeLess`(`E/Private/World.cpp:10422`): priority 오름차순 → unbound 먼저 → Size 큰 것 먼저 → `VolumeGuid`.
- 혼합 `DoPostProcessVolume`(`World.cpp:10697`):
  1. 꺼졌으면 건너뛴다.
  2. 가중치를 `Clamp(BlendWeight, 0, 1)`로 시작한다.
  3. bounded면 `EncompassesPoint(view, 0, &d)`로 거리를 잰다. d < 0이면 가중치 0이다. d > BlendRadius면 0이다. BlendRadius ≥ 1이면 가중치에 (1 − d/BlendRadius)를 곱한다. BlendRadius < 1이면 그대로 둔다(0으로 나누기 방지).
  4. 가중치 > 0이면 `OverridePostProcessSettings`를 부른다.
  5. 모든 볼륨을 정렬 순서대로 처리한다(`AddPostProcessingSettings` `:10764`).

### 28.2 `VolumeShape`(Core)

`Core/Math/VolumeShape.h` — 월드에 놓인 영역 형상 값이다(World·Audio가 함께 쓴다 — Core에 두는 수학).

- `VolumeShapeKind { Box, Sphere, Capsule }`.
- 필드:
  - `center`·`rotation`(단위 quaternion).
  - `boxHalfExtent`(상자).
  - `radius`(구·캡슐).
  - `halfHeight`(캡슐 — 반구를 포함한 반 높이, UE 캡슐과 같다. 축은 회전된 +Z다).
  - 크기는 이미 scale을 적용한 월드 값이다.
- 만들기: `TryBox(center, rotation, halfExtent)`·`TrySphere(center, radius)`·`TryCapsule(center, rotation, radius, halfHeight)`. 비유한·음수 크기·단위가 아닌 회전은 `InvalidArgument`다. 캡슐 halfHeight < radius면 radius로 올린다(UE `SetCapsuleSize`가 halfHeight를 radius 이상으로 맞춘다).
- `TryDistanceToVolume(shape, point) → Result<double>`: 안(경계 포함)이면 0, 밖이면 표면까지의 거리다.
  - 상자: `OrientedBox` 거리.
  - 구: max(0, |p − c| − r).
  - 캡슐: max(0, 축 선분 거리 − r). 축 선분은 center ± 회전Z × (halfHeight − radius)다.
  - 비유한 점은 `InvalidArgument`다.
- 볼록체(`Core/Math/ConvexHull.h` — CD1, 출처는 메시 단순 충돌 MA 4.6절이라 출처 연결은 메시 에셋 뒤): `ConvexHullView { Span<const Vec3d> vertices; Span<const uint32> triangleIndices; }`(월드 좌표, 닫힌 볼록 껍질의 삼각형).
  - `TryDistanceToConvexHull(view, point)`: 안이면 0, 밖이면 삼각형까지 거리의 최솟값이다(경계가 삼각형의 합집합이므로 정확하다).
  - 안 판정: 삼각형이 쓰는 꼭짓점의 평균(볼록체 안 — 쓰이지 않는 꼭짓점은 넣지 않는다)이 음의 쪽이 되도록 각 삼각형 평면의 방향을 맞추고, 모든 평면에서 바깥 쪽으로 나가지 않으면 안이다. 그래서 감기 방향과 무관하다.
  - 오류(`InvalidArgument`): 꼭짓점 4개 미만, 색인 수가 3의 배수가 아니다, 색인 범위 밖, 비유한, 퇴화가 아닌 삼각형이 하나도 없다, 평평하다(중심이 어떤 면 평면에서 껍질 크기 × 1e-9 안 — 부피가 없거나 볼록이 아니어서 방향을 정할 수 없다). 퇴화 삼각형은 안 판정에서 빼고 거리 계산에는 넣는다.
  - UE는 물리(GJK)로 같은 거리를 잰다. Rocks는 물리 없이 같은 값을 낸다.

### 28.3 Shape 컴포넌트(World)

- `ShapeComponent::TryWorldVolumeShape() → Result<VolumeShape>`(순수 가상 — Box·Sphere·Capsule이 구현, 비유한 월드 변환은 `InvalidArgument`):
  - 중심 `WorldLocation`, 회전 `WorldQuaternion`.
  - 크기는 UE 물리 형상(`GetSquaredDistanceToCollision`이 재는 대상 — `E/Private/PhysicsEngine/BodySetup.cpp` `FK*Elem::GetFinalScaled`)과 같다. 상자는 extent × |scale|(`:2056`), 구는 반지름 × 최소 |scale|(`:2014`)이다. 캡슐은 반 높이 max(h × |sz|, 0.1), 반지름 clamp(r × max(|sx|, |sy|), 0.1, 반 높이)다(`:2167–2181`). 캡슐은 컴포넌트 bounds(min scale — 24.3.6절)와 다르다. UE도 그렇다.
- `ShapeComponent::TryDistanceToShape(point) → Result<double>`: UE `GetSquaredDistanceToCollision`의 물리 없는 대응(안이면 0)이다.

### 28.4 후처리 볼륨(World)

- `PostProcessComponent::PostProcessBounds() → bool + ComponentBounds`:
  - unbound가 아니고 부모가 `PrimitiveComponent`면 부모 `Bounds()`다.
  - 아니면 경계가 없다.
- `PostProcessComponent::EncompassesPoint(point, sphereRadius) → PostProcessEncompass { bool inside; double distance; }`: 28.1절 순서 그대로다.
  - 3단계(물리 객체)는 없고, 부모가 Shape면 정확한 형상 거리를 쓴다. UE의 bounds 근사 fallback(물리 body가 없을 때)은 두지 않는다. 형상 거리를 못 재면(비유한 변환) {거짓, `PostProcessOutsideDistance`}다.
  - 부모가 Shape가 아닌 primitive면 UE 마지막 분기처럼 거리 0·참이다.
  - 상자 밖 거리 표지는 `PostProcessOutsideDistance`(= `FLT_MAX`)다.
- `PostProcessComponent::VolumeProperties() → PostProcessVolumeProperties { enabled, unbound, blendRadius, blendWeight, priority, size }`: UE `GetProperties`와 같다. enabled = `postProcessEnabled && IsVisible()`(23.3절 — UE `ShouldRender`).
- `World/PostProcess/PostProcessVolumeBlending.h`: `BlendPostProcessVolumes(World&, Allocator& scratch, viewLocation, PostProcessSettings& inOut) → Result<uint32>`(적용한 볼륨 수 — 오류는 OutOfMemory·비유한 view). `PostProcessVolumeLess`·`PostProcessVolumeWeight`(볼륨 하나의 가중치)도 공개한다(시험·렌더가 같은 식을 쓴다).
  - 모으기: World의 등록된(Initialized·Playing) `PostProcessComponent`를 모두 모은다. 소유 Actor 순회 → 컴포넌트 순이다.
  - 정렬: `PostProcessVolumeLess`. 마지막 동점은 UE `VolumeGuid`(경로 이름 결정적 guid) 대신 handle(slot, generation) 순이다. Rocks에는 컴포넌트 영속 guid가 아직 없고, 같은 조작 열이면 같은 순서다.
  - 가중: `DoPostProcessVolume`과 같은 식이다.
  - 덮어쓰기: `OverridePostProcessSettings`(27.5절)다.
  - UE `TArray` 삽입 정렬과 달리 호출마다 모아 정렬한다. 볼륨 수가 많아지면 등록 목록(World 하위 시스템)으로 바꾼다. 측정 뒤 결정한다.
- 카메라 위치를 넘기고 결과를 렌더 view에 싣는 일은 RS6(렌더 view 구성)이다. 이 함수는 위치만 받는다.

### 28.5 결정·시험

| ID | 항목 | 결정 | 근거 |
|---|---|---|---|
| ERD54 | 형상 위치 | 순수 기하는 Core `VolumeShape`, 컴포넌트 연결은 World | 오디오가 World 없이 쓴다, Core는 수학을 둘 수 있다 |
| ERD55 | 볼록체 거리 | 삼각형 최솟값 + 방향 맞춘 평면 안 판정(GJK 없음) | 정확하고 결정적이다. 꼭짓점·삼각형은 메시 단순 충돌이 준다 |
| ERD56 | 동점 순서 | handle(slot, generation) | 영속 guid 없음. 결정적이다 |

시험:
- Core `VolumeShape_` 접두: 세 형상의 안·경계·밖 거리(회전 포함), 캡슐 halfHeight < radius, 오류, 볼록체(정육면체·사면체, 감기 방향 섞임, 퇴화 삼각형 섞임, 오류).
- World `PostProcessVolume_` 접두:
  - Shape 컴포넌트 월드 형상(scale·회전·부모).
  - `EncompassesPoint` 분기 다섯.
  - `VolumeProperties`(숨김이면 꺼짐).
  - 정렬 네 단계.
  - 혼합 가중(BlendRadius 안·밖·< 1, weight clamp, 꺼진 볼륨).
