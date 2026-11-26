# 머티리얼 시스템(MaterialSystem) 설계

## 6. IR — MS2d 구현 세부

### 6.11 MS2d 구현 세부(표면·대표 노멀·추가 출력)

근거는 UE 5.8 Substrate(`MaterialExpressionSubstrate.cpp`(MES)·`SubstrateTranslatorCommon.cpp`(STC)·`Substrate.ush`(S)·`SubstrateExport.ush`(SE)·`SubstrateLegacyConversion.ush`(SLC))와 고전 경로(`HLSLMaterialTranslator.cpp`(HT)·`MaterialTemplate.ush`(MT)·`MaterialAttributeDefinitionMap.cpp`(MADM))다. Rocks 렌더러는 UE Blendable처럼 픽셀당 closure 하나다(10.3절, Renderer 7.1절). 그래서 번역은 언제나 표면 트리를 closure 하나로 접는다.

| 묶음 | 내용 |
|---|---|
| MS2d1 | 표면 기반: 표면 기록·IR `SurfaceRef`, 출력 노드 `Surface` 핀, 내보내기(`SurfaceAttributes` — `subsurfaceColor` 추가), `SurfaceFromShadingModel`·`ShadingModel` 노드, shading model 집합 정의, 대표 노멀 진입점(`EvaluateMaterialNormal`)·순환 검사, AO 활성 좁히기, 영역 표면(LightFunction·PostProcess·UserInterface·ToDecal·VolumetricFogCloud)·Unlit |
| MS2d2 | BSDF·연산: Slab·SimpleClearCoat·Hair·Eye·SingleLayerWater, 연산(HorizontalMix·VerticalLayer·Add·Weight·Select) parameter blending, 고전 preset → slab 변환(연산 아래의 preset), 도우미 넷(TransmittanceToMeanFreePath·MetalnessToDiffuseAlbedoF0·HazinessToSecondaryRoughness·ThinFilm), Toon TODO |
| MS2d3 | 추가 출력 노드·custom 출력 진입점(번호별 함수·존재 정의·정점 빈도), MaterialCache TODO |

`SurfaceFromAttributes`는 MaterialAttributes 값(MS2e)을 받으므로 **MS2e로 옮긴다**(15장 단계 표를 고친다). MS2d 동안 이 노드는 `NotYetLowered`다.

**MS2d1 표면 기반.**
- **표면 값.** 표면 핀(타입 `Surface`)의 값은 IR 값 `SurfaceRef`(번호 91, 타입 `IrScalar::Surface`(6) 성분 1 — 산술·변환·선택에 들어가지 못한다(V1), 텍스처와 같은 규칙)다. 즉시값 [0]이 번역기 표면 기록 번호다. 기록은 노드 하나의 표면이다: BSDF 종류(Slab·Unlit·Hair·Eye·SingleLayerWater·VolumetricFogCloud·영역 표면·고전 preset), 매개변수(IR 값 번호), 기능 표지(EdgeColor·Fuzz·SecondRoughness·SimpleClearCoat·SSS 종류·MFP·Anisotropy·Glint·Thin), 노멀·tangent, coverage, 연산이면 자식 기록 둘과 연산 매개변수다. 기록은 트리 그대로 남고(10.3절 "IR의 Surface 타입은 처음부터 트리"), 접기는 출력 노드에서 한다(여러 closure gate가 열리면 접기만 바꾼다). 늦은 진단 자리 값(Pending)이 표면 핀에 오면 그 자리가 표면 기록의 자리 값이 된다(그 노드 진단은 결과에서 닿을 때 — 6.10절 MS2c1).
- **ShadingModel 값.** `ShadingModel` 핀 타입은 IR `UInt1`(Renderer 14.2절 번호)이다. `ShadingModel` 노드는 그 상수다. Rocks 번호표에 없는 preset(PreintegratedSkin — Renderer 14.2절이 SubsurfaceProfile로 대신, FromMaterialExpression)은 노드 위치 InvalidProperty "Shading model is not supported: <이름>"이다.
- **SurfaceFromShadingModel(고전 preset).** UE `SubstrateShadingModels`의 고전 입력을 그대로 지나가게 한다(10.3절 "변환 없이"). 입력 기본값은 UE `CompileCommon`(MES:232–377)과 같다: BaseColor 0, Metallic 0, Specular 0.5, Roughness 0.5, Anisotropy 0, Emissive 0, Normal 대표 노멀 규칙(아래), Tangent 정점 tangent, SubSurfaceColor 1(shading model이 TwoSidedFoliage면 0 — MADM:50–55), ClearCoat 1, ClearCoatRoughness 0.1, Opacity 1, TransmittanceColor 0.5, ThinTranslucentSurfaceCoverage 1, 물 계수 0·PhaseG 0·ColorScaleBehindWater 1, ClearCoatNormal = Normal, CustomTangent = Normal. ShadingModel 입력이 연결되면 그 값(픽셀마다 — UE `SetShadingModelFromExpression`), 아니면 속성 `shadingModelOverride`다. 물·얇은 투과·clear coat 아래 노멀·custom tangent 입력은 UE 고전 경로처럼 custom 출력으로 낸다(MS2d3 — ThinTranslucent·SingleLayerWater·ClearCoatNormal·Tangent 출력과 같은 진입점, 연결된 것만).
- **내보내기 대상.** `SurfaceAttributes`는 UE 고전 픽셀 속성(`FPixelMaterialInputs`)과 같은 칸이다: baseColor·metallic·specular·roughness·anisotropy·emissive·opacity·opacityMask·normal·tangent·**subsurfaceColor**(추가 — UE `MP_SubsurfaceColor`, Subsurface·TwoSidedFoliage·Cloth의 색)·customData0·customData1·ambientOcclusion·shadingModel. GBuffer 배치는 렌더러가 고전 규칙(UE `SetGBufferForShadingModel`)으로 한다. `subsurfaceColor` 칸 추가는 Renderer 14.1절 개정 요청이다(16절 MRD17). 칸 값은 UE 템플릿처럼 생성 원문이 saturate한다(baseColor·metallic·specular·roughness·AO·opacity·subsurfaceColor saturate, anisotropy clamp(−1, 1) — MT:3778–4146).
- **내보내기 식(slab → 고전, UE SE:1058–1312·S:3822–4110).** 접은 slab에서: metal = saturate((max3(F0) − 0.08)/0.32), baseColor = lerp(DiffuseAlbedo, F0, metal), specular = saturate(max3(F0)/0.08), roughness는 SimpleClearCoat가 아닌 haze를 lerp(Roughness, HazeRoughness, HazeWeight)로 굽고, F90이 있으면 UE `SubstrateBakeF90ToReflectivity`(NoV = saturate(dot(정점 노멀, CameraVector)))로 F0에 굽는다. emissive = Emissive × coverage. shading model과 칸은 UE 우선순위 사슬(SE:1137–1300)이다:

| 조건(위가 우선) | shading model | 칸 |
|---|---|---|
| Hair | Hair | metallic = Scatter, customData0 = Backlit(고전 MP_CustomData0) |
| Eye | Eye | customData0 = IrisMask, customData1 = IrisDistance, tangent = IrisPlaneNormal(고전 MP_Tangent) |
| SSS profile | SubsurfaceProfile | opacity = RadiusScale(TODO MPQ-SubsurfaceProfile — profile 에셋이 없으므로 속성 값이 있으면 PrerequisiteMissing) |
| SSS Wrap | Subsurface | subsurfaceColor = exp(−1/max(ε, MFP_cm·0.01)), opacity = 1 − PhaseAnisotropy |
| SSS TwoSidedWrap | TwoSidedFoliage | 같다 |
| Fuzz | Cloth | subsurfaceColor = FuzzColor, customData0 = FuzzAmount |
| SimpleClearCoat haze | ClearCoat | customData0 = HazeWeight, customData1 = HazeRoughness |
| Thin(얇은 투과, slab IsThin) | ThinTranslucent | Rocks 10.3절 표(UE Blendable은 DefaultLit로 낸다 — 편차, Renderer 14.2절 ThinTranslucent를 쓴다) |
| 그 밖(SSS Diffusion·SimpleVolume 포함 — UE Blendable과 같다) | DefaultLit | — |
| Unlit·영역 표면 | Unlit | emissive만 |

  - SingleLayerWater는 slab 사슬 밖 전용 BSDF다: baseColor·metallic(UE 내보내기는 0 — S:3965 그대로)·specular·roughness·emissive, shading model SingleLayerWater, 물 계수는 custom 출력(MS2d3).
  - 고전 preset이 연산 없이 뿌리에 바로 이어지면 내보내기 식을 거치지 않고 그 고전 입력을 그대로 칸에 쓴다(10.3절 — UE Blendable의 FrontMaterial 없는 고전 컴파일과 같은 결과).
- **shading model 집합.** 내보낸 shadingModel이 상수면 그 하나, 연결된 ShadingModel 입력·Select로 갈리면 static으로 닿는 상수들의 합집합이다(UE `ShadingModelsFromCompilation` — 상수가 아닌 값(파라미터 등)이 섞이면 Rocks 번호표 전부). 생성 원문은 `#define MATERIAL_SHADING_MODELS <비트>u`와, 하나뿐이면 `#define MATERIAL_SINGLE_SHADING_MODEL <번호>`(여럿이면 정의하지 않는다 — UE `MATERIAL_SINGLE_SHADINGMODEL`과 같다)를 낸다. 번역 결과 `MaterialTranslation.shadingModels`(비트)에도 담는다(8.3절 `MaterialShaderTraits` — 쿠킹이 거르기에 쓴다).
- **대표 노멀(19.3.3절).** 새 진입점 `IrEntry::Normal`(픽셀)과 생성 함수 `float3 EvaluateMaterialNormal(in MaterialInputs c)`를 둔다. 값은 접은 표면의 world 노멀(정규화)이다: shading model을 정하는 BSDF(접은 slab이면 parameter blending으로 섞은 노멀 — UE `SubstrateBlendNormal`)의 Normal 입력, 연결이 없으면 정점 노멀이다. 연결된 Normal은 `tangentSpaceNormal`이면 tangent 기저로 world로 바꾸고(UE `TransformNormalFromRequestedBasisToWorld`) 정규화한다. pass는 UE `CalcPixelMaterialInputs`처럼 이 함수를 먼저 불러 `c.pixelNormal`을 채운 뒤 다른 함수를 부른다(7.3절 개정). `PixelNormal`·`ReflectionVector`·`Fresnel`은 지금처럼 `c.pixelNormal`을 읽는다. **순환 검사(V2)**: Normal 진입점(그리고 MS2d3의 ClearCoatNormal·Tangent·BentNormal 출력)에서 `PixelNormal` 문맥 값이나 그것을 읽는 값이 닿으면 "Invalid node PixelNormalWS used for Normal input."(UE HT:11784–11787)이다. UE보다 넓은 규칙이다(UE는 MP_Normal만 — 19.3.3절).
- **Surface 진입점.** `EvaluateMaterialSurface`는 칸마다 결과(entry Surface, slot = 칸 이름)를 쓴다. normal 칸은 대표 노멀 값(같은 IR 값 — hash-consing으로 한 번 계산), tangent 칸은 접은 tangent(연결 없으면 정점 tangent, tangent 공간이면 world로)다. 결과가 없는 칸은 `DefaultSurfaceAttributes()` 값이다.
- **AO 활성.** AmbientOcclusion 핀은 영역 Surface·DeferredDecal이고 shading model이 lit일 때만 활성이다(UE Material.cpp:7929–7930 — 집합이 Unlit 하나면 비활성, V5 경고).
- **영역 표면.** UE처럼 Unlit BSDF로 낮춘다(MES 1.8–1.11): LightFunction → emissive = Color, PostProcess → emissive = Color·opacity = Opacity, UserInterface → UE `SubstrateCreateUIMaterial`(블렌드 모드별: Additive emissive = Color·Opacity, Modulate emissive 0·opacity 칸 없이 transmittance = Color — Rocks는 emissive = Color로 두고 블렌드는 pass가 한다(편차 기록), 그 밖 emissive = Color·opacity = Opacity), ToDecal → 자식 slab을 내보내고 opacity = Coverage(UE `SubstrateWeightParameterBlending` — 자식은 slab만, 아니면 UE 문구 오류), VolumetricFogCloud → baseColor = Albedo, subsurfaceColor = Extinction(UE 고전 Volume 칸), emissive, ambientOcclusion = AO(emissiveOnly면 Albedo·Extinction 0, AO 1). 영역과 맞지 않는 표면(예: Surface 영역의 PostProcess 표면)은 V6 "Surface node is not valid for the material domain"이다(UE는 Substrate 트리 검증이 거른다).

**MS2d2 BSDF·연산.**
- **BSDF 입력 기본값**은 UE Compile 값이다(조사표 — Slab: DiffuseAlbedo 0.18, F0 0.04, F90 1, Roughness 0.5, Anisotropy 0, SSS MFP 0·Scale 1·PhaseAnisotropy 0, Emissive 0, SecondRoughness 0·Weight 0, FuzzRoughness = Roughness(연결 없으면), FuzzAmount 0·FuzzColor 0, Glint 1, GlintUV 0; SimpleClearCoat: Coverage 1·ClearCoatRoughness 0.1·BottomNormal = Normal; Hair: BaseColor 0·Scatter 0·Specular 0.5·Roughness 0.5·Backlit 0; Eye: DiffuseColor 0·Roughness 0.5·IrisMask 0·IrisDistance 0, CorneaNormal은 UE처럼 정점 tangent 기본(MES:2237); SingleLayerWater: BaseColor 0·Metallic 0·Specular 0.5·Roughness 0.5·TopMaterialOpacity 0·WaterAlbedo 0·Extinction 0·PhaseG 0·ColorScaleBehindWater 1; Unlit: Emissive 0·Transmittance 1).
- **기능 표지**는 UE `SubstrateGenerateMaterialTopologyTree`(MES:1230–1250)와 같다: EdgeColor ⇐ F90 연결, Fuzz ⇐ FuzzAmount 연결, SecondRoughness ⇐ SecondRoughnessWeight 연결(또는 profile), SSS ⇐ profile 또는 MFP 연결, Anisotropy ⇐ Anisotropy 연결, Glint ⇐ GlintValue 연결. 표지가 없으면 그 매개변수를 UE "simplification"대로 기본으로 둔다(MES:898–926). SSS 종류: subSurfaceType 속성, MFP가 없으면 None, 연산의 위층(bottom 아님)은 SimpleVolume(S:1595–1607).
- **접기.** 연산은 모두 parameter blending이다(UE Blendable은 closure 1이라 연산을 깊은 것부터 전부 blending으로 바꾼다 — STC:623–1433. 결과가 같으므로 한 번에 전부 바꾼다). 식은 UE S:2853–3361과 같다(입력 slab은 먼저 F90·glint를 F0에 굽는다, c = saturate(Coverage)):
  - Horizontal: w = c_F·m/max(ε, c_F·m + c_B·(1 − m)), m = saturate(Mix). 색·F0·F90·거칠기·anisotropy·fuzz·haze·MFP·두께 = lerp(B, F, w). 표지 OR, SSS 종류 max. Coverage = lerp(c_B, c_F, Mix(saturate 전 — UE 그대로)). 노멀 = normalize(lerp(N_B, N_F, w)).
  - Add: a = c_A/max(ε, c_A + c_B). Emissive 합, DiffuseAlbedo·F0·F90·FuzzColor saturate(합), 나머지 lerp(B, A, a). Coverage = saturate(c_A + c_B). PhaseAnisotropy는 UE처럼 0. 노멀 = normalize(lerp(N_B, N_A, a)).
  - Vertical: UE 층 정보(TTB = c_T·c_B, TOB = (1 − c_T)c_B, Coverage = c_T + TOB), 위층 slab 투과 Tslab(UE `IsotropicMediumSlabTransmittance`, 굴절 NoV), 위층 albedo는 UE ALU 근사(`IsotropicMediumSlabPunctualDirectionalAlbedo`의 LUT 없는 분기 — Rocks에 SimpleVolume LUT가 없다, 편차로 기록), L = saturate((TTB·avg(Tslab) + TOB)/Coverage)로 거칠기·fuzz·haze lerp(Top, Base, L), 색은 (TTB·Tslab + TOB)/Coverage·Base + c_T/Coverage·Top. anisotropy 0, SSS 종류 min(Base, Diffusion). 노멀 = normalize(lerp(N_Base, N_Top, saturate(c_T)·0.5)).
  - Weight: Coverage ×= saturate(Weight). Select: (SelectValue > threshold) ? B : A 전체(노멀·tangent도 같은 쪽 — UE는 tangent를 거꾸로 고르는 결함이 있다(MES:3370) — Rocks는 고친다, 편차 기록).
- **허용 표.** UE STC:388–394와 같다: Weight {Unlit·Slab·Toon}, Select {Slab·Hair·Eye·Toon}, Horizontal·Vertical·Add·ToDecal {Slab}. 어기면 UE 문구 "Substrate error: Operator <연산> cannot be used with the following data flowing through <BSDF 목록>."(V1 자리 — UnsupportedCapability). 고전 preset은 연산 아래에서 UE `SubstrateConvertLegacyMaterialDynamic`(SLC:153–524)으로 slab이 된다(DefaultLit·Subsurface·TwoSidedFoliage·Cloth·ClearCoat·ThinTranslucent — 조사표 식), Hair·Eye·SLW·Unlit preset은 그 전용 BSDF다.
- **SingleLayerWater**는 parameter blending에 쓸 수 없다(UE 문구 둘 — MES:2368–2375).
- **도우미**는 UE 식 그대로 IR로 펼친다(조사표 — TransmittanceToMFP, MetalnessToDiffuseAlbedoF0, HazinessToSecondaryRoughness, ThinFilm). ThinFilm의 F_ThinFilm은 `MaterialSubstrate.rsh`(새 셰이더 파일)의 함수 문맥 값이 아니라 IR 상수·연산으로 펼치기 어려우므로 새 IR 연산 `ShaderFunction`(이름 있는 순수 함수 호출 — 셰이더 파일 정의)으로 낸다.
- `SurfaceToon`은 TODO(MPQ-ToonShading — 이미 `.Missing`) 늦은 진단, profile 속성(subsurfaceProfile·specularProfile)이 값을 가지면 그 노드 위치에 PrerequisiteMissing(MS-T12 (b)).

**MS2d3 추가 출력.**
- **생성 함수.** custom 출력 노드마다 출력 번호 i(UE `GetNumOutputs`)별로 `float4 EvaluateMaterialCustomOutput_<이름><i>(in MaterialInputs c)`(픽셀) 또는 `(in MaterialVertexInputs c)`+`EvaluateMaterialCustomOutputPrevious_<이름><i>`(정점 빈도·이전 프레임 — FirstPersonOutput)을 낸다. 이름은 UE `GetFunctionName`에서 `Get` 접두를 뗀 Rocks 이름이다(예: BentNormal·ClearCoatBottomNormal·TangentOutput·ThinTranslucentMaterialOutput·SingleLayerWaterMaterialOutput·VolumetricAdvancedMaterialOutput·FirstPersonOutput·VirtualTextureOutput). 값은 float4로 맞춘다(짧으면 0을 붙인다). 존재 정의 `#define MATERIAL_CUSTOM_OUTPUT_<NAME> <출력 수>`·`#define MATERIAL_HAS_CUSTOM_OUTPUT_<NAME><i> 1`(UE `NUM_MATERIAL_OUTPUTS_`·`HAVE_`)를 낸다. 7.3절 표의 한 줄을 이 규칙으로 고친다.
- **입력.** 기본값 있는 입력은 UE 기본값(조사표 5.1), 기본값 없는 입력은 연결이 없으면 그 번호의 함수를 내지 않는다(UE INDEX_NONE — SubsurfaceMedium·MotionVectorWorldOffset·TemporalResponsiveness), BentNormal·ClearCoatNormal·TangentOutput은 연결이 없으면 "Input missing"(UE MaterialExpressions.cpp:20262–20344). 노멀·tangent 출력은 tangent 공간 그대로 낸다(UE처럼 소비자가 바꾼다 — MT:4913–4925).
- **하나만.** 같은 종류의 출력 노드가 둘이면 "The material can contain only one <이름> node"(UE HT:876–961 — MaterialCache만 여럿 가능).
- **IR.** 진입점은 `IrEntry::CustomOutput`이고 slot은 "<이름><i>"(정적 원문 표)다. 정점 빈도 출력은 `IrEntry::CustomOutputVertex`(새)·이전 프레임은 정점 문맥 둘에서 번역한다. 번역 결과 `customOutputs`(이름 목록 — 8.3절 거르기)에 담는다.
- 입력 노드처럼 보이는 VolumetricAdvancedInput·CloudSampleAttribute·VolumetricCloudEmptySpaceSkippingInput은 문맥 값(이미 표에 있다)을 읽는 lowering이다. NeuralNetworkInput·Output은 UE 신경망 후처리(`neural` 기능 — profile은 TODO MPQ-NeuralProfile, 19.9절)로 custom 출력 GetNeuralInput과 문맥 값이다. MaterialCacheOutput은 TODO(MPQ-VirtualTexture).

**편차·한계(요약).** UI 표면의 Modulate·AlphaHoldout 변형은 pass 블렌드가 맡는다. Vertical의 위층 albedo는 ALU 근사다(UE 데스크톱은 4D LUT). Select tangent 결함은 고친다. MFP만 있는 SSS(Diffusion)는 UE Blendable처럼 DefaultLit이다. slab IsThin은 ThinTranslucent로 낸다(UE Blendable DefaultLit). 대표 노멀 순환 규칙은 UE보다 넓다.
