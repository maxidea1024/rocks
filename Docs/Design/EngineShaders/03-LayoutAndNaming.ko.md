# 엔진 셰이더 — 3. 디렉터리·이름·셰이더 규약

[목차](README.ko.md)

## 3. 디렉터리·이름·셰이더 규약

### 3.1 디렉터리(결정 SLD1)

가상 경로는 SS SC2(`/Engine` = `Engine/Shaders`, 그 아래 `Public`·`Private`·`Shared`)를 그대로 쓴다. `Private` 아래를 영역 폴더로 나눈다. UE는 `Private` 최상위에 469개를 두지만 Rocks는 최상위에 파일을 두지 않는다.

```text
Engine/Shaders/
  Public/
    Platform.rsh                    (있음, SH1a) 형식·컴파일러·단계 정의 검사, ROCKS_SPIRV·ROCKS_IMAGE_FORMAT
    ShaderFeatures.rsh              형식 기능 → ROCKS_FEATURE_* 매크로(웨이브·typed UAV load 등), 대체 경로 선택
    Backend/<D3D|Vulkan|Metal|Gles>.rsh   형식별 차이(필요할 때만 만든다)
  Shared/                           C++·HLSL 공용 .h(번호표·구조체 배치·상수)
  Private/
    Common/        A01  모든 셰이더의 기본 묶음, 출력 구조, compute 보조, 웨이브 대체
    Math/          A02  수학·인코딩·패킹·난수·표본·교차
    Color/         A03  색 공간·전달 함수·톤 곡선·ACES·디스플레이 출력
    Shading/       A04·A23  BRDF·셰이딩 모델·광원 적분·에너지 보존·고급 모델 평가
    Subsurface/    A23  화면 공간 SSS·profile atlas
    Water/         A23  single layer water 셰이딩·합성
    Material/      A05  MaterialCommon.rsh(MS 7.2절)·텍스처 표본 보조
    VertexInput/   A05·A22  정점 입력 타입(기본·로컬 메시·spline·skinned·동적 메시)
    Scene/         A06  view 상수·GPU Scene 읽기·장면 텍스처·컬링·HZB
    BasePass/      A07  depth·velocity·base pass·GBuffer 인코딩
    Lighting/      A08·A09  광원 기록·light grid·직접 조명·Deferred 조명 pass·광원 함수
    Shadow/        A10  그림자 depth·표본·필터·contact shadow
    Reflection/    A11  반사 capture·sky light·IBL·planar·SSR
    Occlusion/     A12  SSAO·GTAO
    Decal/         A13  GBuffer·DBuffer decal
    StaticLighting/ A14 라이트맵·볼륨 라이트맵 표본
    Translucency/  A15  투명 합성·굴절
    Atmosphere/    A16  sky atmosphere·height fog
    PostProcess/   A17  노출·bloom·tone map·LUT·FXAA·TAA·upscale·grain
    Utility/       A18  전체 화면·복사·clear·밉·resolve·간접 인자
    Ui/            A19  UI 툴킷·단순 요소·글리프
    Debug/         A20  debug view·시각화·GPU 출력·편집 표시
    Compute/       A21  정렬·scan·reduction·압축
    Skinning/      A22  GPU skinning·morph·tangent 재계산
```

- 후속 영역(A24–A35)은 들일 때 `Private/<기능>/` 폴더를 더한다(예: `Private/VirtualGeometry/`). 지금은 폴더를 만들지 않는다.
- 렌더러 pass 셰이더(`RendererShaders` 모듈이 등록하는 타입, RN 3.2절)의 원문도 이 트리에 있다. 셰이더 타입 C++ 등록은 각 모듈이, 원문은 `Engine/Shaders`가 갖는다(UE와 같다).
- 엔진 밖(플러그인·프로젝트) 셰이더는 `/Plugin/<이름>`·`/Project` 아래에 두고 `/Engine/Private/...` 라이브러리를 include할 수 있다(SS SC2는 루트 사이 include를 막지 않는다). 엔진 라이브러리는 그래서 사실상 공개 API이며, 3.5절 함수 규칙이 그 계약이다.

### 3.2 `Public`과 형식 기능

- `Public/Platform.rsh`는 SH1a 그대로 둔다. 이 문서는 그 파일을 바꾸지 않는다(형식 정의 → 정의 조립 규칙은 SS 소관).
- `Public/ShaderFeatures.rsh`(SL0): 형식 기능을 셰이더가 묻는 매크로 `ROCKS_FEATURE_<이름>`(늘 0 또는 1로 정의)으로 모은다. 기능 어휘는 SS가 이미 정한 형식 정의 `features`(`compute`·`waveOperations`·`vertexViewportLayerOutput` — SS 2·12.2절, 15절 결정 12)이고, 지금 실제 형식 정의 파일(`Engine/Config/ShaderFormats/*.json`)에서 `waveOperations`는 `D3D12_SM6`에만 있다. SC10 정의 조립이 이 어휘를 정의로 내보내게 SS에 요구하고, typed UAV load(형식 확장 load)는 새 어휘 `typedUavLoad`로 더하게 요구한다(9.3절 SLR1). 그 전에는 형식 정의(`ROCKS_FORMAT_<id>`)로 판정하는 임시 표를 이 파일에 둔다: 웨이브 연산은 `D3D12_SM6`만 1이고 `VULKAN_SM5`·`METAL_SM5`·`D3D11_SM5`·`GLES31_*`는 0이다(형식 정의와 같게). 1차에 쓰지 않는 것: 64-bit atomic(후속 기능만), half 연산(결정 SLD4). `vertexViewportLayerOutput`은 런타임 확인 기능이라 원문에서는 퍼뮤테이션 정의로만 쓴다(SS 7.2절 7항).
- **단계별 자원 접근(SS 7.2절, RN 13절).** 형식 정의의 `uavStages`는 모든 형식에서 `compute`뿐이다. VS·PS는 UAV를 쓰지 않는다. 그래서 PS에서 GPU 메시지·print·quad overdraw 계수를 쓰는 디버그 기능은 "디버그 전용 PS UAV" 기능을 SS·RN에 따로 요구하고(9.3절 SLR12, 결정 SLD18), 그 기능이 없는 형식(D3D11·GLES 포함)에서는 PS 쪽 호출이 아무것도 하지 않는다(CS 경로는 그대로 동작).
- `Public/Backend/*.rsh`: 형식별 차이를 한 곳에 모은다. UE의 `Public/Platform/<API>/<API>Common.ush`에 해당한다. 1차에 예상하는 내용은 GLES의 정밀도 기본(`precision highp` — SPIRV-Cross가 내므로 원문에는 거의 없다)과 Metal의 texture buffer 차이 정도라 처음에는 파일을 만들지 않고 필요할 때 만든다.

### 3.3 `Shared` 공용 헤더

C++와 HLSL이 같은 정의를 써야 하는 것만 둔다. 파일마다 `#pragma once`, HLSL과 C++ 양쪽에서 컴파일되는 문법만 쓴다.

| 파일 | 내용 | 단계 | UE 대응 |
|---|---|---|---|
| `ShaderTypeAliases.h` | C++ 쪽에서 `float2`·`float4x4`·`uint` 같은 HLSL 이름을 ShaderCore 셰이더 정렬 타입(SS 7.2절 5항)에 연결한다. HLSL 쪽은 비어 있다 | SL0 | `HLSLTypeAliases.h`·`HLSLMathAliases.h` |
| `ShaderStaticAssert.h` | C++는 `static_assert`, HLSL은 빈 매크로(HLSL 2018·FXC에 정적 검사 수단이 없다 — C++ 쪽이 같은 헤더를 검사, 7.8절, 개정 4). 메시지는 영어 | SL0 | `HLSLStaticAssert.h` |
| `ReservedBindingSpaces.h` | Rocks 예약 register space 번호(루트 상수 space 3 등, SS 7.2절 2항) | SL0 | `HLSLReservedSpaces.h` |
| `SurfaceEnums.h` | `ShadingModel`·`BlendMode`·`MaterialDomain` 번호표(RN 결정 RN5) | SL2 | `ShadingCommon.ush`의 번호 정의 |
| `ViewLayout.h` | view 상수 블록 배치·플래그 | SL2 | `ViewDefinitions.h` |
| `GpuSceneLayout.h` | 인스턴스·primitive 기록 배치(RN RS0 `GpuScene`) | SL2 | `SceneDefinitions.h` |
| `InstanceCullingLayout.h` | 컬링 입출력·간접 인자 배치 | SL6 | `InstanceCullingDefinitions.h`·`SceneCullingDefinitions.h` |
| `LightRecordLayout.h` | 광원 기록 배치·광원 종류 번호 | SL3 | `LightDefinitions.h`·`LightSceneData.h`·`LightViewData.h` |
| `LightGridLayout.h` | grid 셀 크기(64 px)·Z 단 수(32)·overflow 표지(RN 8.1절) | SL3 | `LightGridDefinitions.h` |
| `SkinningLayout.h` | 본 행렬 버퍼 배치(RB 결정 R18) | SL10 | `SkinningDefinitions.h` |
| `SplineMeshLayout.h` | spline 변형 파라미터 | SL6 | `SplineMeshShaderParams.h` |
| `EnvironmentFlags.h` | 하늘·안개·대기 사용 플래그 | SL7 | `EnvironmentComponentsFlags.h` |
| `WaterLayout.h` | single layer water 상수 | SL10 | `SingleLayerWaterDefinitions.h` |

C++ 쪽 값은 `RenderData`(번호표)·`Renderer`(배치)가 이 헤더를 include해 `static_assert`로 맞춘다(RN 3.2절과 같은 방식).

### 3.4 셰이더 규약(좌표·단위·정밀도)

| 항목 | 규약 | 근거 |
|---|---|---|
| world 좌표 | 왼손 +X 전방·+Y 오른쪽·+Z 위, cm | EIP 2.2절 |
| 셰이더 world 위치 | 카메라 상대(translated world) float. 절대 world 위치가 필요한 함수(머티리얼 world 위치 노드·world 정렬 텍스처)는 확장 정밀도 값(float 둘, `Math/ExtendedPrecision.rsh`)으로 받는다 | EIP 2.2절 "렌더링은 카메라 상대 float", 결정 SLD6 |
| 행렬 | 행 벡터 `mul(v, M)`, 행 우선 packing(DXC `-Zpr`, SS SC9 `matrixLayout=Row`). 이름은 `<From>To<To>`(예: `TranslatedWorldToClip`) | SS 7.2절 6항 |
| view 공간 | +X 오른쪽·+Y 위·+Z 전방 | RB 1절 |
| clip·NDC | D3D 규약: NDC y 위, z [0,1], reversed-Z(가까움 1·멂 0) 무한 원평면. 깊이 비교는 `GreaterEqual`이 "가깝거나 같음". 선형 깊이 복원 함수는 이 규약 하나만 둔다 | SS 7.2절 8항, EIP 2.2절 |
| 화면 UV | 왼쪽 위 (0,0), 아래로 +v. 픽셀 중심 0.5 | D3D 규약(GLES·Vulkan 차이는 backend가 맞춘다, SS 7.2절 8항) |
| 앞면 | 화면 반시계가 앞, 뒤면 컬. 음수 행렬식이면 컬 반전(C++ 쪽) | RB 1절 |
| 법선 | world 공간 정규화. tangent 공간 법선 맵은 +Z가 표면 밖. 법선 맵 녹색 채널 방향은 UE와 같다(DirectX 규약, 텍스처 임포트가 맞춘다) | RN 14.1절 |
| 광량 단위 | directional lux, point·spot·rect lumen 또는 candela(C++가 셰이더 상수로 바꿔 넘긴다), sky·emissive는 cd/m²(nit) 기준 선형 값 | EIP 14.2절 |
| 노출 | scene color는 pre-exposure를 곱한 값으로 저장한다(UE와 같다). 셰이더는 view 상수의 pre-exposure로 나누거나 곱하는 함수만 쓴다 | RN 11.2절 PC6 |
| 작업 색 공간 | 선형 Rec.709 원색(sRGB primaries)·D65. 다른 작업 색 공간은 후속(A03) | UE 기본 작업 색 공간과 같다 |
| 정밀도 | float(32-bit)만. half·`min16*`는 1차 범위에서 쓰지 않는다 | 결정 SLD4, SS 7.2절 5항 |
| velocity | 현재 표본 → 이전 위치, 정규화 화면 단위(RN 11.2절) | RN 11.2절 |
| cube 면 | 면 순서 +X·−X·+Y·−Y·+Z·−Z(그래픽 API 규약), Z-up world에서 각 면의 방향은 UE capture와 같게 둔다(SL9에서 기준 영상으로 확인) | RN 7.4절 |

### 3.5 이름·코딩 규칙(결정 SLD2)

- **파일.** PascalCase 서술형 이름, 역할이 이름에 드러나게 한다(`UnitVectorEncoding.rsh`, `BuildLightGrid.rsf`). 엔트리 파일(`.rsf`)은 하는 일, include 파일(`.rsh`)은 담는 것으로 짓는다. UE 파일 이름(`Common`, `Definitions`, `MonteCarlo`…)을 그대로 쓰지 않는다. 업계 공용어(GGX·SSAO·FXAA·TAA·HZB·SH)는 이름에 써도 된다.
- **한 파일 여러 엔트리.** 같은 pass 계열의 VS·PS·CS 엔트리는 한 `.rsf`에 함께 둔다(UE와 같다). 엔트리 이름은 동작 + 단계 접미사다: `ResolveDepthPixel`, `BuildLightGridCompute`, `FullscreenTriangleVertex`.
- **함수.** PascalCase 동사구(`EncodeUnitVectorOctahedral`, `EvaluateGgxDistribution`). 반환값 단위가 있으면 이름이나 주석에 단위를 적는다(cm·라디안·nit). 단일 정밀도 근사 함수는 `Fast` 접두를 붙여 정확한 함수와 구분한다.
- **이름 있는 상수.** 의미 있는 수치(스레드 그룹 크기·grid 셀·임계값·물리 상수)는 `static const` 이름 있는 상수다(CodingStyle 이름 있는 상수 규칙). 스레드 그룹 크기는 C++와 공유하면 `Shared/*.h`에 둔다.
- **include.** `.rsh`는 `#pragma once`이고 필요한 것을 스스로 include한다. 절대 가상 경로(`/Engine/Private/Math/BitPacking.rsh`)를 기본으로 쓰고, 같은 폴더 안에서만 상대 경로를 쓴다(SS SC3).
- **주석.** 함수 머리에 목적·인자·반환·범위·근거 문헌을 한국어로 적는다(공개 API 주석 규칙). `#error`·정적 검사 메시지는 영어다.
- **전역 자원 선언.** 라이브러리 `.rsh`는 자원·상수 버퍼를 선언하지 않는다(값을 인자로 받는다). 자원을 쓰는 보조(`Scene/ViewConstants.rsh` 등)는 파라미터 구조체(SS P7)가 생성한 선언을 include하는 쪽에서만 쓴다. 그래서 수학·색·BRDF 라이브러리는 어떤 pass에도 바인딩을 더하지 않는다.
- **분기.** 형식 차이는 `Platform.rsh`·`ShaderFeatures.rsh`의 매크로로만 가른다. 컴파일러 내장 이름(`__SHADER_TARGET_*` 등)은 쓸 수 없다(SS SC3).
