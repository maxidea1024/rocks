# 머티리얼 시스템(MaterialSystem) 설계

## 7. 코드 생성

### 7.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 삽입 방식 | `%{name}` 이름 있는 자리 문자열 템플릿이다. 형식별로 템플릿을 캐시하고 파일 감시로 무효화한다. 자리 이름을 정렬해 해시한다 | `RE/Private/Materials/StringTemplate.h:13–29`, `MaterialSourceTemplate.h:12–52`, `MaterialSourceTemplate.cpp:93–146` |
| 템플릿 | `SH/Private/MaterialTemplate.ush`. 구조체 `FPixelMaterialInputs`(`:371`)·`FMaterialPixelParameters`(`:427`)·`FMaterialVertexParameters`(`:677`), 계산 `CalcPixelMaterialInputs`(`:4333`)·`CalcMaterialParametersEx`(`:4664`), 자리 `material_declarations`(`:360`)·`pixel_material_inputs`(`:373`)·`calc_pixel_material_inputs_*`(`:4338–4466`) | 같은 파일 |
| 생성 파일 경로 | `/Engine/Generated/Material.ush`를 공유 컴파일 환경의 가상 경로 표에 등록한다 | `RE/Private/Materials/MaterialShared.cpp:3573–3575` |
| pass 셰이더 연결 | pass 원문이 `/Engine/Generated/Material.ush`를 include한다(`SH/Private` 아래 63개 파일). 예: `SH/Private/BasePassPixelShader.usf:94`, `DepthOnlyPixelShader.usf:12` | grep 결과 |
| 두 번역기 공통 | 기본 번역기와 IR 번역기가 같은 템플릿·같은 가상 경로를 쓰므로 pass 원문은 번역기를 모른다 | `MaterialShared.cpp:3664–3678` |

### 7.2 Rocks 파일 구조

```text
/Engine/Private/Material/MaterialCommon.rsh     정적. 문맥 구조체·보조 함수, 생성 파일 include
SurfaceAttributes 정의(Renderer 소유)           Renderer 14.1절. MaterialCommon.rsh가 include
/Engine/Generated/Material.rsh                  job마다 연결되는 생성 파일(번역 결과)
/Engine/Generated/VertexInput.rsh               정점 입력 타입 원문(RenderBase 10.2절)
pass 셰이더(.rsf, Renderer 스펙)                 MaterialCommon.rsh와 VertexInput.rsh를 include
```

- **템플릿 치환을 하지 않는다.** 생성 파일은 정해진 이름의 정의·자원 선언·함수만 담고, 정적 원문이 그것을 부른다. 텍스트 치환이 없으므로 템플릿 버전 hash가 필요 없고, 정적 원문의 내용은 전처리 결과로 job 키에 들어간다(셰이더 시스템 10절 7항).
- **연결.** 메인 프로세스 전처리기가 job마다 `/Engine/Generated/Material.rsh`를 번역 결과의 메모리 텍스트로 잇는다. 정점 입력 타입의 `VertexInput.rsh` 연결(RenderBase 10.2절)과 같은 장치다. 머티리얼이 없는 셰이더 타입(엔진 전역 셰이더)이 이 경로를 include하면 전처리 오류다.
- **셰이더 맵 키.** 같은 pass 셰이더라도 머티리얼마다 원문이 다르다. 그래서 셰이더 맵 키와 런타임 조회 키에 머티리얼 셰이더 집합 id가 들어간다(8.6절, 16절 MSR2).

### 7.3 생성 파일 계약

생성 파일은 아래 정의와 함수를 **항상 모두** 낸다. 쓰지 않는 함수는 기본값을 돌려준다. 그래서 pass 원문이 머티리얼마다 `#if`로 함수가 있는지 묻지 않는다. 예외는 custom 출력 함수로, 머티리얼이 정의한 것만 낸다(pass가 요구한 custom 출력이 없는 머티리얼은 거르기에서 빠진다, 8.3절).

| 정의·함수 | 뜻 |
|---|---|
| `MATERIAL_DOMAIN_SURFACE` 등 영역 하나 = 1 | 영역 |
| `MATERIAL_BLEND_OPAQUE`·`_MASKED`·`_TRANSLUCENT`·`_ADDITIVE`·`_MODULATE`·`_ALPHA_COMPOSITE` 하나 = 1 | 블렌드 모드(Renderer 14.2절) |
| `MATERIAL_TWO_SIDED`, `MATERIAL_USES_WPO`, `MATERIAL_USES_PREVIOUS_WPO`, `MATERIAL_USES_PIXEL_DEPTH_OFFSET`, `MATERIAL_USES_OPACITY_MASK` | 기능 표지(0·1) |
| `MATERIAL_SHADING_MODELS`, `MATERIAL_SINGLE_SHADING_MODEL` | 쓰는 shading model 집합 비트(번호는 Renderer 14.2절)와, 하나뿐이면 그 번호. Renderer가 그 분기만 컴파일한다(Renderer 14.4절) |
| `MATERIAL_NUM_TEXCOORDS`, `MATERIAL_NUM_INTERPOLANT_VECTORS`, `MATERIAL_NUM_CUSTOMIZED_UVS` | 정점 입력에서 읽을 UV 채널 수, 머티리얼이 쓰는 보간 `float4` 수 |
| `MATERIAL_OPACITY_MASK_CLIP` | 마스크 기준값(기본 속성 상수) |
| `void EvaluateMaterialVertex(in MaterialVertexInputs c, inout MaterialVertexResult r)` | WPO(cm, world 공간 오프셋)·customized UV·보간 값 |
| `void EvaluateMaterialVertexPrevious(in MaterialVertexInputs c, inout MaterialVertexResult r)` | 이전 프레임 WPO(계획 14.7절 current/previous 계약). 문맥의 이전 시간·이전 instance 데이터로 평가한다 |
| `float3 EvaluateMaterialNormal(in MaterialInputs c)` | 대표 노멀(world, 정규화 — 6.11절). pass가 다른 함수보다 먼저 불러 `c.pixelNormal`을 채운다(UE `CalcPixelMaterialInputs` 순서) |
| `SurfaceAttributes EvaluateMaterialSurface(in MaterialInputs c)` | 표면 속성(Renderer 14.1절 함수 이름) |
| `float EvaluateMaterialOpacity(in MaterialInputs c)` | 반투명 coverage(`SurfaceAttributes.opacity`와 같은 값, 표면 전체 없이) |
| `float EvaluateMaterialOpacityMask(in MaterialInputs c)` | 마스크 값. depth·shadow pass가 표면 전체를 계산하지 않게 따로 둔다(10.4절 D8) |
| `float EvaluateMaterialPixelDepthOffset(in MaterialInputs c)` | PDO(cm) |
| `float4 EvaluateMaterialCustomOutput_<이름><i>(in MaterialInputs c)` | 머티리얼이 정의한 custom 출력의 번호 i(6.11절 MS2d3 — UE `<Fn><i>`). 정점 빈도 출력은 `(in MaterialVertexInputs c)`와 `EvaluateMaterialCustomOutputPrevious_<이름><i>`. 존재 정의 `MATERIAL_CUSTOM_OUTPUT_<NAME>`(출력 수)·`MATERIAL_HAS_CUSTOM_OUTPUT_<NAME><i>` |
| `float4 EvaluateMaterialPreview(in MaterialInputs c)` | 노드 미리보기 번역에서만 의미가 있다(11.4절) |

함수 사이에는 계산이 겹칠 수 있다(예: 마스크 계산과 표면 계산이 같은 텍스처를 읽는다). 진입점마다 필요한 값만 내므로, 한 pass가 쓰는 함수는 필요한 계산만 한다. 한 pass에서 두 함수를 부르면 겹치는 계산의 제거는 하위 컴파일러(DXC·FXC)의 CSE에 맡긴다.

**문맥 구조체**(`MaterialCommon.rsh`):
- `MaterialVertexInputs`·`MaterialInputs`의 칸은 문맥 값 표에서 만든 `MaterialContext.rsh`가 정한다(6.9절). 고정 칸은 `MaterialInputs.svPosition`(문맥 값 표의 SvPosition 칸 — 6.13절 MS2f2. 고정 칸이라 접근자는 공급 표지와 상관없이 늘 칸을 읽는다)·`interpolants`, `MaterialVertexResult.worldPositionOffset`·`customizedUvs`·`interpolants`다.
- `MaterialVertexInputs`: world·translated world 위치, local 위치, 법선·tangent 기저, UV 채널, 정점 색, instance 데이터 index, 이전 프레임 변환 접근.
- `MaterialInputs`(Renderer 14.1절 이름): 화면 위치, translated world 위치, 기저, 보간된 UV·정점 색·custom 보간 값, 앞면 여부, view 상수 접근.
- 채우는 쪽은 pass 원문과 정점 입력 타입이다. RenderBase 10.2절이 "첫 메시 셰이더 착수 전에 정한다"고 남긴 `.rsh` 함수 인터페이스를 이 구조체로 정한다(16절 MRB3). UE 템플릿의 픽셀·정점 파라미터 구조체(`SH/Private/MaterialTemplate.ush:427, 677`)와 역할이 같다.

### 7.4 바인딩 규약(셰이더 시스템 7.2절)과의 관계

| 7.2절 항 | 머티리얼 생성 원문 규칙 |
|---|---|
| 1항 선언 | register·`[[vk::*]]`를 쓰지 않는다. 이름 없는 전역 uniform 금지에 맞춰 상수는 이름 있는 `cbuffer MaterialConstants`로 낸다 |
| 2항 루트 상수 | 머티리얼은 루트 상수를 쓰지 않는다. 그리기별 상수는 Renderer 몫이다 |
| 3항 슬롯 정규화 | 생성 cbuffer의 정규화 layout 항목에 동적 layout 서명을 단다(RenderBase SX8의 "파라미터 구조체 identity" 자리, 16절 MRB2) |
| 4항 상한 | 텍스처는 (pass, 퍼뮤테이션, 형식)별 `형식 상한 − 렌더러 합계`(Renderer 7.4절, 결정 MD4)로 검사한다. 번역은 원문을 형식마다 바꾸지 않고 프로필 형식마다 진단만 내며, 넘는 형식의 job만 쿠킹 오류다. 자기 샘플러를 쓰는 텍스처는 형식의 샘플러 상한에도 묶인다(9항). 상수 버퍼·보간 수는 (프로필 공통 상한 − pass 예약 수)로 검사하고(V9), job에서 형식 상한으로 다시 검사한다 |
| 5항 상수 버퍼 layout | `float4` 경계 정렬, 16 byte slot 안에 float·int·uint를 섞지 않음, 배열은 `float4` 배열, bool은 uint 비트. 머티리얼 컴파일러가 이 규칙으로 layout 표를 만들고, 워커가 리플렉션 오프셋과 대조한다 |
| 6항 행렬 | 변환 노드는 `mul(v, M)`으로 내고 행렬은 행 우선 상수다 |
| 7항 단계 사이 변수 | 머티리얼 보간 값은 pass가 준 기준 번호부터 `TEXCOORDn`을 이어 쓴다(n ≤ 15, GLES는 형식 상한). struct 멤버 순서는 n 오름차순이다 |
| 9항 텍스처·샘플러 짝 | 텍스처 `MaterialTexture<k>`마다 `MaterialTexture<k>Sampler`를 낸다(짝 규칙 그대로, 결정 MD4). 공유 샘플러(16절 MSR13)가 생기면 텍스처 slot의 샘플러 출처(자기 샘플러·공유 Wrap·공유 Clamp, UE `ESamplerSourceMode`)로 고른다. 공유 출처면 `MaterialTexture<k>Sampler`를 내지 않고 `SharedWrapSampler`·`SharedClampSampler`(셰이더 시스템 개정 13, 7.2절 9항)로 샘플링한다. slot은 (텍스처, 샘플러 출처)마다 하나라 한 텍스처를 두 출처로 샘플링하면 slot이 둘이다(6.10절 텍스처 표) |
| 결정 7(경고) | 생성 원문도 경고 0이 기준이다. 생성기가 낸 경고는 생성기 버그로 본다 |

**레이아웃 서명.** 머티리얼 상수 블록의 정규 layout 표(멤버 이름·타입·offset·크기, 텍스처·샘플러 순번)의 SHA-256이다. job 키 8항(셰이더 시스템 10절)에 들어가고, 쿠킹 셰이더 집합 항목에도 기록되어 실행 때 상수 블록 layout과 대조한다(RenderBase 9.3절·SX6 서명 검증과 같다).

**스트리밍 텍스처 규칙(TextureStreaming 5.3절).**
- Texture Size 노드는 텍스처 slot의 **논리 크기** 상수(상수 블록)를 읽는다. 논리 크기는 바뀌지 않으므로 스트리밍 교체 때 상수 블록을 다시 쓰지 않는다.
- 명시 LOD 샘플과 정수 Load는 TextureStreaming의 상주 mip helper를 거쳐 생성한다. helper를 쓸 수 없는 식이면 그 텍스처 slot에 "전체 상주 전용" 표지를 달아 번역 결과로 넘긴다(TextureStreaming이 비스트리밍으로 돌린다).
- 도함수 기반 샘플은 변환하지 않는다.

### 7.5 임의 HLSL 노드

계획 14.7절대로 선택 고급 노드다. 기본 에디터 Gate를 막지 않는다.
- **선언.** 입력(이름·타입), 출력(이름·타입), 본문 텍스트, include 목록(허용 목록의 가상 경로만), 요구 기능(HLSL 2021·SM6 기능 등).
- **생성.** 노드마다 `MaterialCustom_<NodeId 앞 16 hex>(입력…, out 출력…)` 함수로 낸다. 본문은 함수 몸체에만 들어간다.
- **금지(V13).** 자원 선언·register·전역 변수·`#include`(include는 선언 목록으로만)·`#pragma`·임의 `discard`/`clip`(선언한 경우만 허용). 검사는 두 겹이다: 토큰 수준 텍스트 검사, 그리고 job 리플렉션(머티리얼 layout 밖 자원이 생기면 오류).
- **형식 거르기.** SM6 기능처럼 형식 정의에 없는 기능을 요구하면 그 형식에서 쿠킹 오류다. HLSL 2021은 셰이더 결정 2대로 형식 정의 기본값과 셰이더 타입 선택을 따른다: `D3D11_SM5`는 SPIRV-Cross 경로로 컴파일되고, GLES 형식은 기본값이 끔이라 pass 셰이더 타입이 켜지 않는 한 HLSL 2021 문법 노드는 GLES에서 쿠킹 오류다.
- CPU bake·ray profile에서는 미지원이다(6.7절).
- UE 대응은 `RE/Public/Materials/MaterialExpressionCustom.h:74–104`이며, UE는 자원 접근과 include를 막지 않는다.

### 7.6 진단과 원문 위치

- 생성 원문의 `#line`은 가상 경로 `/Engine/Generated/Material.rsh`만 쓴다(셰이더 시스템 10절 경로 누출 금지).
- 번역 결과에 **원문 위치 지도**(생성 줄 범위 → NodeId·PinKey)를 둔다(계획 14.6절 SourceMap). job 오류의 줄 번호는 이 지도로 노드 위치 진단으로 바뀐다.
- 에디터의 생성 원문 보기(계획 14.5절 "generated HLSL")는 이 파일 텍스트를 그대로 보여 준다.

### 7.7 MS2a 생성 원문 세부

- **파일.** 생성 파일 `/Engine/Generated/Material.rsh`는 7.3절 정의를 먼저 내고, 진입점 함수를 7.3절 표 순서로 낸다. 정적 원문 `Engine/Shaders/Private/Material/MaterialCommon.rsh`는 문맥 구조체(`MaterialVertexInputs`·`MaterialVertexResult`·`MaterialInputs`), 보조 함수, `Shading/SurfaceAttributes.rsh` include, 생성 파일 include를 담는다(7.2절). MS2a의 문맥 구조체 칸은 7.3절 목록의 첫 칸(정점: translated world 위치, 픽셀: 화면 위치)과 `MaterialVertexResult.worldPositionOffset`뿐이고 MS2b가 문맥 값 표(19.3.2절)대로 채운다.
- **API.** `WriteMaterialSource(allocator, module, traits, asset, graph, out)`가 IR 모듈(6.8절)과 기본 속성으로 `MaterialGeneratedSource { text, locations, diagnostics, succeeded }`를 채운다. 생성할 수 없는 값(MS2a: Double — LWC 표현이 MS2b, `WorldPositionTile`)은 그 값의 원인 노드마다 `NotYetLowered` 하나를 진단하고 succeeded가 거짓이다.
- **정의.** 영역 `MATERIAL_DOMAIN_<영역>`과 블렌드 모드 `MATERIAL_BLEND_<모드>`는 모두 내고 해당하는 것만 1, 나머지는 0이다(pass 원문이 `#if`로 묻는다 — 정의가 없을 때와 0을 가르지 않는다). 블렌드 모드는 Rocks `BlendMode` 여덟 값 전부다(`_ALPHA_HOLDOUT`·`_TRANSLUCENT_COLORED_TRANSMITTANCE` 포함 — Renderer가 그리기 전에는 거르기가 뺀다). `MATERIAL_USES_WPO`·`MATERIAL_USES_PIXEL_DEPTH_OFFSET`은 그 진입점 결과가 있고 상수 0 벡터가 아니면 1이다. `MATERIAL_USES_PREVIOUS_WPO`는 이전 정점 진입점 결과가 상수 0이 아니면 1이다(6.9절). `MATERIAL_USES_OPACITY_MASK`는 블렌드 모드 Masked일 때 1이다. `MATERIAL_SHADING_MODELS`·`MATERIAL_SINGLE_SHADING_MODEL`은 Surface 입력을 번역하는 MS2d 전까지 DefaultLit(비트 1·번호 1 — `DefaultSurfaceAttributes`와 같다)이다. `MATERIAL_NUM_TEXCOORDS`는 진입점에서 닿는 TexCoord의 최대 색인 + 1(SpeedTree면 8), `MATERIAL_NUM_INTERPOLANT_VECTORS`는 ⌈보간 성분 수 / 4⌉, `MATERIAL_NUM_CUSTOMIZED_UVS`는 CustomizedUv 결과 수다(6.9절). 진입점별 사용 정의 `MATERIAL_<E>_USES_<F>`도 낸다. `MATERIAL_OPACITY_MASK_CLIP`은 `opacityMaskClipValue` 리터럴이다.
- **함수 몸체.** 진입점 함수마다 그 함수의 결과에서 닿는 값을 모듈 번호 순(6.6절 후위 순 — 피연산자가 먼저)으로 `const <타입> m<번호> = <식>;`로 낸다. 상수는 식 안에 리터럴로 바로 쓴다(지역 변수로 두지 않는다). 여러 함수가 같은 값을 쓰면 함수마다 다시 낸다(함수 사이 공유는 하위 컴파일러가 한다 — 7.3절). 결과가 없는 함수는 기본값을 돌려준다(Opacity·OpacityMask 1, PDO 0, Preview float4 0, Surface는 `DefaultSurfaceAttributes()`, Vertex는 결과를 건드리지 않는다).
- **진입점 → 함수.** Vertex 결과 `WorldPositionOffset` → `r.worldPositionOffset`. `EvaluateMaterialVertexPrevious`는 이전 정점 문맥의 번역 결과를 낸다(이전 프레임 문맥 값 — 6.9절). Vertex 결과 `CustomizedUv<i>` → `r.customizedUvs[i]`, 보간 값의 정점 쪽 → `r.interpolants[k].<성분>`(픽셀 함수는 `c.interpolants[k]`를 읽는다 — 6.9절 MS2b4a). Surface 결과 `AmbientOcclusion` → `s.ambientOcclusion`, Opacity·OpacityMask 결과는 각 함수와 `s.opacity`·`s.opacityMask`에 같은 값으로 쓴다. Surface 결과 `Refraction`·`SurfaceThickness`는 Renderer `SurfaceAttributes`에 칸이 없어 쓰지 않는다(칸이 생기면 Renderer 개정과 함께 쓴다 — 16절 요청).
- **리터럴.** float는 Core StringBuilder `{}`(최단 왕복, C runtime·locale 무관)로 쓰고, 숫자와 부호만이면 `.0`을 붙인다(`5` → `5.0`, 과학 표기 `1e+20`은 그대로). −0은 `-0.0`이다. 벡터는 `float3(a, b, c)`다. 정수는 10진이고 uint는 `u` 접미, bool은 `true`·`false`다. double(LWC) 값은 MS2b의 LWC 표현(5.3절 — 타일 + 오프셋)으로 내며 MS2a는 진단한다.
- **연산 철자.** 함수 연산은 HLSL 내장 함수 이름이다(`saturate`·`frac`·`lerp`·`rsqrt`·`trunc` 등). 연산자는 괄호로 감싼다(`(a + b)`). 그 밖에:
  - Clamp는 `min(max(x, lo), hi)`다(UE 기본 번역기와 같은 식).
  - `sign`은 HLSL이 int를 돌려주므로 값 타입으로 감싼다(`float3(sign(x))`). 나머지 `%`, Reciprocal `(1.0 / x)`, 행렬 곱 `mul(a, b)`다.
  - 벡터 조건 선택·논리곱·논리합은 HLSL 2021이 벡터 `?:`·`&&`·`||`를 막으므로 `MaterialSelect`·`MaterialAnd`·`MaterialOr`(`MaterialCommon.rsh` — `ROCKS_HLSL2021`이면 `select`·`and`·`or`, 아니면 연산자)로 낸다.
  - 빠른 역삼각 함수는 UE `FastMathThirdParty.ush`의 Eberly 다항식과 같은 `MaterialAcosFast`·`MaterialAsinFast`·`MaterialAtanFast`·`MaterialAtan2Fast`다(엔진 `FastAcos` 등은 다른 근사라 쓰지 않는다 — 결과가 UE와 같아야 한다).
  - HLSL에 없는 역쌍곡 함수는 `MaterialAcosh`·`MaterialAsinh`·`MaterialAtanh`(로그 식)다.
  - IsNan·IsInf·IsFinite는 fast-math가 지우지 않는 비트 검사 `MaterialIsNan`·`MaterialIsInf`·`MaterialIsFinite`다(6.2절). 0 나누기는 HLSL 그대로다(UE도 그대로).
- **`DynamicBranch`.** `<타입> m<번호>; [branch] if (c) { m = a; } else { m = b; }`로 낸다(UE 기본 번역기의 float `lerp` 형태와 다른 편차 — 6.8절). MS2a는 두 가지 값을 분기 앞에서 계산한다(값을 가장 좁은 블록에 두는 배치는 후속이다 — UE IR 빌더의 배치 단계, 6.1절).
- **원문 위치 지도.** 값을 낸 줄마다 (줄 번호 1부터, 원인 NodeId — 같은 값을 여러 노드가 만들면 가장 작은 NodeId, 6.8절)를 기록한다(7.6절). 컴파일러는 include 경로로 파일 이름을 알므로 `#line`을 내지 않는다.
- **결정성.** 같은 그래프는 같은 바이트다(6.6절, MS-T1). 노드 저장 순서와 화면 편집(재경유 추가 포함)은 결과에 영향이 없다(출력 핀에서 거슬러 가고 재경유는 값을 그대로 넘긴다 — MS-T2의 생성 원문 부분).
- **시험(MS-T3).** fixture 그래프의 생성 원문을 저장된 기대 원문(`Tests/Golden/*.txt` — 셰이더 원문 검사 대상이 아니게 `.txt`)과 byte로 견준다. 생성 규칙을 바꾸면 기대 원문을 함께 고친다.

### 7.8 MS2 완료 — 생성 원문 컴파일 확인

MS2a–MS2f의 생성 원문이 실제 셰이더 컴파일러에서 통과하는지 본다(15장 MS2 완료). 문자열 골든(7.7절)은 모양만 보므로, 이 단계가 HLSL 문법·타입·바인딩 규칙 오류와 하위 단계(DXIL 검증·SPIR-V → GLSL·자원 바인딩) 오류를 잡는다.

- **생성 파일 연결(16절 MSR3 구현).** 셰이더 컴파일 job에 `materialSource`(생성 원문 메모리 텍스트)를 둔다. 비지 않으면 전처리가 `/Engine/Generated/Material.rsh` include를 그 텍스트로 잇는다(정점 입력 연결 `vertexInputSource`와 같은 `ShaderGeneratedInclude` 장치). job 키 5행 전처리 결과 SHA-256에 이어 붙인 원문이 들어가 머티리얼끼리 너겟을 섞지 않는다. 비면 잇지 않으므로 머티리얼 없는 셰이더가 이 경로를 include하면 전처리 오류다(7.2절). 경로 상수는 ShaderCore `MaterialGeneratedIncludePath`(짝 `VertexInputGeneratedIncludePath` 옆)다.
- **경고 정책(사용자 결정 2026-11-28).** 엔진 셰이더는 경고를 오류로 한다(셰이더 시스템 결정 7). 머티리얼 job은 UE처럼 경고를 오류로 올리지 않고 진단으로 남긴다(UE `r.Shaders.WarningsAsErrors` 기본 0 — `Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:484`, 0이면 어떤 셰이더도, 1이면 전역 셰이더만 오류 — `:3719–3723`). 옵션 `ShaderCompileOptions.warningsAsErrors`(기본 참, 키에 든다)는 셰이더 시스템이 정한다. 생성기가 만드는 경고는 여전히 생성기 버그로 고친다(7.2절 결정 7). 다만 UE와 같은 원문이라 UE도 내는 경고(FXC X3571 pow 음수 밑 — 6.8절 Power 행)는 UE처럼 그대로 둔다(진단으로 남는다). 옵션 `ShaderCompileOptions.warningsAsErrors`(셰이더 시스템 16.4절 SSD79–SSD84 — 렌더 세션 c19d226fa)가 들어와 머티리얼 job은 false로 컴파일한다(경고는 job 결과 진단 — 캐시 적중 job은 비어 있다, 셰이더 시스템 16.4절). D3D11 대체 경로(DXC → SPIR-V → HLSL 5.0 → FXC)의 중간 원문은 UE처럼 X3571을 끈다.
- **시험 pass 셰이더.** `Engine/Source/Developer/MaterialCompiler/Tests/Shaders/MaterialTestPass.rsf`(가상 루트 `/Plugin/MaterialCompilerTests`). 엔트리는 정점 `MaterialTestVertex`(`EvaluateMaterialVertex`·`EvaluateMaterialVertexPrevious`)와 픽셀 `MaterialTestPixel`(`EvaluateMaterialNormal` → `c.pixelNormal`, `EvaluateMaterialSurface`·`Opacity`·`OpacityMask`·`PixelDepthOffset`·`Preview`, 머티리얼이 정의하면 custom 출력 `BentNormal0`)이고 결과를 출력에 모아 하위 컴파일러가 지우지 않게 한다. 문맥 구조체는 실행 값(정점 번호·픽셀 위치의 스칼라를 구조체로 바꾼 값)으로 채운다 — 상수 입력이면 FXC가 식을 접어 상수 경고를 낸다. 공급 퍼뮤테이션 둘: 공급 없음(UE 기본값 경로)과 공급 전부(`MATERIAL_SUPPLY_<기능>` 전부 — 시험이 기능 표에서 job 정의로 만든다, 표와 늘 맞는다). 공급 전부의 함수형 문맥 값은 pass가 `MaterialPass<이름>`(이전 프레임 판 `MaterialPassPrevious<이름>`)을 정의해야 하므로, 시험이 문맥 값 표에서 만든 0 값 stub 매크로를 그 퍼뮤테이션의 생성 원문 앞에 붙인다(자원 형 값은 stub이 없다). 정점 입력 타입은 쓰지 않는다(16절 MRB3 때 더한다).
- **판정.** 공급 전부 퍼뮤테이션은 경고를 오류로 컴파일해 생성기 경고를 잡는다. 공급 없음은 머티리얼 job 정책대로 `warningsAsErrors = false`다(MD15 0 기본값의 상수 접기 경고 — 예: ViewProperty 역수 `1 / 0` — 는 진단으로 남는다). 형식마다 컴파일 수와 경고가 있는 job 수를 찍는다. 형식 SKIP은 job 전부가 UnsupportedCapability일 때뿐이다(ToolError는 원문·도구 실패로 센다).
- **sink가 없으면 실패.** 첫 번역의 IR Custom 표에서 sink 함수를 찾지 못하면(본문을 형으로 만들 수 없다) 시험 구성 실패다(임시 본문 `return 0.0;`으로 컴파일하면 계산이 지워져 조용히 통과한다).
- **노드 모음**(자동): 엔진 노드 정의 중 상태가 Implemented인 것마다 그래프를 만든다(제외: Custom HLSL — sink 자체, FunctionCall·FunctionOutput·LayerOutput·MaterialSample — 에셋 그래프 펼침, 함수·레이어 시험이 본다). 입력에는 실행 값 probe를 잇는다: 그래프마다 스칼라 파라미터 Float4 하나를 보간기로 넘긴 픽셀 값의 핀 폭 마스크에 입력마다 다른 스칼라 파라미터(핀 폭대로 Append)를 더한 값(상수 버퍼 값만이면 노드 연산이 상수 식 프로그램으로 올라가 셰이더에 남지 않는다 — 6.10절; 입력마다 보간기를 두면 보간 예산을 넘는다), 텍스처 입력에는 시험 텍스처 객체. 출력은 Custom HLSL sink 하나가 모두 받고 본문이 인자를 모두 읽는다(Numeric·Any 출력은 ComponentMask R을 거친다; 본문은 임시 본문으로 한 번 번역해 IR Custom 표의 인자 형을 읽고 그 형으로 만든다 — 수는 첫 성분, Double은 낮춘 첫 성분, 텍스처는 가운데 샘플링; 안 읽으면 하위 컴파일러가 계산을 지운다). 두 판의 생성 원문이 같으면 하나만 컴파일한다. 수 입력 전부를 이은 판과 필수 입력만 이은 판을 각각 만들어 번역되는 판을 모두 컴파일한다(기본값 경로 — PixelNormal·TexCoord 대체 — 도 지나게). 픽셀에서 번역되지 않으면(정점 전용 노드) sink를 WPO에 잇고 정점 판(uniform probe)으로 다시 만든다. 번역 실패의 첫 진단을 찍고, 진단에 NotYetLowered가 하나라도 있으면(어느 판에서든) 실패다(구현 노드는 모두 번역된다 — 카탈로그 완전성). 번역기 하드 실패도 실패다. 들어간 노드 수가 하한(`MaterialCompileCorpusMinimumNodes` — 측정 값에서 작은 여유를 뺀 값)보다 적으면 실패다.
- **손 모음**: 노드 모음이 지나지 않는 길이다 — 7.7절 Math 골든 그래프(WPO·OpacityMask·AO·PDO·분기 — 골든 시험과 같은 모양, 입력만 sin(0.5) 대신 TexCoord 0 R + probe: 상수면 FXC가 분기 조건을 접어 `[branch]` 경고 X3554를 내고, uniform이면 cos·Append가 상수 식 프로그램으로 올라가 셰이더에 남지 않는다; TexCoord는 정점·픽셀 둘 다 실행 값이다), 정점 결과(이전 프레임·customized UV·보간기), Operator의 연산 전부(연산마다 번역되는 입력 모양 — 실수 Float3 셋, 부호 없는 정수 셋, 실행 값 비교 Bool 셋 순 — 을 혼자 번역해 고르고, 번역되지 않는 연산은 기대 목록과 같아야 한다: Transpose·MatrixMultiply(행렬 입력)·LWCTile(LWC 타일 — 그 NotYetLowered만 예외)), ViewProperty의 속성 전부, Noise ALU 함수 셋(quality 1–4)·VectorNoise 다섯·Double 위치, ColorRamp 연결 입력(커브 공급), CurveAtlasRowParameter(atlas 공급·행 파라미터 — 6.10절 MS2c5), Custom HLSL(텍스처·Double 인자·추가 출력·정의, 정점·픽셀 두 판), 텍스처 샘플(자기·공유 샘플러), custom 출력(BentNormal), 기본 표면. 손 모음은 두 엔트리를 컴파일한다.
- **형식.** 형식 정의 전부(엔진 셰이더 컴파일 행렬과 같은 목록 — 시험 지원 공용)로 실제 ShaderCompileWorker가 컴파일한다. 이 호스트가 만들 수 없는 형식(Windows의 Metal)과 자리 형식(단계 없음)은 형식마다 SKIP과 사유, 워커가 없거나 도구 저장소가 준비되지 않으면 컴파일 부분이 SKIP이다(모음 번역·완전성 검사는 늘 돈다). 실패하면 머티리얼·엔트리·퍼뮤테이션·형식과 진단을 찍고, 생성 원문을 `out/MaterialCorpusFailures/`(실행마다 비운다)에 써서 경로를 알린다(쓰기 실패도 찍는다)(공급 전부 판은 stub 줄 수만큼 줄 번호가 밀린다 — 그 수도 찍는다).
- **드러난 결함(이 단계에서 고침).** MaterialInputs의 `svPosition` 중복 칸(MS2f2 문맥 값 SvPosition과 손으로 넣던 칸), 공유 샘플러 선언 헤더 부재(셰이더 시스템 7.2절 9항의 `/Engine/Shared/SharedSamplers.rsh`는 Shared가 C++·HLSL 공용 `.h`만 받아 `/Engine/Private/Common/SharedSamplers.rsh`로 만들었다 — 16절 MSR16), Power의 X3571을 UE와 다른 근거로 끄던 것(UE는 원문 FXC 경로에서 경고로 둔다 — 허용 목록으로), 한 텍스처를 자기·공유 샘플러로 함께 샘플링하면 slot 하나에 출처가 둘이 되던 것(Vulkan·GLES 바인딩 규칙 오류 — 6.10절 텍스처 표를 (출처, id, 샘플러 출처)로).
- **값 대조는 이 단계가 아니다.** noise 등 셰이더 함수의 값을 UE와 대조하려면 셰이더를 실행해야 한다. 그것은 GPU 시험 장치(소프트웨어 Vulkan 포함)에서 머티리얼을 그리는 첫 단계(MS4 — 렌더러 pass 연결)에서 한다. 그때까지 CPU 쪽 값은 IR 계산기 시험(6.13절)이 본다.
