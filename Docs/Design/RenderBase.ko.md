# 렌더 기반(RenderBase) 설계

기준일: 2026-11-16, 개정 1(초안). UE 5.8 RenderCore 모듈이 맡는 일을 Rocks에 세우는 구현 스펙이다. 범위는 다음과 같다.
- 렌더 lane과 프레임 전달, 장치 자원 수명, 렌더 그래프
- 셰이더 런타임 객체(셰이더 집합·파라미터 바인딩·상수 버퍼·파이프라인 캐시)
- 정점 입력 타입, 공용 자원·그리기 유틸리티, GPU 계측
- 이것들을 올리기 전에 Core·Graphics가 갖춰야 할 **선행 작업**

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론) 원본이다. 경로는 `Engine/Source/Runtime/` 기준이며, `RenderCore/` 아래 파일은 `RC/`로 줄여 적는다. Rocks 경로는 저장소 루트 기준이다.

[셰이더 시스템](ShaderSystem.ko.md)(개정 8, 구현 전)과 겹치는 부분은 그 문서의 결정을 따른다. RenderCore 런타임 쪽 요구 때문에 그 문서를 고쳐야 하는 곳은 9.8절에 모았다. [엔진 구현 계획](EngineImplementationPlan.ko.md) 8·9.6·11절, [엔진 런타임](EngineRuntime.ko.md) 10절과 충돌하는 부분은 이 문서 기준으로 정리하고 그 문서들도 함께 고친다(15절).

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). 이 문서 전체는 **설계**이며 구현하지 않는다. 셰이더 시스템과 같이, 엔진 기반이 준비될 때까지 스펙으로만 둔다. 착수 위치는 16절 결정 R10이다.

**용어 방침.** UE 구조는 따르되 이름은 Rocks 이름을 쓴다. UE의 모듈·타입·매크로 이름은 대응표(2절)와 "UE 5.8 대응"·근거 열에만 적는다. pass·barrier·pipeline·descriptor·render target·swap chain·fence·SRV·UAV처럼 그래픽 API와 업계에서 함께 쓰는 말은 그대로 쓴다. 계획 문서에 이미 있는 Rocks 이름(`RenderGraph`·`RenderResourceLease`·`Retire`·`GpuFencePoint`·`DeviceCaps`·`BuiltinId`·`BuiltinReady`)도 그대로 쓴다.

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 모듈 배치 | UE RenderCore 하나를 넷으로 나눈다. **ShaderCore**(장치 없는 셰이더 메타데이터, 셰이더 시스템 3절), **RenderBase**(장치에 묶인 렌더 기반: 렌더 lane·장치 자원·셰이더 런타임 객체·정점 입력 타입·공용 자원·계측), **RenderGraph**(렌더 그래프, 계획 3.1절에 이미 있는 모듈), **Graphics 확장**(UE RHI가 하는 일 중 지금 Graphics에 없는 것) | UE RenderCore는 셰이더 컴파일 쪽 타입(`RC/Public/ShaderCore.h`, `ShaderCompilerCore.h`)과 RDG(`RC/Public/RenderGraph*.h`)를 한 모듈에 둔다. Rocks는 셰이더 컴파일러(Developer)가 장치 없이 쓰는 부분을 ShaderCore로 이미 분리했다(셰이더 시스템 3절). RDG는 셰이더 파라미터에 기대지만 파라미터 바인딩은 RDG에 기대지 않으므로 RenderGraph를 RenderBase 위에 둔다. 계획의 "엔진 안쪽은 핵심만" 원칙에 따라 디버그 도구(자원 덤프·텍스처 시각화·캡처 연동)는 Developer 모듈로 뺀다 |
| 렌더 실행 | 전용 스레드 하나가 직렬 FIFO로 일을 처리하는 **렌더 lane**. 헤드리스·시험용으로 같은 스레드에서 바로 실행하는 **인라인 모드**를 둔다 | UE도 전용 스레드 하나(`RC/Private/RenderingThread.cpp:321`, task graph 이름 붙은 스레드 `:227–262`)와 인라인 대체 경로(`RC/Public/RenderingThread.h:149, 504–525`)를 둔다. 계획 8.4절 "lane을 줄여도 진행 가능한 순차 기준 실행기"와 같다 |
| 게임 → 렌더 전달 | 불변 값 묶음 **프레임 packet**과 handle만 넘긴다. 게임 객체 주소를 담은 closure는 금지한다 | 계획 8.1절 Render lane "Game 주소 없음, immutable packet과 resource handle 수신". UE는 게임 소유 객체 포인터를 캡처한다(`RC/Private/RenderResource.cpp:290–296`). 이 점은 따르지 않는다 |
| 게임 대기 | 게임 lane이 막히는 곳은 **프레임 slot 확보** 하나다(backpressure). 전체 비우기는 종료·장치 재생성·시험 전용 API로만 허용한다 | 계획 8.1절 "Game/Render 상호 동기 wait 금지", 8.2절 "bounded queue backpressure". UE는 프레임마다 펜스를 블로킹으로 기다리고(`RC/Private/RenderingThread.cpp:2454–2572`) 전체 flush도 흔히 쓴다(`:1274–1316`) |
| 자원 수명 | 계획 11.2절의 lease·`Retire`·모든 queue fence 확인 방식이다. UE의 "큐 순서 + 한 프레임 뒤 delete"는 쓰지 않는다 | UE는 명령 FIFO 순서, 펜스, 프레임 끝 동기 한 번 뒤 delete에 기댄다(`RC/Private/RenderingThread.cpp:1323–1419`). 계획 11.2절은 그보다 강한 규칙(lease 원자 획득·Retire가 새 lease 차단·last-use fence 집계)을 요구한다 |
| 내장 자원 | 계획 9.6절 순서(manifest → `DeviceCaps` → 생성·업로드 → fence → `BuiltinReady`)를 렌더 lane이 명시적으로 실행한다. 정적 초기화에 기대지 않는다 | UE `TGlobalResource`는 정적 생성과 Pre/Default 단계 목록(`RC/Public/RenderResource.h:43–48, 446–505`)에 기대고, 준비 완료 gate와 생성 실패 경로가 없다 |
| 렌더 그래프 | pass를 선언 순서대로 실행하고 재배치하지 않는다. 의존은 앞쪽 pass만 가리키므로 순환이 생기지 않는다. 1차는 단일 graphics queue·aliasing 끔·병합 끔이다 | UE RDG 실행 루프(`RC/Private/RenderGraphBuilder.cpp:2104–2167`). 계획 11.3절의 1차 범위와 같다 |
| 파라미터 구조체 | 한 구조체가 셰이더 상수·자원 바인딩과 렌더 그래프 의존 선언을 함께 맡는다(UE 방식). 셰이더 시스템 결정 4(MetaCompiler 셰이더 파라미터)를 이 방향으로 확장한다(9.8절, 결정 R4) | UE 파라미터 매크로가 RDG 자원 멤버·렌더 타깃 슬롯을 함께 선언하고(`RC/Public/ShaderParameterMacros.h:1697–1760, 1868–1900`), RDG가 같은 메타데이터로 의존을 만든다(`RC/Private/RenderGraphBuilder.cpp:2384–2532`) |
| 셰이더 런타임 객체 | 셰이더 시스템 12절의 라이브러리 위에 **셰이더 집합**을 둔다. (타입, 퍼뮤테이션)마다 코드 참조·바인딩 layout·레이아웃 서명을 갖고, 장치 셰이더는 처음 쓸 때 한 번 만든다. UE의 frozen memory image는 들이지 않는다 | UE `FShaderMapResource::GetShader` 지연 생성(`RC/Public/Shader.h:403–415`, `RC/Private/ShaderResource.cpp:579–662`). memory image(`Core/Public/Serialization/MemoryLayout.h`)는 C++ 객체 배치를 직접 굽는 장치로, Rocks는 정규화 표를 Archive·Record로 읽는다 |
| 설정 변수 | 렌더 기능 대부분이 실행 중 설정 변수에 기대므로 Core에 **콘솔 설정**을 선행 작업으로 둔다. 렌더 lane에는 프레임 packet 안의 설정 snapshot으로 넘긴다 | RenderCore에 `TAutoConsoleVariable` 122곳, `FAutoConsoleVariableRef` 72곳이 있다(`git grep -c` 줄 수). UE는 렌더 스레드 사본을 대상 주소에 쓰는 명령으로 전파한다(`RC/Private/RenderingThread.cpp:493–530`). Rocks에는 이 체계가 없다(UnrealCoreFeatureAudit UE-C29 **후속**) |

## 2. 용어 대응

이름은 제안이며 결정 R1로 확정한다. "공용어"는 업계 공용 용어라 그대로 쓰는 것이다.

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| RenderBase(모듈) | RenderCore(일부) | 장치에 묶인 렌더 기반 |
| ShaderCore(모듈, 셰이더 시스템 3절) | RenderCore의 셰이더 메타데이터 부분 | 장치 없이 쓰는 셰이더 타입·퍼뮤테이션·파라미터 메타데이터·라이브러리 형식 |
| RenderGraph(모듈, 계획 3.1절) | RenderCore의 RDG | 렌더 그래프(공용어) |
| Graphics(모듈, 기존) | RHI | 그래픽 장치 계약과 backend |
| 렌더 lane `RenderLane` | rendering thread | 렌더 일을 직렬로 처리하는 전용 스레드 |
| 전용 lane 실행기 `DedicatedLane`(Core Task) | `FRunnableThread` + named thread | 스레드 하나가 직렬 FIFO를 처리하는 일반 실행기 |
| lane 작업 게시 `RenderLane::Post` | `ENQUEUE_RENDER_COMMAND` | handle만 받는 좁은 작업을 lane에 넣는다 |
| lane 표지 `LaneMarker` | `FRenderCommandFence` | lane이 어디까지 처리했는지 알리는 완료 표지 |
| 프레임 slot `FrameSlot` | `FFrameEndSync`의 프레임 지연 한도 | 동시에 진행 중인 프레임 2–3개 중 하나 |
| 프레임 packet `FramePacket` | (없음. UE는 포인터 캡처) | 게임 lane이 만든 불변 렌더 입력 묶음 |
| 장치 자원 소유자 `DeviceResourceOwner` | `FRenderResource` | 장치 handle과 초기화·해제 상태를 가진 렌더 쪽 객체 |
| `RenderResourceLease`·`Retire`(계획 11.2) | (없음) | 사용 lease와 회수 |
| 내장 자원 팩 `BuiltinResourcePack`·`BuiltinId`·`BuiltinReady`(계획 9.6) | `TGlobalResource`, `GWhiteTexture` 등 | 장치 생성 직후 만드는 기본 자원 |
| 그래프 텍스처·버퍼 `GraphTexture`·`GraphBuffer` | `FRDGTexture`·`FRDGBuffer` | 그래프 수명 자원 |
| 그래프 가져오기·내보내기 `ImportTexture`·`ExportTexture` | `RegisterExternalTexture`·`QueueTextureExtraction` | 그래프 밖 자원 연결(계획 11.3 import/export) |
| 그래프 자원 풀 `GraphResourcePool` | `FRenderTargetPool`·`FRDGBufferPool` | 같은 desc 자원의 프레임 안팎 재사용 |
| 그래프 공유 값 `GraphSharedData` | `FRDGBlackboard` | 그래프 수명 동안 pass 사이에 넘기는 타입별 값 |
| 유지 pass `KeepAlive` | `ERDGPassFlags::NeverCull` | 미사용 제거 대상이 아닌 pass |
| 셰이더 집합 `ShaderSet` | `FShaderMap`(content·resource) | 라이브러리에서 연 셰이더 묶음과 장치 셰이더 |
| 타입 있는 셰이더 참조 `TypedShader<T>` | `TShaderRef<T>`·`TShaderMapRef<T>` | 셰이더 타입과 집합의 쌍 |
| 파라미터 바인딩 지도 `ParameterBindingMap` | `FShaderParameterBindings` | 구조체 오프셋 → (단계, 종류, 순번) |
| 파라미터 묶음 `ParameterBatch` | `FRHIBatchedShaderParameters` | 상수·자원 바인딩을 한 번에 올리는 기록 객체 |
| 상수 버퍼(공용어) `ConstantBuffer` | uniform buffer | 수명은 `PerDraw`·`PerFrame`·`Persistent`(UE SingleDraw·SingleFrame·MultiFrame) |
| 공유 상수 slot `SharedConstantSlot` | static uniform buffer slot | 패스 전체에 한 번 묶는 상수 버퍼 자리 |
| 파이프라인 캐시(공용어) `PipelineCache` | `PipelineStateCache`(RHI) | 내용 키로 파이프라인을 한 번만 만든다 |
| 파이프라인 미리 준비 `PipelinePrewarm` | PSO precache | 비동기 사전 생성(후속) |
| 정점 입력 타입 `VertexInputType` | vertex factory | 메시 정점을 셰이더에 공급하는 방식의 등록 단위 |
| 정점 배치 `VertexLayout`, 정점 stream(공용어) | vertex declaration, stream component | 속성·stream·stride |
| 상태 프리셋 `StatePreset` | `TStaticBlendState` 등 | 이름 붙은 블렌드·래스터·깊이 조합 |
| 장치 능력 `DeviceCaps`(계획 9.6) | feature level, `GRHISupports*`, DataDrivenShaderPlatformInfo | 장치 기능·한도 조회 |
| GPU 구간 `GpuScope` | `RDG_EVENT_SCOPE`·RHI breadcrumb | 이름·marker·timestamp를 함께 기록하는 구간 |
| 콘솔 설정 `ConsoleSetting`(Core) | `IConsoleManager`·CVar | 실행 중 바꿀 수 있는 이름 있는 설정 값 |
| 프레임 arena `FrameArena`(Core) | `FMemStackBase`, RDG allocator | 프레임 끝에 한 번에 비우는 할당기 |

## 3. UE 5.8 RenderCore 구성과 Rocks 배치

RenderCore는 Public 38,326줄·Private 63,918줄·Internal 1,239줄이다(`wc -l`, 합계 약 103,500줄). 파일을 역할별로 나누고 Rocks 배치를 정한다.

| 영역 | UE 파일(요지) | Rocks 배치 | 이 문서 |
|---|---|---|---|
| 렌더 스레드·명령·펜스 | `RenderingThread.*`, `RenderCommandFence.h`, `RenderCommandTag.h`, `TickableObjectRenderThread.*`, `RenderDeferredCleanup.h`, `Internal/RenderThreadTimeoutControl.h` | Core `DedicatedLane`, RenderBase `RenderLane` | 5절 |
| 렌더 자원 | `RenderResource.*`, `GlobalRenderResources.*`, `CommonRenderResources.*` | RenderBase | 6·11절 |
| 렌더 그래프 | `RenderGraph*.h/.inl/.cpp`, `RenderTargetPool.*`, `PooledRenderTarget.h` | RenderGraph | 7·8절 |
| 셰이더 런타임 | `Shader.*`, `ShaderMap.cpp`, `ShaderResource.cpp`, `GlobalShader.*`, `ShaderParameter*`, `UniformBuffer.h`, `ExtensibleUniformBuffer.*`, `ShaderCodeLibrary.*`·`ShaderCodeArchive.*`·`ShaderLibrary/*`, `ShaderPipelineCache.*`, `PipelineCacheUtilities.*`, `StaticBoundShaderState.*` | 메타데이터는 ShaderCore, 장치 객체는 RenderBase | 9절 |
| 셰이더 컴파일 쪽 | `ShaderCore.*`, `ShaderCompilerCore.*`, `ShaderCompilerJobTypes.h`, `ShaderPreprocessTypes.*`, `ShaderSource.*`, `ShaderParameterParser.*`, `Internal/ShaderCompile*` | 셰이더 시스템(Developer/ShaderCompiler·워커) | 셰이더 시스템 문서 |
| 셰이더 플랫폼 설정 | `ShaderPlatformCachedIniValue.h`, `ShaderPlatformConfig.*` | 셰이더 형식 정의(셰이더 시스템 12.1) + 콘솔 설정 | 12절 |
| 정점 입력 | `VertexFactory.*`, `VertexStreamComponent.h` | RenderBase | 10절 |
| 그리기 유틸리티 | `RHIStaticStates.h`, `PixelShaderUtils.*`, `ClearQuad.*`, `OneColorShader.*`, `RenderUtils.*`(크기·형식 계산 부분), `PackedNormal.h`, `RenderMath.h`, `RenderTransform.*` | RenderBase(일부는 GraphicsTypes·Core 수학) | 11절 |
| 기능 단계에 필요한 것 | `GenerateMips.*`, `ClearReplacementShaders.*`, `ResolveShader.*`, `CopyTextureShaders.*`, `DynamicBufferAllocator.*`, `UnifiedBuffer.*`, `GBufferInfo.*`, `DynamicRenderScaling.*`, `RendererInterface.h`, `ReadOnlyCVARCache.*`, `SceneRenderingAllocator.h`, `ShaderMaterial.h`, `RenderUtils.*`(기능 질의 부분) | 해당 기능 단계에서 RenderBase 또는 Renderer로 | 14절 |
| 계측 | `ProfilingDebugging/RealtimeGPUProfiler.h`, `RenderCounters.h`, `RenderTimer.h`, `GPUMessaging.*` | RenderBase `GpuScope`, 나머지 후속 | 13절 |
| 디버그 도구 | `DumpGPU.*`, `VisualizeTexture.*`, `RenderCaptureInterface.*`, `IRenderCaptureProvider.h`, `GPUDebugCrashUtils.*`, `LongGPUTask.*` | Developer 모듈(후속) | 13·14절 |
| 범위 밖 | `VirtualTexturing*.h`, `RayTracingGeometry*`, `BuiltInRayTracingShaders.*`, `RayTracingValidationShaders.*`, `MediaShaders.*`, `BinkShaders.*`, `OculusShaders.*`, `StereoRenderUtils.*`, `HDRHelper.h`, `ShaderBundles.*`, `Animation/MeshDeformerProvider.*`, `CustomResourcePool.*`, `RenderCommandStateStream.*`(5.8 실험, `WITH_STATE_STREAM` 기본 0, `Core/Public/Misc/Build.h:175–176`) | 각 기능 단계에서 따로 정한다 | 14절 |

## 4. 현재 코드베이스와 선행 작업

### 4.1 현재 코드베이스(2026-11-16)

HEAD `1638a7f` 기준이다.

| 영역 | 있는 것 | 렌더 기반에 부족한 것 → 선행 작업 |
|---|---|---|
| Graphics 계약 | `Device`(스왑체인·버퍼·텍스처·파이프라인·`BeginFrame`/`BeginOffscreen`/`EndFrame`·`ReadPixels`·`Destroy`·`WaitIdle`, `Engine/Source/Runtime/Graphics/Public/Graphics/Device.h:42–112`), `CommandContext`(`SetPipeline`·`SetVertexBuffer`·`SetTexture`·`SetConstants`·`Draw`만, `:11–24`). "생성한 스레드에서만 사용", 프레임은 한 번에 하나(`:43–46`) | 4.4절 GX1–GX10 |
| 자원 종류 | 버퍼는 `BufferUsage::Vertex`만(`DeviceTypes.h:99–106`), 텍스처는 2D·`Sampled`/`RenderTarget`·RGBA8 2종(`:108–128`, `GraphicsTypes/Public/GraphicsTypes/PixelFormat.h:12–16`). 깊이·MSAA·배열·3D·큐브·UAV·index·structured 없음 | GX2(A0-G 뒤) |
| 파이프라인 | `PipelineDesc`가 backend별 원문을 받아 컴파일한다. 바인딩 고정(상수 b0, 텍스처 t0·s0), 블렌드는 `alphaBlend` bool, 깊이·컬링 없음, 정점 stream 하나(`vertexStride`)(`DeviceTypes.h:138–157`) | 셰이더 시스템 SH2 + GX3 |
| 상태 전이 | D3D12 내부 고정 전이(RT↔SRV↔COPY, 휴지 상태 관례)만 있고 공개 API가 없다(`D3D12Graphics/Private/Win64/D3D12Device.cpp:155–162, 635–637, 803, 816, 1197`). OpenGL backend에는 memory barrier 호출이 없다 | GX5 |
| 제출·회수 | D3D12 DIRECT queue 하나(`D3D12Device.cpp:390`), 프레임 2개 동시 진행·fence로 slot 회수(`:17, 838–846`), `Destroy` 지연 해제(`:1409–1441`). OpenGL은 즉시 삭제. lease·`Retire`·공개 fence 없음 | GX7 |
| 업로드·readback | 생성 때 초기 데이터만, 텍스처 업로드는 동기 완료(`Device.h:50`), `ReadPixels`는 시험용 동기 대기(`:90–92`) | GX8 |
| 계측 | query·marker 없음 | GX9 |
| Renderer | 내장 셰이더 원문 문자열(`Renderer/Private/Renderer.cpp:14–71`), 파이프라인 하나, 메시 둘(`Renderer/Public/Renderer/Renderer.h:15–23`) | 셰이더 시스템 SH2, RB 단계 |
| 엔진 루프 | `EngineLoopHooks::EndFrame`에서 렌더 추출과 제출을 게임 스레드가 동기로 한다(`Engine/Source/Runtime/Engine/Public/Engine/Engine.h:52–69`). 렌더 스레드·in-flight snapshot은 후속(`Docs/Design/EngineRuntime.ko.md` 14절) | RB1 |
| 스레드·동기화 | join 소유 `platform::Thread`(`Core/Source/Runtime/PlatformBase/Public/Platform/Thread.h:23–44`), `Mutex`·`ConditionVariable`·`ManualResetEvent`·`Semaphore`(`Sync.h:19,62,79,98`), `Atomics.h`. 스레드 이름·우선순위·affinity·스택 크기는 후속(`Docs/Implementation/PlatformConcurrency.ko.md:42`) | CP1 |
| 작업 | `WorkerScheduler`(선행 작업·취소·Drain, 외부 이벤트 `WorkerTaskEvent`, `Core/Source/Runtime/Task/Public/Task/WorkerScheduler.h:93–149, 182–217`), `InlineExecutor`(동기 기준, `TaskScheduler.h:55–56`). 직렬 체인·ParallelFor·전용 스레드 lane 없음 | CP1·CP2 |
| 할당기 | `StackAllocator`(단일 스레드 LIFO, `Core/Memory/StackAllocator.h:42`), `ArenaAllocator`(반환 안 된 할당이 있으면 Reset이 Busy, 프레임 일괄 무효화 없음, `ArenaAllocator.h:23–25`) | CP3 |
| 설정 변수 | 코드 없음(UnrealCoreFeatureAudit UE-C29 **후속**, config 계층 UE-C21 **후속**). 계획 24.2절에 Config 우선순위만 있다 | CP4 |
| 계측 | `ProfileScope`(한 스레드 `LogBuffer`에 기록, `Core/Diagnostics/ProfileScope.h:11`). 다중 스레드 trace·채널·GPU 상관 없음(UE-C23 **부분**) | CP5 |
| 객체 쪽 지연 회수 | `BackendLease`(저장소 회수만 늦춘다. 주석에 "현재 실제 GPU/IO 작업과 연결되지는 않는다", `Core/Source/Runtime/ObjectRuntime/Public/Object/ObjectRefs.h:215–220`) | RB2에서 GPU 완료에 연결 |
| 그 밖 | `Delegate`·`MulticastDelegate`(`Core/Delegate.h:88,567`), JSON(`Core/Text/Json.h`), SHA-256, Archive·Record, `Name`(`Core/Text/Name.h:22`, 엔진 전역 pool 미정) | CP6·CP7 |

### 4.2 선행 작업 목록

ID는 이 문서가 붙인다. CP는 Core 선행, GX는 Graphics 확장, RB는 RenderBase 단계, RG는 RenderGraph 단계다. 셰이더 시스템의 P·SH, 에셋 시스템의 A, 리플렉션의 B는 그 문서 ID 그대로 쓴다. 규모는 S(며칠)·M(1–2주)·L(그 이상)의 대략값이다.

| ID | 작업 | 상태 | 의존 | 막는 것 | 규모 | 상세 |
|---|---|---|---|---|---|---|
| CP1 | 전용 lane 실행기, 스레드 이름·우선순위 | 미설계 | Thread·Sync(구현) | RB1 | M | 5.2절 |
| CP2 | 직렬 작업 체인·ParallelFor·작업 문맥 표지 | 미설계 | WorkerScheduler(구현) | RG3, RB3(병렬 셰이더 생성) | M | 4.3절 |
| CP3 | 프레임 arena(소멸자 추적·일괄 해제·다중 스레드 묶음 할당) | 미설계 | Allocator(구현) | RG1 | M | 4.3절 |
| CP4 | 콘솔 설정(타입 있는 변수·명령·출처·렌더 snapshot) | 미설계, 별도 스펙 필요 | Config 계층(UE-C21)과 함께 설계 | RB1(설정 snapshot), RG1(디버그 설정) | M | 12절 |
| CP5 | 다중 스레드 CPU 구간 계측·카운터·GPU 상관 | 부분 | ProfileScope(구현) | 13절 계측 | M | 13절 |
| CP6 | 엔진 수명 이벤트(종료·오류 종료·프레임 경계) | 미설계 | Delegate(구현) | RB1 종료 순서 | S | 4.3절 |
| CP7 | 엔진 전역 `Name` pool 결정 | 미결정 | Name(구현) | RB3(셰이더 타입 이름) | S | 4.3절 |
| GX1 | 장치 능력 `DeviceCaps` | 미설계 | A0-G G1, 셰이더 시스템 P6a | RB3, RG1 | M | 4.4절 |
| GX2 | 자원 desc·view 확장 | 미설계 | A0-G | RB2, RG1 | L | 4.4절 |
| GX3 | 파이프라인 완성(SH2 범위 밖 부분) | 미설계 | 셰이더 시스템 SH2·P2·P14 | RB4, RB6 | L | 4.4절 |
| GX4 | 명령 기록 확장 | 미설계 | GX2·GX3 | RG1 | L | 4.4절 |
| GX5 | 명시 접근 상태와 전이 | 미설계 | GX2·GX4 | RG1 | M–L | 4.4절 |
| GX6 | 파라미터 바인딩 런타임(상수 버퍼·묶음 설정·공유 slot) | 미설계 | P2·SH2 | RB4 | L | 4.4절, 9절 |
| GX7 | 제출·완료·회수(계획 11.2) | 설계(계획 11.2) | — | RB1·RB2 | L | 4.4절, 6절 |
| GX8 | 업로드 ring·비동기 readback·fence 객체 | 미설계 | GX7 | RB2(업로드), 13절 | M | 4.4절 |
| GX9 | timestamp query·debug marker | 미설계 | GX7 | 13절 | M | 4.4절, 13절 |
| GX10 | 장치 소유 스레드 이관 | 미설계 | — | RB1 | S | 5.4절 |

### 4.3 Core 선행 작업

**CP1 전용 lane 실행기.** 스레드 하나가 직렬 FIFO 큐를 소비하는 일반 실행기다. Audio·Physics lane(계획 8.1절)도 같은 형태가 필요하므로 렌더 전용으로 만들지 않고 Core Task에 둔다(공용 기능 중복 금지).
- 요건: 이동 전용 작업, 다중 생산자, 시작·종료 handshake를 이벤트로 처리, 종료 때 Drain 뒤 Join, 같은 스레드에서 바로 실행하는 인라인 모드(헤드리스·시험·결정적 기준), 현재 스레드가 그 lane인지 판별.
- 스레드 이름·우선순위·스택 크기(`PlatformConcurrency.ko.md:42` 후속)를 함께 넣는다. 디버거·trace에서 lane을 구분해야 한다. affinity는 후속이다.
- UE 대응: 전용 스레드와 task graph 이름 붙은 스레드(`RC/Private/RenderingThread.cpp:321, 227–262`, `Core/Public/Async/TaskGraphInterfaces.h:63,353`).
- 완료 기준: 다중 생산자 FIFO 순서, 종료 Drain(남은 작업 수 0), 인라인 모드와 스레드 모드의 결과 동일, 다른 스레드에서 lane 전용 API 호출 거부. 시험은 sleep 없이 이벤트 gate로 순서를 만든다.

**CP2 직렬 작업 체인·ParallelFor.** 이전 작업을 선행으로 거는 직렬 체인(UE `UE::Tasks::FPipe`, RDG 설정 task `RC/Public/RenderGraphBuilder.h:256–284`), 범위 분할 병렬 실행, 작업 문맥 표지(어느 lane의 일인지)다. 렌더 그래프 병렬 기록(RG3)과 셰이더 일괄 생성이 쓴다.

**CP3 프레임 arena.** 그래프 수명 객체(pass·자원·파라미터 구조체)를 할당하고, 그래프가 끝날 때 등록된 소멸자를 부른 뒤 한 번에 비운다. 여러 스레드가 블록 단위로 할당한다. 기존 `ArenaAllocator`는 반환 안 된 할당이 있으면 Reset을 거부하므로(`ArenaAllocator.h:23–24`) 별도 타입이 필요하다. UE 대응: `FMemStackBase`(`Core/Public/Misc/MemStack.h:77`), RDG allocator(`RC/Public/RenderGraphAllocator.h:23,129`), 동시 linear 할당기(`Core/Public/Experimental/ConcurrentLinearAllocator.h:769`).

**CP6 엔진 수명 이벤트.** 렌더 lane 종료 순서(5.5절)가 엔진 종료 단계에 걸려야 한다. 계획 3.2절의 Shutdown 순서("GPU fence … 완료" 단계)를 이벤트로 드러낸다.

**CP7 Name pool.** 셰이더 타입·형식·pass 이름을 `Name`으로 다룰지, 정적 문자열과 SHA-256으로 다룰지 정한다. 셰이더 시스템은 키에 이름 정렬·SHA-256을 쓰므로(12.2절) 런타임 조회만 정하면 된다.

### 4.4 Graphics 확장(GX)

RenderCore가 쓰는 RHI 기능군과 Rocks Graphics의 차이다. "SH2" 열은 셰이더 시스템 SH2가 이미 계획한 것(○), 다른 선행이 일부 다루는 것(△), 셰이더 시스템에 없는 것(×)이다.

| 기능군 | UE 5.8 근거(`RHI/Public`) | RenderCore가 쓰는 곳 | Rocks | SH2 | GX |
|---|---|---|---|---|---|
| 장치 능력 | `RHIFeatureLevel.h:109`, `RHIShaderPlatform.h:82`, `DataDrivenShaderPlatformInfo.h:19`, `RHIGlobals.h:87`(약 45종 사용) | `GlobalShader.cpp:484,526`, RDG barrier 병합·분할 판단 | 없음(`DeviceInfo`는 backend·이름·검증·소프트웨어만, `DeviceTypes.h:174–179`) | △ P6a·A0-G G1 | GX1 |
| 자원 desc | `RHIResources.h:1466`(버퍼), `:1965`(텍스처) | `RC/Private/GlobalRenderResources.cpp:59,86`, `RenderGraphResourcePool.cpp:190` | 일부 | △ A0-G(형식·차원) | GX2 |
| view(SRV·UAV) | `RHIResources.h:2713, 3359, 3369` | `GlobalRenderResources.cpp:99`, `DynamicBufferAllocator.cpp:188` | 없음(텍스처 하나 = SRV 하나 고정) | × | GX2 |
| 상태 객체 | `DynamicRHI.h:232–241` | `RC/Private/ClearQuad.cpp:44–68` | 일부(샘플러는 텍스처 소속, 블렌드 bool, 래스터·깊이 고정) | ○ 래스터·깊이, × 블렌드 MRT·스텐실 | GX3 |
| 그래픽·compute 파이프라인 | `RHIResources.h:4655, 1111`, `PipelineStateCache.h:173` | `ClearQuad.cpp:65,95` | 일부(원문 컴파일 + 생성, compute 없음) | ○ 코드로 생성·layout, × compute·캐시 | GX3, RB4 |
| 명령 목록 | `RHICommandList.h:2493, 3590, 4388, 5047` | `PixelShaderUtils.cpp:24`, `RenderGraphUtils.cpp:401,432` | 일부(삼각형 Draw만) | × | GX4 |
| 병렬 기록·제출 | `RHICommandList.h:4440–4458`, `RHIResources.h:5646` | `RenderGraphBuilder.cpp:2093, 2995` | 없음 | × | RG3 |
| 접근 상태·전이 | `RHIAccess.h:14–60`, `RHITransition.h:118, 355–404`, `RHIPipeline.h:12–20` | `RenderGraphPass.cpp:270–312` | 내부 고정만 | × | GX5 |
| render pass | `RHIResources.h:5346` | `RenderGraphBuilder.cpp:3450` | 일부(색 1개 + clear) | × | GX4 |
| 파라미터 묶음 설정 | `RHIShaderParameters.h:246–330`, `RHIContext.h:341, 749` | `ShaderParameterStruct.cpp:975–990` | 없음(`SetConstants(DrawConstants)` 80 byte) | △ P2·SH3 | GX6 |
| 상수 버퍼·layout·수명 | `RHIResources.h:1183`, `RHIDefinitions.h:517–525`, `DynamicRHI.h:1165` | `ShaderParameterMetadata.cpp:207, 920` | 없음(D3D12 루트 상수 20개만, `D3D12Device.cpp:1086–1121`) | △ P2·SH2 | GX6 |
| transient 할당 | `RHITransientResourceAllocator.h:535–555`, 기본 nullptr `DynamicRHI.h:312` | `RenderGraphBuilder.cpp:591` | 없음 | × | RG5 |
| 업로드·lock | `RHICommandList.h:753, 846` | `GlobalRenderResources.cpp:335, 835` | 일부(생성 때만) | × | GX8 |
| readback | `RHIGPUReadback.h:132,151` | `GPUMessaging.cpp:121`, `RenderGraphUtils.cpp:829` | 일부(시험용 동기) | × | GX8 |
| query | `DynamicRHI.h:1322`, `RHIResources.h:2454, 2490` | `RenderGraphEvent.cpp:166` | 없음 | × | GX9 |
| GPU fence 객체 | `RHIResources.h:2397` | `DumpGPU.cpp:1133` | 내부 전용 | × | GX7·GX8 |
| 자원 수명 | `RHIResources.h:53, 65–87`, 지연 삭제 `RHI/Private/RHIResources.cpp:46–58` | `RenderGraphBuilder.cpp:332` | 일부(slot+generation, D3D12 fence 뒤 회수) | × | GX7 |
| breadcrumb·GPU 이벤트 | `RHIBreadcrumbs.h:1300`, `GPUProfiler.h:92–236` | `RenderGraphBuilder.cpp:1769–1778` | 없음 | × | GX9 |
| multi-GPU mask | `MultiGPU.h:32` | 27곳 | 해당 없음 | — | 넣지 않는다(UE도 Windows 데스크톱에서만 켜고 꺼지면 mask가 사소해진다, `RHI.Build.cs:23`, `MultiGPU.h:19,233`) |

각 GX의 내용:

- **GX1 장치 능력.** 셰이더 형식 목록(셰이더 시스템 12.3절 런타임 형식 선택), 형식별 지원 표(A0-G G1), 전역 한도(dispatch 그룹 수·view 크기·MRT 수·상수 버퍼 크기), 동작 표시(split barrier·async compute·transient aliasing·timestamp·tiled GPU 여부). 셰이더 시스템의 형식 정의(12.1절)가 "쿠킹 때 가정하는 상한"이고 `DeviceCaps`는 "실행 장치의 실제 값"이다. 시작 때 장치 값이 형식 정의 상한 이상인지 검사하고, 아니면 그 형식을 고르지 않는다(셰이더 시스템 12.3절 "장치가 지원하는 첫 형식").
- **GX2 자원 desc·view.** 버퍼 용도(정점·index·상수·structured·raw·간접 인자·UAV 조합), 텍스처 차원(A0-G)·깊이/스텐실 형식·MSAA·용도 플래그(샘플·렌더 타깃·깊이·UAV)·mip·array·clear 값, SRV/UAV view 객체(mip·slice 범위). 텍스처마다 샘플러를 정하는 현재 규칙(셰이더 시스템 7.2절 9항)은 유지한다.
- **GX3 파이프라인 완성.** SH2가 하는 일(컴파일된 코드로 셰이더 생성·파이프라인 layout·root signature·래스터·깊이)에 더해 다음을 넣는다. MRT 블렌드(타깃별)·스텐실, compute 파이프라인, 다중 stream 정점 배치(10.3절), 속성 없는 파이프라인(정점 입력 없음, 11.2절), 기본도형 종류(삼각형 목록·띠, 선, 점). SH2 범위를 넓히는 셰이더 시스템 개정으로 처리한다(9.8절 SX10).
- **GX4 명령 기록 확장.** index·instanced·간접 draw, 버퍼 없는 draw, dispatch(간접 포함), 버퍼·텍스처 복사, UAV clear, viewport·scissor, attachment 시작·끝(색 MRT·깊이·load/store·clear 값, mip·slice), 그리기별 상수의 임의 크기 바이트(`SetConstants` 고정 타입 일반화, 9.8절 SX2), 여러 기록 객체와 제출 순서. 지금의 `BeginFrame`·`BeginOffscreen`(색 1개 + clear 고정)을 attachment 시작으로 바꾼다.
- **GX5 명시 접근 상태와 전이.** 추상 접근 비트(읽기: 정점·index·상수·SRV(그래픽·compute)·간접 인자·복사 원본·present / 쓰기: RTV·DSV·UAV·복사 대상 / 깊이 읽기 전용), pipe(그래픽·async compute), subresource 범위, 처음 상태 unknown·discard. 지금 D3D12의 휴지 상태 관례(RT는 PIXEL_SHADER_RESOURCE, 백버퍼는 PRESENT)를 걷어 내고 전이를 호출자(렌더 그래프)가 정한다. backend lowering:
  - D3D12: 1차는 legacy barrier. enhanced barrier 선택은 backend 내부 결정이다(UE도 RHI 안에서 고른다, `D3D12RHI/Private/Windows/WindowsD3D12BarriersFactory.h:13–21`).
  - Vulkan: pipeline barrier.
  - OpenGL ES: 전이는 대부분 생략하고 UAV(image·storage) 쓰기 뒤 읽기에 `glMemoryBarrier`를 둔다(UE는 dispatch 앞뒤에 `GL_ALL_BARRIER_BITS`, `OpenGLDrv/Private/OpenGLCommands.cpp:2671–2673`). Rocks는 접근 비트에서 필요한 barrier bit만 고른다.
  - D3D11·Metal: SH5에서 정한다(미확인).
- **GX6 파라미터 바인딩 런타임.** 9.3–9.5절을 Graphics 쪽에서 받는 부분이다: 상수 버퍼 생성(수명 3종)·갱신, 파라미터 묶음 설정, 공유 상수 slot 표. GLES는 상수 버퍼가 CPU 사본이고 바뀐 버퍼만 올린다(셰이더 시스템 7.2절 4항, 9.4절).
- **GX7 제출·완료·회수.** 계획 11.2절의 `RenderDevice` 계약(`Submit`·`Retire`·`GpuFencePoint`·`RenderResourceLease`·device epoch)을 Graphics `Device`에 들인다. OpenGL ES도 fence sync(`glFenceSync`)로 같은 계약을 따르고 "즉시 삭제" 예외를 없앤다. 장치가 내부에서 프레임 2개를 돌리는 지금 방식은 프레임 slot(5.3절)이 넘겨받는다.
- **GX8 업로드·readback.** 프레임 slot별 업로드 ring(map 쓰기 → 복사), staging과 fence 폴링에 의한 비동기 readback. `ReadPixels`(시험용 동기)는 남겨 두되 프레임 경로에서 쓰지 않는다.
- **GX9 timestamp·marker.** 13절.
- **GX10 장치 소유 스레드 이관.** 5.4절.

## 5. 렌더 lane

### 5.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 스레드 | 전용 OS 스레드 하나를 task graph의 렌더 스레드로 붙이고 그 큐를 처리한다. task 기반이 아니다 | `RC/Private/RenderingThread.cpp:321, 227–262` |
| 시작 | 스트리밍 정지 → flush → 게임 스레드가 RHI 소유권을 놓음 → RHI 스레드 모드 결정 → 스레드 생성·붙음 대기 → 설정 전파기 등록 → 펜스 왕복 → heartbeat 스레드 | `:561–654`(소유권 `:582`) |
| 종료 | heartbeat 정지 → flush → RHI 스레드 삭제 → 렌더 루프 탈출 작업 → Join → 게임 스레드가 소유권을 다시 잡음 → 지연 정리 묶음 삭제 | `:668–760`(소유권 `:746`) |
| 인라인 경로 | 렌더 스레드가 없으면 명령을 바로 실행한다 | `RC/Public/RenderingThread.h:149, 504–525`, `Launch/Private/LaunchEngineLoop.cpp:2869–2872` |
| 명령 | 이동 전용 함수 세 형태(인자 없음·명령 목록·immediate 명령 목록) | `RC/Public/RenderingThread.h:180–184, 1087–1089` |
| 명령 큐 | 뮤텍스로 보호하는 큐에 넣고, 비어 있다 처음 찰 때만 렌더 스레드용 task 하나를 띄워 FIFO로 비운다 | `RC/Private/RenderingThread.cpp:1905–1935`, `RC/Public/RenderingThread.h:394–435` |
| 보조 직렬 pipe | worker에서 직렬 실행하는 pipe. 엔진 전체에 SkeletalMesh 하나뿐 | `RC/Public/RenderingThread.h:561`, `RC/Private/RenderingThread.cpp:2380–2416`, `Engine/Private/Rendering/RenderCommandPipes.cpp:5` |
| 지연 기록 명령 목록 | 다른 스레드에서 잠금 없이 기록해 두었다가 나중에 끼워 넣는다(5.7에서 추가) | `RC/Public/RenderingThread.h:725–740, 803–832` |
| 동기화 | 펜스는 깊이 셋(렌더 스레드 처리·GPU 제출·스왑체인 flip). 게임 대기는 1 ms 단위로 기다리며 메시지 펌프와 상태 검사를 돌리고, 120초를 넘기면 hang 보고 후 종료 | `RC/Public/RenderCommandFence.h:15–53`, `RC/Private/RenderingThread.cpp:977–1051, 1089, 1096–1257` |
| 프레임 지연 한도 | 렌더 스레드 펜스는 N−1 프레임(`r.OneFrameThreadLag`), 더 깊은 단계는 `r.GTSyncType`에 따라 기본 N−2까지 겹친다 | `:2435–2451, 2454–2572` |
| 실패 | Windows SEH로 잡아 상태를 기록하고 게임 스레드가 감지해 종료 | `:380–405, 816–842` |
| 렌더 스레드 tickable | heartbeat 스레드가 sleep 폴링으로 tick 명령을 넣는다 | `:466–484`, `RC/Public/TickableObjectRenderThread.h:61–95` |

### 5.2 Rocks 구조

```text
게임 lane                                 렌더 lane(DedicatedLane)                  장치
FrameSlot 확보 ─(slot 없으면 대기)──┐
World → FramePacket 추출             │
RenderLane::Post(packet 처리) ──────▶ packet 처리: 그래프 구성·기록 ──────────▶ Submit → GpuFencePoint
RenderLane::Post(자원 생성·Retire)──▶ DeviceResourceOwner 갱신                  │
                                      slot 완료 표지(lane 처리 완료) ◀────────── fence 완료(GPU 완료)
```

- 렌더 lane은 CP1 `DedicatedLane`의 인스턴스다. lane 작업은 handle·값만 담은 이동 전용 작업이다. lane 작업 종류는 셋으로 닫는다: 프레임 packet 처리, 장치 자원 생성·갱신·`Retire`, lane 표지. 게임 객체 주소를 담은 임의 closure는 받지 않는다(계획 8.1절). 이 규칙은 API 형태로 강제한다(작업 타입이 handle·값 타입만 받는다).
- 인라인 모드: lane 스레드를 만들지 않고 `Post`가 그 자리에서 실행한다. 헤드리스·CPU 시험·결정적 기준 실행이 쓴다. 두 모드의 결과(그래프 기록 열, 자원 상태)가 같아야 한다.
- 보조 직렬 pipe와 지연 기록 명령 목록(UE 5.7)은 후속이다. 병렬 장면 준비(계획 8.4절)가 생길 때 CP2 직렬 체인으로 만든다.

### 5.3 프레임 slot과 대기 규칙

- 프레임 slot은 2–3개다(계획 8.2절, 개수는 설정). slot마다 완료 표지가 둘이다: **lane 처리 완료**(렌더 lane이 그 프레임 packet을 다 처리)와 **GPU 완료**(그 프레임의 모든 queue `GpuFencePoint` 완료). UE 펜스 깊이 RenderThread·RHIThread에 해당한다.
- 게임 lane은 프레임 시작 때 slot을 확보한다. 빈 slot이 없으면 가장 오래된 slot의 GPU 완료를 기다린다. 이것이 게임 lane이 막히는 유일한 곳이며 한 방향 대기다(렌더 lane은 게임 lane을 기다리지 않는다). 계획 8.1절 "Game/Render 상호 동기 wait 금지"와 8.2절 backpressure를 함께 만족한다.
- 전체 비우기(`RenderLane::Drain`, UE `FlushRenderingCommands`)는 종료·장치 재생성·시험에만 쓰며, 이 API는 호출 이유(열거)를 인자로 받아 계측에 남긴다. 일반 게임 코드의 자원 해제는 `Retire`로 하고 비우기를 쓰지 않는다(UE `ReleaseResourceAndFlush`에 해당하는 API는 두지 않는다).
- 대기 시간 상한(UE 120초 hang 판정)은 진단 전용이다. 시간 초과는 hang 보고 후 실패로 끝내되 기능 판정에 쓰지 않고, 디버거 연결·명시 정지 계수(UE `SuspendRenderThreadTimeout`)가 있으면 끈다. 시험은 시간 초과에 기대지 않는다.
- 렌더 설정(CP4)은 프레임 packet에 snapshot으로 담는다. UE처럼 렌더 쪽 사본 주소에 쓰는 전파는 하지 않는다.

### 5.4 장치 소유 스레드

지금 `Device`와 `Renderer`는 "생성한 스레드에서만 사용"이다(`Device.h:43`, `Renderer.h:56`). UE는 시작·종료 때 RHI 소유권을 게임 스레드와 렌더 스레드 사이에서 넘긴다(`RC/Private/RenderingThread.cpp:582, 746`, `RHI/Public/DynamicRHI.h:1497`).

GX10: 계약을 "소유 스레드에서만 사용 + 명시 이관"으로 바꾼다. 이관은 소유 스레드가 `Release`하고 새 스레드가 `Acquire`하며, 그 사이 다른 호출은 `WrongThread`로 거부한다(Development 검사). 렌더 lane 시작 때 장치를 lane으로 넘기고, 종료 때 되돌린다. 장치를 렌더 lane 안에서 만드는 방식도 가능하지만 창·스왑체인 생성이 게임 lane 쪽 PlatformWindow와 엮이므로 이관 방식을 기본으로 한다. Rocks D3D12 장치가 호출 스레드를 실제로 검사하는지는 미확인이다(코드에 `WrongThread` 사용이 없다).

### 5.5 시작·종료 순서

시작: 장치 생성(게임 lane) → `DeviceCaps` 조회 → 렌더 lane 시작(스레드 이름 붙임) → 장치 소유권 이관 → 렌더 lane에서 내장 자원 팩 생성(6.3절) → `BuiltinReady` 게시 → 엔진 셰이더 집합 열기와 완결성 검사(9.2절) → 일반 에셋·프레임 시작.

종료(계획 3.2절 Shutdown 순서 안의 "GPU fence … 완료" 단계): 새 프레임 packet 차단 → 마지막 slot의 GPU 완료 대기 → `Retire`된 자원 회수 → 내장 자원 해제 → 장치 소유권을 게임 lane으로 되돌림 → 렌더 lane Drain·Join → 장치 `WaitIdle`·소멸. 실패한 단계는 그 뒤 단계를 건너뛰지 않고 오류로 보고한다.

### 5.6 실패 전파

렌더 lane의 장치 손실·치명 오류는 lane 상태에 기록하고, 게임 lane은 다음 slot 확보 결과(`Result`)로 관찰한다. UE의 상태 폴링(`CheckRenderingThreadHealth`)과 SEH 포착은 플랫폼 Fatal 처리(기존 `Platform/Fatal.h`) 쪽 일이다. 장치 손실 뒤의 재생성은 계획 11.2절(device epoch, 구 장치 recovery domain)을 따른다.

### 5.7 완료 기준(RB1)

- 인라인 모드와 lane 모드에서 같은 프레임 packet 열이 같은 장치 호출 열을 낸다(기록 backend 대조).
- slot 2개 설정에서 GPU 완료를 gate로 막으면 게임 lane이 세 번째 slot 확보에서 대기하고, gate를 열면 진행한다(시간이 아니라 gate로 단언).
- 게임 객체 주소를 담는 lane 작업은 컴파일 오류다(음성 대조).
- 종료 순서: 진행 중 packet이 있는 상태의 종료에서 남은 작업 0, 해제 누락 0, 장치 소유권 복귀.
- 장치 손실 주입 뒤 다음 slot 확보가 오류를 돌려준다.

## 6. 장치 자원 수명

### 6.1 UE 5.8 방식

- 렌더 쪽 자원 기반 객체가 장치 자원 초기화·해제 가상 함수(`InitRHI(FRHICommandListBase&)`·`ReleaseRHI()`)와 `InitResource`·`ReleaseResource`·`UpdateRHI`를 가진다(`RC/Public/RenderResource.h:36, 83–108`). 초기화 상태로 소멸하면 Fatal이다(`RC/Private/RenderResource.cpp:247–256`).
- 게임 스레드는 자원 포인터를 캡처한 명령으로 초기화·갱신·해제를 넣는다(`:290–390`).
- 전역 자원은 Pre·Default 단계 목록에 올렸다가 RHI 준비 뒤 일괄 초기화하고(`:121–140`, 호출 예 `D3D12RHI/Private/Windows/WindowsD3D12Device.cpp:2029`), 장치 재생성·기능 수준 변경 때 목록 전체를 해제·재초기화한다(`:142–162`).
- 지연 정리는 프레임 끝 동기 한 번 뒤 delete(`RC/Private/RenderingThread.cpp:1323–1419`), RHI 자원은 참조 0에서 지연 삭제 목록(`RHI/Private/RHIResources.cpp:46–58`).

### 6.2 Rocks 규칙

- **장치 자원 소유자.** 렌더 lane 쪽 객체로, 장치 handle과 상태(`Uninitialized`·`Ready`·`Retired`)를 가진다. 게임 lane은 생성 요청 때 곧바로 handle(slot+generation)을 예약받고, 실제 장치 자원 생성은 렌더 lane 작업이 한다(UE `BeginInitResource`에 해당). 예약 handle을 생성 완료 전에 그리기에 쓰면 그 그리기는 내장 대체 자원을 쓰고 진단 계수를 올린다(에셋 단계 typed fallback, 계획 9.6절).
- **회수.** 게임 lane의 해제 요청은 `Retire` 작업이다. `Retire`는 새 lease를 막고, 이미 승인된 기록(그래프 import·기록 packet)의 lease와 마지막으로 쓴 모든 queue의 `GpuFencePoint`가 끝난 뒤 장치 자원을 해제한다(계획 11.2절). 게임 쪽 CPU 데이터(에셋 bulk 등)를 GPU 완료 뒤까지 붙잡아야 하는 경우 ObjectRuntime `BackendLease`를 이 회수 완료에 연결한다(`ObjectRefs.h:215–220`의 "현재 실제 GPU/IO 작업과 연결되지는 않는다"를 해소).
- **진단.** `Ready` 상태로 파괴하면 Development에서 오류 보고·계수, Release에서는 누수 대신 `Retire` 경로로 넘긴다(UE의 Fatal보다 약하지만 누수 Gate가 잡는다, 계획 29.6절).
- **장치 재생성.** 장치 손실 뒤 새 device epoch에서 소유자 목록을 다시 초기화한다. 구 장치 handle은 stale이다(계획 11.2절 device epoch, 9.6절 BR04). 기능 수준 변경 재초기화(UE `ChangeFeatureLevel`)는 장치 재생성과 같은 경로로 처리한다.
- **고정 크기 자원과 갱신.** 버퍼·텍스처 내용 갱신은 GX8 업로드 ring을 쓰는 lane 작업이다. 같은 프레임 안 갱신 순서는 lane FIFO가 보장한다.

### 6.3 내장 자원 팩 초기화

계획 9.6절 순서를 렌더 lane 시작 단계(5.5절)에서 실행한다. UE와 다른 점: 정적 초기화 목록에 기대지 않고, 생성 실패 시 `BuiltinReady`를 게시하지 않으며 시작이 실패한다(UE에는 준비 완료 gate와 생성 실패 경로가 없다). 내용은 11.1절.

### 6.4 완료 기준(RB2)

- 계획 11.2절 시험(API04: recording·cancel·late submit·자원 교체·builtin·GPU completion 수명), 9.6절 BR01–BR04.
- 예약 handle을 생성 전에 쓴 그리기의 대체 자원 사용과 계수.
- `Retire` 뒤 lease가 남은 동안 회수 0건, fence 완료 뒤 회수(fence를 gate로 막아 단언).
- 장치 재생성 뒤 구 handle 사용이 stale 오류.

## 7. 렌더 그래프

### 7.1 UE 5.8 방식

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| 생성 | 스택 객체, prologue 감시 pass, async compute·병합·병렬 허용과 transient 할당기 결정. 병합은 tiled GPU 전용이고 async compute와 함께 켜지지 않는다 | `RC/Public/RenderGraphBuilder.h:45–48`, `RC/Private/RenderGraphBuilder.cpp:586–648`, `RC/Private/RenderGraphPrivate.h:224–234` |
| pass 추가 | 이름·파라미터 구조체(그래프 수명 할당, 넘긴 뒤 수정 금지)·플래그·람다. 파라미터 없는 pass는 자동 유지 | `RenderGraphBuilder.h:180, 203–207, 220–233`, `RenderGraphBuilder.inl:249–283` |
| 자원 수집 | 구조체 layout의 그래프 자원 목록을 훑어 subresource별 접근 상태를 모은다 | `RenderGraphBuilder.cpp:2384–2532`, `RC/Public/RenderGraphResources.h:72–131` |
| culling | 자원별 마지막 생산자로 간선을 만들고, 유지 pass·외부 출력·내보내기에서 역방향 DFS로 살린다. pass 추가 때 점진 계산 | `RenderGraphBuilder.cpp:1199–1325, 2329–2382` |
| 컴파일 | culling 정리 → render pass 병합(같은 타깃의 연속 raster pass) → async compute fork/join(수명을 구간 전체로 연장) | `:1327–1675` |
| barrier | 같은 자원의 병합 가능한 연속 상태를 합치고 바뀌는 곳에 전이. 같은 pipe면 생산 pass epilogue에서 begin, 소비 pass prologue에서 end(split) | `:3783–3928, 4439–4584`, `RC/Private/RenderGraphResources.cpp:69–152` |
| 수명·할당 | 참조 수 0인 pass에서 해제, 처음 쓰는 pass에서 할당. 풀 자원 → transient 자원 → 첫/마지막 전이 → 전이 객체 미리 생성 | `RenderGraphBuilder.cpp:1962–2044, 3590–3780, 3930–4044` |
| 실행 | pass를 선언 순서대로 실행한다(재배치 없음). pipe 전환 → prologue barrier → attachment 시작 → 람다 → 끝 → epilogue barrier | `:2104–2167, 3424–3528` |
| 풀 | 텍스처는 렌더 타깃 풀, 버퍼는 그래프 버퍼 풀. 해제 fence와 새 할당 fence가 겹치지 않으면 같은 프레임 안에서도 같은 desc 자원을 통째로 재사용(aliasing 아님). 3프레임 넘게 안 쓰면 정리 | `RC/Private/RenderTargetPool.cpp:211–245, 389`, `RC/Private/RenderGraphResourcePool.cpp:110–145` |
| transient aliasing | RHI transient 할당기(D3D12·Vulkan만 구현, OpenGL은 풀로 대체). 대상은 UAV 버퍼와 공유 아닌 텍스처 | `RHI/Public/RHITransientResourceAllocator.h:535–555`, `RenderGraphBuilder.cpp:502–535` |
| 업로드 | 그래프 실행 앞부분에서 풀 버퍼를 lock으로 채운다 | `RenderGraphBuilder.h:309–333`, `RenderGraphBuilder.cpp:2691–2839` |
| pass 종류 | Raster·Compute·AsyncCompute·Copy(그래픽 pipe)·NeverCull·SkipRenderPass·NeverMerge·NeverParallel·Readback(=Copy+NeverCull) | `RC/Public/RenderGraphDefinitions.h:129–160` |
| 디버그 | 즉시 실행 모드, 검증(한 pass 안 상충 접근·생산 전 읽기·생산 전 내보내기), 전이 로그, 자원 덮어쓰기, 이벤트 구간 | `RC/Private/RenderGraphPrivate.cpp:18–365`, `RC/Private/RenderGraphValidation.cpp:427, 583, 597, 653` |
| 5.8 특유 | 설정 task·명령 목록 설정 task, task로 기록하는 dispatch pass, 기다리지 않는 비동기 pass 표지. legacy GPU stat 구간은 아무 일도 안 한다 | `RenderGraphBuilder.h:239–284`, `RenderGraphDefinitions.h:65–107`, `RC/Public/RenderGraphEvent.h:520` |

### 7.2 계획 11.3절과의 대조

| 계획 11.3·11.5절 | UE RDG | Rocks 결정 |
|---|---|---|
| DAG 검증 | 재배치·위상 정렬 없음. 의존은 앞쪽 pass만 가리키므로 순환이 생기지 않는다. 검증은 한 pass 안 상충 접근과 생산 전 읽기다 | 선언 순서 실행, 재배치 없음(결정 R5). "DAG 검증"을 "의존 정합성 검증"으로 고친다. 계획 11.4절의 "current color를 다시 자기 조명 입력으로 쓰는 순환 거부"는 한 pass 안의 같은 subresource 읽기·쓰기 상충 검사로 한다 |
| 미사용 pass 제거 | 점진 culling | 같다 |
| lifetime 분석 | 참조 수 | 같다 |
| barrier·queue wait | split barrier, cross-pipe fence | 같다(RG2·RG4) |
| transient alias | 지원 RHI만, 없으면 풀 | 같다(RG5). alias를 끈 1차에도 **풀**이 필요하다(계획에 없던 단계) |
| copy queue → compute overlap → alias 순서 | 그래프 pipe는 그래픽·async compute 둘뿐. Copy pass는 그래픽 pipe. 업로드는 그래프 앞 lock | copy queue를 그래프 pipe로 둘지 그래프 밖 업로드·스트리밍 계층으로 둘지는 결정 R6(권장: 그래프 밖). UE 대응이 없으므로 자체 설계로 표시한다 |
| 계획에 없음 | render pass 병합(tiled GPU 전용) | OpenGL ES·Android가 필수이므로 후속 단계 RG3에 넣는다 |
| 계획에 없음 | 병렬 기록·람다 형태에 따른 실행 방식 | RG3 |
| D3D11/OpenGL 직렬 lowering(11.5) | OpenGL transition은 빈 함수, dispatch 앞뒤 memory barrier | 같다(GX5) |
| enhanced barriers(11.3) | RHI 안에서 고른다 | 그래프 쪽 접근 상태는 하나, backend가 lowering(GX5) |

### 7.3 Rocks 구조

- 그래프 객체 하나가 한 프레임(또는 한 offscreen 작업)의 pass를 모은다. 렌더 lane이 프레임 packet을 처리하면서 만든다. 그래프 수명 할당은 CP3 프레임 arena다.
- 자원: `GraphTexture`·`GraphBuffer`와 view(SRV·UAV, mip·slice 범위). 외부 자원은 `ImportTexture`·`ImportBuffer`로 들이고(처음 접근 상태 명시), `ExportTexture`·`ExportBuffer`로 내보낸다(마지막 접근 상태 명시, 기본은 SRV). 스왑체인·history·에셋 텍스처는 import다(계획 11.3절).
- pass: 이름·파라미터 구조체·종류(Raster·Compute·AsyncCompute·Copy)·플래그(`KeepAlive`·attachment 생략·병합 금지·병렬 금지)·기록 함수. 파라미터 구조체가 의존 선언이다(9.6절). 파라미터 없는 pass는 자동으로 `KeepAlive`다. readback·present pass는 `KeepAlive`다(계획 11.3절 "side effect 있는 pass는 제거 대상이 아니다").
- 접근 상태는 멤버 종류와 pass 종류의 조합으로 정한다(Raster의 SRV는 그래픽 SRV, Compute의 SRV·UAV는 compute 접근, Copy는 복사 원본·대상). UAV는 CS만 허용되므로(셰이더 시스템 7.2절 4항) Raster pass에 UAV 쓰기 멤버가 오면 검증 오류다.
- 공유 값(`GraphSharedData`): 타입별 값 하나씩, 그래프 수명.
- 즉시 실행 디버그 모드: pass를 추가하는 즉시 수집·할당·전이·실행한다. 지연 모드와 결과가 같아야 한다.
- 이벤트 구간: 13절 `GpuScope`.

## 8. 렌더 그래프 단계와 완료 기준

공통: CPU 시험은 장치 호출을 기록만 하는 기록 backend로 결정적으로 단언한다. GPU 시험은 D3D12 debug layer·Vulkan validation 오류 0과 Sandbox self-test 픽셀 대조(채널 ±8, 2%)다.

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| RG1 | 최소 그래프: 단일 graphics queue, aliasing·병합 끔, 단일 스레드 기록. 자원 생성·import·export, 파라미터 구조체 의존, 점진 culling, 참조 수 수명, 프레임 간 desc 풀, subresource 상태 추적과 읽기 상태 병합, prologue 즉시 전이, 즉시 실행 모드, 검증, 이벤트 구간 | RB4·GX4·GX5·CP3 | (a) 미사용 체인 제거와 `KeepAlive`·외부 출력·export 보존 (b) 기대 전이 열 일치, 읽기→읽기 전이 0, UAV→UAV는 생략 표시가 없을 때만 (c) 할당·해제 pass 번호 (d) 즉시·지연 모드 명령 열 동치 (e) 음성 검증: 생산 전 읽기, 한 pass 안 상충 접근, Raster pass의 UAV (f) D3D12·Vulkan·GLES 미리보기 픽셀 대조 |
| RG2 | 프레임 안 풀 재사용(수명이 겹치지 않는 같은 desc), split 전이, 그래프 안 업로드 | RG1·GX8 | 겹치지 않는 수명의 같은 desc 자원이 실제 객체 하나를 공유, split 전후 픽셀 동일 |
| RG3 | 병렬 기록, render pass 병합(tiled GPU 형식만) | RG2·CP2 | 병렬·직렬 결과 동일, 병합 구간 전이 0, GLES에서 병합 켬·끔 픽셀 동일 |
| RG4 | async compute overlap(fork/join, 수명 연장, 병합과 배타) | RG3·GX1(async compute 표시) | 계획 API06 직렬·비동기 결과 일치, 실제 queue 겹침 계측 |
| RG5 | transient aliasing(heap 배치, acquire·discard 전이). 지원하지 않는 backend는 풀 | RG4 | alias 켬·끔 픽셀 동일, transient 최대 사용량 감소 계측, validation 오류 0 |
| (결정 R6) | copy queue | — | 결정 뒤 단계 위치를 정한다 |

## 9. 셰이더 런타임

셰이더 시스템 스펙은 오프라인 컴파일·쿠킹·바인딩 규약·라이브러리 형식과 조회 키까지 정한다. RenderCore의 런타임 객체는 거의 없다. 이 절이 그 빈자리를 채우고, 셰이더 시스템 문서를 고쳐야 할 곳은 9.8절에 모은다.

### 9.1 UE 5.8 흐름

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| 등록 | 셰이더 타입 하나 = 단계 하나, 정적 등록. 타입 종류는 Global·Material·MeshMaterial 등 7종. 타입 함수표에 컴파일 거르기와 PSO 미리 준비 여부 | `RC/Public/GlobalShader.h:433`, `RC/Public/Shader.h:1219–1226, 1256–1257` |
| 퍼뮤테이션 | 차원 도메인. 5.8에는 specialization constant 차원도 있다 | `RC/Public/ShaderPermutation.h:272, 381–387, 636, 651` |
| 셰이더 객체 | 바인딩 표·파라미터 정보·UB 목록·타입 참조·자원 번호. 바인딩 표는 셰이더 생성자에서 컴파일 결과의 파라미터 지도로 만들어 셰이더 맵과 함께 저장(오프라인) | `Shader.h:733–785, 950–995`, `RC/Public/ShaderParameterStruct.h:50–56` |
| 셰이더 맵 | 내용(셰이더 객체·바인딩·타입 → 셰이더 표, memory image) / 포인터 표 / 코드 / 장치 셰이더 배열. 쿠킹된 경우 코드는 라이브러리에서 붙이고 없으면 로드 실패 | `Shader.h:2535–2636`, `RC/Private/ShaderMap.cpp:291–395` |
| 엔진 셰이더 맵 로드 | 셰이더 타입 초기화 직후. 쿠킹 실행은 캐시 파일과 "Global" 라이브러리. 없고 컴파일도 안 되면 Fatal | `Launch/Private/LaunchEngineLoop.cpp:3301–3307`, `Engine/Private/ShaderCompiler/ShaderCompiler.cpp:5314–5316, 5518–5528, 5724` |
| 장치 셰이더 | 처음 쓸 때 만든다(원자 포인터, 잠금 밖 생성·경쟁에서 진 쪽 해제, 생성 뒤 코드 해제). 로드 때 모두 만드는 선택(기본 끔) | `Shader.h:403–415`, `RC/Private/ShaderResource.cpp:566–710`, `ShaderCompiler.cpp:532–536` |
| 라이브러리 API | 열기·닫기·자원 로드·preload·셰이더 생성. 셰이더 맵 hash → 셰이더 인덱스 범위로 묶는다 | `RC/Public/ShaderCodeLibrary.h:213–288`, `RC/Public/ShaderCodeArchive.h:48–79, 206–230` |
| 파라미터 메타데이터 | 구조체 매크로가 멤버별 정렬 검사와 멤버 기술을 만든다. 용도는 파라미터 구조체·UB·데이터 기반 UB. UB는 생성 HLSL을 include, 파라미터 구조체는 생성 HLSL 없이 이름으로 매칭(느슨한 전역, 형식에 따라 루트 cbuffer로 옮김) | `RC/Public/ShaderParameterMacros.h:1308–1495`, `RC/Public/ShaderParameterMetadata.h:139–176`, `RC/Private/ShaderParameters.cpp:85–99`, `RC/Private/ShaderParameterStruct.cpp:251–330`, `RC/Private/ShaderParameterParser.cpp:1145–1290` |
| 그리기 때 설정 | 바인딩 표대로 상수 칸·자원 칸을 묶음 객체에 넣어 한 번에 올린다. Development에서 구조체 layout hash가 다르면 실패 | `ShaderParameterStruct.cpp:520–527, 912–990`, `RHI/Public/RHIShaderParameters.h:246–330` |
| UB 런타임 | 수명 3종, layout 객체, 렌더 자원 래퍼. 정적 slot(최대 255)을 명령 목록에 묶고 셰이더별 slot 표로 연결 | `RHI/Public/RHIDefinitions.h:517–525, 721`, `RC/Public/UniformBuffer.h:20–77`, `ShaderParameterMacros.h:1511–1590`, `RHI/Public/RHIResources.h:823–835`, `D3D12RHI/Private/D3D12Commands.cpp:180–211, 420–427` |
| PSO | 초기화자(정점 선언·셰이더·상태·RT 형식). 캐시는 RHI 모듈에 있고 비동기 미리 준비·캐시 확인을 둔다. 기록 파일 캐시와 시작 precompile | `RHIResources.h:4440, 4655, 4835–4842`, `RHI/Public/PipelineStateCache.h:165–280`, `RHI/Public/PipelineFileCache.h:314–405`, `RC/Public/ShaderPipelineCache.h:4–66` |

### 9.2 셰이더 집합

- **내용.** 셰이더 라이브러리(셰이더 시스템 12.2절)에서 연 (셰이더 타입, 퍼뮤테이션 ID[, 정점 입력 타입])마다: 코드 참조, 정규화 바인딩 layout(`ShaderBindingLayout`, 7.2절 3항), 파라미터 레이아웃 서명, 파라미터 바인딩 지도(9.3절), 장치 셰이더 자리.
- **표현.** UE frozen memory image를 들이지 않는다. 정규화 표는 Core Archive·Record로 읽는다. memory image의 이득(객체 단위 역직렬화 생략)에 대한 Epic의 설계 근거는 확인하지 못했고(미확인), 지금 셰이더 규모에서 필요성이 확인되지 않았다. 로드 비용이 문제가 되면 측정 M3로 다시 본다.
- **장치 셰이더 생성.** 처음 쓸 때 스레드 안전하게 한 번 만든다(UE와 같다: 잠금 밖 생성, 경쟁에서 진 쪽 해제). 만든 뒤 코드 바이트를 놓는다. 로드 때 일괄 생성 선택을 콘솔 설정으로 둔다. 장치 셰이더 해제는 `Retire` 규칙(6.2절)이다.
- **엔진 셰이더 집합.** 렌더 lane 시작 단계(5.5절)에서 엔진 내장 셰이더 라이브러리(셰이더 시스템 12.3절)를 열고, 등록된 엔진 셰이더 타입 × 그 형식에서 컴파일되는 퍼뮤테이션(P14 거르기 결과) 전체가 라이브러리에 있는지 검사한다. 하나라도 없으면 시작이 `IncompatibleCook`으로 실패한다. UE는 엔진 셰이더 맵이 없으면 Fatal, 개별 셰이더는 생성 때 Fatal이다. Rocks는 정적 등록으로 전체를 알 수 있으므로 시작 때 결정적으로 실패시킨다.
- **타입 있는 셰이더 참조** `TypedShader<T>`: (셰이더 타입, 집합)의 쌍. 렌더 그래프 pass와 그리기 유틸리티가 이것으로 셰이더를 고른다.
- **라이브러리 런타임 API.** 열기·닫기, 엔진·프로젝트·플러그인 라이브러리 조회 순서(엔진 → 프로젝트 → 플러그인, 같은 키가 둘 이상이면 쿠킹 오류), 열 때 레이아웃 서명 검사(9.3절), 묶음 단위 preload(후속).

### 9.3 파라미터 바인딩 지도와 묶음 설정

- **바인딩 지도.** 파라미터 구조체의 멤버 오프셋 → (단계, 종류, 단계 안 순번)이다. 상수 멤버는 (상수 버퍼 순번, 바이트 오프셋, 크기), 자원 멤버는 (종류, 순번), 상수 버퍼 참조 멤버는 (상수 버퍼 순번), 그래프 자원 멤버는 (종류, 순번, 그래프 자원 표시)다.
- **만드는 시점.** Rocks는 리플렉션이 런타임에 있으므로(TypeRegistry) 셰이더 집합을 열 때 라이브러리의 바인딩 layout과 파라미터 구조체 스키마로 만든다. UE는 오프라인에서 만들어 셰이더 맵에 굽는다. 만드는 비용은 측정 M3로 확인하고, 문제가 되면 라이브러리 부분으로 굽는다.
- **레이아웃 서명 런타임 검증.** 셰이더 시스템은 레이아웃 서명을 job 키에만 넣는다(10절 8항, 13절). 그러면 실행 파일과 쿠킹 라이브러리의 빌드가 어긋날 때 잘못된 오프셋으로 조용히 올라간다. 라이브러리 항목에 서명을 넣고(9.8절 SX6), 셰이더 집합을 열 때 실행 파일의 구조체 서명과 다르면 `IncompatibleCook`이다. UE는 그리기 때 Development에서만 검사한다(`ShaderParameterStruct.cpp:520–527`). Rocks는 열 때 모든 구성에서 검사한다.
- **묶음 설정** `ParameterBatch`: 상수 칸·자원·상수 버퍼 참조를 모아 장치에 한 번에 올리는 기록 객체(GX6). Development에서 null 자원·종류 불일치를 검증한다. P7(셰이더 시스템 13절) 전에는 손으로 쓴 레이아웃 표(7.2절 5항)를 입력으로 쓴다.

### 9.4 상수 버퍼

- 수명 3종: `PerDraw`(그리기 한 번, 업로드 ring에서 할당), `PerFrame`(프레임 slot 하나, slot GPU 완료 때 회수), `Persistent`(명시 `Retire`).
- backend 표현: D3D12·Vulkan·Metal·D3D11은 GPU 버퍼, OpenGL ES는 CPU 사본(셰이더 시스템 7.2절 4항 에뮬레이션)이다. GLES는 상수 버퍼 객체가 바뀜 표시를 가지고, 그리기 때 바뀐 버퍼만 복사 표대로 `glUniform4fv/4iv/4uiv`로 올린다. 셰이더 시스템이 "바뀐 버퍼만 올린다"고 정했으나 그 상태를 가진 런타임 객체가 없었다.
- layout 객체: 파라미터 구조체 스키마에서 만든다(크기·멤버 오프셋·자원 멤버 목록·그래프 자원 멤버 목록). 렌더 그래프가 이 목록만 훑어 의존을 만든다(UE layout이 그래프 자원 오프셋을 미리 계산해 두는 것과 같다, `RHI/Public/RHIResources.h:1183–1245`).

### 9.5 공유 상수 slot

뷰·장면처럼 pass 전체에 한 번 묶는 상수 버퍼다. 렌더 그래프·장면 렌더러 단계에서 필요하다(1차 화면 출력에는 필요 없다).
- slot은 정적 등록(이름, 파라미터 구조체 identity)이다. 최대 수는 형식 정의 상한 안이다.
- 셰이더 쪽: 컴파일러가 셰이더마다 정한 상수 버퍼 번호와 slot을 잇는 표가 필요하므로 `ShaderBindingLayout`의 상수 버퍼 항목에 **파라미터 구조체 identity**를 둔다(9.8절 SX8). 지금은 이름 hash만 있다(7.2절 3항).
- 명령 쪽: 기록 객체에 slot 표를 묶고, 파이프라인 설정 때 셰이더의 slot 표로 적용한다(UE `D3D12Commands.cpp:420–427`와 같다). GLES는 공유 slot도 에뮬레이션 배열로 올린다(UE의 GLES 정적 UB 처리는 미확인).

### 9.6 파라미터 구조체와 렌더 그래프

셰이더 시스템 13절은 파라미터 구조체 멤버를 "셰이더 전용 정렬 타입 + 셰이더 자원 값 타입"으로 닫아 두었다. 렌더 그래프가 같은 구조체를 의존 선언으로 쓰려면(결정 R4) 멤버 종류가 다음과 같아야 한다.

| 멤버 종류 | 뜻 | UE 5.8 대응 |
|---|---|---|
| 상수(정렬 타입) | 상수 버퍼 멤버 | `SHADER_PARAMETER` |
| 장치 자원(텍스처·버퍼 SRV·UAV) | 그래프 밖 영구 자원. 쓰기 자원이 있으면 그 pass는 외부 출력(자동 `KeepAlive`) | `SHADER_PARAMETER_TEXTURE`·`_SRV`·`_UAV`, `ShaderParameterMetadata.cpp:652–669, 822` |
| 그래프 자원(텍스처·버퍼 SRV·UAV) | 그래프가 추적하는 자원 | `SHADER_PARAMETER_RDG_TEXTURE`·`_SRV`·`_UAV`·`_RDG_BUFFER_*`, `ShaderParameterMacros.h:1697–1760` |
| 접근만 선언 | 셰이더에 묶지 않고 의존만 선언(복사·간접 인자 등) | `RDG_TEXTURE_ACCESS`·`RDG_BUFFER_ACCESS`, `:1868–1884` |
| 렌더 타깃 slot 묶음 | 색 MRT·깊이 attachment, load/store, mip·slice | `RENDER_TARGET_BINDING_SLOTS`, `:1900` |
| 상수 버퍼 참조 | 별도 상수 버퍼(그래프 소유 또는 영구) | `SHADER_PARAMETER_STRUCT_REF`·`SHADER_PARAMETER_RDG_UNIFORM_BUFFER`, `:1768, 1854` |
| 펼쳐 넣기 | 다른 파라미터 구조체를 평탄화해 포함 | `SHADER_PARAMETER_STRUCT_INCLUDE`, `:1840` |

규칙:
- 텍스처·샘플러 짝 규칙(셰이더 시스템 7.2절 9항)에 따라 텍스처 멤버마다 짝 샘플러가 정해져야 한다. 텍스처 멤버의 샘플러는 그 텍스처 desc의 샘플러를 따르므로(현재 규칙) 생성기가 짝 `SamplerState XSampler`를 자동 생성하고 구조체에 샘플러 멤버를 따로 두지 않는다. UE는 샘플러를 별개 멤버로 두고 공유도 허용한다(`ShaderParameterMacros.h:1677`). 공유·정적 샘플러는 셰이더 시스템에서 후속이다.
- "상수 버퍼 참조 = 별도 cbuffer", "펼쳐 넣기 = 평탄화"로 정하면 셰이더 시스템 7.2절 5항의 금지(cbuffer 안 구조체 배열·2단 중첩)와 충돌하지 않는다.
- 그래프 수명 구조체는 프레임 arena에 할당하고 pass에 넘긴 뒤 바꾸지 않는다.
- 셰이더가 쓰지 않는 멤버는 바인딩 layout으로 걸러 의존에서 뺀다(UE `ClearUnusedGraphResources`, `RC/Private/RenderGraphUtils.cpp:22–101`). 그래서 바인딩 지도에 멤버 오프셋이 함께 있어야 한다(9.3절).

### 9.7 파이프라인 캐시

- 파이프라인 객체는 셰이더(VS·PS 또는 CS), 정점 배치, 래스터·깊이·블렌드 상태, 렌더 타깃 형식·샘플 수, 기본도형 종류를 담는다. 내용 SHA-256(또는 같은 프로세스 안 hash) 키로 같은 파이프라인을 한 번만 만든다. 키 항목은 계획 14.1절 PSO 캐시 키(셰이더 hash·root signature·형식·샘플 수)를 따른다. root signature 캐시(셰이더 시스템 7.2절 4항)와 같은 계층에 둔다.
- UE는 이 캐시를 RHI 모듈에 둔다(`RHI/Public/PipelineStateCache.h`). Rocks는 RenderBase에 둔다: Graphics는 만들기만 하고, 캐시·수명·통계는 렌더 쪽 일이다.
- `PipelineDesc`의 원문 경로는 SH2에서 없어진다(셰이더 시스템 결정 9).
- 후속: 비동기 미리 준비(`PipelinePrewarm`, 정점 입력 타입의 정적 배치를 쓴다, 10.2절), 사용 기록 파일과 시작 precompile, 드라이버 캐시 연계.

### 9.8 셰이더 시스템 문서 개정(SX)

| ID | 셰이더 시스템 위치 | 개정 | 근거 |
|---|---|---|---|
| SX1 | 7.2절 1항(이름 없는 전역 uniform 금지) | 파라미터 구조체 하나 = 이름 있는 cbuffer 하나라는 규칙, 그 cbuffer와 루트 상수의 관계, 구조체 안 자원 멤버의 HLSL 생성 규칙(전역 `Texture2D X; SamplerState XSampler;`는 `$Globals`가 아니므로 1항 위반이 아님)을 적는다 | UE는 파라미터 구조체 상수를 느슨한 전역으로 쓰고 이름으로 매칭한다(`ShaderParameterStruct.cpp:251–330`). Rocks는 생성 cbuffer라 모순은 없으나 규칙이 비어 있다 |
| SX2 | 7.2절 2항(루트 상수) | 파라미터 구조체 상수가 루트 상수로 가는지 cbuffer로 가는지 정하는 규칙(선언 명시, 결정 R7). Graphics `SetConstants(DrawConstants)`를 파이프라인 layout이 아는 크기의 바이트로 일반화(SH2 범위) | UE의 "루트 파라미터"는 셰이더별 상수 cbuffer(`ShaderParameterMetadata.h:173–176`), "루트 상수"는 4 DWORD로 서로 다르다 |
| SX3 | 13절(P7) | 9.6절 멤버 종류 표와 규칙(그래프 자원·접근 선언·렌더 타깃 slot·상수 버퍼 참조·펼쳐 넣기·샘플러 자동 짝). `ValueKind` 추가 항목이 바뀐다. **P7 착수 전**에 정해야 한다 | 정하지 않으면 레이아웃 서명 정의와 `ValueKind`를 다시 고쳐야 한다 |
| SX4 | 12.2절(셰이더 타입) | 정점 입력 타입 축(10.2절): 키 (셰이더 타입, 정점 입력 타입, 퍼뮤테이션 ID), 거르기는 셰이더 타입 조건 AND 정점 입력 타입 조건 | UE `FShaderKey::VertexFactoryType`(`RC/Public/Shader.h:644–671`), `Engine/Private/Materials/MeshMaterialShader.cpp:178–213` |
| SX5 | 12.2절(라이브러리 형식) | 항목 표에 묶음 id 열(엔진 셰이더 = 0, 머티리얼은 셰이더 집합 id). 라이브러리 형식 버전 1을 확정하기 전에 넣는다 | UE 라이브러리는 셰이더 맵 hash → 셰이더 인덱스 범위로 묶는다(`ShaderCodeArchive.h:48–51, 221–230`) |
| SX6 | 12.2절 | 항목마다 파라미터 레이아웃 서명. 열 때 검사(9.3절) | — |
| SX7 | 12.2·12.3절 | 엔진 셰이더 집합 완결성 검사를 런타임 시작 단계의 완료 기준으로(9.2절) | UE `ShaderCompiler.cpp:5724` Fatal |
| SX8 | 7.2절 3항(`ShaderBindingLayout`) | 상수 버퍼 항목에 파라미터 구조체 identity, 자원 항목에 파라미터 구조체 멤버 오프셋 | 9.5·9.6절 |
| SX9 | 7.2절 7항(정점 입력) | `ATTRIBUTEn` 상한 n ≤ 15(16개), 정점 배치 ⊇ 셰이더 입력 마스크 검사, 형식 기본 타입과 셰이더 입력 타입 일치 검사(정규화 → float, 정수 → int·uint, GLES는 정수 attribute를 `glVertexAttribIPointer`) | 10.3절. 16은 Vulkan `maxVertexInputAttributes`·GLES 3.2 `GL_MAX_VERTEX_ATTRIBS` 최소 보장(사양 값, UE 원본으로는 확인하지 않음). UE는 선언당 17개(`RHI/Public/RHIDefinitions.h:269`) |
| SX10 | 16절 SH2 | SH2 범위에 GX3 일부(MRT 블렌드·스텐실·compute 파이프라인·다중 stream·속성 없는 파이프라인)와 셰이더 집합(9.2절)·레이아웃 서명 검사를 더하거나, 이 문서 RB3·RB4로 나눈다(결정 R8). 완료 기준에 지연 생성 경쟁 시험(명시 gate), 완결성 음성 대조, 서명 불일치 음성 대조, 파이프라인 캐시 적중 계수를 더한다 | — |
| SX11 | 6절 4항(구현 순서) | P14 뒤에 셰이더 런타임 객체(RB3)를 넣는다 | — |
| SX12 | 2절(SM6 기능) | specialization constant 차원은 후속으로 적는다 | UE 5.8 `ShaderPermutation.h:636, 651` |

### 9.9 완료 기준

- RB3(셰이더 집합): 첫 사용 생성의 경쟁(두 lane이 gate로 동시에 요청해도 생성 1회, 진 쪽 해제), 완결성 검사 음성 대조(타입 하나를 뺀 라이브러리로 시작 실패), 서명 불일치 음성 대조, 라이브러리 조회 순서와 중복 키 오류.
- RB4(바인딩·상수 버퍼·파이프라인 캐시): 셰이더 시스템 7.3절 (a)·(d)를 런타임 경로로, 파이프라인 캐시 적중·미적중 계수, `PerFrame` 상수 버퍼의 slot 회수(fence gate), GLES 바뀐 버퍼만 업로드(호출 수 계측, 7.3절 (i)), null·종류 불일치 검증의 음성 대조.

## 10. 정점 입력 타입

### 10.1 UE 5.8 방식

- 클래스마다 정적 타입 객체(이름·`.ush` 원문 경로·기능 플래그 22종·파라미터 생성·layout·정점 요소·컴파일 거르기·컴파일 환경 수정)를 등록한다(`RC/Public/VertexFactory.h:135–160, 561–578`). 생성자는 확장자 검사·이름 hash 등록을 하고, 셰이더 타입 초기화 뒤에 생기면 check 실패다(`RC/Private/VertexFactory.cpp:102–161`).
- 메시 셰이더는 셰이더 타입과 정점 입력 타입이 둘 다 허용해야 컴파일된다(`Engine/Private/Materials/MeshMaterialShader.cpp:178–213`). 셰이더 키에 정점 입력 타입이 별도 축이다(`RC/Public/Shader.h:644–671`).
- 공용 메시 셰이더는 고정 가상 경로 `/Engine/Generated/VertexFactory.ush`를 include하고, 컴파일 환경이 이를 그 타입의 `.ush`로 잇는다(`VertexFactory.h:457–476`). `.ush`는 고정 함수 인터페이스(입력 구조체·중간값·월드 위치·보간값)를 구현한다(`Shaders/Private/BasePassVertexShader.usf:47–86`). 원문 hash는 셰이더 맵 의존이다(`VertexFactory.h:439, 593–610`).
- stream 성분(버퍼·offset·stride ≤ 255·원소 타입·용도: instancing·override·manual fetch, `RC/Public/VertexStreamComponent.h:10–72`)을 모아 같은 버퍼·stride·용도를 합치고 정점 요소(stream·offset·타입·attribute 번호·stride·인스턴스 여부, `RHI/Public/RHI.h:171–182`)를 만든다(`VertexFactory.h:799–813`). attribute 번호는 셰이더 `ATTRIBUTEn`과 같다(`Shaders/Private/LocalVertexFactory.ush:52–106`, `Engine/Private/LocalVertexFactory.cpp:520–610`).
- 정점 배치는 기본·위치 전용·위치+법선 전용(깊이 pass) 셋이다(`VertexFactory.cpp:398–413`). 원소 타입 21종(`RHIDefinitions.h:477–500`).
- manual vertex fetch는 ES3.1 초과 기능 수준에서만이다. GLES는 늘 고정 입력 경로다(`VertexFactory.cpp:180–184, 262–276`).
- PSO 미리 준비를 지원하는 타입은 인스턴스 없이 정적 정점 요소 목록을 낸다(`VertexFactory.h:331, 393, 686–688`).

### 10.2 Rocks 규칙

- **등록.** 이름, `.rsh` 가상 경로, 기능 표시, 거르기 함수, 정적 정점 배치(인스턴스 데이터와 무관), 소유 파라미터 구조체를 가진다. 정적 등록 표이고, 셰이더 타입 등록(셰이더 시스템 12.2절)과 같은 시점에 닫힌다. 늦게 로드되는 모듈의 등록은 거부한다.
- **셰이더 키 축.** 셰이더 시스템 12.2절 키에 정점 입력 타입을 더한다(SX4, 결정 R9 권장안 (a) 키 필드). 엔진 셰이더(전체화면·유틸리티)는 정점 입력 타입 "없음"이다. 거르기는 셰이더 타입 조건 AND 정점 입력 타입 조건이다. job 키 7항(전처리 결과)에 정점 입력 원문이 이미 들어가므로 따로 넣을 것은 없다(include 폐포).
- **include 연결.** 공용 메시 셰이더는 고정 가상 경로 `/Engine/Generated/VertexInput.rsh`를 include한다. 워커 입력을 만드는 메인 프로세스 전처리기가 이 경로를 그 정점 입력 타입의 `.rsh`로 잇는다(셰이더 시스템 결정 6 전처리기). `.rsh`가 구현할 함수 인터페이스(입력 구조체, 월드 위치, 단계 사이 변수 채우기)는 메시 렌더링 단계에서 정한다(후속, 첫 메시 셰이더 착수 전).
- **정점 배치.** 속성마다 (location, 형식, stream 번호, offset), stream마다 (stride, 정점/인스턴스, 인스턴스 step 1). Metal 정점 buffer 상한(셰이더 시스템 7.2절 4항: 정점 stream 8개)과 맞춰 stream ≤ 8이다. 같은 버퍼·stride·용도의 stream 합치기는 UE와 같다.
- **검사.** 파이프라인 생성 때 정점 배치 ⊇ 셰이더 입력 마스크, 형식 기본 타입 일치(SX9). 위치 전용 같은 부분 배치는 깊이 pass 단계에서(후속).
- **형식.** `VertexFormat`은 Float2/3/4뿐이다(`DeviceTypes.h:27–31`). 메시 에셋 단계 전에 패킹 법선(8 bit·10:10:10:2), half UV, 8 bit 정규화 색, 정수형을 더한다(UE `PackedNormal.h`·`RHIDefinitions.h:484–498`). A0-G(텍스처·샘플러) 범위 밖이므로 메시 에셋 착수 전 별도 결정이다(15절 개정 목록).
- **manual vertex fetch**는 후속이다(GLES 불가, UE도 같다).

### 10.3 완료 기준(RB5)

등록 결정성(등록 순서와 무관한 키), 늦은 등록 거부, 키 축 포함 라이브러리 왕복, 정점 배치 ⊇ 입력 마스크·타입 불일치의 음성 대조, 다중 stream·인스턴스 stream 픽셀 대조(D3D12·Vulkan·GLES 미리보기).

## 11. 공용 자원·그리기 유틸리티

### 11.1 내장 자원 팩 1차 내용

| 자원 | 값·형식 | UE 5.8 대응 | 비고 |
|---|---|---|---|
| white·black·transparent 2D | 1×1, (1,1,1,1)·(0,0,0,1)·(0,0,0,0), 선형 RGBA8 | `GlobalRenderResources.cpp:47–72, 231–266`(B8G8R8A8, sRGB 아님, point·wrap) | 계획 9.6절 alpha 규칙과 같다 |
| flat-normal 2D | 1×1, 법선 인코딩별 중립값 | Renderer `SystemTextures.h:32`(값 미확인) | UE에서는 RenderCore 밖 |
| 차원별 대체 | array·3D·cube·cube array·depth·uint 변형 | `GlobalRenderResources.h:54–78` | A0-G 차원 확장 뒤. 9.6절 "2D white를 cube slot에 넣지 않음"과 같다 |
| 0·1 버퍼 | 빈 버퍼, black float4, white(1) | `GlobalRenderResources.h:30–51` | identity 행렬 버퍼는 Rocks 추가(9.6절) |
| 기본 메시 | cube·plane | `RenderUtils.h:402–405`(단위 cube 버퍼) | 지금 Renderer 내장 메시(삼각형·사각형)를 옮긴다 |
| checker·error 텍스처, error material, 기본 셰이더, UI 글꼴 | 9.6절 | UE는 Engine UObject 에셋(`Engine/Classes/Engine/Engine.h:903, 911`) | 에셋·셰이더 단계 |

- 지금 null 텍스처를 묶으면 backend가 직접 만든 1×1 white를 쓴다(`Device.h:15–16`, `D3D12Device.cpp:438–445`). 내장 자원 팩이 생기면 white의 소유를 팩으로 옮기고, backend에는 "빈 바인딩 방지" 용도만 남긴다.
- (값, 형식, 차원) 키로 기본 텍스처를 만드는 캐시(UE Renderer `SystemTextures.h:46–56`)는 9.6절 typed variant와 같은 역할이며, 화면 마일스톤 뒤 필요할 때 넣는다.

### 11.2 그리기 유틸리티

- **전체화면 pass.** 정점 버퍼 없이 정점 번호로 삼각형 하나를 만드는 속성 없는 파이프라인과 버퍼 없는 draw(GX3·GX4)를 쓴다. 지금은 정점 버퍼가 없으면 그리기를 무시한다(`Device.h:18–20`). UE는 화면 사각형 정점·index 버퍼를 쓴다(`RC/Private/PixelShaderUtils.cpp:20–46`). Rocks는 index 버퍼가 필요 없는 방식을 기본으로 한다.
- **상태 프리셋.** 블렌드·래스터·깊이 desc를 값 타입으로 두고 자주 쓰는 조합(불투명·알파 블렌드·가산, 컬 없음·뒤면 컬, 깊이 끔·읽기·읽기쓰기)을 이름 붙은 상수로 둔다. UE의 정적 상태 템플릿 캐시(`RC/Public/RHIStaticStates.h:20–53`)에 해당하나, Rocks는 상태를 파이프라인에 넣으므로 캐시가 필요 없다.
- **단색·clear.** 프레임 시작 clear는 attachment load 동작으로 한다(UE `AddClearRenderTargetPass`도 load 동작만 쓴다, `RenderGraphUtils.cpp:667–704`). 부분 clear(쿼드)·UAV clear 셰이더는 후속이다.
- **크기 계산.** `MipExtent`·`MipChainBytes`는 이미 있다(`GraphicsTypes/Public/GraphicsTypes/PixelFormat.h`). A0-G 형식 확장 때 형식별 바이트·채널 정보를 같이 늘린다(UE `RenderUtils.h:147–262`).
- **샘플러.** 렌더 타깃도 `TextureDesc::sampler`를 가지므로 1차는 이것으로 된다. 같은 타깃을 다른 샘플러로 번갈아 읽는 경우(view별 샘플러)는 셰이더 시스템의 공유·정적 샘플러 후속과 함께 다룬다.

### 11.3 완료 기준(RB6)

BR01–BR02(계획 9.6절), 전체화면 pass 픽셀 대조, 상태 프리셋의 backend 대조(컬링·깊이·블렌드 픽셀), backend 내부 white 제거 뒤 null 바인딩이 팩 white를 쓰는지.

## 12. 콘솔 설정(CP4)

- RenderCore는 설정 변수로 기능 전환·디버그 모드·예산을 다룬다(RDG만 해도 `r.RDG.*` 20여 개, `RC/Private/RenderGraphPrivate.cpp:18–365`). Rocks에는 이 체계가 없고 Config 계층도 후속이다(UE-C21·C29).
- 이 문서는 렌더 쪽 요구만 정한다. 체계 자체는 별도 Core 스펙이 필요하다(계획 24.2절 Config 우선순위: engine default → platform → project → device profile → user → session override, 런타임 변경 가능·재시작 필요 구분, 값 출처 표시).
- 렌더 요구:
  - 타입 있는 변수(bool·int·float·열거·문자열)와 명령, 이름 공간(렌더 그래프·셰이더·렌더 lane 등).
  - **렌더 snapshot.** 렌더 lane이 읽는 변수는 "렌더 읽기" 표시를 가지고, 게임 lane이 프레임 packet을 만들 때 그 값들의 snapshot을 담는다. 렌더 lane은 packet의 값만 읽는다(UE `ECVF_RenderThreadSafe`·`GetValueOnRenderThread`를 주소 쓰기 없이 대체).
  - 셰이더 형식별 값(UE `FShaderPlatformCachedIniValue`, `RC/Public/ShaderPlatformCachedIniValue.h:13–95`)은 셰이더 형식 정의(12.1절)나 TargetProfile 쪽으로 두고 콘솔 설정에 두지 않는다. 쿠킹 결과에 영향을 주는 값은 키에 들어가야 하기 때문이다.
  - 헤드리스·시험에서 결정적 값을 고정할 수 있어야 한다.

## 13. GPU 계측

### 13.1 UE 5.8 방식

- GPU 프로파일링이 RHI로 옮겨졌다. RenderCore 헤더는 매크로만 남았다: draw event → RHI breadcrumb, GPU stat → RHI GPU 프로파일러, 옛 GPU stat 매크로는 아무 일도 안 한다(`RC/Public/ProfilingDebugging/RealtimeGPUProfiler.h:102–158`). breadcrumb은 Debug·Development·profile 빌드에서만 켜진다(`RHI/Public/RHIBreadcrumbs.h:26`).
- 구간 시작·끝마다 프로파일러 이벤트와 timestamp query를 넣는다(D3D12 `D3D12RHI/Private/D3D12CommandContext.cpp:175–176`, OpenGL은 시작 TOP·끝 BOP query와 debug group, `OpenGLDrv/Private/OpenGLDrv.cpp:119–165`). 이벤트 종류는 프레임 경계·작업 시작·끝(CPU·GPU 시간 대응)·fence signal·wait다(`RHI/Public/GPUProfiler.h:92–236`). OpenGL은 GPU가 언제 일하는지 몰라 프레임 전체를 바쁨으로 표시한다(`OpenGLDrv/Private/OpenGLDevice.cpp:1531–1537`).
- 동적 해상도 예산은 별도 timestamp 풀을 쓰고 몇 프레임 늦게 비차단으로 회수한다(`RC/Private/RenderGraphEvent.cpp:166, 197–256`).
- 캡처 도구는 모듈식 제공자(시작·끝·프레임 캡처)이고 RenderDoc·PIX·Xcode 플러그인이 구현한다(`RC/Public/IRenderCaptureProvider.h:71–88`, `RC/Public/RenderCaptureInterface.h:20–30`).

### 13.2 Rocks 규칙

- **`GpuScope`.** 이름 구간 하나가 debug marker push·pop과 timestamp 한 쌍을 함께 기록한다. 렌더 그래프는 pass마다 자동으로 구간을 둔다(계획 11.3절 "pass 이름 기록"). Release에서는 marker를 끄고 timestamp는 설정으로 켠다.
- **timestamp query**(GX9): 프레임 slot 링으로 회수하고, 결과가 아직 없으면 "미완료"로 표시한다. disjoint 표시가 있으면 그 구간을 무효로 한다(계획 29.4절). GLES timestamp는 확장(`GL_EXT_disjoint_timer_query`)이 있을 때만이고, 없으면 GPU 시간을 "없음"으로 공개하며 추정값을 만들지 않는다(UE는 Android에서 `glQueryCounterEXT`를 쓴다, `OpenGLDrv/Private/Android/AndroidOpenGLFunctions.h:77`).
- **CPU·GPU 시간 대응**: 계획 29.1절 calibration pair. D3D12 `GetClockCalibration`, Vulkan `VK_EXT_calibrated_timestamps` 등 backend별 방법은 GX9에서 정한다(UE D3D12가 쓰는 API는 미확인).
- **계수**: 그래프 pass 수·draw·dispatch 수·transient 최대 사용량·파이프라인 캐시 적중(계획 11.3절).
- **후속(Developer 모듈)**: 캡처 도구 연동(RenderDoc·PIX), 그래프 자원 덤프, 텍스처 시각화, 셰이더 → CPU 메시지, GPU 오류 주입(개발 빌드 전용, 계획 11.2절 device loss 시험에 쓴다), 긴 GPU 작업 벤치마크.

### 13.3 완료 기준

`GpuScope` 중첩이 캡처 도구(PIX·RenderDoc)에서 pass 이름으로 보이는지(수동 확인 기록), timestamp 링 회수가 GPU 완료 gate 뒤에만 값을 내는지, GLES 확장 없음에서 "없음" 공개.

## 14. 후속·범위 밖

| 항목 | UE 5.8 파일 | 시기 |
|---|---|---|
| 런타임 mip 생성 | `GenerateMips.*` | 쿠킹 mip이 기본. 런타임 생성 렌더 타깃이 필요한 기능 단계 |
| 부분 clear·UAV clear 셰이더·compute 복사 | `ClearQuad.*`, `ClearReplacementShaders.*`, `CopyTextureShaders.*`, `OneColorShader.*` | 렌더 그래프 RG2 뒤 필요할 때 |
| MSAA resolve | `ResolveShader.*` | Forward+ MSAA(계획 11.1절) |
| 프레임 동적 버퍼·scatter upload | `DynamicBufferAllocator.*`, `UnifiedBuffer.*` | GPU Scene(계획 13절) |
| GBuffer 메타 | `GBufferInfo.*` | Deferred 경로 |
| 동적 해상도 | `DynamicRenderScaling.*` | 계획 12절 |
| 렌더러 모듈 인터페이스·장면 할당 | `RendererInterface.h`, `SceneRenderingAllocator.h`, `ReadOnlyCVARCache.*` | Renderer 설계 |
| 정점 패킹·렌더 변환 | `PackedNormal.h`, `RenderMath.h`, `RenderTransform.*` | 메시 에셋 단계(Core 수학과 중복 금지) |
| 가상 텍스처 | `VirtualTexturing*.h` | AssetSystem 12.4절 계약 |
| 레이 트레이싱 | `RayTracingGeometry*`, `BuiltInRayTracingShaders.*`, `RayTracingValidationShaders.*` | 계획 14.4절 Ray Scene, capability 확장 |
| bindless | `UpdateDescriptorHandle.*` | 셰이더 시스템 7.2절 10항 |
| 실행 중 layout 상수 버퍼, 셰이더 묶음 dispatch | `ExtensibleUniformBuffer.*`, `ShaderBundles.*` | 필요해질 때 |
| 미디어·외부 제품·스테레오·HDR 출력 | `MediaShaders.*`, `BinkShaders.*`, `OculusShaders.*`, `StereoRenderUtils.*`, `HDRHelper.h` | 각 기능 단계 |
| 렌더 lane tickable | `TickableObjectRenderThread.*` | sleep heartbeat 대신 프레임 packet이나 명시 타이머 이벤트로(시간 의존 금지) |
| 지연 기록 명령 목록·보조 직렬 pipe | `RenderingThread.h:561, 725–832` | 병렬 장면 준비(계획 8.4절) |
| GPU 제출 lane(UE RHI 스레드) | `RenderingThread.cpp:161–224` | 측정 뒤 |
| 실행 중 렌더 lane 켜기·끄기 | `RenderingThread.cpp:762–797, 1422–1451` | 넣지 않는다(시작 설정만) |
| state stream·AutoRTFM 지연 | `RenderCommandStateStream.*`, `RenderingThread.h:994–1003` | 넣지 않는다(UE에서도 실험·기본 꺼짐) |
| 펜스 묶기·해제 일괄 처리 | `RenderingThread.cpp:873–953` | GC 대량 파괴 계측 뒤(GarbageCollectionScale과 함께) |
| PSO 미리 준비·기록 파일 | `ShaderPipelineCache.*`, `PipelineCacheUtilities.*` | 9.7절 후속 |
| 셰이더 묶음 preload·묶음 압축 | `ShaderCodeArchive.h:691–773` | 패키징 단계 |
| specialization constant 차원 | `ShaderPermutation.h:636, 651` | 셰이더 시스템 후속(SX12) |
| PSO 이전 시절 바운드 셰이더 상태 | `StaticBoundShaderState.*` | 넣지 않는다 |

## 15. 다른 문서 개정

이 문서 확정과 함께 고친다. 위치는 절 번호와 인용 문구로 적는다.

| 문서 | 위치 | 현재 | 개정 |
|---|---|---|---|
| ShaderSystem | 9.8절 SX1–SX12 | — | 9.8절 표대로. SX3은 P7 착수 전, SX4–SX6은 P14 전 |
| ShaderSystem | 3절 구조 | ShaderCore에 "라이브러리 로드" | ShaderCore는 장치 없는 메타데이터·라이브러리 형식 읽기, 장치 셰이더 객체는 RenderBase(1절 모듈 배치) |
| EngineImplementationPlan | 3.1절 모듈 표 "RHI", "RenderGraph / Renderer" 행, 3.2절 트리 | RHI/{Null,D3D11,…}, RenderGraph, Renderer | Graphics(기존 이름)와 backend 모듈, RenderBase 행 추가(의존: Graphics·ShaderCore·Task·Core), RenderGraph는 RenderBase 위 |
| 〃 | 8.1절 Lane 표 Render 행, 8.2절 | Render lane, in-flight slot | 게임 대기 지점을 프레임 slot 확보 하나로 한정(5.3절), lane 작업 세 종류 |
| 〃 | 9.6절 초기화 | manifest → DeviceCaps → … → BuiltinReady | 렌더 lane 시작 단계에서 실행(5.5절), 엔진 셰이더 집합 완결성 검사를 BuiltinReady 뒤에 더함 |
| 〃 | 11.2절 RenderDevice | 별도 인터페이스 이름 | Graphics `Device`에 Submit·Retire·GpuFencePoint를 들임(GX7). OpenGL ES도 fence sync |
| 〃 | 11.3절 | "DAG 검증", copy queue → compute overlap → alias | 선언 순서 실행·의존 정합성 검증(결정 R5), 풀 단계 추가, render pass 병합(tiled) 추가, copy queue 위치(결정 R6) |
| 〃 | 11.5절 OpenGL 행 | core 4.5급 | 셰이더 시스템 14절 개정과 같이 OpenGL ES. 그래프 lowering 대상도 같다 |
| EngineRuntime | 10.1절 계약, 14절 제한 | 생성 스레드 전용, 렌더 추출은 게임 스레드 동기 | 소유 스레드 + 이관(GX10), 렌더 lane(RB1) |
| PlatformConcurrency(구현 문서) | :42 | 스레드 이름·우선순위·affinity·스택은 후속 | CP1에서 이름·우선순위·스택 크기 |
| UnrealCoreFeatureAudit | UE-C21·C23·C29 행 | 후속·부분 | CP4·CP5의 선행 관계를 적는다 |
| AssetSystem | 메시 에셋 단계(정해지면) | 정점 형식 Float만 | 패킹 법선·half·정규화 색 형식(10.2절) |

## 16. 결정 필요 사항

| ID | 결정 | 선택지와 권장 | 기한 |
|---|---|---|---|
| R1 | 이름 확정 | 2절 표. 특히 모듈 이름 `RenderBase`(대안: 렌더 기반을 `RenderCore`로 두는 것은 UE 모듈 이름이라 피한다) | 문서 확정 |
| R2 | 모듈 배치 | 권장: ShaderCore(장치 없음)·RenderBase·RenderGraph 셋으로(1절). 대안: UE처럼 RenderBase 하나에 그래프까지 | 문서 확정 |
| R3 | 게임 대기 규칙 | 권장: 프레임 slot 확보만 막힘, 전체 비우기는 종료·재생성·시험 전용(5.3절). 계획 8.1절 해석의 확인 | 문서 확정 |
| R4 | 파라미터 구조체와 그래프 의존 결합 | 권장: 같은 구조체(UE 방식, 9.6절). 대안: 셰이더 파라미터와 pass 의존 선언을 다른 구조체로 나눔(두 번 적어야 하고 미사용 멤버 거르기가 어려움). 셰이더 시스템 결정 4와 함께 | P7 전 |
| R5 | 그래프 pass 재배치 | 권장: 선언 순서, 재배치 없음(UE와 같다) | RG1 전 |
| R6 | copy queue 위치 | 권장: 그래프 밖 업로드·스트리밍 계층(UE 그래프에 copy pipe 없음). 대안: 그래프 pipe로 | RG4 전 |
| R7 | 파라미터 상수의 루트 상수/cbuffer 배치 | 권장: 선언 명시(구조체 종류에 "그리기별 상수" 표시, 128 byte 상한). 대안: 크기 기준 자동 | SH2 전(SX2) |
| R8 | 셰이더 런타임 객체를 SH2에 넣을지 | 권장: 셰이더 집합(RB3)은 SH2 완료 조건에 넣는다(SH2가 쿠킹 라이브러리 로드를 하므로 같은 변경이어야 한다). 바인딩 지도·상수 버퍼·파이프라인 캐시(RB4)는 SH2(손으로 쓴 layout)와 SH3(생성 layout)에 나눠 넣는다 | SH2 전 |
| R9 | 정점 입력 타입 키 | 권장: (a) 셰이더 키 필드(UE 방식). 대안: (b) 열거형 퍼뮤테이션 차원 | P14 전 |
| R10 | 착수 위치 | 고정 순서(A0-B → A0-O → B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)는 바꾸지 않는다. RB3·RB4는 셰이더 SH2·SH3에 묶이므로 셰이더 시스템 결정 11과 같은 위치다. CP1–CP7(Core 단독)과 GX·RB1·RB2·RG의 위치는 화면 마일스톤 범위를 정할 때 함께 정한다 | 화면 마일스톤 범위 정의 때 |
| R11 | 바인딩 지도 생성 시점 | 권장: 셰이더 집합을 열 때 런타임 리플렉션으로 생성, 측정 M3가 나쁘면 라이브러리에 굽기 | RB4 전 |

## 17. 측정

| ID | 측정 | 결과가 정하는 것 |
|---|---|---|
| M1 | 렌더 lane 작업 게시·처리 비용, 프레임 packet 크기 | 작업 묶음 단위, packet 복사 방식 |
| M2 | 파이프라인 생성 시간(backend별), 캐시 적중률 | 미리 준비(후속) 시기 |
| M3 | 셰이더 집합 열기 비용(바인딩 지도 생성 포함) | 결정 R11, memory image 재검토 |
| M4 | 그래프 컴파일 비용(pass 수 대비) | 병렬 설정(RG3) 시기 |

## 18. 구현 순서(제안)

결정 R10 전까지는 순서만 적는다. 화살표는 의존이다.

1. Core: CP1 → (CP2·CP3·CP6·CP7 병행) → CP4(별도 스펙) → CP5.
2. Graphics: GX7 → GX10 → (A0-G) → GX1·GX2 → (셰이더 SH2) → GX3 → GX4 → GX5 → GX6 → GX8 → GX9.
3. RenderBase: RB1(CP1·GX7·GX10) → RB2(GX2·GX7) → RB3(SH2·P14·P6b) → RB4(GX3·GX6, SH2·SH3) → RB5(P14·SX4) → RB6(GX3·GX4·GX9).
4. RenderGraph: RG1(RB4·GX4·GX5·CP3) → RG2 → RG3 → RG4 → RG5.

## 19. 개정 이력

- 개정 1(2026-11-16): 초안. UE 5.8.4 RenderCore 다섯 갈래 조사(렌더 스레드·자원 수명, 렌더 그래프, 셰이더 런타임, 정점 입력·공용 자원·계측, RHI·Core 선행 조건)를 종합했다. 사용자 지시 "용어들은 우리 rocks에 맞춰야함"에 따라 UE 이름은 2절 대응표와 근거 열에만 둔다.
