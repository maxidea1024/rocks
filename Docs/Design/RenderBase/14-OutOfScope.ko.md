## 14. 후속·범위 밖

| 항목 | UE 5.8 파일 | 시기 |
|---|---|---|
| 런타임 mip 생성 | `GenerateMips.*` | 쿠킹 mip이 기본. 런타임 생성 렌더 타깃이 필요한 기능 단계 |
| 부분 clear·UAV clear 셰이더·compute 복사 | `ClearQuad.*`, `ClearReplacementShaders.*`, `CopyTextureShaders.*`, `OneColorShader.*` | 렌더 그래프 RG2 뒤 필요할 때 |
| MSAA resolve | `ResolveShader.*` | 11.2절 그리기 유틸리티로 설계(Renderer RQ16, 개정 15), 구현은 Renderer RS8 |
| 프레임 동적 버퍼·scatter upload | `DynamicBufferAllocator.*`, `UnifiedBuffer.*` | GPU Scene(계획 13절) |
| GBuffer 메타 | `GBufferInfo.*` | Deferred 경로 |
| 동적 해상도 | `DynamicRenderScaling.*` | 계획 12절 |
| 렌더러 모듈 인터페이스·장면 할당 | `RendererInterface.h`, `SceneRenderingAllocator.h`, `ReadOnlyCVARCache.*` | Renderer 설계 |
| 정점 패킹·렌더 변환 | `PackedNormal.h`, `RenderMath.h`, `RenderTransform.*` | 메시 에셋 단계(Core 수학과 중복 금지) |
| 가상 텍스처 | `VirtualTexturing*.h` | AssetSystem 12.4절 계약 |
| 레이 트레이싱 | `RayTracingGeometry*`, `BuiltInRayTracingShaders.*`, `RayTracingValidationShaders.*` | 계획 14.4절 Ray Scene, capability 확장 |
| bindless | `UpdateDescriptorHandle.*` | 셰이더 시스템 7.2절 10항 |
| 실행 중 layout 상수 블록, 셰이더 묶음 dispatch | `ExtensibleUniformBuffer.*`, `ShaderBundles.*` | 필요해질 때 |
| 미디어·외부 제품·스테레오·HDR 출력 | `MediaShaders.*`, `BinkShaders.*`, `OculusShaders.*`, `StereoRenderUtils.*`, `HDRHelper.h` | 각 기능 단계 |
| 렌더 lane tickable | `TickableObjectRenderThread.*` | sleep heartbeat 대신 프레임 packet이나 명시 타이머 이벤트로(시간 의존 금지) |
| 지연 기록 명령 목록·보조 직렬 pipe | `RenderingThread.h:561, 725–832` | 병렬 장면 준비(계획 8.4절) |
| GPU 제출 lane(UE RHI 스레드) | `RenderingThread.cpp:161–224` | 측정 뒤 |
| 실행 중 렌더 lane 켜기·끄기 | `RenderingThread.cpp:762–797, 1422–1451` | 넣지 않는다(시작 설정만) |
| state stream·트랜잭션 메모리 지연 | `RenderCommandStateStream.*`, `RenderingThread.h:994–1003` | 넣지 않는다(UE에서도 실험·기본 꺼짐) |
| 펜스 묶기·해제 일괄 처리 | `RenderingThread.cpp:873–953` | GC 대량 파괴 계측 뒤(GarbageCollectionScale과 함께) |
| PSO 미리 준비·기록 파일 | `ShaderPipelineCache.*`, `PipelineCacheUtilities.*` | 9.7절 후속 |
| 셰이더 묶음 preload·묶음 압축·플러그인 라이브러리 | `ShaderCodeArchive.h:691–773` | 패키징 단계(셰이더 시스템 12.3절) |
| specialization constant 차원 | `ShaderPermutation.h:636, 651` | 셰이더 시스템 후속(SX12) |
