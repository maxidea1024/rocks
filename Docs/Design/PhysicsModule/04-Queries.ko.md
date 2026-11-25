# 물리 모듈 — 4. 장면 질의

[목차](README.ko.md)

UE `UWorld`의 trace·sweep·overlap(`E/Classes/Engine/World.h:2128–2499`, 구현 `E/Private/Collision/WorldCollision.cpp`)과 같은 의미를 Rocks 이름으로 둔다. 게임플레이 형상 질의는 Jolt scene query다(계획 개정 1.13 — 비충돌 2D 해시는 쓰지 않는다).

## 4.1 함수

World 모듈의 자유 함수(`World/Physics/WorldCollision.h`)다. 장면이 없으면 hit 없음(false·빈 결과)을 돌려준다.

| Rocks | UE(같은 의미) | 결과 |
|---|---|---|
| `LineTraceTest(world, start, end, filter, params)` | `LineTraceTestBy{Channel,ObjectType,Profile}` | bool: 막는 hit가 있는지 |
| `LineTraceFirst(world, start, end, filter, params, HitResult&)` | `LineTraceSingleBy…` | 첫 막는 hit |
| `LineTraceAll(world, start, end, filter, params, Array<HitResult>&)` | `LineTraceMultiBy…` | 겹침 hit들 + 첫 막는 hit(4.5절) |
| `ShapeTraceTest/First/All(world, start, end, rotation, shape, filter, params, …)` | `SweepTestBy…`·`SweepSingleBy…`·`SweepMultiBy…` | 같다 |
| `OverlapBlocking(world, position, rotation, shape, filter, params)` | `OverlapBlockingTestBy{Channel,Profile}` | bool: 막는 겹침이 있는지 |
| `OverlapAny(…)` | `OverlapAnyTestBy…` | bool: 겹침이든 막음이든 하나라도 |
| `OverlapAll(…, Array<OverlapResult>&)` | `OverlapMultiBy…` | 전부 |
| `ComponentTraceAll(world, component, start, end, rotation, params, …)` | `ComponentSweepMulti` | 컴포넌트 자기 형상으로 sweep(5.1절이 쓴다) |
| `ComponentOverlapAll(world, component, position, rotation, params, …)` | `ComponentOverlapMulti` | 컴포넌트 자기 형상으로 overlap(5.3절이 쓴다) |

- **필터 종류를 인자로 합쳤다**(PMD8). UE는 `ByChannel`·`ByObjectType`·`ByProfile` 세 벌(27개 함수)이다. Rocks는 `QueryFilter` 하나가 세 종류를 담는다(4.2절). 동작은 UE 세 벌과 같다.
- 형상 크기가 거의 0이면 shape trace는 line trace로 바꾼다(UE `WorldCollision.cpp:151–155`).
- 비동기 질의(UE `AsyncLineTrace…` — 결과를 다음 프레임에)는 들이지 않는다. 필요해지면 PhysicsResults 다음 자리에서 일괄 처리하는 방식으로 더한다.

## 4.2 필터와 인자

```cpp
// QueryFilter (World 모듈)
struct QueryFilter {
    enum class Kind : uint8 { Channel, ObjectTypes, Profile };
    Kind kind = Kind::Channel;
    CollisionChannel channel = CollisionChannel::Visibility;   // Channel
    CollisionResponses responses{CollisionResponse::Block};     // Channel — UE FCollisionResponseParams 기본 Block
    uint64 objectTypes = 0;                                     // ObjectTypes — 채널 번호 bit(64칸, UE ObjectTypesToQuery64)
    StringView profile;                                         // Profile — 표에서 채널·응답을 꺼낸다
};
```

- `ObjectTypes` 도우미: `AllObjects`·`AllStaticObjects`·`AllDynamicObjects`(UE `CollisionQueryParams.h:458–463`·`528–545`). UE `FCollisionQueryFlag`는 AllObjects를 모든 bit로 시작해(`:359–362`) trace 채널(Visibility·Camera·trace 사용자 채널)만 뺀다(`E/Private/Collision/CollisionProfile.cpp` `RemoveFromAllObjectsQueryFlag`). 그래서 이름 없는 사용자 채널도 AllObjects에 든다. 정적 묶음은 WorldStatic과 `staticObject` 사용자 채널(profile 표 `IsStaticObjectChannel`), AllDynamic = AllObjects − 정적 묶음이다.
- `Profile`: 표에서 그 profile의 object type을 채널로, 응답 묶음을 응답으로 써서 `Channel` 질의를 한다(UE `WorldCollision.cpp:286–293`). 표에 없으면 경고 후 `WorldStatic` + 기본 응답(UE `World.cpp:2052–2063`).

`QueryParams`(UE `FCollisionQueryParams` — `CollisionQueryParams.h:42–126`, 기본값 `:210–228`):

| 필드 | 기본값 | 뜻 |
|---|---|---|
| `traceTag`·`ownerTag` | 빈 | 디버그 표시·통계용 이름 |
| `traceComplex` | 거짓 | 참이면 complex(삼각형) 형상과 판정 |
| `findInitialOverlaps` | 참 | shape trace 시작점 겹침을 hit로 돌려준다 |
| `returnFaceIndex` | 거짓 | complex hit의 삼각형 번호 |
| `returnPhysicalMaterial` | 거짓 | hit의 물리 재질(6.6절 — 재질 에셋 뒤) |
| `ignoreBlocks`·`ignoreTouches` | 거짓 | 막음·겹침 hit를 버린다 |
| `mobility` | Any | Static·Dynamic 하나만 질의(UE `MobilityType`) |
| `ignoreComponents`·`ignoreActors` | 빈 | 무시할 컴포넌트·Actor(약한 참조 → body handle 집합으로 바꿔 Jolt BodyFilter) |

- UE의 기본 인자 `DefaultQueryParam`은 `bTraceComplex = true`로 만들어진다(`WorldCollision.cpp:50`). struct 기본값(거짓)과 다르다. Rocks는 함수 기본 인자도 struct 기본값(거짓)을 쓴다(PMD9 — 같은 이름의 두 기본값이 다른 것은 함정이다). 차이는 대응표에 적는다.
- `traceIntoSubComponents`·`replaceHitWithSubComponents`·`ignoreMask`(UE)는 sub-component(instanced·skeletal) 단계에서 더한다.

## 4.3 질의 형상

`CollisionShape`(Collision 모듈, UE `FCollisionShape` — `PC/Public/CollisionShape.h:10–110`):

| 종류 | 값(cm) | 비고 |
|---|---|---|
| `Line` | — | 기본 |
| `Box` | 반폭 x·y·z | |
| `Sphere` | 반경 | |
| `Capsule` | 반경, 반높이(**반구 포함**) | UE와 같은 의미. extent로 만들면 반경 = max(x, y), 반높이 = z |
| `Cylinder` | 반경, 반높이 | Rocks 추가(CY2 — convex radius 0) |

## 4.4 필터 판정

UE와 같은 규칙이다(`CH/Private/Chaos/CollisionFilterData.cpp:434–513`, `E/Private/PhysicsEngine/CollisionQueryFilterCallback.cpp:146–223`).

1. 무시 목록(`ignoreComponents`·`ignoreActors`)·mobility·질의 가능 여부(실효 충돌 켬에 query가 있는지 — CollisionData 2.2절)를 먼저 본다.
2. **Channel 질의**: 질의 쪽 판정 = body의 응답 묶음에서 질의 채널의 응답, body 쪽 판정 = 질의 응답 묶음에서 body object type의 응답. 결과 = 둘 중 작은 것(Ignore < Overlap < Block).
3. **ObjectTypes 질의**: body object type bit가 질의 집합에 있으면 Block, 아니면 Ignore. `All` 질의는 모든 결과를 겹침으로 모은 뒤 Block으로 보고한다(UE `CollisionConversions.cpp:363–365` — ObjectTypes hit의 `blockingHit`은 참).
4. `ignoreTouches`·`ignoreBlocks`를 적용한다. overlap 질의는 Block도 겹침으로 모은다(최종 `blockingHit`으로 구분).
5. shape trace의 시작점 겹침(막음)은 계속 진행하는 겹침으로 두었다가 막는 시작 겹침으로 보고한다. `findInitialOverlaps`가 거짓이면 버린다(UE `QF:203–223`).

- Jolt 구현은 6.2–6.3절(층 bit와 쌍 필터, 질의용 `ObjectLayerFilter`·`BodyFilter`·`ShapeFilter`).

## 4.5 결과

`HitResult`(UE `FHitResult` — `E/Classes/Engine/HitResult.h`):

| 필드 | 뜻 |
|---|---|
| `time` | 0–1 비율(시작 → 끝). sweep 이동은 pullback 때문에 조금 작다(5.1절) |
| `distance` | 시작점에서 `location`까지 거리(cm). 시작 겹침이면 0 |
| `location` | 형상 중심이 닿은 위치 = 시작 + time·(끝 − 시작) |
| `impactPoint` | 실제 접촉점. 시작 겹침이면 `location` |
| `normal` | 쓸고 간 형상 쪽 법선(line은 `impactNormal`과 같다) |
| `impactNormal` | 맞은 표면 법선(바깥 방향, 어댑터 경계 5절) |
| `traceStart`·`traceEnd` | 시작·시도한 끝 |
| `penetrationDepth` | 시작 겹침의 MTD 깊이(`normal` 방향, cm). MTD를 못 구하면 법선 +Z·깊이 0(UE `CollisionConversions.cpp:617–628`) |
| `blockingHit`·`startPenetrating` | 막음 여부·시작 겹침 여부. `IsValidBlockingHit` = 막음 && !시작 겹침 |
| `faceIndex` | complex hit 삼각형 번호(`returnFaceIndex`) |
| `component`·`actor` | 맞은 컴포넌트·Actor(약한 참조). weld 자식은 원래 자식 컴포넌트 |
| `item`·`myItem`·`boneName`·`myBoneName`·`physicalMaterial` | instanced·skeletal·재질 단계에서 채운다 |

`OverlapResult`: `component`·`actor`·`item`·`blockingHit`(UE `OverlapResult.h`).

- **정렬**(`All`): `time` 오름차순, 같은 `time`이면 겹침이 막음보다 앞. 막는 hit가 있으면 가장 가까운 것 하나만 끝에 둔다(그 뒤 겹침은 버린다 — UE `World.h:2199–2201`, `CollisionConversions.cpp:526–527`·`929–939`). shape trace는 시작 겹침을 먼저 넣는다(UE `World.h:2313`).
- 같은 time의 순서가 Jolt 결과 순서에 기대지 않도록, 같은 time·같은 막음 여부 안에서는 body handle 순으로 정렬한다(UE에 없는 결정성 규칙 — PMD10).

## 4.6 단위·좌표

- 입력·결과는 Rocks 월드 좌표(double cm)다. 어댑터가 지역 원점·cm → m·축 사상을 적용하고(어댑터 경계 2·3·6절), `distance`는 비율 × Rocks 길이로 계산한다(어댑터 경계 5절 — m에서 다시 계산하지 않는다).
- Jolt ray의 방향 벡터 길이가 최대 거리다(J13). shape cast는 `RShapeCast::sFromWorldTransform`(시작 transform, 이동 벡터)이다.
- 시작점이 볼록 형상 안인 line trace는 `mTreatConvexAsSolid` 참으로 fraction 0 hit → `startPenetrating`(어댑터 경계 5절).

## 4.7 UE와 다른 점

| 항목 | UE | Rocks | 이유 |
|---|---|---|---|
| 함수 수 | 필터 종류마다 따로(27개) | `QueryFilter` 인자(9개) | PMD8 — 의미는 같다 |
| `traceComplex` 기본 | 함수 기본 인자 참, struct 기본 거짓 | 둘 다 거짓 | PMD9 |
| 같은 time hit 순서 | 정렬 기준 밖(결과 순서에 따름) | body handle 순 | PMD10 — 결정성 |

## 4.8 나중

- 비동기 질의, 질의 통계·디버그 그리기(`traceTag` — 디버그 그리기 CR23 뒤), sub-component 질의(instanced 메시·skeletal), physical material 반환.
