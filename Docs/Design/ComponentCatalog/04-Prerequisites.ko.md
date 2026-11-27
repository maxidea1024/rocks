# 엔진 컴포넌트 도입 — 4. 선행 작업

[목차](README.ko.md)

## 4. 선행 작업

이 절은 **컴포넌트를 만들기 전에 있어야 하는 것**이다. 상태는 2026-11-23 코드·문서 기준이다. "정본" 열의 문서가 그 작업을 정하고, 이 문서는 컴포넌트 쪽 요구만 적는다(요구가 정본에 없으면 "추가 요구"로 표시하고 7절에서 그 문서에 반영한다).

### 4.1 World 기반

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR1 | 컴포넌트 시스템 상태 수명: 등록 안에서 렌더·물리·오디오 상태를 만들고 지우는 hook과 "다시 보내기" 표시(UE `CreateRenderState`·`MarkRenderStateDirty`·`CreatePhysicsState` 대응). 등록 실패 시 부분 등록 해제. 대량 등록 묶음 | 구현(CM0a — ER 23.2절, 대량 등록 묶음은 CM1a·RS0) | `World/Component.h`에 `OnRegister`·`OnUnregister`만 있다 | EIP 20.2절(순서 Register 단계), 추가 요구 |
| CR2 | SceneComponent 보강: 부착 규칙(KeepRelative·KeepWorld·SnapToTarget — UE `EE/EngineTypes.h:62`), socket 이름 부착과 `GetSocketTransform`, 절대 location·rotation·scale, Mobility(Static·Stationary·Movable), 가시성(visible 자식 전파·hiddenInGame), 속도, bounds 계산 가상 함수와 bounds 갱신, Transform 변경 통지(구독형 — CR7) | 없음 | `World/SceneComponent.h`는 상대 Transform·계층·월드 행렬만 있다 | ER 7.3절, 추가 요구 |
| CR3 | 부착 계층 영속(부모 WeakObjectRef PROPERTY) | 없음(자리) | ER 14절 "부착 계층은 저장되지 않고" | PF 5.1절 World 단계, 리플렉션 B9d1 |
| CR4 | Component 시작 준비 hook(`IsReadyToStart`) — 메시·텍스처 로드 대기 | 구현(ER S9b — 17.4절) | SE-W2 | ER 7.2절, SE-W2 |
| CR5 | EditorOnly 컴포넌트와 쿠킹 제외, 태그(`TagSet`) | 일부(런타임 EditorOnly — CM0, Name 태그 — ER 31장) | 런타임 EditorOnly 규칙은 CM0(ER 23.2절), Name 배열 태그(UE `Tags`·`ComponentTags`)는 ER 31장이다. 계층 태그 사전과 쿠킹 쪽이 남는다. 처음 조사: `World/Component.h`에 없었다. A0-B `EditorOnly` 지정자는 필드 단위다. 객체 단위 EditorOnly는 패키지 Exports 플래그로 예약만 됐고 클래스 지정자가 없으며 reader는 그 비트를 `CorruptPackage`로 거부한다(AssetSystem 243행 — 켜려면 schemaVersion을 올린다). 태그 사전은 계획만 있다 | EIP 20.1절(태그), AssetSystem 패키지 형식 개정, MetaCompiler 클래스 지정자 |
| CR6 | 컴포넌트 타입 규칙(Actor당 하나·필수 동반·상호 배타)을 타입 메타로 | 구현(CM0a — `Component::TypeRules`, ER 23.2절) | — | EIP 20.2절 |
| CR7 | 이벤트 slot 표(overlap·hit·타임라인·재생 끝 등 컴포넌트 이벤트를 구독이 있을 때만 저장) | 없음 | SE-W8 | SE-W8, EIP 20.2절 |
| CR8 | World 필드 편집 setter·통지(CU5) — 속성을 바꾸면 CR1의 "다시 보내기" 표시 | 없음 | ER 14절 "World 필드는 ReadOnly" | ReflectionExtensions CU5 |
| CR8a | Tick 단계 UE 대응: Rocks 단계(PrePhysics·Physics·PostPhysics·LateUpdate·PreRender)와 UE `ETickingGroup`(`EE/EngineBaseTypes.h:86–104`) 대응, 물리 결과 반영·skeletal mesh 물리 마무리 지점 | 결정(CD11) — 구현은 물리·애니메이션 컴포넌트와 함께 | ER 8절 대응표 | ER 8절 |

### 4.2 에셋·데이터

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR9 | 에셋 런타임 A1–A5: 패키지·로더·`AssetLease`(WorldOnly 클래스 멤버 허용 — A0-B), 클래스 기본값 레코드(A5). 컴포넌트가 메시·머티리얼·사운드를 필드로 잡는 전제 | A1–A3 완료, A4 진행(A4c2까지) | 메모리·AssetSystem 1절 | [에셋 시스템](../AssetSystem/README.ko.md) |
| CR9a | Prefab PF1(클래스 원형·default subobject) — 컴포넌트를 클래스 원형으로 선언하고 `ChildActorComponent`가 템플릿 Actor를 갖는 전제 | 설계만 | PF 5.2절 | PF |
| CR10 | 커브 에셋(Curve 에셋 층 — 타임라인·햅틱 효과·감쇠 커브) | 값 타입 층 완료(CV1–CV10), 에셋 층 설계 완료(2026-11-27 — CA1–CA6) | — | CurveData, [커브 에셋](../CurveAssets/README.ko.md) |

### 4.3 렌더

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR11 | 카메라·view: `CameraComponent`(1.2절)와 `ViewDesc` 생성 | `CameraComponent` 구현(CM0c — ER 23.4절), `ViewDesc` 생성은 RS0 `WorldIntegration` | — | RN 5.1절, 별도 문서 |
| CR12 | `WorldIntegration`·`RenderData`: 컴포넌트 변경 감지 → `SceneChangeList`(인스턴스·광원 추가·제거·변환·재질), `RenderNotices` 처리. 인스턴스 종류(정적 메시·인스턴스드·skinned·sprite) 등록 | 없음(모듈 없음) | RN 3.2·4.2절 | RN |
| CR13 | 렌더러 단계 RS0(정적 메시·Unlit)·RS1(광원)·RS3(그림자)·RS5(투명·하늘·안개)·RS6(후처리)·RS7(capture·decal·bake 표본)·RS9(rect)·RS10(screen-space)·RS12(skinned) | 설계만(화면 마일스톤 뒤) | RN 17절 | RN |
| CR14 | 머티리얼 런타임: section별 머티리얼 `RenderHandle`, 인스턴스 파라미터 override, 실행 중 머티리얼 인스턴스(UE `UMaterialInstanceDynamic` 대응) | MS3a 골격만 | 메모리 mesh-unreal-exchange-spec | [머티리얼 시스템](../MaterialSystem/README.ko.md) |
| CR14a | 텍스처 스트리밍 입력: 컴포넌트가 내는 텍스처 사용 기록(UV 밀도·화면 크기 — UE `FRenderAssetOwnerStreamingState`) | 설계만 | TextureStreaming 4.1절 | TextureStreaming |
| CR15 | 추가 view와 render target 에셋(scene capture·planar reflection) | 없음 | — | RN 5절, AssetSystem 텍스처 계열 |
| CR23 | 디버그·편집 표시: 선·도형·아이콘 billboard(EditorOnly 시각화 포함) | 결정(CD12), 설계 RN RS13 — 코드 없음 | `Sandbox` 샘플 렌더러뿐(ER 3절). UE는 line batch proxy가 프레임마다 동적 메시 요소를 내는 렌더러 경로다(`Engine/Private/Components/LineBatchComponent.cpp:60`) | RN 4.2절 "디버그·편집 표시", 11.2절 PC9a·PC9b, 17절 RS13 |
| CR18 | 글꼴 에셋·글리프 atlas(`TextRenderComponent`) | 없음 | — | [글꼴 설계](../Font/07-Consumers.ko.md) 7.3절(미리 구운 글꼴만 — UE와 같다), 단계 FT5(미리 구운 글꼴)·FT7(사용처) |

### 4.4 물리

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR16 | 물리 모듈(Jolt): body(UE `FBodyInstance` 대응), 충돌 채널·응답·프로필, overlap·hit 이벤트, sweep 이동(UE `MoveComponent`), scene query, simulate physics, ragdoll용 physics asset | 설계·데이터 있음(2026-11-25) | Jolt 5.7.0 반입(JL1), 충돌 데이터 `Collision` 모듈(CD1)·`PrimitiveComponent` 연결(CD2) 구현. Physics 모듈은 상세 설계만([물리 모듈](../PhysicsModule/README.ko.md) PH0 — 구현은 M4 PH1–PH6). 어댑터 경계는 Jolt 사실 확인 완료(개정 3) | EIP 16절, PhysicsJoltAdapter, CollisionData, PhysicsModule, CylinderShape CY2 |
| CR17 | 영역 형상 `VolumeShape`(상자·구·캡슐·볼록체)와 물리 없는 내부 판정 — brush volume 대체(CD1). 후처리 볼륨·리버브 존·트리거 아닌 영역이 같은 형상과 판정을 쓴다 | 구현(041c356de — Core `VolumeShape`·`ConvexHull`, Shape 컴포넌트 월드 형상, 후처리 볼륨 모으기·혼합). 남은 것: 볼록체 출처(메시 단순 충돌)를 컴포넌트에 잇기 — 메시 컴포넌트 뒤, 리버브 존 연결은 오디오 AU3 | `Core/Math/VolumeShape.h`, `World/PostProcess/PostProcessVolumeBlending.h` | [ER 28장](../EngineRuntime/28-VolumeShape.ko.md), AU 3.15.2절 |

### 4.5 애니메이션·오디오·입력·큰 월드

| ID | 요구 | 상태 | 확인 근거 | 정본 |
|---|---|---|---|---|
| CR19 | 애니메이션 런타임: pose 평가·blend·본 transform·소켓·root motion·LOD별 본 맵, skinning 데이터 전달 | 에셋만(`Animation` 모듈은 Skeleton·Clip 에셋) | `Engine/Source/Runtime/Animation/Public` | EIP 15절, MA 6·7절 |
| CR20 | 오디오 엔진 연결 AU3(`AudioWorldSubsystem`·emitter 등록) | 설계만 | AU 3.8절 | AU |
| CR21 | 입력 액션·매핑 컨텍스트·리바인딩, 햅틱 출력(XInput 진동 등) | 구현(InputActions IA1–IA8 — 에셋 감싸기 IA6a·IA6b, 함수 바인딩 IA7, 입력 스택 IA8). 남은 것: 편집기 IA6c(Studio), Mac 진동 | `Engine/Source/Runtime/InputActions` | [InputActions](../InputActions/README.ko.md), EIP 20.4절 |
| CR22 | World Partition(셀·streaming source·data layer) | 없음 | SE-W7 | EIP 10.2절 |
| CR26 | 게임 프레임워크: PlayerController·LocalPlayer(입력 컴포넌트 스택, 햅틱 효과 관리, 주 리스너·view 소유) | 골격 구현(ER S10–S14 — `Gameplay` 모듈), 햅틱 구현(InputActions IA5), 입력 컴포넌트 스택·`InputComponent` 구현(InputActions 5.5절 IA8 — 2026-11-27 사용자 결정으로 당김) | `Engine/Source/Runtime/Gameplay` | ER 1절 S10–S12, EIP 20.4·20.5절 |
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

### 4.7 셰이더 선행(SL 단계)

컴포넌트 묶음마다 셰이더 영역과 단계를 함께 들인다. 정본은 [엔진 셰이더](../EngineShaders/05-ComponentCoverage.ko.md) 5장이고(SLR7), 이 표는 묶음 쪽에서 본 요약이다. 컴포넌트를 셰이더보다 먼저 만들지 않는다(5절 "동작하는 척하지 않는다"와 같다).

| 묶음 | 셰이더 단계(기본 영역 — 5.1절) | 기본 영역 밖(5.2절, 그 기능 문서와 함께) |
|---|---|---|
| CM0 카메라 | SL1(화면 마일스톤 — `ViewConstants`·노출·tone map 최소). CM0 World 쪽은 2026-11-24에 앞당겨 구현됐고, 셰이더는 SL1과 함께다 | — |
| CM1a 정적 메시 | SL2. custom depth·stencil 플래그는 SL8(SLR11 — RN에 단계 요구, 5.3절) | — |
| CM1b 광원 | SL3·SL5 | light shaft → A25 |
| CM2 시각화 | SL10(SL10e — 디버그 표시, 5.3절) | — |
| CM3 충돌 형상 표시 | SL10 | — |
| CM5 게임플레이 도구 | `TextRenderComponent`는 SL2 + SLU(글리프 표본 — SLU 뒤) | — |
| CM6 인스턴싱·스플라인 메시 | SL6 | — |
| CM7 skinned 계열 | A22(skinning 전체) | — |
| CM8 대기·후처리·decal·rect | SL7(대기·안개)·SL8·SL9(decal·후처리)·SL10(rect light) | 볼륨 안개 선택 → A25 |
| CM9 반사·캡처 | SL9·SL10 | — |
| CM10 정적 조명·가상 텍스처 | — | A33(런타임 가상 텍스처) |
| CM11 큰 월드·후속 | — | A25(볼륨 안개·구름·이종 볼륨), A22 후속 부분(애니메이션 bank) |
| CM15 카메라 효과·시네마틱 | SL8(노출·필름백) | A24(DOF·움직임 흐림·지역 노출·렌즈 효과) |
| CM17 위젯·미디어 | `WidgetComponent`는 SLU + SL2·SL7 | `MediaComponent` → A35·A18 후속 부분 |
| CM18·CM19 지형·HLOD | 인스턴스 경로는 SL6 | A34(지형 정점 입력·컬링·layer, 풀 무게) |
| CM20·CM28 동적·절차 메시·케이블·geometry cache | SL2(프레임 업로드 정점) | geometry cache → A34 |
| CM21 효과 | — | Niagara 플러그인 셰이더(사용자 결정으로 후속) |

- 거리장·capsule 그림자는 A27이다(광원 묶음의 해당 설정 — 그 전에는 설정을 켜면 `Unsupported`로 진단한다).
- 셰이더 단계 순서와 묶음 순서가 다른 곳(custom depth, 디버그 표시, 글리프)은 엔진 셰이더 5.3절이 처리를 정한다.
