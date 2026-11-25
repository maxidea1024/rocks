# 엔진 런타임 — 30. 메시 컴포넌트 데이터(D2b)

[목차](README.ko.md)

## 30. 메시 컴포넌트 데이터(D2b)

설계일 2026-11-25. [24장](24-ComponentData.ko.md) 데이터 먼저 묶음에서 StaticMesh 에셋(MA2b — [메시·애니메이션 에셋](../MeshAnimationAssets/13-Stages.ko.md))을 기다리던 D2b다(24.3.1절, ERD39). 기준은 24.1절과 같다: UE 5.8 속성·기본값·단위·Mobility 규칙을 리플렉션 영속 필드로 넣고(`ROCKS_PROPERTY(ReadOnly)`, 변경은 setter), 순수 계산만 구현하고, 시스템(렌더 proxy RS1·충돌 CR16·메시 로드)은 붙이지 않는다. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`(UE 5.8.4).

### 30.1 모듈과 에셋 참조

- 모듈은 새 `WorldIntegration`(`Engine/Source/Runtime/WorldIntegration`, 3절)이다. World는 에셋 모듈을 참조하지 않는다(3절 `World` 행). 메시 컴포넌트는 `StaticMesh`(Mesh 모듈)와 `MaterialBase`(Material 모듈)를 기대 타입으로 쓰므로 World 위 모듈에 둔다. 의존은 공개 World·Mesh·Material이다. RenderBase는 RenderScene 추출(RS 단계)이 붙을 때 더한다(지금 쓰지 않는 의존을 두지 않는다).
- **World 객체 → 에셋 참조는 `SoftObjectRef`다**([엔진 구현 계획](../EngineImplementationPlan.ko.md) 638행 "World 객체 → 에셋은 Soft 참조, 런타임 보유는 AssetLease"). World 객체는 World Registry에, 에셋 객체는 에셋 Registry에 있어 `ObjectRef`(같은 Registry 안 강한 간선)로 가리킬 수 없다(다른 Registry handle은 WrongContext — ReflectionExtensions 쓰기 검증). 저장 wire는 `SoftObjectReference`(AssetId 16 byte + 객체 경로)다.
  - UE는 `TObjectPtr<UStaticMesh>`(하드 참조 — 레벨과 함께 로드)다. Rocks의 "함께 로드"는 로드 시스템 몫이다: 컴포넌트 시작 준비 hook(CR4 `IsReadyToStart` — 17.4절)이 메시 lease를 기다리고, 쿠킹은 레벨의 soft 참조를 의존으로 센다. 둘 다 메시 로드·렌더 연결 단계에서 붙인다(30.6절).
  - 런타임 보유(AssetLease)와 로드된 메시 객체로의 접근은 이 단계에 없다. 대신 로드 시스템이 메시를 붙일 때 부를 **로드 정보 사본**(30.3절 `ApplyLoadedMeshInfo`)을 둔다: 확장 bounds와 머티리얼 슬롯 수만 값으로 복사하고 에셋 객체 포인터는 쥐지 않는다(Registry 수명이 다르다).
- 파일 배치(24.1절 규칙): `Public/WorldIntegration/Components/`(`MeshComponent.h`·`StaticMeshComponent.h`)와 `Public/WorldIntegration/Actors/`(`StaticMeshActor.h`), `Private/`도 같은 폴더다. 리플렉션 등록은 `RegisterWorldIntegrationTypes`(생성 집계 — World·Mesh·Material 타입도 함께)다.
- World의 공용 도우미 둘을 공개 헤더로 옮긴다(동작 불변 이동 — World 밖 컴포넌트 모듈이 같은 규칙을 쓴다): `World/ComponentValueChecks.h`(`rocks::component_checks`), `World/ActorRootSetup.h`(`rocks::actor_root_setup::PrepareRoot<T>`).

### 30.2 MeshComponent

`MeshComponent`(Abstract) : `PrimitiveComponent`(UE `UMeshComponent` — `E/Classes/Components/MeshComponent.h`). 생성자(`E/Private/Components/MeshComponent.cpp:27`)는 `castShadow`·`useAsOccluder`를 켠다(24.3.3절 표의 "메시 계열이 켠다"). `bCanEverAffectNavigation`은 내비게이션 체계 몫이다(24.3.8절).

| 필드 | 기본 | 뜻 |
|---|---|---|
| `overrideMaterials` | 빈 배열 | 슬롯별 머티리얼 override(`Array<SoftObjectRef<MaterialBase>>`, UE `OverrideMaterials`). null 원소 = 메시 슬롯 머티리얼 |
| `overlayMaterial` | null | 메시 전체에 겹쳐 그리는 머티리얼(UE `OverlayMaterial`) |
| `overlayMaterialMaxDrawDistance` | 0 (cm) | overlay 최대 거리. 0이면 primitive 최대 거리로 culling한다(UE 주석) |
| `materialSlotsOverlayMaterial` | 빈 배열 | 슬롯별 overlay override(UE 5.8 `MaterialSlotsOverlayMaterial`). 없거나 null이면 전체 overlay → 메시 슬롯 overlay 순 |

setter(24.3.5절처럼 Mobility를 보지 않는다 — UE `SetMaterial`·`SetOverlayMaterial`에 `AreDynamicDataChangesAllowed`가 없다):
- `SetMaterial(index, material)`: index < 0 또는 ≥ `StaticMeshMaxMaterialSlots`(65535 — 메시 슬롯 상한)면 `InvalidArgument`. 같은 값이면 아무것도 하지 않는다. 배열이 짧으면 null로 늘린다(UE `AddZeroed` — `MeshComponent.cpp:76`). 값 검증은 `SoftObjectRef::Validate`(실패 `InvalidArgument`), 할당 실패는 값 유지.
- `SetOverlayMaterial(material)`, `SetSlotOverlayMaterial(slot, material)`: 슬롯 판은 UE처럼 배열을 slot + 1로 늘린다. UE는 `slot < GetNumMaterials()`(메시 슬롯 수)일 때만 늘리고 아니면 조용히 무시한다(`:360`). Rocks는 로드 정보가 있으면 같은 조건을 쓰고(밖이면 `InvalidArgument`), 없으면 메시 슬롯 상한까지 받는다(메시가 아직 로드되지 않은 편집 데이터를 거부하지 않는다 — 30.5절 한계).
- `SetOverlayMaterialMaxDrawDistance(d)`: 비유한·음수는 `InvalidArgument`(UE는 검증 없음).
- 값이 바뀌면 Render 다시 보내기를 표시한다(UE `MarkRenderStateDirty`).
- 조회: `NumOverrideMaterials()`, `OverrideMaterial(index)`(범위 밖·null이면 null 참조), `OverlayMaterial()`, `SlotOverlayMaterial(slot)`, `NumMaterials()`(virtual — 기본 override 수, StaticMesh는 로드 정보의 슬롯 수, 30.3절).
- 머티리얼 해석(override → 메시 슬롯 머티리얼, overlay 우선순위)은 메시 슬롯 머티리얼을 아는 렌더 proxy(RS1)가 한다. 지금은 override 쪽 데이터만 있다.

### 30.3 StaticMeshComponent

`StaticMeshComponent` : `MeshComponent`(UE `UStaticMeshComponent` — `E/Classes/Components/StaticMeshComponent.h`, 생성자 `E/Private/Components/StaticMeshComponent.cpp:200`). 기본 충돌 profile은 "BlockAllDynamic"이다(`:206` — `DefaultCollisionProfileName` 재정의, 이름 상수 `StaticMeshCollisionProfileName`). UE `bCanEverTick = false`는 Rocks Component 기본(Tick 꺼짐)과 같다.

| 필드 | 기본 | 뜻·범위 |
|---|---|---|
| `staticMesh` | null | 메시 에셋(`SoftObjectRef<StaticMesh>`) |
| `forcedLodModel` | 0 | 0 = 자동, n ≥ 1 = LOD n − 1 고정(UE `ForcedLodModel`). 음수 거부 |
| `minLod`·`overrideMinLod` | 0·거짓 | 컴포넌트 최소 LOD(켜졌을 때 메시 minLod 대신). 음수 거부 |
| `wireframeColorOverride`·`overrideWireframeColor` | (255, 255, 255, 255)·거짓 | wireframe 색 |
| `worldPositionOffsetDisableDistance` | 0 (cm) | 이 거리 밖에서 WPO를 끈다(0 = 끄지 않음). 음수 거부 |
| `evaluateWorldPositionOffset`·`worldPositionOffsetWritesVelocity`·`evaluateWorldPositionOffsetInRayTracing` | 참·참·거짓 | |
| `forceNaniteForMasked`·`disallowNanite`·`forceDisableNanite` | 모두 거짓 | 가상 기하(Nanite) 사용 조정 |
| `nanitePixelProgrammableDistance` | 0 (cm) | 이 거리 밖에서 가상 기하 pixel programmable 래스터를 끈다(0 = 끄지 않음). 음수 거부 |
| `overrideLightMapRes`·`overriddenLightMapRes` | 거짓·64 | lightmap 해상도 override. 음수 거부 |
| `castDistanceFieldIndirectShadow`·`distanceFieldIndirectShadowMinVisibility` | 거짓·0.1 | [0, 1](UE ClampMin·ClampMax) |
| `overrideDistanceFieldSelfShadowBias`·`distanceFieldSelfShadowBias` | 거짓·0 | 0 이상(UE ClampMin) |
| `useSubDivisions`·`subDivisionStepSize` | 참·32 | 굽는 조명 세분(값 보존). step은 1 이상 |
| `useDefaultCollision` | 거짓 | 메시 BodySetup의 기본 충돌을 쓴다(StaticMeshActor가 켠다) |
| `sortTriangles`·`reverseCulling` | 거짓·거짓 | |
| `ignoreInstanceForTextureStreaming` | 거짓 | |
| `streamingDistanceMultiplier` | 1 | UE ClampMin 0. 음수·비유한 거부 |

setter:
- 필드마다 getter·setter(24.2.3절 규칙 — 거부는 `InvalidArgument`, 값 유지, 바뀌면 Render 다시 보내기).
- **Mobility**: `SetStaticMesh`만 UE `SetStaticMesh`(`:2370`)처럼 게임 초기화 뒤 Static이면 거부한다(`CheckDynamicDataChange("staticMesh", false)` — Warning `StaticComponentDataChanged`, 24.2.3절). UE의 다른 setter(`SetForcedLodModel`·`SetReverseCulling`·`SetEvaluateWorldPositionOffset` 등)는 Mobility를 보지 않는다. 24.3.5절과 같다.
- `SetStaticMesh(mesh)`: 같은 값이면 아무것도 하지 않는다(UE는 false를 돌려준다 — Rocks는 성공·무변경). 바뀌면 로드 정보를 버리고(30.3절 아래) bounds를 무효화한다.

로드 정보(native 런타임 상태 — 저장하지 않는다):
- `ApplyLoadedMeshInfo(const StaticMesh& mesh)`: 메시 로드 시스템이 lease를 얻은 뒤 부른다. `StaticMeshExtendedBounds(mesh)`(Mesh — UE `GetBounds` = ExtendedBounds)와 슬롯 수(`materialSlots.Size()`)를 복사하고 bounds를 무효화한다. 에셋 객체를 쥐지 않는다. `ClearLoadedMeshInfo()`는 지운다.
- `HasLoadedMeshInfo()`, `LoadedMeshMaterialSlotCount()`. `NumMaterials()`는 로드 정보가 있으면 슬롯 수, 없으면 0이다(UE `GetNumMaterials` — 메시가 없으면 0).

bounds(UE `CalcBounds` — `:662`):
- 로드 정보가 있으면 지역 bounds = 메시 확장 bounds에 `boundsScale`을 곱한 것(상자 extent·구 반지름 — UE는 TransformBy 뒤 곱하지만 extent·반지름 변환이 extent에 선형이라 같다)을 `TryLocalBounds`로 준다. `Bounds()`가 월드 행렬로 옮긴다(24.3.6절 `TransformComponentBounds`).
- 없으면 `TryLocalBounds`가 거짓 → 기본 `CalcBounds`(위치의 점, extent·반지름 0)다. UE의 메시 없음 결과와 같다.

### 30.4 StaticMeshActor

`StaticMeshActor` : `Actor`(UE `AStaticMeshActor` — `E/Private/StaticMeshActor.cpp:27`). 프레임워크 PreAwake에서 `StaticMeshComponent` root를 준비한다(24.2.5절 규칙 — 같은 타입 재사용, 다른 타입 `InvalidArgument`). 만든 컴포넌트 설정(UE 생성자):
- 충돌 profile "BlockAll"(`StaticMeshActor::StaticMeshActorCollisionProfileName`)
- Mobility Static
- `generateOverlapEvents` 거짓
- `useDefaultCollision` 참

조회 `StaticMeshComponentHandle()`(root). UE `SetCanBeDamaged(false)`(피해 체계), `bCanBeInCluster`(GC cluster), `bStaticMeshReplicateMovement`(네트워크)는 그 체계 몫이다(30.5절).

### 30.5 들이지 않은 UE 속성(사유)과 한계

| UE | 사유 |
|---|---|
| `LODData`(인스턴스별 lightmap·정점 색)·`CachedStaticLighting`·`StaticMeshDerivedDataKey` | 굽는 조명·mesh paint 체계 |
| mesh paint(`bEnableVertexColorMeshPainting`·`MeshPaintTexture*`·`bOverride…MeshPaint…`)·material cache(`MaterialCache*`·`bWritesToMaterialCache`) | 해당 체계 |
| `StreamingTextureData`·`MaterialStreamingRelativeBoxes` | 텍스처 스트리밍 빌드 데이터(스트리밍 단계) |
| `bOverrideNavigationExport`·`bForceNavigationObstacle`·`bHasCustomNavigableGeometry` | 내비게이션 체계 |
| 편집기 전용(`SelectedEditorSection`·`SectionIndexPreview`·`bDisplayVertexColors`·`StaticMeshImportVersion` 등)·deprecated(`IrrelevantLights`·`bDisallowMeshPaintPerInstance`) | 편집기·레거시(하위 호환 금지) |
| `bEnableMaterialParameterCaching`·머티리얼 파라미터 캐시 | 머티리얼 인스턴스 런타임(MS 단계) |
| `BodyInstance.bAutoWeld`(StaticMeshComponent 기본 참) | 물리 body(CR16) |
| StaticMeshActor `SetCanBeDamaged`·`bCanBeInCluster`·`bStaticMeshReplicateMovement` | 피해·GC cluster·네트워크 |

한계:
- 머티리얼 해석과 슬롯 overlay 정리(UE `CleanUpMaterialSlotsOverlayMaterial` — 메시 슬롯 수보다 긴 배열 자르기)는 메시 슬롯 정보가 있는 렌더 proxy 단계에서 한다. 로드 정보가 없을 때 받은 긴 슬롯 overlay 배열은 그때 잘린다.
- `forcedLodModel`·`minLod`는 범위를 메시 LOD 수로 자르지 않는다(UE도 쓰는 쪽이 자른다).

### 30.6 시스템 연결(나중)

- 메시 로드: 컴포넌트 등록·시작 준비(CR4)에서 `staticMesh` lease를 요청하고 준비되면 `ApplyLoadedMeshInfo`, 바뀌거나 해제되면 `ClearLoadedMeshInfo`. 쿠킹은 레벨 패키지의 soft 참조를 쿠킹 대상 의존으로 센다(AssetSystem 10절).
- 렌더 proxy(RS1): 머티리얼 해석·LOD 선택·WPO·가상 기하 플래그를 쓴다.
- 충돌(CR16): 메시 단순 충돌·`useDefaultCollision`.

### 30.7 결정·시험

| ID | 항목 | 결정 | 근거 |
|---|---|---|---|
| ERD60 | World → 에셋 참조 | `SoftObjectRef` 영속 필드, 런타임 보유·로드는 로드 시스템 | 계획 638행, Registry가 달라 ObjectRef 불가 |
| ERD61 | 로드된 메시 값 | bounds·슬롯 수만 native 사본(`ApplyLoadedMeshInfo`) | 에셋 객체 수명과 분리, 데이터 먼저 |
| ERD62 | 슬롯 overlay 길이 | 로드 정보 있으면 UE 조건(슬롯 수 미만), 없으면 메시 슬롯 상한 | 로드 전 편집 데이터 거부 회피 |
| ERD63 | Mobility | `SetStaticMesh`만 Static 거부, 나머지는 보지 않음 | UE setter |

시험 `WorldIntegration/Tests/StaticMeshComponentTests.cpp`(`MeshComponent_` 접두):
1. MeshComponent·StaticMeshComponent 기본값(30.2·30.3절 표, 그림자·occluder 켬, profile "BlockAllDynamic").
2. 머티리얼 setter: 늘리기(null 채움)·같은 값 무변경·범위 밖 index·잘못된 soft 참조 값 유지, overlay·슬롯 overlay(로드 정보 있음/없음), 거리 검증.
3. StaticMeshComponent setter 검증(음수·비유한·[0, 1] 범위), `SetStaticMesh` Mobility(게임 World Static 거부 + 진단, Movable 성공, 편집 World 성공), 다른 setter는 Static에서도 성공.
4. bounds: 로드 정보 없음 = 위치 점, `ApplyLoadedMeshInfo`(에셋 Registry에서 만든 StaticMesh — 확장 bounds)·회전·비균일 scale·`boundsScale` 손계산 대조, `SetStaticMesh`가 정보를 지움, `NumMaterials`.
5. StaticMeshActor root·profile "BlockAll"·Static·overlap 꺼짐·useDefaultCollision, root 재사용·다른 타입 거부.
6. 리플렉션 형태(Abstract·base·필드 수)와 저장 왕복(soft 참조 AssetId·경로, override 배열 null 원소 포함).
