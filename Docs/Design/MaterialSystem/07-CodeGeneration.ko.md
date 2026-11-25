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
| `MATERIAL_NUM_TEXCOORDS`, `MATERIAL_NUM_INTERPOLANT_VECTORS` | 정점 입력에서 읽을 UV 채널 수, 머티리얼이 쓰는 보간 `float4` 수 |
| `MATERIAL_OPACITY_MASK_CLIP` | 마스크 기준값(기본 속성 상수) |
| `void EvaluateMaterialVertex(in MaterialVertexInputs c, inout MaterialVertexResult r)` | WPO(cm, world 공간 오프셋)·customized UV·보간 값 |
| `void EvaluateMaterialVertexPrevious(in MaterialVertexInputs c, inout MaterialVertexResult r)` | 이전 프레임 WPO(계획 14.7절 current/previous 계약). 문맥의 이전 시간·이전 instance 데이터로 평가한다 |
| `SurfaceAttributes EvaluateMaterialSurface(in MaterialInputs c)` | 표면 속성(Renderer 14.1절 함수 이름) |
| `float EvaluateMaterialOpacity(in MaterialInputs c)` | 반투명 coverage(`SurfaceAttributes.opacity`와 같은 값, 표면 전체 없이) |
| `float EvaluateMaterialOpacityMask(in MaterialInputs c)` | 마스크 값. depth·shadow pass가 표면 전체를 계산하지 않게 따로 둔다(10.4절 D8) |
| `float EvaluateMaterialPixelDepthOffset(in MaterialInputs c)` | PDO(cm) |
| `float4 EvaluateMaterialCustomOutput_<이름>(in MaterialInputs c)` | 머티리얼이 정의한 custom 출력 |
| `float4 EvaluateMaterialPreview(in MaterialInputs c)` | 노드 미리보기 번역에서만 의미가 있다(11.4절) |

함수 사이에는 계산이 겹칠 수 있다(예: 마스크 계산과 표면 계산이 같은 텍스처를 읽는다). 진입점마다 필요한 값만 내므로, 한 pass가 쓰는 함수는 필요한 계산만 한다. 한 pass에서 두 함수를 부르면 겹치는 계산의 제거는 하위 컴파일러(DXC·FXC)의 CSE에 맡긴다.

**문맥 구조체**(`MaterialCommon.rsh`):
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
| 9항 텍스처·샘플러 짝 | 텍스처 `MaterialTexture<k>`마다 `MaterialTexture<k>Sampler`를 낸다(짝 규칙 그대로, 결정 MD4). 공유 샘플러(16절 MSR13)가 생기면 텍스처 slot의 샘플러 출처(자기 샘플러·공유 Wrap·공유 Clamp, UE `ESamplerSourceMode`)로 고른다. 공유 출처면 `MaterialTexture<k>Sampler`를 내지 않고 `SharedWrapSampler`·`SharedClampSampler`(셰이더 시스템 개정 13, 7.2절 9항)로 샘플링한다 |
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

- **파일.** 생성 파일 `/Engine/Generated/Material.rsh`는 7.3절 정의를 먼저 내고, 진입점 함수를 7.3절 표 순서로 낸다. 정적 원문 `Engine/Shaders/Private/Material/MaterialCommon.rsh`는 문맥 구조체(`MaterialVertexInputs`·`MaterialVertexResult`·`MaterialInputs`)와 `Shading/SurfaceAttributes.rsh` include, 생성 파일 include를 담는다(7.2절). MS2a의 문맥 구조체 칸은 MS2a 노드가 쓰는 것뿐이고 MS2b가 문맥 값 표(19.3.2절)대로 채운다.
- **함수 몸체.** 진입점에서 닿는 값을 다시 매긴 번호 순으로 `const <타입> m<번호> = <식>;`로 낸다. 상수는 식 안에 리터럴로 바로 쓴다(지역 변수로 두지 않는다). 쓰지 않는 함수는 기본값을 돌려준다(7.3절).
- **리터럴.** float는 Core StringBuilder `{}`(최단 왕복, C runtime·locale 무관)로 쓰고, 정수 모양이면 `.0`을 붙인다. 벡터는 `float3(a, b, c)`다. 정수는 10진이고 uint는 `u` 접미다. double(LWC) 상수는 MS2b의 LWC 표현(5.3절 — 타일 + 오프셋)으로 낸다.
- **연산 철자.** 함수 연산은 HLSL 내장 함수 이름이다(`saturate`·`frac`·`lerp`·`rsqrt` 등). IsNan·IsInf는 fast-math가 지우지 않는 비트 검사 함수(`MaterialIsNan`·`MaterialIsInf` — `MaterialCommon.rsh`)로 낸다(6.2절). 0 나누기는 HLSL 그대로다(UE도 그대로).
- **`DynamicBranch`.** `[branch] if (c) { r = a; } else { r = b; }`로 낸다. MS2a는 두 가지 값을 분기 앞에서 계산한다(값을 가장 좁은 블록에 두는 배치는 후속이다 — UE IR 빌더의 배치 단계, 6.1절).
- **원문 위치 지도.** 값을 낸 줄마다 원인 노드(NodeId·PinKey)를 기록한다(7.6절). `#line`은 `/Engine/Generated/Material.rsh`만 쓴다.
- **결정성.** 같은 그래프는 같은 바이트다(6.6절, MS-T1). 노드 저장 순서는 결과에 영향이 없다(출력 핀에서 거슬러 가므로).
