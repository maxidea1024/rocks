# 엔진 컴포넌트 도입 설계(UE 5.8 `Classes/Components` 대응)

기준일: 2026-11-23, 개정 3(Tick 단계·디버그 표시 결정 — 9절). 결정 CD1–CD12는 권장안으로 적었다(6절). 구현하지 않는다. 착수 시점은 고정 구현 순서(리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)를 따르며 이 문서는 그 순서를 바꾸지 않는다(5절).

이 문서는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)의 `Engine/Source/Runtime/Engine/Classes/Components` 폴더(헤더 73개)에 있는 컴포넌트를 Rocks에 들이는 범위와 순서를 정한다. 사용자 지시(2026-11-23): 기본은 전부 가져오되 폐기된 것과 BSP는 제외한다. 필수 컴포넌트 목록(3절)과 선행 작업 목록(4절)을 따로 둔다.

- 근거 문서: [엔진 런타임](EngineRuntime.ko.md)(이하 ER) 7절, [엔진 구현 계획](EngineImplementationPlan.ko.md)(이하 EIP) 10·13–16·18·20절, [렌더러](Renderer.ko.md)(이하 RN) 3·4·17절, [오디오](AudioSystem.ko.md)(이하 AU) 3.8·3.15절, [Prefab](Prefab.ko.md)(이하 PF) 5절, [Sparks 엔진 연동](SparksEngineIntegration.ko.md)(이하 SE) 2.3절, [메시·애니메이션 에셋](MeshAnimationAssets.ko.md)(이하 MA), [물리 어댑터](PhysicsJoltAdapter.ko.md), [Cylinder](CylinderShape.ko.md), [커브](CurveData.ko.md), [공간 색인](SpatialIndex.ko.md), [텍스처 스트리밍](TextureStreaming.ko.md).
- UE 경로 약어: `EC/` = `Engine/Source/Runtime/Engine/Classes/Components/`, `EE/` = `Engine/Source/Runtime/Engine/Classes/Engine/`.
- 좌표·단위·행렬은 UE 5.8과 같다(ER 7.3절). 이 문서의 모든 Transform·bounds·감쇠 거리는 cm다.

## 1. 범위

### 1.1 대상

`EC/`의 헤더 73개 중 컴포넌트 클래스를 정의한 헤더 66개다. 나머지 7개는 컴포넌트가 아니다(2.4절).

### 1.2 이 문서 밖(같은 방식으로 별도 문서가 다룰 것)

`EC/` 밖에도 필수 컴포넌트가 있다. 사용자 지시 범위가 이 폴더라 목록만 남긴다. 화면 마일스톤에 카메라가 필요하므로 `UCameraComponent`는 CM0과 함께 다뤄야 한다(4.3절 CR11).

| 위치 | 컴포넌트 | 메모 |
|---|---|---|
| `Classes/Camera` | `UCameraComponent`, `UCameraShakeSourceComponent` | 카메라는 화면 마일스톤 필수 |
| `Classes/GameFramework` | `UMovementComponent`·`UPawnMovementComponent`·`UNavMovementComponent`·`UCharacterMovementComponent`·`UProjectileMovementComponent`·`URotatingMovementComponent`·`UFloatingPawnMovement`·`USpectatorPawnMovement`, `USpringArmComponent` | 이동 계열. `UInterpToMovementComponent`(3.9절)의 부모 `UMovementComponent`도 여기 있다 |
| `Classes/PhysicsEngine`, `Classes/PhysicsField` | constraint·handle·spring·thruster·radial force·physical animation·cluster union·physics field | 물리 단계 |
| `Classes/Debug` | `UDebugDrawComponent` | 디버그 그리기 |
| `Classes/Particles` | `UParticleSystemComponent`(Cascade) | 제외 후보(CD5와 같은 이유) |
| Runtime 다른 모듈 | UMG `UWidgetComponent`(`Runtime/UMG/Public/Components/WidgetComponent.h`), Landscape, Foliage(`UFoliageInstancedStaticMeshComponent` — HISM 상속, `Runtime/Foliage/Public/FoliageInstancedStaticMeshComponent.h:20`), AIModule(Perception 등) | 각 기능 문서. Foliage는 CD2(HISM 흡수)의 영향을 받는다 — 식생 문서가 ISM 위에 다시 세운다 |
| 플러그인 | Niagara·Enhanced Input·AudioGameplayVolume 등 | 각 기능 문서 |

## 2. 분류 결과

### 2.1 요약

| 분류 | 수 | 뜻 |
|---|---|---|
| 필수 | 58 | Rocks에 같은 역할의 컴포넌트를 만든다(3절). 시점은 묶음 CM0–CM11(5절) |
| 대체 | 3 | 기능은 들이되 컴포넌트 형태가 아니다(2.3절) |
| 제외 | 4 | 들이지 않는다(2.2절) |
| 후속 프로필 | 1 | 계획상 P3 프로필이 열릴 때 다룬다(2.3절) |
| 컴포넌트 아님 | 7 헤더 | 2.4절 |

### 2.2 제외

| UE 클래스 | 근거 | 대신 |
|---|---|---|
| `UModelComponent` | BSP 기하의 렌더·충돌 표현(`EC/ModelComponent.h:27, 38`). 사용자 지시로 BSP 제외 | 정적 메시(모델링 도구는 에디터 문서) |
| `UBrushComponent` | BSP 빌드용 brush와 volume 형상을 겸한다(`EC/BrushComponent.h:16, 44`). BSP를 빼면 brush 편집 모델이 필요 없다 | volume 형상은 영역 형상 `VolumeShape`(CD1, CR17)로 대체 |
| `UVectorFieldComponent` | 쓰는 곳이 구 파티클(Cascade) FX 시스템뿐이다(`Private/Particles/FXSystem.cpp`·`FXSystemSet.cpp`, `VectorField/VectorFieldVolume.h`). Niagara는 이 컴포넌트를 쓰지 않는다. 클래스에 `UE_DEPRECATED` 표지는 없으나 UE5에서 Cascade는 Niagara로 대체된 계열이다 | VFX 문서가 Niagara식 vector field 데이터를 정할 때 다룬다 |
| `UPawnNoiseEmitterComponent` | UE가 스스로 legacy 컴포넌트로 표시한다(`Engine/Private/Actor.cpp:3402` — "legacy component. Use AISense_Hearing instead."). 짝인 `UPawnSensingComponent`도 5.5에서 deprecated다(`AIModule/Classes/Perception/PawnSensingComponent.h:20`). 기본 호환 모드에서 AI Perception에도 전달하지만(`EC/PawnNoiseEmitterComponent.h:24`) 그 길의 본체는 Perception의 Hearing 감각이다 | AI 문서의 Perception(소리 자극)이 맡는다 |

### 2.3 대체·후속 프로필

| UE 클래스 | 분류 | 근거 | Rocks |
|---|---|---|---|
| `UHierarchicalInstancedStaticMeshComponent` | 대체(CD2) | ISM 위에 CPU cluster tree(`FClusterNode`)로 컬링·LOD를 더한 것이다(`EC/HierarchicalInstancedStaticMeshComponent.h:71–141`). Rocks는 인스턴스 판정을 GPU 인스턴스 culling(RN RS4)과 RenderScene 계층 공간 해시(EIP 10.4절 "정적 mesh·instanced·foliage의 렌더 판정")가 맡는다 | `InstancedStaticMeshComponent` 하나. 대량 인스턴스 판정은 렌더러 몫 |
| `UApplicationLifecycleComponent` | 대체(CD3) | `FCoreDelegates`의 앱 수명(활성·비활성·저메모리 등)을 Blueprint에 노출하는 래퍼다 | 앱 수명 이벤트를 엔진 이벤트로 Sparks·C++에 직접 노출(AppInfo, SE EI 단계). Blueprint가 없으므로 래퍼 컴포넌트를 두지 않는다 |
| `UPlatformEventsComponent` | 대체(CD3) | 변환형 노트북의 노트북·태블릿 모드 조회·통지 래퍼다(`EC/PlatformEventsComponent.h:14–40`) | 같은 정보를 플랫폼 이벤트로 노출 |
| `UStereoLayerComponent` | 후속 프로필(CD4) | HMD 합성 layer(`EC/StereoLayerComponent.h:277`). 계획은 stereo/VR을 별도 P3 프로필로 둔다(EIP 14절 1352행) | VR 프로필 문서 |

### 2.4 컴포넌트가 아닌 헤더

| 헤더 | 내용 | Rocks |
|---|---|---|
| `ComponentInterfaces.h`, `ComponentInterfaceIterator.h`, `ActorPrimitiveComponentInterface.h`, `ActorStaticMeshComponentInterface.h`, `ActorSkinnedMeshComponentInterface.h` | UObject 컴포넌트가 아닌 primitive(대량 인스턴스 등)도 같은 질의로 다루기 위한 인터페이스 층 | 들이지 않는다. Rocks는 렌더·물리 상태를 시스템 쪽 표(`RenderScene`·물리 body 표)에 두고 컴포넌트는 변경만 보낸다(RN 4.2절). 같은 문제가 생기지 않는다 |
| `ExternalMorphSet.h` | skinned mesh 외부 morph target 묶음 | `SkinnedMeshComponent`(CM7)의 부속 |
| `UnregisterComponentContext.h` | 대량 등록 해제 묶음 도우미 | CR1(등록 묶음 처리)에서 필요하면 내부 도우미로 |

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
| `UTimelineComponent` | `TimelineComponent` | CR10(커브 에셋)·CR7(이벤트 slot 표) | CM5 |
| `USplineComponent` | `SplineComponent` | Core 스플라인 수학(`InterpCurve` 있음) | CM5 |
| `UInterpToMovementComponent` | `PathMoverComponent` | `MovementComponent`(1.2절, 이동 문서)·CR16(sweep) | CM5 |
| `UTextRenderComponent` | `TextRenderComponent` | CR18(글꼴), CM1a(PrimitiveComponent 렌더 경로)·CR14(글자 머티리얼 — `EC/TextRenderComponent.h:44, 54`) | CM5 |
| `UWorldPartitionStreamingSourceComponent` | `StreamingSourceComponent` | CR22(World Partition) | CM11 |

### 3.10 충돌 형상

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UBoxComponent` | `BoxComponent` | CR16 | CM3 |
| `USphereComponent` | `SphereComponent` | CR16 | CM3 |
| `UCapsuleComponent` | `CapsuleComponent` | CR16 | CM3 |
| (없음) | `CylinderComponent` | CR16 + [Cylinder](CylinderShape.ko.md) CY2 — Rocks가 UE보다 하나 더 가진 형상(사용자 결정 2026-11-19) | CM3 |

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

## 4. 선행 작업

이 절은 **컴포넌트를 만들기 전에 있어야 하는 것**이다. 상태는 2026-11-23 코드·문서 기준이다. "정본" 열의 문서가 그 작업을 정하고, 이 문서는 컴포넌트 쪽 요구만 적는다(요구가 정본에 없으면 "추가 요구"로 표시하고 7절에서 그 문서에 반영한다).

### 4.1 World 기반

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR1 | 컴포넌트 시스템 상태 수명: 등록 안에서 렌더·물리·오디오 상태를 만들고 지우는 hook과 "다시 보내기" 표시(UE `CreateRenderState`·`MarkRenderStateDirty`·`CreatePhysicsState` 대응). 등록 실패 시 부분 등록 해제. 대량 등록 묶음 | 없음 | `World/Component.h`에 `OnRegister`·`OnUnregister`만 있다 | EIP 20.2절(순서 Register 단계), 추가 요구 |
| CR2 | SceneComponent 보강: 부착 규칙(KeepRelative·KeepWorld·SnapToTarget — UE `EE/EngineTypes.h:62`), socket 이름 부착과 `GetSocketTransform`, 절대 location·rotation·scale, Mobility(Static·Stationary·Movable), 가시성(visible 자식 전파·hiddenInGame), 속도, bounds 계산 가상 함수와 bounds 갱신, Transform 변경 통지(구독형 — CR7) | 없음 | `World/SceneComponent.h`는 상대 Transform·계층·월드 행렬만 있다 | ER 7.3절, 추가 요구 |
| CR3 | 부착 계층 영속(부모 WeakObjectRef PROPERTY) | 없음(자리) | ER 14절 "부착 계층은 저장되지 않고" | PF 5.1절 World 단계, 리플렉션 B9d1 |
| CR4 | Component 시작 준비 hook(`IsReadyToStart`) — 메시·텍스처 로드 대기 | 없음 | SE-W2 | ER 7.2절, SE-W2 |
| CR5 | EditorOnly 컴포넌트와 쿠킹 제외, 태그(`TagSet`) | 없음 | `World/Component.h`에 없다. A0-B `EditorOnly` 지정자는 필드 단위다. 객체 단위 EditorOnly는 패키지 Exports 플래그로 예약만 됐고 클래스 지정자가 없으며 reader는 그 비트를 `CorruptPackage`로 거부한다(AssetSystem 243행 — 켜려면 schemaVersion을 올린다). 태그 사전은 계획만 있다 | EIP 20.1절(태그), AssetSystem 패키지 형식 개정, MetaCompiler 클래스 지정자 |
| CR6 | 컴포넌트 타입 규칙(Actor당 하나·필수 동반·상호 배타)을 타입 메타로 | 없음 | — | EIP 20.2절 |
| CR7 | 이벤트 slot 표(overlap·hit·타임라인·재생 끝 등 컴포넌트 이벤트를 구독이 있을 때만 저장) | 없음 | SE-W8 | SE-W8, EIP 20.2절 |
| CR8 | World 필드 편집 setter·통지(CU5) — 속성을 바꾸면 CR1의 "다시 보내기" 표시 | 없음 | ER 14절 "World 필드는 ReadOnly" | ReflectionExtensions CU5 |
| CR8a | Tick 단계 UE 대응: Rocks 단계(PrePhysics·Physics·PostPhysics·LateUpdate·PreRender)와 UE `ETickingGroup`(`EE/EngineBaseTypes.h:86–104`) 대응, 물리 결과 반영·skeletal mesh 물리 마무리 지점 | 결정(CD11) — 구현은 물리·애니메이션 컴포넌트와 함께 | ER 8절 대응표 | ER 8절 |

### 4.2 에셋·데이터

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR9 | 에셋 런타임 A1–A5: 패키지·로더·`AssetLease`(WorldOnly 클래스 멤버 허용 — A0-B), 클래스 기본값 레코드(A5). 컴포넌트가 메시·머티리얼·사운드를 필드로 잡는 전제 | A1–A3 완료, A4 진행(A4c2까지) | 메모리·AssetSystem 1절 | [에셋 시스템](AssetSystem.ko.md) |
| CR9a | Prefab PF1(클래스 원형·default subobject) — 컴포넌트를 클래스 원형으로 선언하고 `ChildActorComponent`가 템플릿 Actor를 갖는 전제 | 설계만 | PF 5.2절 | PF |
| CR10 | 커브 에셋(Curve 에셋 층 — 타임라인·햅틱 효과·감쇠 커브) | 값 타입 층 완료(CV1–CV10), 에셋 층은 A1 뒤 | 메모리 curve-data-spec | CurveData |

### 4.3 렌더

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR11 | 카메라·view: `CameraComponent`(1.2절)와 `ViewDesc` 생성 | 없음 | — | RN 5.1절, 별도 문서 |
| CR12 | `WorldIntegration`·`RenderData`: 컴포넌트 변경 감지 → `SceneChangeList`(인스턴스·광원 추가·제거·변환·재질), `RenderNotices` 처리. 인스턴스 종류(정적 메시·인스턴스드·skinned·sprite) 등록 | 없음(모듈 없음) | RN 3.2·4.2절 | RN |
| CR13 | 렌더러 단계 RS0(정적 메시·Unlit)·RS1(광원)·RS3(그림자)·RS5(투명·하늘·안개)·RS6(후처리)·RS7(capture·decal·bake 표본)·RS9(rect)·RS10(screen-space)·RS12(skinned) | 설계만(화면 마일스톤 뒤) | RN 17절 | RN |
| CR14 | 머티리얼 런타임: section별 머티리얼 `RenderHandle`, 인스턴스 파라미터 override, 실행 중 머티리얼 인스턴스(UE `UMaterialInstanceDynamic` 대응) | MS3a 골격만 | 메모리 mesh-unreal-exchange-spec | [머티리얼 시스템](MaterialSystem.ko.md) |
| CR14a | 텍스처 스트리밍 입력: 컴포넌트가 내는 텍스처 사용 기록(UV 밀도·화면 크기 — UE `FRenderAssetOwnerStreamingState`) | 설계만 | TextureStreaming 4.1절 | TextureStreaming |
| CR15 | 추가 view와 render target 에셋(scene capture·planar reflection) | 없음 | — | RN 5절, AssetSystem 텍스처 계열 |
| CR23 | 디버그·편집 표시: 선·도형·아이콘 billboard(EditorOnly 시각화 포함) | 결정(CD12), 설계 RN RS13 — 코드 없음 | `Sandbox` 샘플 렌더러뿐(ER 3절). UE는 line batch proxy가 프레임마다 동적 메시 요소를 내는 렌더러 경로다(`Engine/Private/Components/LineBatchComponent.cpp:60`) | RN 4.2절 "디버그·편집 표시", 11.2절 PC9a·PC9b, 17절 RS13 |
| CR18 | 글꼴 에셋·글리프 atlas(`TextRenderComponent`) | 없음 | — | [UI 툴킷](UiToolkit.ko.md) 글꼴 |

### 4.4 물리

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR16 | 물리 모듈(Jolt): body(UE `FBodyInstance` 대응), 충돌 채널·응답·프로필, overlap·hit 이벤트, sweep 이동(UE `MoveComponent`), scene query, simulate physics, ragdoll용 physics asset | 없음(모듈 없음) | `Engine/Source/Runtime`에 Physics 모듈이 없다. 어댑터 문서는 Jolt 사실을 아직 확인하지 않은 전제 단계 | EIP 16절, PhysicsJoltAdapter, CylinderShape CY2 |
| CR17 | 영역 형상 `VolumeShape`(상자·구·캡슐·볼록체)와 물리 없는 내부 판정 — brush volume 대체(CD1). 후처리 볼륨·리버브 존·트리거 아닌 영역이 같은 형상과 판정을 쓴다 | 오디오 문서에만 있음 | AU 3.15.2절 "영역 형상(상자·구·볼록체)… 물리 엔진은 쓰지 않는다" | 추가 요구(공용화) |

### 4.5 애니메이션·오디오·입력·큰 월드

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR19 | 애니메이션 런타임: pose 평가·blend·본 transform·소켓·root motion·LOD별 본 맵, skinning 데이터 전달 | 에셋만(`Animation` 모듈은 Skeleton·Clip 에셋) | `Engine/Source/Runtime/Animation/Public` | EIP 15절, MA 6·7절 |
| CR20 | 오디오 엔진 연결 AU3(`AudioWorldSubsystem`·emitter 등록) | 설계만 | AU 3.8절 | AU |
| CR21 | 입력 액션·매핑 컨텍스트·리바인딩, 햅틱 출력(XInput 진동 등) | 없음(프레임 입력 상태만) | ER 9절 "액션 매핑… 이번 범위가 아니다" | EIP 20.4절 |
| CR22 | World Partition(셀·streaming source·data layer) | 없음 | SE-W7 | EIP 10.2절 |
| CR26 | 게임 프레임워크: PlayerController·LocalPlayer(입력 컴포넌트 스택, 햅틱 효과 관리, 주 리스너·view 소유) | 없음 | ER 3절 표에 `Gameplay` 모듈이 있으나 `Engine/Source/Runtime`에 없다(ER 1절 S10–S12 미착수) | ER 1절 S10–S12, EIP 20.4·20.5절 |
| CR24 | 복제(컴포넌트 복제·서브오브젝트) | 없음 | — | EIP 17절. 컴포넌트 도입을 막지 않는다 — 필드에 복제 표지를 둘 자리만 남긴다 |
| CR25 | Sparks 바인딩(컴포넌트 API 노출) | 없음 | SE 3절 EI 단계 | SE. C++ 구현을 막지 않는다 |

### 4.6 선행 → 묶음 대응

| 묶음 | 막는 선행 |
|---|---|
| CM0 기반 보강(+카메라) | CR1·CR2·CR3·CR4·CR5·CR6·CR8a·CR11(CR7·CR8은 있으면 쓰고 없으면 자리 — 3.1절 `Component`의 CR8도 같다) |
| CM1a 정적 메시 | CM0, CR9, CR12, CR13(RS0), CR14, CR14a |
| CM1b 광원 | CM1a, CR13(RS1·RS3) |
| CM2 시각화 | CM1a, CR5, CR12, CR23(RN RS13) |
| CM3 충돌 형상 | CM0, CR16, CR17, CR7. `DrawSphereComponent`만 CM2(CR23)도 |
| CM4 오디오·햅틱·입력 | CM0, CR10, CR11, CR20, CR21, CR26 |
| CM5 게임플레이 도구 | CM0, CR9a, CR10, CR7, CR16(`PathMoverComponent`의 sweep), CR18·CM1a·CR14(`TextRenderComponent`), 이동 컴포넌트 문서 |
| CM6 인스턴싱·스플라인 메시 | CM1a, CM5(스플라인), RN RS4 |
| CM7 skinned 계열 | CM1a, CR19, RN RS12, CR16(`SkeletalMeshComponent`의 ragdoll·physics asset — 없으면 그 기능만 후속) |
| CM8 대기·후처리·decal·rect | CM1b, RN RS5·RS6·RS7·RS9, CR17 |
| CM9 반사·캡처 | CM1b, CR15, RN RS7·RS10 |
| CM10 정적 조명·가상 텍스처 | CM1b, 라이트맵 bake, 가상 텍스처 문서 |
| CM11 큰 월드·후속 | 컴포넌트별: `StreamingSourceComponent`는 CR22, `InstancedSkinnedMeshComponent`는 CM7·CR19·애니메이션 bank, 볼륨 안개·구름 두 개는 볼륨 렌더 스펙, 나머지는 각 기능 문서 |

## 5. 묶음 순서

- 고정 순서를 바꾸지 않는다. CM0은 World 단계 작업이라 렌더를 기다리지 않지만, 착수는 A6 뒤 화면 마일스톤 첫 단계로 둔다(화면 출력을 앞당기는 제안이 아니다). 화면 마일스톤에 드는 것은 CM0과 카메라(CR11)까지다.
- 정식 렌더러는 화면 마일스톤 다음이고 첫 단계가 RS0, 그다음 RS1이다(RN 17절 841행). 그래서 CM1a(정적 메시)는 RS0과, CM1b(광원)는 RS1·RS3과 함께 한다.
- CM2 이후는 각 선행(렌더 RS 단계·물리·애니메이션·오디오 AU3·입력)이 들어오는 시점에 그 기능 단계와 함께 한다. 컴포넌트만 먼저 만들어 "동작하는 척"하지 않는다(시스템이 없으면 그 컴포넌트를 만들지 않는다).
- 단계마다 완료 조건:
  1. 수명 순서 헤드리스 시험: 등록 → Awake → Enable → Start → Disable → Destroy → 해제에서 시스템 상태 생성·삭제가 한 번씩, 등록 실패 시 부분 등록 해제(CR1).
  2. 변경 목록 결정성: 같은 World 조작 열 → 같은 `SceneChangeList` 열(렌더 계열), 같은 물리 명령 열(충돌 계열).
  3. Transform·부착: 부착 규칙 셋 × 절대 플래그 셋 × 부모 비균일 scale 조합, socket 부착, Mobility 위반(Static 이동) 진단.
  4. GPU 계열은 RN 17절의 해당 RS 시험에 컴포넌트 경로를 더한다(기준 영상은 RS 단계 것을 그대로).
  5. UE 대조표(7절)의 속성 중 들이지 않은 것은 사유를 적는다.

## 6. 결정(권장안)

| ID | 항목 | 권장안 | 근거 |
|---|---|---|---|
| CD1 | BSP와 brush | `UModelComponent`·`UBrushComponent` 제외. volume 형상은 `VolumeShape`(상자·구·캡슐·볼록체)로, 볼록체는 메시 에셋 단순 충돌(MA 4.6절)에서 가져온다. 트리거처럼 물리 이벤트가 필요한 영역은 충돌 형상 컴포넌트(CM3)를 쓴다 | 사용자 지시. 오디오 리버브 존이 이미 같은 형상을 정했다(AU 3.15.2절) |
| CD2 | HISM | 별도 클래스 없이 `InstancedStaticMeshComponent`로 흡수 | 2.3절. 인스턴스 컬링은 렌더러 몫 |
| CD3 | 앱 수명·플랫폼 이벤트 래퍼 | 컴포넌트 없이 엔진 이벤트로 노출 | Blueprint가 없다(EIP 20.4절) |
| CD4 | Stereo layer | VR P3 프로필 문서로 미룸 | EIP 14절 |
| CD5 | Cascade vector field·pawn noise | 제외 | 2.2절 |
| CD6 | 이름 | 업계 공용 이름(StaticMesh·PointLight·Decal·SkyAtmosphere 등)은 그대로 쓰고 UE 고유 이름은 바꾼다: `ExponentialHeightFog` → `HeightFog`, `LightmassPortal` → `LightBakePortal`(Lightmass는 UE 정적 조명 베이커 이름이다. 베이크 전용이라는 뜻을 남기고 계획 14.10절의 Bake 어휘에 맞춘다. `LightPortal`은 런타임 광원처럼 읽히고, `LightmapPortal`은 프로브 bake도 쓰는 범위를 좁히며, V-Ray식 `SkyPortal`은 베이크 전용임이 드러나지 않아 버렸다 — 사용자 지적 2026-11-23), `WorldPartitionStreamingSource` → `StreamingSource`, `InterpToMovement` → `PathMover`, `RuntimeVirtualTexture` → `VirtualTextureVolume`, `LODSync` → `LodSync`, `WindDirectionalSource` → `WindSource`. `Component`(UE `UActorComponent`)는 이미 정한 이름 | Rocks 이름 규칙(UE 구조, Rocks 이름) |
| CD7 | 입력 컴포넌트 | Enhanced Input 구조(액션·매핑 컨텍스트) 하나. 옛 축·액션 이름 바인딩 없음 | 3.8절, 하위호환 없음 원칙 |
| CD8 | 활성(Activate) | UE의 Activate·Deactivate·`bAutoActivate`는 Rocks `enabled`·`OnEnable`·`OnDisable`로 대응(EIP 20.2절 Activate→Enable). 별도 활성 상태를 두지 않는다. "등록됐지만 재생 안 함" 같은 상태는 컴포넌트 속성(예: 오디오 자동 재생)으로 | 상태 하나를 줄인다. UE 대응은 계획 20.2절이 이미 정했다 |
| CD9 | AssetUserData | 들이지 않는다. 필요한 확장 데이터는 리플렉션 필드나 별도 컴포넌트로 | 플러그인용 임의 부착 데이터로, 타입 규칙(CR6)과 겹친다. 요구가 생기면 다시 본다 |
| CD10 | Mobility | UE와 같은 셋(Static·Stationary·Movable). Static 이동은 거부(진단), 정적 조명·캐시 그리기 항목과 연동 | UE와 같게(애매하면 UE 방식) |
| CD11 | Tick 단계(CR8a) | 단계를 더하지 않는다. UE `TG_PrePhysics` → PrePhysics(컴포넌트 기본도 PrePhysics — UE 컴포넌트 기본 `TG_DuringPhysics`는 대응 단계가 없고 관찰 결과가 같다), `TG_StartPhysics`·`TG_EndPhysics` → Physics phase(엔진 전용), `TG_PostPhysics` → PostPhysics, 카메라 갱신 + `TG_PostUpdateWork` → LateUpdate(시작에 엔진 카메라 갱신), end-of-frame 갱신 → PreRender. 옛 PhysicsSync·AnimationFinalize는 Physics phase 끝의 엔진 단계 둘(물리 결과 반영 → skeletal mesh 물리 마무리)이다. 상세 대응표는 ER 8절 | UE 그룹 순서를 그대로 지키면서 Rocks 물리가 Physics phase에서 동기로 돈다는 점만 다르다. "물리와 병렬" 등록 종류는 비동기 물리·병렬 Tick 때 다시 본다(사용자 지시로 권장안 확정 2026-11-23) |
| CD12 | 디버그·편집 표시(CR23) | UE와 같은 세 경로: 디버그 그리기 빌드(Debug·Development·에디터)는 디버그 전용 수집 → 후처리 합성 PC9b, 에디터 view의 편집 표시는 PC9a, Shipping은 base pass 불투명 단순 요소. 자리는 tone map·spatial AA 뒤, spatial upscale 앞. 깊이 우선 그룹 World·Foreground. 렌더러 단계 RS13(선행 RS0, RS6에서 정식 자리) — 에디터 뷰포트 단계를 기다리지 않는다 | UE `UE_ENABLE_DEBUG_DRAWING`·`EPass` 순서(RN 4.2·11.2절 근거). 개발 빌드 디버그 선은 에디터 전에도 필요하다(물리·AI 디버깅). 사용자 지시로 권장안 확정 2026-11-23 |

## 7. 다른 문서 개정

개정 2(2026-11-23)에서 모두 반영했다. 요구의 정본은 여전히 이 문서 4절이고, 각 문서에는 요구 요약과 링크만 넣었다.

| 문서 | 고친 것 |
|---|---|
| ER 7.3·8절 | 7.3절에 보강 예정 항목(CR1 시스템 상태 hook, CR2 SceneComponent 보강), 8절에 Tick 단계 대조 미결(CR8a) |
| EIP 20.2절 | tick group 문장을 구현된 ER 8절 단계 기준으로 고치고, 물리 결과 반영·애니메이션 마무리 지점은 CR8a에서 정한다고 적음 |
| RN 3.2·4.2절 | `WorldIntegration` 행에 이 문서 3절 링크, 4.2절에 "디버그·편집 표시"(CR23). 처음 표의 RenderBase 대상은 잘못이었다 — UE 디버그 선은 RenderCore가 아니라 렌더러의 동적 메시 경로다 |
| AU 3.15.2절 | 영역 형상을 공용 `VolumeShape`로(CR17) |
| AssetSystem 5.7절 4항 | 객체 단위 EditorOnly 활성 시점을 컴포넌트 도입(CR5)으로 연결 |
| ER 3절 | `Gameplay` 행에 미착수와 입력·햅틱 컴포넌트 선행(CR26) |
| PhysicsJoltAdapter | 관련 문서에 컴포넌트 쪽 요구(CR16) 링크 |

## 8. 확인하지 못한 것

- UE 컴포넌트 속성 전체 대조표(필드 단위)는 아직 만들지 않았다. 각 묶음 착수 때 그 묶음의 UE 헤더를 필드 단위로 대조해 이 문서에 붙인다(5절 완료 조건 5).
- `UInstancedSkinnedMeshComponent`·`UHeterogeneousVolumeComponent`의 UE 5.8 성숙도(실험 기능 여부)는 헤더만으로 판정하지 못했다(헤더에 실험 표지는 없다).
- Cascade의 공식 deprecated 표지는 `UVectorFieldComponent`·`UParticleSystemComponent` 헤더에 없다. 제외 근거는 사용처(구 FX 시스템 전용)다.

## 9. 개정 기록

- 개정 3(2026-11-23): 남아 있던 두 항목을 권장안으로 확정(사용자 지시) — CD11 Tick 단계 UE 대응(ER 8절 대응표, EIP 20.2절), CD12 디버그·편집 표시(RN 4.2절 세 경로, 11.2절 PC9a·PC9b, 17절 RS13). CR8a·CR23 상태를 결정으로.
- 개정 2(2026-11-23): 7절 다른 문서 개정 반영. CR23 정본을 RenderBase에서 Renderer 4.2절로 정정. `LightmassPortal` 대응 이름을 `LightPortal`에서 `LightBakePortal`로(사용자 지적 — 베이크 전용 의미, CD6).
- 개정 1(2026-11-23): 리뷰어 1명 교차검증 반영 — CM1을 CM1a(정적 메시, RS0)·CM1b(광원, RS1·RS3)로 나누고 화면 마일스톤 범위를 CM0+카메라로 정정, 게임 프레임워크 선행 CR26 추가(입력·햅틱), 볼륨 안개·구름을 CM11로, CR5 근거(객체 단위 EditorOnly 미활성), 3절과 4.6절 선행 불일치·ID 오기 정정, PawnNoiseEmitter·HISM·InputComponent 인용 정정, 1.2절 Runtime 다른 모듈(UMG·Foliage·Landscape·AIModule).
- 개정 0(2026-11-23): 첫 초안. 분류(필수 58·대체 3·제외 4·후속 프로필 1), 선행 CR1–CR25, 묶음 CM0–CM11, 결정 CD1–CD10 권장안.
