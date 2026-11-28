# 렌더러(Renderer) 설계 — 1. 결정 요약

[목차](README.ko.md)

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 렌더 경로 | `RenderPath::ForwardPlus`·`RenderPath::Deferred` 둘. 고전 Forward는 없다. view마다 고른다 | 사용자 결정 2026-11-16, 계획 11.1절. UE는 한 렌더러 안에서 forward 여부를 플랫폼 설정으로 가르고(`RU:1680–1685`) 모바일은 별도 렌더러다(`R/SceneRendering.h:2957`, `R/SceneRenderBuilder.cpp:499–522`). Rocks는 렌더러 하나에서 view별로 고른다(5절) |
| 모듈 | `Renderer`(렌더 lane 위 장면·pass 조립), `RenderData`(장치 없는 장면 기술 값 타입, shading model·블렌드 모드·영역 번호표의 C++ 정의), `RendererShaders`(장치 없는 pass 셰이더 타입 등록, 쿠커와 공유). 번호표의 HLSL 상수는 `/Engine/Shared/` 공용 헤더다. 의존은 Renderer → RenderGraph → RenderBase → Graphics. World·ObjectRuntime 의존 없음 | 3절. 계획 3.1절 "RenderGraph / Renderer"·"RenderData" 행 |
| 장면 전달 | WorldIntegration이 프레임 packet에 **장면 변경 목록**(추가·제거·변환·재질·광원)을 값으로 담고, 렌더 lane의 `RenderScene`이 프레임 시작에 일괄 적용한다. 게임 쪽 proxy 객체는 없다 | UE도 변경을 큐에 모아 프레임 시작에 일괄 적용한다(`R/RendererScene.cpp:1073, 5534–5596`, `R/SceneRendering.cpp:4378`). UE proxy는 게임 스레드가 만든 C++ 객체다(`R/RendererScene.cpp:1455`). Rocks는 RenderBase 1절 "프레임 packet과 handle만" |
| 깊이 prepass | 두 경로 모두 불투명·masked 전체 depth prepass. HZB는 prepass 깊이로 base pass 전에 만든다 | UE forward는 전체 prepass를 강제하고(`RU:681–700`), 조기 깊이가 완전하면 HZB를 base pass 전에 만든다(`DSR:2781–2786`). 계획 11.4절 공통 전반부 |
| 광원 목록 | 두 경로가 **같은 light grid**(compute 생성, 64 px 셀·로그 Z 32단)를 쓴다 | UE는 deferred에서도 매 프레임 light grid를 만들어 투명·안개가 쓴다(`DSR:2808`, `R/LightGridInjection.cpp:71–85, 627–634`) |
| Deferred 조명 | 전체 화면 **조명 pass 하나**가 directional 목록과 light grid의 local light를 Forward+ base pass와 **같은 조명 함수**로 평가한다(결정 RN1 권장안) | UE 5.8 기본은 광원별 pass(`R/LightRendering.cpp:1627–1844`)이고 grid 기반 deferred는 제거 예정이다(`R/ClusteredDeferredShadingPass.cpp:36–46`). Rocks 자체 판단이며 측정 MR1로 확인한다(7.3절) |
| GBuffer | UE 5.8 Blendable GBuffer와 같은 배치: SceneColor `RGBA16Float`, A 법선 `RGB10A2Unorm`, B `RGBA8Unorm`(metallic·specular·roughness·shading model), C `RGBA8UnormSrgb`(base color·AO), 선택 D·E. 깊이 `D32FloatS8Uint`, velocity는 별도 `RG16Float` | `RC/Private/GBufferInfo.cpp:344–553`, `R/SceneTextures.cpp:60–69, 756–799`. 7.1절 |
| MRT 하한 | 기본 layout은 MRT 4개(SceneColor·A·B·C)로 Vulkan·GLES 최소 보장 안에 든다. D·E가 필요한 기능은 MRT 8 형식에서만 | 형식 정의 `renderTargets`: Vulkan·GLES 4, D3D·Metal 8. UE 모바일 deferred도 기본 4개(`RC/Private/GBufferInfo.cpp:556–584`) |
| 그림자 | CSM 단과 spot·point(6면) 그림자를 shadow atlas 하나에 base pass 전에 그리고(보통 Z·`D16Unorm`, UE와 같다), 두 경로가 조명 함수 안에서 **직접 표본**한다(screen shadow mask 없음) | UE forward는 그림자를 base pass 전에 그리고(`DSR:2922–2927`) directional CSM을 직접 표본한다(`R/SceneRendering.h:765–779`). local light 그림자는 forward 전용 4채널 배정에 기댄다(`R/DynamicShadowMapChannelBindingHelper.h:15–21`). Rocks는 채널 상한을 두지 않는다(9절) |
| 투명 | 두 경로 공유 forward 투명 pass. 정렬, 같은 조명 함수·light grid·그림자 | 계획 11.1절. UE 투명도 base pass 공유 파라미터(light grid 포함)를 쓴다(`R/TranslucentRendering.cpp:1181`, `R/BasePassRendering.cpp:677`) |
| 하늘·안개 | 두 경로 모두 불투명 조명 뒤 전체 화면 pass. 투명은 자기 셰이더에서 안개를 적용한다 | UE는 height fog를 경로와 무관하게 전체 화면 pass로 그린다(`DSR:3823`). sky atmosphere pass는 forward에서 건너뛴다(`DSR:3782`). Rocks는 둘을 같게 한다(결정 RN8) |
| 후처리 | UE 순서: DOF → (TAA·TSR·외부 upscaler) → motion blur → 노출 → bloom → tone map → FXAA → spatial upscale. 계획 12절 `Upscaler`가 temporal 자리를 맡는다 | `R/PostProcess/PostProcessing.cpp:976–2205`. 11절 |
| GI | 정적(None·Baked)·동적(None·VoxelConeTracing·DynamicGI·RayTraced) 두 축. 렌더러는 **간접광 입력 계약** 두 가지(world-space 표본 함수, screen-space 결과 텍스처)만 정하고 두 경로가 같은 결과를 소비한다 | 계획 14.3절. UE는 Lumen이 forward를 지원하지 않는다(`RU:2602–2623`). Rocks는 계획 14.3절대로 두 경로 공통 |
| PathTracing | `RenderMode::PathTracing`은 래스터 pass를 건너뛰고 scene color를 누적 결과로 채운 뒤 같은 후처리로 보낸다 | UE도 base pass·조명·투명·안개를 건너뛰고 후처리로 넘긴다(`DSR:2284–2288, 3058, 3482, 4108–4119, 4241`). 12.3절 |
| 단계당 텍스처(SRV) 상한 | 셰이더 원문은 모든 형식에 하나다. 상한은 **형식 정의**가 정한다: D3D12·Vulkan·Metal은 넓게, GLES·D3D11 SM5는 보장치 16. 렌더러 자원은 16 안에 둔다. 머티리얼 텍스처는 (pass, 퍼뮤테이션, 형식)마다 `형식 상한 − 렌더러 합계`이고, 넘는 머티리얼은 그 형식을 쿠킹할 때 명시 오류다(결정 RN13, 2026-11-18 사용자 결정으로 개정, 7.4절) | UE도 원문은 하나이고 플랫폼 상한을 컴파일 환경에 넣는다(`PLATFORM_MAX_SAMPLERS`, `EN/Private/ShaderCompiler/ShaderCompiler.cpp:3945–3946`). 샘플러 상한은 기본 16, `PCD3D_SM6` 32(`RHI/Private/DataDrivenShaderPlatformInfo.cpp:163`, `Engine/Config/Windows/DataDrivenPlatformInfo.ini:136`), OpenGL은 16 초과가 컴파일 오류다(`Engine/Source/Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:868–877`) |
| 지원하지 않는 조합 | 몰래 낮추지 않는다. 지원표(13절)에 따른 공개 대체 또는 `Unsupported` | 계획 11.1절 "명시적인 fallback 또는 cook 오류", PixelFormat 1절 "대체 금지" |
| 좌표·행렬 | UE 5.8과 같다: 왼손 +X 전방·+Y 오른쪽·+Z 위, cm, 행 벡터 `p·M`, 셰이더 `mul(v, M)`·행 우선, view +X 오른쪽·+Y 위·+Z 전방, reversed-Z 무한 원평면, 앞면 화면 반시계 | 사용자 결정, [셰이더 시스템](../ShaderSystem/07-BindingConventions.ko.md) 7.2절 6·8항, 계획 2.2절. 렌더러의 모든 깊이 비교·HZB·light grid Z·velocity가 이 규약 위에 있다(5.1절) |
