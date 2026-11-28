# 렌더러(Renderer) 설계 — 14. 렌더러가 머티리얼에 요구하는 계약

[목차](README.ko.md)

## 14. 렌더러가 머티리얼에 요구하는 계약

이 절은 렌더러 쪽 요구만 정한다. 머티리얼 그래프·IR·컴파일러·인스턴스·에디터는 [머티리얼 시스템](../MaterialSystem/README.ko.md)(개정 2)과 계획 14.5–14.9절이 맡는다. 겹치는 곳은 14.6절 경계표로 나눈다. 두 문서의 다른 점은 머티리얼 시스템 10.4절 "정합 결과"로 맞췄다(이 문서 개정 3).

### 14.1 표면 속성(`SurfaceAttributes`)과 머티리얼 함수

머티리얼은 pass 셰이더가 부르는 함수를 생성한다(머티리얼 시스템 7.3절 생성 파일 계약). 생성 파일은 아래 함수를 **항상 모두** 내고 쓰지 않는 함수는 기본값을 돌려주므로, pass 원문은 함수가 있는지 `#if`로 묻지 않는다.

| 함수 | 단계 | 결과 | 부르는 pass(14.3절) |
|---|---|---|---|
| `EvaluateMaterialVertex(MaterialVertexInputs, inout MaterialVertexResult)` | VS | WPO(cm, world 공간 오프셋)·customized UV·보간 값 | 모든 메시 pass |
| `EvaluateMaterialVertexPrevious(MaterialVertexInputs, inout MaterialVertexResult)` | VS | 이전 프레임 WPO(계획 14.7절 current/previous 계약) | `DepthVelocity`·`Velocity`, velocity를 쓰는 투명 |
| `EvaluateMaterialSurface(MaterialInputs) → SurfaceAttributes` | PS | 표면 속성(아래 표) | `GBuffer`·`ForwardOpaque`·`Translucent`·`DepthAux` |
| `EvaluateMaterialOpacityMask(MaterialInputs)` | PS | 마스크 값(clip 기준값 빼기 전) | masked 머티리얼의 `Depth`·`DepthVelocity`·`ShadowDepth` |
| `EvaluateMaterialOpacity(MaterialInputs)` | PS | 반투명 coverage(`SurfaceAttributes.opacity`와 같은 값) | 투명 그림자·coverage만 필요한 pass(후속) |
| `EvaluateMaterialPixelDepthOffset(MaterialInputs)` | PS | PDO(cm) | PDO를 쓰는 머티리얼의 depth 계열 pass와 base pass(`SV_Depth`) |
| `EvaluateMaterialPreview(MaterialInputs)` | PS | 노드 미리보기 색 | `MaterialPreview`(에디터 전용) |

- **WPO는 `SurfaceAttributes` 필드가 아니다.** 정점 단계 값이므로 정점 함수 결과다(머티리얼 시스템 10.4절 D1). UE도 WPO는 픽셀 입력 구조체(`SH/MaterialTemplate.ush:371` `FPixelMaterialInputs`)가 아니라 정점 파라미터를 받는 함수이고(`SH/MaterialTemplate.ush:4051, 4073`), depth·velocity·base pass 정점 셰이더가 부른다(`SH/DepthOnlyVertexShader.usf:55`, `SH/VelocityShader.usf:73, 81`, `SH/BasePassVertexShader.usf:59, 264`).
- **마스크 전용 함수(머티리얼 시스템 10.4절 D8).** depth·그림자 pass는 표면 전체를 계산하지 않고 `EvaluateMaterialOpacityMask`만 부른다. UE는 depth PS가 픽셀 입력 전체를 계산한 뒤(`SH/DepthOnlyPixelShader.usf:42–44`) clip 함수만 쓰고(`:96`, `SH/MaterialTemplate.ush:4579`) 나머지는 하위 컴파일러의 불필요 코드 제거에 맡긴다. Rocks는 진입점마다 IR을 따로 내므로(머티리얼 시스템 6.2절) pass별 텍스처 수(7.4절)가 그 pass가 실제로 부르는 함수 기준으로 정확히 세어진다.
- **PDO.** PDO를 쓰는 머티리얼은 depth prepass와 base pass가 같은 함수로 `SV_Depth`를 써서 base pass의 깊이 `Equal`이 성립한다. UE도 PDO가 있으면 depth pass에 PS를 둔다(`R/DepthRendering.cpp:171`).

**`SurfaceAttributes` 필드.** 렌더러는 이 구조체만 읽는다.

| 필드 | 타입 | 뜻 | 읽는 곳 |
|---|---|---|---|
| `baseColor` | float3(선형) | albedo | GBufferC, 조명 |
| `metallic`·`specular`·`roughness` | float | PBR 값(specular 기본 0.5) | GBufferB, 조명 |
| `normal` | float3(world, 정규화) | 법선 맵 적용 뒤 | GBufferA, 조명 |
| `emissive` | float3(선형, 노출 전) | 자체 발광 | SceneColor |
| `ambientOcclusion` | float | 머티리얼 AO | GBufferC.a |
| `opacity`·`opacityMask` | float | 투명도·마스크(clip 기준값은 머티리얼 정의 `MATERIAL_OPACITY_MASK_CLIP`, 기본 속성이라 static 변형을 가른다) | 투명 블렌드·masked clip |
| `shadingModel` | uint(14.2절 번호) | BRDF 종류 | GBufferB.a, 조명 분기 |
| `customData0`·`customData1` | float | shading model 전용(clear coat 강도·roughness 등) | GBufferD |
| `tangent`·`anisotropy` | float3·float | 이방성(후속) | 후속 |

- 좌표·단위: world 공간은 1절 좌표계, 법선은 world 공간, 길이는 cm이다. 머티리얼 그래프의 노멀 출력은 tangent 공간이 기본이고, 머티리얼 번역기가 내보낼 때 world 공간 정규화 값으로 바꾼다(머티리얼 시스템 10.3절).
- 색 공간: 텍스처 sRGB 해석은 머티리얼·텍스처 쪽이고 `SurfaceAttributes` 값은 선형이다.

### 14.2 shading model·블렌드 모드·영역

| 종류 | 값(번호) | 단계 | UE 5.8 대응 |
|---|---|---|---|
| `ShadingModel` | `Unlit`(0)·`DefaultLit`(1) | RS1·RS2 | `EN/Classes/Engine/EngineTypes.h:707–728` |
| 〃 | `ClearCoat`·`Subsurface`·`SubsurfaceProfile`(skin)·`TwoSidedFoliage`·`Cloth`·`Eye`·`ThinTranslucent`·`SingleLayerWater` | RS9(계획 14.4절 고급 재질) | 같다. UE의 `PreintegratedSkin`은 계획 14.4절 skin/profile을 `SubsurfaceProfile` 하나로 받는다는 제안에 따라 넣지 않는다(결정 RN5와 함께 확정) |
| 〃 | `Hair` | P3(계획 11.1절) | 같다 |
| `BlendMode` | `Opaque`·`Masked` | RS0 | `EN/Classes/Engine/EngineTypes.h:245–248` |
| 〃 | `Translucent`·`Additive`·`Modulate`·`AlphaComposite`(premultiplied) | RS5 | 같다 |
| 〃 | `AlphaHoldout` | 후속(Holdout pass와 함께), 그 전에는 `Unsupported` | `EN/Classes/Engine/EngineTypes.h:253`. UE는 Unlit 전용으로 검사하고(`EN/Private/Materials/MaterialShared.cpp:6378–6382`) source alpha 모양으로 뒤를 가리는 블렌드와 전용 Holdout pass를 쓴다(`R/TranslucentRendering.cpp:2361–2364, 2482–2490`, `R/BasePassRendering.cpp:2539`) |
| `MaterialDomain` | `Surface` | RS0 | `EN/Public/MaterialDomain.h:12–30` |
| 〃 | `DeferredDecal`·`LightFunction`·`PostProcess`·`Volume`·`UserInterface` | 각 기능 단계, 그 전에는 `Unsupported`(계획 14.7절) | 같다(`MD_RuntimeVirtualTexture`는 deprecated라 제외) |

- **번호표(결정 RN5).** shading model 번호는 GBuffer 4 bit이므로 이 문서가 번호표(`ShadingModel`·`BlendMode`·`MaterialDomain`)의 정의 권한을 갖고 머티리얼 스키마가 참조한다. 코드 위치는 C++ `RenderData`, HLSL `/Engine/Shared/` 공용 헤더다(3.2절). 이름 표시·UI·검증은 머티리얼 쪽이다.
- **`AlphaHoldout`(머티리얼 시스템 10.4절 D4).** UE 5.8의 활성 값이라 목록에 남기되 후속이다. 들일 때 UE처럼 `Unlit` 전용 검사, Holdout 투명 pass(10.2절), 알파를 전하는 후처리를 함께 정한다. 그 전에는 머티리얼 쪽이 `Unsupported`로 진단하고 다른 블렌드 모드로 바꾸지 않는다. UE의 Substrate 전용 값(`TranslucentColoredTransmittance`, dual source blend 요구)과 숨은 별칭은 넣지 않는다(`EN/Classes/Engine/EngineTypes.h:254–258`).
- 모든 shading model은 Forward 평가와 Deferred 인코딩을 둘 다 가져야 한다(계획 14.4절). 한쪽만 있으면 그 모델은 지원표에 "경로 제한"으로 표시하고 그 경로 쿠킹이 오류다.

### 14.3 pass별 셰이더 퍼뮤테이션 요구

렌더러는 머티리얼 하나에 대해 다음 pass 셰이더를 요구한다. pass 셰이더 타입의 거르기 함수는 `ShouldCompile(const MaterialShaderTraits&, VertexInputTypeId, PermutationId, FormatId)`다(머티리얼 시스템 8.3절, 10.4절 D5). `MaterialShaderTraits`는 영역·블렌드 모드·양면·shading model 집합·투명 pass 종류·velocity 쓰기·그림자 caster·WPO·이전 WPO·PDO·마스크 사용·필요 기능(scene texture 읽기 등)·custom 출력을 담는다. UE도 pass 셰이더 거르기가 머티리얼 요약 구조체 전체를 받는다(`EN/Public/MaterialShared.h:1152–1271`, `EN/Private/Materials/MeshMaterialShader.cpp:178–186`).

"경로" 열은 그 pass를 어느 렌더 경로가 쓰는지다. 경로 전용 pass는 머티리얼 Render Path Switch의 그 경로 입력을, 공통 pass는 Default 입력을 쓴다(머티리얼 시스템 6.3절 3단계, 10.4절 D7).

| `MeshPass` | 경로 | 셰이더 | 부르는 머티리얼 함수(14.1절) | 대상 머티리얼 |
|---|---|---|---|---|
| `Depth` | 공통 | VS(+ masked 또는 PDO면 PS) | 정점, 마스크, PDO | Opaque·Masked |
| `DepthVelocity` | 공통 | VS·PS | 정점(현재·이전), 마스크, PDO | 이동 물체 |
| `DepthAux` | Forward+ | VS·PS | 정점, 표면(normal·roughness), 마스크, PDO | Forward+ 보조 출력 view |
| `GBuffer` | Deferred | VS·PS | 정점, 표면, PDO | Opaque·Masked |
| `ForwardOpaque` | Forward+ | VS·PS | 정점, 표면, PDO + 조명 | Opaque·Masked |
| `ShadowDepth` | 공통 | VS(+ masked 또는 PDO면 PS) | 정점, 마스크, PDO | 그림자 caster |
| `Translucent` | 공통 | VS·PS | 정점(+ 이전, velocity 쓰기면), 표면 + 조명 | 투명 블렌드 |
| `Velocity` | 공통 | VS·PS | 정점(현재·이전), 마스크 | MSAA view 등 별도 velocity |
| `Distortion` | 공통 | VS·PS | 굴절 입력(후속) | 굴절 머티리얼 |
| `MaterialPreview` | 공통(에디터 전용) | VS·PS | 미리보기 | 노드 미리보기 번역(머티리얼 시스템 11.4절) |
| `Decal` 등 | 각 단계 | — | — | 각 단계 |

- **경로 정의 이름.** 렌더러가 pass job에 `RENDER_PATH_DEFERRED`·`RENDER_PATH_FORWARD_PLUS`를 넣는다. 경로 전용 pass는 그 경로 하나가 1이고, 공통 pass는 둘 다 0이다. 생성 원문의 Render Path Switch는 이 둘로 `#if`를 고른다. 기능 등급 정의 이름은 형식 정의가 정한다(머티리얼 시스템 16절 MSR9).
- **깊이 일치.** 공통 pass(Default 입력)와 경로 전용 base pass(경로 입력)가 다른 WPO·마스크·PDO를 쓰면 base pass의 깊이 `Equal`(7.2·8.2절)이 깨진다. 그래서 Render Path Switch가 이 셋에 닿는 그래프는 머티리얼 검증 오류다(머티리얼 시스템 5.4절 V15).
- **퍼뮤테이션 차원(렌더러 쪽).** `GBuffer`: GBuffer profile {기본, +D, +E, +D+E}. `ForwardOpaque`·`Translucent`: 정적 조명 정책 {없음, 라이트맵, 프로브} × 동적 GI 표본 {없음, 있음} × 보조 출력 {없음, 있음}(ForwardOpaque만) × DBuffer {0, 1}(ForwardOpaque만) × 광원 함수·IES atlas {0, 1}(RS9). 마스크·PDO·WPO 사용은 `MaterialShaderTraits`가 정하므로(생성 원문의 `MATERIAL_USES_*` 정의) 퍼뮤테이션 차원이 아니다. 머티리얼이 정하는 값을 차원으로 두면 거르기로 반을 버리는 차원이 생기기 때문이다. 광원 수·그림자 유무·광원 종류는 동적 분기이고 퍼뮤테이션이 아니다(계획 14.1절 "runtime parameter는 불필요한 variant를 생성하지 않는다").
- MSAA sample 수는 셰이더 퍼뮤테이션이 아니라 파이프라인 상태다(alpha-to-coverage 포함).
- 퍼뮤테이션 수 보고(계획 14.1절 platform×path×sample count×material×skinning)는 쿠커가 이 표로 계산한다.

### 14.4 direct lighting 평가

- BRDF(`EvaluateDirectLighting`)·광원 감쇠·그림자 표본은 렌더러 소유 HLSL(`RendererShaders` 셰이더 원문)이고 머티리얼이 바꾸지 않는다. Forward+ base pass·투명·Deferred 조명 pass가 같은 원문을 쓴다.
- shading model별 BRDF 분기는 렌더러 원문 안 `switch(shadingModel)`이다. 머티리얼 pass 셰이더(`ForwardOpaque`·`Translucent`·`GBuffer`)는 머티리얼 shading model이 정적으로 하나이면 생성 원문의 `MATERIAL_SINGLE_SHADING_MODEL`로 그 분기만 컴파일한다(머티리얼 시스템 7.3절). `DeferredLightingPass`는 머티리얼이 없는 전역 셰이더라 모든 분기를 가진다.

### 14.5 렌더러가 머티리얼에서 읽는 런타임 값

- 셰이더 집합: (pass 셰이더 타입, 정점 입력 타입, 머티리얼 셰이더 집합 id, 퍼뮤테이션 ID, 단계)로 조회(RenderBase 9.2절 키 + SX4 + SX5, 머티리얼 시스템 8.6절).
- 파라미터 묶음: 머티리얼 인스턴스의 상수 블록과 텍스처 `RenderHandle` 목록. 텍스처는 스트리밍 바인딩 snapshot으로 그래프 구성 때 해석한다(TextureStreaming 5.3절).
- 머티리얼 정적 속성: 쿠킹 데이터의 `MaterialShaderTraits`(머티리얼 시스템 8.3절)를 그대로 읽는다. 블렌드 모드·영역·shading model·양면·투명 pass 종류·velocity 쓰기·그림자 caster·WPO 사용이 그리기 항목 캐시 키에 들어간다.
- 셰이더 집합 교체(hot reload, RenderBase 9.8절)는 프레임 경계에서 해당 그리기 항목 캐시를 무효화한다.

### 14.6 경계표

| 주제 | 렌더러(이 문서) | 머티리얼 시스템 |
|---|---|---|
| `SurfaceAttributes` 필드와 의미 | 정한다 | 그래프 출력 pin을 이 필드로 낮춘다 |
| shading model 번호·BRDF·GBuffer 인코딩 | 정한다 | 선택 UI·이름·검증 |
| 블렌드 모드·영역 목록 | 렌더러가 지원하는 값과 단계 | 머티리얼 속성으로 노출, 미지원 값 진단 |
| pass 셰이더 타입·퍼뮤테이션 차원(렌더러 쪽) | 정한다(`RendererShaders`) | 머티리얼 static 변형(변형마다 셰이더 집합이 따로, 머티리얼 시스템 9.5절)과 곱한다. 거르기 입력 `MaterialShaderTraits`를 준다 |
| 머티리얼이 내는 함수(정점·표면·마스크·PDO 등) | 어느 pass가 부르는지(14.1·14.3절) | 생성 파일 계약(머티리얼 시스템 7.3절) |
| 렌더 경로·기능 등급 정의 이름 | 경로 정의 이름을 정한다(14.3절). 기능 등급은 형식 정의 | 생성 원문의 `#if`에 쓴다 |
| (pass, 퍼뮤테이션, 형식)별 머티리얼 텍스처 상한 | `형식 상한 − 렌더러 합계` 표(7.4절) | 계산해 진단·쿠킹 검사에 쓴다 |
| 머티리얼 셰이더 집합 키·쿠킹 단위 | 조회 키만 요구 | 정한다 |
| 그래프·IR·HLSL 생성·인스턴스·파라미터 갱신 | — | 정한다 |
| scene texture 읽기 노드 | 경로별 가능 여부(Deferred GBuffer, Forward+ 보조 출력) | 노드 검증(계획 14.7절 "Forward에서 없는 GBuffer를 읽는 node를 정상 지원으로 표시하지 않는다") |

### 14.7 머티리얼 시스템 요구(MRD1–MRD12) 처리

머티리얼 시스템 16절이 이 문서에 낸 요구다.

| ID | 요구 | 처리 |
|---|---|---|
| MRD1 | WPO를 정점 함수 결과로 분리(D1) | 반영. 14.1절 함수 표, `SurfaceAttributes`에서 뺐다 |
| MRD2 | 번호표 정의 위치(D2) | 반영. C++ `RenderData`, HLSL `/Engine/Shared/`(3.2·14.2절) |
| MRD3 | RN13 재검토 조건(D3) | 반영. RN13을 사용자 결정으로 형식별 상한으로 개정(7.4절, 19.1절). 측정 MM5는 GLES·D3D11 형식의 오류 비율을 보는 데 쓴다 |
| MRD4 | `AlphaHoldout` 처리(D4) | 반영. 목록에 두고 후속, 그 전에는 `Unsupported`(14.2절) |
| MRD5 | 거르기 인자 `MaterialShaderTraits`(D5) | 반영(14.3·14.5·15.3절) |
| MRD6 | view 상수의 translated world 기준과 절대 위치 표현 | 셰이더 기본 위치는 카메라 상대 world(translated world)다. 절대 위치는 상위(큰 격자 위치 float3)·하위(float3) 두 부분이고 view 상수에 카메라 이동 보정 상위·하위를 둔다(5.1절, `View` slot 15.2절). 머티리얼 `WorldPosition` 타입(`Float3` 둘)은 이 두 부분과 같은 표현이다. 격자 크기는 RS0 완료 기준 (e)에서 정한다 |
| MRD7 | instance 데이터 계약 | GPU Scene 인스턴스 항목은 현재·이전 local-to-world(상위·하위), bounds, 인스턴스 표시다(4.2·5.1절). 머티리얼 `Instance` 빈도 입력(인스턴스별 custom data)의 슬롯 수와 읽기 함수는 RS0의 GPU Scene 배치와 함께 정한다 |
| MRD8 | 기본 머티리얼 대체 표시 | 렌더러는 덧칠하지 않고 기본 머티리얼의 셰이더로 그린다. 대체 수를 계수(16.2절)로 남긴다. 화면 표시는 에디터 진단(머티리얼 시스템 11.3절)이 맡는다 |
| MRD9 | 경로 전용·공통 구분과 정의 이름(D7) | 반영(14.3절 "경로" 열, `RENDER_PATH_*`) |
| MRD10 | Deferred·Forward+ 기준 영상 허용 오차 | RS2 완료 기준 (a)에서 RS0 기준 영상과 같은 채널 ±8·2%로 시작한다. GBuffer 양자화(법선 10 bit, base color 8 bit sRGB)로 넘는 차이가 측정되면 그 항목만 RS2에서 정한다 |
| MRD11 | 마스크 전용 함수(D8) | 반영(14.1·14.3절) |
| MRD12 | DeferredDecal 속성, 반투명 scene color·depth 입력, PostProcess 위치 | DeferredDecal이 쓰는 속성은 decal 단계(RS7)에서 GBuffer 배치(7.1절) 기준으로 정한다. 반투명의 scene color·depth 읽기는 RS5에서 distortion과 같은 SceneColor 복사본·깊이 SRV로 주고, `MaterialShaderTraits`의 필요 기능으로 켠다(SRV 예산 7.4절). PostProcess 영역의 삽입 위치는 `PostChain` 자리(11.2절)에 맞춰 계획 26절 후처리 catalog 스펙이 정한다 |
