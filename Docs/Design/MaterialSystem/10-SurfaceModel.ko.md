# 머티리얼 시스템(MaterialSystem) 설계

## 10. shading model과 출력

### 10.1 렌더러와의 경계

Renderer 스펙 14.6절 경계표와 같은 나눔이다. 아래는 그 표에 머티리얼 쪽 항목을 더한 것이다.

| 항목 | 머티리얼 시스템(이 문서) | Renderer 스펙 |
|---|---|---|
| `SurfaceAttributes` 필드와 의미 | 그래프(BSDF 트리)를 이 필드로 낮춘다(10.3절) | 정한다(14.1절) |
| shading model·블렌드 모드·영역 번호표 | 참조한다(`RenderData`). 노드·이름 표시·검증·UI | 정의 권한을 가진다(결정 RN5). 코드는 C++ `RenderData`, HLSL `/Engine/Shared/`(3.2·14.2절) |
| GBuffer 배치·인코딩, BRDF·직접광 평가 | — | 정한다(7.1·14.4절) |
| pass 목록·경로·pass 셰이더·pass 퍼뮤테이션 차원 | 거르기 입력(`MaterialShaderTraits`)을 준다. static 변형이 pass 퍼뮤테이션과 곱해진다(9.5절) | 정한다(14.3절). 거르기 함수가 `MaterialShaderTraits`를 받는다 |
| 머티리얼 셰이더 집합 키·쿠킹 단위·static 변형 | 정한다(8.6·12절) | 조회 키만 요구(14.5절) |
| 그래프·IR·HLSL 생성·인스턴스·파라미터 갱신 | 정한다 | — |
| 생성 함수(정점·이전 정점·표면·opacity·마스크·PDO·custom 출력·미리보기) | 생성 파일 계약으로 항상 낸다(7.3절) | 어느 pass가 어느 함수를 부르는지 정한다(14.1·14.3절) |
| 문맥 구조체 `MaterialInputs`·`MaterialVertexInputs` | 정한다(7.3절) | pass 원문이 채운다(정점 입력 타입과 함께) |
| (pass, 퍼뮤테이션, 형식)별 머티리얼 텍스처 상한 | 계산해 진단·쿠킹 검사에 쓴다(7.4절 4항·14절 E9) | `형식 상한 − 렌더러 합계` 표를 정한다(7.4절, 결정 RN13) |
| scene texture 읽기 노드 | 노드 검증(V6) | 경로별 가능 여부(14.6절) |
| WPO 현재·이전 | 정점 함수로 낸다(7.3절). `SurfaceAttributes` 필드가 아니다 | 모든 메시 pass의 정점 셰이더가 부르고, 이전 WPO는 velocity pass가 쓴다(14.1절) |
| 렌더 경로·기능 등급 정의 이름 | 생성 원문의 `#if`에 쓴다(6.3절). Render Path Switch의 깊이 일치를 검사한다(V15) | 경로 이름 `RENDER_PATH_DEFERRED`·`RENDER_PATH_FORWARD_PLUS`를 정한다(14.3절). 기능 등급 이름은 형식 정의(MSR9) |
| 기본 머티리얼 대체 | 대체를 결정하고 진단한다(11.3절) | 덧칠 없이 기본 머티리얼로 그리고 대체 수를 계수로 남긴다(14.7절 MRD8) |

### 10.2 UE 5.8 조사: Substrate와 고전 shading model

**Substrate 기본 여부.**

| 항목 | 값 | 근거 |
|---|---|---|
| 엔진 CVar `r.Substrate` | 기본 0, ReadOnly | `RC/Private/RenderUtils.cpp:2068–2072` |
| 새 프로젝트 | `GameProjectUtils`가 DefaultEngine.ini에 `r.Substrate=True`와 `r.Substrate.ProjectGBufferFormat=0`(Blendable)을 쓴다(템플릿에 값이 있으면 유지). 정책 주석 "New project will using Substrate with Blendable GBuffer" | `ED/GameProjectGeneration/Private/GameProjectUtils.cpp:199–211`, `RC/Private/RenderUtils.cpp:2074–2083` |
| 고급 템플릿 | AEC·PhotoStudio 등은 Adaptive(=1) 명시 | `Templates/TP_AEC_ArchvisBP/Config/DefaultEngine.ini:43–44`, `Templates/TP_PhotoStudioBP/Config/DefaultEngine.ini:65–70`(리포지토리 루트 기준) |
| Blendable 판정 | 모바일 deferred이거나, 플랫폼이 Adaptive를 지원하지 않거나, 프로젝트가 Adaptive를 요청하지 않으면 Blendable | `RC/Private/RenderUtils.cpp:2226–2233` |
| Adaptive 지원 플랫폼 | PCD3D_SM6·METAL_SM6(Mac·iOS)·VULKAN_SM6만 | `CFG/Windows/DataDrivenPlatformInfo.ini:78, 135`, `CFG/Mac/DataDrivenPlatformInfo.ini:68, 113`, `CFG/IOS/DataDrivenPlatformInfo.ini:81, 125`, `CFG/VulkanPC/DataDrivenPlatformInfo.ini:141, 251` |
| Blendable 제약 | 픽셀당 20 byte, closure 1개 강제, glint 없음, rough refraction·확률적 조명 없음 | `RC/Private/RenderUtils.cpp:2255–2261, 2328–2373, 2441, 2237, 2546` |
| SM5 플랫폼 | PCD3D_SM5·VULKAN_SM5는 closure 최대 1 | `CFG/Windows/BaseWindowsEngine.ini:50–60` |
| Blendable 저장 | Substrate 데이터를 고전 GBuffer 데이터로 내보낸다 | `SH/Private/BasePassPixelShader.usf:1927–1948`, `SH/Private/Substrate/SubstrateExport.ush:1058` |
| Adaptive 저장 | 고전 GBufferA–C를 끄고 top layer(노멀 22·거칠기 8·종류 2비트)와 material 배열(`r.Substrate.BytesPerPixel` 기본 80) | `RC/Private/GBufferInfo.cpp:265–328`, `SH/Private/Substrate/Substrate.ush:922–940`, `RR/Private/Substrate/Substrate.cpp:646–656`, `RC/Private/RenderUtils.cpp:2092–2096` |

**고전 경로의 레거시 상태.**
- C++·열거 수준 deprecation은 없다. `EMaterialShadingModel` 값에 deprecated 표시가 없다(`RE/Classes/Engine/EngineTypes.h:706–728`).
- 셰이더 함수에는 `// UE_DEPRECATED 5.7 - Deprecated by Substrate` 주석이 있다: GBuffer 인코딩(`SH/Private/DeferredShadingCommon.ush:966`), 고전 동적 조명 누적(`SH/Private/DeferredLightingCommon.ush:315`), BxDF 적분(`SH/Private/ShadingModels.ush:1136`).
- Substrate를 켜도 고전 루트 노드 에셋은 자동으로 다시 쓰지 않는다(`IsHiddenMaterialAssetConversionEnabled`가 하드코딩 true, `RC/Private/RenderUtils.cpp:2564–2570`). Blendable에서 FrontMaterial이 연결되지 않은 머티리얼은 고전 머티리얼 셰이더로 컴파일된다(`RE/Private/Materials/SubstrateTranslatorCommon.cpp:1579–1593`).
- 정리: 5.8에서 고전 경로는 "지원되는 선택지이지만 셰이더 수준에서 Substrate로 대체 예고된 경로"다. 새 프로젝트 기본은 Substrate다.

**Substrate 구조(참고).**
- BSDF 노드: Slab(DiffuseAlbedo·F0·F90·Roughness·Anisotropy·Normal·Tangent·SSS MFP·Emissive·SecondRoughness·Fuzz·Glint·Profile 등, `RE/Public/Materials/MaterialExpressionSubstrate.h:273–436`, 입력 `:281–405`), Hair·Eye·Single Layer Water·Unlit·Volumetric Fog Cloud·Light Function·PostProcess·UI·Toon(Experimental)(`:508–980`, Toon `:860–861`).
- 연산: Horizontal Blend·Vertical Layer·Add·Coverage Weight·Select(`:1135–1338`), 셰이더 상수(`SH/Shared/SubstrateDefinitions.h:59–65`).
- 한도: closure 최대 8, 연산 15, 트리 깊이 48(`SubstrateDefinitions.h:16–22`, `RE/Classes/Engine/EngineTypes.h:840`).
- 예산 초과 시 연산을 깊이 순으로 parameter blending하고 slab을 단순화한다(`RE/Private/Materials/SubstrateTranslatorCommon.cpp:624–713, 1319–1433`).
- 고전 shading model → Substrate 변환 노드(`MaterialExpressionSubstrate.h:72–271`)와 셰이더 쪽 변환(`SH/Private/Substrate/SubstrateLegacyConversion.ush:153`). 대응: Subsurface → SubsurfaceWrap, PreintegratedSkin → SubsurfaceMFP, TwoSidedFoliage → SubsurfaceThinTwoSided(`RE/Private/Materials/Material.cpp:3425–3439`).

**영역·블렌드 모드(레거시 확인).**

| 열거 | 값 | 상태 | 근거 |
|---|---|---|---|
| 영역 | Surface·DeferredDecal·LightFunction·Volume·PostProcess·UI | 활성 | `RE/Public/MaterialDomain.h:11–30` |
| 〃 | RuntimeVirtualTexture | **Deprecated**, Hidden | `MaterialDomain.h:26–27` |
| 블렌드 모드 | Opaque·Masked·Translucent·Additive·Modulate·AlphaComposite·AlphaHoldout | 활성 | `RE/Classes/Engine/EngineTypes.h:244–259` |
| 〃 | TranslucentColoredTransmittance | Substrate 전용(dual source blending 요구) | `EngineTypes.h:254`, `RE/Private/Materials/HLSLMaterialTranslator.cpp:15000–15001` |
| 〃 | TranslucentGreyTransmittance·ColoredTransmittanceOnly | Hidden 별칭 | `EngineTypes.h:256–258` |
| shading model | Unlit·DefaultLit·Subsurface·PreintegratedSkin·ClearCoat·SubsurfaceProfile·TwoSidedFoliage·Hair·Cloth·Eye·SingleLayerWater·ThinTranslucent·FromMaterialExpression | 활성(열거 수준) | `EngineTypes.h:706–728` |
| 〃 | Strata(Substrate) | Hidden | `EngineTypes.h:721` |

### 10.3 Rocks 표면 모델(결정 MD1)

**그래프 모델은 Substrate형이고, 렌더러에는 `SurfaceAttributes`로 내보낸다.**
- 출력 노드의 `Surface` 입력은 BSDF 트리다. BSDF 노드는 `SurfaceSlab`·`SurfaceSimpleClearCoat`·`SurfaceHair`·`SurfaceEye`·`SurfaceSingleLayerWater`·`SurfaceUnlit`·`SurfaceToon`(TODO)·`SurfaceVolumetricFogCloud`이고, 연산 노드는 `SurfaceHorizontalMix`(수평)·`SurfaceVerticalLayer`(수직)·`SurfaceAdd`·`SurfaceWeight`·`SurfaceSelect`다. 영역 표면 노드는 `SurfaceLightFunction`·`SurfacePostProcess`·`SurfaceUserInterface`·`SurfaceToDecal`이다(이름은 19장).
- **shading preset 노드**는 `SurfaceFromShadingModel` 하나다(UE `SubstrateShadingModels`와 같다). shading model은 속성으로 고르거나 `ShadingModel` 입력으로 픽셀마다 고른다. 고를 수 있는 값은 Renderer 스펙 14.2절 목록이다: `Unlit`·`DefaultLit`·`ClearCoat`·`Subsurface`·`SubsurfaceProfile`·`TwoSidedFoliage`·`Cloth`·`Eye`·`ThinTranslucent`·`SingleLayerWater`(`Hair`는 P3). 고전 입력(baseColor·metallic·specular·roughness 등)을 받아 BSDF 트리를 낸다. 새 머티리얼의 기본 그래프는 `DefaultLit`인 `SurfaceFromShadingModel` 하나가 출력에 이어진 모양이라 고전 사용 경험과 같다. 머티리얼 속성 집합은 `SurfaceFromAttributes`가 받는다.
- **내보내기.** 번역기의 마지막 단계가 IR의 `Surface` 값을 Renderer 스펙 14.1절 `SurfaceAttributes`(baseColor·metallic·specular·roughness·normal·emissive·ambientOcclusion·opacity·opacityMask·shadingModel·customData0·customData1·tangent·anisotropy)로 낮춘다.
  - preset 노드의 고전 입력은 변환 없이 그대로 지나간다. UE도 고전 변환 머티리얼은 고전 GBuffer 데이터를 그대로 넘기는 표지를 둔다(`RE/Public/Rendering/SubstrateMaterialShared.h:180–237`의 컴파일 출력).
  - `SurfaceSlab`의 직접 입력(DiffuseAlbedo·F0·F90)을 고전 값으로 바꾸는 변환은 UE Blendable의 내보내기(`SH/Private/Substrate/SubstrateExport.ush:1058`, 호출 `SH/Private/BasePassPixelShader.usf:1927–1948`)에 대응한다. 변환식은 UE `SubstrateExport.ush`와 같게 MS2d에서 정한다. 번역(MS2d)은 모든 표면 노드를 받는다. 렌더러가 그 shading model을 그리는 단계(아래 SF 단계)가 오기 전에는 그 조합이 거르기에서 빠진다(V6).
  - `customData0`·`customData1`의 shading model별 의미는 Renderer 스펙이 정한다(14.1절 "clear coat 강도·roughness 등").
  - 노멀 핀은 tangent 공간이 기본이고(6.4절 공간 분석), 내보낼 때 문맥의 기저로 world 공간으로 바꿔 정규화한다. `SurfaceAttributes.normal`은 world 공간 정규화 값이다(Renderer 14.1절).
  - WPO·PDO·마스크는 `SurfaceAttributes`로 내보내지 않고 각자의 진입점 함수로 낸다(7.3절, Renderer 14.1절).
- **closure 하나로 접기.** 번역기는 BSDF 트리를 closure 하나로 접는다. 연산은 parameter blending(입력 값의 가중 혼합)으로 바꾼다. 결과 BSDF와 기능 조합에서 shading model 하나를 고른다(아래 표). 표로 나타낼 수 없는 조합(예: fuzz와 2차 반사를 함께 쓰는 slab, slab과 hair의 혼합)은 정해진 우선순위로 하나를 고르고 경고한다. UE Blendable이 closure 하나를 강제하고 예산을 넘으면 단순화하는 것과 같은 성격이다(10.2절).

| 접은 BSDF·기능 | shading model(Renderer 14.2절) |
|---|---|
| `SurfaceUnlit` | `Unlit` |
| slab(추가 기능 없음) | `DefaultLit` |
| slab + 2차 반사, `SurfaceSimpleClearCoat` | `ClearCoat` |
| slab + SSS wrap | `Subsurface` |
| slab + SSS profile | `SubsurfaceProfile` |
| slab + 양면 wrap | `TwoSidedFoliage` |
| slab + fuzz | `Cloth` |
| slab + 얇은 투과 | `ThinTranslucent` |
| `SurfaceEye` | `Eye` |
| `SurfaceSingleLayerWater` | `SingleLayerWater` |
| `SurfaceHair` | `Hair`(P3, 계획 11.1절) |
| `SurfaceToon` | 없음(TODO `MPQ-ToonShading`, 16절 MRD14) |
| `SurfaceVolumetricFogCloud` | shading model 아님: Volume 영역 출력(albedo·extinction·emissive·AO) |
| `SurfaceLightFunction`·`SurfacePostProcess`·`SurfaceUserInterface` | shading model 아님: 그 영역의 출력(5.6절) |
| `SurfaceToDecal` | DeferredDecal 영역의 쓰는 속성(Renderer RS7) |

- **shading model이 하나인 머티리얼.** 트리가 static으로 shading model 하나로 정해지면 생성 원문에 `MATERIAL_SINGLE_SHADING_MODEL`을 내고, Renderer는 그 분기만 컴파일한다(Renderer 14.4절). 실행 중 선택(`SurfaceSelect`의 dynamic 입력)이면 픽셀마다 `shadingModel` 값을 쓴다.
- **여러 closure 저장은 계획 14.4절 연구 gate**다(Renderer 7.1절도 Blendable과 같은 고정 배치로 시작한다). IR의 `Surface` 타입은 처음부터 트리이므로, gate를 통과하면 접기·내보내기 단계만 기능 등급별로 바꾸면 된다. 그래프·에셋 migration은 필요 없다.

**블렌드 모드.** Renderer 스펙 14.2절 목록(`Opaque`·`Masked`·`Translucent`·`Additive`·`Modulate`·`AlphaComposite`)을 쓴다. UE 5.8의 `AlphaHoldout`(활성)은 Renderer 14.2절에 후속 값으로 있다. 필드 값은 지금 두고 그 단계 전에는 TODO 진단(`MPQ-AlphaHoldout`)이다. 들일 때 UE처럼 `Unlit` 전용 검사를 V5에 더한다(UE `RE/Private/Materials/MaterialShared.cpp:6378–6382`). Substrate 전용 색 투과 반투명(`TranslucentColoredTransmittance`)도 필드 값은 지금 두고 TODO 진단(`MPQ-ColoredTransmittance` — dual-source blending 경로)이다(10.4절 D4).

**GBufferD가 필요한 shading model.** `ClearCoat` 등은 GBufferD를 쓰므로 Deferred에서는 MRT 8 형식에서만 지원된다(Renderer 7.1절, 결정 RN3·RN4). GLES Deferred에서는 `Unsupported`이고 Forward+에서는 지원된다. 한 경로만 되는 조합은 그 경로의 쿠킹 오류다(Renderer 14.2절). 머티리얼 컴파일러는 이 조건을 V6 capability 검사로 진단한다.

**단계.**

| 단계 | 표면 범위 | 계획·Renderer 대응 |
|---|---|---|
| SF1 | `DefaultLit`·`Unlit` preset, `Opaque`·`Masked` | MAT0–MAT1(M3), Renderer RS0(`Unlit`)·RS1(Forward+ `DefaultLit`)·RS2(Deferred) |
| SF2 | `Translucent`·`Additive`·`Modulate`·`AlphaComposite` | MAT2–MAT3, Renderer RS5 |
| SF3 | `ClearCoat`·`Subsurface`·`SubsurfaceProfile`·`TwoSidedFoliage`·`Cloth`·`Eye`·`ThinTranslucent`·`SingleLayerWater` preset, `SurfaceSlab` 직접 입력, 표면 연산(parameter blending) | MAT4, 계획 11.1절 "M6–M8 고급 공통 재질", Renderer RS9 |
| Hair | `Hair` preset 값·`SurfaceHair` | 계획 11.1절 P3 별도 gate, Renderer 14.2절 |
| SF4 | 여러 closure 저장(기능 등급 SM6 형식만), glint·rough refraction | 필수 후속 — [LayeredSurface](../LayeredSurface.ko.md) |

SF 단계는 **렌더러가 그 표면을 그리는** 단계다. 번역은 단계 MS2d에서 모든 표면 노드·preset 값·연산을 받는다(19장, 개정 10). SF 단계 전의 조합은 V6로 거르기에서 빠지므로, MS2d 산출물은 SF 단계를 기다리지 않는다.

**결정 전 대안 (가)(고전 enum).** 출력 노드가 baseColor·metallic·specular·roughness·normal·shadingModel 핀을 직접 가지는 방식이다. 지금 렌더러 계약(`SurfaceAttributes`)과는 바로 맞지만, 이후 Substrate형으로 옮기려면 UE처럼 에셋 변환이 필요하다(UE `RE/Private/Materials/Material.cpp:3522–3569`). 결정 MD1은 (나)이다.

### 10.4 Renderer 문서와의 정합 결과(Renderer 개정 3)

개정 1의 다른 점 D1–D9를 2026-11-18 교차 검증에서 판단하고 두 문서 본문에 반영했다. 판단 기준은 UE 5.8 근거, 사용자 원칙(UE 구조 + Rocks 이름, 하위 호환 없음, 대체 금지), 단순성이다.

| 항목 | 결정 | 근거 | 반영 위치 |
|---|---|---|---|
| D1 WPO | `SurfaceAttributes` 필드가 아니라 정점 함수 `EvaluateMaterialVertex`·`EvaluateMaterialVertexPrevious`의 결과다 | UE도 WPO는 픽셀 입력 구조체(`SH/Private/MaterialTemplate.ush:371`)가 아니라 정점 파라미터를 받는 함수이고(`:4051, 4073`) depth·velocity·base pass 정점 셰이더가 부른다(`SH/Private/DepthOnlyVertexShader.usf:55`, `SH/Private/VelocityShader.usf:73, 81`). 픽셀 함수 결과에 정점 값을 섞지 않는 것이 단순하다 | Renderer 14.1·14.3·14.6절, 이 문서 7.3·10.1·10.3절 |
| D2 번호표 위치 | 정의 권한은 Renderer(결정 RN5) 그대로. C++ 열거는 `RenderData`, HLSL 상수는 `/Engine/Shared/` C++·HLSL 공용 `.h`. GBuffer 의미는 `RendererShaders` | MaterialSchema → `RendererShaders` → MaterialSchema 순환을 없앤다. 셰이더 시스템 3절이 공유 디렉터리의 `.h`를 허용하고, UE도 공용 정의를 `Engine/Shaders/Shared/`에 둔다(`SH/Shared/SubstrateDefinitions.h`) | Renderer 1·3.2·14.2·19.1절(RN5), 이 문서 2·3.2·10.1절 |
| D3 텍스처 상한 | 사용자 결정(2026-11-18)으로 RN13·MD4를 **형식별 상한**으로 개정했다. 원문은 하나, 상한은 형식 정의(D3D12·Vulkan·Metal 넓게, GLES·D3D11 SM5 16), 머티리얼 텍스처 = (pass, 퍼뮤테이션, 형식)별 `형식 상한 − 렌더러 합계`, 모바일 형식에서 넘으면 그 형식 쿠킹 오류. 16장을 넘으려면 공유 샘플러(MSR13)가 필요하다 | 개정 1의 위험(일반 PBR 머티리얼도 무거운 퍼뮤테이션에서 쿠킹 오류)을 데스크톱에서 없앤다. UE도 원문은 하나이고 플랫폼 샘플러 상한을 컴파일 환경에 넣으며(`RE/Private/ShaderCompiler/ShaderCompiler.cpp:3945–3946`), OpenGL은 16 초과가 오류다(`Engine/Source/Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:868–877`). Metal 샘플러 상한은 16이다(`Engine/Source/Runtime/Apple/MetalRHI/Private/MetalPipeline.h:47`) | Renderer 1·7.4·13.1·19.1절, RQ22·RQ23, 이 문서 1·7.4·13·15·16·17절 |
| D4 `AlphaHoldout` | Renderer 14.2절 목록에 **후속 값**으로 둔다. 들일 때 `Unlit` 전용 검사·Holdout 투명 pass·알파 전파 후처리를 함께 정한다. 그 전에는 `Unsupported` 진단이고 다른 블렌드 모드로 바꾸지 않는다 | UE 5.8 활성 값(`RE/Classes/Engine/EngineTypes.h:253`), `Unlit` 강제(`RE/Private/Materials/MaterialShared.cpp:6378–6382`), 전용 pass와 블렌드(`RR/Private/TranslucentRendering.cpp:2361–2364, 2482–2490`). UE 구조는 따르되 단계를 미룬다. 대체 금지 원칙 | Renderer 10.2·14.2절, 이 문서 10.3절 |
| D5 거르기 키 | pass 셰이더 거르기 함수는 `ShouldCompile(const MaterialShaderTraits&, VertexInputTypeId, PermutationId, FormatId)` 하나로 통일한다. 마스크·WPO·PDO처럼 `MaterialShaderTraits`가 정하는 값은 pass 퍼뮤테이션 차원에서 뺐다 | UE도 머티리얼 요약 구조체 전체를 거르기에 넘긴다(`RE/Public/MaterialShared.h:1152–1271`, `RE/Private/Materials/MeshMaterialShader.cpp:178–186`). 머티리얼이 정하는 값을 차원으로 두면 반을 거르기로 버리는 차원이 생긴다 | Renderer 3.2·14.3·14.5·15.3절, RQ13, 이 문서 8.3절 |
| D6 용어 | "static switch 차원"이 아니라 "static 변형"(변형마다 따로 번역하고 셰이더 집합 id가 다르다)이다. pass 퍼뮤테이션과 곱해지는 수는 같다 | 9.5·8.6절 | Renderer 14.6절, 이 문서 10.1절 |
| D7 경로 구분 | Renderer pass 표에 "경로" 열을 둔다: `GBuffer` Deferred, `ForwardOpaque`·`DepthAux` Forward+, 나머지 공통. 정의 이름은 `RENDER_PATH_DEFERRED`·`RENDER_PATH_FORWARD_PLUS`(공통 pass는 둘 다 0). 기능 등급 이름은 형식 정의(MSR9) | Rocks는 view마다 경로를 고르므로(계획 11.1절) pass job이 경로를 정한다. UE는 플랫폼당 shading path 하나다(`RE/Public/Materials/MaterialExpressionShadingPathSwitch.h:50`) | Renderer 14.3·14.6절, 이 문서 6.3·8.2·10.1절 |
| D8 마스크 전용 함수 | depth·그림자 pass는 `EvaluateMaterialOpacityMask`만 부른다. PDO를 쓰면 `EvaluateMaterialPixelDepthOffset`도 부르고 PS를 둔다 | UE depth PS는 픽셀 입력 전체를 계산하고 clip만 쓴다(`SH/Private/DepthOnlyPixelShader.usf:42–44, 96`). Rocks는 진입점마다 IR을 따로 내므로 pass별 텍스처 수가 그 pass가 부르는 함수 기준으로 정확하다(Renderer 7.4절). PDO는 UE도 depth PS 조건이다(`RR/Private/DepthRendering.cpp:171`) | Renderer 14.1·14.3절, 이 문서 7.3절 |
| D9 영역 이름 | Renderer 이름(`DeferredDecal`·`UserInterface`)을 그대로 쓴다. 한국어 용어도 Renderer와 같게 "영역"·"블렌드 모드"로 통일했다(개정 1의 "도메인"·"혼합 방식") | 같은 개념은 같은 이름. UE 이름(`MD_UI` 등)은 대응표에만 | 이 문서 전체 |

**D 밖에서 맞춘 것.**

| 항목 | 결정 | 반영 위치 |
|---|---|---|
| Render Path Switch와 깊이 일치 | 공통 depth pass(Default 입력)와 경로 전용 base pass(경로 입력)가 다른 WPO·마스크·PDO를 쓰면 base pass 깊이 `Equal`이 깨진다. 이 셋에 닿는 Render Path Switch는 검증 오류(V15) | 이 문서 5.4·6.3절, Renderer 14.3절 |
| 생성 함수와 pass 대응 | Renderer가 함수별로 부르는 pass를 표로 둔다. 노드 미리보기 pass `MaterialPreview`를 Renderer pass 표에 넣었다 | Renderer 14.1·14.3절, 이 문서 11.4절 |
| 마스크 기준값 | `MATERIAL_OPACITY_MASK_CLIP` 정의(기본 속성, static 변형)다. Renderer의 "머티리얼 상수" 표현을 고쳤다 | Renderer 14.1절, 이 문서 7.3·9.5절 |
| 노멀 공간 | 그래프 노멀은 tangent 공간 기본, 내보낼 때 world 정규화 | 이 문서 10.3절, Renderer 14.1절 |
| 조회 키 | (pass 셰이더 타입, 정점 입력 타입, 셰이더 집합 id, 퍼뮤테이션, 단계), SX5 포함 | Renderer 14.5절, 이 문서 8.6절 |
| pass별 렌더러 SRV 합계 | `GBuffer` pass 0–2, depth 계열 0을 더했다 | Renderer 7.4절 |
| 단계 선후 | Renderer RS0 전에 MS1–MS3과 MS4 일부, MS-T6은 RS2와 함께, RS5는 MS7(SF2), RS9는 MS8(SF3) | Renderer 17절, 이 문서 10.3·15.1절 |
| 모듈 의존 | MaterialCook 표 행에 `RendererShaders`를 더했다(의존 그림과 맞춤) | 이 문서 3.2절 |
| 셰이더 결정 4 | MetaCompiler 방식으로 확정(셰이더 시스템 개정 12). `ShaderParameterLayout` 요구(MSR4)를 셰이더 시스템 13절에 연결 | 이 문서 1·15.1·16·17절(MD3) |
| 기본 머티리얼 대체 표시 | Renderer는 덧칠하지 않고 대체 수를 계수로 남긴다 | Renderer 14.7·16.2절, 이 문서 10.1절 |
