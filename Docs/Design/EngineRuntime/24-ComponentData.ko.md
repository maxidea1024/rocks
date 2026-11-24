# 엔진 런타임 — 24. 컴포넌트 데이터 먼저(D1–D5)

[목차](README.ko.md)

## 24. 컴포넌트 데이터 먼저(D1–D5)

설계일 2026-11-24. 사용자 결정(2026-11-24): 렌더·물리 시스템 없이 정할 수 있는 컴포넌트 데이터는 지금 넣는다. [컴포넌트 도입](../ComponentCatalog/05-BundleOrder.ko.md) 5절 "시스템이 없으면 컴포넌트를 만들지 않는다"에 대한 예외다. 이 장은 그 상세 설계다. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`(UE 5.8.4).

### 24.1 범위

| 묶음 | 내용 | 시스템 연결(나중) |
|---|---|---|
| D1 | 광원 컴포넌트(Point·Spot·Rect·Directional·Sky)와 광원 Actor | 렌더 장면 등록(CR12 SceneChangeList·RS1·RS3) |
| D2 | `PrimitiveComponent`·`MeshComponent`·`StaticMeshComponent`와 `StaticMeshActor` | 렌더 proxy(RS1), 충돌(CR16) |
| D3 | `SplineComponent`(끝까지 동작) | 없음 |
| D4 | 이동(Movement·Rotating·Projectile·SpringArm·PathMover), sweep 없이 끝까지 동작 | sweep·충돌 응답(CR16) |
| D5 | 대기·높이 안개·decal·후처리 볼륨·반사 캡처 데이터 | RS6·RS9 등 |

기준:
- **데이터**는 UE 5.8의 속성·기본값·단위·Mobility 규칙이다. 리플렉션 영속 필드로 넣는다(`ROCKS_PROPERTY(ReadOnly)`, 변경은 setter — CR8 전 23.3절 규칙과 같다).
- **순수 계산**(단위 변환, 원뿔 각 clamp, 경계 구)은 지금 구현하고 UE 값으로 시험한다. 렌더러가 그대로 쓴다.
- **시스템 연결**은 하지 않는다. 시스템 상태(23.2절 `WantsSystemState`)를 요청하지 않으므로 아무것도 그리지 않는다. setter는 Render 시스템 상태가 있을 때만 다시 보내기를 표시한다(`MarkSystemStateDirty(Render)` — 상태가 없으면 아무것도 하지 않는다). 그래서 RS 단계가 상태를 붙이면 setter를 고치지 않아도 된다.
- 모듈은 World다(CameraComponent와 같은 자리 — 23.4절). 에셋 참조(텍스처·머티리얼)가 필요한 속성은 그 참조 필드 체계가 World에 들어올 때 넣는다(24.2.8절).

D2–D5는 묶음마다 이 장에 절을 더해 설계한다(설계 → 구현 → 리뷰어 1명 → 커밋).

### 24.2 D1 — 광원

#### 24.2.1 클래스

| Rocks(World) | UE | 부모 |
|---|---|---|
| `LightComponentBase`(Abstract) | `ULightComponentBase` | `SceneComponent` |
| `LightComponent`(Abstract) | `ULightComponent` | `LightComponentBase` |
| `LocalLightComponent`(Abstract) | `ULocalLightComponent` | `LightComponent` |
| `PointLightComponent` | `UPointLightComponent` | `LocalLightComponent` |
| `SpotLightComponent` | `USpotLightComponent` | `PointLightComponent` |
| `RectLightComponent` | `URectLightComponent` | `LocalLightComponent` |
| `DirectionalLightComponent` | `UDirectionalLightComponent` | `LightComponent` |
| `SkyLightComponent` | `USkyLightComponent` | `LightComponentBase`(UE도 `ULightComponent`가 아니다) |
| `Light`(Abstract Actor) | `ALight` | `Actor` |
| `PointLight`·`SpotLight`·`RectLight`·`DirectionalLight` | `APointLight` 등 | `Light` |
| `SkyLight` | `ASkyLight`(`AInfo`) | `Light`(Rocks에 `Info`가 없다 — 켜고 끄기 API를 같이 쓴다) |

이름은 UE 이름에서 접두만 뗐다. 광원 이름(point·spot·directional·sky light)은 업계 공용어라 Rocks식으로 바꾸지 않는다(`PlayerStart`와 같은 방식).

열거·구조체:
- `LightUnits : uint8 { Unitless, Candelas, Lumens, EV, Nits }`(UE `ELightUnits` — `E/Classes/Engine/Scene.h:83`). EV는 EV100이다.
- `SkyLightSourceType : uint8 { CapturedScene, SpecifiedCubemap }`(UE `ESkyLightSourceType`).
- `LightingChannels { channel0 = true, channel1 = false, channel2 = false }`(UE `FLightingChannels` — `EngineTypes.h:568`). 리플렉션 구조체다.

#### 24.2.2 필드와 기본값

모든 필드는 영속(`ROCKS_PROPERTY(ReadOnly)`)이다. 수치는 UE처럼 `float`다(렌더러 입력과 같은 정밀도). 줄 번호는 UE 생성자다.

`LightComponentBase`(`E/Private/Components/LightComponent.cpp:432–452`):

| 필드 | 기본 | 뜻 |
|---|---|---|
| `intensity` | π(3.1415927) | 방출 에너지. 단위는 파생이 정한다 |
| `lightColor` | `Color::White()` | 필터 색, sRGB 인코딩 byte(UE `FColor`) |
| `affectsWorld` | 참 | 거짓이면 광원 전체가 꺼진다(게임 World에서 등록 뒤 바꿀 수 없다 — UE 주석) |
| `castShadows`·`castStaticShadows`·`castDynamicShadows` | 참·참·참 | 그림자 주 스위치·정적·동적 시전체 |
| `affectTranslucentLighting` | 참(Sky 거짓 — UE는 `ULightComponent` 생성자만 켠다) | 반투명에 빛을 준다 |
| `transmission` | 거짓 | subsurface 투과 |
| `castVolumetricShadow` | 거짓(Directional·Sky 참) | 볼륨 안개 그림자 |
| `indirectLightingIntensity` | 1 | GI 배율 |
| `volumetricScatteringIntensity` | 1 | 볼륨 안개 산란 배율 |

`LightComponent`(`LightComponent.cpp:459–508`):

| 필드 | 기본 | 뜻 |
|---|---|---|
| `temperature`·`useTemperature` | 6500 K·거짓 | 흑체 색온도(D65 = 6500)와 사용 여부 |
| `maxDrawDistance`·`maxDistanceFadeRange` | 0·0 (cm) | 그리기 멈춤 거리(0 이하 = 제한 없음)와 그 앞 fade 폭 |
| `specularScale`·`diffuseScale` | 1·1 | [0, 1] |
| `shadowResolutionScale` | 1 | 0이면 그림자 맵을 끈다 |
| `shadowBias`·`shadowSlopeBias` | 0.5·0.5 | 깊이·기울기 편향(Directional만 [0, 10] — UE `DirectionalLightComponent.cpp:1107`) |
| `shadowSharpen` | 0 | 그림자 필터 선명도 |
| `contactShadowLength`·`contactShadowLengthInWorldSpace` | 0·거짓 | 접촉 그림자 길이(0 = 끔, 기본은 화면 공간 비율, 참이면 cm) |
| `castTranslucentShadows` | 거짓(Directional 참) | 반투명 물체가 그림자를 드리운다 |
| `lightingChannels` | {참, 거짓, 거짓} | 광원 채널(setter는 UE처럼 Mobility를 보지 않는다 — `LightComponent.cpp:1366`) |

`LocalLightComponent`(`LocalLightComponent.cpp:10–18`): `intensity` 기본을 **5000**으로 바꾼다.

| 필드 | 기본 | 뜻 |
|---|---|---|
| `intensityUnits` | `Unitless` | `intensity`의 단위 |
| `attenuationRadius` | 1000 cm | 영향 반경(물리 값이 아니라 경계), 0 이상(음수는 경계 구가 뒤집힌다 — UE UIMin 8) |
| `inverseExposureBlend` | 0 | 노출 무관 밝기로 섞는 비율 [0, 1] |

UE 편집기는 Point·Spot·Rect Actor를 놓을 때 `r.DefaultFeature.LightUnits`(기본 Candelas)로 단위를 바꾼다(`Editor/UnrealEd/Private/Factories/ActorFactoryPointLight.cpp:23`). 이는 편집기 배치 동작이라 컴포넌트 기본값이 아니다. 실행 중 Spawn한 UE 광원은 5000 Unitless 그대로다. Rocks도 컴포넌트·Actor 기본은 5000 Unitless이고, 편집기 배치 단위는 Studio의 배치 도구가 정한다.

`PointLightComponent`(`PointLightComponent.cpp:14–35`):

| 필드 | 기본 | 뜻 |
|---|---|---|
| `useInverseSquaredFalloff` | 참 | 물리 1/r² 감쇠(반경은 clamp로만) |
| `lightFalloffExponent` | 8 | 1/r²가 꺼졌을 때 반경 감쇠 지수 |
| `sourceRadius`·`softSourceRadius`·`sourceLength` | 0·0·0 (cm) | 구 광원 반지름·부드러운 반지름·캡슐 길이, [0, 10000] |

`SpotLightComponent`(`SpotLightComponent.cpp:15–33`): `innerConeAngle` 0°, `outerConeAngle` 44°. 둘 다 **반각**(도)이다.

`RectLightComponent`(`RectLightComponent.cpp:85–109`): `sourceWidth`·`sourceHeight` 64·64 cm([0, 100000]), `barnDoorAngle` 88°([0, 88] — `GetRectLightBarnDoorMaxAngle`), `barnDoorLength` 20 cm(0.1 이상 — UE `SetBarnDoorLength`의 `Max(0.1)`, `RectLightComponent.cpp:160`).

`DirectionalLightComponent`(`DirectionalLightComponent.cpp:1014–1083`): `intensity` 기본 **10**(lux — UE `GetLightUnits`는 Unitless를 돌려주고 주석이 lux다), `castTranslucentShadows`·`castVolumetricShadow` 참.

| 필드 | 기본 | 뜻 |
|---|---|---|
| `lightSourceAngle`·`lightSourceSoftAngle` | 0.5357°·0 | 태양 각지름, [0, 5] 권장 |
| `shadowAmount` | 1 | [0, 1] |
| `dynamicShadowDistanceMovableLight` | 40000 cm | Movable일 때 cascade 그림자 거리 |
| `dynamicShadowDistanceStationaryLight` | 0 | Stationary일 때 |
| `dynamicShadowCascades` | 4 | [0, 10] |
| `cascadeDistributionExponent` | 3 | [0.1, 10] |
| `cascadeTransitionFraction` | 0.1 | [0, 0.3] |
| `shadowDistanceFadeoutFraction` | 0.1 | [0, 1] |
| `farShadowCascadeCount`·`farShadowDistance` | 0·300000 cm | 원거리 cascade |
| `forwardShadingPriority` | 0 | 0 이상, Forward 경로에서 주 방향광 고르기 |
| `atmosphereSunLight`·`atmosphereSunLightIndex` | 참·0 | 대기 태양 역할과 번호 [0, 1] |
| `atmosphereSunDiskColorScale` | `LinearColor` 흰색 | 태양 원반 색 배율 |

`SkyLightComponent`(`SkyLightComponent.cpp:294–342`): `intensity` 1, **Mobility Stationary**(컴포넌트 생성자가 정한다), `castVolumetricShadow` 참.

| 필드 | 기본 | 뜻 |
|---|---|---|
| `realTimeCapture` | 거짓 | 매 프레임 하늘 캡처 |
| `sourceType` | `CapturedScene` | 장면 캡처·지정 cubemap |
| `sourceCubemapAngle` | 0° | 지정 cubemap 회전(UE처럼 자르지 않는다 — [0, 360]은 UI 범위) |
| `cubemapResolution` | 128 | 2의 거듭제곱으로 올리고 [8, 16384]로 자른다(24.2.4절) |
| `skyDistanceThreshold` | 150000 cm | 이보다 먼 기하를 하늘로 본다 |
| `captureEmissiveOnly` | 거짓 | |
| `lowerHemisphereIsBlack`·`lowerHemisphereColor` | 참·`LinearColor` 검정 | 아래 반구 |

#### 24.2.3 setter와 Mobility

- 필드마다 getter와 setter가 있다. 값 검증 실패(비유한, 범위 밖)는 `InvalidArgument`이고 값을 바꾸지 않는다. 범위는 24.2.2절 표와 UE `ClampMin`·`ClampMax`다. UE `UIMin`·`UIMax`(편집기 슬라이더 범위)는 거부 조건이 아니다.
- `intensity`는 음수를 거부한다. 단, 단위가 EV면 음수도 받는다(UE `PostEditChangeProperty` — `LightComponent.cpp:781`). 단위가 EV에서 다른 단위로 바뀌면(`SetIntensityUnits`, 1/r² 끄기) 음수 세기를 0으로 둔다(UE가 편집 뒤 같은 clamp를 한다).
- UE `PostEditChangeProperty`·실행 중 setter의 clamp(Specular·Diffuse [0, 1], 방향광 cascade 값·그림자 편향, Rect 크기 1 이상, barn door 길이 0.1 이상 등)는 Rocks setter에서 거부 조건이다(값을 몰래 바꾸지 않는다).
- **Mobility 검사**(UE `AreDynamicDataChangesAllowed` — `E/Classes/Components/SceneComponent.h:1363`): 대상 조건은 23.3절 이동 검사와 같다(등록됨, Game·Tryout World, 소유 Actor가 Initialized 이후).
  - 대부분의 setter는 **Static**일 때 거부한다(Stationary는 실행 중 세기·색·온도를 바꿀 수 있다).
  - `attenuationRadius`·Spot 원뿔 각은 **Static·Stationary** 모두 거부한다(UE `AreDynamicDataChangesAllowed(false)` — `LocalLightComponent.cpp:20`, `SpotLightComponent.cpp:59`).
  - `affectsWorld`는 Mobility와 무관하게 거부한다(UE: 실행 중 바꿀 수 없다). 진단은 `ComponentDataLockedInGame`이다.
  - Mobility 거부는 `InvalidArgument`와 Warning 진단 `StaticComponentDataChanged`다. UE는 조용히 무시한다. Rocks는 이동 검사(`StaticComponentMoved`)와 같게 오류로 알린다.
  - 검사는 `SceneComponent`의 protected 함수다: `IsPastGameInitialization()`(23.3절 이동 검사와 같은 조건 — `CheckMovable`도 이것을 쓴다), `CheckDynamicDataChange(property, blockStationary)`, `CheckChangeBeforeGameInitialization(property)`. D2 이후 컴포넌트도 쓴다.
  - UE는 등록된 Static 광원을 편집기 World에서도 setter로 바꾸지 못한다(편집기 속성 창은 setter를 거치지 않는다). Rocks는 CU5 전까지 편집도 setter로 하므로 편집 World에서는 막지 않는다.
- 값이 실제로 바뀌면 Render 시스템 상태 다시 보내기를 표시한다(24.1절).
- `PointLightComponent::SetUseInverseSquaredFalloff(false)`는 단위를 `Unitless`로 되돌린다. 1/r²가 꺼진 동안 `SetIntensityUnits`는 Unitless 밖을 거부한다(UE는 로드·편집에서 Unitless로 되돌린다 — `PointLightComponent.cpp:230`·`270`). Point의 `IntensityUnits()`는 1/r²가 꺼져 있으면 저장 값과 무관하게 Unitless를 돌려준다. 그래서 저장 파일의 어긋난 조합(1/r² 꺼짐 + Candelas)도 UE 로드 결과와 같은 뜻으로 읽힌다.
- 색: `SetLightColor(Color)`는 sRGB byte를 그대로 둔다. `SetLightColor(LinearColor, bool srgb = true)`는 UE처럼 참이면 sRGB 인코딩(`ToSRGB`), 거짓이면 인코딩 없이 byte로 양자화(`QuantizeRound`)한다(`LightComponent.cpp:1130`). 비유한 성분은 거부한다. `LinearLightColor()`는 sRGB 해독 값이다(UE `GetLightColor`).

#### 24.2.4 계산

모두 렌더 없이 정해지는 순수 계산이다. UE 식과 같고 UE 값으로 시험한다.

- `LightComponentBase::IntensityUnits()`(virtual, 기본 Unitless — 국소 광원이 필드를 돌려준다)와 `LightComponent::ComputeLightBrightness()`(virtual, 기본 `intensity`). IES 프로파일은 아직 없다(24.2.8절).
- `PointLightComponent::ComputeLightBrightness`(`PointLightComponent.cpp:127`): 1/r²가 켜져 있으면 단위별로 곱한다. Candelas ×1e4, Nits ×4π·R·(R + 0.5L), Lumens ×1e4/(4π), EV는 2^EV × 1e4(대입), Unitless ×16. 꺼져 있으면 `intensity` 그대로다.
- `SpotLightComponent::ComputeLightBrightness`: Point와 같고 Lumens만 ×1e4/(2π(1 − cos 반각))이다(`SpotLightComponent.cpp:79`).
- `RectLightComponent::ComputeLightBrightness`(`RectLightComponent.cpp:181`): Candelas ×1e4, Nits ×W·H, Lumens ×1e4/π, EV 2^EV × 1e4, Unitless ×16. UE `bLightRequiresBrokenEVMath`(옛 콘텐츠 호환)는 들이지 않는다.
- `LocalLightComponent::UnitsConversionFactor(from, to, cosHalfConeAngle = -1)`(static, `LocalLightComponent.cpp:146`): cos를 [-1, 1 − 1e-4]로 자른 뒤 원 단위 계수 × 대상 단위 역계수다. 원 단위 계수: Candelas 1e4, Lumens 1e4/(2π(1 − cos)), EV 1e4, Nits 1, Unitless 16. 같은 단위면 1이다.
- `SpotLightComponent::ClampedConeAngles()`(`SpotLightComponent.cpp:35`): 라디안. inner = clamp(inner, 0, 88.9°), outer = clamp(outer, inner + 0.001 rad, 88.9° + 0.001 rad). `HalfConeAngle()`은 clamp한 outer, `CosHalfConeAngle()`은 그 cos다. setter는 UE 실행 중 setter처럼 값만 저장하고 clamp는 이 계산이 한다.
- `LocalLightComponent::BoundingSphere()`: 월드 위치 중심, 반지름 `attenuationRadius`(UE `GetBoundingSphere` — `LocalLightComponent.cpp:78`). `AffectsBounds(ComponentBounds)`: 중심 거리 ≤ 반지름 + bounds 구 반지름(UE `AffectsBounds` — `:42`). 둘 다 virtual이다: Spot의 원뿔 경계 구(UE `ComputeBoundingSphereForCone`)·원뿔 판정은 렌더 culling과 함께 재정의로 넣는다(24.2.8절).
- `LightComponent::Direction()`: 월드 회전의 +X 축(UE `GetDirection` — `LightComponent.cpp:1450`).
- `LightComponentBase::HasStaticLighting()` = Static이고 소유 Actor가 있다. `HasStaticShadowing()` = Movable이 아니고 소유 Actor가 있다(UE `:293`·`:298`).
- `SkyLightComponent::SetCubemapResolution(n)`: 1 이상이어야 하고(아니면 `InvalidArgument`), 2의 거듭제곱으로 올린 뒤 [8, 16384]로 자른다(UE `SanitizeCubemapSize` — `SkyLightComponent.cpp:371`). 상한 16384는 UE `GetMaxCubeTextureDimension`(RHI 최대 cube 크기)의 D3D12 값이다. RHI 질의가 World에 없으므로 이름 있는 상수로 둔다.

#### 24.2.5 Actor

- `Light`(Abstract): root가 광원 컴포넌트다. `LightComponentHandle()`(root handle), `SetLightEnabled(bool)`·`IsLightEnabled()`(root 컴포넌트의 visible — UE `ALight::SetEnabled`·`bEnabled`가 컴포넌트 visibility로 간다, `E/Private/Light.cpp:88–120`). 복제(`bEnabled` replicated)는 네트워크 단계다.
- 파생은 프레임워크 PreAwake(17.8절)에서 root를 만든다. `PlayerStart`와 같은 자리다(Spawn Transform이 root에 붙는다 — 17.3절). 엔진 Actor 클래스도 이 hook을 쓸 수 있게 Actor 주석을 고친다.
  - root가 이미 그 광원 타입(파생 포함)이면(레벨 로드로 영속 컴포넌트가 먼저 등록된 경우) 그대로 쓰고 다시 설정하지 않는다. 다른 타입 root가 있으면 Spawn이 `InvalidArgument`로 실패한다(옛 root를 떼어 둔 채 바꾸지 않는다 — UE 광원 Actor는 root를 생성자에서 정하므로 이 상태가 없다). root가 없으면 만들어 root로 둔다.
  - 만든 컴포넌트의 Mobility는 **Stationary**다(UE 각 Actor 생성자 — `Light.cpp:190`·`244`, `SpotLight.cpp:27`, `RectLight.cpp:12`).
  - `SpotLight`는 root 상대 회전 pitch −90°(아래), `DirectionalLight`는 pitch −46°다(UE `SpotLight.cpp:28`, `Light.cpp:245`). Spawn Transform은 이 값에 곱해진다(UE `RootRelative × UserSpawnTransform` — Rocks 17.3절과 같다).
  - UE `ADirectionalLight`의 scale 2.5(편집기 아이콘 크기)와 화살표 컴포넌트(편집기 전용)는 들이지 않는다(24.2.8절).
- `SkyLight : Light`: root `SkyLightComponent`(Stationary). 같은 enabled API를 가진다.

#### 24.2.6 결정

| ID | 항목 | 결정 | 근거 |
|---|---|---|---|
| ERD34 | 데이터 먼저 | 렌더·물리 없이 정해지는 데이터·순수 계산을 먼저 넣고, 시스템 상태는 요청하지 않는다 | 사용자 결정 2026-11-24 |
| ERD35 | 광원 수치 형식 | UE와 같은 `float` | 렌더 입력 정밀도, UE 값 대조 |
| ERD36 | Static 광원 setter | 게임 World 초기화 뒤 `InvalidArgument` + `StaticComponentDataChanged` | 23.3절 이동 검사와 같은 규칙, UE는 조용히 무시 |
| ERD37 | 광원 Actor root | 프레임워크 PreAwake에서 만들고 같은 타입이 이미 있으면 재사용, 다른 타입이면 거부 | Spawn Transform 적용 순서, 레벨 로드 |
| ERD38 | 이름 | UE 이름에서 접두만 뗀 업계 공용어 | Rocks 이름 규칙의 공용어 예외 |

#### 24.2.7 시험

`World/Tests/WorldLightComponentTests.cpp`(8개 시험 — `Light_` 접두):
1. 클래스마다 기본값이 24.2.2절 표와 같다. Actor가 만든 컴포넌트는 Stationary이고 Spot·Directional root 회전이 UE 값이다.
2. setter의 값 검증: 비유한·범위 밖 거부와 값 유지, intensity 음수는 EV에서만 받고 단위가 바뀌면 0, 1/r² 끄기 → Unitless 강제와 단위 거부, 방향광만 그림자 편향 [0, 10].
3. Mobility(Game·Tryout·Editor World): 게임 World 초기화 뒤 Static 광원 세기 변경 거부, Stationary는 세기는 되고 반경·원뿔 각은 거부, 진단 수(`StaticComponentDataChanged`·`ComponentDataLockedInGame`), 편집 World는 막지 않음, OnAwake 안 설정은 됨, `affectsWorld`는 등록 뒤 게임 World에서 거부, 광원 채널은 막지 않음.
4. 계산: 단위 계수 표 전체(5×5, Spot 원뿔 cos 포함), Point·Spot·Rect 밝기(단위별, 1/r² 꺼짐), 원뿔 clamp(inner > outer, 0°, 89° 넘는 값), cubemap 해상도 정리(1·8·100·128·20000·0), 경계 구·AffectsBounds, Direction(회전한 root).
5. 색: sRGB 왕복, `SetLightColor(LinearColor, false)` 양자화.
6. 리플렉션: base·필드 수와 Abstract 표시, Level 그래프 저장 → 로드 값 보존(Spot·Directional·Sky — float·int32·enum·Color·LinearColor·구조체 필드).
7. Spawn Transform이 Directional 기본 회전에 곱해진다. 레벨 로드처럼 같은 타입 root가 먼저 있는 Actor는 컴포넌트를 더 만들지 않고, 다른 타입 root면 Spawn이 실패한다.

#### 24.2.8 들이지 않은 UE 속성(사유)

들인 속성은 24.2.2절 표다.


| UE | 사유 |
|---|---|
| `LightFunctionMaterial`·`LightFunctionScale`·`LightFunctionFadeDistance`·`DisabledBrightness` | 머티리얼 참조 필드가 World에 들어올 때(MS 단계 + 렌더 RS) |
| `IESTexture`·`bUseIESBrightness`·`IESBrightnessScale` | 텍스처 참조 필드와 IES 임포트가 생길 때 |
| Rect `SourceTexture`·`SourceTextureScale`·`SourceTextureOffset`·`LightFunctionConeAngle` | 같은 사유 |
| Sky `Cubemap` | TextureCube 참조가 생길 때(`sourceCubemapAngle`·`sourceType`은 지금 넣는다) |
| `CastRaytracedShadow`·`bAffectReflection`·`bAffectGlobalIllumination`·`SamplesPerPixel`·`RayStartOffsetDepthScale`·`bOverrideRayEndBias`·`RayEndBias` | 레이 트레이싱 경로가 계획에 들어올 때 |
| `bAllowMegaLights`·`MegaLightsShadowMethod` | UE MegaLights 대응 렌더 경로가 생길 때 |
| `bCastDeepShadow`·`DeepShadowLayerDistribution` | deep shadow(머리카락) 렌더 경로 |
| `bUseRayTracedDistanceFieldShadows`·`DistanceFieldShadowDistance`·`TraceDistance` | distance field 그림자 |
| `LightmassSettings`·`OriginalLightGuid`·`LightGuid`·`PreviewShadowMapChannel` | 굽는 조명(lightmap) 체계 |
| `bEnableLightShaftBloom`·`BloomScale`·`BloomThreshold`·`BloomMaxBrightness`·`BloomTint`·`bEnableLightShaftOcclusion`·`OcclusionMaskDarkness`·`OcclusionDepthRange`·`LightShaftOverrideDirection` | light shaft 후처리(RS6) |
| `ContactShadowCastingIntensity`·`ContactShadowNonCastingIntensity`·`bCastShadowsFromCinematicObjectsOnly`·`bForceCachedShadowsForMovablePrimitives`·`ViewLightingChannels` | 그림자 렌더 세부 — 그림자 구현(RS3) 때 |
| Directional `LightSourceSoftAngle` 외 그림자 세부(`ShadowSourceAngleFactor`·`ShadowCascadeBiasDistribution`·`bUseInsetShadowsForMovableObjects`·`bCastModulatedShadows`·`ModulatedShadowColor`) | 같은 사유 |
| Directional 구름·대기 세부(`bPerPixelAtmosphereTransmittance`·`bCastShadowsOnClouds`·`bCastShadowsOnAtmosphere`·`bCastCloudShadows`·`CloudShadow*`·`CloudScatteredLuminanceScale`) | 대기·볼륨 구름(D5·RS9) |
| Sky 거리장 AO(`OcclusionMaxDistance`·`Contrast`·`OcclusionExponent`·`MinOcclusion`·`OcclusionTint`·`OcclusionCombineMode`)·구름 AO(`bCloudAmbientOcclusion`·`CloudAmbientOcclusion*`) | distance field AO·볼륨 구름 |
| Rect `bLightRequiresBrokenEVMath` | 옛 콘텐츠 호환(하위호환 없음 원칙) |
| 편집기 아이콘(`StaticEditorTexture` 등)·`ADirectionalLight` scale 2.5·화살표 컴포넌트·`EditorApplyScale` | 편집기 표시(Studio) |
| Spot 원뿔 경계 구(`ComputeBoundingSphereForCone`)·원뿔 `AffectsBounds` | 렌더 culling(RS1)과 함께 |
| 색온도 색(`GetColorTemperature`·`GetColoredLightBrightness`) | 작업 색 공간(color management)이 생길 때 — 필드는 지금 넣는다 |
| `ALight` `bEnabled` 복제 | 네트워크 단계(CR24) |
