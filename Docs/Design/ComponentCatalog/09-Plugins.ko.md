# 엔진 컴포넌트 도입 — 9. 플러그인 컴포넌트(3부)

[목차](README.ko.md)

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
