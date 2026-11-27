# 엔진 컴포넌트 도입 — 8. 폴더 밖 컴포넌트(2부)

[목차](README.ko.md)

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

**편집기 쪽**(9): `UGizmoBaseComponent`·`UGizmoArrowComponent`·`UGizmoBoxComponent`·`UGizmoCircleComponent`·`UGizmoLineHandleComponent`·`UGizmoRectangleComponent`·`UViewAdjustedStaticMeshGizmoComponent`(`RT/InteractiveToolsFramework/Public/BaseGizmos/`), `ULandscapeGizmoRenderComponent`, `ULevelInstanceComponent`(레벨 인스턴스의 편집용 인스턴스 Actor Transform을 맞추는 편집 도우미 — `RT/Engine/Public/LevelInstance/LevelInstanceComponent.h:15–30`). 런타임 카탈로그에 넣지 않고 에디터 문서(gizmo)와 [Prefab](../Prefab.ko.md) PF5·PF9(레벨 인스턴스)가 정한다(CD17). 그리는 경로는 편집 표시(CD12, RN PC9a)다.

**컴포넌트 아님**(13): 시험 전용 3(`UAutoRTFMTestChildActorComponent`·`UAutoRTFMTestPrimitiveComponent`·`USSAMTestComponent`), `UObject` 파생 2(`UClientPilotComponent`·`UExternalRpcRegistrationComponent`), 인터페이스 2(`UPhysicsComponent`·`UToolFrameworkComponent`), UMG 위젯 확장 5(`UUIComponent`·`UMouseHoverComponent`·`UScaleBoxComponent`·`USizeBoxComponent`·`UNavigationUIComponent` — Actor가 아니라 위젯에 붙는 `UObject`, `RT/UMG/Public/Extensions/UIComponent.h`, [UI 툴킷](../UiToolkit/README.ko.md) 몫), 패킷 처리기 팩토리 1(`UEngineHandlerComponentFactory`).

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
| CR32 | world-space UI: Widget 층을 render target에 그리기, world hit test·포인터 입력 라우팅 | 설계 있음(코드 없음) | UiToolkit 5.9절 "월드 공간 Widget", 3절 `WorldIntegration` 행의 `UWidgetComponent` 대응 | [UI 툴킷](../UiToolkit/README.ko.md) 5.9절·UT6, EIP 21.4절 |
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
