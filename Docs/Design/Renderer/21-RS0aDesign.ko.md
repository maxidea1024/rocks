# 렌더러(Renderer) 설계 — 21. RS0a 설계

[목차](README.ko.md)

## 21. RS0a 설계(개정 16–18, 2026-11-28)

17.2절 RS0a(렌더 단독)의 구현 설계다. 머티리얼 세션이 MS3f(머티리얼 셰이더를 Project 라이브러리에 실제로 컴파일)를 하려면 렌더러 pass 셰이더 타입이 먼저 있어야 하므로, 그 부분을 **RS0a1**로 먼저 끝내고 알린다. 나머지 런타임(장면·view·가시성·GPU Scene 업로드·pass 실행)이 **RS0a2**다. 개정 18은 교차 리뷰(막는 문제 4·중요 12·사소 6)를 반영했다.

### 21.1 RS0a1 범위

- `RendererShaders` 모듈(3.2절 배치)과 그 엔진 모듈 등록.
- 머티리얼 pass 타입 넷과 엔진 타입(위치 전용 depth·velocity, 21.3절), "지원 머티리얼" 판정과 traits 거르기(21.4절), 정적 메시 정점 입력 타입 `StaticMesh`(21.5절).
- 머티리얼 문맥 공급 VIEW·VERTEX·PRIMITIVE(21.6절), GPU Scene 인스턴스 항목(21.7절), 공유 상수·자원 선언(21.8절).
- 완료 기준(21.11절)과 머티리얼 세션 알림(21.9절).

RS0a1은 장치를 쓰지 않는다. 런타임 그리기는 RS0a2(21.10절)다.

### 21.2 모듈

- `Engine/Source/Runtime/RendererShaders`, 의존은 ShaderCore·RenderData·MaterialSchema·Core(3.2절). 장치·Renderer·RenderBase를 링크하지 않는다(쿠커가 링크한다 — 결정 RN11).
- 수명 객체 Startup이 셰이더 타입 등록부(ShaderCore `ShaderTypeRegistry` — RenderBase `EngineShaderTypes`와 같은 방식)에 21.3절 타입과 21.5절 정점 입력 타입을 등록한다. 등록부가 얼기 전(엔진 모듈 시작 단계)이다.
- 쿠킹 등록부 원천(`ModuleShaderCookRegistrySource`)은 엔진 모듈 묶음을 시작해 등록부를 받으므로(`ModuleShaderCookRegistrySource.cpp:55–79`), `RendererShaders`가 엔진 모듈 묶음에 들어 있으면 쿠커가 이 타입을 본다(3.2절 개정 14). 엔진을 쓰는 모든 타깃(Sandbox·Commandlet·EngineModular·게임 템플릿)의 엔진 묶음에 넣는다.
- 원문: `/Engine/Private/Renderer/`(pass `.rsf`, 공용 `.rsh`), 정점 입력 `.rsh`는 `/Engine/Private/VertexInput/`.
- 장치 정점 배치(어느 stream·형식을 어느 location에 묶는지)는 RenderBase 10.6절 (3)대로 desc에 넣지 않고 **Renderer**가 메시마다 만든다(`RendererShaders`는 RenderBase를 링크하지 않는다). `RendererShaders`는 location 표(21.5절)만 정한다.

### 21.3 셰이더 타입

머티리얼 타입(머티리얼마다 곱한다 — 14.3절 행 가운데 RS0이 쓰는 것):

| 타입 | 단계 | `MeshPass` | 원문·진입점 | 거르기(21.4절) | UE |
|---|---|---|---|---|---|
| `DepthPassVertex` | VS | `Depth` | `Renderer/DepthPass.rsf` `DepthPassVertex` | 지원 머티리얼·(위치를 바꾸거나 모든 픽셀을 쓰지 않음) | `TDepthOnlyVS<false>` |
| `DepthPassPixel` | PS | `Depth` | 같은 파일 `DepthPassPixel` | 지원 머티리얼·(마스크 또는 PDO) | `FDepthOnlyPS` |
| `ForwardOpaqueVertex` | VS | `ForwardOpaque` | `Renderer/ForwardOpaquePass.rsf` `ForwardOpaqueVertex` | 지원 머티리얼 | `TBasePassVS` |
| `ForwardOpaquePixel` | PS | `ForwardOpaque` | 같은 파일 `ForwardOpaquePixel` | 지원 머티리얼 | `TBasePassPS` |

엔진 타입(머티리얼 없음, 정점 입력 축만 — RenderBase `BuiltinMeshVertex`와 같은 방식, `EngineShaderTypes.cpp:22–39`):

| 타입 | 단계 | 쓰임 | UE |
|---|---|---|---|
| `DepthPositionOnlyVertex` | VS | 위치 전용 depth(아래) | `TDepthOnlyVS<true>` |
| `VelocityPositionOnlyVertex`·`VelocityPositionOnlyPixel` | VS·PS | 위치 전용 머티리얼의 이동 물체 velocity(현재·이전 변환) | `FVelocityVS`·`FVelocityPS`의 기본 머티리얼 경로 |
| `HzbBuild`(RS0a2) | CS | HZB furthest·closest(F5) | `FHZBBuildCS` |

- 머티리얼 타입 넷은 `ShaderTypeMaterialShader | ShaderTypeVertexInputAxis`이고 `materialShouldCompile`(셰이더 시스템 SSD86)만 쓴다. 엔진 타입은 `shouldCompile`(정점 입력 = `StaticMesh`)이다.
- **위치 전용 depth(UE와 같은 판정, 다른 배치).** UE는 "불투명이고, 위치를 바꾸지 않고(WPO·PDO 없음 — UE `MaterialModifiesMeshPosition`은 PDO도 넣는다, `HLSLMaterialTranslator.cpp:1730`), 모든 픽셀을 쓰고(마스크 없음), 정점 입력에 위치 전용 stream이 있는" 머티리얼의 depth를 위치 전용 VS와 기본 머티리얼로 그린다(`Renderer/Private/DepthRendering.cpp:949–990`, 둘째 갈래: 위치 전용 stream이 없으면 기본 머티리얼 + 전체 VS). Rocks 정적 메시는 위치 stream이 따로 있으므로(21.5절) 첫째 갈래만 있다. 이 VS는 머티리얼 함수를 부르지 않으므로 엔진 타입으로 둔다(머티리얼마다 곱하지 않아 쿠킹이 줄고, RS0a2 depth prepass가 머티리얼 없이 돈다).
  - 컬 모드는 원래 머티리얼의 양면 여부를 따른다(UE `DepthRendering.cpp:1016–1017`) — 셰이더가 아니라 렌더러 상태 묶음 키다.
  - UE `WritesEveryPixel`은 dithered LOD 전환·stencil·wireframe에서도 거짓이다(`MaterialShared.cpp:4886–4895`). Rocks에 이 기능이 생기면 이 판정에 더한다.
- **RS0의 base pass는 따로 두지 않는다.** RS0의 "Unlit·emissive base pass"는 `ForwardOpaque`를 지원 shading model {Unlit}으로 만든 것이고, RS1이 같은 타입에 DefaultLit 조명과 14.3절 퍼뮤테이션 차원을 더한다(원문이 바뀌므로 셰이더 집합 id가 바뀐다 — 하위 호환 없음 규칙대로 다시 쿠킹한다).
- **출력.**
  - `DepthPassPixel`: 마스크면 `clip(opacityMask − clipValue)`, PDO면 `SV_Depth`.
  - `ForwardOpaquePixel`: SceneColor(`RGBA16Float`) 하나에 Unlit 결과 `emissive`(UE Unlit은 emissive를 장면 색에 그대로 쓴다). **마스크면 늘 `clip`**(prepass 없는 프로필 — 21.12절 — 에서도 맞고, prepass가 있으면 `Equal` 깊이와 같은 결과다). **PDO면 `SV_Depth`**(prepass와 같은 깊이를 써야 `Equal`이 성립한다 — 14.1절).
- **위치 불변(invariance).** depth·velocity·base pass의 VS가 같은 정점에 bit 단위로 같은 clip 위치를 내야 `Equal`이 맞는다(엔진 라이브러리 VS와 Project 라이브러리 VS가 섞인다). 규칙: 위치는 공용 함수 하나(`StaticMeshTranslatedWorldPosition`, 21.5절)로만 만들고, `SV_Position` 출력은 `precise`다(UE `INVARIANT_OUTPUT`·`INVARIANT()` — `PositionOnlyDepthVertexShader.usf:13,25`, `DepthOnlyVertexShader.usf:24,61`, `BasePassVertexShader.usf:76`). GLSL에서 `invariant gl_Position`이 나오는지는 RS0a1 컴파일 시험에서 확인하고(SPIRV-Cross 출력 대조), 안 나오면 셰이더 시스템에 요구로 올린다. 최종 확인은 RS0b의 깊이 `Equal` 영상 시험이다.

### 21.4 지원 머티리얼 판정과 traits 거르기

- **지원 머티리얼(머티리얼 단위).** 다음을 모두 만족하는 머티리얼이다. 머티리얼 타입 넷의 거르기는 모두 이 판정을 먼저 본다. 그래서 대체는 **머티리얼 단위**다: 지원하지 않는 머티리얼은 모든 pass(depth·velocity·base)를 엔진 기본 머티리얼로 그리고, pass마다 따로 대체하지 않는다(대체하면 prepass와 base pass 깊이가 달라 `Equal`이 깨진다 — UE도 셰이더 맵이 없으면 머티리얼 전체를 대체한다).
  - `domain == Surface`, `blendMode`가 Opaque 또는 Masked.
  - `shadingModels`가 0이 아니고 지원 집합의 부분집합(RS0 {Unlit}, RS1부터 {Unlit, DefaultLit} — `RendererShaders` 상수 하나).
  - 형식 등급 ≥ `minimumTier`(아래).
- **형식 등급.** MaterialSystem 6.4절 요구("형식 등급이 `minimumTier`보다 낮은 형식은 거르기에서 빠진다")를 따른다. `ShaderFormatFacts`에는 등급 칸이 없으므로 렌더러가 유도한다: `platformClasses`에 Mobile이 있으면 Mobile, 아니면 형식 셰이더 모델 6 이상이면 Sm6, 그 밖은 Sm5(SSD89 `platformClasses`, 형식 정의 셰이더 모델). GLES31_PC(Pc+Mobile)는 Mobile 등급이다(실행 기기 능력이 모바일 GLES와 같다). 이 유도식은 셰이더 시스템에 요구로 올려 `ShaderFormatFacts`의 칸으로 옮긴다(공용화).
- **pass별 조건.**
  - `DepthPassVertex`: `usesWpo || usesPdo || usesOpacityMask`(UE `!bWritesEveryPixel || MayModifyMeshPosition` — `DepthRendering.h:93–98`). 아니면 `DepthPositionOnlyVertex`가 그린다.
  - `DepthPassPixel`: `usesOpacityMask || usesPdo`(UE `DepthRendering.h:136`).
  - `ForwardOpaque*`: 지원 머티리얼이면 늘.
  - 정점 입력 타입이 `StaticMesh`(21.5절).
- **엔진 기본 머티리얼.** `engineDefaultMaterial`이면 네 타입을 모두 컴파일한다(UE special 머티리얼은 `TDepthOnlyVS<false>`도 컴파일한다 — `DepthRendering.h:86,96`). 기본 머티리얼은 지원 머티리얼이어야 한다(Opaque·Unlit 또는 DefaultLit). 어기면 쿠킹 오류다: 셰이더 라이브러리 단계의 묶음 정규화(셰이더 시스템 SSD88)가 "엔진 기본 머티리얼인데 머티리얼 pass 타입 조합이 0인 형식"을 `Inconsistent`로 낸다(거르기 함수는 bool이라 오류를 낼 수 없으므로 검사 자리를 단계로 둔다).
- **traits 읽기.** 거르기는 조합마다 불리므로 할당 없이 읽어야 한다. MaterialSchema에 고정 부분만 읽는 `ReadMaterialShaderTraitsFixed(bytes, fixed, consumed)`(custom 출력 이름 목록은 길이만 건너뜀, 할당 없음)를 요청한다(머티리얼 세션 요구 MSR20). 손상 바이트는 거르기 false다. 조용히 지나가지 않도록 셰이더 라이브러리 단계가 묶음마다 한 번 `ReadMaterialShaderTraits`로 전체 검증(`consumed == size` 포함)을 먼저 하고, 실패하면 그 묶음을 `Inconsistent`로 낸다(묶음마다 진단 한 번 — 조합마다 Log를 쓰지 않는다).
- **문맥 기능.** 공급하지 않는 기능(21.6절 밖)을 쓰는 머티리얼도 컴파일한다. 그 접근자는 UE 기본값을 돌려준다(`MaterialContext.rsh` 머리 계약 — UE도 그 데이터가 없는 정점 팩토리·pass에서 같은 기본값이다). 렌더러가 그 기능을 나중에 공급하면 원문이 바뀌어 다시 컴파일된다.

### 21.5 정적 메시 정점 입력 타입 `StaticMesh`

- **이름.** 정점 입력 타입 이름은 머티리얼 사용 대상 이름과 같은 `StaticMesh`다(`MaterialSchema/VertexInputUsage.h:16`). 정점 입력 id는 이름 해시이므로, 머티리얼 쪽은 사용 대상 이름을 해시해 id를 얻는다(변환 표가 없다 — MaterialSystem 12장 "usage → id 표는 RS0가 만든다"를 이 규칙으로 닫는다). 다른 정점 입력 타입(skinned — RS12)도 같은 규칙이다.
- **원문 둘(위치 전용과 머티리얼 채우기 분리).**
  - `/Engine/Private/VertexInput/StaticMeshVertexFetch.rsh`: 정점 입력 구조체(location 표), 인스턴스 번호 → GPU Scene 항목 읽기(21.7절), `StaticMeshTranslatedWorldPosition`(현재·이전)과 법선·tangent 변환. 머티리얼 원문을 include하지 않는다. 엔진 타입(위치 전용 depth·velocity)과 머티리얼 타입이 같이 쓴다.
  - `/Engine/Private/VertexInput/StaticMeshVertexInput.rsh`: 위 파일을 include하고 `MaterialVertexInputs`·보간값을 채운다. 머티리얼 pass만 include한다(`MaterialCommon.rsh` 뒤).
- **location 표(고정).** 메시 에셋 5.1절 `MeshRenderStream`에서 Renderer가 장치 배치를 만든다. 셰이더가 읽는 location은 늘 같다.

| location | 속성 | 형식(메시) | 없을 때 |
|---|---|---|---|
| 0 | position | `Float3` | 없을 수 없다(쿠킹이 보장) |
| 1 | tangent(w = 1) | `Byte4Snorm` 또는 `Short4Snorm` | 없을 수 없다 |
| 2 | 법선(w = 쌍법선 부호) | 같음 | 없을 수 없다 |
| 3–6 | UV 쌍 0–3(채널 2k, 2k+1) | `Half4`·`Float4`(마지막 홀수 채널 `Half2`·`Float2`) | 마지막 UV stream을 되풀이해 묶는다. UV가 하나도 없으면 공용 기본 stream(0) |
| 7 | 정점 색 | `Bgra8Unorm`·`UByte4Unorm` | 공용 기본 stream(흰색) |

  - 셰이더가 읽는 UV 쌍 수는 머티리얼이 쓰는 UV 수(`NUM_MATERIAL_TEXCOORDS`, 생성 원문)에서 정한다. 머티리얼마다 컴파일하므로 퍼뮤테이션이 아니다. 위치 전용 엔진 타입은 location 0만 읽는다.
  - **공용 기본 stream.** RenderBase 10.4절은 stride ≥ 형식 크기라 stride 0을 쓸 수 없다. 그래서 Renderer가 "정점 수 이상" 크기의 공용 버퍼(흰색 `UByte4Unorm`·0 `Half4`)를 하나씩 두고 필요한 메시 정점 수까지 늘린다. 셰이더가 읽는 location ⊆ 배치(10.4절)를 늘 지킨다.
  - 형식 변형(Snorm 폭, Half·Float)은 장치 배치가 흡수하고 셰이더 입력은 모두 float다. 정점 입력 타입은 하나이고 퍼뮤테이션이 없다.
- **인스턴스 변환.** RenderBase 10.5절: `instanceIndex = InstanceIndices[MeshDrawConstants.instanceListOffset + SV_InstanceID]` → 21.7절 항목 → 카메라 상대 위치.
- **법선 변환.** 비균일 scale에서도 맞도록 local → world 행렬 3×3의 여인수 행렬(adjugate의 전치)로 법선을 변환하고 정규화한다. 행렬식 부호(항목의 표시 비트)를 곱해 음수 scale에서도 바깥을 향하게 한다. 역행렬·`InvNonUniformScale`을 항목에 두지 않는다(UE는 둔다 — `SceneData.ush:233–234`). 셰이더 ALU가 조금 늘지만 항목이 작다. tangent는 3×3 그대로 변환한다.
- **보간값.** 머티리얼 보간값은 생성 원문의 `MATERIAL_NUM_INTERPOLANT_VECTORS`(상한 `MATERIAL_MAX_INTERPOLANT_VECTORS` 16 — `MaterialContext.rsh:14`)만큼 정점 출력에 이어 붙인다.

### 21.6 머티리얼 문맥 공급(RS0)

머티리얼 pass는 `MATERIAL_SUPPLY_VIEW`·`MATERIAL_SUPPLY_VERTEX`·`MATERIAL_SUPPLY_PRIMITIVE`를 1로 정의한 뒤 `/Engine/Private/Material/MaterialCommon.rsh`를 include한다(이 파일이 생성 원문 `/Engine/Generated/Material.rsh`를 include한다 — `MaterialCommon.rsh:168`). 공급 함수는 렌더러 공용 원문 `/Engine/Private/Renderer/MaterialPassSupply.rsh`가 정의한다. 나머지 기능(SKIN·LIGHTMAP·LIGHT·SKY·SCENE 등)은 공급하지 않으므로 UE 기본값이다(21.4절).

- **함수 수.** VIEW 36, VERTEX 3, PRIMITIVE 60(생성 표 `MaterialContext.rsh`). 값의 출처는 다음과 같다.

| 묶음 | 함수 예 | 출처 |
|---|---|---|
| 변환(카메라·view·translated world, 현재·이전) | `MaterialPassTransformTranslatedWorldPositionToView`, `MaterialPassPreviousTransformViewVectorToTranslatedWorld` 등 | `View` 행렬(21.8절) |
| PreViewTranslation(현재·이전) | `MaterialPassPreViewTranslation` | `View` 카메라 위치 DF(21.8절)의 부호 반전 |
| 화면 | `MaterialPassPixelPosition`·`ViewportUv`·`PixelDepth`·`IsOrthographic` | `SV_Position`, `View` 크기·투영 |
| pass 정체 | `ShadowPass`·`RayTracingPass`·`ReflectionCapturePass`·`LightBakePass`·`GlobalIlluminationPass`·`VirtualizedGeometryPass` | pass 정의(RS0은 모두 거짓) |
| 경로 | `RenderPathDeferred`·`RenderPathForwardPlus` | 14.3절 경로 정의 이름 |
| 능력 등급 | `CapabilityTierSm5`·`Sm6` | 21.4절 형식 등급 |
| 기타 view | `PeriodicWorldOrigin*`·`PostProcessVolumeFlagTest`·`ScalarBlueNoise` | UE와 같은 식. 후처리 볼륨 플래그는 거짓, 파란 잡음은 RS0에서 시스템 텍스처가 없으므로 UE 기본값 — 시스템 텍스처가 생기면 공급 |
| 정점 | `MaterialPassTexCoord`·`TransformTangentVectorToWorld`·`WorldToTangent` | 21.5절 정점 입력·법선 변환 |
| 인스턴스·local 변환(현재·이전, double 판 포함) | `MaterialPassLocalToWorldDouble`·`TransformLocalPositionToWorld` 등 | GPU Scene 항목(21.7절). 역변환은 셰이더에서 3×4 역행렬 |
| bounds | `ObjectBounds`·`ObjectLocalBoundsMin` 등 | GPU Scene 항목 bounds |
| 인스턴스 값 | `PerInstanceRandom`·`PerInstanceFadeAmount`·`PerInstanceCustomData*`·`CustomPrimitiveData*` | 항목 칸이 생길 때까지 UE의 "데이터 없음" 기본값 — custom data는 MS4·RS 후속 |
| 미리 skin된 bounds | `PreSkinnedLocalBounds*` | 정적 메시는 local bounds와 같다(UE 정적 메시와 같다) |

- `MaterialInputs`의 view 칸(실시간·pre-exposure·FOV·temporal 번호·mip bias·이전 행렬 등)은 같은 `View` 값으로 채운다. RS0에 없는 값(노출 — RS6)은 UE의 "노출 없음" 값(pre-exposure 1)이다.
- 이 표의 "UE 기본값"은 데이터가 아직 없는 칸이다. 공급하지 않은 채 0이 조용히 들어가는 것이 아니라, 문서로 정한 값이다. 칸이 생기는 단계에서 표를 고친다.

### 21.7 GPU Scene 인스턴스 항목(MRD7 확정)

`RGBA32Float` typed buffer에서 인스턴스마다 `float4` 10개다(RenderBase 10.5절 추정 10–12 안). 용량은 `texelBufferElements ÷ 10`이다(최소 보장 65536이면 6,553 인스턴스).

| float4 | 내용 |
|---|---|
| 0 | 현재 위치 상위(DF high) xyz, w = 표시 비트(`asuint`: 행렬식 음수, 그림자 caster 등) |
| 1–3 | 현재 local → 상대 world 행렬의 열 셋(행 벡터 `p·M`의 열 — `dot(float4(p, 1), 열 j)` = world j − 상위 j) |
| 4 | 이전 위치 상위 xyz, w = 0 |
| 5–7 | 이전 local → 상대 world 행렬의 열 셋 |
| 8 | local bounds 중심 xyz, w = 0 |
| 9 | local bounds 반 크기(half extent) xyz, w = 0(예약 — custom data 배치는 그 칸이 생기는 단계에서 정한다) |

- **큰 월드: UE와 같은 double-float(MRD6 확정, 개정 18).** UE 5.8 GPU Scene은 격자가 아니라 double-float다: CPU에서 원점을 double로 만들고 `PositionHigh = float(원점)`, 행렬은 그 상위 기준의 상대 값이다(`Engine/Public/PrimitiveUniformShaderParametersBuilder.h:313–323`, `Core/Public/Math/DoubleFloat.h:22–25`, `Shaders/Private/SceneData.ush:153–155`). Rocks도 같다. 격자(tile) 방식은 버린다: 2^21 cm 격자면 격자 가장자리에서 하위 값이 2^20 cm까지 커져 정점이 약 0.125 cm 간격으로 양자화된다. DF는 위치와 무관하게 국소 정밀도가 유지된다.
- 카메라 상대 위치 = `(인스턴스 상위 − View.cameraPositionHigh) + (p·M − View.cameraPositionLow)`이고(5.1절), 이전 프레임도 같은 식이다. 계산은 21.5절 공용 함수 하나로만 한다(21.3절 위치 불변).
- 머티리얼 LWC 값(`MaterialDouble3` — tile 번호 + offset, `MaterialDouble.rsh:1–31`)이 필요한 함수(`*Double`)는 셰이더에서 DF → tile 변환을 한다: tile = `round(high ÷ UE_LWC_RENDER_TILE_SIZE)`, offset = `(high − tile × 크기) + low`(UE 머티리얼 LWC와 같은 tile 크기 2,097,152 cm — `Core/Public/Misc/LargeWorldRenderPosition.h:14`). 이 상수는 머티리얼 LWC에만 쓴다.
- 행렬식 음수 비트는 컬 방향과 법선 부호에 쓴다(RenderBase 10.5절 앞면). "이전 변환 있음" 비트는 두지 않는다 — 4.2절대로 갱신이 없으면 이전 = 현재로 쓰므로 항목에 늘 이전 값이 있다.
- UE `FInstanceSceneData`(`SceneData.ush:227`)는 역행렬·비균일 scale·primitive id·random 등을 더 담는다. RS0에는 쓰는 곳이 없어 넣지 않는다(쓰는 단계에서 항목을 늘린다 — 런타임 버퍼라 형식 호환이 없다).

### 21.8 공유 상수·자원·파라미터 layout(RS0 멤버)

15.2절 `View`·`Scene`을 RS0 멤버로 선언한다. 상수는 공유 상수 slot(RenderBase 9.5절), 자원은 공유 자원 구조체(9.6절), 생성 HLSL은 셰이더 시스템 SH3(16.2절)이다.

- `View`(slot): `translatedWorldToClip`(jitter 포함)·`translatedWorldToClipNoJitter`·`clipToTranslatedWorld`·`translatedWorldToView`·`viewToTranslatedWorld`·`viewToClip`·`clipToView`와 각 이전 값, `cameraPositionHigh`·`cameraPositionLow`·`prevCameraPositionHigh`·`prevCameraPositionLow`(float3, DF), `renderSize`·`invRenderSize`(float2), `realTime`·`deltaTime`·`frameIndexMod8`, `isOrthographic`, `preExposure`(RS0은 1), `materialTextureMipBias`(RS0은 0). 21.6절 공급 함수가 읽는 값은 모두 여기서 온다.
- `Scene`(slot): `instanceCount`.
- `SceneResources`(공유 자원 구조체): `InstanceData`(`RGBA32Float` typed buffer SRV), `InstanceIndices`(`R32Uint` typed buffer SRV).
- `MeshDrawConstants`(루트 상수 앞 16 byte, RenderBase 10.5절): `instanceListOffset` + 예약 12 byte.
- **타입별 `parameterLayouts`**(라이브러리 항목의 layout 서명이 이 값이다 — 셰이더 시스템 SSD88):
  - 메시 pass 타입(머티리얼 넷·위치 전용 depth·velocity): `View`, `Scene`, `SceneResources`, `MeshDrawConstants`. 머티리얼 상수·텍스처는 머티리얼 생성 원문의 layout이고(MS4가 값을 채운다), 레이아웃 서명 칸은 MSR14가 정한다.
  - `HzbBuild`: 자기 pass 상수(원본 mip·크기)와 SRV·UAV.

### 21.9 머티리얼 세션에 줄 것과 받을 것

| 방향 | 내용 |
|---|---|
| 렌더 → 머티리얼(RS0a1) | 타입 이름 넷과 21.4절 거르기, 정점 입력 이름 = 사용 대상 이름(`StaticMesh`), 공급 기능 VIEW·VERTEX·PRIMITIVE, 생성 원문 호출 계약(아래) |
| 머티리얼 → 렌더 | MSR20 `ReadMaterialShaderTraitsFixed`(할당 없는 고정 부분 읽기), MS3d4·MS3f(RS0a1 타입으로 실제 컴파일), MS4 최소(17.1절) |

- **생성 원문 호출 계약.** 공급 정의 → `MaterialCommon.rsh` include → 픽셀에서 `EvaluateMaterialNormal`로 `pixelNormal`을 먼저 채운 뒤 `EvaluateMaterialSurface`(MaterialSystem 7장, 시험 pass `MaterialTestPass.rsf:32`와 같은 순서). 정점에서 `EvaluateMaterialVertex`(·이전), 마스크·PDO는 `EvaluateMaterialOpacityMask`·`EvaluateMaterialPixelDepthOffset`. 이것으로 MRD17(픽셀 법선 순서 — Renderer 담당)을 닫는다.

### 21.10 RS0a2(런타임) 순서

1. `Renderer` 모듈 골격, `RenderScene`(변경 목록 적용·SoA 표), `RenderView`·`ViewHistory`.
2. 가시성 V1(5.3절), 그리기 항목 캐시·상태 묶음(위치 전용 depth·velocity 항목 — 컬 모드는 머티리얼 양면, 행렬식 부호 따로 묶음).
3. `GpuScene` 업로드(변경 범위만, slot GPU 완료 gate 뒤 영역 재사용), `InstanceIndices` 프레임 목록, 공용 기본 정점 stream(21.5절).
4. depth prepass(불투명 인스턴스 전부를 `DepthPositionOnlyVertex`로 — prepass 유무는 인자, 21.12절), velocity(`VelocityPositionOnly*`), HZB(`HzbBuild`), 최소 tone map 연결, 공유 slot 등록(SM10 남은 부분).
5. 완료 기준: 17.2절 RS0a (a)·(b)·(c)·(e)와 RenderBase 10.5절 RS0 기준 — (`instanceListOffset`, `SV_InstanceID`) → 인스턴스 변환이 CPU 참조와 같은지 픽셀 대조(D3D12·Vulkan·GLES 미리보기, GLES는 VS texel buffer 경로), offset이 0이 아닌 그리기, 행렬식 음수 인스턴스의 별도 그리기와 컬링 픽셀.

런타임 Project 셰이더 라이브러리 열기(RR9)는 머티리얼 셰이더를 쓰는 RS0b의 일이다(17.2절 표를 그에 맞게 고쳤다).

### 21.11 RS0a1 완료 기준

- 등록: 머티리얼 타입 넷과 정점 입력 타입이 등록되고 엔진 열거(`EnumerateShaderLibraryCombinations` — `ShaderLibraryContents.h:46`)에 나오지 않는다(SSD88). 엔진 타입 셋(`DepthPositionOnlyVertex`·`VelocityPositionOnlyVertex`·`VelocityPositionOnlyPixel`)은 엔진 열거에 나오고 엔진 라이브러리에 쿠킹된다.
- 거르기 표 시험: traits 조합(domain·blend·shading model·마스크·WPO·PDO·기본 머티리얼·tier × 형식 class·셰이더 모델)마다 네 타입의 컴파일 여부가 21.4절과 같다. 손상 traits 바이트는 false. 묶음 단계의 손상·기본 머티리얼 검사는 `Inconsistent`.
- 컴파일 시험: MS2 노드 모음의 Unlit·Masked·PDO·WPO 머티리얼 생성 원문으로 머티리얼 타입 넷을 형식마다(D3D12·D3D11·Vulkan·GLES) 컴파일한다(`MaterialShaderCompileTests` 방식, `warningsAsErrors` false — SSD79). 엔진 타입 셋도 형식마다 컴파일한다. GLSL 출력의 `invariant gl_Position` 유무를 확인한다(21.3절).
- 엔진 셰이더 쿠킹(`asset-cook --engine`) 결과는 엔진 타입 셋 항목만 늘어난다.
- 알림: 21.9절 표를 머티리얼 세션에 보낸다(MS3f 선행 해소).

### 21.12 타일 기반 GPU(TBDR) 고려(개정 17)

모바일 GPU(Apple·Mali·Adreno·PowerVR)는 화면을 타일로 나눠 타일 메모리 안에서 그리고 끝에만 메모리에 쓴다. 이미 반영된 것과 빈 곳은 다음과 같다.

- **이미 반영:** 모바일 기본 경로가 Forward+다(결정 RN7 — GBuffer 왕복 대역폭이 없다). Deferred 조명은 tiled compute가 아니라 전체 화면 PS다(7.3절). 같은 attachment를 쓰는 연속 raster pass를 render pass 하나로 합친다(RenderBase RG3). 장치 패스는 attachment마다 load·store·clear를 받는다(GX4 — 쓰지 않을 내용은 버린다). subpass·framebuffer fetch(모바일 deferred 단일 pass, MSAA on-tile resolve)는 GX13로 모바일 경로(RS8)·Android(SH6)와 함께 간다.
- **depth prepass는 경로·프로필 선택이다(결정).** 데스크톱은 늘 켠다(HZB·base pass `Equal`, 5.1절). TBDR GPU는 하드웨어 숨은 면 제거(HSR·FPK)가 있어 prepass가 정점 일만 두 번 하게 되므로, UE 모바일은 기본으로 끈다(`r.Mobile.EarlyZPass` 기본 0 — `Renderer/Private/RendererScene.cpp:157`). Rocks도 모바일 프로필의 Forward+는 기본 prepass 없음으로 둔다. 그때 base pass는 깊이 `GreaterEqual` 쓰기이고, HZB는 base pass 깊이에서 만든다. 마스크 머티리얼만 prepass에 남기는 선택(UE `DDM_MaskedOnly`)도 같은 설정의 값으로 둔다. **RS0a 코드는 prepass 유무를 인자로 받는다**(base pass 깊이 상태·HZB 원천이 그 값을 따른다). 모바일 기본값과 측정은 RS1(Forward+)과 Android 프로필(SH6)에서 정한다.
- **메모리 없는 attachment(요구 RQ24).** 패스 밖으로 내용을 내보내지 않는 attachment(MSAA 색·깊이 중간값, 단일 pass 안의 GBuffer)는 메모리를 잡지 않는다: Vulkan `TRANSIENT_ATTACHMENT` 용도 + `LAZILY_ALLOCATED` 메모리, Metal `MTLStorageModeMemoryless`(UE `TexCreate_Memoryless` — `RHI/Public/RHIDefinitions.h:1116`). D3D·데스크톱에서는 일반 텍스처다. RenderBase GX13와 함께 장치 텍스처 용도 비트로 둔다.
- **대역폭 측정.** 타일 flush·reload 횟수와 대역폭은 실기기에서 잰다(Android 단계 측정 목록에 넣는다).
