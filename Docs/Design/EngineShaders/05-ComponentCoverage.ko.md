# 엔진 셰이더 — 5. 렌더링 컴포넌트 → 셰이더 표

[목차](README.ko.md)

## 5. 렌더링 컴포넌트 → 셰이더 표

[컴포넌트 도입](../ComponentCatalog/README.ko.md)(CC, 개정 7)의 필수 컴포넌트 가운데 화면에 무언가를 그리는 것([1부 3절](../ComponentCatalog/03-RequiredComponents.ko.md), [2부 8.4절](../ComponentCatalog/08-OutsideFolder.ko.md), [3부 9절](../ComponentCatalog/09-Plugins.ko.md))을 모두 적고, 그것이 화면에 나오는 데 필요한 셰이더 영역·Rocks 파일·단계를 맞췄다. "기본" 열이 **아니오**인 행은 필요한 셰이더가 기본 영역(A01–A23)에 없는 것이다.

- 단계는 [CC 5절](../ComponentCatalog/05-BundleOrder.ko.md) 순서와 맞춘다: CM0(카메라)은 화면 마일스톤(SL1), CM1a는 RN RS0(SL2), CM1b는 RS1·RS3(SL3·SL5), 그 뒤는 각 RS 단계와 함께다. 컴포넌트를 셰이더보다 먼저 만들지 않는다([CC 5절](../ComponentCatalog/05-BundleOrder.ko.md) "동작하는 척 금지"). 묶음 단계와 셰이더 단계가 다른 곳은 5.3절에 모았다.
- 모든 메시 계열은 공통으로 A05 `Material/MaterialCommon.rsh`·`VertexInput/*`, A06 `Scene/ViewConstants.rsh`·`GpuSceneRead.rsh`, A07 `BasePass/MeshPassCommon.rsh`·`DepthPasses.rsf`·`ForwardOpaquePass.rsf`(Deferred면 `GBufferPass.rsf`), A17 `TonemapPass.rsf`를 쓴다. 표에는 그 위에 더 필요한 것만 적는다.

### 5.1 기본 영역으로 덮이는 컴포넌트

| 컴포넌트(CC 묶음) | 추가로 필요한 셰이더(영역 · Rocks 파일) | 단계 | 기본 |
|---|---|---|---|
| `CameraComponent`(CM0) | A06 `Scene/ViewConstants.rsh`·`DepthConversion.rsh`, A17 `ExposureCommon.rsh`(수동·물리 카메라 노출)·`TonemapPass.rsf`(최소), A03 `DisplayEncoding.rsh`, A18 `FullscreenTriangle.rsf` | SL1(화면 마일스톤 텍스처 표시 view)·SL2 | 예 |
| `PrimitiveComponent` 렌더 플래그(CM1a) | 그림자 caster: A10 `ShadowDepthPass.rsf`. custom depth·stencil: A07 `DepthPasses.rsf` `CustomDepth`(SL8, RN 단계는 SLR11로 요구 — 5.3절). 조명 채널: A08 `LightRecord.rsh` 채널 mask × A06 인스턴스 플래그. 거리 컬링: RS0은 CPU 가시성 V1이 하므로 셰이더가 없고, GPU 경로는 SL6 A06 `InstanceCulling.rsf` | SL2·SL3·SL5·SL6·SL8 | 예 |
| `StaticMeshComponent`(CM1a) | A05 `VertexInput/LocalMeshVertexInput.rsh`(정적), 공통 메시 pass | SL2 | 예 |
| `MeshComponent`(섹션 머티리얼·동적 인스턴스, CM1a) | 셰이더 추가 없음(머티리얼 상수 블록·텍스처 handle만 바뀐다 — RN 14.5절) | SL2 | 예 |
| `CameraProxyMeshComponent`(CM2, EditorOnly) | 정적 메시와 같다 | SL2 | 예 |
| `DirectionalLightComponent`(CM1b) | A08 `LightRecord.rsh`, A09 `ForwardLighting.rsh`·`DeferredLightingPass.rsf`, A04 `DirectLighting.rsh`, A10 `CascadeSelection.rsh`·`ShadowFiltering.rsh`·`ShadowDepthPass.rsf`(CSM), 대기 광원이면 A16 대기 투과율 | SL3·SL5·SL7 | 예 |
| `PointLightComponent`·`SpotLightComponent`(CM1b) | A08 `LightAttenuation.rsh`·`BuildLightGrid.rsf`·`LightGridLookup.rsh`, A04 `SphereLightIntegration.rsh`(source radius·length), A10 `ShadowAtlas.rsh`(point 6면·spot) | SL3·SL5 | 예 |
| `LightComponent` IES·광원 함수 속성 | A08 `IesProfile.rsh`·`BuildIesAtlas.rsf`·`LightFunctionAtlas.rsf`(머티리얼 영역 `LightFunction`) | SL10 | 예 |
| `RectLightComponent`(CM8) | A04 `RectLightIntegration.rsh`(LTC), A08 `RectLightTextureAtlas.rsf`, A10 spot과 같은 그림자 | SL10 | 예 |
| `SkyLightComponent`(CM1b 상수·CM9 capture) | A11 `SkyLighting.rsh`(SH 상수 — SL3), `FilterEnvironmentCube.rsf`(capture 사전 필터·SH 투영 — SL9), A02 `SphericalHarmonics.rsh`. 대기 실시간 capture는 A16 + A11 | SL3·SL9 | 예 |
| `SkyAtmosphereComponent`(CM8) | A16 `BuildAtmosphereLuts.rsf`·`SkyAtmospherePass.rsf`·`AtmosphereModel.rsh`, 투명은 A15가 aerial perspective 적용 | SL7 | 예 |
| `HeightFogComponent`(CM8) | A16 `HeightFog.rsh`·`HeightFogPass.rsf`, 투명은 A15 자기 셰이더 안 적용 | SL7 | 예(볼륨 안개 선택은 5.2절) |
| `ReflectionCaptureComponent`·`Box…`·`Sphere…`·`Plane…`(CM9) | A11 `ReflectionCaptures.rsh`(상자·구·평면 영향·parallax), `FilterEnvironmentCube.rsf`, `IndirectComposite.rsf`, capture 그리기는 cube 면 6개 view로 전체 렌더러 + `SceneCaptureOutput.rsf` | SL9 | 예 |
| `SceneCaptureComponent2D`·`…Cube`(CM9) | 추가 view로 전체 렌더러, A11 `SceneCaptureOutput.rsf`(출력 종류 변환), A18 `CopyTexture.rsf`·`GenerateMips.rsf` | SL9 | 예 |
| `PlanarReflectionComponent`(CM9) | A11 `PlanarReflection.rsf`·`.rsh`(반사 평면 view 결과 흐림·표본) | SL10 | 예 |
| `DecalComponent`(CM8) | A13 전체(`DeferredDecalPass.rsf`·`DBufferDecalPass.rsf`·`DBufferRead.rsh`), 머티리얼 영역 `DeferredDecal`(MS MS7) | SL9 | 예 |
| `PostProcessComponent`(CM8) | A17 전체(노출·bloom·LUT·tone map·FXAA·TAA·upscale·grain), A03 `ColorGrading.rsh`, AO 설정 A12, SSR 설정 A11, 후처리 머티리얼 `PostProcessMaterialPass.rsf` | SL8·SL9·SL10 | 예(DOF 등 일부는 5.2절) |
| `InstancedStaticMeshComponent`(CM6) | A05 `LocalMeshVertexInput.rsh` 인스턴스 경로, A06 `InstanceCulling.rsf`·`HzbOcclusion.rsh`·`LodDitherTransition.rsf`, A21 `WorkDistribution.rsh`·scan | SL6 | 예 |
| `SplineMeshComponent`(CM6) | A05 `VertexInput/SplineMeshDeform.rsh` | SL6 | 예 |
| `FoliageInstancedStaticMeshComponent`·`GrassInstancedStaticMeshComponent`·`InteractiveFoliageComponent`(CM18 — 앞 둘은 인스턴스, `InteractiveFoliage`는 정적 메시 + 접촉 반응 머티리얼 WPO)·`HlodInstancedStaticMeshComponent`(CM19)·`PcgProceduralIsmComponent`(CM27)·`MassVisualizationComponent`(CM30) | 인스턴스드 메시 경로(위 행). 바람은 머티리얼 WPO | SL6 | 예(지형·풀 배치 계산은 5.2절) |
| `SkinnedMeshComponent`·`SkeletalMeshComponent`·`PoseableMeshComponent`(CM7)·`BudgetedSkeletalMeshComponent`(CM22) | A22 전체(`BoneTransforms.rsh`·`SkinnedMeshVertexInput.rsh`·`SkinningCache.rsf`·`ApplyMorphTargets.rsf`·`RecomputeTangents.rsf`) | SL10 | 예 |
| `TextRenderComponent`(CM5) | 동적 메시(A05 `LocalMeshVertexInput.rsh`, 인스턴스 없음) + 글자 머티리얼이 A19 `Ui/GlyphSampling.rsh` 표본 함수를 쓴다(머티리얼 노드로 노출) | SL2 + SLU | 예 |
| `MaterialBillboardComponent`(CM8) | 동적 메시(카메라 정면 사각형은 CPU가 만든다 — UE와 같다) + 머티리얼 | SL2 | 예 |
| `BillboardComponent`(CM2, EditorOnly) | A20 `IconSprites.rsf` | SL10 | 예 |
| `ArrowComponent`·`DrawFrustumComponent`·`LineBatchComponent`·`DebugDrawComponent`(CM2)·`DrawSphereComponent`(CM3), 충돌 형상 편집 표시 `BoxComponent`·`SphereComponent`·`CapsuleComponent`·`CylinderComponent`(CM3), 곡선 편집 표시 `SplineComponent`(CM5)·`ZoneShapeComponent`(CM30) | A20 `DebugLines.rsf`(깊이 그룹 World·Foreground)·`EditorComposite.rsf` | SL10 | 예 |
| 내비·AI·SmartObject·ZoneGraph·PCG 디버그 표시(CM16·CM26·CM27·CM30) | A20 `DebugLines.rsf`, 반투명 면은 A15 `TranslucentPass.rsf`(동적 메시) | SL10 | 예 |
| `WidgetComponent`(CM17) | A19 UI 전체로 render target에 그림 → world 사각형 머티리얼(동적 메시, 불투명 또는 A15 투명) | SLU + SL2·SL7 | 예 |
| `DynamicMeshComponent`(CM20)·`ProceduralMeshComponent`(CM20)·`CableComponent`(CM28) | 동적 메시(A05 `LocalMeshVertexInput.rsh`, 프레임 업로드 정점) | SL2 | 예 |
| `CinematicCameraComponent`(CM15) | 노출·필름백은 A17 `ExposureCommon.rsh` | SL8 | 예(DOF는 5.2절) |

### 5.2 기본 영역에 없는 것

| 컴포넌트(CC 묶음) | 빠진 셰이더 | 속한 후속 영역 | 권장 |
|---|---|---|---|
| `LocalFogVolumeComponent`·`VolumetricCloudComponent`·`HeterogeneousVolumeComponent`(CM11) | 볼륨 안개·구름·이종 볼륨 전체 | A25 | 컴포넌트 묶음 CM11이 이미 "볼륨 렌더 스펙(문서 없음)"을 선행으로 둔다. 그 스펙과 함께 A25를 들인다. 기본 영역 완료(SL10) 뒤 첫 후속 후보 |
| `HeightFogComponent`의 볼륨 안개 선택 | froxel 볼륨 안개 | A25 | 위와 같다. 그 전에는 설정을 `Unsupported`로 진단 |
| `PostProcessComponent`의 DOF·움직임 흐림·지역 노출·lens flare·FFT bloom 설정, `CinematicCameraComponent` 초점·조리개 DOF(CM15) | cinematic DOF·움직임 흐림·지역 노출·렌즈 효과 | A24 | EIP 26.4절 M5/M6 순서. CM15 선행에 "RN RS6(DOF 연결점)"이 있으므로 A24 DOF는 CM15와 함께 |
| `InstancedSkinnedMeshComponent`(CM11)·`HlodInstancedSkinnedMeshComponent`(CM19) | 애니메이션 bank GPU 평가(A22의 후속 부분) | A22 후속 부분 | CM11 착수 때 A22에 더한다 |
| `VirtualTextureVolumeComponent`(CM10) | 런타임 가상 텍스처 전체 | A33 | 가상 텍스처 렌더 기능 문서(없음)와 함께 |
| `LandscapeComponent`·`LandscapeSplinesComponent`·`ControlPointMeshComponent`·`LandscapeMeshProxyComponent`(CM18·CM19), 풀 자동 배치 무게 | 지형 정점 입력·컬링·편집 layer, 풀 무게 | A34 | 지형 문서(CR35)와 함께. `ControlPointMeshComponent`는 정적 메시 경로라 지형 없이도 그려지지만 묶음이 CM18이다 |
| `EffectComponent`(CM21, Niagara 구조) | 입자 sprite·ribbon·mesh 정점 입력, GPU 시뮬레이션, 입자 정렬(정렬 자체는 A21 기본에 있다) | 플러그인(Niagara, 1.5절) | 사용자 결정으로 후속. VFX 스펙이 A21 정렬·A05 정점 입력 인터페이스·A07 메시 pass를 재사용하게 한다 |
| `MediaComponent`(CM17) | 미디어 YUV → RGB 변환, Android 외부 텍스처 | A35, A18 후속 부분 | 미디어 문서(CR33)와 함께. 변환 행렬은 A03에 더한다 |
| `GeometryCacheComponent`(CM28) | geometry cache 정점 입력(프레임 보간) | A34 | CR50과 함께 |
| `DirectionalLightComponent`의 light shaft(가림·bloom) 설정 | 광선 축 post(`LightShaftShader`) | A25 | 볼륨 렌더링과 함께 들인다. 그 전에는 설정을 켜면 `Unsupported`로 진단하고 무시하지 않는다 |
| `DirectionalLightComponent`·local 광원의 거리장 그림자 설정, `SkinnedMeshComponent` 계열의 capsule 그림자(직접·간접) | 거리장 그림자(`DistanceFieldShadowing`), capsule 그림자(`CapsuleShadowShaders`) | A27 | 거리장·DynamicGI 스펙과 함께(RN 9.2절 "거리장·RT 그림자 후속"). 그 전에는 `Unsupported` 진단 |
| `WaterBodyComponent` 계열·`WaterMeshComponent`(CM31) | 물 플러그인 셰이더(표면 quadtree·물 정보 텍스처). 물 셰이딩 자체는 A23 `Water/*`(SL10)에 있다 | 플러그인(Water) + A34 | CR53과 함께. A23은 기본 영역에서 먼저 준비된다 |

`ForceFeedbackComponent`·`AudioComponent`·물리·이동·AI 컴포넌트는 그리지 않으므로 셰이더가 없다. `WindSourceComponent`는 SpeedTree 바람(A34)에만 쓰이고 셰이더 쪽 대응은 식생 문서가 정한다. `LightBakePortalComponent`·`BoundsCopyComponent`는 베이커·편집기 전용이라 런타임 셰이더가 없다.

### 5.3 묶음 단계와 셰이더 단계가 다른 곳

| 컴포넌트·속성 | CC 묶음(RN 단계) | 셰이더 단계 | 차이의 처리 |
|---|---|---|---|
| `PrimitiveComponent` custom depth·stencil 플래그 | CM1a(RS0) | SL8(`CustomDepth` pass) | RN 17.2절에 custom depth·stencil pass 단계가 없다. RN에 단계를 요구한다(SLR11, 권장: RS6 — custom depth를 읽는 후처리 머티리얼·외곽선과 같은 단계). CM1a에서는 플래그만 저장·전달하고, pass가 생기기 전에 그 플래그를 읽는 소비자가 없으므로 화면 차이가 없다 |
| `PrimitiveComponent` 거리 컬링 | CM1a(RS0) | 셰이더 없음(RS0 CPU V1) → SL6(GPU 컬링) | RS0은 CPU 가시성 V1로 충분하다. GPU 경로는 RS4에서 같은 판정을 셰이더로 옮긴다 |
| 디버그 표시 계열(`Arrow`·`Billboard`·`LineBatch`·충돌 형상 표시 등) | CM2·CM3(CR23 → RN RS13) | SL10e | RS13 선행이 RS0·RS6뿐이라 SL10e는 SL8 뒤 바로 할 수 있다(6장) |
| `PlanarReflectionComponent` | CM9(선행 RN RS10) | SL10c | CC가 이미 RS10을 선행으로 둔다. 차이 없음 |
| `TextRenderComponent` | CM5 | SL2 + SLU(`GlyphSampling.rsh`) | 글리프 표본 함수가 UI 단계(SLU)에 있으므로 CM5는 SLU 뒤다 |

