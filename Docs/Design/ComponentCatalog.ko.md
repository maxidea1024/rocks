# 엔진 컴포넌트 도입 설계(UE 5.8 컴포넌트 대응)

기준일: 2026-11-23, 개정 6(3부 플러그인 컴포넌트 — 9절, 개정 기록 11절). 결정 CD1–CD24는 권장안으로 적었다(6·8.7·9.8절). 구현하지 않는다. 착수 시점은 고정 구현 순서(리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)를 따르며 이 문서는 그 순서를 바꾸지 않는다(5절).

이 문서는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)의 `Engine/Source/Runtime/Engine/Classes/Components` 폴더(헤더 73개)에 있는 컴포넌트를 Rocks에 들이는 범위와 순서를 정한다. 사용자 지시(2026-11-23): 기본은 전부 가져오되 폐기된 것과 BSP는 제외한다. 필수 컴포넌트 목록(3절)과 선행 작업 목록(4절)을 따로 둔다. 1–7절(1부)은 이 폴더, 8절(2부)은 폴더 밖(Engine 모듈의 다른 폴더, Runtime의 다른 모듈), 9절(3부)은 엔진 플러그인이다.

- 근거 문서: [엔진 런타임](EngineRuntime.ko.md)(이하 ER) 7절, [엔진 구현 계획](EngineImplementationPlan.ko.md)(이하 EIP) 10·13–16·18·20절, [렌더러](Renderer.ko.md)(이하 RN) 3·4·17절, [오디오](AudioSystem.ko.md)(이하 AU) 3.8·3.15절, [Prefab](Prefab.ko.md)(이하 PF) 5절, [Sparks 엔진 연동](SparksEngineIntegration.ko.md)(이하 SE) 2.3절, [메시·애니메이션 에셋](MeshAnimationAssets.ko.md)(이하 MA), [물리 어댑터](PhysicsJoltAdapter.ko.md), [Cylinder](CylinderShape.ko.md), [커브](CurveData.ko.md), [공간 색인](SpatialIndex.ko.md), [텍스처 스트리밍](TextureStreaming.ko.md).
- UE 경로 약어: `EC/` = `Engine/Source/Runtime/Engine/Classes/Components/`, `EE/` = `Engine/Source/Runtime/Engine/Classes/Engine/`.
- 좌표·단위·행렬은 UE 5.8과 같다(ER 7.3절). 이 문서의 모든 Transform·bounds·감쇠 거리는 cm다.

## 1. 범위

### 1.1 대상

`EC/`의 헤더 73개 중 컴포넌트 클래스를 정의한 헤더 66개다. 나머지 7개는 컴포넌트가 아니다(2.4절).

### 1.2 폴더 밖(8절에서 다룬다)

`EC/` 밖에도 필수 컴포넌트가 있다. 개정 4에서 8절(2부)로 정리했다. 아래 표는 위치 안내다. 화면 마일스톤에 카메라가 필요하므로 `UCameraComponent`는 CM0과 함께 다룬다(4.3절 CR11).

| 위치 | 컴포넌트 | 메모 |
|---|---|---|
| `Classes/Camera` | `UCameraComponent`, `UCameraShakeSourceComponent` | 카메라는 화면 마일스톤 필수 |
| `Classes/GameFramework` | `UMovementComponent`·`UPawnMovementComponent`·`UNavMovementComponent`·`UCharacterMovementComponent`·`UProjectileMovementComponent`·`URotatingMovementComponent`·`UFloatingPawnMovement`·`USpectatorPawnMovement`, `USpringArmComponent` | 이동 계열. `UInterpToMovementComponent`(3.9절)의 부모 `UMovementComponent`도 여기 있다 |
| `Classes/PhysicsEngine`, `Classes/PhysicsField` | constraint·handle·spring·thruster·radial force·physical animation·cluster union·physics field | 물리 단계 |
| `Classes/Debug` | `UDebugDrawComponent` | 디버그 그리기 |
| `Classes/Particles` | `UParticleSystemComponent`(Cascade) | 제외(8.3절, CD5와 같은 이유) |
| Runtime 다른 모듈 | UMG `UWidgetComponent`(`Runtime/UMG/Public/Components/WidgetComponent.h`), Landscape, Foliage(`UFoliageInstancedStaticMeshComponent` — HISM 상속, `Runtime/Foliage/Public/FoliageInstancedStaticMeshComponent.h:20`), AIModule(Perception 등) | 각 기능 문서. Foliage는 CD2(HISM 흡수)의 영향을 받는다 — 식생 문서가 ISM 위에 다시 세운다 |
| 플러그인 | Niagara·Enhanced Input·AudioGameplayVolume·GameplayAbilities·PCG 등 | 9절(3부) |

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
| `UInterpToMovementComponent` | `PathMoverComponent` | `MovementComponent`(8.4절)·CR16(sweep) | CM12(개정 4에서 CM5에서 옮김) |
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
| CM5 게임플레이 도구 | CM0, CR9a, CR10, CR7, CR18·CM1a·CR14(`TextRenderComponent`). `PathMoverComponent`는 CM12(8.6절) |
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

## 8. 폴더 밖 컴포넌트(2부)

`EC/` 밖의 컴포넌트를 1–6절과 같은 방식으로 정한다(사용자 지시 2026-11-23). ID는 1부에 이어 붙인다(선행 CR27–, 묶음 CM12–, 결정 CD13–). UE 경로 약어: `RT/` = `Engine/Source/Runtime/`.

### 8.1 범위

| 대상 | 방식 |
|---|---|
| Engine 모듈의 다른 폴더(`RT/Engine/Classes/Camera`·`GameFramework`·`PhysicsEngine`·`PhysicsField`·`Debug`·`Particles`·`Interfaces`, `RT/Engine/Public/…`) | 클래스 하나하나 분류 |
| Runtime의 다른 모듈(AIModule·NavigationSystem·UMG·Landscape·Foliage·AudioMixer·CinematicCamera·MediaAssets·GameplayTasks·GameplayDebugger·GeometryFramework·InteractiveToolsFramework·AugmentedReality·HeadMountedDisplay·MRMesh·Renderer·Dataflow·Experimental 등) | 클래스 하나하나 분류 |
| 엔진 플러그인(`Engine/Plugins`) | 9절(3부)에서 클래스 하나하나 분류(개정 6 — CD13을 CD19로 대체) |
| 시험 전용 클래스(`RT/Engine/Private/Tests/…`의 `UAutoRTFMTest*`·`USSAMTestComponent`) | 대상 아님 |

수집 방법: 위 경로의 헤더에서 이름이 `…Component`이거나 컴포넌트 계열(`UActorComponent`·`USceneComponent`·`UPrimitiveComponent`·`U…MovementComponent` 등)을 상속한 `class` 선언을 모두 모았다. 같은 헤더에 여러 클래스가 있으면 모두 센다. 개정 6에서 3부와 같은 상속 추적(9.1절)으로 다시 대조해, 선언에 `UE_DEPRECATED(…)`가 끼어 빠졌던 4종을 제외에 더했다.

### 8.2 분류 요약

| 분류 | 수 | 절 |
|---|---|---|
| 필수 | 56 | 8.4절. 묶음 CM0·CM2·CM12–CM21 |
| 대체 | 2 | 8.3절 |
| 제외 | 8 | 8.3절 |
| 후속(P3·프로필·별도 기능 문서) | 38(+ Chaos field 보조 클래스 25) | 8.3절 |
| 편집기 쪽 | 9 | 8.3절. 런타임 카탈로그 밖, 에디터 문서 |
| 컴포넌트 아님 | 13 | 8.3절 |

### 8.3 제외·후속·편집기·컴포넌트 아님

**제외**

| UE 클래스 | 근거 | 대신 |
|---|---|---|
| `UParticleSystemComponent`(`RT/Engine/Classes/Particles/ParticleSystemComponent.h`) | 구 파티클(Cascade) 컴포넌트. CD5와 같은 이유 | VFX 효과 컴포넌트(CD16) |
| `UPawnSensingComponent` | 5.5에서 deprecated(`RT/AIModule/Classes/Perception/PawnSensingComponent.h:20` — "AI Perception should be used instead") | `AIPerceptionComponent` |
| `UGridPathFollowingComponent` | `UCLASS(… Experimental …)`(`RT/AIModule/Classes/Navigation/GridPathFollowingComponent.h:20`) | `PathFollowingComponent` |
| `UDEPRECATED_UWorldPartitionDestructibleHLODMeshComponent` | 클래스 이름이 deprecated(`RT/Engine/Public/WorldPartition/HLOD/DestructibleHLODComponent.h`) | — |
| `UAtmosphericFogComponent`(`RT/Engine/Classes/Atmosphere/AtmosphericFogComponent.h:21`) | `UE_DEPRECATED(4.26, "Please use the SkyAtmosphere component instead.")` | `SkyAtmosphereComponent`(1부 3.4절) |
| `ULandscapeMeshCollisionComponent_DEPRECATED`(`RT/Landscape/Classes/LandscapeMeshCollisionComponent.h:24`) | `UE_DEPRECATED(5.7, …)` — edit layer 없는 지형 제거와 함께 폐기 | `LandscapeCollisionComponent` |
| `UGeometryCollectionISMPoolComponent`·`UGeometryCollectionISMPoolDebugDrawComponent`(`RT/Experimental/GeometryCollectionEngine/Public/GeometryCollection/GeometryCollectionISMPoolComponent.h:349`, `GeometryCollectionISMPoolDebugDrawComponent.h:13`) | `UE_DEPRECATED(5.6, "… please use UISMPoolComponent instead.")` | `UISMPoolComponent`(후속 — 파괴, CD14) |

**대체**

| UE 클래스 | 근거 | Rocks |
|---|---|---|
| `USynthComponent`(`RT/AudioMixer/Public/Components/SynthComponent.h`) | 오디오 문서가 채택하지 않는 UE 요소로 정했다(AU 9.1절) | 절차 오디오는 `AudioComponent`가 재생하는 `SoundGenerator` source 입력(AU10, CR34) |
| `UMediaSoundComponent`(`RT/MediaAssets/Public/MediaSoundComponent.h:150–152`, `USynthComponent` 파생) | 미디어 플레이어 소리 출력용 synth 컴포넌트다. 부모를 들이지 않으므로 같은 길로 간다 | 미디어 소리는 `MediaComponent`(CM17)가 `AudioComponent`로 `SoundGenerator` source(CR34)를 재생한다 |

**후속**(필요한 기능이 계획상 뒤이거나 별도 문서가 정한다. 컴포넌트로 들이는 것 자체는 그 문서가 정한다)

| 묶음 | UE 클래스 | 수 | 근거 | 넘길 곳 |
|---|---|---|---|---|
| XR | `UARComponent`과 그 파생 10종(Plane·Point·Face·Image·QRCode·Pose·EnvironmentProbe·Object·Mesh·GeoAnchor), `UARLifeCycleComponent`, `UARTrackableNotifyComponent`(`RT/AugmentedReality/Public/`), `UMotionControllerComponent`(`RT/HeadMountedDisplay`), `UMRMeshComponent`·`UMockDataMeshTrackerComponent`(`RT/MRMesh/Public/MockDataMeshTrackerComponent.h:31–33`) | 16 | stereo/VR은 P3 프로필(EIP 14절 1352행), CD4와 같다 | VR·AR 프로필 문서 |
| 파괴(Chaos) | `UGeometryCollectionComponent`·`UGeometryCollectionDebugDrawComponent`·`UFieldSystemComponent`(+ field 보조 클래스 25종: `UFieldNodeBase`와 파생 20, `UFieldSystemMetaData`와 파생 3 — `RT/Experimental/FieldSystem/Source/FieldSystemEngine/Public/Field/FieldSystemObjects.h`)·`UPhysicsFieldComponent`·`UClusterUnionComponent`·`UClusterUnionReplicatedProxyComponent`·`UChaosEventListenerComponent`·`UChaosGameplayEventDispatcher`·`UChaosDestructionListener`·`UChaosDebugDrawComponent`·`UISMPoolComponent`·`UISMPoolDebugDrawComponent`·`UDataflowDebugDrawComponent`·`UWorldPartitionDestructibleHLODComponent` | 14 | 계획은 destruction을 P3로 둔다(EIP 21절 "groom·destruction·고급 fluid는 P3"). Rocks 물리는 Jolt라 Chaos 구현 고유 구조를 그대로 들일 수 없다 | 파괴 문서(CD14) |
| 네트워크 물리 | `UAsyncPhysicsInputComponent`·`UNetworkPhysicsComponent`·`UNetworkPhysicsSettingsComponent`(`RT/Engine/Public/Physics/`) | 3 | Chaos 비동기 물리·물리 복제 전용(`NetworkPhysicsComponent.h:11`이 `Physics/Experimental/PhysScene_Chaos.h`를 쓴다) | 네트워크 단계(EIP 17절)와 물리 문서 |
| 음성 | `UVOIPTalker` | 1 | 온라인 음성 채팅 | 온라인 서비스 문서 |
| 내비 그래프 | `UNavigationGraphNodeComponent`(`RT/NavigationSystem/Public/NavGraph/`) | 1 | navmesh가 아닌 일반 그래프 내비 데이터. 1차 내비(navmesh — EIP 19절) 밖 | 내비 문서가 그래프 데이터를 들일 때 |
| 가상 지오메트리 | `ULandscapeNaniteComponent` | 1 | VirtualGeometry(EIP 13.2절) 선행 | 지형 문서 + VirtualGeometry |
| 볼륨 표시 | `USparseVolumeTextureViewerComponent`(`RT/Renderer/Private/SparseVolumeTexture/`) | 1 | sparse volume texture 확인 도구 | `HeterogeneousVolumeComponent`(CM11)와 함께 |
| 절차 식생 | `UProceduralFoliageComponent`(`RT/Foliage/Public/ProceduralFoliageComponent.h`) | 1 | UE에서 실험 기능이다 — 편집기 실험 설정 `bProceduralFoliage`로 켠다(`Engine/Source/Editor/UnrealEd/Classes/Settings/EditorExperimentalSettings.h:56–58`). 클래스 표지가 아니라 설정이라 헤더만으로는 보이지 않는다 | PCG·식생 문서(EIP 24.1절 PCG P2/P3·M7–M8) |

**편집기 쪽**(9): `UGizmoBaseComponent`·`UGizmoArrowComponent`·`UGizmoBoxComponent`·`UGizmoCircleComponent`·`UGizmoLineHandleComponent`·`UGizmoRectangleComponent`·`UViewAdjustedStaticMeshGizmoComponent`(`RT/InteractiveToolsFramework/Public/BaseGizmos/`), `ULandscapeGizmoRenderComponent`, `ULevelInstanceComponent`(레벨 인스턴스의 편집용 인스턴스 Actor Transform을 맞추는 편집 도우미 — `RT/Engine/Public/LevelInstance/LevelInstanceComponent.h:15–30`). 런타임 카탈로그에 넣지 않고 에디터 문서(gizmo)와 [Prefab](Prefab.ko.md) PF5·PF9(레벨 인스턴스)가 정한다(CD17). 그리는 경로는 편집 표시(CD12, RN PC9a)다.

**컴포넌트 아님**(13): 시험 전용 3(`UAutoRTFMTestChildActorComponent`·`UAutoRTFMTestPrimitiveComponent`·`USSAMTestComponent`), `UObject` 파생 2(`UClientPilotComponent`·`UExternalRpcRegistrationComponent`), 인터페이스 2(`UPhysicsComponent`·`UToolFrameworkComponent`), UMG 위젯 확장 5(`UUIComponent`·`UMouseHoverComponent`·`UScaleBoxComponent`·`USizeBoxComponent`·`UNavigationUIComponent` — Actor가 아니라 위젯에 붙는 `UObject`, `RT/UMG/Public/Extensions/UIComponent.h`, [UI 툴킷](UiToolkit.ko.md) 몫), 패킷 처리기 팩토리 1(`UEngineHandlerComponentFactory`).

### 8.4 필수 컴포넌트

1부 3절과 같은 형식이다. 이름은 CD18 권장안이다.

**카메라**

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UCameraComponent`(`RT/Engine/Classes/Camera/CameraComponent.h`) | `CameraComponent` | CR11 | CM0 |
| `UCameraProxyMeshComponent`(같은 헤더) | `CameraProxyMeshComponent`(EditorOnly — 편집 화면의 카메라 모형) | CM1a·CR5 | CM2 |
| `UCameraShakeSourceComponent` | `CameraShakeSourceComponent` | CR29 | CM15 |
| `UCineCameraComponent`(`RT/CinematicCamera/Public/CineCameraComponent.h`) | `CinematicCameraComponent` | CR29(렌즈·필름백·초점 — 렌더러 DOF, EIP 26절) | CM15 |

**이동**(`RT/Engine/Classes/GameFramework/`)

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UMovementComponent` | `MovementComponent` | CR16·CR27 | CM12 |
| `UNavMovementComponent` | `NavMovementComponent` | CM12(경로 이동 자체는 CR30) | CM12 |
| `UPawnMovementComponent` | `PawnMovementComponent` | CR26(Pawn·Controller 입력) | CM12 |
| `UFloatingPawnMovement` | `FloatingPawnMovement` | CM12 | CM12 |
| `USpectatorPawnMovement` | `SpectatorPawnMovement` | CM12 | CM12 |
| `UProjectileMovementComponent` | `ProjectileMovementComponent` | CM12 | CM12 |
| `URotatingMovementComponent` | `RotatingMovementComponent` | CM12 | CM12 |
| `UCharacterMovementComponent` | `CharacterMovementComponent` | CM12·CR27(바닥 찾기·step up·물리 볼륨)·CR19(root motion)·CR28(네트워크 예측, 없으면 단독 실행만) | CM13 |
| `USpringArmComponent` | `SpringArmComponent` | CR16(sweep)·CR11 | CM12 |

`UInterpToMovementComponent`(1부 3.9절 `PathMoverComponent`)도 부모가 `UMovementComponent`라 CM5에서 CM12로 옮긴다.

**물리 보조**(`RT/Engine/Classes/PhysicsEngine/`)

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UPhysicsConstraintComponent` | `PhysicsConstraintComponent` | CR16(constraint) | CM14 |
| `UPhysicsHandleComponent` | `PhysicsHandleComponent` | CR16 | CM14 |
| `UPhysicsSpringComponent` | `PhysicsSpringComponent` | CR16(ray·sweep 질의) | CM14 |
| `UPhysicsThrusterComponent` | `PhysicsThrusterComponent` | CR16(힘) | CM14 |
| `URadialForceComponent` | `RadialForceComponent` | CR16(overlap 질의·충격량) | CM14 |
| `UPhysicalAnimationComponent` | `PhysicalAnimationComponent` | CR16·CR19, CM7 | CM14 |

**AI·내비게이션**

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UBrainComponent` | `BrainComponent` | CR31·CR39 | CM16 |
| `UBehaviorTreeComponent` | `BehaviorTreeComponent` | CR31 | CM16 |
| `UBlackboardComponent` | `BlackboardComponent` | CR31 | CM16 |
| `UAIPerceptionComponent` | `AiPerceptionComponent`(CD24) | CR31·CR39 | CM16 |
| `UAIPerceptionStimuliSourceComponent` | `AiPerceptionStimuliSourceComponent`(CD24) | CR31 | CM16 |
| `UPathFollowingComponent` | `PathFollowingComponent` | CR30·CR39·CM12 | CM16 |
| `UCrowdFollowingComponent` | `CrowdFollowingComponent` | CR30(crowd) | CM16 |
| `UGameplayTasksComponent`(`RT/GameplayTasks`) | `GameplayTasksComponent` | CR31 | CM16 |
| `UNavRelevantComponent` | `NavRelevantComponent` | CR30 | CM16 |
| `UNavModifierComponent` | `NavModifierComponent` | CR30 | CM16 |
| `USplineNavModifierComponent` | `SplineNavModifierComponent` | CR30·`SplineComponent`(CM5) | CM16 |
| `UNavLinkComponent` | `NavLinkComponent` | CR30 | CM16 |
| `UNavLinkCustomComponent` | `NavLinkCustomComponent` | CR30 | CM16 |
| `UNavigationInvokerComponent` | `NavigationInvokerComponent` | CR30(invoker 기반 동적 생성) | CM16 |
| `UNavMeshRenderingComponent`·`UNavLinkRenderingComponent`·`UNavTestRenderingComponent` | `NavMeshDrawComponent`·`NavLinkDrawComponent`·`NavTestDrawComponent`(디버그 표시, CD24) | CR30·CR23 | CM16 |
| `UGameplayDebuggerRenderingComponent` | `GameplayDebugDrawComponent` | CR31(gameplay debugger)·CR23 | CM16 |
| `UEQSRenderingComponent` | `EqsDrawComponent` | CR31(EQS — 계획상 AI 확장 단계)·CR23 | CM16(EQS 도입 때) |

**UI·미디어**

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UWidgetComponent`(`RT/UMG/Public/Components/WidgetComponent.h`) | `WidgetComponent` | CR32 | CM17 |
| `UWidgetInteractionComponent` | `WidgetInteractionComponent` | CR32·CR16(ray 질의) | CM17 |
| `UMediaComponent`(`RT/MediaAssets`) | `MediaComponent` | CR33 | CM17 |

`USynthComponent`(`RT/AudioMixer/Public/Components/SynthComponent.h`)는 대체다(8.3절). 오디오 문서가 이미 채택하지 않기로 정했다(AU 9.1절 — 절차 오디오는 `SoundGenerator` source 입력, AU10).

**지형·식생**

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `ULandscapeComponent` | `LandscapeComponent` | CR35 | CM18 |
| `ULandscapeHeightfieldCollisionComponent` | `LandscapeCollisionComponent` | CR35·CR16(heightfield 형상) | CM18 |
| `ULandscapeSplinesComponent` | `LandscapeSplinesComponent` | CR35 | CM18 |
| `UControlPointMeshComponent` | `ControlPointMeshComponent` | CR35, CM1a | CM18 |
| `ULandscapeMeshProxyComponent` | `LandscapeMeshProxyComponent` | CR35·CR36(HLOD) | CM19 |
| `UFoliageInstancedStaticMeshComponent` | `FoliageInstancedStaticMeshComponent`(ISM 상속 — CD2) | CR35(식생), CM6 | CM18 |
| `UGrassInstancedStaticMeshComponent` | `GrassInstancedStaticMeshComponent`(ISM 상속) | CR35(grass 자동 배치), CM6 | CM18 |
| `UInteractiveFoliageComponent` | `InteractiveFoliageComponent` | CR35, CM1a·CR16(접촉) | CM18 |

**큰 월드·스트리밍 데이터**

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UHLODInstancedStaticMeshComponent` | `HlodInstancedStaticMeshComponent`(CD24) | CR36, CM6 | CM19 |
| `UHLODInstancedSkinnedMeshComponent` | `HlodInstancedSkinnedMeshComponent`(CD24) | CR36, CM11(`InstancedSkinnedMeshComponent`) | CM19 |
| `UActorTextureStreamingBuildDataComponent` | `TextureStreamingBuildDataComponent` | CR36(텍스처 스트리밍 빌드 데이터) | CM19 |

**동적 메시·디버그·VFX**

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UBaseDynamicMeshComponent`·`UDynamicMeshComponent`(`RT/GeometryFramework`) | 같은 이름 | CR37 | CM20 |
| `UDebugDrawComponent`(`RT/Engine/Classes/Debug/DebugDrawComponent.h`) | `DebugDrawComponent`(디버그 표시 기반 클래스) | CR23 | CM2 |
| `UFXSystemComponent`(`ParticleSystemComponent.h` 안의 추상 기반) | `EffectComponent`(CD16) | CR38 | CM21 |

### 8.5 선행 작업

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR27 | 이동 공통: sweep 이동과 겹침 해소, 바닥 찾기·step up·경사 한계, 이동 기반(움직이는 바닥) 추종, 물리 볼륨(UE `APhysicsVolume` — 물·중력·유체 마찰 영역, `VolumeShape`(CR17) 위 속성) | 없음 | — | 이동 문서(없음), EIP 16절 캐릭터 이동 |
| CR28 | 네트워크 이동 예측: 클라이언트 예측·서버 보정·이동 기록 재생 | 없음 | — | EIP 17절. 단독 실행은 막지 않는다 |
| CR29 | 카메라 시스템: `CameraDirector`(gameplay·spectator·cutscene control layer 합성, view target·blend·modifier — UE `APlayerCameraManager` 대응), camera shake 패턴 에셋, 시네마틱 렌즈·필름백·초점(DOF) | 이름·역할만 계획에 있음 | EIP 20.5절 "CameraDirector는 gameplay·spectator·cutscene control layer를 합성한다" | EIP 20.5절(정본), 25·26절 |
| CR30 | 내비게이션: navmesh 생성(Recast·Detour — EIP 19절), 영역·modifier·link, 경로 질의, crowd, invoker 기반 동적 생성 | 없음 | — | 내비 문서(없음), EIP 19절 |
| CR31 | AI 런타임: behavior tree·blackboard 에셋과 실행기, perception 감각(시각·청각·피해), gameplay task, gameplay debugger, EQS(확장) | 없음 | — | AI 문서(없음), EIP 19절 "초기에는 Behavior Tree와 Blackboard, 시각·청각 perception, async path following" |
| CR32 | world-space UI: Widget 층을 render target에 그리기, world hit test·포인터 입력 라우팅 | 설계 있음(코드 없음) | UiToolkit 5.9절 "월드 공간 Widget", 3절 `WorldIntegration` 행의 `UWidgetComponent` 대응 | [UI 툴킷](UiToolkit.ko.md) 5.9절·UT6, EIP 21.4절 |
| CR33 | 미디어 재생: 영상 디코드·텍스처 출력·오디오 동기 | 없음 | — | 미디어 문서(없음) |
| CR34 | 절차 오디오 source: 게임 코드가 PCM을 생성해 voice로 넣는 경로(`SoundGenerator`) — `USynthComponent` 대체 | 설계(AU10 단계) | AU 9.1절 "`USynthComponent`(procedural은 `SoundGenerator` source 입력으로 — AU10)" | AU |
| CR35 | 지형·식생: heightfield·tile LOD·지형 머티리얼 layer·지형 충돌(Jolt heightfield)·spline, 식생 인스턴스·grass 자동 배치·절차 배치·바람 | 없음 | EIP 21절 "terrain heightfield/tile LOD, foliage instancing·wind … P2" | 환경 문서(없음) |
| CR36 | HLOD 생성·쿠킹(World Partition, CR22), 텍스처 스트리밍 빌드 데이터 | 없음 | — | EIP 10.2절, TextureStreaming |
| CR37 | 동적 메시 자료 구조(UE `FDynamicMesh3`)와 실행 중 메시 갱신 렌더 경로(동적 인스턴스 종류) | 없음 | — | 메시 문서(MA)·RN 4.2절 인스턴스 종류 |
| CR38 | VFX 시스템(System·Emitter·Module, CPU → GPU 시뮬레이션) | 없음 | EIP 21절 | VFX 문서(없음) |
| CR39 | AI 컨트롤러: AI 판단 컴포넌트(brain·path following·perception)를 소유하고 Pawn을 조종하는 Controller(UE `AAIController`) | 없음 | ER 3절 `Gameplay` 행(Controller·Pawn, S10–S12). CR26은 PlayerController·LocalPlayer만 다룬다 | ER 1절 S12, EIP 19·20.4절 |

### 8.6 선행 → 묶음

| 묶음 | 막는 선행 |
|---|---|
| CM0(카메라 추가) | CR11 |
| CM2(추가: `CameraProxyMeshComponent`·`DebugDrawComponent`) | CM1a, CR5, CR23 |
| CM12 이동 기본(+ `PathMoverComponent`) | CM0, CM3(sweep하는 충돌 형상), CR16, CR17, CR26, CR27 |
| CM13 캐릭터 이동 | CM12, CM3(capsule), CR19(root motion), CR28(예측 — 없으면 단독 실행만) |
| CM14 물리 보조 | CM3, CR16. `PhysicalAnimationComponent`만 CM7·CR19도 |
| CM15 카메라 효과·시네마틱 | CM0, CR29, RN RS6(DOF 연결점) |
| CM16 AI·내비 | CM12, CR30, CR31, CR39(AI 컨트롤러), CR23(디버그 표시), CM5(`SplineNavModifierComponent`) |
| CM17 UI·미디어 | CM1a, CR32, CR33, 미디어 소리는 CM4(`AudioComponent`)·CR34(`SoundGenerator`) |
| CM18 지형·식생 | CM1a, CM6, CR16, CR35 |
| CM19 HLOD·스트리밍 데이터(+ `LandscapeMeshProxyComponent`) | CM6, CM11, CM18, CR22, CR36 |
| CM20 동적 메시 | CM1a, CR37 |
| CM21 VFX 효과 | CM1a, CR38 |

1부 5절의 순서 원칙(고정 순서 유지, 시스템이 들어올 때 함께, 동작하는 척 금지)과 완료 조건을 그대로 따른다.

### 8.7 결정(권장안)

| ID | 항목 | 권장안 | 근거 |
|---|---|---|---|
| CD13 | 플러그인 범위 | (개정 6에서 CD19로 대체) 처음 권장안은 영역 색인만 두는 것이었다. 사용자 지시로 플러그인도 클래스 단위로 분류한다(9절) | — |
| CD14 | Chaos 파괴 계열 | 컴포넌트 단위로 들이지 않는다. P3 파괴 문서가 Jolt 위 방식(파괴 메시 컴포넌트 하나 + 힘 영역)을 정하고, Chaos 구현 고유 보조(cluster union·ISM pool·Dataflow·Chaos debug draw·event listener)의 필요 여부도 그 문서가 판단한다 | Rocks 물리는 Jolt다(EIP 16절). destruction은 P3 |
| CD15 | 캐릭터 이동 | UE 안정판 `UCharacterMovementComponent` 구조(이동 모드 walking·falling·swimming·flying·custom, 바닥 찾기, step up, root motion)를 들인다. 실험 플러그인 Mover는 참고만 한다. 단독 실행을 먼저 만들고 네트워크 예측(CR28)은 이동 기록 자리를 남겨 뒤에 붙인다 | UE 5.8 기본 캐릭터 이동은 CMC다. Mover는 `Engine/Plugins/Experimental/Mover` |
| CD16 | VFX 컴포넌트 | UE `UFXSystemComponent`(추상)와 Niagara `UNiagaraComponent`에 대응하는 `EffectComponent` 하나. Cascade(`UParticleSystemComponent`)는 제외 | 정본은 VFX 문서 |
| CD17 | 편집기 쪽 컴포넌트 | gizmo·지형 gizmo·레벨 인스턴스 편집 도우미는 런타임 카탈로그 밖. 에디터 문서와 Prefab이 정한다. 편집 표시는 CD12 경로 | 런타임 Shipping에 들어가지 않는다 |
| CD18 | 이름(2부) | 업계 공용 이름은 그대로. UE 줄임말·브랜드만 바꾼다: `CineCamera` → `CinematicCamera`, `FXSystem` → `Effect`, `LandscapeHeightfieldCollision` → `LandscapeCollision`, `ActorTextureStreamingBuildData` → `TextureStreamingBuildData`, `GameplayDebuggerRendering` → `GameplayDebugDraw`, `EQSRendering` → `EqsDraw` | CD6과 같은 규칙 |

### 8.8 다른 문서 개정

개정 5(2026-11-23)에서 모두 반영했다(EIP 개정 1.19 범위에 더함). 요구의 정본은 이 문서 8.5절이다.

| 문서 | 고친 것 |
|---|---|
| EIP 16.1절 | 물리 쪽 컴포넌트(CM3·CM12–CM14) 링크, 캐릭터 이동 구조(CD15), Chaos 파괴·field·cluster union·네트워크 물리 미도입(CD14) |
| EIP 19절 | AI·내비 컴포넌트(CM16)와 선행 CR30·CR31·CR39 링크, PawnSensing·GridPathFollowing 미도입 |
| EIP 20.5절 | `CameraDirector` = UE `APlayerCameraManager` 대응, 카메라 컴포넌트(CM0·CM12·CM15) 링크, CR29의 정본, AI 컨트롤러 CR39 |
| EIP 21절 | VFX 효과(CD16, CM21)·world-space UI(CM17)·지형·식생(CM18) 링크, 절차 식생 후속, destruction(CD14) |

UiToolkit은 world-space UI를 이미 5.9절·UT6에서 다루므로 고칠 것이 없다(CR32가 그쪽을 정본으로 가리킨다).

### 8.9 플러그인 색인

개정 6에서 9절(3부)로 대체했다.

## 9. 플러그인 컴포넌트(3부)

엔진 플러그인의 컴포넌트를 1·2부와 같은 방식으로 정한다(사용자 지시 2026-11-23 — CD13의 색인 방식을 대체, CD19). ID는 2부에 이어 붙인다(선행 CR40–, 묶음 CM22–, 결정 CD19–). UE 경로 약어: `PL/` = `Engine/Plugins/`.

### 9.1 범위와 수집

- 대상: `PL/` 아래 모든 헤더에서 `UActorComponent`를 직접·간접 상속한 `class` 선언이다. 상속은 `Engine/Source`와 `Engine/Plugins` 전체 헤더로 따라간다(플러그인 클래스가 Runtime 클래스를 상속하는 경우 포함). 부모 이름이 namespace로 한정된 선언(`: public MeshPartition::UModifierComponent`)도 따라간다. 결과는 **139개 플러그인(.uplugin 단위)의 클래스 418개**다. 시험·예시 폴더도 세고 "컴포넌트 아님"으로 분류한다.
- 판정에 쓴 표지: `.uplugin`의 `IsExperimentalVersion`·`IsBetaVersion`·`EnabledByDefault`·`Description`, 클래스 `UCLASS`의 `Experimental`·`EarlyAccessPreview`, `UE_DEPRECATED`, 모듈 `Type`(Runtime·Editor·UncookedOnly·EditorAndProgram 등).
- 같은 추적을 Runtime에도 돌려 2부에서 빠진 deprecated 4종을 8.3절에 더했다.

### 9.2 분류 기준과 요약

| 분류 | 기준 | 수 | 절 |
|---|---|---|---|
| 필수 | 정식(표지 없음) 또는 베타 플러그인이고, 계획(EIP 본문·24.1절 표·33.2절 표)이 그 기능을 필수 단계에 둔 것. 행마다 근거를 9.6절 선행에 적는다 | 31 | 9.3절. CM20·CM22–CM29 |
| 필수(실험 기반) | 실험 플러그인이지만 계획이 그 기능을 필수로 두고 UE에 정식 대안이 없는 것(CD20) | 25 | 9.4절. CM30·CM31 |
| 대체 | Rocks가 다른 구조로 이미 정한 것(CD21) | 28 | 9.5절 |
| 제외 | deprecated·legacy·"Not supported"와 그에 의존하는 것 | 10 | 9.5절 |
| 후속 | 계획에 없거나 P3·후속 플랫폼인 기능, 또는 위 분류에 들지 않는 실험 플러그인 | 154 | 9.5절 |
| 편집기 쪽 | 필수·대체 기능에 딸린 편집기 모듈·편집 도구 표시 | 26 | 9.5절. 런타임 카탈로그 밖(CD17과 같다) |
| 범위 밖 | 가상 제작·방송·원격 송출·특정 제작 제품·Chaos 전용 도구 등 엔진 범위 밖(CD22) | 124 | 9.5절 |
| 컴포넌트 아님 | 시험·mock·예시 플러그인 | 20 | 9.5절 |

한 플러그인이 후속·범위 밖이면 그 플러그인의 편집기 모듈 클래스도 같은 분류로 센다(CD19). 편집기 쪽은 필수·대체 기능에 딸린 것만이다.

### 9.3 필수 컴포넌트

1부 3절과 같은 형식이다. 이름은 CD24 권장안이다.

**애니메이션**

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UControlRigComponent`(`PL/Animation/ControlRig`) | `ControlRigComponent` | CR40, CM7 | CM22 |
| `UIKRigComponent`(`PL/Animation/IKRig`) | `IkRigComponent` | CR41, CM7 | CM22 |
| `UMotionWarpingComponent`(`PL/Animation/MotionWarping`, 베타) | `MotionWarpingComponent` | CR42·CR19, CM13 | CM22 |
| `UAttributeBasedRootMotionComponent`(같은 플러그인 — 애니메이션 속성 값으로 root motion을 캐릭터에 적용) | 같은 이름 | CR42, CM13 | CM22 |
| `USkeletalMeshComponentBudgeted`(`PL/Runtime/AnimationBudgetAllocator`) | `BudgetedSkeletalMeshComponent` | CR44, CM7 | CM22 |
| `UPhysicsControlComponent`·`UPhysicsControlInitializerComponent`(`PL/Animation/PhysicsControl`) | 같은 이름 | CR43, CM7·CM14 | CM23 |

**게임플레이 프레임워크**

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UGameFrameworkComponent`(`PL/Runtime/ModularGameplay`, 베타) | 같은 이름 | CR48 | CM24 |
| `UControllerComponent`·`UPawnComponent`·`UPlayerStateComponent`·`UGameStateComponent` | 같은 이름 | CR26·CR48(`ControllerComponent`는 CR39도) | CM24 |
| `UAbilitySystemComponent`(`PL/Runtime/GameplayAbilities`, 부모 `UGameplayTasksComponent`) | 같은 이름 | CR45, CM16 | CM25 |
| `UStateTreeComponent`(부모 `UBrainComponent`)·`UStateTreeAIComponent`(`PL/Runtime/GameplayStateTree`) | `StateTreeComponent`·`StateTreeAiComponent` | CR46, CM16 | CM26 |
| `USmartObjectComponent`·`USmartObjectUserComponent`(`PL/Runtime/SmartObjects`) | 같은 이름 | CR47 | CM26 |
| `USmartObjectRenderingComponent`·`USmartObjectContainerRenderingComponent`·`USmartObjectDebugRenderingComponent`·`USmartObjectSubsystemRenderingComponent` | `SmartObjectDrawComponent`·`SmartObjectContainerDrawComponent`·`SmartObjectDebugDrawComponent`·`SmartObjectSubsystemDrawComponent` | CR47·CR23 | CM26 |

**PCG**(`PL/PCG`)

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UPCGComponent` | `PcgComponent` | CR49 | CM27 |
| `UPCGGenSourceComponent`(Actor를 런타임 생성 원천으로) | `PcgGenerationSourceComponent` | CR49 | CM27 |
| `UPCGProceduralISMComponent`(`Source/PCG/Private/Components/PCGProceduralISMComponent.h:33` — CPU 인스턴스 데이터 없이 GPU scene에 직접 쓰는 인스턴스 primitive) | `PcgProceduralIsmComponent` | CR49(GPU 인스턴스 직접 기록), CM6 | CM27 |
| `UPCGDebugDrawComponent` | `PcgDebugDrawComponent` | CR49·CR23 | CM27 |

**메시·시뮬레이션**

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UProceduralMeshComponent`(`PL/Runtime/ProceduralMeshComponent`) | 같은 이름 | CR37 | CM20 |
| `UGeometryCacheComponent`(`PL/Runtime/GeometryCache`) | 같은 이름 | CR50, CM1a | CM28 |
| `UCableComponent`(`PL/Runtime/CableComponent`) | 같은 이름 | CR51·CR37 | CM28 |

**오디오**(`PL/AudioGameplay`, 베타)

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UAudioGameplayComponent`(오디오 게임플레이 컴포넌트 기반) | 같은 이름 | CR54 | CM29 |
| `UAudioParameterComponent`(소유 Actor의 오디오 재생에 파라미터를 모아 전달) | 같은 이름 | CR54, CM4 | CM29 |
| `UAudioComponentGroup`(여러 `AudioComponent`에 같은 파라미터·볼륨 수정자) | 같은 이름 | CR54, CM4 | CM29 |

### 9.4 필수(실험 기반) 컴포넌트

UE에서 실험 플러그인(`IsExperimentalVersion: true`)이지만 계획이 기능을 필수로 둔다. 역할·이름·계층은 UE를 따르고, 착수 때 그 시점 UE 소스를 다시 대조해 기능 문서가 구조를 확정한다(CD20).

**대량 군중**(EIP 24.1절 "Mass 계열 대량 처리 P2 · M7", 33절 "AI·EQS·State/Behavior·SmartObject·Mass 필수 M4–M7". MassGameplay·MassCrowd·ZoneGraph·ZoneGraphAnnotations 모두 실험)

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UMassAgentComponent`(`PL/Runtime/MassGameplay` — Actor와 Mass entity를 잇는다) | 같은 이름 | CR52 | CM30 |
| `UMassVisualizationComponent`(LOD별 인스턴스 표현) | 같은 이름 | CR52, CM6 | CM30 |
| `UMassDebugVisualizationComponent` | 같은 이름 | CR52·CR23 | CM30 |
| `USmartObjectZoneAnnotations` | 같은 이름 | CR52·CR47 | CM30 |
| `UZoneShapeComponent`(`PL/Runtime/ZoneGraph` — 차선 모양) | 같은 이름 | CR52 | CM30 |
| `UZoneGraphRenderingComponent` | `ZoneGraphDrawComponent` | CR52·CR23 | CM30 |
| `UZoneGraphAnnotationComponent`(추상, 부모 `UDebugDrawComponent`)·`UZoneGraphDisturbanceAnnotation`(`PL/Runtime/ZoneGraphAnnotations`) | 같은 이름 | CR52·CR23 | CM30 |
| `UZoneGraphCrowdLaneAnnotations`·`UMassCrowdLaneDataRenderingComponent`(`PL/AI/MassCrowd`) | `ZoneGraphCrowdLaneAnnotations`·`MassCrowdLaneDrawComponent` | CR52·CR23 | CM30 |

**물**(EIP 21절 "M7에는 water surface·underwater 원신호", 33절 "water/cloud 필수 M7–M8". `PL/Experimental/Water` 실험)

| UE | Rocks | 선행 | 묶음 |
|---|---|---|---|
| `UWaterBodyComponent`(추상)·`UWaterBodyOceanComponent`·`UWaterBodyLakeComponent`·`UWaterBodyRiverComponent`·`UWaterBodyCustomComponent` | 같은 이름 | CR53 | CM31 |
| `UWaterSplineComponent` | 같은 이름 | CR53, CM5 | CM31 |
| `UWaterMeshComponent`(큰 물 표면 quadtree 메시) | 같은 이름 | CR53 | CM31 |
| `UWaterBodyMeshComponent`·`UWaterBodyStaticMeshComponent`·`UWaterBodyInfoMeshComponent` | 같은 이름 | CR53, CM1a | CM31 |
| `UOceanCollisionComponent`·`UOceanBoxCollisionComponent`·`ULakeCollisionComponent` | 같은 이름 | CR53·CR16, CM3 | CM31 |
| `UBuoyancyComponent` | 같은 이름 | CR53·CR16 | CM31 |
| `UWaterTerrainComponent`(지형 역할을 하는 primitive를 물 정보 텍스처에 기록 — 지형 → 물. 아무 Actor에나 붙는다, `WaterTerrainComponent.h:14`) | 같은 이름 | CR53(물 정보 텍스처), CM1a | CM31 |

같은 플러그인의 `UBakedShallowWaterSimulationComponent`(구운 얕은 물 시뮬레이션)는 고급 fluid라 후속이다(9.5절).

### 9.5 대체·제외·후속·편집기·범위 밖·아님

**대체**(28)

| UE | Rocks | 수 |
|---|---|---|
| `UEnhancedInputComponent`(`PL/EnhancedInput`) | `InputComponent`(CD7 — Rocks 입력 컴포넌트가 처음부터 action·context 구조) | 1 |
| `UNiagaraComponent`·`UNiagaraCullProxyComponent`(`PL/FX/Niagara`) | `EffectComponent`(CD16). cull proxy는 VFX 문서가 효과 내부 기능으로 정한다 | 2 |
| `PL/Experimental/Mover` 6(`UMoverComponent`·`UCharacterMoverComponent`·`UNavMoverComponent`·`UMoverStandaloneLiaisonComponent`·`UMoverNetworkPredictionLiaisonComponent`·`UMoverDebugComponent`), `PL/Experimental/ChaosMover` 4, `PL/Experimental/MoverIntegrations` 1(`UMassMoverInputComponent`). 같은 플러그인의 `UMoverCVDSimDataComponent`(UncookedOnly, 부모 `UChaosVDSolverDataComponent`)는 Chaos 디버거 자료라 범위 밖에 센다 | `CharacterMovementComponent`(CD15 — Mover는 참고만) | 11 |
| `PL/AudioGameplayVolume` 8(`UAudioGameplayVolumeComponentBase`·`UAudioGameplayVolumeComponent`·`UAudioGameplayVolumeMutator`·`UReverbVolumeComponent`·`UAttenuationVolumeComponent`·`UFilterVolumeComponent`·`USubmixOverrideVolumeComponent`·`USubmixSendVolumeComponent`) | AU 3.15절 리버브 존 영역(공용 `VolumeShape` + 영역 속성). 리버브·안팎 감쇠·LPF는 AU에 있고, submix send·효과 교체는 CR54로 더한다 | 8 |
| `PL/Runtime/Synthesis` 신스 5(`UGranularSynth`·`UModularSynthComponent`·`USynthComponentMonoWaveTable`·`USynthComponentToneGenerator`·`USynthSamplePlayer` — 모두 `USynthComponent` 파생) | `SoundGenerator` source(CR34, AU 9.1절). 신스 자체는 오디오 문서가 `SoundGenerator` 구현으로 정한다 | 5 |
| `UCustomMeshComponent`(`PL/Runtime/CustomMeshComponent`) | `ProceduralMeshComponent` — 삼각형 목록만 받는 부분집합이다 | 1 |

**제외**(10)

| UE | 근거 | 수 |
|---|---|---|
| `UDEPRECATED_LiveLinkDrivenComponent`(`PL/Animation/LiveLink`) | 클래스 이름이 deprecated | 1 |
| `PL/Compositing/Composure` 5 | `.uplugin` "Legacy system for real-time compositing. This plugin is no longer developed." | 5 |
| `USynthComponentMoto`(`PL/Experimental/MotoSynth`) | `.uplugin` "An experimental granular vehicle engine. … Not supported." | 1 |
| `UMotorSimOutputMotoSynth`(`PL/Experimental/MotorSimOutputMotoSynth`) | MotoSynth에 의존한다(`.uplugin` "A MotorSim Output component using MotoSynth.") | 1 |
| `UCascadeParticleSystemComponent`(`PL/FX/Cascade`) | Cascade(CD5·CD16) | 1 |
| `UDestructibleComponent`(`PL/Runtime/ApexDestruction/Source/ApexDestruction/Public/DestructibleComponent.h:29`) | `UE_DEPRECATED(4.26, "APEX is deprecated. …")` | 1 |

**후속**(154, 들이는 것 자체는 넘길 곳이 정한다)

| 묶음 | 플러그인(클래스 수) | 수 | 근거 | 넘길 곳 |
|---|---|---|---|---|
| 2D | `2D/Paper2D`(6) | 6 | 계획에 2D 게임 도구 항목이 없다(CD23). UE에서도 일부는 실험·EAP(`PaperTerrainComponent.h:53` `Experimental`, `PaperTileMapComponent.h:37` `EarlyAccessPreview`) | 2D 문서(EIP 개정 뒤) |
| 고급 애니메이션·캐릭터 | `Animation/LiveLink`(5)·`PerformanceCaptureCore`(2)·`MLDeformer` 계열(4)·`Editor/FacialAnimation`(1 — `UCLASS(Experimental)`), 실험 `ContextualAnimation`(1)·`MotionTrajectory`(1)·`UAF`(1)·`DirectMeshControl`(1 — 편집기 모듈), `MetaHuman/MetaHumanSDK`(2 — 부위별 Control Rig·physics asset을 LOD 임계와 함께 실행하는 런타임 캐릭터 컴포넌트, `MetaHumanComponentBase.h:51–69`) | 18 | EIP 15절 "cloth·muscle·motion matching은 P3다", 15.1절 "M8/M9의 motion matching·cloth/groom profile", 24.3절 "M7 이후 Live Link형 mocap". MetaHuman은 CR40(Control Rig)과 MetaHuman 에셋 생태계 위 | 애니메이션 문서(EIP 15절) |
| 천·털·연성체·파괴 | `ChaosClothAsset`(1)·`Runtime/HairStrands`(2), 실험 `ChaosFlesh`(7)·`ChaosDataflowSolver`(1)·`ChaosRigidAsset`(1), `Dataflow`(3) | 15 | EIP 33.2절 표 "고급 cloth/groom P3", "파괴/softbody/vehicle 개별 P3" | cloth·groom 프로필, 파괴 문서(CD14) |
| 차량 | 실험 `ChaosVehiclesPlugin`(2)·`ChaosModularVehicle`(12)·`Runtime/AudioMotorSim`(10) | 24 | EIP 33.2절 표 "파괴/softbody/vehicle 개별 P3", 16.3절 "soft body·vehicle은 Jolt가 제공하는 범위를 별도 모듈 단계에서 평가" | 물리 문서 vehicle 단계 |
| 카메라·시네마틱 | `Cameras/GameplayCameras`(7 — 실험, 기본 활성), `CineCameraRigs`(1 — 베타), 실험 `CineCameraSceneCapture`(1), `MovieScene/ActorSequence`(1 — `UCLASS(Experimental)`) | 10 | Rocks 카메라 정본은 `CameraDirector`(CR29, UE `APlayerCameraManager`). 데이터 기반 camera rig·Actor 내장 sequence는 UE에서 실험이고 계획 필수 범위 밖 | 카메라 문서(CR29)·시퀀서(EIP 25절) |
| AI·게임플레이 확장 | `AI/HTNPlanner`(1 — `.uplugin` "[EXPERIMENTAL]"), 실험 `LearningAgents`(4)·`CommonConversation`(1)·`InteractionInterface`(2)·`AsyncMessageSystem`(1)·`Runtime/InstancedActors`(3) | 12 | 계획 AI는 BT·StateTree·EQS·utility(EIP 19절). InstancedActors는 Mass 위의 상호작용 Actor 인스턴스 계층이다 — 계획의 Mass "Entity presentation bridge"(24.1절 표)는 CM30의 `MassAgentComponent`·`MassVisualizationComponent`가 맡으므로, 그 위 계층은 군중 문서가 CM30 뒤에 판단한다(CD20). 나머지는 계획에 없다 | AI·게임플레이·군중 문서 |
| 월드·지형·PCG 확장 | 실험 `MeshPartition`(27 — modifier 20종 포함)·`MeshPartitionWater`(4)·`PCGMeshPartitionInterop`(2)·`ProceduralVegetationEditor`(4)·`VirtualHeightfieldMesh`(1)·`FastGeoStreaming`(1)·`WaterAdvanced`(1)·`DaySequence`(1), `SpatialReadiness`(1), Water의 `UBakedShallowWaterSimulationComponent`(1) | 43 | 실험 기능, 고급 fluid는 P3(EIP 21절). DaySequence는 Sequencer 기반 시간대다 — 계획의 time-of-day(EIP 21절 "sky·sun/time-of-day … P2")는 환경 문서가 sky·sun 구조로 정하고, Sequencer 기반 방식은 구조만 참고한다(CD20) | 환경·PCG 문서(EIP 21·24.1절) |
| 렌더·VFX 확장 | `NaniteDisplacedMesh`(1 — 베타), 실험 `NiagaraNanite`(1)·`NiagaraUIRenderer`(1), `Runtime/ComputeFramework`(1 — 베타), `Enterprise/LidarPointCloud`(1 — 범용 point cloud 렌더) | 5 | VirtualGeometry는 EIP 13.2절 M8–M9. point cloud 렌더는 계획에 없다 | 렌더러·VFX 문서·UI 툴킷 |
| 미디어·오디오 확장 | 실험 `MediaStream`(1), `Runtime/AudioCapture`(1)·`Soundscape`(1 — 베타)·Synthesis `UEnvelopeFollowerListener`(1)·`Online/OnlineSubsystemUtils` `UVoipListenerSynthComponent`(1) | 5 | 마이크 입력은 AU10, 오디오 분석·VoIP는 AU 9.2절 범위 밖 | 오디오·미디어·온라인 문서 |
| 가져오기·커스터마이즈 | `Runtime/USDCore`(1)·`Importers/USDImporter`(1), `Mutable`(1 — 베타) | 3 | "선택 USD/Alembic", 고급 interchange M8+(EIP 33절). 캐릭터 커스터마이즈는 계획에 없다 | 가져오기 문서 |
| XR | `Runtime/XRBase`(2)·`AR/ARUtilities`(1)·`AR/AppleAR/AppleARKitFaceSupport`(1)·`MixedRealityCaptureFramework`(3), `Experimental/XRCreativeFramework`(4) | 11 | XR은 후속 플랫폼(EIP 33절), CD4 | VR·AR 프로필 문서 |
| 네트워크 | `Runtime/NetworkPrediction`(1 — 베타, 추상) | 1 | CR28 | 네트워크 단계(EIP 17절) |
| UI | `Experimental/UIFramework`(1 — 서버가 UMG를 조종) | 1 | 네트워크 UI는 계획에 없다 | UI 툴킷 |

**편집기 쪽**(26): `UControlRigSkeletalMeshComponent`, `UPhysicsControlAssetEditorPhysicsHandleComponent`·`UPhysicsControlAssetEditorSkeletalMeshComponent`, `UPoseSearchMeshComponent`(UncookedOnly), `UInsightsSkeletalMeshComponent`(`Animation/GameplayInsights`), `USkeletalMeshBackedDynamicMeshComponent`(`Animation/SkeletalMeshModelingTools`), `UGeometryCacheAbcFileComponent`(Editor 모듈), `UJumpFloodComponent2D`(WaterEditor), `UPCGCollisionVisComponent`, `Editor/LandscapePatch` 3(지형 편집 patch), `Editor/UVEditor` 9, `Runtime/MeshModelingToolset` 5(Runtime 모듈이지만 모델링 도구 미리보기 표시 — line·point·triangle set·wireframe·octree dynamic mesh). 에디터 문서(EIP 21.1절 geometry editor, 33절 "모델링" 기본 도구 M6–M7)와 각 기능 편집기가 정한다(CD17). 그리는 경로는 편집 표시(CD12, RN PC9a)다.

**범위 밖**(124, CD22)

| 묶음 | 플러그인(클래스 수) | 수 | 근거 |
|---|---|---|---|
| 가상 제작·방송·모션 디자인 | `PL/VirtualProduction/` 17개 플러그인(Avalanche 23·SVGImporter 6·DMX 3개 플러그인 6·GeometryMask 3·ClonerEffector 2·Text3D 2·PerformanceCaptureWorkflow 2·그 밖 8), `Runtime/nDisplay`(21), `Media/LinearTimecode`(1)·`MediaPlate`(1), `Experimental/ImagePlate`(2)·`RemoteControlComponents`(1)·`ColorCorrectRegions`(1)·`PostProcessMaterialChainGraph`(1) | 80 | EIP 33절 "nDisplay·DMX·LED stage·전문 방송 — 일반 게임/컷씬과 다른 장비/동기화 제품 범위, 별도 확장" |
| 원격 송출·실시간 합성 | `Media/PixelStreaming`(3)·`PixelStreaming2`(5)·`Experimental/PixelStreamingPlayer`(2), `Compositing/Composite`(6)·`CompositeCore`(1), `Experimental/PanoramicCapture`(1) | 18 | 게임 런타임이 아니라 송출·합성·영상 캡처 제품 기능 |
| 특정 제작 제품 전용 | `MetaHuman/MetaHumanAnimator`(6 — 얼굴 캡처 제작 도구, Editor 모듈), `Enterprise/DataprepEditor`(1), `Runtime/ArchVisCharacter`(1 — 건축 시각화용 조작 캐릭터) | 8 | 특정 제작 제품·건축 시각화 전용 |
| Chaos 디버거 | `ChaosVD`(14), Mover의 `UMoverCVDSimDataComponent`(1) | 15 | Chaos 솔버 전용 기록·재생 도구와 그 자료. Rocks 물리는 Jolt이고 물리 debug recorder는 EIP 16절 범위 |
| 음악 | `Runtime/Harmonix`(3) | 3 | AU 9.2절이 범위 밖으로 정했다(UE 실험) |

**컴포넌트 아님**(20): 시험·mock 10(`UTestModifierComponent`(MeshPartition `Private/Test`, `UCLASS(Hidden)`)·`UMockHTNComponent`·`UMassNavigationTestingComponent`·`UNavCorridorTestingComponent`·`UZoneGraphTestingComponent`·`UZoneGraphAnnotationTestingComponent`·`USmartObjectTestRenderingComponent`·`UPCGUnitTestDummyComponent`·`USnapshotTestComponent`·`UTestSineSynthComponent`), `Runtime/NetworkPredictionExtras` 8(`.uplugin` "Non essential classes for Network Prediction. Samples, test maps, etc."), `ScriptPlugin` 2(`.uplugin` "An example of a script plugin").

### 9.6 선행 작업

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR40 | Control Rig 런타임: rig 그래프 실행기(UE RigVM 대응)·control·space switching·본 constraint | 없음 | EIP 24.1절 표 "Control Rig / IK Retarget P2 · M6–M7" | 애니메이션 문서(없음), EIP 15·24절 |
| CR41 | IK Rig·retarget: IK solver 묶음, retarget profile | 없음 | 같은 행 | 같음 |
| CR42 | Motion Warping: warp target·애니메이션 notify window·root motion 수정자, 애니메이션 속성 기반 root motion | 없음 | EIP 15절 "Motion Warping·stride/orientation warping·foot placement" | EIP 15절 |
| CR43 | 물리 기반 애니메이션 제어: body별 constraint drive·control 집합(UE Physics Control) | 없음 | EIP 15절 "ragdoll은 physics result를 bone별 blend weight로 합성". 2부 `PhysicalAnimationComponent`(CM14, 필수)와 같은 역할의 UE 정식 플러그인(표지 없음)이다 | 물리 문서, CR16·CR19 위 |
| CR44 | 애니메이션 tick 예산: significance에 따라 skeletal mesh tick 빈도를 낮추고 보간(UE AnimationBudgetAllocator) | 없음 | EIP 15절 "animation budget allocator는 거리·가시성·중요도별 update rate를 조절" | 애니메이션 문서 |
| CR45 | 능력 체계: attribute·effect·stack·duration·ability·cue·prediction key(UE GAS 대응). CR31의 gameplay task 위 | 없음 | EIP 24.2절 "능력 체계는 attribute base/current value, modifier aggregation, …" | EIP 24.2절 |
| CR46 | StateTree 런타임: 에셋·평가기·task·조건, AI 확장(brain 컴포넌트로 실행) | 없음 | EIP 19절 "State Tree·EQS·utility AI를 확장한다", 24.1절 표 P2 · M6–M7 | AI 문서(없음) |
| CR47 | Smart Object: interaction definition·slot·예약 token·접근 경로·중단 사유, subsystem | 없음 | EIP 24.2절 "SmartObject는 interaction definition·slot·예약 token·접근 경로·중단 사유를 …" | EIP 24.2절 |
| CR48 | 모듈식 게임플레이: 게임 프레임워크 Actor에 component·extension을 등록하는 관리자(UE `UGameFrameworkComponentManager` — receiver Actor, init state), Game Feature 수명. `PlayerStateComponent`의 소유 클래스 PlayerState(EIP 20.5절에만 있고 ER 2절 명칭에 없다 — ER에 추가 필요) | 없음 | EIP 24.1절 표 "Game Features / Plugins — component/input/data 등록 token P2 · M6", 24.2절 "GameFeature는 Installed → Registered → Loaded → Active" | EIP 24.2절 |
| CR49 | PCG 런타임: 그래프·point/attribute 데이터·실행·incremental rebuild, 런타임 생성(생성 원천 주변), GPU scene 인스턴스 직접 기록(RN 4.2절 인스턴스 종류) | 없음 | EIP 24.1절 표 "PCG 계열 P2/P3 · M7–M8" | PCG 문서(없음) |
| CR50 | Geometry cache: 정점 애니메이션 track 에셋·재생·스트리밍, Alembic 가져오기(선택 importer) | 없음 | EIP 15절 "geometry cache/Alembic 계열 … M7 이후 편입" | MA·가져오기 문서 |
| CR51 | 케이블 시뮬레이션: verlet 입자·길이 제약·충돌 질의(CR16). 렌더는 동적 메시(CR37) | 없음 | EIP 16.2절 "rope … 기본 이동과 joint 안정화 이후 개별 모듈로 추가", 16.3절 "fracture·vehicle·cloth·rope·buoyancy는 지원표와 sample을 갖고". 33.2절 표의 P3 목록은 "파괴/softbody/vehicle"뿐이라 rope는 물리 필수(M4–M7) 안에 남는다 — 그래서 케이블은 필수, vehicle은 후속이다 | 물리 문서 |
| CR52 | 대량 군중 실행 기반(실험 기반): Mass entity·fragment·processor·표현(LOD별 인스턴스·Actor 전환), ZoneGraph 차선·annotation, crowd 차선 관리, 2D 격자 | 없음 | EIP 24.1절 표 "Mass 계열 대량 처리 … Entity presentation bridge P2 · M7", 33.2절 표 "대량 군중용 2D 격자는 … 군중 시스템을 설계할 때 그 단계의 선행 작업으로" | 군중 문서(없음) |
| CR53 | 물(실험 기반): water body(바다·호수·강·custom)·spline·물 표면 메시와 shading model(EIP 14절 MAT4 water)·수중 후처리·물 충돌·buoyancy 질의·지형 반영 | 없음 | EIP 21절 "M7에는 water surface·underwater 원신호" | 환경 문서(없음) |
| CR54 | 오디오 게임플레이 확장: 재생 파라미터 묶음(여러 `AudioComponent`에 같은 파라미터·볼륨 수정자), 리버브 존 영역 속성에 submix send·효과 교체 | 설계 일부 | AU 3.15.2절(리버브·안팎 gain·LPF), AU1 파라미터 seqlock, AU10 모듈레이션 | AU |

### 9.7 선행 → 묶음

| 묶음 | 막는 선행 |
|---|---|
| CM20(추가: `ProceduralMeshComponent`) | CM1a, CR37 |
| CM22 애니메이션 확장 | CM7, CM13(Motion Warping·속성 root motion), CR19, CR40, CR41, CR42, CR44 |
| CM23 물리 제어 | CM7, CM14, CR43 |
| CM24 모듈식 게임플레이 | CR26, CR39, CR48 |
| CM25 능력 | CM16(`GameplayTasksComponent`), CR45 |
| CM26 StateTree·Smart Object | CM16(`BrainComponent`), CR23, CR46, CR47 |
| CM27 PCG | CM6, CR23, CR49 |
| CM28 geometry cache·케이블 | CM1a, CR16, CR37, CR50, CR51 |
| CM29 오디오 게임플레이 | CM4, CR54 |
| CM30 대량 군중(실험 기반) | CM6, CM26(`SmartObjectZoneAnnotations`), CR23, CR52 |
| CM31 물(실험 기반) | CM1a, CM3, CM5, CR16, CR53 |

1부 5절의 순서 원칙과 완료 조건을 그대로 따른다. CM30·CM31은 착수 때 UE 소스 재대조가 완료 조건에 더해진다(CD20).

### 9.8 결정(권장안)

| ID | 항목 | 권장안 | 근거 |
|---|---|---|---|
| CD19 | 플러그인 범위 | CD13을 대체한다. 플러그인도 클래스 단위로 분류한다. 한 플러그인이 후속·범위 밖이면 그 편집기 모듈 클래스도 같은 분류로 센다 | 사용자 지시(2026-11-23) |
| CD20 | 실험 플러그인 | 계획이 필수로 둔 기능이고 UE에 정식 대안이 없으면 "필수(실험 기반)"으로 둔다(물·대량 군중). 역할·이름·계층은 UE를 따르되 UE 실험 API는 바뀌므로, 착수 때 그 시점 UE 소스를 다시 대조하고 기능 문서가 구조를 확정한다. 대체(CD21)·제외·범위 밖(CD22)에 들지 않는 나머지 실험 플러그인은 후속이다(구조 참고). 계획 기능과 겹치지만 정본 구조가 따로 있는 것(DaySequence → 환경 문서의 sky·sun, InstancedActors → CM30 위 계층)도 후속이다 | 사용자 방침 "기본은 전부 가져온다", 애매하면 UE 방식. UE에 물·대량 군중의 정식 대안이 없다 |
| CD21 | 대체 | Mover → `CharacterMovementComponent`(CD15), Niagara → `EffectComponent`(CD16), Enhanced Input → `InputComponent`(CD7), AudioGameplayVolume → 리버브 존 영역(AU 3.15절 + CR54), Synthesis 신스 → `SoundGenerator`(AU 9.1절), CustomMesh → `ProceduralMeshComponent` | 이미 정한 구조와 겹치지 않게 한다 |
| CD22 | 범위 밖 | 가상 제작·방송·nDisplay·DMX·원격 송출·실시간 합성·특정 제작 제품·Chaos 디버거·건축 시각화·Harmonix는 들이지 않는다. 요구가 생기면 별도 확장 문서가 다룬다 | EIP 33절 "nDisplay·DMX·LED stage·전문 방송 — 별도 확장, 현재 기본 완료 조건에 미포함", AU 9.2절 |
| CD23 | 2D(Paper2D) | 후속. 계획에 2D 게임 도구 항목이 없으므로, 들이려면 EIP에 2D 항목을 먼저 넣는다 | 계획 범위는 사용자 결정 사항이다 |
| CD24 | 이름(3부·2부 정렬) | CD18 규칙에 더해: 약어는 한 낱말처럼 쓴다(코드 선례 `GpuScope` — `PcgComponent`·`IkRigComponent`·`StateTreeAiComponent`·`PcgProceduralIsmComponent`). 디버그 표시 `…Rendering`은 `…Draw`다. 어순·줄임말: `SkeletalMeshComponentBudgeted` → `BudgetedSkeletalMeshComponent`, `PCGGenSource` → `PcgGenerationSource`. 2부도 같은 규칙으로 맞춘다: `HLOD…` → `Hlod…`, `AIPerception…` → `AiPerception…`, `NavMeshRendering`·`NavLinkRendering`·`NavTestRendering` → `NavMeshDraw`·`NavLinkDraw`·`NavTestDraw` | 한 문서 안 이름 규칙 통일 |

### 9.9 다른 문서 개정

개정 6(2026-11-23)에서 모두 반영했다(EIP 개정 1.19 범위에 더함, AU 개정 26). 요구의 정본은 이 문서 9.6절이다.

| 문서 | 고친 것 |
|---|---|
| EIP 표지(1.19) | 3부 요약, 개정 범위에 15.1·24.1·33.2절 |
| EIP 15.1절 | Control Rig·IK Rig·Motion Warping·애니메이션 예산·geometry cache 컴포넌트(CM22·CM28) 링크, Live Link·ML deformer·cloth 계열 후속 |
| EIP 16.1절 | Physics Control(CM23)·케이블(CM28) 링크, Chaos vehicle·modular vehicle·motor sim 오디오는 vehicle 단계 후속 |
| EIP 21절 | 물(CM31, 실험 기반 — CD20)·PCG(CM27) 링크, 얕은 물 시뮬레이션은 고급 fluid 후속 |
| EIP 24.1절 표 아래 | Game Features·GAS·StateTree·Smart Objects·Mass·PCG에 딸린 컴포넌트 묶음(CM24–CM27·CM30) 링크 |
| EIP 33.2절 표 | "nDisplay·DMX·LED stage·전문 방송" 행에 범위 밖 플러그인 목록(9.5절, CD22) 링크 |
| AU 3.15.2절·9.1절 | 리버브 존 영역 속성에 submix send·효과 교체(CR54), AudioGameplayVolume mutator 대응. Synthesis 신스 컴포넌트 5종도 `SoundGenerator` 구현으로 간다는 것 |

남은 것: CR48의 PlayerState 명칭을 ER 2절에 더하는 일은 ER을 다음에 고칠 때 한다.

## 10. 확인하지 못한 것

- UE 컴포넌트 속성 전체 대조표(필드 단위)는 아직 만들지 않았다. 각 묶음 착수 때 그 묶음의 UE 헤더를 필드 단위로 대조해 이 문서에 붙인다(5절 완료 조건 5).
- `UInstancedSkinnedMeshComponent`·`UHeterogeneousVolumeComponent`의 UE 5.8 성숙도(실험 기능 여부)는 헤더만으로 판정하지 못했다(헤더에 실험 표지는 없다).
- Cascade의 공식 deprecated 표지는 `UVectorFieldComponent`·`UParticleSystemComponent` 헤더에 없다. 제외 근거는 사용처(구 FX 시스템 전용)다.
- 3부 수집(9.1절)은 `class … : public [ns::]U…` 선언을 정규식으로 찾는 방식이다. 매크로로 만든 선언은 놓칠 수 있다. 리뷰에서 namespace 한정 부모(`MeshPartition::UModifierComponent`) 때문에 빠진 26종을 찾아 고쳤고, 2부 재대조에서는 `UE_DEPRECATED(…)`가 끼인 선언 4건을 찾았다.
- 베타 플러그인(ModularGameplay·AudioGameplay·MotionWarping 등)의 UE 안 성숙도 변화는 추적하지 않는다. 착수 때 `.uplugin` 표지를 다시 본다.

## 11. 개정 기록

- 개정 6(2026-11-23): 9절 플러그인 컴포넌트(3부) 추가(사용자 지시) — `Engine/Plugins` 139개 플러그인의 클래스 418개를 상속 추적으로 전수 분류(필수 31·필수(실험 기반) 25·대체 28·제외 10·후속 154·편집기 26·범위 밖 124·아님 20), 선행 CR40–CR54, 묶음 CM22–CM31(+ CM20에 `ProceduralMeshComponent`), 결정 CD19–CD24. CD13(색인만)을 CD19로 대체하고 8.9절 색인을 지움. 같은 추적으로 2부 재대조 — 빠진 deprecated 4종(`UAtmosphericFogComponent`·`ULandscapeMeshCollisionComponent_DEPRECATED`·GeometryCollection ISM pool 2종)을 8.3절 제외에 더함(2부 제외 4 → 8). 2부 이름을 CD24 규칙으로 맞춤(`Hlod…`, `AiPerception…`, 내비 디버그 표시 `…Draw`). 9.9절 다른 문서 개정 반영(EIP 15.1·16.1·21·24.1·33.2절, AU 3.15.2·9.1절). 리뷰어 1명 교차검증 반영: namespace 한정 부모로 빠진 26종(MeshPartition modifier·MeshPartitionWater·PCG 어댑터·시험 1), EIP 인용 절 번호 정정(24.3절 Live Link, 16.2·16.3·33.2절 rope·vehicle), CR43·CR44 근거, `UWaterTerrainComponent` 역할(지형 → 물 정보 텍스처), DaySequence·InstancedActors의 CD20 판정 사유, MetaHumanSDK·LidarPointCloud를 범위 밖에서 후속으로, `UDirectMeshControlComponent`(실험)를 편집기에서 후속으로, `UMoverCVDSimDataComponent`를 대체에서 범위 밖(Chaos 디버거)으로, CR48에 PlayerState, CM30·CM31 선행 정리, ISM pool 정의 줄, MotoSynth 의존 행 분리. 확인하지 못한 것·개정 기록을 10·11절로 옮김.
- 개정 5(2026-11-23): 8.8절 다른 문서 개정 반영(EIP 16.1·19·20.5·21절).
- 개정 4(2026-11-23): 8절 폴더 밖 컴포넌트(2부) 추가(사용자 지시) — Engine 다른 폴더·Runtime 다른 모듈 전수 분류(필수 56·대체 2(`USynthComponent`·`UMediaSoundComponent` — AU 9.1절)·제외 4·후속 38·편집기 9·아님 13), 선행 CR27–CR39, 묶음 CM12–CM21, 결정 CD13–CD18, 플러그인 색인. `PathMoverComponent`를 CM5에서 CM12로. 리뷰어 1명 교차검증 반영: 빠진 `UMediaSoundComponent`(대체)·`UMockDataMeshTrackerComponent`(XR 후속), 절차 식생은 UE 실험 기능이라 후속, AI 컨트롤러 선행 CR39, CR29 정본을 EIP 20.5절 `CameraDirector`로, CM12·CM13에 CM3, `LandscapeMeshProxyComponent`를 CM19로, 플러그인 색인의 모델링 도구·Dataflow·단계 표기 정정. 확인하지 못한 것·개정 기록을 9·10절로 옮김.
- 개정 3(2026-11-23): 남아 있던 두 항목을 권장안으로 확정(사용자 지시) — CD11 Tick 단계 UE 대응(ER 8절 대응표, EIP 20.2절), CD12 디버그·편집 표시(RN 4.2절 세 경로, 11.2절 PC9a·PC9b, 17절 RS13). CR8a·CR23 상태를 결정으로.
- 개정 2(2026-11-23): 7절 다른 문서 개정 반영. CR23 정본을 RenderBase에서 Renderer 4.2절로 정정. `LightmassPortal` 대응 이름을 `LightPortal`에서 `LightBakePortal`로(사용자 지적 — 베이크 전용 의미, CD6).
- 개정 1(2026-11-23): 리뷰어 1명 교차검증 반영 — CM1을 CM1a(정적 메시, RS0)·CM1b(광원, RS1·RS3)로 나누고 화면 마일스톤 범위를 CM0+카메라로 정정, 게임 프레임워크 선행 CR26 추가(입력·햅틱), 볼륨 안개·구름을 CM11로, CR5 근거(객체 단위 EditorOnly 미활성), 3절과 4.6절 선행 불일치·ID 오기 정정, PawnNoiseEmitter·HISM·InputComponent 인용 정정, 1.2절 Runtime 다른 모듈(UMG·Foliage·Landscape·AIModule).
- 개정 0(2026-11-23): 첫 초안. 분류(필수 58·대체 3·제외 4·후속 프로필 1), 선행 CR1–CR25, 묶음 CM0–CM11, 결정 CD1–CD10 권장안.
