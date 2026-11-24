## 11. 공용 자원·그리기 유틸리티

### 11.1 내장 자원 팩 1차 내용

| 자원 | 값·형식 | UE 5.8 대응 | 비고 |
|---|---|---|---|
| white·black·transparent 2D | 1×1, (1,1,1,1)·(0,0,0,1)·(0,0,0,0), 선형 RGBA8 | `RC/Private/GlobalRenderResources.cpp:47–72, 231–266`(B8G8R8A8, sRGB 아님, point·wrap) | 계획 9.6절 alpha 규칙과 같다 |
| flat-normal 2D | 1×1, 법선 인코딩별 중립값(tangent space (0,0,1)) | 없음. UE Renderer 기본 법선 텍스처는 (128,128,128,255)로 tangent flat normal이 아니다(`Renderer/Private/SystemTextures.cpp:314–320`). 머티리얼용 flat normal은 Engine 에셋 쪽이다 | Rocks 정의 |
| 차원별 대체 | array·3D·cube·cube array·depth·uint 변형 | `RC/Public/GlobalRenderResources.h:54–78` | A0-G 차원 확장 뒤. 9.6절 "2D white를 cube slot에 넣지 않음"과 같다 |
| 0·1 버퍼 | 빈 버퍼, black float4, white(1) | `GlobalRenderResources.h:30–51` | identity 행렬 버퍼는 Rocks 추가(9.6절) |
| 기본 메시 | cube·plane(엔진 기본 정점 입력 타입) | `RC/Public/RenderUtils.h:402–405`(단위 cube 버퍼) | 지금 Sandbox 샘플 렌더러(`SampleRenderer`)의 내장 메시(삼각형·사각형)를 옮긴다. 최소 렌더러는 엔진 모듈이 아니다(2026-11-17 Sandbox로 이동) |
| fallback 셰이더 | 엔진 기본 정점 입력 타입의 VS·PS | (UE 엔진 셰이더 맵) | 엔진 셰이더 집합에서 온다(9.2절) |
| checker·error 텍스처, error material, UI 글꼴 | 9.6절 | UE는 Engine UObject 에셋(`Engine/Classes/Engine/Engine.h:903, 911`) | 에셋·머티리얼·UI 단계에서 팩 범위를 넓힌다. UI 글꼴 구성은 [글꼴 설계](../Font/07-Consumers.ko.md) 7.5절 |
| 빈 light grid | `R32Uint` typed buffer(셀 하나의 머리: 시작 0·개수 0)와 `RGBA32Float` typed buffer(원소 1개, 0) | UE는 원소 하나짜리 기본 버퍼를 묶는다(`Renderer/Private/LightGridInjection.cpp:236–271`) | 렌더러는 이 grid를 묶을 때 `ForwardLight` 상수를 grid 크기 1×1×1·local 광원 0으로 둔다. 셰이더는 셀 좌표를 grid 크기 − 1로 clamp한 뒤 읽으므로(Renderer 8.1절) 모든 픽셀이 셀 0을 읽어 광원 0개를 돈다. 범위 밖 texel fetch 결과에 기대지 않는다(개정 16). Renderer 8.1절 "광원 없는 view", RS1 |
| 빈 그림자 depth | 1×1 `D16Unorm`, 깊이 1.0(보통 Z의 원평면, Renderer 9.2절). 팩을 만들 때 깊이 attachment clear 1로 채운다 | UE는 directional 그림자가 없을 때 검은 텍스처를 묶는다(`Renderer/Private/LightGridInjection.cpp:274–279`) | Rocks는 값이 아니라 비교 결과로 정한다: 그림자 비교(`LessEqual`)는 수신 깊이 [0,1] 전체에서 "빛 받음"이다. 그림자 함수는 비교 전에 수신 깊이를 [0,1]로 clamp한다. 그래서 셰이더가 분기 없이 표본해도 그림자가 생기지 않는다. 비교 샘플러 짝(SX13). RS3 |
| 검은 cube·cube array | 1×1 cube(면 6) (0,0,0,1), 1×1 층 1 cube array (0,0,0,0), 선형 RGBA8 | `RC/Private/GlobalRenderResources.cpp:573`(cube, `FColor::Black` 알파 255), `:578–612`(cube array, (0,0,0,0), 주석 "alpha is used by reflection environment to say how much of the foreground texture is visible" `:597`) | 위 "차원별 대체" 가운데 sky light 자리(cube)와 반사 capture 자리(cube array, Renderer 7.4절)의 값이다. cube array 알파 0은 UE와 같은 이유다: capture 알파는 capture가 덮는 비율이고 남는 비율에 sky를 더하므로(UE `Engine/Shaders/Private/ReflectionEnvironmentComposite.ush:158–163, 242`, Renderer 8.3절), 알파 0이면 capture 기여 0·sky 전부다(개정 16). cube는 2D black과 같게 알파 1(계획 9.6절). cube array는 `DeviceCaps` cube array 능력이 있는 장치에서 만든다(GX1). RS7 |
| 기본 BRDF LUT | 128×32 `RG16Float`. x = NoV, y = roughness(셀 중심), R = 스케일·G = 바이어스(split-sum 환경 BRDF). 시작 때 CPU로 만든다: double 계산, 셀마다 GGX 중요도 표본 128개(Hammersley 수열), Smith 결합 가시성 근사, [0,1] clamp 뒤 half 반올림(round-to-nearest-even) | `Renderer/Private/SystemTextures.cpp:534–670`(128×32, 필터 가능하면 `PF_G16R16` 아니면 `PF_R8G8`, 표본 128개, 시작 때 CPU 생성) | 결정 R14. `RG16Float`은 ES 3.2 core에서 표본·필터가 된다(Renderer 7.1절 velocity 형식과 같은 근거). UE와 달리 장치마다 형식을 바꾸지 않는다. RS7 |

- 지금 null 텍스처를 묶으면 backend가 직접 만든 1×1 white를 쓴다(`Device.h:15–16`, `D3D12Device.cpp:438–445`). Graphics는 RenderBase에 의존할 수 없으므로 backend의 white는 "빈 바인딩 방지" 용도로 남긴다. RenderBase는 null을 묶지 않고 팩의 typed 자원을 명시적으로 묶는다.
- (값, 형식, 차원) 키로 기본 텍스처를 만드는 캐시(UE Renderer `Renderer/Public/SystemTextures.h:46–56`)는 9.6절 typed variant와 같은 역할이며, 화면 마일스톤 뒤 필요할 때 넣는다.
- 위 표의 빈 light grid·빈 그림자 depth·검은 cube·기본 BRDF LUT는 Renderer RQ17로 더했다(개정 15). 렌더러 단계(RS1·RS3·RS7)에서 팩에 넣으며 화면 마일스톤 BR01 판정 범위 밖이다. 생성 실패는 다른 팩 자원과 같이 시작 실패다(6.3절). BRDF LUT 시험은 고정 점 몇 개의 값을 참조값과 half 1 ulp 안으로 대조한다(호스트 수학 함수 차이 허용).

### 11.2 그리기 유틸리티

- **전체화면 pass.** 정점 버퍼 없이 정점 번호로 삼각형 하나를 만드는 속성 없는 파이프라인과 버퍼 없는 draw(GX3·GX4)를 쓴다. 지금은 정점 버퍼가 없으면 그리기를 무시한다(`Device.h:18–20`). UE는 화면 사각형 정점·index 버퍼를 쓴다(`RC/Private/PixelShaderUtils.cpp:20–46`). Rocks는 index 버퍼가 필요 없는 방식을 기본으로 한다.
- **상태 프리셋.** 블렌드·래스터·깊이 desc를 값 타입으로 두고 자주 쓰는 조합(불투명·알파 블렌드·가산, 컬 없음·뒤면 컬, 깊이 끔·읽기·읽기쓰기)을 이름 붙은 상수로 둔다. UE의 정적 상태 템플릿 캐시(`RC/Public/RHIStaticStates.h:20–53`)에 해당하나, Rocks는 상태를 파이프라인에 넣으므로 캐시가 필요 없다.
- **clear.** 전체 clear는 attachment load 동작과 clear 값으로 한다(그래픽 API는 임의 clear 값을 받는다). UE는 viewport 지정이 없고 clear 색이 텍스처의 fast clear 색과 같을 때만 load 동작을 쓰고 나머지는 쿼드 clear로 간다(`RC/Private/RenderGraphUtils.cpp:682–735`). viewport 부분 clear·UAV clear 셰이더는 후속이다.
- **크기 계산.** `MipExtent`·`MipChainBytes`는 이미 있다(`GraphicsTypes/Public/GraphicsTypes/PixelFormat.h`). A0-G 형식 확장 때 형식별 바이트·채널 정보를 같이 늘린다(UE `RC/Public/RenderUtils.h:147–262`).
- **MSAA resolve(Renderer RQ16, 개정 15·16).** Forward+ MSAA view(Renderer 8.2절)가 쓰는 그리기 유틸리티다. MSAA view는 temporal 유무와 무관하게 단일 표본 깊이가 필요하다(HZB·하늘·안개·후처리, Renderer 6.2절). 그래서 깊이 resolve는 늘 하고, velocity resolve는 temporal(또는 motion blur)이 있을 때, 색 resolve는 아래 규칙대로 한다. 깊이·velocity는 평균하면 안 되므로 늘 셰이더로 한다(계획 11.1절 "깊이와 motion vector를 단순 평균하지 않고 전경 sample 선택과 coverage 규칙"). 표본 수 2·4·8은 퍼뮤테이션이다(UE도 표본 수별 셰이더 타입, `RC/Public/ResolveShader.h:20–113`). 구현은 Renderer RS8이고 화면 마일스톤 범위 밖이다.
  - **색(결정 R17).** temporal이 있는 view는 셰이더 휘도 가중 평균으로 고정한다(upscaler 입력이 backend와 무관해야 한다): 표본마다 `c / (1 + luma(c))`(Rec.601 가중 0.299·0.587·0.114)로 바꿔 평균한 뒤 되돌린다. 밝은 표본 하나가 가장자리 평균을 지배하는 것을 막는다. UE desktop scene color resolve와 같은 식이다(`Engine/Shaders/Private/HdrCustomResolveShaders.usf:46–54, 90–144`, 호출 `Renderer/Private/SceneRendering.cpp:6250, 6322`). alpha는 단순 평균이다(UE도 rgb만 가중한다). temporal이 없는 view(품질 profile Forward+ 4x MSAA + native·spatial, 계획 11.1절)는 데스크톱 형식에서 같은 셰이더를 쓰고, tiled GPU에서는 base pass attachment의 resolve(박스 평균)를 허용한다. MSAA 색을 메모리에 쓰지 않아 대역폭이 준다. UE도 모바일은 base pass 렌더 타깃의 resolve 대상으로 색·깊이를 resolve한다(`Renderer/Private/MobileShadingRenderer.cpp:2071, 2085–2087`). tiled GPU 여부는 GX1 계획의 표시(아직 `DeviceCaps`에 없다)로 정하고, 두 결과가 다르므로 Renderer 지원표에 표시한다. GLES의 on-tile resolve 수단(확장 여부)은 RS8에서 측정한다(**미확인**).
  - **깊이.** 깊이 값이 가장 큰 표본(reversed-Z에서 가장 가까운 전경)을 골라 `SV_Depth`로 단일 표본 깊이 attachment에 쓴다(깊이 비교 `Always`, 쓰기 켬). UE 깊이 resolve도 최댓값을 고른다(`Engine/Shaders/Private/ResolvePixelShader.usf:47–57`, 상태 `Renderer/Private/SceneRendering.cpp:6459`). 스텐실은 0을 쓴다(UE도 스텐실 `Always`·`SO_Zero`, 같은 줄). 셰이더가 스텐실을 표본별로 고를 수단이 없고, Forward+ MSAA에서 resolve 뒤 스텐실을 읽는 pass가 없기 때문이다(스텐실을 쓰는 decal 수신은 Deferred GBuffer decal, Renderer 7.2절). 개정 16.
  - **velocity.** 깊이에서 고른 **같은 표본**의 velocity를 쓴다. 깊이가 같은 표본이 여럿이면 번호가 가장 작은 것이다(결정적). 깊이와 velocity를 한 pass에서 resolve한다(MSAA 깊이·velocity SRV → 깊이 attachment + velocity 렌더 타깃). 고른 표본이 velocity를 쓰지 않은 표본(clear 값, 정지 물체)이면 그 값을 그대로 내고, 소비자가 resolve된 깊이와 이전 view 행렬로 계산한다(Renderer 7.1절). UE는 velocity 텍스처가 단일 표본이라 대응이 없다(`Renderer/Private/VelocityRendering.cpp:808–811`, `RC/Public/RenderGraphDefinitions.h:711–723`).
  - **coverage.** 고른 전경 표본과 깊이가 같은 표본의 비율을 선택 출력(`R8Unorm`)으로 낼 수 있게 둔다. temporal upscaler가 이것을 받을지(reactive·가장자리 표시)는 RS8에서 정한다.
  - **입력.** MSAA 텍스처는 표본 번호로 읽는다(HLSL `Texture2DMS.Load`, GLES `sampler2DMS`·`texelFetch`). MSAA 깊이 SRV는 GX2 깊이 읽기 view다(Renderer RQ2).
  - **완료 기준(Renderer RS8).** 표본 배치를 아는 fixture(전경·배경이 섞인 가장자리 픽셀)에서 깊이 = 전경 표본, velocity = 같은 표본의 값, 같은 깊이 표본의 번호 규칙, 색 휘도 가중 결과가 CPU 참조와 같음(D3D12·Vulkan·GLES 미리보기).

### 11.3 완료 기준(RB6)

전체화면 pass 픽셀 대조, 상태 프리셋의 backend 대조(컬링·깊이·블렌드 픽셀), RenderBase 경로의 null 텍스처 바인딩 0건(Development 계수), 11.1절 1차 팩으로 BR01–BR02 판정.
