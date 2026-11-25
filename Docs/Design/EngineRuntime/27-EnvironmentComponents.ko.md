# 엔진 런타임 — 27. 환경 컴포넌트 데이터(D5)

[목차](README.ko.md)

## 27. 환경 컴포넌트 데이터(D5)

설계일 2026-11-25. [24장](24-ComponentData.ko.md) 데이터 먼저 묶음의 마지막 D5다. 대기·높이 안개·decal·후처리·반사 캡처의 **데이터**(UE 5.8 속성·기본값·단위·Mobility)와 렌더 없이 정해지는 계산을 넣는다. 그리기는 렌더 단계(RS6·RS9 등)다. 모듈은 World이며 에셋을 참조하지 않는다(3절): 머티리얼·cubemap·텍스처 참조 필드는 그 참조가 World 위 모듈(`WorldIntegration`)에 생길 때 넣는다(27.7절). UE 경로 약어: `C` = `Engine/Source/Runtime/Engine/Private/Components/`.

### 27.1 클래스

| Rocks(World) | UE | 부모 | Mobility 기본 |
|---|---|---|---|
| `SkyAtmosphereComponent` | `USkyAtmosphereComponent` | `SceneComponent` | Movable |
| `HeightFogComponent` | `UExponentialHeightFogComponent`(이름 — [컴포넌트 도입](../ComponentCatalog/06-Decisions.ko.md) CD6) | `SceneComponent` | Movable |
| `DecalComponent` | `UDecalComponent` | `SceneComponent` | Movable |
| `PostProcessComponent` | `UPostProcessComponent` | `SceneComponent` | Movable |
| `ReflectionCaptureComponent`(Abstract) | `UReflectionCaptureComponent` | `SceneComponent` | **Static**(UE `C/ReflectionCaptureComponent.cpp:773`) |
| `SphereReflectionCaptureComponent`·`BoxReflectionCaptureComponent`·`PlaneReflectionCaptureComponent` | UE 같은 이름 | `ReflectionCaptureComponent` | Static |
| Actor `SkyAtmosphere`·`HeightFog`·`DecalActor` | `ASkyAtmosphere`·`AExponentialHeightFog`·`ADecalActor` | `Actor` | 컴포넌트 기본 |
| Actor `ReflectionCapture`(Abstract)·`SphereReflectionCapture`·`BoxReflectionCapture`·`PlaneReflectionCapture` | UE 같은 이름 | `Actor` | Static |

- 필드가 많은 대기·안개·후처리는 설정 구조체 하나(`SkyAtmosphereSettings`·`HeightFogSettings`·`PostProcessSettings` — 리플렉션 구조체, 공개 필드)를 영속 필드로 갖고 `Settings()`·`SetSettings()`로 바꾼다. `SetSettings`는 범위를 검증하고 하나라도 벗어나면 `InvalidArgument`로 전체를 거부한다(값 유지). Decal·반사 캡처는 필드가 적어 필드별 setter다(D1–D4와 같다).
- 값이 바뀌면 Render 시스템 상태 다시 보내기를 표시한다(24.1절).
- Actor는 광원 Actor와 같은 규칙으로 PreAwake에서 root를 만든다(24.2.5절 — 같은 타입 root는 재사용, 다른 타입은 `InvalidArgument`). 켜고 끄기는 root visible이다(UE replicated `bEnabled` — 복제는 네트워크 단계). Actor 이름은 UE 접두만 뗐다(decal은 컴포넌트와 구별하려고 `DecalActor`).

### 27.2 SkyAtmosphereComponent

UE `C/SkyAtmosphereComponent.cpp:91–139`. 거리는 **km**, 산란·흡수 계수는 **1/km**다(UE 주석 `:95` — 렌더러가 cm로 바꾼다, 1 km = 1e5 cm).

`SkyAtmosphereSettings`(필드 — 기본 — 범위):
- `transformMode`(`SkyAtmosphereTransformMode { PlanetTopAtAbsoluteWorldOrigin, PlanetTopAtComponentTransform, PlanetCenterAtComponentTransform }`) — 첫 값.
- `bottomRadius` 6360 [0.1, 10000], `atmosphereHeight` 60 [0.1, ∞), `groundAlbedo` `Color(170, 170, 170)`(sRGB).
- `multiScatteringFactor` 1 [0, 100], `traceSampleCountScale` 1 [0.25, ∞).
- Rayleigh: `rayleighScatteringScale` 0.0331 [0, ∞), `rayleighScattering` (0.005802, 0.013558, 0.0331)/0.0331(LinearColor), `rayleighExponentialDistribution` 8 [0.001, ∞).
- Mie: `mieScatteringScale` 0.003996, `mieScattering` 흰색, `mieAbsorptionScale` 0.000444, `mieAbsorption` 흰색, `mieAnisotropy` 0.8 [0, 0.999], `mieExponentialDistribution` 1.2 [0.001, ∞). scale은 [0, ∞).
- 흡수(오존): `otherAbsorptionScale` 0.001881, `otherAbsorption` (0.00065, 0.001881, 0.000085)/0.001881, `otherTentDistribution`(`SkyAtmosphereTentDistribution { tipAltitude 25 km, tipValue 1 [0, ∞), width 15 km [0, ∞) }`).
- 빛: `skyLuminanceFactor`·`skyAndAerialPerspectiveLuminanceFactor` 흰색, `aerialPerspectiveViewDistanceScale` 1 [0, ∞)(UE 철자 `AerialPespective…`를 고쳤다), `heightFogContribution` 1 [0, ∞), `transmittanceMinLightElevationAngle` −90 [−90, 90](도), `aerialPerspectiveStartDepth` 0.1 [0.001, ∞).
- `holdout` 거짓, `renderInMainPass` 참.

계산: `PlanetCenterKm()`(UE `SkyAtmosphereCommonData.cpp:166`): 첫 모드 (0, 0, −bottomRadius), 둘째 (0, 0, −bottomRadius) + 컴포넌트 월드 위치 × 1e-5, 셋째 컴포넌트 월드 위치 × 1e-5.

Mobility: UE setter는 `AreDynamicDataChangesAllowed()`를 본다(`:464`) — Rocks `SetSettings`도 게임 World 초기화 뒤 Static이면 거부한다(24.2.3절 규칙).

### 27.3 HeightFogComponent

UE `C/ExponentialHeightFogComponent.cpp:72–119`. 안개 높이는 컴포넌트 월드 Z다(UE `:135`). 거리는 cm다.

`HeightFogSettings`:
- `fogDensity` 0.02 [0, 10], `fogHeightFalloff` 0.2 [0, 2](UE `PostEditChange` clamp — `:244`), `secondFogData`(`HeightFogData { fogDensity 0, fogHeightFalloff 0.2, fogHeightOffset 0 }` — 같은 범위, 생성자가 밀도를 0으로).
- `fogInscatteringLuminance` 검정(LinearColor), `skyAtmosphereAmbientContributionColorScale` 흰색, `inscatteringColorCubemapAngle` 0 [0, 360], `inscatteringTextureTint` 흰색, `skyLightCaptureAffectsHeightFogStrength` 0 [0, ∞), `skyLightCaptureAffectsHeightFogRoughness` 0.15 [0, 1].
- `fullyDirectionalInscatteringColorDistance` 100000 [0, ∞), `nonDirectionalInscatteringColorDistance` 1000 [0, fully…], `directionalInscatteringExponent` 4, `directionalInscatteringStartDistance` 10000, `directionalInscatteringLuminance` 검정.
- `fogMaxOpacity` 1 [0, 1], `startDistance` 0, `endDistance` 0(끔), `fogCutoffDistance` 0(끔) — 셋 다 [0, ∞).
- 볼륨 안개: `enableVolumetricFog` 거짓, `volumetricFogScatteringDistribution` 0.2 [−0.9, 0.9], `volumetricFogAlbedo` `Color` 흰색, `volumetricFogEmissive` 검정, `volumetricFogExtinctionScale` 1, `volumetricFogDistance` 6000, `volumetricFogStartDistance` 0, `volumetricFogNearFadeInDistance` 0, `volumetricFogStaticLightingScatteringIntensity` 1, `overrideLightColorsWithFogInscatteringColors` 거짓. 거리·배율은 [0, ∞).
- `holdout` 거짓, `renderInMainPass`·`visibleInReflectionCaptures`·`visibleInRealTimeSkyCaptures` 참.

계산: `IsFogVisible()`(UE `:126`): (밀도 + 둘째 밀도) × 1000 > 1e-8이고 `fogMaxOpacity` > 1e-8. `FogHeight()` = 월드 Z, `SecondFogHeight()` = Z + `secondFogData.fogHeightOffset`.

Mobility: UE 안개 setter는 보지 않는다 — Rocks도 막지 않는다.

### 27.4 DecalComponent

UE `C/DecalComponent.cpp:233–512`. 필드: `sortOrder` 0, `fadeScreenSize` 0.01, `fadeStartDelay`·`fadeDuration`·`fadeInDuration`·`fadeInStartDelay` 0(초, 0 이상), `destroyOwnerAfterFade` 참, `decalSize` (128, 256, 256) cm(반 크기, 0 이상), `decalColor` 흰색(LinearColor).

- bounds(`TryLocalBounds`): 지역 (0, decalSize, |decalSize|)를 월드로(UE `:472` — `TransformedSphere` 규칙).
- 수명(**끝까지 동작**): `OnStart`(UE `BeginPlay`)가 `SetLifeSpan(fadeStartDelay + fadeDuration)`을 부른다. `SetLifeSpan(seconds)`: > 0이면 그 시간 뒤 컴포넌트를 파괴하고, `destroyOwnerAfterFade`이고 fade 시간이 있으면 소유 Actor도 파괴한다(UE `LifeSpanCallback` — `:285`). 0 이하면 취소다. UE는 World 타이머를 쓴다. Rocks는 World 타이머가 없어 이 컴포넌트의 OnTick(PrePhysics)이 남은 시간을 세고, 수명이 없으면 Tick을 끈다. `SetFadeOut(start, duration, destroyOwner)`(UE `:318` — cvar `r.Decal.FadeDurationScale`은 Rocks에 없어 1)·`SetFadeIn(start, duration)`.
- Mobility: UE setter는 보지 않는다.

`DecalActor`: root `DecalComponent`, 상대 회전 pitch −90°(아래 — UE `DecalActor.cpp:30`), `destroyOwnerAfterFade` 참.

### 27.5 PostProcessComponent

UE `C/PostProcessComponent.cpp:16–24`. 필드: `settings`(`PostProcessSettings`), `priority` 0, `blendRadius` 100 cm [0, ∞), `blendWeight` 1 [0, 1], `enabled` 참, `unbound` 참(UE 컴포넌트 기본 — 볼륨 Actor는 거짓).

`PostProcessSettings`(UE `FPostProcessSettings` — `Engine/Classes/Engine/Scene.h:711`, 기본 `Scene.cpp:398`)의 다음 묶음을 넣는다. 각 값은 `override…` 플래그(UE `bOverride_X`, 기본 거짓)와 짝이다.
- 흰색 균형: `temperatureType`(`TemperatureMethod { WhiteBalance, ColorTemperature }`), `whiteTemp` 6500, `whiteTint` 0.
- 색 보정 전역: `colorSaturation`·`colorContrast`·`colorGamma`·`colorGain` (1, 1, 1, 1), `colorOffset` 0 (Vec4d, w = 휘도).
- 톤 매핑: `tonemappingMethod`(`TonemappingMethod { Filmic, StandardAces }`), `filmSlope` 0.88, `filmToe` 0.55, `filmShoulder` 0.26, `filmBlackClip` 0, `filmWhiteClip` 0.04.
- 색 수차 `sceneFringeIntensity` 0. Bloom: `bloomMethod`(`BloomMethod { Standard, Convolution }`), `bloomIntensity` 0.675, `bloomThreshold` −1, `bloomSizeScale` 4.
- 노출: `autoExposureMethod`(`AutoExposureMethod { Histogram, Basic, Manual }`), `autoExposureBias` 1(UE cvar `r.DefaultFeature.AutoExposure.Bias` 기본), `autoExposureMinBrightness` 0.03·`autoExposureMaxBrightness` 8, `autoExposureSpeedUp` 3·`autoExposureSpeedDown` 1, `autoExposureLowPercent` 10·`autoExposureHighPercent` 90, `autoExposureApplyPhysicalCameraExposure` 참, `cameraShutterSpeed` 60, `cameraIso` 100.
- 렌즈 flare `lensFlareIntensity` 1, vignette `vignetteIntensity` 0.4, grain `filmGrainIntensity` 0, AO `ambientOcclusionIntensity` 0.5·`ambientOcclusionRadius` 200, DOF `depthOfFieldFocalDistance` 0(끔)·`depthOfFieldFstop` 4·`depthOfFieldSensorWidth` 24.576 mm, motion blur `motionBlurAmount` 0.5·`motionBlurMax` 5·`motionBlurTargetFps` 30·`motionBlurPerObjectSize` 0.
- 범위는 UE `ClampMin`·`ClampMax`(예: AO intensity [0, 1], bloom intensity ≥ 0, exposure percent [0, 100], motion blur amount [0, 1])다. 넘으면 `SetSettings`가 거부한다.

계산(**끝까지 동작**): `OverridePostProcessSettings(destination, source, weight)`(UE `FSceneView::OverridePostProcessSettings` — `SceneView.cpp:1622`): override 플래그가 켜진 값만, 연속 값은 `Lerp(dest, src, weight)`, 이산 값(enum·불)은 weight > 0이면 그대로 쓴다(UE `SET_PP`). weight는 [0, 1]로 자른다. 볼륨 모으기(카메라 위치·우선순위 정렬·bounded 볼륨의 거리 가중 — UE `World.cpp:10422–10744`)는 카메라·렌더(RS6)와 함께 넣는다. bounded 판정이 물리 shape 거리를 쓰기 때문이다.

Mobility: UE setter는 보지 않는다.

### 27.6 반사 캡처

UE `C/ReflectionCaptureComponent.cpp:768–1401`. 기반 필드: `reflectionSourceType`(`ReflectionSourceType { CapturedScene, SpecifiedCubemap }`), `sourceCubemapAngle` 0 [0, 360], `brightness` 1 [0, ∞), `captureOffset` 0(cm, 월드), `runtimeSkylightScale` (0.1, 0.1, 0.1), `runtimeCapture` 거짓, `maxViewDistance` 0 [0, ∞), `finiteFarPlane` 거짓. 생성자 Mobility Static(UE "실행 중 바꾸면 안 된다").

| 클래스 | 필드 | `InfluenceBoundingRadius()` |
|---|---|---|
| Sphere | `influenceRadius` 3000 cm [0, ∞) | influenceRadius |
| Box | `boxTransitionDistance` 100 cm [0, ∞) | \|월드 scale\|(상자 크기가 scale이다 — UE `:1364`) |
| Plane | `influenceRadiusScale` 2 [0, ∞) | \|(scale.y, scale.z)\| × scale(`:1398`) |

Actor: `SphereReflectionCapture`(root Sphere), `BoxReflectionCapture`(root 상대 scale (1000, 1000, 400)), `PlaneReflectionCapture`(root 상대 scale (1, 1000, 1000)) — UE `ReflectionCapture.cpp:282·335·387`. Mobility Static이라 게임 World에서는 놓은 자리에서 움직이지 않는다.

`PlanarReflectionComponent`는 SceneCapture 계열이라 넣지 않는다(24.1절 — SceneCapture 선행).

### 27.7 들이지 않은 UE 속성(사유)

| UE | 사유 |
|---|---|
| Fog `InscatteringColorCubemap`, Decal `DecalMaterial`, 반사 `Cubemap`·`PostProcessMaterial`, 후처리 `ColorGradingLUT`·`BloomDirtMask`·`BloomConvolutionTexture`·`AutoExposureMeterMask`·`VignetteTexture`·커브·`WeightedBlendables` | 에셋 참조 — World 위 모듈(`WorldIntegration`)에서 |
| 후처리의 나머지 묶음(색 보정 Shadows·Midtones·Highlights, Local exposure, Lumen·레이 트레이싱, Path tracing, Bloom convolution 세부, 렌즈·DOF 세부, Mobile 전용 등 — UE `bOverride_*` 206개 중 위 목록 밖) | 각 렌더 기능(RS6 후처리)과 함께. 들인 묶음은 Forward+·Deferred 1차 후처리 범위다 |
| `r.DefaultFeature.*` cvar로 정하는 기준값(LensFlare 기본 0 등) | CVar 체계에 렌더 기본 기능 cvar가 생길 때(RS6) |
| `APostProcessVolume`(brush 볼륨) | 볼륨 Actor(brush) 체계 |
| Fog FSSS(실험) | UE 실험 기능 |
| SkyAtmosphere `OverrideAtmosphericLight` | 광원 연결(RS9) |
| `ASkyAtmosphere`·`AExponentialHeightFog`의 편집기 화살표·sprite, Decal Actor 화살표 | 편집기 표시 |
| 반사 캡처 `MapBuildDataId`·미리보기 shape | 굽는 데이터·편집기 |
| `UPlanarReflectionComponent`·`APlanarReflection` | SceneCapture 계열 |

### 27.8 결정·시험

| ID | 항목 | 결정 | 근거 |
|---|---|---|---|
| ERD51 | 설정 구조체 | 대기·안개·후처리는 리플렉션 구조체 하나 + `SetSettings` 전체 검증 | 필드가 많다, UE도 구조체(`FPostProcessSettings`) |
| ERD52 | decal 수명 | 컴포넌트 OnTick이 남은 시간을 센다 | World 타이머 없음, 결정적 |
| ERD53 | 후처리 혼합 | 덮어쓰기 함수만 지금, 볼륨 모으기는 RS6 | bounded 판정이 물리 shape 거리 |

시험 `World/Tests/WorldEnvironmentComponentTests.cpp`(`Environment_` 접두): 기본값(UE 값) 전부, `SetSettings` 범위 거부·값 유지, 대기 Static 거부·안개 허용, 행성 중심 세 모드, 안개 보임·높이, decal bounds·수명(파괴 시점·소유 Actor 파괴·취소·fade in/out), 후처리 덮어쓰기(Lerp·SET·플래그 꺼짐·weight clamp), 반사 캡처 반경(Sphere·Box·Plane)·Static, Actor root(회전·scale)·켜고 끄기, 리플렉션·저장 왕복.
