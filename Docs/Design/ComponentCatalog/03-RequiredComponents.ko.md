# 엔진 컴포넌트 도입 — 3. 필수 컴포넌트

[목차](README.ko.md)

## 3. 필수 컴포넌트

이 절은 **무엇을 만드는가**만 적는다. 선행 작업은 4절, 묶음 순서는 5절이다. "선행" 열은 4절 ID, "묶음" 열은 5절 ID다. 이름은 CD6 권장안이다(UE 이름은 대응표에만, 업계 공용어는 그대로).

### 3.1 기반

| UE | Rocks | 역할 | 선행 | 묶음 |
|---|---|---|---|---|
| `UActorComponent` | `Component`(있음) | 수명·Tick·활성. 보강할 것: 시스템 상태 hook, 태그, EditorOnly, 타입 규칙 | CR1·CR4·CR5·CR6·CR8 | CM0 |
| `USceneComponent` | `SceneComponent`(있음) | Transform 계층. 보강할 것: 부착 규칙·socket·절대 Transform·Mobility·가시성·속도·bounds·변경 통지 | CR2·CR3 | CM0 |
| `UPrimitiveComponent` | `PrimitiveComponent` | 그려지거나 충돌하는 것의 공통 상위: 렌더 플래그(그림자·거리 컬링·조명 채널·custom depth), bounds, 충돌·overlap·hit, 텍스처 스트리밍 입력 | CR1·CR12·CR13·CR14a·CR16 | CM1a(렌더 부분)·CM3(충돌 부분) |
| `UMeshComponent` | `MeshComponent` | section별 머티리얼 override, 동적 머티리얼 인스턴스 | CR14 | CM1a |
| `UShapeComponent` | `ShapeComponent` | 단순 충돌 형상 공통 상위 | CR16 | CM3 |

UE 근거: `UActorComponent`의 tick·태그·AssetUserData·자동 활성·EditorOnly·물리 상태·복제(`EC/ActorComponent.h:177, 181, 186, 317, 344, 621, 634`), `USceneComponent`의 socket 부착·속도·절대 Transform·가시성·Mobility(`EC/SceneComponent.h:113, 158, 171–179, 183, 226, 303`), `UPrimitiveComponent`의 거리 컬링·overlap·그림자·body·hit·overlap 이벤트·scene proxy(`EC/PrimitiveComponent.h:337, 429, 548, 1445, 1457, 1468, 2395`).

### 3.2 메시

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UStaticMeshComponent` | `StaticMeshComponent` | CR9·CR12·CR13(RS0)·CR14 | CM1a |
| `UInstancedStaticMeshComponent` | `InstancedStaticMeshComponent`(HISM 흡수, CD2) | CR12(인스턴스 종류)·RN RS4 | CM6 |
| `USplineMeshComponent` | `SplineMeshComponent` | CM1a + `SplineComponent`(CM5), 정점 변형 셰이더(머티리얼 정점 입력) | CM6 |
| `USkinnedMeshComponent` | `SkinnedMeshComponent` | CR19·CR13(RS12) | CM7 |
| `USkeletalMeshComponent` | `SkeletalMeshComponent` | CR19·CR16(ragdoll·physics asset) | CM7 |
| `UPoseableMeshComponent` | `PoseableMeshComponent` | CM7 | CM7 |
| `ULODSyncComponent` | `LodSyncComponent` | CM7 | CM7 |
| `UInstancedSkinnedMeshComponent` | `InstancedSkinnedMeshComponent` | CM7 + 애니메이션 bank(`UAnimBank`, `EC/InstancedSkinnedMeshComponent.h:8, 20`) | CM11 |

### 3.3 광원

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `ULightComponentBase` | `LightComponentBase` | CR12 | CM1b |
| `ULightComponent` | `LightComponent` | CR12 | CM1b |
| `ULocalLightComponent` | `LocalLightComponent` | CR12 | CM1b |
| `UDirectionalLightComponent` | `DirectionalLightComponent` | RN RS1·RS3(CSM) | CM1b |
| `UPointLightComponent` | `PointLightComponent` | RN RS1·RS3 | CM1b |
| `USpotLightComponent` | `SpotLightComponent` | RN RS1·RS3 | CM1b |
| `URectLightComponent` | `RectLightComponent` | RN RS9 | CM8 |
| `USkyLightComponent` | `SkyLightComponent` | RN RS1(상수)·RS7(capture) | CM1b(상수)·CM9(capture) |

### 3.4 대기·환경

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `USkyAtmosphereComponent` | `SkyAtmosphereComponent` | RN RS5 | CM8 |
| `UExponentialHeightFogComponent` | `HeightFogComponent` | RN RS5 | CM8 |
| `ULocalFogVolumeComponent` | `LocalFogVolumeComponent` | 볼륨 렌더 스펙(문서 없음 — RN 10.2절 "볼륨 안개·볼륨 구름·지역 안개 볼륨은 후속이며 각 스펙이…") | CM11 |
| `UVolumetricCloudComponent` | `VolumetricCloudComponent` | 볼륨 렌더 스펙(문서 없음), 머티리얼 Volume 영역 | CM11 |
| `UHeterogeneousVolumeComponent` | `HeterogeneousVolumeComponent` | sparse volume texture 에셋, 렌더 기능 문서 | CM11 |
| `UWindDirectionalSourceComponent` | `WindSourceComponent` | UE는 SpeedTree만 쓴다(`EC/WindDirectionalSourceComponent.h:21`). 식생 문서가 바람 모델을 정할 때 | CM11 |

### 3.5 반사·캡처

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UReflectionCaptureComponent` | `ReflectionCaptureComponent` | RN RS7 | CM9 |
| `UBoxReflectionCaptureComponent` | `BoxReflectionCaptureComponent` | RN RS7 | CM9 |
| `USphereReflectionCaptureComponent` | `SphereReflectionCaptureComponent` | RN RS7 | CM9 |
| `UPlaneReflectionCaptureComponent` | `PlaneReflectionCaptureComponent` | RN RS7 | CM9 |
| `USceneCaptureComponent` | `SceneCaptureComponent` | CR15(추가 view·render target 에셋) | CM9 |
| `USceneCaptureComponent2D` | `SceneCaptureComponent2D` | CR15 | CM9 |
| `USceneCaptureComponentCube` | `SceneCaptureComponentCube` | CR15 | CM9 |
| `UPlanarReflectionComponent` | `PlanarReflectionComponent` | CR15, RN RS10 | CM9 |

### 3.6 표면·후처리·가상 텍스처

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UDecalComponent` | `DecalComponent` | RN RS7, 머티리얼 DeferredDecal 영역 | CM8 |
| `UPostProcessComponent` | `PostProcessComponent` | RN RS6, CR17(영역 형상) | CM8 |
| `URuntimeVirtualTextureComponent` | `VirtualTextureVolumeComponent` | 가상 텍스처 렌더 기능(문서 없음) | CM10 |
| `UBoundsCopyComponent` | `BoundsCopyComponent`(EditorOnly) | `VirtualTextureVolumeComponent` | CM10 |

### 3.7 정적 조명

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `ULightmassPortalComponent` | `LightBakePortalComponent` | 라이트맵·프로브 bake(EIP 14.10절), RN RS7 `StaticLighting::Baked`. 베이커만 읽는다 — UE도 정적 조명 빌드가 portal 변환을 베이커로 내보낼 뿐(`Editor/UnrealEd/Private/StaticLightingSystem/StaticLightingSystem.cpp:2316–2321`, `Editor/UnrealEd/Private/Lightmass/Lightmass.h:113–115`) 런타임 렌더러는 쓰지 않는다(render state는 편집 미리보기 상자뿐, `Engine/Private/Components/PortalComponent.cpp:79–89`) | CM10 |

### 3.8 오디오·햅틱·입력

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UAudioComponent` | `AudioComponent`(AU 3.8.3절이 정함) | CR20, CR11(리스너 = 플레이어 카메라 — AU 3.8.2절) | CM4 |
| `UForceFeedbackComponent` | `ForceFeedbackComponent` | CR21(햅틱 출력)·CR10(커브 에셋)·CR26(UE는 PlayerController가 force feedback 관리자를 돈다 — `Engine/Private/PlayerController.cpp:3464`) | CM4 |
| `UInputComponent` | `InputComponent` | CR21(액션·컨텍스트)·CR26(UE는 PlayerController의 입력 스택으로 동작 — `GameFramework/PlayerController.h:1707, 1764`) | CM4 |

`UInputComponent`의 키 상태 질의 함수 9개는 deprecated다(`EC/InputComponent.h:1100–1132` — "Use PlayerController..."). Rocks는 UE 5.8 기본 입력인 Enhanced Input 구조(입력 액션·매핑 컨텍스트 — 플러그인 `EnhancedInputComponent`가 `UInputComponent`를 상속한다)를 따르는 `InputComponent` 하나만 둔다(CD7). 축·액션 이름 문자열 바인딩은 만들지 않는다.

### 3.9 게임플레이 도구

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UChildActorComponent` | `ChildActorComponent` | CR9a(Prefab PF1 — 템플릿 Actor) | CM5 |
| `UTimelineComponent` | `TimelineComponent` | CR10(커브 에셋 — 구현), 이벤트는 `Event` 필드(CR7 해소) | CM5 |
| `USplineComponent` | `SplineComponent` | Core 스플라인 수학(`InterpCurve` 있음) | CM5 |
| `UInterpToMovementComponent` | `PathMoverComponent` | `MovementComponent`(8.4절)·CR16(sweep) | CM12(개정 4에서 CM5에서 옮김) |
| `UTextRenderComponent` | `TextRenderComponent` | CR18(글꼴), CM1a(PrimitiveComponent 렌더 경로)·CR14(글자 머티리얼 — `EC/TextRenderComponent.h:44, 54`) | CM5 |
| `UWorldPartitionStreamingSourceComponent` | `StreamingSourceComponent` | CR22(World Partition) | CM11 |

### 3.10 충돌 형상

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UBoxComponent` | `BoxComponent` | CR16 | CM3 |
| `USphereComponent` | `SphereComponent` | CR16 | CM3 |
| `UCapsuleComponent` | `CapsuleComponent` | CR16 | CM3 |
| (없음) | `CylinderComponent` | CR16 + [Cylinder](../CylinderShape.ko.md) CY2 — Rocks가 UE보다 하나 더 가진 형상(사용자 결정 2026-11-19) | CM3 |

### 3.11 시각화·디버그

에디터·개발 빌드 표시용이다. `LineBatchComponent`를 뺀 나머지는 EditorOnly(CR5 — 쿠킹에서 뺀다)다. 모두 PrimitiveComponent라 렌더 경로(CM1a·CR12)를 공통 선행으로 갖는다.

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UArrowComponent` | `ArrowComponent` | CR23(디버그 그리기) | CM2 |
| `UBillboardComponent` | `BillboardComponent` | CR23, 텍스처 에셋(A5) | CM2 |
| `UMaterialBillboardComponent` | `MaterialBillboardComponent` | CM2 + 머티리얼(CR14) | CM8 |
| `UDrawFrustumComponent` | `DrawFrustumComponent` | CR23 | CM2 |
| `UDrawSphereComponent` | `DrawSphereComponent` | CR23 + `SphereComponent`(CM3) | CM3 |
| `ULineBatchComponent` | `LineBatchComponent` | CR23 — World의 지속 디버그 선 묶음(개발 빌드) | CM2 |
