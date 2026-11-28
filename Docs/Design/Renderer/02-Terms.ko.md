# 렌더러(Renderer) 설계 — 2. 용어 대응

[목차](README.ko.md)

## 2. 용어 대응

이름은 제안이며 결정 RN11로 확정한다.

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| `Renderer`(모듈) | Renderer 모듈 | 장면·view·pass 조립 |
| `RenderData`(모듈, 계획 3.1) | `FPrimitiveSceneDesc`·`FLightSceneProxy` 생성 입력 일부 | 장치 없는 장면 기술 값 타입과 ID |
| `RendererShaders`(모듈) | Renderer 모듈의 셰이더 타입 등록부(`IMPLEMENT_MATERIAL_SHADER_TYPE`, `R/BasePassRendering.cpp:180–192`) | pass 셰이더 타입·퍼뮤테이션·거르기 함수. 쿠커가 셰이더 타입 등록부 원천으로 시작하는 엔진 모듈이다(MaterialCook은 링크하지 않는다 — 3.2절) |
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
