## 3. UE 5.8 RenderCore 구성과 Rocks 배치

RenderCore는 Public 38,326줄·Private 63,918줄·Internal 1,239줄이다(`wc -l`, 합계 약 103,500줄).

| 영역 | UE 파일(요지) | Rocks 배치 | 이 문서 |
|---|---|---|---|
| 렌더 스레드·명령·펜스 | `RenderingThread.*`, `RenderCommandFence.h`, `RenderCommandTag.h`, `TickableObjectRenderThread.*`, `RenderDeferredCleanup.h`, `Internal/RenderThreadTimeoutControl.h` | Core `DedicatedLane`, RenderBase `RenderLane` | 5절 |
| 렌더 자원 | `RenderResource.*`, `GlobalRenderResources.*`, `CommonRenderResources.*` | RenderBase | 6·11절 |
| 렌더 그래프 | `RenderGraph*.h/.inl/.cpp`, `RenderTargetPool.*`, `PooledRenderTarget.h` | RenderGraph | 7·8절 |
| 셰이더 런타임 | `Shader.*`, `ShaderMap.cpp`, `ShaderResource.cpp`, `GlobalShader.*`, `ShaderParameter*`, `UniformBuffer.h`, `ExtensibleUniformBuffer.*`, `ShaderCodeLibrary.*`·`ShaderCodeArchive.*`·`ShaderLibrary/*`, `ShaderPipelineCache.*`, `PipelineCacheUtilities.*` | 메타데이터는 ShaderCore, 장치 객체는 RenderBase | 9절 |
| 셰이더 컴파일 쪽 | `ShaderCore.*`, `ShaderCompilerCore.*`, `ShaderCompilerJobTypes.h`, `ShaderPreprocessTypes.*`, `ShaderSource.*`, `ShaderParameterParser.*`, `Internal/ShaderCompile*` | 셰이더 시스템(Developer/ShaderCompiler·워커) | 셰이더 시스템 문서 |
| 셰이더 플랫폼 설정 | `ShaderPlatformCachedIniValue.h`, `ShaderPlatformConfig.*` | 셰이더 형식 정의(셰이더 시스템 12.1절) + 콘솔 설정 | 12절 |
| 정점 입력 | `VertexFactory.*`, `VertexStreamComponent.h` | RenderBase | 10절 |
| 그리기 유틸리티 | `RHIStaticStates.h`, `PixelShaderUtils.*`, `ClearQuad.*`, `OneColorShader.*`, `RenderUtils.*`(크기·형식 계산 부분), `PackedNormal.h`, `RenderMath.h`, `RenderTransform.*` | RenderBase(일부는 GraphicsTypes·Core 수학) | 11절 |
| 기능 단계에 필요한 것 | `GenerateMips.*`, `ClearReplacementShaders.*`, `ResolveShader.*`, `CopyTextureShaders.*`, `DynamicBufferAllocator.*`, `UnifiedBuffer.*`, `GBufferInfo.*`, `DynamicRenderScaling.*`, `RendererInterface.h`, `ReadOnlyCVARCache.*`, `SceneRenderingAllocator.h`, `ShaderMaterial.h`, `RenderUtils.*`(기능 질의 부분) | 해당 기능 단계에서 RenderBase 또는 Renderer로 | 14절 |
| 계측 | `ProfilingDebugging/RealtimeGPUProfiler.h`, `RenderCounters.h`, `RenderTimer.h`, `GPUMessaging.*` | RenderBase `GpuScope`, 나머지 후속 | 13절 |
| 디버그 도구 | `DumpGPU.*`, `VisualizeTexture.*`, `RenderCaptureInterface.*`, `IRenderCaptureProvider.h`, `GPUDebugCrashUtils.*`, `LongGPUTask.*` | Developer 모듈(후속) | 13·14절 |
| 범위 밖 | `VirtualTexturing*.h`, `RayTracingGeometry*`, `BuiltInRayTracingShaders.*`, `RayTracingValidationShaders.*`, `MediaShaders.*`, `BinkShaders.*`, `OculusShaders.*`, `StereoRenderUtils.*`, `HDRHelper.h`, `ShaderBundles.*`, `Animation/MeshDeformerProvider.*`, `CustomResourcePool.*`, `RenderCommandStateStream.*`(실험 기능, `WITH_STATE_STREAM` 기본 0, `Core/Public/Misc/Build.h:175–176`) | 각 기능 단계에서 따로 정한다 | 14절 |
