# 렌더러(Renderer) 설계

기준일: 2026-11-18, 개정 7(초안). UE 5.8 Renderer 모듈이 맡는 일 가운데 **렌더 경로**(Forward+·Deferred)와 그 위에 올라가는 그림자·투명·하늘·안개·후처리의 구조, GI 두 축과 PathTracing 모드의 연결점을 Rocks에 세우는 설계 스펙이다. 범위는 다음과 같다.
- 장면 표현과 게임 → 렌더 전달, view와 가시성
- 공통 frame graph, Forward+와 Deferred가 갈라지는 곳
- GBuffer layout, light grid, 조명 평가, 그림자, 투명·하늘·안개, 후처리·업스케일 연결
- GI 두 축과 PathTracing 모드의 경계(방식 내부는 각 방식의 스펙이 정한다)
- backend 지원표, 렌더러가 머티리얼에 요구하는 계약, 렌더 그래프·셰이더 타입 사용 방식, 성능 예산과 계측
- 단계·완료 기준, 선행 작업에 반영할 요구

**근거 표기.** UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론) 원본이다. 경로는 `Engine/Source/Runtime/` 기준이며 다음 약어를 쓴다.

| 약어 | 경로 |
|---|---|
| `R/` | `Renderer/Private/` |
| `RPub/` | `Renderer/Public/` |
| `DSR` | `Renderer/Private/DeferredShadingRenderer.cpp` |
| `RC/` | `RenderCore/` |
| `RU` | `RenderCore/Private/RenderUtils.cpp` |
| `EN/` | `Engine/` |
| `SH/` | `Engine/Shaders/Private/`(셰이더 원문, `Engine/Source` 밖) |

`Editor/`로 시작하는 경로는 `Engine/Source/Editor/` 기준이다. Rocks 경로는 저장소 루트 기준이고 `44bb1d2`(형식 정의 JSON은 `origin/feature/engine-runtime` `6eeba23`)에서 확인했다. 확인하지 못한 사실은 그 자리에 **미확인**으로 적고 19.2절에 모은다.

**상태.** 이 문서는 **설계**다. 정식 렌더러는 화면 마일스톤 다음 단계다([렌더 기반](RenderBase.ko.md) 18.1절 "범위 밖" 첫 항목). 상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음).

**용어 방침.** UE 구조는 따르되 이름은 Rocks 이름을 쓴다. UE 타입·함수·설정 변수 이름은 용어 대응(2절)과 "UE 5.8 방식" 표·근거 열에만 적는다. GBuffer·Forward+·clustered·light grid·HZB·CSM·shadow atlas·PCF·TAA·tone mapping처럼 업계에서 함께 쓰는 말은 그대로 쓴다. 계획·RenderBase에 이미 있는 Rocks 이름(`RenderPath`·`ViewDesc`·`RenderInstanceId`·`StaticLighting`·`DynamicGiMethod`·`RenderMode`·`Upscaler`·`RayScene`·`GpuScope`·`FramePacket`·`RenderHandle`)은 그대로 쓴다.

**UE 레거시 제외.** UE가 하위 호환이나 제거 예정으로 남긴 경로는 근거로 삼지 않는다(2026-11-16 사용자 지시). 이 문서에 해당하는 것: clustered deferred shading(제거 예정, "low utility and thus use", `R/ClusteredDeferredShadingPass.cpp:36–46`), SSGI(5.8부터 deprecated, `R/IndirectLightRendering.cpp:599`, `EN/Classes/Engine/EngineTypes.h:463`), 레거시 RT 반투명(`EN/Classes/Engine/Scene.h:116`), 독립 RT 반사(선언만 있고 정의 없음, `R/DeferredShadingRenderer.h:1010, 1018`). 다만 UE가 "Legacy"라고 이름 붙인 GBuffer layout 함수(`RC/Private/GBufferInfo.cpp:247`)는 5.8 새 프로젝트 기본값(Substrate Blendable GBuffer)이 그대로 쓰는 현재 경로이므로 근거로 쓴다(7.1절).

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
| 좌표·행렬 | UE 5.8과 같다: 왼손 +X 전방·+Y 오른쪽·+Z 위, cm, 행 벡터 `p·M`, 셰이더 `mul(v, M)`·행 우선, view +X 오른쪽·+Y 위·+Z 전방, reversed-Z 무한 원평면, 앞면 화면 반시계 | 사용자 결정, [셰이더 시스템](ShaderSystem.ko.md) 7.2절 6·8항, 계획 2.2절. 렌더러의 모든 깊이 비교·HZB·light grid Z·velocity가 이 규약 위에 있다(5.1절) |

## 2. 용어 대응

이름은 제안이며 결정 RN11로 확정한다.

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| `Renderer`(모듈) | Renderer 모듈 | 장면·view·pass 조립 |
| `RenderData`(모듈, 계획 3.1) | `FPrimitiveSceneDesc`·`FLightSceneProxy` 생성 입력 일부 | 장치 없는 장면 기술 값 타입과 ID |
| `RendererShaders`(모듈) | Renderer 모듈의 셰이더 타입 등록부(`IMPLEMENT_MATERIAL_SHADER_TYPE`, `R/BasePassRendering.cpp:180–192`) | pass 셰이더 타입·퍼뮤테이션·거르기 함수. 쿠커가 링크한다 |
| 렌더 장면 `RenderScene` | `FScene`(`R/ScenePrivate.h:1624`) | 렌더 lane이 소유하는 장면 표(SoA) |
| 렌더 인스턴스 `RenderInstanceId`·`RenderInstanceDesc` | `FPrimitiveSceneInfo`·`FPrimitiveSceneProxy`(`RPub/PrimitiveSceneInfo.h:269`, `EN/Public/PrimitiveSceneProxy.h:291`) | 그려지는 물체 하나와 그 기술 값 |
| 장면 변경 목록 `SceneChangeList` | `FScenePrimitiveUpdates`·`FSceneUpdateChangeSet`(`R/ScenePrimitiveUpdates.h:54`, `R/RendererScene.cpp:5325`) | 프레임 packet에 담는 추가·제거·갱신 명령 |
| 렌더 광원 `RenderLightId`·`RenderLightDesc` | `FLightSceneInfo`·`FLightSceneProxy`(`R/LightSceneInfo.h:207`, `EN/Public/LightSceneProxy.h:44`) | 광원 하나 |
| `ViewDesc`(계획 11.1) | `FSceneView`(`EN/Public/SceneView.h:1481`) | 게임 쪽 view 기술 |
| 렌더 view `RenderView` | `FViewInfo`(`R/SceneRendering.h:1254`) | 한 프레임 view의 렌더 쪽 상태 |
| view 묶음 `ViewSet` | `FSceneViewFamily`(`EN/Public/SceneView.h:2320`) | 같은 장면 snapshot을 보는 view들 |
| view 이력 `ViewHistory` | `FSceneViewState`, `FPreviousViewInfo`(`R/SceneRendering.h:1078`) | 이전 프레임 HZB·TAA·노출·path tracing 누적 |
| 메시 pass `MeshPass` | `EMeshPass`(`RPub/MeshPassProcessor.h:56–109`) | 메시를 그리는 pass 종류 |
| 그리기 항목 `MeshDrawItem` | `FMeshDrawCommand`(`RPub/MeshPassProcessor.h:1281`) | 파이프라인·바인딩·stream·범위가 확정된 그리기 하나 |
| 그리기 항목 생성기 `MeshPassBuilder` | `FMeshPassProcessor`(`RPub/MeshPassProcessor.h:2258`) | 메시 하나를 pass의 그리기 항목으로 바꾼다 |
| 상태 묶음 `DrawStateBucket` | `FStateBucketMap`·`StateBucketId`(`RPub/MeshPassProcessor.h:1807, 2021`) | 인스턴싱으로 합칠 수 있는 같은 상태 그리기 |
| GPU 장면 `GpuScene`(계획 13.1) | `FGPUScene`(`R/GPUScene.h:218`) | 인스턴스 데이터 GPU 버퍼 |
| light grid(공용어) `LightGrid` | light grid(`R/LightGridInjection.cpp`) | view 공간 clustered 광원 목록 |
| 장면 텍스처 `SceneTextureSet` | `FSceneTextures` | scene color·깊이·GBuffer·velocity 묶음 |
| GBuffer 배치 `GBufferLayout` | `FGBufferInfo`(`RC/Public/GBufferInfo.h:282–291`) | GBuffer 타깃·형식·채널 의미 |
| 표면 속성 `SurfaceAttributes` | `FGBufferData`·`FPixelMaterialInputs` | 머티리얼이 내고 조명이 읽는 표면 값 |
| shading model(공용어) `ShadingModel` | `EMaterialShadingModel`(`EN/Classes/Engine/EngineTypes.h:707–728`) | BRDF 종류 |
| 블렌드 모드 `BlendMode` | `EBlendMode`(`EN/Classes/Engine/EngineTypes.h:245`) | 불투명·masked·투명 종류 |
| 머티리얼 영역 `MaterialDomain` | `EMaterialDomain`(`EN/Public/MaterialDomain.h:12`) | 표면·decal·광원 함수·볼륨·후처리·UI |
| 조명 pass `DeferredLightingPass` | `RenderLights`(`R/LightRendering.cpp:1627`) | Deferred 직접광 평가 |
| shadow atlas(공용어) `ShadowAtlas` | "ShadowDepthAtlas"(`R/ShadowSetup.cpp:6056`) | local light 그림자 depth 모음 |
| 후처리 사슬 `PostChain` | `AddPostProcessingPasses`(`R/PostProcess/PostProcessing.cpp:412`) | 노출·bloom·tone map·AA·upscale |
| 간접광 입력 `IndirectLightingInputs` | Lumen·플러그인 GI 결과와 합성(`R/IndirectLightRendering.cpp:1073`) | GI 방식이 렌더 경로에 넘기는 결과 |
| path tracing 누적 `PathTraceAccumulation` | `FPathTracingState`(`R/PathTracing.cpp:906–950`) | 프레임 사이 누적 표본 |
| `RayScene`(계획 14.4) | `FRayTracingScene`(`R/RayTracing/RayTracingScene.h:46`) | BLAS·TLAS 소유 |

## 3. 모듈 배치

### 3.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 렌더러 선택 | feature level이 SM5 이상이면 deferred 렌더러(desktop forward 포함), 아니면 모바일 렌더러를 만든다 | `R/SceneRenderBuilder.cpp:499–522`, `EN/Public/SceneUtils.h:23–33` |
| desktop forward | 별도 렌더러가 아니다. `r.ForwardShading`(읽기 전용, 플랫폼별 mask)이 `FORWARD_SHADING` 셰이더 정의와 GBuffer 사용 여부를 가른다 | `RU:737–745, 1680–1696`, `EN/Private/ShaderCompiler/ShaderCompiler.cpp:3962–3969` |
| 모바일 | 별도 렌더러 클래스. 자체 forward·deferred 모드 | `R/SceneRendering.h:2957`, `R/MobileShadingRenderer.cpp:404, 1181–1909` |
| 셰이더 타입 | pass 셰이더(머티리얼 결합)가 Renderer 모듈 안에 등록된다 | `R/BasePassRendering.cpp:180–192` |
| GBuffer 메타 | RenderCore에 있다 | `RC/Public/GBufferInfo.h:282` |

### 3.2 Rocks 배치

| 모듈 | 위치 | 직접 의존 | 책임 | 금지 |
|---|---|---|---|---|
| `RenderData` | `Engine/Source/Runtime/RenderData` | Core·MetaSchema | `RenderInstanceDesc`·`RenderLightDesc`·`ViewDesc`·`SceneChangeList` 값 타입, `RenderInstanceId`·`RenderLightId`·`ViewId`, 렌더 전달 가능 등록(RenderBase 5.2절), `ShadingModel`·`BlendMode`·`MaterialDomain` 번호표의 C++ 정의(결정 RN5, 14.2절) | 장치·Object·World |
| `RendererShaders` | `Engine/Source/Runtime/RendererShaders` | ShaderCore·RenderData·MaterialSchema | pass 셰이더 타입·퍼뮤테이션 도메인·거르기 함수(`MaterialShaderTraits`를 받는다, 14.3절), `GBufferLayout` 정의와 shading model의 GBuffer 의미, 파라미터 구조체 선언 | 장치·Renderer |
| `Renderer` | `Engine/Source/Runtime/Renderer` | RendererShaders·RenderGraph·RenderBase·RenderData·MaterialRuntime·TextureStreaming·SpatialIndex·Task·Core | `RenderScene`·`RenderView`·가시성·`GpuScene`·light grid·그림자·두 경로의 pass 조립·`PostChain`·GI 연결점 | World·ObjectRuntime·AssetRuntime |
| `WorldIntegration` | 계획 3.1 | World·RenderData·… | 컴포넌트를 `SceneChangeList`와 `ViewDesc`로 바꾼다. `RenderNotices`를 받아 게임 쪽 해제 | Renderer 내부 주소 |

의존 방향(위가 아래를 쓴다):

```text
WorldIntegration ──▶ RenderData ◀──────────────────────┐
      │                  ▲                              │
      ▼ (프레임 packet)   │ (번호표)                      │
Renderer ──▶ RendererShaders ──▶ MaterialSchema ──┘      │
   │   │            └──────▶ ShaderCore                  │
   │   ├──▶ MaterialRuntime ──▶ RenderBase ──────────────┤
   │   ├──▶ TextureStreaming ──▶ RenderBase              │
   │   └──▶ RenderGraph ──▶ RenderBase ──▶ Graphics      │
   └─────────────────────────────────────────────────────┘
MaterialCook(Host) ──▶ RendererShaders(pass 셰이더 타입 목록)
```

- **번호표 위치(결정 RN5, 머티리얼 시스템 10.4절 D2).** 번호표의 정의 권한은 이 문서(14.2절)에 있다. 코드는 두 곳에 둔다: C++ 열거는 `RenderData`, HLSL 상수는 `/Engine/Shared/`의 C++·HLSL 공용 `.h`(셰이더 시스템 3절)다. C++ 열거는 공용 헤더의 값과 `static_assert`로 맞춘다. MaterialSchema는 `RenderData`만 참조하므로 `RendererShaders` → MaterialSchema 의존과 순환이 생기지 않는다. GBuffer 인코딩 의미(GBufferB.a 4 bit 등)는 `RendererShaders`에 남는다. UE도 C++·셰이더 공용 정의를 `Engine/Shaders/Shared/`에 둔다(예 `Engine/Shaders/Shared/SubstrateDefinitions.h`).

- `RendererShaders`를 따로 두는 이유: 머티리얼 쿠킹은 "이 머티리얼로 컴파일할 pass 셰이더 타입 전체"를 알아야 한다. UE는 그 목록이 Renderer 모듈의 정적 등록이고 에디터·쿠커가 Renderer를 링크한다(`R/BasePassRendering.cpp:180–192`). Rocks 쿠커(Host)는 장치 모듈을 링크하지 않으므로 장치 없는 등록부를 분리한다(결정 RN11).
- RenderBase 14절이 "Renderer 설계"로 미룬 항목(`GBufferInfo`·`RendererInterface`·`SceneRenderingAllocator`·`ReadOnlyCVARCache`, MSAA resolve)은 이 배치를 따른다: GBuffer 메타는 `RendererShaders`, 장면 할당은 RenderBase CP3 프레임 arena, 설정은 ConfigSystem snapshot(RenderBase 12절), MSAA resolve는 RenderBase 그리기 유틸리티(18절 RQ16).
- MaterialRuntime은 Renderer에 의존하지 않는다. Renderer가 MaterialRuntime의 셰이더 집합·파라미터 묶음을 소비한다(14절). 계획 14.6절 MaterialRuntime 의존(Core·RenderData·RHI 계약)과 같은 방향이고, 머티리얼 시스템 3.2절이 그 의존을 MaterialSchema·RenderBase·ShaderCore로 구체화했다.

## 4. 장면 표현과 게임 → 렌더 전달

### 4.1 UE 5.8 방식

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| proxy 생성 | 게임 스레드가 컴포넌트의 `CreateSceneProxy()`로 proxy를 만들고 scene info를 할당한 뒤 render command 하나로 넘긴다 | `R/RendererScene.cpp:1341, 1455, 1471, 1507–1516` |
| 큐 | 렌더 스레드는 추가를 바로 넣지 않고 갱신 큐에 쌓는다 | `R/RendererScene.cpp:1073`, `R/ScenePrimitiveUpdates.h:54` |
| 일괄 적용 | 렌더 시작(`OnRenderBegin`)에서 큐를 프레임 수명 change set으로 옮기고 추가·제거·삭제를 나눠 적용한다. 그 뒤 static mesh 등록 → 그리기 명령 캐시 → GPU Scene 갱신 | `R/SceneRendering.cpp:4378`, `R/RendererScene.cpp:5534–5596, 6446, 6492, 6633–6637, 6792` |
| 변환 갱신 | 변환 명령(world·local bounds, local-to-world)을 큐에 넣는다 | `R/RendererScene.cpp:1572–1579, 1614–1696` |
| 저장 | 장면이 인스턴스별 SoA 배열(변환·proxy·bounds·flags·가시성 ID·occlusion)을 가진다 | `R/ScenePrivate.h:1699–1715` |
| 소유 | 생성 뒤 렌더 스레드 소유. "렌더 스레드는 UObject를 역참조하지 않는다" | `EN/Public/PrimitiveSceneProxy.h:494–495`, 삭제 `R/RendererScene.cpp:6799–6816` |
| 동적 메시 | proxy 가상 함수가 프레임마다 메시 묶음을 낸다 | `EN/Public/PrimitiveSceneProxy.h:504`, `R/SceneVisibility.cpp:4191–4207` |

### 4.2 Rocks 구조

```text
게임 lane(WorldIntegration)                          렌더 lane(Renderer)
컴포넌트 변경 감지 ─▶ SceneChangeList(값) ─┐
카메라 ─▶ ViewDesc 목록 ──────────────────┼─▶ FramePacket ─▶ RenderScene.Apply(변경 목록)
동적 데이터(skin pose·입자) ─▶ RenderBlob ┘                   ─▶ ViewSet 구성 ─▶ 그래프 구성
RenderNotices 처리(해제 완료) ◀──────────────────────────────── 완료 통지
```

- **변경 목록.** `SceneChangeList`는 추가(`RenderInstanceDesc`), 제거(ID), 변환 갱신(local-to-world, bounds, 이전 프레임 변환 보존 표시), 재질 교체(section → 머티리얼 `RenderHandle`), 표시 플래그, 광원 추가·제거·갱신을 담는다. 프레임 packet의 가변 길이 목록이므로 프레임 arena에 둔다(RenderBase 5.2절). UE의 "큐 → 프레임 시작 일괄 적용" 구조는 같고, 게임이 만든 C++ 객체를 넘기지 않는 점이 다르다.
- **ID.** `RenderInstanceId`·`RenderLightId`는 게임 lane이 예약하는 slot+generation이다(RenderBase `RenderHandle`과 같은 방식). 계획 11.4절에 따라 ID domain에 SessionId·WorldEpoch를 넣어 EditorWorld·PIE를 격리한다.
- **`RenderScene`.** 렌더 lane 소유 SoA 표: 변환(현재·이전), world bounds, 메시 `RenderHandle`과 section별 머티리얼, flags(그림자 caster, 정적·이동, velocity 필요, 정적 조명 수신), 정적 조명 데이터 참조, 공간 인덱스 항목(SpatialIndex, 계획 3.1). 적용 순서는 제거 → 추가 → 갱신이고 같은 프레임 안 같은 ID 명령은 목록 순서대로다.
- **인스턴스 종류.** 가상 함수 proxy 대신 렌더러가 정한 **인스턴스 종류**(정적 메시, skinned 메시, 입자 sprite 등)마다 렌더러 쪽 처리기를 등록한다. 종류별 프레임 데이터(skin pose·입자 버퍼)는 `RenderBlob`으로 넘긴다. 1차 종류는 정적 메시 하나이고(17절 RS0) skinned 메시가 RS12다.
- **그리기 항목 캐시.** 정적 인스턴스는 추가될 때 pass별 `MeshDrawItem`을 만들어 캐시한다(UE `R/PrimitiveSceneInfo.cpp:583`, 사용 `R/SceneVisibility.cpp:1269–1305`). 재질·메시·셰이더 집합 epoch가 바뀌면 그 항목만 다시 만든다.
- **이전 프레임 변환.** velocity·TAA는 이전 변환이 필요하다(계획 12.2절). `RenderScene`이 직전 적용 값을 보존하고, 이번 프레임에 변환 갱신이 없으면 이전 = 현재로 둔다. teleport 표시가 있으면 이전 = 현재로 리셋한다.
- **텍스처 사용 기록.** 렌더러는 그린 그리기 항목이 쓴 텍스처의 사용 기록마다 "마지막으로 화면에 그린 프레임 번호"를 `RenderNotices`로 돌려준다(TextureStreaming 4.1절·결정 T3). 상주 계획이 보임 신호로 쓴다(RQ21).
- **해제.** 인스턴스 제거는 렌더 lane에서 즉시 표에서 빼고, 그 인스턴스가 쓰던 GPU Scene 영역은 프레임 slot GPU 완료 뒤 재사용한다(계획 13.1절 "frame overlap 중 같은 buffer 영역을 덮어쓰지 않는다").

## 5. view와 visibility

### 5.1 view

- `RenderView`는 `ViewDesc`(계획 11.1절: 경로·render 크기·출력 크기·sample 수·upscale·카메라)에서 만든다. 행렬은 1절 좌표 규약을 따른다: view space +X 오른쪽·+Y 위·+Z 전방, reversed-Z 무한 원평면 원근(수평 반각), 깊이 [0,1]·가까움 1. 그래서 깊이 clear 값은 0, 깊이 비교는 `GreaterEqual`, base pass(prepass 뒤)는 `Equal`이다.
- jitter는 투영 행렬에 넣고 jitter 없는 행렬을 따로 둔다(계획 12.2절). 이전 프레임 행렬도 jitter 없는 것과 있는 것을 모두 보존한다.
- **큰 월드.** 셰이더 계산은 카메라 상대 float이다(계획 2.2절). 그러나 GPU Scene에 view 원점을 빼서 올리면 view마다 GPU Scene이 따로 필요하고 카메라가 움직일 때마다 전체를 다시 올려야 하므로(4.2절 "변경 범위만 upload"와 충돌) 그렇게 하지 않는다. UE처럼 GPU Scene에는 view와 무관한 world 위치를 상위·하위 두 부분(상위 = 큰 격자 위치 float3, 하위 = 상대 local-to-world float)으로 두고, view 상수에 카메라 이동 보정값(상위·하위)을 둔다. 셰이더는 `(인스턴스 상위 − view 상위) + 상대 변환`으로 카메라 상대 행렬을 만든다. 이전 프레임 변환도 같은 형태로 둔다. UE 근거: GPU Scene의 double-float 위치(`SH/SceneData.ush:153–155`), view의 이동 보정(`EN/Public/SceneView.h:382–401`). 격자 크기·정밀도 시험은 RS0 완료 기준 (e)에 넣는다.
- **view 상수**는 공유 상수 slot(RenderBase 9.5절) `View`로 묶는다. UE view uniform buffer(`EN/Public/SceneView.h:1157`, 생성 `R/SceneRendering.cpp:2397–2432`)에 해당한다.
- **`ViewHistory`**: 이전 HZB, TAA history, 노출 값, occlusion 이력, path tracing 누적. 키는 계획 11.4절대로 (SessionId·WorldEpoch, ViewId+generation, 경로, 크기, sample 수, jitter 수열)다. 경로·크기·sample 수가 바뀌면 history를 버린다.
- **`ViewSet`**: 같은 장면 snapshot을 보는 view들이다. 장면 갱신·GPU Scene·그림자(같은 light/caster revision·projection·coverage·filter 키일 때만, 계획 11.4절)는 한 번 하고 view 크기 타깃·history는 view마다 따로 둔다.

### 5.2 UE 5.8 가시성

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| frustum | 장면 SoA bounds 위 병렬 task(구 판정 뒤 상자-평면 8개). octree는 설정 | `R/SceneVisibility.cpp:603, 860, 3967–3974, 4043, 343, 730` |
| 거리 | 최소·최대 그리기 거리와 fade | `R/SceneVisibility.cpp:1011–1029` |
| 사전 계산 가시성 | 정적 표 조회 | `R/SceneVisibility.cpp:3532, 4494` |
| hardware occlusion query | 이전 프레임 결과를 읽는다(버퍼 1프레임) | `R/SceneVisibility.cpp:2824–2861`, `Core/Private/HAL/ConsoleManager.cpp:4259–4260` |
| HZB | prepass 깊이로 furthest·closest를 만들고 furthest를 다음 프레임용으로 뺀다. 크기는 view 크기를 2의 거듭제곱으로 올린 뒤 절반 | `R/SceneTextureReductions.cpp:111–161`, `DSR:561–610` |
| HZB 위치 | 조기 깊이가 완전하면 base pass 전, 아니면 뒤 | `DSR:2781–2786, 3137–3140` |
| GPU 인스턴스 culling | 이전 프레임 HZB로 판정(occlusion은 기본 끔) | `R/InstanceCulling/InstanceCullingContext.cpp:28–38, 853–858`, `SH/InstanceCulling/BuildInstanceDrawCommands.usf:203–215` |

UE의 occlusion 판정은 전부 이전 프레임 정보에 기댄다. 계획 11.4절은 "이전 HZB에서 탈락한 집합을 최종 폐기하지 않는다"를 요구하므로 Rocks는 아래 2단계 구조를 쓴다. 이 구조는 UE 대응이 없는 자체 설계다(계획 11.4절과 같이 표시).

### 5.3 Rocks 가시성

| 단계 | 내용 | 실행 | 단계(17절) |
|---|---|---|---|
| V1 frustum·거리 | `RenderScene` SoA bounds를 CP2 `ParallelFor`로 판정. 공간 인덱스(SpatialIndex)는 인스턴스 수가 많을 때 후보 축소에만 쓴다 | CPU | RS0 |
| V2 1차 그리기 | 이전 HZB에서 보이는 집합만 depth prepass로 그린다. 이전 HZB에서 가려진 집합은 **보류 집합**으로 남긴다 | GPU(1차는 CPU 판정) | RS4 |
| V3 현재 HZB | 1차 깊이로 HZB(furthest)를 만든다 | GPU compute | RS4 |
| V4 재검사 | 보류 집합을 현재 HZB로 다시 판정해 보이는 것을 같은 프레임 depth prepass에 추가로 그린다. 그 뒤 HZB를 갱신한다 | GPU | RS4 |
| V5 그림자 view | 광원 view마다 별도 frustum 판정(계획 11.4절). HZB는 쓰지 않는다(1차) | CPU | RS3 |

- 보수 규칙: 이전 HZB가 없거나(첫 프레임·cut·teleport·view 크기 변경) 무효이면 V2는 전부 그린다. "가시 물체 누락 0"(계획 13.1절 V_ref ⊆ V_submit)이 기준이다.
- RS0–RS3에서는 V1만 한다. HZB는 RS0에서 이미 만들되(light grid HZB 판정·화면 효과용) 인스턴스 판정에 쓰는 것은 RS4다.
- UE light grid는 furthest HZB가 유효하면 셀 판정에 쓴다(`R/LightGridInjection.cpp:996–1011`). Rocks도 V3의 HZB를 light grid에 쓴다.

## 6. 공통 frame graph

### 6.1 UE 5.8 순서(`FDeferredShadingSceneRenderer::Render`)

| 순서 | 단계 | 근거 | forward에서 |
|---|---|---|---|
| 1 | 장면 갱신·가시성 task 시작 | `DSR:1893`, `R/SceneRendering.cpp:4198, 4363, 4378` | 같다 |
| 2 | GPU Scene 업로드 | `DSR:2234–2255` | 같다 |
| 3 | depth prepass(+ velocity) | `DSR:2450–2504, 2693` | 전체 prepass 강제(`RU:681–700`) |
| 4 | occlusion query·HZB | `DSR:2712–2786` | 같다 |
| 5 | light grid | `DSR:2808` | 같다 |
| 6 | 그림자 depth | forward: base pass 전 `DSR:2922–2927`, deferred: base pass 뒤 `DSR:3292` | 앞당김 |
| 7 | forward 그림자 mask·볼륨 안개 | `DSR:2941–2944` | forward만 |
| 8 | DBuffer decal·forward AO | `DSR:2962`, `R/CompositionLighting/CompositionLighting.cpp:548–593` | AO는 깊이만으로 base pass 전 |
| 9 | base pass | `DSR:3060` | 조명까지 평가(`SH/BasePassPixelShader.usf:203–206, 1366, 1480`) |
| 10 | velocity(필요 시 별도) | `DSR:3373–3376`, `R/VelocityRendering.cpp:30–36` | 같다 |
| 11 | AO·decal(base pass 뒤) | `DSR:3392` | 없음 |
| 12 | 간접광·AO 합성, 직접광 | `DSR:3419–3480`(`RenderLights` 3468) | 건너뜀(`RU:1687–1696`) |
| 13 | 반사·sky 합성 | `DSR:3510–3529` | 건너뜀 |
| 14 | 볼륨 안개·구름 | `DSR:3662–3686` | 7에서 |
| 15 | sky atmosphere·height fog | `DSR:3785, 3823` | sky pass 건너뜀(`DSR:3782`), fog는 그린다 |
| 16 | 투명·distortion | `DSR:4012, 4026` | 같다 |
| 17 | path tracing | `DSR:4108–4119` | 같다 |
| 18 | 후처리 | `DSR:4312` | 같다 |

### 6.2 Rocks 순서

계획 11.4절의 공통 전반부·경로별 중반·공통 후반부를 pass로 풀었다. "공통"은 두 경로가 같은 pass·셰이더를 쓴다는 뜻이다.

| # | pass | Forward+ | Deferred | 출력 | 단계 |
|---|---|---|---|---|---|
| F1 | 장면 변경 적용·가시성 V1 | 공통 | 공통 | 가시 목록 | RS0 |
| F2 | GPU Scene 업로드 | 공통 | 공통 | 인스턴스 버퍼 | RS0 |
| F3 | sky LUT(대기) | 공통 | 공통 | LUT 텍스처 | RS5 |
| F4 | depth prepass(+ 이동 물체 velocity) | 공통(MSAA면 MSAA 깊이) | 공통 | 깊이·velocity·(선택) 보조 법선 | RS0 |
| F5 | HZB(V3)·재검사(V4) | 공통 | 공통 | HZB furthest·closest | RS0·RS4 |
| F6 | light grid | 공통 | 공통 | grid 버퍼 | RS1 |
| F7 | 그림자 depth(CSM 단·local 광원, atlas 하나) | 공통 | 공통 | shadow atlas | RS3 |
| F8 | AO·world-space GI 준비 | AO(깊이·보조 법선)와 GI cache 준비 | GI cache 준비(F11 전이면 되므로 F9 뒤로 옮겨도 된다). AO는 F10 | AO·GI cache | RS7·RS11 |
| F8a | (Forward+) DBuffer decal | 깊이 위에 decal을 DBuffer로 그리고 base pass가 읽는다 | — | DBuffer | RS7 |
| F9 | base pass | 조명 평가 → SceneColor(+ 보조 출력) | GBuffer 쓰기 + emissive·정적 간접광 → SceneColor | — | RS1·RS2 |
| F10 | (Deferred) GBuffer decal·AO | — | GBuffer에 decal 블렌드, AO | — | RS7 |
| F11 | (Deferred) 조명 pass | — | `DeferredLightingPass` | SceneColor | RS2 |
| F12 | 간접광 합성(반사·sky·GI screen-space) | 보조 출력 기반 | GBuffer 기반 | SceneColor | RS7·RS10 |
| F13 | 하늘·height fog(전체 화면) | 공통 | 공통 | SceneColor | RS5 |
| F14 | 투명(Standard)·distortion | 공통 | 공통 | SceneColor | RS5 |
| F15 | `PostChain`(DOF·temporal·투명 AfterDof·motion blur·AfterMotionBlur·노출·bloom·tone map·AA·spatial upscale) | 공통 | 공통 | 출력 색 | RS6 |
| F16 | HUD·UI 합성 → present | 공통 | 공통 | 스왑체인 | 화면 마일스톤 경로 |

- **F7을 base pass 앞에 둔다.** 두 경로가 조명 함수 안에서 그림자를 직접 표본하므로(9절) base pass 전에 있어야 한다. UE forward와 같은 위치다(`DSR:2922–2927`).
- **F8 Forward+ 보조 출력.** Forward+는 GBuffer가 없으므로 screen-space 효과(AO·SSR·contact shadow·screen-space GI)가 켜진 view에서만 depth prepass가 보조 법선·roughness(`RGB10A2Unorm`)를 함께 쓴다(계획 11.1절 "필요한 depth/normal/roughness 보조 출력 선언"). 간접광 합성(F12)이 albedo를 요구하면 base pass가 보조 색 타깃을 하나 더 쓴다(8.4절).
- **decal.** 계획 11.1절은 Deferred에 GBuffer decal, Forward에 mesh decal 또는 pre-shading decal을 요구한다. Deferred는 F10에서 GBuffer에 블렌드하고, Forward+는 F8a에서 DBuffer(base pass 전 decal 버퍼)에 그려 base pass가 읽는다. UE도 DBuffer decal을 base pass 전에 그린다(`DSR:2962`, `R/CompositionLighting/CompositionLighting.cpp:548`). DBuffer는 base pass SRV 예산을 쓰므로 퍼뮤테이션으로 켠다(7.4절).
- **순환 금지.** F12의 screen-space 추적은 F9·F11 뒤의 현재 SceneColor를 읽고, 결과는 별도 기여 버퍼로 합성한다(계획 11.4절 "같은 항을 이중 가산하지 않는다"). 렌더 그래프가 한 pass 안의 같은 subresource 읽기·쓰기를 거부한다(RenderBase 7.2절).
- **path tracing.** `RenderMode::PathTracing` view는 F4–F14를 건너뛰고 누적 pass로 SceneColor를 만든 뒤 F15로 간다(12.3절).

## 7. Deferred

### 7.1 GBuffer layout

**UE 5.8 근거.** `r.GBufferFormat` 기본 1(`R/SceneTextures.cpp:60–69`)의 배치(`RC/Private/GBufferInfo.cpp:247–553`). Substrate는 엔진 기본 꺼짐(`RU:2068–2072`)이고, 새 프로젝트는 Substrate를 켜되 일반 템플릿은 Blendable GBuffer(`r.Substrate.ProjectGBufferFormat=0`), 고급 템플릿은 Adaptive GBuffer로 만든다(`Editor/GameProjectGeneration/Private/GameProjectUtils.cpp:199–212`, 설명 `RU:2074–2083`). Blendable은 아래 배치를 그대로 쓰고 closure를 1개로 강제한다(`RU:2088, 2226–2233`). 배치를 바꾸는 것은 Adaptive 모드(`R32_UINT` 텍스처 배열, `RC/Private/GBufferInfo.cpp:265–283`, `R/Substrate/Substrate.cpp:646–660`)뿐이다. Substrate식 closure 체계는 계획 14.4절의 연구 gate이므로 Rocks는 Blendable과 같은 고정 배치로 시작한다.

| 타깃 | Rocks 형식(PixelFormat 5.2절) | 내용 | UE 5.8 | 조건 |
|---|---|---|---|---|
| MRT0 SceneColor | `RGBA16Float` | emissive·정적 간접광(base pass), 직접광 누적(조명 pass) | `PF_FloatRGBA`(`r.SceneColorFormat` 4, `Core/Private/HAL/ConsoleManager.cpp:4141–4150`) | 항상 |
| MRT1 GBufferA | `RGB10A2Unorm` | RGB = world 법선 `N·0.5+0.5`, A = 인스턴스 표시 2 bit | `:344–345, 363, 424–430`, 인코딩 `SH/DeferredShadingCommon.ush:137–147, 994–1005` | 항상 |
| MRT2 GBufferB | `RGBA8Unorm` | R metallic, G specular, B roughness, A = shading model 4 bit + 선택 출력 표시 4 bit | `:358, 364, 433–458`, 인코딩 `SH/DeferredShadingCommon.ush:295–309` | 항상 |
| MRT3 GBufferC | `RGBA8UnormSrgb` | RGB base color, A 머티리얼 AO | `:366–367, 464–467, 499–502` | 항상 |
| MRT4(아래 번호 규칙) GBufferD | `RGBA8Unorm` | shading model 전용 값(clear coat 등) | `:373, 536–541` | 고급 shading model 사용 profile(RS9) |
| MRT4–5(아래 번호 규칙) GBufferE | `RGBA8Unorm` | 정적 그림자 mask(Mixed 광원 4채널) | `:375–379, 517–524`, 조건 `EN/Private/ShaderCompiler/ShaderGenerationUtil.cpp:2307` | `StaticLighting::Baked` + shadow mask profile(RS7) |
| 깊이 | `D32FloatS8Uint` | reversed-Z 깊이 + 스텐실(shading model 분류·decal 수신·광원 채널) | `PF_DepthStencil`(PixelFormat 10절) | 항상 |
| velocity | `RG16Float`(별도 텍스처, MRT 아님) | 현재 표본 → 이전 위치, 출력 정규화(계획 12.2절) | `PF_G16R16`, OpenGL은 `R16G16_UINT`(`R/VelocityRendering.cpp:786–799`) | 이동 물체가 있을 때 |

- **D·E 번호.** D와 E는 켜진 것만 MRT4부터 차례로 붙는다(D만 → D=4, E만 → E=4, 둘 다 → D=4·E=5). GBuffer profile은 {기본, +D, +E, +D+E} 넷이다. UE 기본 layout은 D를 늘 4에, E를 5에 둔다(`RC/Private/GBufferInfo.cpp:369–379`). Rocks는 고급 shading model이 없는 장면에서 D를 만들지 않으려고 나눈다.
- **GBufferF(UE).** UE deferred는 tangent·이방성용 GBufferF를 데이터 기반 layout 밖에서 따로 만든다(`R/SceneTextures.cpp:791–799`). Rocks는 이방성(14.1절 `tangent`·`anisotropy`)을 후속으로 두므로 1차 layout에 넣지 않는다.
- **MRT 수.** 기본은 4개(MRT0–3)다. D·E를 쓰는 profile은 5–6개이므로 형식 정의 `renderTargets` ≥ 8인 형식에서만 켠다(13절, 결정 RN3·RN4). UE 모바일 deferred도 기본 4개다(`RC/Private/GBufferInfo.cpp:556–584`).
- **velocity를 MRT에 넣지 않는다.** UE 기본도 velocity를 depth pass에서 쓴다(`r.VelocityOutputPass` 0, `R/VelocityRendering.cpp:30–36`). Rocks도 F4에서 이동 물체만 velocity를 쓰고, 정지 물체의 카메라 이동분은 소비자가 깊이와 이전 view 행렬로 계산한다. UE가 정지 물체 velocity를 같은 방식으로 다루는지는 확인하지 못했다(**미확인**).
- **velocity 형식.** UE는 `RG16Unorm` 인코딩과 OpenGL용 정수 형식을 쓴다. Rocks는 모든 backend에서 `RG16Float` 하나다: `RG16Unorm`은 GLES에서 확장(`GL_EXT_texture_norm16`) 조건이고(PixelFormat 8.6절) `RG16Float`는 ES 3.2 core에서 렌더·필터가 된다(같은 표). 결정 RN6.
- **법선 인코딩.** UE desktop은 `N·0.5+0.5`를 10:10:10에 담고 octahedral은 모바일에서만 쓴다(`SH/DeferredShadingCommon.ush:137–147, 610–612, 721–724`). Rocks는 desktop과 같은 방식을 모든 backend에 쓴다(결정 RN2).
- **shading model 번호.** 4 bit(최대 16)다. UE도 16 이하를 정적 검사한다(`EN/Classes/Engine/EngineTypes.h:730`, `SH/ShadingCommon.ush:20–43`). 번호표는 14.2절.
- **장면 텍스처 묶음.** 위 타깃과 AO·HZB를 `SceneTextureSet`으로 묶어 공유 상수 slot `SceneTextures`로 셰이더에 준다(15절).

### 7.2 Deferred base pass

- 깊이 `Equal`·깊이 쓰기 없음(F4 prepass가 완전하므로). UE도 prepass가 완전하면 base pass 깊이를 읽기 전용으로 둔다(`DSR:2105–2114`, `R/RendererScene.cpp:4408–4419`).
- 머티리얼 `SurfaceAttributes`를 받아 GBuffer로 인코딩하는 함수는 렌더러가 소유한다(14절). emissive와 정적 간접광(라이트맵·프로브, 12.1절)은 MRT0에 바로 쓴다. UE도 base pass가 정적 간접광을 scene color에 쓴다(`SH/BasePassPixelShader.usf:554, 1359`).
- 스텐실: decal 수신 bit를 쓴다(GBuffer decal이 쓴다). 조명 pass는 스텐실을 쓰지 않는다(7.3절).

### 7.3 Deferred 조명 pass

**UE 5.8 방식.**

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 정렬 | 광원을 sort key(종류·IES·광원 함수·그림자·batch 가능·MegaLights)로 정렬하고 구간 경계를 계산 | `R/LightSceneInfo.h:80–166`, `R/LightRendering.cpp:1409, 1552–1622` |
| batch 가능 조건 | 그림자 없음·atlas 호환 광원 함수·directional 아님·contact shadow 없음 | `R/LightRendering.cpp:1484–1499` |
| 기본 경로 | batch 가능한 광원도 광원별 그리기, 그림자 광원은 screen shadow mask(`PF_B8G8R8A8`)를 채운 뒤 광원별 그리기 | `R/LightRendering.cpp:1699–1757, 1844, 1895–1908, 2354, 2436–2439` |
| 광원 기하 | directional은 전체 화면, point·rect는 구, spot은 원뿔. 카메라가 볼륨 안이면 컬 뒤집기, 선택 depth bounds | `R/LightRendering.cpp:2666–2679, 3031–3042, 3064–3093` |
| grid 기반 deferred | 있으나 기본 꺼짐·SM6·VSM 플랫폼 조건, "low utility and thus use"로 제거 예정 | `R/ClusteredDeferredShadingPass.cpp:36–65` |
| tiled deferred | 5.8에 없다(셰이더도 clustered만) | `R/` 전체 grep, `R/LightRendering.cpp:1487` 주석만 |
| MegaLights | RT 기반, 프로젝트 기본 꺼짐 | `R/MegaLights/MegaLights.cpp:26–31, 541–549, 2428`, `DSR:3472` |

**Rocks 방식(결정 RN1 권장안).** 전체 화면 pass 하나가 모든 직접광을 평가한다.

```text
DeferredLightingPass (전체 화면 PS, 렌더 타깃은 SceneColor 하나·가산 블렌드, 깊이 attachment 없음)
  depth = 깊이 SRV(픽셀); 원평면(0)이면 종료
  surface = DecodeGBuffer(픽셀); Unlit이면 종료
  for 각 directional 광원(목록, 최대 4):  EvaluateLight(surface, light, CSM 표본)
  cell = LightGridCell(픽셀 위치, view 깊이)
  for 각 local 광원 in cell:             EvaluateLight(surface, light, atlas 표본)
```

- `EvaluateLight`·`EvaluateDirectLighting`(BRDF)·그림자 표본 함수는 Forward+ base pass(8.3절)와 **같은 HLSL 함수**다. 계획 API02("동일 material/mesh의 Deferred/Forward 기준 영상")가 직접광에서 같은 결과를 내게 하는 장치다.
- **깊이·스텐실을 attachment로 붙이지 않는다.** 깊이를 SRV로 읽으면서 같은 텍스처를 attachment로 붙이면 GLES에서는 rendering feedback loop가 되고, 깊이와 스텐실을 동시에 읽을 수도 없다(PixelFormat 10절). 그래서 Unlit·하늘 픽셀은 스텐실 대신 셰이더 분기로 건너뛴다. D3D·Vulkan에서도 같은 셰이더를 쓴다.
- tiled compute deferred는 쓰지 않는다. compute로 SceneColor(`RGBA16Float`, GLES image 가능 형식)를 쓰는 것은 가능하지만, 타일 기반 GPU에서 전체 화면 PS가 대역폭에 유리하고 조명 함수를 Forward+ PS와 그대로 나눠 쓸 수 있기 때문이다. UE 5.8에도 tiled deferred가 없다.
- **광원별 pass를 두지 않는 이유와 위험.** UE 기본(광원별 pass)은 광원마다 GBuffer를 다시 읽는다. B2(광원 100개, 계획 29.2절)와 GLES 대역폭에서 불리하다고 보고 grid 한 번 순회를 고른다. 위험은 셰이더 레지스터 압력과 분기 발산이다. 측정 **MR1**(B2 장면에서 광원별 pass 시제품과 GPU 시간·대역폭 대조)로 확정한다. UE가 grid 기반 deferred를 지우는 이유("low utility")가 성능 때문인지는 원문에 없다(**미확인**).
- **batch되지 않는 광원.** 광원 함수(머티리얼)는 광원 함수 atlas(RS9)로 같은 pass에 넣는다. atlas 전에는 광원 함수가 있는 광원을 `Unsupported`로 보고하고 광원 함수 없이 그리지 않는다(지원표에 "광원 함수 미지원" 표시, 몰래 끄지 않는다).
- **SRV 예산.** 7.4절.

### 7.4 단계당 SRV 예산

**제약(결정 RN13, 2026-11-18 사용자 결정으로 개정).** 셰이더 원문은 모든 형식에 하나다. 단계당 SRV 텍스처(typed buffer 포함) 상한은 **형식 정의**(`sampledTextures`, 셰이더 시스템 12.1절)가 정한다. D3D12·Vulkan·Metal 형식은 넓게, GLES·D3D11 SM5 형식은 보장치 16으로 둔다. 렌더러 pass 셰이더는 GLES에서도 쓰므로 **렌더러 자원은 16 안**에 둔다(아래 표). 머티리얼 텍스처 상한은 (pass, 퍼뮤테이션, 형식)마다 `형식 상한 − 렌더러 합계`다. 그래서 같은 머티리얼이 데스크톱 형식에서는 쿠킹되고 GLES·D3D11 형식에서는 상한을 넘을 수 있으며, 그때는 **그 형식을 쿠킹할 때 명시 오류**다(UE 모바일 머티리얼의 샘플러 제약과 같은 성격, `Engine/Source/Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:868–877`). 이 검사는 셰이더 시스템 7.2절 4항의 프로필 공통 상한(형식들의 최소값)이 아니라 job 형식의 상한으로 한다(RQ22).

**샘플러.** 텍스처·샘플러 1:1 짝 규칙(셰이더 시스템 7.2절 9항)에서는 자기 샘플러를 쓰는 텍스처 수가 형식의 `samplers` 상한에 묶인다. 지금 형식 정의의 `samplers`는 모든 형식이 16이고, Metal은 장치 상한이 16이다(UE `ML_MaxSamplers = 16`, `Apple/MetalRHI/Private/MetalPipeline.h:47`). 그래서 넓은 형식이 실제로 16장을 넘으려면 짝 규칙의 예외인 **공유 샘플러**(UE `SSM_Wrap_WorldGroupSettings`·`SSM_Clamp_WorldGroupSettings`, `EN/Classes/Engine/EngineTypes.h:283–292`, 생성 `EN/Private/Materials/HLSLMaterialTranslator.cpp:7364–7388`)가 필요하다. 셰이더 시스템에서 후속이던 것을 요구로 올렸고(RQ23), 셰이더 시스템 개정 13이 SH2로 앞당겨 규칙을 정했다(셰이더 시스템 7.2절 9항: `SharedWrapSampler`·`SharedClampSampler`, 텍스처마다 출처 하나). 그 전에는 모든 형식에서 짝 규칙대로 `samplers − 렌더러 샘플러 수`가 실제 상한이다.

**줄이는 규칙.** grid의 셀 개수와 광원 index를 한 `R32Uint` 버퍼(앞부분 셀 머리, 뒷부분 목록)로 합친다(8.1절). CSM 단과 local 광원 그림자를 **shadow atlas 하나**에 둔다(9.2절). 광원 함수·IES는 atlas 하나(RS9). 정적 조명은 정책 퍼뮤테이션으로 라이트맵 또는 프로브 하나만 묶는다.

| 자원 | `DeferredLightingPass` | `ForwardOpaque`·`Translucent` |
|---|---|---|
| 깊이 | 1 | 0(contact shadow 퍼뮤테이션은 깊이 복사본 1) |
| GBuffer A·B·C(+D·E) | 3(+2) | 0 |
| AO | 1 | 1 |
| 광원 데이터 `RGBA32Float` | 1 | 1 |
| grid `R32Uint` | 1 | 1 |
| shadow atlas | 1 | 1 |
| 반사 capture cube array | 1 | 1 |
| BRDF LUT | 1 | 1 |
| 정적 조명(라이트맵 또는 프로브) | 0(base pass에서 처리) | 0–2 |
| 동적 GI cache | 0–1 | 0–1 |
| 광원 함수·IES atlas(RS9) | 0–1 | 0–1 |
| DBuffer(Forward+ decal, 퍼뮤테이션) | 0 | 0–3 |
| height fog·aerial perspective LUT | 0 | 0–1(Translucent) |
| **렌더러 합계** | 10–14 | 6–15 |
| 머티리얼 텍스처에 남는 수 | — | 형식 상한 − 렌더러 합계 |

- `GBuffer` pass의 렌더러 합계는 정적 조명(라이트맵 또는 프로브) 0–2뿐이고, depth 계열 pass(`Depth`·`DepthVelocity`·`DepthAux`·`ShadowDepth`·`Velocity`)는 0이다.
- 머티리얼 텍스처가 형식 상한을 넘으면 셰이더 워커의 형식별 상한 검사가 그 (머티리얼, pass, 퍼뮤테이션, 형식)의 쿠킹 오류를 낸다. 머티리얼 컴파일러는 번역 때 프로필 형식마다 이 값을 계산해 편집기·쿠커 진단에 보여 준다(머티리얼 시스템 5.4절 V9, RQ18).
- GLES·D3D11 형식에서는 선택 자원을 모두 켠 조합(합계 15)에 머티리얼 텍스처가 1개만 남고, 정적 조명 2·GI·atlas·DBuffer만 켜도(합계 13) 3개다. 이런 조합의 실제 영향은 머티리얼 시스템 측정 MM5로 본다. 데스크톱 형식은 형식 상한이 넓어 같은 조합에서도 남는다(공유 샘플러 전에는 위 샘플러 문단의 상한).
- bindless(셰이더 시스템 7.2절 10항)는 이 결정 밖의 후속이다.

## 8. Forward+

### 8.1 light grid

**UE 5.8 방식.**

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 셀 | XY 64 px, Z 32단. 셀당 상한 설정(기본 32)이 있으나 연결 목록 방식(기본 켬)에서는 셀 상한이 없어지고 전체 상한(가시 local 광원 수)이 된다 | `R/LightGridInjection.cpp:71–85, 123–135, 1681, 1720–1722` |
| Z 분포 | `slice = log2(z·B + O)·S`, 분포 계수 4.05, 마지막 단은 먼 범위 전용 | `R/LightGridInjection.cpp:627–634`, `RC/Public/RenderUtils.h:721–740`, `SH/LightGridCommon.ush:40–45` |
| 먼 평면 | 가장 먼 광원·반사 capture까지 + 10 | `R/LightGridInjection.cpp:1726–1730` |
| 생성 | compute, 선택 HZB 판정, 선택 2단 grid, 선택 async | `R/LightGridInjection.cpp:47–52, 107–113, 435–610, 937–1047, 1818–1851` |
| 출력 | 셀별 개수 버퍼, 셀별 광원 index(16 bit typed 또는 32 bit structured), 광원 데이터(광원당 float4 6개) | `R/LightGridInjection.cpp:911–935`, `R/BasePassRendering.h:60–69`, `R/SceneRendering.h:728–763` |
| directional | grid 밖 별도 목록과 첫 directional 전용 상수 | `R/SceneRendering.h:728–763` |
| 소비자 | forward base pass, 투명, 볼륨 안개, 반투명 조명 볼륨, 모바일 | `R/BasePassRendering.cpp:677`, `R/TranslucentRendering.cpp:1181`, `R/VolumetricFog.cpp:1790`, `R/MobileShadingRenderer.cpp:1399–1400` |

**Rocks 방식.**
- 셀 64 px × 64 px, Z 32단, 같은 로그 분포 식이다. reversed-Z이므로 셀 Z는 깊이 버퍼 값이 아니라 **view 공간 Z(전방 거리)**로 계산한다(5.1절). 값은 콘솔 설정(ConfigSystem)으로 두고 프레임 packet snapshot으로 넘긴다.
- **생성.** CS 두 단계다. (1) 셀마다 view 공간 광원 bounds(구·원뿔, rect는 구로 보수 근사)를 셀 frustum과 판정해 개수를 센다. HZB(furthest)가 유효하면 셀 깊이 범위를 좁힌다. (2) 개수의 prefix sum으로 셀마다 목록 시작 위치를 정하고 index를 쓴다. 그래서 셀당 상한이 없다(UE 연결 목록 방식과 같은 효과, 전체 상한만 있다).
- **전체 상한 넘침.** 목록 버퍼는 전체 용량을 가진다. 이번 프레임 합계가 용량을 넘으면 넘친 셀에 "넘침" 표시를 하고, 셰이더는 그 셀에서 가시 local 광원 **전체**를 순회한다(느리지만 광원을 빠뜨리지 않는 보수 대체). 넘침 수를 계측에 남기고 다음 프레임에 용량을 늘린다. 계획 13.1절 "overflow 시 객체를 임의로 소실시키지 않고 … 보수적 fallback·다음 frame 재요청"을 따른다.
- **형식.** grid 자원은 모든 backend에서 **typed buffer**다: 광원 데이터 `RGBA32Float`, 셀 머리(시작·개수)와 광원 index를 합친 `R32Uint` 하나(7.4절 SRV 예산). PS는 typed buffer SRV로 읽는다. ES 3.2 PS의 storage block 최소 보장이 0이기 때문이고(셰이더 시스템 7.2절 4항), 계획 11.1절의 GLES "typed buffer(texture buffer)로 읽는다"를 모든 backend로 넓혀 셰이더 경로를 하나로 둔다. CS는 같은 버퍼를 typed UAV로 쓴다. 쓰기 형식을 `R32Uint`·`RGBA32Float`로 고른 이유는 GLES image 형식이 이 둘을 받기 때문이다(PixelFormat 8.6절 "image" 표시, `R16Uint`에는 없다). 두 형식이 GLES texel buffer(읽기) 형식인지는 PixelFormat 8.6절에서 아직 대조하지 않았다(**미확인**, RQ9).
- **광원 데이터.** 광원당 float4 6–8개: 위치·역반경, 색·광량, 방향·spot 각, 종류·그림자 index·IES·광원 함수 index, source 크기. 단위는 계획 14.2절(directional lux, local lumen·candela)이며 렌더러는 GPU에 올리기 전에 내부 단위로 바꾼다.
- 로컬 광원이 없는 view는 grid 생성을 건너뛰고 빈 grid(내장 자원)를 묶는다(계획 11.1절).

### 8.2 Forward+ depth prepass와 MSAA

- 전체 prepass(F4), base pass 깊이 `Equal`·쓰기 없음. UE도 forward에서 전체 prepass를 강제하고 base pass를 깊이 읽기로 둔다(`RU:681–700`, `R/RendererScene.cpp:4408–4419`, `DSR:2105–2114`).
- base pass가 깊이를 읽기 전용 attachment로 붙인 채 같은 깊이를 SRV로 읽는 기능(Forward+ contact shadow)은 GLES에서 feedback loop이므로 깊이 복사본을 읽는다(7.4절). D3D·Vulkan의 읽기 전용 깊이 + SRV 동시 사용은 GX5 요구(RQ6)다.
- MSAA 1·2·4(계획 11.1절, 기본 4x는 품질 profile). UE도 MSAA는 forward 경로만 받는다(`EN/Private/SceneUtils.cpp:184–225`, `EN/Classes/Engine/RendererSettings.h:731–736`). MSAA view는 velocity를 depth pass에서 쓰지 않고 별도 pass로 쓴다(UE 조건 `R/VelocityRendering.cpp:821–826`).
- MSAA와 temporal upscaling을 함께 쓰면 계획 11.1절의 검증된 resolve pass(깊이·velocity는 전경 표본 선택)를 거친다(RQ16).
- masked 머티리얼은 MSAA에서 alpha-to-coverage를 쓴다(GX3 요구, RQ5).

### 8.3 base pass 조명 평가

```text
ForwardOpaquePS
  surface = EvaluateMaterialSurface(...)          // 머티리얼(14절)
  color   = surface.emissive
  color  += SampleStaticIndirect(surface)          // 정적 축(12.1절), 없으면 0
  color  += SampleDynamicGiCache(surface)          // 동적 축 world-space 표본(12.2절), 없으면 0
  color  += EvaluateReflectionAndSky(surface)      // 반사 capture·sky light(RS7)
  for 각 directional: color += EvaluateLight(surface, light, CSM 표본)
  for 각 local in LightGridCell: color += EvaluateLight(surface, light, atlas 표본)
  출력 SceneColor (+ 보조 출력, 8.4절)
```

- `EvaluateLight` 이하는 7.3절과 같은 함수다. UE도 forward base pass가 forward 조명 공통 원문을 include해 같은 grid를 순회한다(`SH/BasePassPixelShader.usf:203–206, 1366, 1480`).
- UE forward는 그림자 광원을 4채널 동적 그림자 채널에 배정하고("This is used in forward only", `R/DynamicShadowMapChannelBindingHelper.h:15–21`) screen-space 그림자 mask 하나(`PF_B8G8R8A8`)에 투영한다(`R/LightGridInjection.cpp:2045–2080`). 채널을 받지 못한 광원의 동작은 확인하지 못했다(**미확인**). Rocks는 atlas 직접 표본이므로 채널 상한이 없다(9절).
- 퍼뮤테이션은 정적 조명 정책(없음·라이트맵·프로브)과 동적 GI 표본 유무로 제한하고, 광원 수·그림자 유무는 동적 분기다(14.3절).

### 8.4 Forward+ 보조 출력

| 출력 | 형식 | 쓰는 pass | 필요한 소비자 |
|---|---|---|---|
| 보조 법선·roughness | 법선 `RGB10A2Unorm`(GBufferA와 같은 인코딩). roughness 자리는 결정 RN12 | F4 depth prepass | AO·SSR·contact shadow·screen-space GI·denoiser |
| 간접광 합성용 색 | `RGBA8UnormSrgb`(diffuse color·specular 강도) | F9 base pass 추가 MRT | F12 screen-space 간접광 합성 |

- 소비자가 없는 view에서는 만들지 않는다(계획 14.2절 "full GBuffer를 의무화하지 않는다"). `RGB10A2Unorm`의 A는 2 bit라 roughness를 담을 수 없으므로 roughness 자리는 RS10에서 정한다(결정 RN12).
- 보조 출력이 있는 Forward+ view의 MRT 수는 2(SceneColor + 색)이고 prepass는 1–2개라 MRT 하한 4 안이다.

### 8.5 투명 처리

10절. Forward+와 Deferred가 같은 pass를 쓴다.

## 9. 그림자

### 9.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| CSM | directional, 기본 최대 10단(scalability 1–10), 해상도 2048, `PF_ShadowDepth` | `R/SceneRendering.cpp:470–475`, `Engine/Config/BaseScalability.ini:135,170,205,243,281`, `Core/Private/HAL/ConsoleManager.cpp:4367–4375`, `R/ShadowSetup.cpp:6154–6202` |
| 분할 | 광원 컴포넌트가 분할 거리 계산 | `EN/Private/Components/DirectionalLightComponent.cpp:786, 928` |
| atlas | 물체별·spot 그림자는 "ShadowDepthAtlas" | `R/ShadowSetup.cpp:5892–5931, 5971, 6056–6075` |
| point | 한 번에 그리는 cube. 정점 셰이더 layer 출력이 없으면 geometry shader로 대체 | `R/ShadowSetup.cpp:6215–6239`, `R/ShadowDepthRendering.cpp:1883`, `RU:1672–1678` |
| 깊이 규약 | 일반 그림자는 보통 Z(`CF_LessEqual`, clear 1), one-pass point만 reversed-Z. 형식 `PF_ShadowDepth`(D16, PixelFormat 10절) | `R/ShadowDepthRendering.cpp:856–865`, `R/ShadowSetup.cpp:6074–6075` |
| 그리기 순서 | VSM → atlas → cube | `R/ShadowDepthRendering.cpp:1836–1884` |
| 투영 | 광원마다 screen shadow mask에 투영(shadow map·VSM·capsule) | `R/ShadowRendering.cpp:2468–2513` |
| VSM | 엔진 기본 꺼짐(새 프로젝트 켬), Nanite·64 bit atomic·forward 아님 필요. 페이지 128², 가상 16k | `R/VirtualShadowMaps/VirtualShadowMapArray.cpp:160–170`, `Editor/GameProjectGeneration/Private/GameProjectUtils.cpp:136–143`, `RU:1274–1328, 1425–1434, 1498–1526`, `Engine/Shaders/Shared/VirtualShadowMapDefinitions.h:12–20` |
| 거리장 그림자 | 기본 켬, mesh distance field 필요(엔진 기본 꺼짐) | `R/DistanceFieldShadowing.cpp:36–40, 798, 1122`, `EN/Private/DistanceFieldAtlas.cpp:55–57` |
| contact shadow | 조명 셰이더 안 screen-space ray | `SH/DeferredLightingCommon.ush:14, 48`, `R/LightRendering.cpp:285, 669` |
| capsule·RT 그림자 | capsule 기본 켬, RT 그림자 기본 꺼짐 | `R/CapsuleShadowRendering.cpp:36–40`, `R/LightRendering.cpp:83–88`, `R/RayTracing/RayTracingShadows.cpp:383` |

### 9.2 Rocks 구조

| 그림자 | 방식 | 표본 | 단계 |
|---|---|---|---|
| directional | CSM, 1차 최대 4단(설정), shadow atlas 타일(단마다 하나), 직교 투영 | 조명 함수 안 직접 PCF(비교 샘플러, SX13) | RS3 |
| spot | atlas 타일, 원근 투영 | 직접 PCF | RS3 |
| point | atlas 타일 6개(면마다 그리기 1회) | 면 선택 후 직접 PCF | RS3 |
| rect | spot과 같은 반구 투영(근사) | 직접 PCF | RS9 |
| contact shadow | 조명 함수 안 screen-space ray(깊이 버퍼) | — | RS10 |
| 가상 그림자(VSM 유사) | 계획 14.2절 별도 모듈 | — | 후속(별도 스펙) |
| 거리장·RT 그림자 | SDF(DynamicGI G3)·`RayScene` 공유 | — | 후속 |

- **screen shadow mask를 두지 않는다.** 두 경로가 `EvaluateLight` 안에서 atlas·CSM을 직접 표본한다. 그래서 그림자가 F7(base pass 전)에 있어야 하고(6.2절), Forward+의 채널 상한이 없다. 대가는 픽셀당 그림자 표본 비용이고, 측정 MR1에 포함한다.
- **point 6면.** 정점 셰이더 layer 출력(`SV_RenderTargetArrayIndex`)은 셰이더 시스템에서 선택 기능·후속이고(7.2절 7항), UE의 geometry shader 대체(`R/ShadowDepthRendering.cpp:1883`)는 Rocks 셰이더 단계(VS·PS·CS)에 없다. 그래서 1차는 면마다 그리기다. layer 출력 지원 장치의 한 번 그리기는 능력 확장이다(결정 RN10).
- **atlas 하나.** CSM 단과 local 광원 그림자를 같은 atlas에 둔다. 단계당 SRV 16 예산 때문이다(7.4절). 단마다 크기가 크므로 atlas 기본 크기는 CSM 단 수 × 단 해상도를 담을 수 있게 정한다.
- **깊이 규약과 형식.** UE를 따른다. 그림자 depth는 보통 Z(가까움 0, `LessEqual`, clear 1)이고 형식은 `D16Unorm`이다(PixelFormat 10절 그림자 기본). 화면 깊이(reversed-Z 무한 원평면)와 다른 이유는 그림자 투영은 유한 원평면이고 UE도 그렇게 쓰기 때문이다. 정밀도가 부족한 장면용 `D32Float` atlas는 설정으로 둔다. 비교 샘플러 함수는 이 규약에 맞춘다(SX13). slope-scaled bias·직교 투영의 깊이 clamp(pancaking)가 GX3에 필요하다(RQ5). 깊이 clamp 능력이 없는 backend는 그림자 depth 정점 셰이더가 깊이를 clamp하는 셰이더 대체를 쓰고 지원표에 그렇게 표시한다(Rocks 설계, UE 대응은 확인하지 않았다).
- **caster 판정.** 광원 view마다 별도 frustum 판정(V5)이고 caster 집합·revision이 같은 view 묶음끼리만 그림자를 공유한다(계획 11.4절).
- **예산.** atlas 크기·CSM 해상도·광원당 해상도는 화면 크기 기반 할당이다. atlas가 차면 낮은 우선 광원부터 해상도를 내리고, 그래도 넘치면 그 광원을 "그림자 없음"으로 그리되 overflow 계수를 올린다(조용히 빠뜨리지 않는다).
- **정적 그림자.** `StaticLighting::Baked`의 Mixed 광원은 정적 caster 그림자를 shadow mask(Deferred GBufferE, Forward+ base pass 직접 표본)로 받고 이동 caster만 동적 그림자로 그린다(계획 14.10절). 채널 4개 상한과 overflow 처리는 bake 스펙(14.10절)과 같이 정한다.

## 10. 투명·하늘·안개

### 10.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 투명 pass 종류 | Standard(DOF 전), Standard Modulate, AfterDOF(+Modulate), AfterMotionBlur, Holdout, All | `Renderer/Internal/TranslucentPassResource.h:12–23` |
| 위치 | 투명 → distortion → light shaft bloom → 투명 upscale | `DSR:4012, 4026, 4095, 4101` |
| 해상도 | 별도 투명 화면 비율(기본 100) | `R/TranslucentRendering.cpp:89–95, 324, 2250` |
| 정렬 | 그리기 명령 정렬 키, OIT 선택 | `R/MeshDrawCommands.cpp:1137–1166`, `R/OIT/OIT.cpp:24, 52` |
| sky atmosphere | LUT 5종(transmittance·다중 산란·먼 하늘빛·sky view·aerial perspective 볼륨), deferred만 전체 화면 pass | `R/SkyAtmosphereRendering.cpp:925–1081, 1478, 2155`, `DSR:3782–3785` |
| height fog | 전체 화면 pass(경로 무관) | `R/FogRendering.cpp:276, 688`, `DSR:3823` |
| 볼륨 안개 | froxel(16 px, Z 64), light grid 사용 | `R/VolumetricFog.cpp:118–129, 576, 1375, 1497`, `SH/VolumetricFog.usf:1054–1055` |
| 볼륨 구름 | 별도 렌더 타깃에 그린 뒤 합성 | `R/VolumetricCloudRendering.cpp:1695, 2702`, `R/VolumetricRenderTarget.cpp:1033` |

### 10.2 Rocks 구조

- **투명 pass 종류.** `TranslucentStandard`(DOF 전), `TranslucentAfterDof`, `TranslucentAfterMotionBlur` 셋이다. `Modulate` 블렌드 모드는 이 세 pass 안에서 그린다(RS5). UE의 Modulate 짝 pass(`…Modulate`, 분리 투명의 modulate를 따로 합성)는 후속이고, Holdout pass는 `AlphaHoldout` 블렌드 모드와 함께 후속이다(14.2절). 머티리얼이 어느 pass에 들어갈지는 머티리얼 설정(`MaterialShaderTraits`의 투명 pass 종류)이다(14.5절).
- **조명.** 투명은 Forward+ base pass와 같은 조명 함수(8.3절)로 표면 조명을 평가한다. 반투명 조명 볼륨(UE `RenderTranslucencyLightingVolume`, `DSR:3479`)은 후속이다.
- **정렬.** 인스턴스 bounds 중심의 view 깊이로 뒤 → 앞 정렬, 같은 깊이는 `RenderInstanceId` 순서로 결정적이다. 인스턴스 안 삼각형 정렬과 OIT는 후속(계획 14.4절 "OIT는 선택 후속").
- **깊이.** 투명은 깊이 쓰기 없음·깊이 `GreaterEqual` 읽기다. velocity는 머티리얼 설정이 켠 경우에만 별도 pass로 쓴다(계획 14.4절 "velocity/DOF 정책").
- **안개 적용.** 투명은 자기 셰이더에서 height fog·aerial perspective를 적용한다(불투명 전체 화면 안개를 덮어쓰지 않으므로).
- **distortion·굴절.** 투명 뒤 별도 pass(SceneColor 복사본을 읽고 offset 버퍼로 합성). 복사는 그래프 Copy pass다.
- **하늘·안개(결정 RN8).** 두 경로 모두 불투명 조명 뒤(F13) 전체 화면 pass로 sky atmosphere와 height fog를 그린다. sky는 깊이가 원평면(reversed-Z 0)인 픽셀만, fog는 깊이에서 거리를 복원해 SceneColor에 혼합한다. UE forward는 sky atmosphere 전체 화면 pass를 건너뛰는데(`DSR:3782`) 그 경우 하늘을 무엇이 그리는지는 확인하지 못했다(**미확인**). Rocks는 두 경로 결과를 같게 하려고 둘 다 pass로 그린다.
- 볼륨 안개(light grid 사용)·볼륨 구름·지역 안개 볼륨은 후속이며 각 스펙이 F13 앞뒤 위치를 정한다.

## 11. 후처리와 업스케일 연결점

### 11.1 UE 5.8 순서

`R/PostProcess/PostProcessing.cpp:412–2265`(호출 `DSR:4312`): DOF `:976` → 색 보정 LUT `:1089` → TAA·TSR·외부 temporal upscaler `:1107–1184` → motion blur `:1269`(AfterMotionBlur 투명 `:1253`) → 축소 사슬 `:1330–1351` → 지역 노출 `:1369` → histogram `:1406` → 노출 `:1440–1461` → bloom(FFT `:1506` 또는 Gaussian `:1555–1564`) → lens flare `:1571` → tone map `:1629` → FXAA `:1648`·SMAA `:1661` → tone map 뒤 후처리 머티리얼 → primary upscale `:2143–2175` → secondary upscale `:2179–2205`.

- 해상도: TSR까지는 render 해상도, TSR은 출력(secondary) 해상도로 쓰므로 그 뒤 motion blur·bloom·tone map은 출력 해상도다(`R/PostProcess/TemporalSuperResolution.cpp:1982`). spatial 경로는 primary upscale에서 바뀐다(`R/PostProcess/PostProcessing.cpp:683`, `EN/Public/SceneView.h:136–147`).
- 외부 upscaler interface: temporal(`RPub/TemporalUpscaler.h:12, 75`), spatial(`R/PostProcess/PostProcessUpscale.h:41, 61`).
- HDR 출력: tone map이 출력 장치 형식(ST2084·scRGB 등)을 LUT 단계에서 인코딩(`R/PostProcess/PostProcessTonemap.cpp:246`, `Core/Public/GenericPlatform/GenericPlatformMisc.h:472–486`, `SH/PostProcessCombineLUTs.usf:434, 440`).

### 11.2 Rocks `PostChain`

| # | pass | 해상도 | 입력 | 단계 |
|---|---|---|---|---|
| PC1 | DOF(후속) | render | SceneColor·깊이 | 계획 26절 |
| PC2 | `TranslucentAfterDof` 합성 | render | — | RS5 |
| PC3 | temporal: `Upscaler::AddPasses`(계획 12.1절의 TAA·TAAU·FSR·DLSS·XeSS) | render → 출력 | `UpscaleInputs`: HDR 색·깊이·velocity·노출·reactive·투명 mask·jitter·reset | RS6(U1) |
| PC4 | motion blur(후속) | 출력 | velocity | 계획 26절 |
| PC5 | `TranslucentAfterMotionBlur` 합성 | 출력 | — | RS5 |
| PC6 | 노출(histogram → 자동 노출) | 출력 | — | RS6 |
| PC7 | bloom(Gaussian 1차) | 출력 | — | RS6 |
| PC8 | tone map + 색 보정 LUT + 출력 인코딩(SDR sRGB, HDR은 계획 26절) | 출력 | 노출 | RS0(최소)·RS6 |
| PC9 | spatial AA(FXAA, 선택) | 출력 | — | RS6 |
| PC10 | spatial upscale(Native·Spatial 모드, 계획 12.1절 U0) | render → 출력 | — | RS6 |
| PC11 | HUD·UI 합성 | 출력 | — | 화면 마일스톤 경로 |

- **temporal 자리.** 계획 12.2절 "선형 HDR color, depth, motion, exposure가 유효한 단계에서 실행하고 tone mapping·출력 UI는 그 뒤"를 따른다. `UpscaleInputs`의 노출은 이전 프레임 노출 값이다(이번 프레임 PC6이 뒤에 있으므로). UE도 temporal 단계 뒤에 노출을 계산한다(`:1107–1184` → `:1440`).
- **velocity 규약.** 렌더러 velocity는 계획 12.2절의 엔진 공통 규약(현재 표본 → 이전 위치, 출력 정규화)이고 adapter가 SDK 단위로 바꾼다. reversed-Z 깊이는 adapter가 SDK 깊이 규약 표시로 넘긴다(계획 12.1절).
- **텍스처 mip bias.** temporal upscale(PC3)을 쓰는 view는 머티리얼 텍스처 표본에 음수 mip bias(log2(render 폭 ÷ 출력 폭))를 view 상수로 건다. 텍스처 스트리밍의 표본 해상도 계산(TextureStreaming 4.1절 "렌더 폭 × 2^(−mip bias)")이 이 값을 쓰므로 렌더러가 view마다 그 값을 사용 기록과 함께 넘긴다(RQ21).
- **경로 독립.** `PostChain`은 SceneColor·깊이·velocity만 받으므로 Forward+·Deferred·PathTracing이 같은 사슬을 쓴다. 경로 전환·upscaler 교체·크기 변경은 history reset이다(계획 12.2절).
- 세부 효과(DOF·motion blur·지역 노출·lens flare·film grain·후처리 머티리얼)는 계획 26절 후처리 catalog 스펙이 정한다. 이 문서는 자리(해상도·입력)만 정한다.

## 12. GI 두 축과 PathTracing 모드의 연결점

### 12.1 정적 축(`StaticLighting`)

**UE 5.8 근거.** 정적 조명은 base pass가 표본한다: 라이트맵 정책 선택(`R/BasePassRendering.cpp:2164–2255`, 종류 `R/LightMapRendering.h:347–366`), 상수(`EN/Public/LightmapUniformShaderParameters.h:14–25`), 셰이더 표본(`SH/BasePassPixelShader.usf:554–731`, volumetric lightmap `:572–603`, 2D 라이트맵 `:724, 731`), 정적 그림자 mask → GBufferE(`:1154`). Lumen diffuse GI가 켜진 view는 정적 간접광 배율을 0으로 둔다(`R/SceneRendering.cpp:2052–2058`, 주석 "this has the side effect of removing direct lighting from Static Lights"). Lumen 반사가 켜지면 반사 capture 배율도 0이다(`:2061–2067`).

**Rocks 연결점.**
- `StaticLighting::Baked`면 base pass(두 경로)가 `SampleStaticIndirect(surface)`로 라이트맵 또는 프로브를 표본해 SceneColor에 더한다. Deferred도 base pass에서 더한다(UE와 같다).
- 퍼뮤테이션: 정적 조명 정책 차원 {없음, 라이트맵, 프로브}(14.3절). 정책 선택은 인스턴스의 정적 조명 데이터 유무로 렌더러가 한다(UE `GetUniformLightMapPolicyType`와 같은 역할).
- 데이터 형식(라이트맵 계수·프로브 SH)과 bake는 계획 14.10절 bake 스펙이 정한다. 렌더러는 상수 slot `PrecomputedLighting`의 자리만 둔다.
- **이중 가산 금지.** 동적 GI가 켜진 view에서 정적 간접광을 쓰는 규칙(무시·움직이지 않는 객체만·혼합)은 방식별 지원표가 정한다(계획 14.3절). 렌더러는 view 상수에 **정적 간접 diffuse 배율·정적 반사 배율** 두 값을 두고 방식이 정한 값으로 채운다. UE의 배율 두 개와 같은 장치다.

### 12.2 동적 축(`DynamicGiMethod`)

**UE 5.8 근거.** 방식 선택은 view마다 한 번(`R/IndirectLightRendering.cpp:570–645`, 호출 `DSR:1403`)이고 선택 결과로 HZB 요구(furthest·closest)를 정한다(`DSR:1411–1433`). Lumen 진입점: 장면 갱신(`DSR:1967, 2895`), 장면 조명(`DSR:3042`, base pass 전), 간접광 계산·합성(`DSR:3430, 3519`, `R/IndirectLightRendering.cpp:1073–1262`), 투명 앞층(`DSR:3497, 3996`). 소비 자원: scene textures(GBuffer·깊이), HZB, velocity, distance field 또는 RT TLAS. 외부 GI plugin hook은 GBuffer A·B·C·깊이·SceneColor를 넘기는 delegate다(`R/DeferredShadingRenderer.h:225–255`, broadcast `R/IndirectLightRendering.cpp:1249–1262`). Lumen은 forward를 지원하지 않는다(`RU:2622`).

**Rocks 간접광 입력 계약.** 모든 동적 GI 방식(VoxelConeTracing·DynamicGI·RayTraced)은 아래 두 제품 가운데 하나 이상을 낸다. 두 경로가 같은 제품을 같은 함수로 소비한다(계획 14.3절 "모든 방식은 Forward+와 Deferred가 같은 GI 결과를 조명 입력으로 소비").

| 제품 | 뜻 | 생성 시점 | 소비(Forward+) | 소비(Deferred) |
|---|---|---|---|---|
| world-space 표본 함수 `SampleDynamicGiCache(surface)` | voxel·probe·radiance cache 등 world 공간 표현의 표본 | F8(base pass 전) | base pass 안(8.3절) | 조명 pass 안(7.3절과 같은 자리) |
| screen-space 결과 `IndirectDiffuse`·`IndirectSpecular` | 픽셀별 간접 diffuse·specular(denoise 뒤) | F12(현재 조명 뒤) | 보조 출력 색(8.4절)으로 합성 | GBuffer로 합성 |

- **방식이 받는 입력**(`IndirectLightingInputs`): 깊이·HZB(furthest·closest 선택), 법선·roughness(Deferred GBuffer A·B, Forward+ 보조 출력), velocity, 현재 SceneColor(screen trace), `RayScene`(RayTraced·하드웨어 trace), 장면 표현(SDF·voxel은 방식 소유).
- **방식 선택과 낮춤.** view마다 하나, 능력이 없으면 profile 순서로 낮추고 결과를 resolved profile에 남긴다(계획 14.3절). 렌더러는 선택 결과로 F5 HZB 종류·F4 보조 출력·F12 합성 pass 유무를 정한다(UE `DSR:1411–1433`과 같은 방식).
- **반사.** 반사 방식은 GI와 같은 두 축의 별도 선택이다(계획 14.3절). 1차 반사는 반사 capture + sky light(RS7), SSR은 screen-space 결과 제품의 specular로 들어간다(RS10).
- **AO.** SSAO·GTAO는 렌더러 공통 효과(RS7)이고 GI 방식이 자기 AO를 내면 view 선택으로 대체한다(UE 규칙 `R/IndirectLightRendering.cpp:608–627`과 같은 역할).

### 12.3 RayTraced와 PathTracing

- **`RayScene`.** BLAS·TLAS 갱신은 F2(GPU Scene) 뒤, base pass와 겹칠 수 있는 위치에 둔다. UE도 base pass와 async로 겹친다(`DSR:2983`, `R/RayTracing/RayTracing.cpp:1993`, `R/RayTracing/RayTracingScene.cpp:223, 506`). `RayScene` 자체는 계획 14.4절 별도 스펙이다. RT를 쓰는 효과가 하나도 없는 view에서는 만들지 않는다(UE `DSR:4405–4432`).
- **`DynamicGiMethod::RayTraced`**는 `RayScene`을 받아 screen-space 결과 제품을 낸다.
- **`RenderMode::PathTracing`.** view 단위 렌더 모드다.

| 항목 | Rocks | UE 5.8 근거 |
|---|---|---|
| 건너뛰는 것 | depth prepass 이후 F4–F14 전부(base pass·조명·투명·안개·하늘·화면 효과) | `DSR:2284–2288, 3058, 3482, 3735–3852`, `R/CompositionLighting/CompositionLighting.cpp:550, 604, 699` |
| 유지하는 것 | F1·F2·`RayScene`, `PostChain`(F15) | `DSR:4241`(후처리 입력) |
| 누적 | `ViewHistory`의 `PathTraceAccumulation`(radiance·분산·albedo·법선·깊이, 표본 수). 목표 표본 수까지 누적 | `R/PathTracing.cpp:906–965, 4257–4261, 4690` |
| 무효화 | 장면 변경·카메라 변경·설정 변경이 누적을 리셋 | `DSR:1119` |
| denoiser | 외부 공급자 interface(공간·시공간), albedo·법선 보조 입력 | `RPub/PathTracingDenoiser.h:20, 42, 112–118` |
| 요구 | 하드웨어 RT 또는 compute fallback(계획 14.3절). 없으면 `Unsupported`이고 Raster로 몰래 바꾸지 않는다 | — |

- PathTracing의 depth prepass·velocity 유지 여부는 UE에서 확인하지 못했다(**미확인**). Rocks는 화면 출력·피킹 일관성을 위해 깊이를 path tracer의 1차 hit 깊이로 채운다(설계, 후속 스펙에서 확정).

## 13. backend 지원표

**근거 종류.** "형식 정의"는 `Engine/Config/ShaderFormats/<id>.json`의 값(셰이더 시스템 12.1절), "사양"은 API 사양 최소 보장, "PixelFormat"은 [픽셀 형식](PixelFormat.ko.md)의 backend 표다. 표의 ○는 그 근거로 가능, △는 장치 능력(GX1)에 따라, ×는 불가, "미확인"은 확인하지 못함이다. 지원하지 않는 칸은 13.2절 처리 규칙을 따른다.

### 13.1 기능별 표

| 기능 | D3D12(`D3D12_SM6`) | D3D11(`D3D11_SM5`) | Vulkan(`VULKAN_SM5`) | Metal(`METAL_SM5`) | GLES(`GLES31_*`) | 근거 |
|---|---|---|---|---|---|---|
| compute(light grid·HZB·AO) | ○ | ○(CS 5.0) | ○ | ○ | ○(ES 3.1+) | 형식 정의 `features: compute`, 계획 11.1절 |
| PS typed buffer 읽기(grid·GPU Scene) | ○ | ○ | ○ | ○ | ○(texture buffer, 형식 대조 미확인) | 셰이더 시스템 7.2절 3·4항, PixelFormat 8.6절 |
| VS·PS storage buffer | ○ | ○ | ○(단계당 4) | ○(4) | ×(VS·PS 0) | 형식 정의 `storageBuffers`(GLES 형식의 값 4는 CS 기준), VS·PS 0은 셰이더 시스템 7.2절 4항 "구조화·raw 버퍼 SRV도 GLES 형식에서는 CS만" |
| PS·VS UAV 쓰기 | ×(규약) | ×(규약) | ×(규약) | ×(규약) | × | 셰이더 시스템 7.2절 4항 "UAV는 CS만" |
| 단계당 SRV 텍스처 | 넓음(128) | 16 | 넓음(장치 요구, RQ22) | 넓음(128) | 16 | 형식 정의 `sampledTextures`(결정 RN13 개정). D3D11은 셰이더 시스템 개정 13에서 16으로, Vulkan은 개정 14에서 128로 고쳤다(RQ22). 렌더러 자원은 모든 형식에서 16 안(7.4절) |
| 단계당 샘플러 | 16 | 16 | 16 | 16(장치 상한) | 16 | 형식 정의 `samplers`. 짝 규칙에서는 16장을 넘는 머티리얼 텍스처에 공유 샘플러가 필요하다(7.4절, RQ23) |
| MRT 수 | 8 | 8 | 8(결정 RN3, RQ11 반영: 셰이더 시스템 개정 14) | 8 | 4 | 형식 정의 `renderTargets` |
| Deferred 기본 layout(MRT 4) | ○ | ○ | ○ | ○ | ○ | 7.1절 |
| Deferred D·E(MRT 5–6) | ○ | ○ | ○(RQ11 뒤) | ○ | ×(결정 RN4) | 7.1절 |
| `D32FloatS8Uint` 깊이 | ○ | ○ | △(장치 질의) | ○ | ○(core) | PixelFormat 8.4–8.7·10절. Vulkan은 GX1 형식 질의로 확인하고, 없으면 그 장치에서 Deferred·Forward+ 모두 `Unsupported`(대체 형식 선택 여부는 PixelFormat 10절 결정을 따른다) |
| `RGBA16Float` SceneColor(렌더·블렌드·필터) | ○ | ○ | ○ | ○ | ○(core) | PixelFormat 8.6절 |
| `RG16Float` velocity | ○ | ○ | ○ | ○ | ○(core) | PixelFormat 8.6절 |
| `RGB10A2Unorm` GBufferA | ○ | ○ | ○ | ○ | ○(core) | PixelFormat 8.6절 |
| 비교 샘플러(PCF) | ○ | ○ | ○ | ○ | ○ | SX13, RenderBase 9.6절 |
| Forward+ MSAA 2·4 | △ | △ | △ | △ | △ | 형식별 `Multisample` 능력(PixelFormat 7절) |
| layer 출력(point 한 번 그리기) | △ | △ | △ | 미확인 | ×(형식 단계 VS·PS·CS) | 셰이더 시스템 2절·7.2절 7항 |
| 깊이 clamp(CSM pancaking) | ○ | ○ | △(`depthClamp` 기능) | 미확인 | 미확인(ES 3.2 core 여부·확장 미확인) | RQ5. D3D는 rasterizer depth clip 끄기. 없는 backend는 셰이더에서 깊이를 clamp하는 공개 대체(9.2절) |
| indirect draw·dispatch | ○ | ○ | ○ | ○ | ○(단일 draw, multi-draw는 확장) | RQ3. GLES 3.1 `glDrawElementsIndirect` |
| async compute(RG4) | △ | × | △ | △ | × | RenderBase RG4, 계획 11.5절 |
| framebuffer fetch·subpass(GLES deferred 1-pass) | — | — | △(GX13) | 미확인 | △(확장, GX13) | RenderBase GX13 |
| 64 bit atomic(가상 그림자 유사) | △(SM 6.6 능력) | × | △(확장) | 미확인 | × | UE VSM 조건 `RU:1274–1328` |
| 하드웨어 RT(`RayScene`) | △ | × | △ | △ | × | 계획 14.4절. UE `RU:1903–1935` |
| HDR 출력(ST2084·scRGB) | △ | △ | △ | 미확인 | 미확인 | 계획 26절 |
| image 쓰기 `RG11B10Ufloat` | ○ | ○ | △ | 미확인 | ×(형식 정의 `unsupportedImageFormats`) | SceneColor를 compute로 쓰는 효과는 `RGBA16Float` 사용 |

### 13.2 경로별 표

| 경로·기능 | D3D12 | D3D11 | Vulkan | Metal | GLES |
|---|---|---|---|---|---|
| Forward+(직접광·그림자·투명) | ○ | ○ | ○ | ○ | ○ |
| Forward+ MSAA | △ | △ | △ | △ | △ |
| Deferred 기본 shading model | ○ | ○ | ○ | ○ | ○(다중 pass, GX13 전) |
| Deferred 고급 shading model(GBufferD) | ○ | ○ | ○(RQ11 뒤) | ○ | `Unsupported` → 그 머티리얼은 쿠킹 오류 또는 Forward+ view에서만(결정 RN4) |
| Deferred 정적 그림자 mask(GBufferE) | ○ | ○ | ○(RQ11 뒤) | ○ | `Unsupported`(Forward+는 지원) |
| decal(Deferred GBuffer decal, Forward+ DBuffer) | ○ | ○ | ○ | ○ | ○(SRV 예산 안, 7.4절) |
| 정적 축 Baked | ○ | ○ | ○ | ○ | ○ |
| 동적 축(VoxelConeTracing·DynamicGI) | 각 방식 스펙 | 각 방식 스펙 | 각 방식 스펙 | 각 방식 스펙 | 각 방식 스펙 |
| 동적 축 RayTraced·PathTracing(하드웨어) | △ | × | △ | △ | × |

**처리 규칙.**
- 지원하지 않는 조합은 view 생성(`ViewDesc` 검증) 또는 쿠킹에서 `Unsupported`로 거부한다. 다른 경로·형식·효과로 조용히 바꾸지 않는다(계획 11.1절, PixelFormat 1절).
- 공개된 대체는 profile이 명시한 것만 쓴다(예: 동적 GI 낮춤 순서, 계획 14.3절). 쓴 대체는 resolved profile과 계측에 남는다.
- GLES Deferred는 framebuffer fetch(GX13) 전까지 GBuffer를 메모리에 쓰고 조명 pass가 다시 읽는 다중 pass다. UE 모바일도 다중 pass와 단일 pass(subpass) 둘을 둔다(`R/MobileShadingRenderer.cpp:1805–1814`).

## 14. 렌더러가 머티리얼에 요구하는 계약

이 절은 렌더러 쪽 요구만 정한다. 머티리얼 그래프·IR·컴파일러·인스턴스·에디터는 [머티리얼 시스템](MaterialSystem.ko.md)(개정 2)과 계획 14.5–14.9절이 맡는다. 겹치는 곳은 14.6절 경계표로 나눈다. 두 문서의 다른 점은 머티리얼 시스템 10.4절 "정합 결과"로 맞췄다(이 문서 개정 3).

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

## 15. 렌더 그래프·파라미터 구조체·셰이더 타입 사용 방식

### 15.1 렌더 그래프

- 프레임 packet 하나 = 그래프 하나(RenderBase 7.3절). view마다 6.2절 pass를 선언 순서대로 추가한다(재배치 없음, 결정 R5).
- 장면 텍스처는 그래프 자원이고 `SceneTextureSet`을 `GraphSharedData`로 pass 사이에 넘긴다(UE blackboard 대응, RenderBase 2절).
- history(TAA·HZB·노출·path tracing 누적)는 `ExportTexture`로 `ViewHistory` 소유자에게 넘기고 다음 프레임 `ImportTexture`한다(RenderBase 7.4절).
- 스트리밍 텍스처·GPU Scene 버퍼·shadow atlas(프레임 사이 유지 시)는 import 자원이다.
- 1차는 단일 graphics queue(RG1·RG2). light grid·AO·HZB의 async compute는 RG4 뒤 선택이다(UE light grid async 선택 `R/LightGridInjection.cpp:47–52`).

### 15.2 파라미터 구조체와 공유 상수 slot

결정 R4(파라미터 구조체가 셰이더 상수·자원 바인딩과 그래프 의존을 함께 맡는다)를 그대로 쓴다. UE uniform buffer는 상수와 자원을 함께 담지만, RenderBase 9.4·9.5절의 공유 slot은 **상수 블록**이고 GLES에서는 uniform `vec4` 배열로 바뀌므로 자원을 담을 수 없다. 그래서 묶음마다 둘로 나눈다.
- **상수 부분**: 공유 상수 slot(RenderBase 9.5절)에 정적 등록한다.
- **자원 부분**: "펼쳐 넣기" 파라미터 구조체(RenderBase 9.6절)로 정의하고, 그 자원을 쓰는 pass의 파라미터 구조체가 포함한다. 그래프 의존도 이 멤버로 생긴다.

| 묶음 | 상수 부분(slot) | 자원 부분(펼쳐 넣기) | 수명 | UE 5.8 대응 |
|---|---|---|---|---|
| `View` | 행렬(현재·이전, jitter 유무), 카메라 이동 보정 상위·하위, render 크기, 시간, 노출, mip bias, 정적 간접 배율 둘 | — | `PerFrame`(view마다) | view uniform buffer(`EN/Public/SceneView.h:1157`) |
| `Scene` | 인스턴스 수 | GPU Scene typed buffer들 | `PerFrame` | GPU Scene(`SH/SceneData.ush:351`) |
| `ForwardLight` | grid 크기·Z 인자, directional 목록(최대 4) | 광원 데이터·grid typed buffer | `PerFrame` | `ForwardLightStruct`(`R/SceneRendering.h:728–763`) |
| `Shadow` | CSM 행렬·분할, atlas 크기 | shadow atlas(비교 샘플러 짝) | `PerFrame` | `R/SceneRendering.h:765–779` |
| `SceneTextures` | — | 깊이·GBuffer·AO·velocity·HZB | 그래프 수명 | scene textures uniform buffer |
| `Fog` | height fog 인자 | aerial perspective LUT | `PerFrame` | `FogStruct`(`R/FogRendering.cpp:85`) |
| `PrecomputedLighting` | 정적 조명 상수 | 라이트맵·프로브 텍스처 | `Persistent`(인스턴스 묶음) | `EN/Public/LightmapUniformShaderParameters.h:16` |

- 상수 slot 수(6)는 형식 정의 상수 버퍼 상한(데스크톱 공통 12) 안이다. pass마다 실제 쓰는 멤버만 바인딩 layout에 남는다(RenderBase 9.6절 미사용 멤버 거르기).
- **GLES uniform 예산.** GLES PS 에뮬레이션 상한은 `vec4` 224개다(형식 정의). 위 상수의 추정 크기는 `View` 약 40, `ForwardLight` 약 20, `Shadow` 약 20, `Fog` 약 8, `PrecomputedLighting` 약 8, `Scene` 1로 합계 약 100이다. 머티리얼 상수와 루트 상수(최대 8)를 더해도 남지만 실제 크기는 구조체를 정한 뒤 워커 검사로 확인한다(**미확인**, 추정값).
- **그리기별 값.** 인스턴스 offset은 루트 상수(`ROCKS_ROOT_CONSTANTS`, 128 byte, 셰이더 시스템 7.2절 2항)로 넘긴다. `SV_InstanceID`에 base instance가 들어가는지가 backend마다 다르므로 시스템 값에 기대지 않는다(셰이더 시스템 7.2절 7항).
- **GPU가 만드는 간접 draw(RS4).** GLES 3.1 간접 draw 명령의 base instance 자리는 0이어야 하므로 base instance로 offset을 넘길 수 없다. 1차 규칙: 그리기 항목 하나에 간접 draw 하나를 CPU가 기록하고 그 직전에 루트 상수로 인스턴스 offset을 둔다. GPU culling은 인스턴스 수와 인스턴스 index 목록만 쓴다. draw 여러 개를 한 호출로 묶는 것(D3D12 command signature의 루트 상수, Vulkan multi-draw)은 backend별 최적화로 후속이다. GLES base instance 규칙의 사양 원문 대조는 **미확인**(RQ3).

### 15.3 셰이더 타입

| 종류 | 예 | 등록 | 거르기 |
|---|---|---|---|
| 전역 셰이더(머티리얼 없음) | light grid CS, HZB CS, `DeferredLightingPass` PS, 하늘·안개 PS, tone map·TAA·bloom | `RendererShaders` 정적 등록(셰이더 시스템 12.2절) | 형식·퍼뮤테이션 |
| pass 셰이더(머티리얼 결합) | 14.3절 표 | `RendererShaders` 정적 등록, 머티리얼 쿠킹이 곱한다 | 형식 × 정점 입력 타입(SX4) × `MaterialShaderTraits`(14.3절) |

- 전역 셰이더 퍼뮤테이션 예: `DeferredLightingPass` {GBuffer profile 4} × {동적 GI 표본 0·1} × {contact shadow 0·1}, light grid CS {HZB 판정 0·1}.
- GPU Scene 읽기: 정점 셰이더가 typed buffer에서 인스턴스 변환을 읽는다(GLES VS storage block 0 때문에 모든 backend 같은 방식, 결정 RN9). UE ES3.1은 primitive UBO 경로를 따로 둔다(`SH/SceneData.ush:40–46, 1293`).
- 정점 입력 타입(RenderBase 10절)은 1차로 정적 메시 하나, skinned는 RS12다.

## 16. 성능 예산과 계측

### 16.1 예산(계획 29.2절, 모두 미측정 초기 목표)

| 시험 | workload | 렌더러 기준 |
|---|---|---|
| B2 | 10만 render instance·2,000만 source 삼각형·광원 100·그림자 caster 8 | Deferred·Forward+ 각각 native 1080p GPU p95 ≤ 14 ms·p99 ≤ 16.67 ms |
| B8 | 1080p·1440p·4K 출력, 동적 render scale | 두 경로 지원 matrix 통과, cut 잔상 0 프레임 |
| B9 | dense city·실내외 GI·야간 emissive·스트리밍 | GI artifact 기준·기능별 영상 승인 |

Runtime60 GPU 14 ms 배분(계획 29.2절)을 pass 묶음에 대응시킨다.

| 예산 항목 | ms | 렌더러 pass(6.2절) |
|---|---|---|
| geometry/depth | 3.0 | F2·F4·F5·F8a·F9(Deferred GBuffer 쓰기)·F10 |
| shadow | 2.0 | F7 |
| lighting/GI | 3.0 | F3·F6·F8·F9(Forward+ 조명)·F11·F12 |
| transparency/VFX/fog | 1.5 | F13·F14·PC2·PC5 |
| upscale | 1.5 | PC3·PC10 |
| output post/UI | 1.5 | PC1·PC4·PC6–PC9·PC11·F16 |
| 여유 | 1.5 | — |

- Forward+ base pass는 geometry와 lighting에 걸치므로 나누지 않고 "F9 전체"를 lighting/GI에 넣는다. Deferred F9(GBuffer 쓰기)는 geometry/depth다. 두 경로를 비교할 때는 예산 항목 합계가 아니라 F4–F12 합계로 대조한다.
- F1(CPU)과 F3(sky LUT, 작은 compute)은 각각 CPU 예산·lighting/GI에 둔다.

### 16.2 계측

- **`GpuScope`**(RenderBase 13절): 6.2절 F1–F16과 11.2절 PC1–PC11 이름 그대로의 구간. RG1 뒤에는 그래프 pass 자동 구간이다. 예산 항목 이름을 구간 묶음으로 둔다.
- **계수**: view별 가시 인스턴스·제출 그리기·dispatch·삼각형, light grid 셀당 광원 수 histogram과 overflow, atlas 사용률과 그림자 해상도 강등·생략 수, 보류 집합 크기와 재검사 복구 수(V4), 정적·동적 GI 낮춤 결과, 지원표 거부 수, 기본 머티리얼 대체 수(14.7절 MRD8).
- **CPU**: 장면 변경 적용·가시성·그리기 항목 생성·그래프 구성 시간을 CP5 trace 구간으로(RenderBase 13.2절).
- **측정**:

| ID | 측정 | 결과가 정하는 것 |
|---|---|---|
| MR1 | B2에서 grid 조명 pass와 광원별 pass 시제품의 GPU 시간·대역폭(D3D12·Vulkan·GLES) | 결정 RN1 확정 |
| MR2 | 광원·그림자 직접 표본 비용(그림자 광원 0·8·32) | 9.2절 직접 표본 유지 또는 mask 도입 |
| MR3 | light grid 셀 크기·Z 단 수 대비 overflow·시간 | 8.1절 기본값 |
| MR4 | GPU Scene typed buffer 읽기 비용(structured 대비, D3D12·Vulkan) | 결정 RN9 재검토 |
| MR5 | Deferred·Forward+ 같은 장면 GPU 시간 | profile 기본 경로 권고 |

## 17. 단계와 완료 기준

### 17.1 선행

| 선행 | 문서 | 필요한 것 |
|---|---|---|
| 화면 마일스톤 전체 | RenderBase 18.1절 | SH0–SH4, GX1–GX11, RB1–RB7, RG1–RG2 |
| SH2 | 셰이더 시스템 16절 | 쿠킹 셰이더·파이프라인 |
| GX2 | RenderBase 4.4절 | structured·typed buffer·UAV·깊이·MSAA·array·cube·mip view(18절 RQ1–RQ2) |
| GX4 | RenderBase 4.4절 | MRT·깊이 attachment·dispatch·indirect(RQ3) |
| RB2–RB6 | RenderBase 6·9–11절 | 자원 수명·셰이더 집합·상수 블록·공유 slot·정점 입력 타입·그리기 유틸리티 |
| RG2 | RenderBase 8절 | 프레임 안 풀 재사용·그래프 안 업로드 |
| 머티리얼 | 머티리얼 시스템 15.2절 | RS0 전: MS1–MS3(스키마·번역·쿠킹, SF1 `Unlit`)과 MS4의 렌더 상태·상수 블록·기본 머티리얼 부분. MS4의 두 경로 기준 영상(MS-T6)은 RS2 완료 기준 (a)와 함께 본다. 고급 shading model은 MS8(SF3)이 RS9와 함께 간다 |
| 메시 에셋 | AssetSystem(메시 단계) | 정적 메시(위치·패킹 법선·UV·section·bounds·LOD) |
| WorldIntegration | 계획 3.1절 | 렌더 컴포넌트 → `SceneChangeList`·`ViewDesc` |

### 17.2 단계

화면 마일스톤 다음 첫 단계는 RS0이고, 그다음은 Forward+(RS1)가 Deferred(RS2)보다 먼저다(결정 RN7). 이유: GLES·모바일 기본 경로가 Forward+이고(계획 11.1절) light grid·조명 함수가 Deferred의 기반이기 때문이다.

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| RS0 | `RenderData`·`RendererShaders`·`Renderer` 모듈, `RenderScene`(변경 목록 적용), `RenderView`·`ViewHistory`, 가시성 V1, 그리기 항목 캐시·상태 묶음, `GpuScene`, depth prepass·velocity, HZB 생성, Unlit·emissive base pass, 최소 tone map | 17.1절 전체 | (a) 기록 backend: 같은 변경 목록 열 → 같은 장치 호출 열(결정적), 적용 순서(제거→추가→갱신) 단언 (b) 가시 집합 = CPU 참조(bounds 전수) (c) GPU Scene 영역 재사용이 slot GPU 완료 gate 전에는 없음 (d) D3D12·Vulkan·GLES 미리보기 Unlit 장면 기준 영상(채널 ±8, 2%, RenderBase 8절 기준) (e) reversed-Z·앞면·velocity 부호 시험(정지 카메라 0, 알려진 이동의 velocity 값) |
| RS1 | Forward+ 직접광: 광원 장면, light grid CS, `ForwardOpaque`(DefaultLit), directional·point·spot, sky light 상수 | RS0, GX2 typed buffer UAV | (a) grid 셀 목록 ⊇ CPU 참조 판정(보수), overflow 계수 정확 (b) 광원 0개 view의 grid 생략 (c) 기준 영상 3 backend (d) GLES texel buffer 경로 픽셀 = D3D12 경로(허용 오차) |
| RS2 | Deferred: GBuffer 기본 layout, `DeferredLightingPass`(7.3절) | RS1 | (a) **계획 API02**: 같은 머티리얼·메시·광원 장면의 Deferred·Forward+ 영상 대조(채널 ±8·2%로 시작, 14.7절 MRD10, 머티리얼 MS-T6과 같은 시험), 깊이·법선·velocity 일치(Forward+는 보조 출력과 GBufferA 대조) (b) GBuffer 인코딩 왕복 CPU 시험(법선·shading model bit) (c) GLES 4 MRT 다중 pass 영상 |
| RS3 | 그림자: CSM·spot atlas·point 6면, PCF, caster 판정 V5 | RS1 | (a) 두 경로 그림자 영상 대조 (b) atlas overflow 시 강등·생략 계수와 표시 (c) caster 집합 ⊇ CPU 참조 (d) 직교 깊이 clamp 미지원 backend의 공개 대체 확인 |
| RS4 | 2단계 occlusion(V2–V4), GPU 인스턴스 culling, 그리기 항목마다 간접 draw 하나(15.2절) | RS0, GX4 indirect | (a) 계획 13.1절 V_ref ⊆ V_submit(가시 누락 0), 느린 strafe·이동·삭제 occluder·cut·teleport 시퀀스 (b) 이전 HZB 없음·무효에서 전체 그리기 (c) 재검사 복구 계수 (d) GLES에서 base instance 0 간접 draw와 루트 상수 offset으로 같은 영상 |
| RS5 | 투명 3종·정렬·블렌드 모드, distortion, sky atmosphere LUT·pass, height fog | RS1, 머티리얼 MS7(SF2 반투명 블렌드 모드) | (a) 두 경로 투명 영상 대조 (b) 정렬 결정성(같은 입력 같은 순서) (c) fog·sky가 두 경로에서 같은 영상 |
| RS6 | `PostChain` 1차: 노출·bloom·tone map·LUT, FXAA, spatial upscale(U0), TAA·TAAU(U1), `Upscaler` interface | RS0, 계획 12절 | 계획 12.3절 시퀀스(정지·회전·고속·resize·cut), cut 잔상 0 프레임, history reset 조건 시험 |
| RS7 | G0 기준 조명: 반사 capture·sky light, SSAO·GTAO, decal(Deferred GBuffer decal F10·Forward+ DBuffer F8a), `StaticLighting::Baked` 표본·GBufferE | RS2·RS3, bake 스펙(계획 14.10절) | 계획 LT02(direct·indirect·mixed shadow 항 분리, 두 경로·backend 비교), 이중 가산 0(정적·동적 배율 시험) |
| RS8 | Forward+ MSAA 1·2·4, resolve(계획 11.1절) | RS1·RS6 | MSAA × temporal 조합의 resolve 대조(깊이·velocity 전경 선택), alpha-to-coverage |
| RS9 | 고급 shading model(GBufferD), rect light·IES·광원 함수 atlas | RS2·RS3, 머티리얼 MS8(SF3) | 계획 14.2절 고급 직접 조명 검증, 모델별 Deferred 인코딩·Forward 평가 대조 |
| RS10 | screen-space: SSR·contact shadow, Forward+ 보조 출력, 간접광 합성(F12) | RS2·RS6 | 순환 graph 거부(음성 대조), 두 경로 screen-space 영상 대조 |
| RS11 | GI·RT 연결점: `IndirectLightingInputs`, `RayScene` 자리, `RenderMode::PathTracing` 골격(누적·리셋·후처리 연결) | RS10, 각 방식 스펙 | 방식 없음 view와 방식 있는 view의 직접광 동일, PathTracing 리셋 조건 시험, 미지원 장치 `Unsupported` |
| RS12 | skinned 메시: 정점 입력 타입, 현재·이전 pose(velocity), 두 경로·그림자·투명 | RS3·RS6, 애니메이션 skinning(계획 15절) | 계획 15절 "Deferred·Forward·shadow·picking에서 동일 pose", CPU skinning 기준 결과 대조, 계획 12.1절 U1 skinned motion(velocity) 시험 |

- 모든 GPU 시험은 검증 계층 오류 0(RenderBase 8절)과 렌더 lane·인라인 모드 결과 동일을 함께 요구한다.
- 기준 영상은 계획 29.2절대로 장면 manifest·카메라 경로·seed·build hash를 고정한다. Vulkan·GLES가 CI에서 NOT RUN이면 로컬 결과를 기록한다(RenderBase 18.1절과 같다).
- 볼륨 안개·구름·가상 그림자·VirtualGeometry·OIT·DOF·motion blur·HDR 출력은 별도 스펙이다.

## 18. 선행 작업에 반영할 요구

실제 수정은 하지 않았다. 각 문서의 확정 때 반영한다. "기한"은 그 요구가 막는 단계다.

### 18.1 요구 목록

| ID | 대상 | 요구 | 기한 |
|---|---|---|---|
| RQ1 | GX2 버퍼 | 용도: structured(SRV·UAV), typed buffer(SRV·UAV, `PixelFormat` 지정), 간접 인자. typed buffer 형식 `R32Uint`·`RGBA32Float`(GLES image 가능 형식, 8.1절) | RS0 |
| RQ2 | GX2 텍스처 | `D32FloatS8Uint`·`D32Float` 깊이, 깊이 읽기 전용 view와 스텐실 plane view(PixelFormat 10절), MSAA, cube array(반사 capture), 3D(LUT·후속 GI), UAV 텍스처, mip별 view(HZB) | RS0·RS3 |
| RQ3 | GX4 | MRT 최대 8(타깃별 load·store·clear), 깊이 attachment 읽기 전용 표시, 스텐실 ref, dispatch·간접 dispatch, indexed instanced 간접 draw, 텍스처 복사(history·distortion), UAV clear, viewport·scissor | RS0 |
| RQ4 | GX1 | `maxColorAttachments`, 형식별 MSAA 표본 수, typed UAV load 형식(HZB `R32Float`·`RG16Float`), texel buffer 형식 능력, 깊이 clamp, layer 출력, 64 bit atomic, RT tier, HDR 출력 능력 | RS0·RS3·RS11 |
| RQ5 | GX3 | 깊이 `GreaterEqual`·`Equal`, slope-scaled 깊이 bias, 깊이 clamp, 타깃별 블렌드(decal), 스텐실 연산, 색 쓰기 mask, alpha-to-coverage | RS0·RS3·RS8 |
| RQ6 | GX5 | 깊이 읽기 전용 상태(DSV 읽기 + SRV 동시, D3D·Vulkan·Metal. GLES는 feedback loop이므로 깊이 복사 경로, 8.2절), compute 사슬의 UAV→SRV, MSAA resolve 상태 | RS0 |
| RQ7 | RenderBase 9.5·9.6절 | 15.2절 상수 slot 6개 등록, 자원 부분을 펼쳐 넣기 구조체로 공유하는 방식(같은 자원 묶음을 여러 pass가 포함)을 9.6절 규칙에 적는다. **반영(RenderBase 개정 15, 9.5·9.6절)**: slot 여섯의 선언 위치(`RendererShaders`)·묶는 단위·상수 버퍼 예산, 자원 묶음 규칙(이름 `<묶음>_<멤버>`, 쓰는 멤버만 의존) | RS0 |
| RQ8 | RenderBase 10절 | 정점 입력 타입이 GPU Scene typed buffer로 인스턴스 변환을 읽는 규약, 인스턴스 offset 루트 상수. **반영(RenderBase 개정 15, 10.5절)**: `InstanceIndices[instanceListOffset + SV_InstanceID]`로 인스턴스 번호를 얻고 GPU Scene typed buffer를 읽는다. 늘 목록을 거친다(RenderBase 결정 R13). 루트 상수 앞 16 byte `MeshDrawConstants` | RS0 |
| RQ9 | PixelFormat 8.6절 | GLES texel buffer 형식(ES 3.2 Table 8.18)에서 `R32Uint`·`RGBA32Float` 대조(지금 "미대조") | RS1 |
| RQ10 | 셰이더 시스템 7.2절 4항 | GLES에서 typed buffer SRV가 텍스처 유닛을 차지함을 상한 계산에 명시(이미 "SRV 텍스처(typed buffer 포함)"이나 GLES `samplerBuffer` 관계를 적는다), SPIRV-Cross가 HLSL `Buffer<T>`를 ESSL `samplerBuffer`·`texelFetch`로 내는지 SH0 측정에 추가 | RS1 |
| RQ11 | 셰이더 시스템 12.1절(형식 정의) | `VULKAN_SM5` `renderTargets`를 8로 올리고 `deviceRequirements`에 `maxColorAttachments ≥ 8`(결정 RN3 (가)) 또는 별도 형식 id. **반영(셰이더 시스템 개정 14)**: (가)로 `renderTargets` 8. 장치 요구는 형식 `limits`를 런타임 형식 선택이 `DeviceCaps`와 대조하는 규칙으로 둔다(별도 어휘 없음) | RS7 전(RS2는 기본 layout으로 진행, 결정 RN3과 같다) |
| RQ12 | 셰이더 시스템 7.2절 4항·12.1절 | 렌더러 자원(`DeferredLightingPass`·`ForwardOpaque`·`Translucent`)을 GLES 보장치 16 안에 두는 예산표 유지(7.4절) | RS2 |
| RQ13 | 셰이더 시스템 12.2절(P14) | pass 셰이더 타입의 거르기 함수가 `MaterialShaderTraits`·정점 입력 타입·퍼뮤테이션·형식을 받는 것(SX4와 함께, 머티리얼 MSR5와 같은 요구), 쿠커의 퍼뮤테이션 수 보고. **반영(셰이더 시스템 개정 16, 12.2절)**: 머티리얼 셰이더·정점 입력 축 표지, 머티리얼 시스템 8.3절과 같은 서명과 AND 결합, 형식 사실 조회, 퍼뮤테이션 수 보고 | RS0 |
| RQ14 | 셰이더 시스템 7.2절 7항 | `SV_Target0–7`과 형식별 MRT 상한 검사(이미 있음) 확인, `SV_RenderTargetArrayIndex`(VS)를 능력 형식으로 열 때의 형식 id 규칙. **반영(셰이더 시스템 개정 16, 7.2절 4·7항)**: MRT는 이미 있었으나 프로필 공통 상한(4)으로 검사하던 것을 job 형식 `renderTargets` 검사로 고쳤다. VS 층 출력은 런타임 확인 기능 `vertexLayerOutput`이고 형식 id를 나누지 않는다(셰이더 시스템 결정 12) | RS3(한 번 그리기는 후속) |
| RQ15 | RenderGraph | `GraphSharedData`로 `SceneTextureSet` 전달, history export·import, mip view 그래프 자원, view 여럿의 한 그래프 구성. **반영(RenderBase 개정 15, 7.5절)**: 렌더 그래프 별도 문서가 없어 RenderBase 7절에 두었다 | RS0 |
| RQ16 | RenderBase 11·14절 | MSAA 색·깊이 resolve 셰이더(깊이·velocity 전경 선택, 계획 11.1절)를 그리기 유틸리티로. **반영(RenderBase 개정 15, 11.2·14절)**: 색 휘도 가중 평균, 깊이 최댓값(전경), 같은 표본의 velocity, coverage 선택 출력 | RS8 |
| RQ17 | RenderBase 11.1절 내장 자원 팩 | 빈 light grid, 빈 그림자(깊이 1 = 원평면, 9.2절 보통 Z 규약에서 비교 결과 "그림자 없음"), 검은 cube, 기본 BRDF LUT(또는 시작 때 생성). **반영(RenderBase 개정 15, 11.1절)**: BRDF LUT는 시작 때 CPU 생성 128×32 `RG16Float`(RenderBase 결정 R14) | RS1·RS3·RS7 |
| RQ18 | 머티리얼 시스템 | 14절 계약 전체, 특히 `SurfaceAttributes`·생성 함수(14.1절)·번호표 참조(`RenderData`, 결정 RN5)·pass 셰이더 목록과 경로·`MaterialShaderTraits` 거르기, (pass, 퍼뮤테이션, 형식)별 머티리얼 텍스처 상한 표시(7.4절), Render Path Switch의 깊이 일치 검사(14.3절). 머티리얼 시스템 개정 2가 10.1·10.4절에 반영했다 | MS2(MAT0) |
| RQ19 | AssetSystem 메시 단계 | 정적 메시: 위치·패킹 법선·tangent·UV, section·머티리얼 slot, bounds, LOD. 이동 물체 velocity를 위한 이전 위치는 렌더러가 변환으로 계산(skinned는 이전 pose를 렌더러에 넘김) | RS0 |
| RQ20 | WorldIntegration | `SceneChangeList`·`ViewDesc` 생성, teleport·cut 표시, `RenderNotices` 처리 | RS0 |
| RQ21 | TextureStreaming | 렌더러가 텍스처 사용 기록별 마지막 그린 프레임 번호(4.2절)와 view별 mip bias(11.2절)를 넘기는 형식. TextureStreaming 4.1절 요구를 렌더러 쪽에서 받은 것 | RS0(사용 기록)·RS6(mip bias) |
| RQ22 | 셰이더 시스템 12.1절(형식 정의 `limits`)·7.2절 4항 | 결정 RN13 개정(2026-11-18 사용자 결정). (1) `sampledTextures`를 형식별로: `D3D12_SM6`·`METAL_SM5` 128 유지, `VULKAN_SM5`는 넓히고 `deviceRequirements`에 `maxPerStageDescriptorSampledImages`·`maxPerStageDescriptorSamplers` 하한을 넣는다(값은 GX1 장치 분포 확인 뒤, RQ11과 같은 방식), `D3D11_SM5`는 16, GLES는 16 유지. (2) 단계당 SRV 텍스처 상한 검사는 프로필 공통 상한(형식들의 최소값)이 아니라 job 형식의 상한으로 하고, 오류에 머티리얼·pass·퍼뮤테이션·형식을 담는다. 원문은 형식마다 바꾸지 않는다. **반영(셰이더 시스템 개정 13)**: (2)와 (1)의 D3D11 16은 7.2절 4항·12.1절에 들어갔다. Vulkan은 개정 14에서 128(장치 요구는 `limits`와 `DeviceCaps` 대조, `maxDescriptorSetSampledImages`·`maxPerStageResources` 합산 포함) | MS2·RS1 |
| RQ23 | 셰이더 시스템 7.2절 9항 | 짝 규칙의 예외로 머티리얼 텍스처의 **공유 샘플러**(UE `SSM_Wrap_WorldGroupSettings`·`SSM_Clamp_WorldGroupSettings`와 같은 Wrap·Clamp 두 종, view 상수 쪽 샘플러, mip bias 포함)를 후속에서 앞당긴다. GLES는 공유 샘플러 객체를 각 텍스처 유닛에 묶어 combined sampler 규칙을 유지한다. 이것이 없으면 RQ22로 넓힌 형식도 샘플러 16에 묶인다(7.4절). **반영(셰이더 시스템 개정 13, 7.2절 9항)**: SH2와 함께 구현한다. 공유 샘플러 상태(필터·이방성·mip bias)는 렌더러가 view마다 정해 파이프라인 layout의 공유 slot에 준다 | 머티리얼 텍스처가 16장을 넘는 콘텐츠 전(측정 MM5로 시점 판단) |

### 18.2 문서별 개정 위치

| 문서 | 위치 | 현재 | 개정 |
|---|---|---|---|
| EngineImplementationPlan | 3.1절 모듈 표 "RenderGraph / Renderer" 행 | 의존 RHI·Task·Core·RenderData·SpatialIndex | Renderer → RendererShaders·RenderGraph·RenderBase·RenderData·MaterialRuntime·TextureStreaming·SpatialIndex. `RendererShaders` 행 추가 |
| 〃 | 11.1절 표 "광원" 행 | "clustered/tiled light list를 deferred pass에서 소비" | 같은 light grid를 전체 화면 조명 pass 하나가 소비(결정 RN1), tiled 없음 |
| 〃 | 11.4절 공통 전반부 | "light cluster·표면 shading" | 그림자 depth를 base pass 전(6.2절 F7), 하늘·안개 전체 화면 pass(결정 RN8) |
| 〃 | 14.2절 | "cascaded/atlas shadow" | 두 경로 직접 표본, point 6면(결정 RN10) |
| 〃 | 14.3절 조합 규칙 | 동적 GI 결과 소비 | `IndirectLightingInputs` 두 제품(12.2절), 정적 배율 두 값 |
| RenderBase | 4.4절 GX1·GX2·GX3·GX4·GX5 항목 | 일반 범위 | RQ1–RQ6 항목 추가 |
| 〃 | 9.5·9.6절 공유 상수 slot·펼쳐 넣기 | 일반 규칙 | 렌더러 상수 slot 목록과 공유 자원 묶음(RQ7). **반영(RenderBase 개정 15)** |
| 〃 | 7절 렌더 그래프 | 일반 규칙 | RQ15. **반영(RenderBase 개정 15, 7.5절)** |
| 〃 | 10절 정점 입력 타입 | 인스턴스 데이터 규약 없음 | RQ8. **반영(RenderBase 개정 15, 10.5절)** |
| 〃 | 11.1절 내장 자원 팩 | 1차 내용 | RQ17. **반영(RenderBase 개정 15)** |
| 〃 | 14절 표 "GBuffer 메타"·"MSAA resolve"·"렌더러 모듈 인터페이스" 행 | "Deferred 경로", "Forward+ MSAA", "Renderer 설계" | 이 문서 3.2절 배치(GBuffer 메타 `RendererShaders`, resolve RenderBase 유틸리티 RQ16). MSAA resolve 행은 **반영(RenderBase 개정 15, 11.2절)**, GBuffer 메타·렌더러 인터페이스 행은 남았다 |
| 〃 | 18.1절 "범위 밖" 첫 항목 | "Forward+·Deferred 조명 경로와 GI(계획 11·14절)" | 이 문서 17절 RS0–RS11 참조 |
| ShaderSystem | 7.2절 4항 | GLES 모바일 상한, SRV 상한을 프로필 공통 상한으로 검사 | RQ10·RQ12, SRV 텍스처는 job 형식 상한으로 검사(RQ22) |
| 〃 | 7.2절 9항 | 공유·정적 샘플러는 후속 | 머티리얼 공유 샘플러(RQ23) |
| 〃 | 12.1절 형식 정의 | `VULKAN_SM5` `renderTargets` 4, `sampledTextures` D3D11 128·Vulkan 16 | RQ11(결정 RN3), RQ22(결정 RN13 개정) |
| 〃 | 12.2절 P14 | 셰이더 타입·거르기 | RQ13. **반영(셰이더 시스템 개정 16)** |
| 〃 | 7.2절 7항 | 시스템 값 목록 | RQ14. **반영(셰이더 시스템 개정 16, 4·7항)** |
| PixelFormat | 8.6절 texel buffer 열 | "미대조" | RQ9 |
| TextureStreaming | 4.1절 사용 기록·표본 해상도 입력 | 렌더러 쪽 형식 미정 | 렌더러가 넘기는 사용 기록·mip bias 형식(RQ21) |
| RenderingGuide | 9절 | 계획 기반 개요 | 이 문서 6.2절 순서와 13절 지원표 참조 |
| EngineRuntime | 10.3절 범위 밖 | "Deferred/Forward 경로" | 이 문서 참조 |
| 머티리얼 시스템 | 10.1절 경계·10.4절 정합 결과 | 개정 2에서 반영 | 14절 계약(RQ18). 이 문서 쪽 처리는 14.7절 |

## 19. 결정 필요 사항과 미확인 목록

### 19.1 결정 사항

| ID | 결정 | 선택지와 권장 | 기한 |
|---|---|---|---|
| RN1 | Deferred 조명 구조 | (가) 권장: 전체 화면 조명 pass 하나가 directional 목록 + light grid local 광원을 Forward+와 같은 함수로 평가, 그림자 직접 표본(7.3절). 측정 MR1로 확정. (나) UE 5.8 기본: 광원별 pass(구·원뿔 기하) + 그림자 광원별 screen mask. (다) tiled compute deferred(UE에서 제거됨, GLES image 형식 제약) | RS2 전(MR1 결과로) |
| RN2 | GBuffer 법선 인코딩 | (가) 권장: UE desktop과 같은 `N·0.5+0.5` 10:10:10, 모든 backend. (나) octahedral(UE 모바일 방식) | RS2 전 |
| RN3 | Vulkan Deferred MRT 8 | (가) 권장: `VULKAN_SM5` 장치 요구에 MRT 8을 넣고 상한을 8로(데스크톱 장치 분포는 **미확인**이므로 GX1 조회 결과를 먼저 본다). (나) MRT 8 형식 id를 따로 둔다. (다) Vulkan Deferred는 기본 layout만(D·E는 `Unsupported`). **결정됨(2026-11-18, 권장안 (가))**: GX1 측정(GTX 1060·lavapipe 모두 8)으로 `VULKAN_SM5` `renderTargets` 8(셰이더 시스템 개정 14) | RS7 전(RS2는 기본 layout으로 진행 가능) |
| RN4 | GLES Deferred 범위 | (가) 권장: 기본 layout(MRT 4)만, 다중 pass, GBufferD·E가 필요한 기능은 GLES Deferred에서 `Unsupported`(Forward+는 지원). (나) GLES Deferred를 두지 않음(계획 11.5절 "모든 backend에서 두 경로 검증"과 충돌) | RS2 전 |
| RN5 | shading model·블렌드 모드·영역 번호표 소유 | (가) 권장: 렌더러가 번호·GBuffer 의미를 소유하고 머티리얼 스키마가 참조. 개정 3: 번호표 C++ 정의는 `RenderData`, HLSL 상수는 `/Engine/Shared/` 공용 헤더, GBuffer 의미는 `RendererShaders`(3.2절, 머티리얼 시스템 10.4절 D2의 순환 해소). (나) 머티리얼 스키마가 소유하고 렌더러가 참조 | MAT0 전 |
| RN6 | velocity 형식 | (가) 권장: 모든 backend `RG16Float`. (나) UE처럼 `RG16Unorm` 인코딩(GLES는 확장 조건) | RS0 전 |
| RN7 | 화면 마일스톤 다음 순서 | (가) 권장: RS0 → RS1 Forward+ → RS2 Deferred → RS3 그림자 → …(17.2절). (나) Deferred 먼저 | RS0 착수 전 |
| RN8 | 하늘·안개 위치 | (가) 권장: 두 경로 모두 전체 화면 pass(F13), 투명은 자기 셰이더. (나) Forward+는 base pass 안에서 안개 적용 | RS5 전 |
| RN9 | GPU Scene 저장 형태 | (가) 권장: 모든 backend typed buffer(float4) 하나의 경로. MR4로 재검토. (나) 데스크톱 structured + GLES typed 두 경로 | RS0 전 |
| RN10 | point 그림자 | (가) 권장: atlas 6면 개별 그리기(모든 backend). layer 출력 한 번 그리기는 능력 확장 후속. (나) cube 텍스처 + layer 출력 필수(GLES 불가) | RS3 전 |
| RN11 | 모듈·이름 | 2절 이름과 3.2절 배치(`RenderData`·`RendererShaders`·`Renderer` 셋). 대안: `RendererShaders`를 두지 않고 쿠커가 `Renderer`를 링크(Host 쿠커에 장치 모듈 의존이 생긴다) | 문서 확정 |
| RN12 | Forward+ 보조 출력 배치 | 8.4절 roughness 자리(두 번째 `R8Unorm` 타깃 또는 법선 octahedral + roughness 한 타깃). 권장안은 RS10 측정 뒤 | RS10 전 |
| RN13 | 단계당 SRV 상한 대응(7.4절) | **개정 3(2026-11-18 사용자 결정): 형식별 상한.** 셰이더 원문은 모든 형식에 하나로 유지하고 자원 합치기(grid 버퍼 하나·shadow atlas 하나·atlas 묶기)로 렌더러 자원을 16 안에 둔다. 단계당 SRV 상한은 형식 정의가 정한다(D3D12·Vulkan·Metal 넓게, GLES·D3D11 SM5 16). 머티리얼 텍스처 상한은 (pass, 퍼뮤테이션, 형식)별 `형식 상한 − 렌더러 합계`이고, 모바일 형식에서 넘는 머티리얼은 그 형식 쿠킹의 명시 오류다(RQ22). 넓은 형식이 16장을 넘으려면 공유 샘플러가 필요하다(RQ23). 개정 2의 확정안은 (가) 모든 형식 `16 − 렌더러 합계`였다. 그 밖의 선택지: (나) 16-SRV 형식용 pass 셰이더 타입을 따로 둠(셰이더 타입 수 증가), (다) bindless를 앞당김 | RS1 전 |

**결정(2026-11-18, 사용자 "결정사항들은 권장안대로 하면됨").** RN1–RN11·RN13은 각 행의 권장안으로 확정한다. RN12는 권장안이 정해지지 않은 항목이라 RS10에서 정한다. RN5는 머티리얼 시스템과의 교차 검증에서 번호표 코드 위치를 정했다(개정 3).

**개정(2026-11-18, 사용자 결정).** RN13을 권장안 (가)에서 형식별 상한으로 개정했다(위 행). 이유: (가)는 GLES 보장치 16이 데스크톱 형식까지 묶어 일반 PBR 머티리얼도 무거운 퍼뮤테이션에서 쿠킹 오류가 날 수 있었다(머티리얼 시스템 10.4절 D3). 데스크톱은 쿠킹되고 모바일 형식에서만 명시 오류가 나는 구조는 UE 모바일 머티리얼 제약과 같은 성격이다.

### 19.2 미확인 목록

| 항목 | 위치 | 확인 방법 |
|---|---|---|
| UE가 정지 물체 velocity를 깊이·이전 view 행렬로 복원하는지 | 7.1절 | `SH/` TAA·velocity 원문 조사 |
| UE grid 기반 deferred 제거 사유가 성능인지 | 7.3절 | 원문 주석 외 근거 없음. MR1로 Rocks 판단 |
| UE forward에서 그림자 채널(4개)을 받지 못한 광원의 동작 | 8.3절 | `R/DynamicShadowMapChannelBindingHelper.cpp` 조사 |
| UE forward에서 sky atmosphere 전체 화면 pass 대신 하늘을 그리는 주체 | 10.2절 | `SH/BasePassPixelShader.usf`·sky 머티리얼 조사 |
| GLES에서 깊이를 읽기 전용 attachment로 붙인 채 샘플하는 것이 feedback loop인지(쓰기를 끈 경우 포함) | 8.2절, RQ6 | ES 3.2 사양 "rendering feedback loop" 절 대조. 확인 전에는 깊이 복사로 설계 |
| GLES 간접 draw base instance 0 규칙 | 15.2절 | ES 3.1·3.2 사양 `DrawElementsIndirectCommand` 대조 |
| 공유 상수의 GLES `vec4` 크기 | 15.2절 | 구조체 확정 뒤 워커 검사 |
| UE path tracing에서 depth prepass·velocity 유지 여부 | 12.3절 | `DSR` path tracing 분기 전후 조사 |
| GLES texel buffer 형식(`R32Uint`·`RGBA32Float`) | 8.1절, RQ9 | **확인(2026-11-18)**: Table 8.18(Khronos glTexBuffer ES3 refpage)에 둘 다 있고 image 형식이라 typed SRV·UAV 모두 된다. GTX 1060 ES context에서 SRV 생성·VS·PS 읽기 픽셀 대조(PixelFormat.ko.md 14.3절) |
| SPIRV-Cross의 HLSL `Buffer<T>` → ESSL `samplerBuffer` 변환 | RQ10 | SH0 측정 |
| 데스크톱 Vulkan 장치의 `maxColorAttachments` 분포 | 결정 RN3 | GX1 조회·장치 표본. 측정(2026-11-18): GTX 1060·lavapipe 모두 8(RenderBase 4.4절 GX1 표). 장치 표본을 넓히는 것은 남음 |
| 데스크톱 Vulkan 장치의 `maxPerStageDescriptorSampledImages`·`maxPerStageDescriptorSamplers` 분포 | 결정 RN13, RQ22 | GX1 조회·장치 표본. 측정(2026-11-18): GTX 1060 단계당 1048576·1048576, set 1048576, `maxPerStageResources` 4294967295 / lavapipe 모두 1015808(RenderBase 4.4절 GX1 표). 장치 표본을 넓히는 것은 남음 |
| D3D12 resource binding tier 1 장치가 `D3D12_SHADER_MODEL_6_6` 요구를 통과하는지(샘플러 상한 16 여부) | RQ23 | 장치 표본. 개발 기계 GTX 1060은 tier 3(셰이더 시스템 15절 결정 3 측정) |
| Metal MRT·layer 출력·깊이 clamp·HDR 능력 | 13.1절 | SH5 착수 때 Apple 능력 표 |
| GLES 깊이 clamp 확장 | 13.1절 | 확장 목록 조사(P13 기기) |
| `r.Mobile.ShadingPath` 등록 위치 | 3.1절(모바일) | 렌더러 범위 밖이라 조사 생략. 설정 읽기만 확인(`RC/Private/ReadOnlyCVARCache.cpp:53–69`) |

## 20. 개정 이력

- 개정 1(2026-11-18): 초안. 사용자 지시 "스펙 지금 시작해"에 따라 작성했다. UE 5.8.4 Renderer 네 갈래 조사(프레임 흐름·desktop forward·light grid·모바일 선택 / GBuffer·Substrate·shading model·deferred 조명·그림자·정적 조명 / 장면 표현·그리기 명령·GPU Scene·view·가시성·투명·하늘·안개·후처리 / GI·RT·path tracing 진입점)를 종합하고, 핵심 근거 약 40곳을 원문 줄로 다시 대조했다. 사용자 결정(렌더 경로 Forward+·Deferred 둘, GI 두 축, PathTracing 별도 모드, UE 좌표·행렬, Rocks 이름, 레거시 제외, 5개 backend와 지원표)을 1절에 반영했다. 머티리얼 쪽 계약은 동시에 작성 중인 머티리얼 시스템 스펙과 겹치지 않게 14.6절 경계표로 나눴다. 독립 리뷰 1건(UE 인용 약 75곳 대조, 차단 2·중요 11·사소 다수)을 반영했다: GLES image 형식에 맞춰 grid를 `R32Uint` 하나로(8.1절), 큰 월드 위치를 GPU Scene 상위·하위 + view 이동 보정으로(5.1절), light grid 셀 상한 없음과 넘침 보수 대체(8.1절), 그림자 깊이를 UE와 같은 보통 Z·`D16Unorm`으로(9.2절), 단계당 SRV 16 예산표(7.4절, 결정 RN13), 공유 상수와 자원 묶음 분리(15.2절), 간접 draw의 인스턴스 offset(15.2절), 조명 pass의 깊이·스텐실 attachment 제거(GLES feedback loop, 7.3절), Forward+ DBuffer decal(6.2절), skinned 단계 RS12, 텍스처 스트리밍 요구 RQ21, 인용 경로·줄 번호 정정, PostChain ID를 PC로 변경.
- 개정 2(2026-11-18): 사용자 지시 "결정사항들은 권장안대로 하면됨"으로 RN1–RN11·RN13을 권장안으로 확정했다(19.1절). RN12는 RS10에서 정하고, RN5는 머티리얼 스펙과의 교차 검증에서 경계를 맞춘다.
- 개정 3(2026-11-18): 머티리얼 시스템 개정 1과 교차 검증해 두 문서를 맞췄다(머티리얼 시스템 10.4절 "정합 결과"). 사용자 결정 반영: RN13을 형식별 SRV 상한으로 개정(1·7.4·13.1·19.1절, RQ22), RN5 번호표의 C++ 정의를 `RenderData`·HLSL 상수를 `/Engine/Shared/`로(3.2·14.2절, 순환 해소). D1–D9 반영: WPO를 정점 함수로 분리하고 머티리얼 생성 함수 표를 둠(14.1절), `AlphaHoldout`을 후속 값으로 명시(14.2·10.2절), 거르기 인자를 `MaterialShaderTraits`로(14.3·14.5·15.3절), "static switch 차원"을 "static 변형"으로(14.6절), pass 표에 경로 열·`RENDER_PATH_*` 정의, depth·그림자 pass의 마스크 전용 함수(14.1·14.3절). 그 밖의 불일치 수정: Render Path Switch의 깊이 일치 규칙, 마스크를 퍼뮤테이션 차원에서 뺌, PDO 함수와 depth PS 조건, 노드 미리보기 pass `MaterialPreview`, 조회 키에 단계·SX5, mask clip 값이 static 정의임, 노멀 공간 변환 위치, `GBuffer`·depth 계열 pass의 렌더러 SRV 합계, 짝 규칙과 샘플러 상한(RQ23), 확정된 RN3을 지원표에 반영, 머티리얼 단계(MS)와 RS 선후(17절), 머티리얼 요구 MRD1–MRD12 처리(14.7절), 대체 계수(16.2절).
- 개정 4(2026-11-18): 셰이더 시스템 개정 13의 RQ22·RQ23 반영 상태를 적었다(7.4절, 19.1절 표, 18절 RQ22·RQ23).
- 개정 5(2026-11-18): 19.2절 미확인 3건(GLES texel buffer 형식 RQ9 확인, Vulkan `maxColorAttachments`·단계당 SRV·샘플러 분포)에 RenderBase GX1 측정을 적었다. RQ11·RQ22 값 결정은 하지 않았다.
- 개정 6(2026-11-18): 셰이더 시스템 개정 14 반영(RQ11·RQ22 Vulkan 값, 19.1절 MRT 행).
- 개정 7(2026-11-18): 18절 요구 RQ7·RQ8·RQ15·RQ16·RQ17(RenderBase 개정 15)과 RQ13·RQ14(셰이더 시스템 개정 16) 반영 상태를 18.1·18.2절에 적었다. RQ14를 반영하면서 MRT 상한을 프로필 공통 상한으로 검사하던 빈틈을 셰이더 시스템에서 고쳤다.
