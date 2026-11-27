# 머티리얼 시스템(MaterialSystem) 설계

## 19. 노드 목록 — 노드 표 2

렌더링 기능 입력·함수·속성·표면·추가 출력·플러그인·제외 노드다. 규칙·상태·키는 [19장](README.ko.md)이다. 이 표는 생성 원본 `MaterialNodeCatalog.json`(19.2.2절)과 같아야 한다.

### 19.6.11 깊이(2)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionSceneDepth` | `Material.SceneDepth` | 구현 | MS2c | `scene` |  |
| `UMaterialExpressionDepthFade` | `Material.DepthFade` | 구현 | MS2c | `scene` |  |

### 19.6.12 물(1)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionSceneDepthWithoutWater` | `Material.SceneDepthWithoutWater` | 구현 | MS2c | `water` |  |

### 19.6.13 데칼(5)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionDBufferTexture` | `Material.DecalBufferTexture` | 구현 | MS2c | `decal` |  |
| `UMaterialExpressionDecalColor` | `Material.DecalColor` | 구현 | MS2b | `decal` |  |
| `UMaterialExpressionDecalDerivative` | `Material.DecalDerivative` | 구현 | MS2b | `decal` |  |
| `UMaterialExpressionDecalLifetimeOpacity` | `Material.DecalLifetimeOpacity` | 구현 | MS2b | `decal` |  |
| `UMaterialExpressionDecalMipmapLevel` | `Material.DecalMipmapLevel` | 구현 | MS2b | `decal` |  |

### 19.6.14 파티클(14)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionDynamicParameter` | `Material.DynamicParameter` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleColor` | `Material.ParticleColor` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleDirection` | `Material.ParticleDirection` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleMacroUV` | `Material.ParticleMacroUv` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleMotionBlurFade` | `Material.ParticleMotionBlurFade` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticlePositionWS` | `Material.ParticlePosition` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleRadius` | `Material.ParticleRadius` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleRandom` | `Material.ParticleRandom` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleRelativeTime` | `Material.ParticleRelativeTime` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleSize` | `Material.ParticleSize` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleSpeed` | `Material.ParticleSpeed` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleSpriteRotation` | `Material.ParticleSpriteRotation` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionParticleSubUVProperties` | `Material.ParticleSubUvProperties` | 구현 | MS2b | `particle` |  |
| `UMaterialExpressionSphericalParticleOpacity` | `Material.SphericalParticleOpacity` | 구현 | MS2b | `particle` |  |

### 19.6.15 조명(3)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionAtmosphericLightVector` | `Material.AtmosphericLightVector` | 구현 | MS2b | `sky` |  |
| `UMaterialExpressionAtmosphericLightColor` | `Material.AtmosphericLightColor` | 구현 | MS2b | `sky` |  |
| `UMaterialExpressionMainDirectionalLight` | `Material.MainDirectionalLight` | 구현 | MS2b | `light` |  |

### 19.6.16 하늘·대기(8)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionSkyAtmosphereLightDirection` | `Material.SkyAtmosphereLightDirection` | 구현 | MS2b | `sky` |  |
| `UMaterialExpressionSkyAtmosphereLightIlluminance` | `Material.SkyAtmosphereLightIlluminance` | 구현 | MS2b | `sky` |  |
| `UMaterialExpressionSkyAtmosphereLightIlluminanceOnGround` | `Material.SkyAtmosphereLightIlluminanceOnGround` | 구현 | MS2b | `sky` |  |
| `UMaterialExpressionSkyAtmosphereLightDiskLuminance` | `Material.SkyAtmosphereLightDiskLuminance` | 구현 | MS2b | `sky` |  |
| `UMaterialExpressionSkyAtmosphereAerialPerspective` | `Material.SkyAtmosphereAerialPerspective` | 구현 | MS2b | `sky` |  |
| `UMaterialExpressionSkyAtmosphereDistantLightScatteredLuminance` | `Material.SkyAtmosphereDistantLightScatteredLuminance` | 구현 | MS2b | `sky` |  |
| `UMaterialExpressionSkyAtmosphereViewLuminance` | `Material.SkyAtmosphereViewLuminance` | 구현 | MS2b | `sky` |  |
| `UMaterialExpressionSkyLightEnvMapSample` | `Material.SkyLightEnvironmentSample` | 구현 | MS2b | `sky` |  |

### 19.6.17 후처리 값(4)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionEyeAdaptation` | `Material.EyeAdaptation` | 구현 | MS2b | `post` |  |
| `UMaterialExpressionEyeAdaptationInverse` | `Material.EyeAdaptationInverse` | 구현 | MS2b | `post` |  |
| `UMaterialExpressionDepthOfFieldFunction` | `Material.DepthOfFieldFunction` | 구현 | MS2b | `post` |  |
| `UMaterialExpressionPostVolumeUserFlagTest` | `Material.PostProcessVolumeFlagTest` | 구현 | MS2b | `post` |  |

### 19.6.18 distance field(3)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionDistanceToNearestSurface` | `Material.DistanceToNearestSurface` | 구현 | MS2b | `distancefield` |  |
| `UMaterialExpressionDistanceFieldGradient` | `Material.DistanceFieldGradient` | 구현 | MS2b | `distancefield` |  |
| `UMaterialExpressionDistanceFieldApproxAO` | `Material.DistanceFieldApproxAo` | 구현 | MS2b | `distancefield` |  |

### 19.6.19 노이즈·표본열(5)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionNoise` | `Material.Noise` | 구현 | MS2f | `속성 TODO: MPQ-NoiseTexture` | ALU 함수(GradientALU·ValueALU·Voronoi)는 구현. LUT 함수(SimplexTex·GradientTex·GradientTex3D)는 렌더러 noise 시스템 텍스처가 필요하다(6.13절 MS2f3). 원문은 MaterialNoise.rsh |
| `UMaterialExpressionVectorNoise` | `Material.VectorNoise` | 구현 | MS2f | — |  |
| `UMaterialExpressionScalarBlueNoise` | `Material.ScalarBlueNoise` | 구현 | MS2b | `view` | blue noise 텍스처는 view 공급 |
| `UMaterialExpressionSobol` | `Material.Sobol` | 구현 | MS2f | — |  |
| `UMaterialExpressionTemporalSobol` | `Material.TemporalSobol` | 구현 | MS2f | `view` |  |

### 19.6.20 함수·재사용(5)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionMaterialFunctionCall` | `Material.FunctionCall` | 구현 | MS2e | `src:Function` |  |
| `UMaterialExpressionFunctionInput` | `Material.FunctionInput` | 구현 | MS2e | — |  |
| `UMaterialExpressionFunctionOutput` | `Material.FunctionOutput` | 구현 | MS2e | — |  |
| `UMaterialExpressionMaterialLayerOutput` | `Material.LayerOutput` | 구현 | MS2e | — | 레이어 함수 출력(팔레트 제외) |
| `UMaterialExpressionMaterialSample` | `Material.MaterialSample` | 구현 | MS2e | `src:Material` | 다른 머티리얼 그래프의 출력 참조(UE Internal) |

### 19.6.21 머티리얼 속성(10)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionAggregate` | `Material.Aggregate` | 구현 | MS2e | `속성 TODO: MPQ-MaterialAggregate` | Kind=MaterialAttributes는 구현. Kind=UserDefined는 aggregate 에셋(UE UMaterialAggregate)과 IR aggregate 타입이 필요하다 |
| `UMaterialExpressionBlend` | `Material.Blend` | 구현 | MS2e | — | UE IR 번역기 전용(Blend.h:19). MaterialAttributes까지 받는 다형 노드(Blend.h:25–38)라 MS2e |
| `UMaterialExpressionMakeMaterialAttributes` | `Material.MakeAttributes` | 구현 | MS2e | — |  |
| `UMaterialExpressionBreakMaterialAttributes` | `Material.BreakAttributes` | 구현 | MS2e | — |  |
| `UMaterialExpressionGetMaterialAttributes` | `Material.GetAttributes` | 구현 | MS2e | — |  |
| `UMaterialExpressionSetMaterialAttributes` | `Material.SetAttributes` | 구현 | MS2e | — |  |
| `UMaterialExpressionBlendMaterialAttributes` | `Material.BlendAttributes` | 구현 | MS2e | — |  |
| `UMaterialExpressionMaterialAttributeLayers` | `Material.AttributeLayers` | 구현 | MS2e | `src:Function` |  |
| `UMaterialExpressionLayerStack` | `Material.LayerStack` | 구현 | MS2e | `src:Function` | UE Experimental |
| `UMaterialExpressionShadingModel` | `Material.ShadingModel` | 구현 | MS2d | — |  |

### 19.6.22 표면(Substrate 대응)(25)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionSubstrateSlabBSDF` | `Material.SurfaceSlab` | 구현 | MS2d | `속성 TODO: MPQ-SubsurfaceProfile·MPQ-SpecularProfile` |  |
| `UMaterialExpressionSubstrateSimpleClearCoatBSDF` | `Material.SurfaceSimpleClearCoat` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateUnlitBSDF` | `Material.SurfaceUnlit` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateHairBSDF` | `Material.SurfaceHair` | 구현 | MS2d | — | Renderer hair 경로(P3) |
| `UMaterialExpressionSubstrateEyeBSDF` | `Material.SurfaceEye` | 구현 | MS2d | `속성 TODO: MPQ-SubsurfaceProfile` |  |
| `UMaterialExpressionSubstrateSingleLayerWaterBSDF` | `Material.SurfaceSingleLayerWater` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateVolumetricFogCloudBSDF` | `Material.SurfaceVolumetricFogCloud` | 구현 | MS2d | — | Volume 영역 |
| `UMaterialExpressionSubstrateHorizontalMixing` | `Material.SurfaceHorizontalMix` | 구현 | MS2d | — | closure 하나로 접기(MD1) |
| `UMaterialExpressionSubstrateVerticalLayering` | `Material.SurfaceVerticalLayer` | 구현 | MS2d | — | closure 하나로 접기(MD1) |
| `UMaterialExpressionSubstrateAdd` | `Material.SurfaceAdd` | 구현 | MS2d | — | closure 하나로 접기(MD1) |
| `UMaterialExpressionSubstrateWeight` | `Material.SurfaceWeight` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateSelect` | `Material.SurfaceSelect` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateShadingModels` | `Material.SurfaceFromShadingModel` | 구현 | MS2d | `속성 TODO: MPQ-SubsurfaceProfile` | 고전 shading model 입력 → 표면(preset) |
| `UMaterialExpressionSubstrateConvertMaterialAttributes` | `Material.SurfaceFromAttributes` | 구현 | MS2e | `속성 TODO: MPQ-SubsurfaceProfile` |  |
| `UMaterialExpressionSubstrateLightFunction` | `Material.SurfaceLightFunction` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstratePostProcess` | `Material.SurfacePostProcess` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateUI` | `Material.SurfaceUserInterface` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateConvertToDecal` | `Material.SurfaceToDecal` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateTransmittanceToMFP` | `Material.TransmittanceToMeanFreePath` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateMetalnessToDiffuseAlbedoF0` | `Material.MetalnessToDiffuseAlbedoF0` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateHazinessToSecondaryRoughness` | `Material.HazinessToSecondaryRoughness` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateThinFilm` | `Material.ThinFilm` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubstrateToonBSDF` | `Material.SurfaceToon` | TODO | MS2d | `MPQ-ToonShading` | UE Experimental. Renderer shading model 목록(14.2절)에 Toon이 없고 Toon profile 에셋도 없다 |
| `UMaterialExpressionHairAttributes` | `Material.HairAttributes` | 구현 | MS2b | `hair` |  |
| `UMaterialExpressionHairColor` | `Material.HairColor` | 구현 | MS2f | — |  |

### 19.6.23 추가 출력(19)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionBentNormalCustomOutput` | `Material.BentNormalOutput` | 구현 | MS2d | — |  |
| `UMaterialExpressionClearCoatNormalCustomOutput` | `Material.ClearCoatNormalOutput` | 구현 | MS2d | — |  |
| `UMaterialExpressionTangentOutput` | `Material.TangentOutput` | 구현 | MS2d | — |  |
| `UMaterialExpressionThinTranslucentMaterialOutput` | `Material.ThinTranslucentOutput` | 구현 | MS2d | — | RS5 반투명 |
| `UMaterialExpressionAbsorptionMediumMaterialOutput` | `Material.AbsorptionMediumOutput` | 구현 | MS2d | — |  |
| `UMaterialExpressionSubsurfaceMediumMaterialOutput` | `Material.SubsurfaceMediumOutput` | 구현 | MS2d | — |  |
| `UMaterialExpressionSingleLayerWaterMaterialOutput` | `Material.SingleLayerWaterOutput` | 구현 | MS2d | `water` |  |
| `UMaterialExpressionVolumetricAdvancedMaterialOutput` | `Material.VolumetricAdvancedOutput` | 구현 | MS2d | `volume` |  |
| `UMaterialExpressionVolumetricAdvancedMaterialInput` | `Material.VolumetricAdvancedInput` | 구현 | MS2d | `volume` |  |
| `UMaterialExpressionCloudSampleAttribute` | `Material.CloudSampleAttribute` | 구현 | MS2d | `volume` |  |
| `UMaterialExpressionVolumetricCloudEmptySpaceSkippingInput` | `Material.VolumetricCloudEmptySpaceSkippingInput` | 구현 | MS2d | `volume` | UE Experimental |
| `UMaterialExpressionVolumetricCloudEmptySpaceSkippingOutput` | `Material.VolumetricCloudEmptySpaceSkippingOutput` | 구현 | MS2d | `volume` | UE Experimental |
| `UMaterialExpressionFirstPersonOutput` | `Material.FirstPersonOutput` | 구현 | MS2d | `firstperson` |  |
| `UMaterialExpressionMotionVectorWorldOffsetOutput` | `Material.MotionVectorWorldOffsetOutput` | 구현 | MS2d | — | UE Experimental |
| `UMaterialExpressionTemporalResponsivenessOutput` | `Material.TemporalResponsivenessOutput` | 구현 | MS2d | — | UE Experimental |
| `UMaterialExpressionRuntimeVirtualTextureOutput` | `Material.RuntimeVirtualTextureOutput` | 구현 | MS2d | `rvt` |  |
| `UMaterialExpressionNeuralNetworkInput` | `Material.NeuralNetworkInput` | 구현 | MS2d | `neural` | 신경망 profile은 머티리얼 속성(19.9절, MPQ-NeuralProfile) |
| `UMaterialExpressionNeuralNetworkOutput` | `Material.NeuralNetworkOutput` | 구현 | MS2d | `neural` | 같음 |
| `UMaterialExpressionMaterialCache` | `Material.MaterialCacheOutput` | TODO | MS2d | `MPQ-VirtualTexture` | UE 5.8 머티리얼 캐시(가상 텍스처 기반) |

### 19.6.24 가상 텍스처(3)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionRuntimeVirtualTextureSample` | `Material.RuntimeVirtualTextureSample` | TODO | MS2c | `MPQ-VirtualTexture` |  |
| `UMaterialExpressionRuntimeVirtualTextureSampleParameter` | `Material.RuntimeVirtualTextureSampleParameter` | TODO | MS2c | `MPQ-VirtualTexture` |  |
| `UMaterialExpressionRuntimeVirtualTextureCustomData` | `Material.RuntimeVirtualTextureCustomData` | 구현 | MS2b | `rvt` |  |

### 19.6.25 메시 페인트(2)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionMeshPaintTextureObject` | `Material.MeshPaintTextureObject` | TODO | MS2c | `MPQ-VirtualTexture` | UE는 가상 텍스처 page table을 묶는다(HLSLMaterialTranslator.cpp:2366–2370, MCT_TextureMeshPaint) |
| `UMaterialExpressionMeshPaintTextureCoordinateIndex` | `Material.MeshPaintTextureCoordinateIndex` | 구현 | MS2b | `meshpaint` |  |

### 19.6.26 물리장(6)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionSamplePhysicsVectorField` | `Material.SamplePhysicsVectorField` | 구현 | MS2b | `physicsfield` |  |
| `UMaterialExpressionSamplePhysicsScalarField` | `Material.SamplePhysicsScalarField` | 구현 | MS2b | `physicsfield` |  |
| `UMaterialExpressionSamplePhysicsIntegerField` | `Material.SamplePhysicsIntegerField` | 구현 | MS2b | `physicsfield` |  |
| `UMaterialExpressionEvalPhysicsVectorField` | `Material.EvaluatePhysicsVectorField` | 구현 | MS2b | `physicsfield` | field를 직접 평가한다(Sample은 bSampleField=true — MaterialExpressions.cpp:19205–19244) |
| `UMaterialExpressionEvalPhysicsScalarField` | `Material.EvaluatePhysicsScalarField` | 구현 | MS2b | `physicsfield` | field를 직접 평가한다(Sample은 bSampleField=true — MaterialExpressions.cpp:19205–19244) |
| `UMaterialExpressionEvalPhysicsIntegerField` | `Material.EvaluatePhysicsIntegerField` | 구현 | MS2b | `physicsfield` | field를 직접 평가한다(Sample은 bSampleField=true — MaterialExpressions.cpp:19205–19244) |

### 19.6.27 SpeedTree(1)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionSpeedTree` | `Material.SpeedTree` | 구현 | MS2b | `speedtree` | SpeedTree 바람·LOD 정점 함수. 공급은 SpeedTree 임포터·정점 입력 타입. 이름은 데이터 형식 이름이라 그대로 쓴다(19.5절) |

### 19.6.28 글꼴(3)

| UE 5.8 | Rocks 노드 | 상태 | 단계 | 공급·선행 | 비고 |
|---|---|---|---|---|---|
| `UMaterialExpressionFontSignedDistance` | `Material.FontSignedDistance` | 구현 | MS2b | `ui` | UserInterface 영역 전용 |
| `UMaterialExpressionFontSample` | `Material.FontSample` | TODO | MS2c | `MPQ-Font` | Font FT7 |
| `UMaterialExpressionFontSampleParameter` | `Material.FontSampleParameter` | TODO | MS2c | `MPQ-Font` | Font FT7 |

### 19.6.29 플러그인 노드(45)

접두는 가칭이다. 그 모듈이 생길 때 모듈 스펙이 Rocks 모듈 이름을 정하고 접두도 그 이름으로 한다(19.5절).

| UE 5.8 | Rocks 노드(가칭) | 등록 모듈 | 비고 |
|---|---|---|---|
| `UMaterialExpressionLandscapeGrassOutput` | `Landscape.GrassOutput` | Landscape 모듈 | UE도 Landscape 모듈 |
| `UMaterialExpressionLandscapeLayerBlend` | `Landscape.LayerBlend` | Landscape 모듈 | UE도 Landscape 모듈 |
| `UMaterialExpressionLandscapeLayerCoords` | `Landscape.LayerCoords` | Landscape 모듈 | UE도 Landscape 모듈 |
| `UMaterialExpressionLandscapeLayerSample` | `Landscape.LayerSample` | Landscape 모듈 | UE도 Landscape 모듈 |
| `UMaterialExpressionLandscapeLayerSwitch` | `Landscape.LayerSwitch` | Landscape 모듈 | UE도 Landscape 모듈 |
| `UMaterialExpressionLandscapeLayerWeight` | `Landscape.LayerWeight` | Landscape 모듈 | UE도 Landscape 모듈 |
| `UMaterialExpressionLandscapePhysicalMaterialOutput` | `Landscape.PhysicalMaterialOutput` | Landscape 모듈 | UE도 Landscape 모듈 |
| `UMaterialExpressionLandscapeVisibilityMask` | `Landscape.VisibilityMask` | Landscape 모듈 | UE도 Landscape 모듈 |
| `UMaterialExpressionMaterialXAppend3Vector` | `MaterialX.Append3Vector` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXAppend4Vector` | `MaterialX.Append4Vector` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXBurn` | `MaterialX.Burn` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXContrast` | `MaterialX.Contrast` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXDifference` | `MaterialX.Difference` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXDisjointOver` | `MaterialX.DisjointOver` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXDodge` | `MaterialX.Dodge` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXFractal` | `MaterialX.Fractal` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXIn` | `MaterialX.In` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXLuminance` | `MaterialX.Luminance` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXMask` | `MaterialX.Mask` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXMatte` | `MaterialX.Matte` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXMinus` | `MaterialX.Minus` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXMod` | `MaterialX.Mod` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXOut` | `MaterialX.Out` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXOver` | `MaterialX.Over` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXOverlay` | `MaterialX.Overlay` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXPlus` | `MaterialX.Plus` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXPremult` | `MaterialX.Premult` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXRamp4` | `MaterialX.Ramp4` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXRampLeftRight` | `MaterialX.RampLeftRight` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXRampTopBottom` | `MaterialX.RampTopBottom` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXRange` | `MaterialX.Range` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXRemap` | `MaterialX.Remap` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXScreen` | `MaterialX.Screen` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXSplitLeftRight` | `MaterialX.SplitLeftRight` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXSplitTopBottom` | `MaterialX.SplitTopBottom` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXUnpremult` | `MaterialX.Unpremult` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionMaterialXTextureSampleParameterBlur` | `MaterialX.TextureSampleParameterBlur` | MaterialX 임포터 | UE Interchange MaterialX 전용(Private) |
| `UMaterialExpressionSpriteTextureSampler` | `Sprite2D.SpriteTextureSampler` | 2D 스프라이트 플러그인 | UE Paper2D |
| `UMaterialExpressionPhysicalMaterialOutput` | `RenderTrace.PhysicalMaterialOutput` | RenderTrace 플러그인 | UE Runtime 플러그인 |
| `UMaterialExpressionHeightfieldMinMaxTexture` | `VirtualHeightfieldMesh.MinMaxTexture` | VHM 플러그인 | UE Experimental 플러그인 |
| `UMaterialExpressionMeshPartitionResource` | `MeshPartition.Resource` | MeshPartition 플러그인 | UE Experimental 에디터 플러그인 |
| `UMaterialExpressionMeshPartitionTexcoord` | `MeshPartition.Texcoord` | MeshPartition 플러그인 | UE Experimental 에디터 플러그인 |
| `UMaterialExpressionMeshPartitionChannelSample` | `MeshPartition.ChannelSample` | MeshPartition 플러그인 | UE Experimental 에디터 플러그인 |
| `UMaterialExpressionMeshPartitionChannelSampleIndex` | `MeshPartition.ChannelSampleIndex` | MeshPartition 플러그인 | UE Experimental 에디터 플러그인 |
| `UMaterialExpressionMeshPartitionInspector` | `MeshPartition.Inspector` | MeshPartition 플러그인 | UE Experimental 에디터 플러그인 |

### 19.6.30 노드가 아닌 클래스·제외(23)

| UE 5.8 | 상태 | Rocks | 이유 |
|---|---|---|---|
| `UMaterialExpression` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionCustomOutput` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionExternalCodeBase` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionGenericConstant` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionNamedRerouteBase` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionRerouteBase` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionSparseVolumeTextureBase` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionSubstrateBSDF` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionSubstrateUtilityBase` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionTextureBase` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionTextureSampleParameter` | 추상 | — | 추상 기반. 공통 속성은 서술 묶음 규칙으로 나눈다 |
| `UMaterialExpressionParameter` | 추상 | — | UE에서 abstract는 아니지만 직접 쓰지 않는 기반(팔레트 제외, MaterialExpressionClasses.cpp:118–120) |
| `UMaterialExpressionComment` | 공통 | — | NodeGraph 배치 데이터의 주석 상자(GraphDocumentLayout) |
| `UMaterialExpressionReroute` | 공통 | `Graph.Reroute` | NodeGraph 공통 재경유, 의미 비교 때 접힘 |
| `UMaterialExpressionComposite` | 공통 | `Graph.CollapsedGraph` | NodeGraph 하위 그래프(GD25) |
| `UMaterialExpressionPinBase` | 공통 | `Graph.CollapsedGraph 경계` | 하위 그래프 경계 핀은 NodeGraph가 유도 |
| `UMaterialExpressionAtmosphericFogColor` | 제외 | — | UE deprecated(옛 대기 안개). SkyAtmosphere 노드로 대체 |
| `UMaterialExpressionLegacyBlendMaterialAttributes` | 제외 | — | UE deprecated(Substrate 레이어 시제품, Private) |
| `UMaterialExpressionMaterialXPlace2D` | 제외 | — | deprecated(PL/Interchange/Runtime/Source/Import/Private/MaterialX/MaterialExpressions/MaterialExpressionPlace2D.h:17 — 5.5 deprecated) |
| `UMaterialExpressionMaterialXRotate2D` | 제외 | — | deprecated(PL/Interchange/…/MaterialExpressionRotate2D.h:12 — 5.5 deprecated) |
| `UMaterialExpressionMaterialXSwizzle` | 제외 | — | deprecated(PL/Interchange/…/Public/MaterialX/MaterialExpressions/MaterialExpressionSwizzle.h:10–11 — 5.7 deprecated) |
| `UDEPRECATED_MaterialExpressionARKitPassthroughCamera` | 제외 | — | deprecated(PL/Runtime/AR/AppleAR/AppleARKit/Source/AppleARKit/Private/ExternalTextureMaterialExpression.h:20–21 — UDEPRECATED_) |
| `UDEPRECATED_MaterialExpressionGoogleARCorePassthroughCamera` | 제외 | — | deprecated(PL/Runtime/AR/Google/GoogleARCore/…/MaterialExpressionGoogleARCorePassthroughCamera.h:14–15 — UDEPRECATED_) |
