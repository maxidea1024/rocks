# 렌더 기반(RenderBase) 설계

기준일: 2026-11-18, 개정 16(초안). UE 5.8 RenderCore 모듈이 맡는 일을 Rocks에 세우는 구현 스펙이다. 범위는 다음과 같다.
- 렌더 lane과 프레임 전달, 장치 스레드 모델, 장치 자원 수명, 렌더 그래프
- 셰이더 런타임 객체(셰이더 집합·파라미터 바인딩·상수 블록·파이프라인 캐시)
- 정점 입력 타입, 공용 자원·그리기 유틸리티, GPU 계측
- 이것들을 올리기 전에 Core·Graphics가 갖춰야 할 **선행 작업**

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론) 원본이다. 경로는 `Engine/Source/Runtime/` 기준이며 `RenderCore/` 아래 파일은 `RC/`로 줄여 적는다. 셰이더 원문은 `Engine/Shaders/`, 플러그인은 `Engine/Plugins/` 기준이다. Rocks 경로는 저장소 루트 기준이고 HEAD `79f966a`에서 확인했다(초안 기준 `1638a7f`와 인용 파일 내용이 같다).

[셰이더 시스템](ShaderSystem.ko.md)(개정 8, 구현 전)과 겹치는 부분은 그 문서의 결정을 따른다. RenderCore 런타임 요구 때문에 그 문서를 고쳐야 하는 곳은 9.9절에 모았다. [엔진 구현 계획](EngineImplementationPlan.ko.md) 3·8·9.6·11절, [엔진 런타임](EngineRuntime.ko.md) 6·10–14절과 충돌하는 부분은 이 문서 기준으로 정리하고 그 문서들도 함께 고친다(15절).

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). 이 문서는 **설계**다. 화면 마일스톤(18.1절)에서 구현하며, 미리 해서 유리한 Core 선행 작업만 먼저 착수했다(16절 결정 R10). 다만 결정 R12(샘플러 위치)와 SX13(비교 샘플러)은 A0-G에 걸리므로 **A0-G 착수 전**에 정해 AssetSystem에 반영해야 한다(6절의 기한 가운데 가장 이르다).

**용어 방침.** UE 구조는 따르되 이름은 Rocks 이름을 쓴다. UE의 모듈·타입·함수·매크로 이름은 대응표(2절)와 "UE 5.8 방식" 표·근거 열에만 적는다. Rocks 규칙을 적는 본문에는 UE 이름 대신 근거 `파일:줄`만 단다. pass·barrier·pipeline·descriptor·render target·swap chain·fence·SRV·UAV처럼 그래픽 API와 업계에서 함께 쓰는 말은 그대로 쓴다. 계획 문서에 이미 있는 Rocks 이름(`RenderGraph`·`RenderResourceLease`·`Retire`·`GpuFencePoint`·`DeviceCaps`·`BuiltinId`·`BuiltinReady`)도 그대로 쓴다.

**UE 레거시 제외.** Rocks는 새로 만드는 엔진이므로 UE가 하위 호환을 위해 남긴 코드(deprecated API·옛 경로·이전 버전 형식 대응·전환기 이중 경로)는 조사·설계 대상이 아니다(2026-11-16 사용자 지시). UE의 현재 기본 경로만 근거로 삼고 UE 버전 사이의 변경 이력은 적지 않는다.

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 모듈 배치 | UE RenderCore 하나를 넷으로 나눈다. **ShaderCore**(장치 없는 셰이더 메타데이터, 셰이더 시스템 3절), **RenderBase**(장치에 묶인 렌더 기반), **RenderGraph**(계획 3.1절에 이미 있는 모듈), **Graphics 확장**(UE RHI가 하는 일 중 지금 Graphics에 없는 것). 디버그 도구는 Developer 모듈로 뺀다 | UE RenderCore는 셰이더 컴파일 쪽 타입(`RC/Public/ShaderCore.h`·`ShaderCompilerCore.h`)과 렌더 그래프(`RC/Public/RenderGraph*.h`)를 한 모듈에 둔다. Rocks 셰이더 컴파일러(Developer)는 장치 없이 쓰는 부분만 필요하다(셰이더 시스템 3절). 렌더 그래프는 셰이더 파라미터에 기대지만 반대 방향 의존은 없다 |
| 렌더 실행 | 전용 스레드 하나가 직렬 FIFO로 일을 처리하는 **렌더 lane**. 헤드리스·시험용 **인라인 모드**(같은 스레드에서 실행하되 큐 순서는 lane 모드와 같다)를 둔다 | UE도 전용 스레드 하나(`RC/Private/RenderingThread.cpp:321, 227–262`)와 스레드 없는 실행 경로(`RC/Public/RenderingThread.h:149, 504–525`)를 둔다. 계획 8.4절 "lane을 줄여도 진행 가능한 순차 기준 실행기" |
| 게임 → 렌더 전달 | 불변 값 묶음 **프레임 packet**과 handle만 넘긴다. 전달 가능한 타입은 명시 등록한다 | 계획 8.1절 Render lane "Game 주소 없음". UE는 게임 소유 객체 포인터를 캡처한다(`RC/Private/RenderResource.cpp:290–296`). 이 점은 따르지 않는다 |
| 게임 대기 | 게임 lane이 막히는 곳은 **프레임 slot 확보**다(backpressure). 대기 중에도 OS 메시지를 펌프한다. 예외는 표면 소실 handshake(Android)와 종료·장치 재생성·시험의 전체 비우기다 | 계획 8.1절 "Game/Render 상호 동기 wait 금지", 8.2절 backpressure. UE는 대기 중 메시지를 펌프한다(`RC/Private/RenderingThread.cpp:1158–1257`) |
| 장치 스레드 모델 | 기록 객체는 스레드마다, 제출은 렌더 lane 전용, 자원 생성의 스레드 안전 여부는 backend가 `DeviceCaps`로 공개한다(OpenGL ES는 lane에서만). 게임 lane의 handle 예약은 RenderBase 표가 맡는다 | 5.4절. 지금 `Device`는 "생성한 스레드에서만 사용"이고(`Engine/Source/Runtime/Graphics/Public/Graphics/Device.h:43`) backend에는 스레드 검사가 없다 |
| 자원 수명 | 계획 11.2절의 lease·`Retire`·모든 queue fence 확인 방식이다. UE의 "큐 순서 + 프레임 끝 동기 한 번 뒤 delete"는 쓰지 않는다 | UE `RC/Private/RenderingThread.cpp:1323–1419`. 계획 11.2절은 lease 원자 획득·Retire의 새 lease 차단·last-use fence 집계를 요구한다 |
| 내장 자원 | 엔진 셰이더 집합 검사 → 내장 자원 팩 생성·업로드 → fence → `BuiltinReady`를 렌더 lane이 명시적으로 실행한다. 정적 초기화에 기대지 않는다 | 계획 9.6절. UE 전역 자원은 정적 생성과 단계 목록에 기대고(`RC/Public/RenderResource.h:43–48, 446–505`) 준비 완료 gate와 생성 실패 경로가 없다 |
| 렌더 그래프 | pass를 선언 순서대로 실행하고 재배치하지 않는다. 1차는 단일 graphics queue·aliasing 끔·병합 끔이다 | UE 실행 루프(`RC/Private/RenderGraphBuilder.cpp:2104–2167`). 계획 11.3절 1차 범위 |
| 파라미터 구조체 | 한 구조체가 셰이더 상수·자원 바인딩과 렌더 그래프 의존 선언을 함께 맡는다(결정 R4) | UE 파라미터 매크로(`RC/Public/ShaderParameterMacros.h:1697–1760, 1868–1900`)와 그래프 수집(`RC/Private/RenderGraphBuilder.cpp:2384–2532`) |
| 셰이더 런타임 | 셰이더 라이브러리(셰이더 시스템 12절) 위에 **셰이더 집합**을 둔다. 장치 셰이더는 처음 쓸 때 만들고 게시는 한 번이다. frozen memory image는 들이지 않는다 | UE `RC/Public/Shader.h:405–417`, `RC/Private/ShaderResource.cpp:579–657`. memory image(`Core/Public/Serialization/MemoryLayout.h`)는 C++ 객체 배치를 굽는 장치이고 Rocks는 정규화 표를 Archive·Record로 읽는다 |
| 설정 변수 | Core에 **콘솔 설정**과 Config 계층을 선행 작업으로 둔다. 렌더 lane에는 프레임 packet 안 snapshot으로 넘긴다 | RenderCore에 콘솔 변수 정의가 약 190줄 있다(`git grep -c` 줄 수). UE는 렌더 쪽 사본 주소에 쓰는 명령으로 전파한다(`RC/Private/RenderingThread.cpp:493–530`). Rocks에는 체계가 없다(UnrealCoreFeatureAudit UE-C21·C29 **후속**) |
| 좌표·행렬 규약 | UE 5.8과 같다(2026-11-17 사용자 결정): 행 벡터(`p·M`)·행 우선 저장, 셰이더 `mul(v, M)`과 행 우선 packing, view space +X 오른쪽·+Y 위·+Z 전방, reversed-Z 무한 원평면 원근(수평 반각), 앞면은 화면 반시계·뒤면 컬, 음수 행렬식이면 컬 반전. 렌더 코드 반영은 C7에서 했다(Graphics `CullMode`·`DrawConstants` 행 우선, 샘플 렌더러 반전 컬, Sandbox 컬링·행렬 시험) | [셰이더 시스템](ShaderSystem.ko.md) 7.2절 6·8항, [엔진 구현 계획](EngineImplementationPlan.ko.md) 2.2절. UE `LocalPlayer.cpp:1241–1245`(view 축), `PerspectiveMatrix.h:135–153`(투영), `D3D12State.cpp:33–35, 353`(앞면) |

## 2. 용어 대응

이름은 제안이며 결정 R1로 확정한다.

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| RenderBase(모듈) | RenderCore(일부) | 장치에 묶인 렌더 기반 |
| ShaderCore(모듈, 셰이더 시스템 3절) | RenderCore의 셰이더 메타데이터 부분 | 장치 없이 쓰는 셰이더 타입·퍼뮤테이션·파라미터 메타데이터·라이브러리 형식 |
| RenderGraph(모듈, 계획 3.1절) | RenderCore의 RDG | 렌더 그래프(공용어) |
| Graphics(모듈, 기존) | RHI | 그래픽 장치 계약과 backend |
| 렌더 lane `RenderLane` | rendering thread | 렌더 일을 직렬로 처리하는 전용 스레드 |
| 전용 lane 실행기 `DedicatedLane`(Core Task) | `FRunnableThread` + named thread | 스레드 하나가 직렬 FIFO를 처리하는 일반 실행기 |
| lane 작업 게시 `RenderLane::Post` | `ENQUEUE_RENDER_COMMAND` | 등록된 값 타입만 받는 작업을 lane에 넣는다 |
| lane 표지 `LaneMarker` | `FRenderCommandFence` | lane이 어디까지 처리했는지 알리는 완료 표지 |
| 프레임 slot `FrameSlot` | `FFrameEndSync`의 프레임 지연 한도 | 동시에 진행 중인 프레임 2–3개 중 하나 |
| 프레임 packet `FramePacket` | (없음. UE는 포인터 캡처) | 게임 lane이 만든 불변 렌더 입력 묶음 |
| 완료 통지 큐 `RenderNotices` | (없음) | 렌더 lane이 게임 lane에 돌려주는 결과(readback·timestamp·회수 완료·컴파일 완료) |
| 렌더 handle `RenderHandle` | (없음. UE는 객체 포인터) | 게임 lane이 예약받는 handle. 렌더 lane이 장치 handle에 잇는다 |
| 장치 자원 소유자 `DeviceResourceOwner` | `FRenderResource` | 장치 handle과 초기화·해제 상태를 가진 렌더 쪽 객체 |
| `RenderResourceLease`·`Retire`(계획 11.2) | (없음) | 사용 lease와 회수 |
| 내장 자원 팩 `BuiltinResourcePack`·`BuiltinId`·`BuiltinReady`(계획 9.6) | `TGlobalResource`, `GWhiteTexture` 등 | 장치 생성 직후 만드는 기본 자원 |
| 그래프 텍스처·버퍼 `GraphTexture`·`GraphBuffer` | `FRDGTexture`·`FRDGBuffer` | 그래프 수명 자원 |
| 그래프 가져오기·내보내기 `ImportTexture`·`ExportTexture` | `RegisterExternalTexture`·`QueueTextureExtraction` | 그래프 밖 자원 연결(계획 11.3 import/export) |
| 그래프 자원 풀 `GraphResourcePool` | `FRenderTargetPool`·`FRDGBufferPool` | 같은 desc 자원의 프레임 안팎 재사용 |
| 그래프 공유 값 `GraphSharedData` | `FRDGBlackboard` | 그래프 수명 동안 pass 사이에 넘기는 타입별 값 |
| 그래프 텍스처 view `GraphTextureView` | `FRDGTextureSRV`·`FRDGTextureUAV` | 그래프 텍스처의 mip·slice 범위 view(7.5절) |
| 공유 자원 구조체 `SharedResourceStruct` | `SHADER_PARAMETER_STRUCT`(중첩 구조체, 멤버 이름 접두어) | 여러 pass 구조체가 포함하는 자원 멤버 구조체(9.6절) |
| 그리기별 자원 구조체 `DrawResourceStruct` | 그리기마다 묶는 uniform buffer(예: `FLightmapResourceClusterShaderParameters`) | 그리기마다 바뀌는 자원(9.3·9.6절) |
| 그래프 구간 `GraphScope` | `RDG_EVENT_SCOPE` | pass 여러 개를 감싸는 이름 구간(7.5절) |
| 부수 효과 pass `HasSideEffects` | `ERDGPassFlags::NeverCull` | 미사용 제거 대상이 아닌 pass |
| 그래프 접근 `GraphResourceAccess`, 접근 선언 builder `GraphPassAccesses` | `FRDGPass`의 자원 상태 목록(파라미터 구조체에서 수집) | pass 하나가 자원 subresource 범위를 어떤 접근으로 쓰는지(그래프 내부 접근 모델) |
| 접근 상태 `ResourceAccess`, 전이 `ResourceTransition`·`CommandContext::Transition` | `ERHIAccess`, `FRHITransitionInfo` | GX5 명시 접근 상태와 장치 전이 |
| 셰이더 집합 `ShaderSet` | `FShaderMap`(content·resource) | 라이브러리에서 연 셰이더 묶음과 장치 셰이더 |
| 타입 있는 셰이더 참조 `TypedShader<T>` | `TShaderRef<T>`·`TShaderMapRef<T>` | 셰이더 타입과 집합의 쌍 |
| 셰이더 공급자 `ShaderProvider` | (없음. UE는 엔진 안 컴파일 경로) | 쿠킹 라이브러리 대신 셰이더를 공급하는 Development 경로의 interface |
| 파라미터 바인딩 지도 `ParameterBindingMap` | `FShaderParameterBindings` | 구조체 오프셋 → (단계, 종류, 순번) |
| 파라미터 묶음 `ParameterBatch` | `FRHIBatchedShaderParameters` | 상수·자원 바인딩을 한 번에 올리는 기록 객체 |
| 상수 블록 `ConstantBlock` | uniform buffer | 셰이더 상수 버퍼의 런타임 객체. 수명은 `PerDraw`·`PerFrame`·`Persistent`. HLSL `ConstantBuffer<T>`와 이름을 나눈다 |
| 공유 상수 slot `SharedConstantSlot` | static uniform buffer slot | 패스 전체에 한 번 묶는 상수 블록 자리 |
| 파이프라인 캐시(공용어) `PipelineCache` | `PipelineStateCache`(RHI) | 내용 키로 파이프라인을 한 번만 만든다 |
| 파이프라인 미리 준비 `PipelinePrewarm` | PSO precache | 비동기 사전 생성(후속) |
| 정점 입력 타입 `VertexInputType` | vertex factory | 메시 정점을 셰이더에 공급하는 방식의 등록 단위 |
| 정점 배치 `VertexLayout`, 정점 stream(공용어) | vertex declaration, stream component | 속성·stream·stride |
| 상태 프리셋 `StatePreset` | `TStaticBlendState` 등 | 이름 붙은 블렌드·래스터·깊이 조합 |
| 장치 능력 `DeviceCaps`(계획 9.6) | feature level, `GRHISupports*`·`GRHIGlobals`, DataDrivenShaderPlatformInfo | 장치 기능·한도 조회 |
| GPU 구간 `GpuScope` | `RDG_EVENT_SCOPE`·RHI breadcrumb | 이름·marker·timestamp를 함께 기록하는 구간 |
| 콘솔 설정 `ConsoleSetting`(Core) | `IConsoleManager`·console variable | 실행 중 바꿀 수 있는 이름 있는 설정 값 |
| 프레임 arena `FrameArena`(Core) | `FMemStackBase`, RDG allocator | 그래프·프레임 끝에 한 번에 비우는 할당기 |

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

## 4. 현재 코드베이스와 선행 작업

### 4.1 현재 코드베이스(2026-11-16)

| 영역 | 있는 것 | 렌더 기반에 부족한 것 → 선행 작업 |
|---|---|---|
| Graphics 계약 | `Device`(계약 주석 `Device.h:42–51`, 클래스 `:52–113`: 스왑체인·버퍼·텍스처·파이프라인·`BeginFrame`/`BeginOffscreen`/`EndFrame`·`ReadPixels`·`Destroy`·`WaitIdle`), `CommandContext`(`SetPipeline`·`SetVertexBuffer`·`SetTexture`·`SetConstants`·`Draw`만, `:11–24`). "생성한 스레드에서만 사용", 프레임은 한 번에 하나(`:43–46`). D3D12·OpenGL backend 모두 호출 스레드를 검사하지 않는다 | 4.4절 GX1–GX13 |
| 자원 종류 | 버퍼는 `BufferUsage::Vertex`만(`DeviceTypes.h:99–106`), 텍스처는 2D·`Sampled`/`RenderTarget`·RGBA8 2종, 샘플러는 텍스처 desc에 붙는다(`:108–128`, `GraphicsTypes/Public/GraphicsTypes/PixelFormat.h:12–16`, `Sampler.h:42–52`). 깊이·MSAA·배열·3D·큐브·UAV·index·structured·비교 샘플러 없음 | GX2(A0-G 뒤) |
| 파이프라인 | `PipelineDesc`가 backend별 원문을 받아 컴파일한다. 바인딩 고정(상수 b0, 텍스처 t0·s0), 블렌드는 `alphaBlend` bool, 깊이·컬링 없음, 정점 stream 하나(`DeviceTypes.h:138–157`) | 셰이더 시스템 SH2 + GX3 |
| 상태 전이 | D3D12 내부 고정 전이와 휴지 상태 관례만 있고 공개 API가 없다(`D3D12Graphics/Private/Win64/D3D12Device.cpp:155–162, 635–637, 803, 816, 1197`). OpenGL backend에는 memory barrier 호출이 없다 | GX5 |
| 제출·회수 | D3D12 DIRECT queue 하나(`D3D12Device.cpp:390`), 프레임 2개 동시 진행·fence로 slot 회수(`:17, 838–846`), `Destroy` 지연 해제(`:1396–1445`). OpenGL은 즉시 삭제. lease·`Retire`·공개 fence 없음 | GX7 |
| 업로드·readback | 생성 때 초기 데이터만, 텍스처 업로드는 동기 완료(`Device.h:50`), `ReadPixels`는 시험용 동기 대기(`:90–92`) | GX8 |
| 계측 | query·marker 없음 | GX9 |
| Renderer | 내장 셰이더 원문 문자열, 파이프라인 하나, 메시 둘. 엔진 모듈이 아니라 Sandbox 시험 샘플(`Programs/Sandbox/Private/SampleRenderer.*`, 2026-11-17 이동)이다 | 셰이더 시스템 SH2, RB 단계 |
| 엔진 루프 | `EngineSubsystem`(Initialize·OnFrameBegin·OnFrameEnd·Deinitialize)과 `EngineLoopHooks`(PumpEvents·BeginFrame·EndFrame)가 있다(`Engine/Source/Runtime/Engine/Public/Engine/Engine.h:27–69`). 렌더 추출과 제출은 `EndFrame` hook에서 게임 스레드가 동기로 한다. 렌더 스레드·in-flight snapshot은 후속(`Docs/Design/EngineRuntime.ko.md` 14절) | RB1, CP6 |
| 스레드·동기화 | join 소유 `platform::Thread`(`Core/Source/Runtime/PlatformBase/Public/Platform/Thread.h:23–44`), `Mutex`·`ConditionVariable`·`ManualResetEvent`·`Semaphore`(`Sync.h:19,62,79,98`), `Atomics.h`. 스레드 이름·우선순위·affinity·스택 크기는 후속(`Docs/Implementation/PlatformConcurrency.ko.md:42`) | CP1 |
| 작업 | `WorkerScheduler`(선행 작업·취소·Drain, 외부 이벤트 `WorkerTaskEvent`, `Core/Source/Runtime/Task/Public/Task/WorkerScheduler.h:93–149, 182–217`), `AsyncIoEvent`, `InlineExecutor`(동기 기준, `TaskScheduler.h:55–56`). 직렬 체인·ParallelFor·전용 스레드 lane 없음 | CP1·CP2 |
| 할당기 | `StackAllocator`(단일 스레드 LIFO, `Core/Memory/StackAllocator.h:42`), `ArenaAllocator`(반환 안 된 할당이 있으면 Reset이 Busy, 프레임 일괄 무효화 없음, `ArenaAllocator.h:23–25`) | CP3 |
| 설정 | 콘솔 변수·Config 계층 코드 없음(UnrealCoreFeatureAudit UE-C21·C29 **후속**). 계획 24.2절에 Config 우선순위만 있다 | CP4·CP8 |
| 계측 | TraceCapture(다중 스레드 trace, [trace 설계](TraceSystem.ko.md)) TR1·TR2 구현: 채널·`CpuScope`·카운터·표지. 옛 `ProfileScope`는 지웠다. GPU 상관은 RB7(UE-C23 **부분**) | CP5 |
| 리플렉션 | MetaSchema `TypeOf<T>()`(`Core/Source/Runtime/MetaSchema/Public/Meta/GeneratedType.h`), ObjectRuntime TypeRegistry. 객체 쪽 지연 회수 `BackendLease`(저장소 회수만 늦춘다. 주석에 "현재 실제 GPU/IO 작업과 연결되지는 않는다", `Core/Source/Runtime/ObjectRuntime/Public/Object/ObjectRefs.h:216–220`) | RB4(스키마), RB2(완료 통지) |
| 그 밖 | `Delegate`·`MulticastDelegate`(`Core/Delegate.h:88,567`), JSON(`Core/Text/Json.h`), SHA-256, `Core/Hash.h`, Archive·Record, `Name`(`Core/Text/Name.h:22`, 엔진 전역 pool 미정) | CP7 |

### 4.2 선행 작업 목록

ID는 이 문서가 붙인다. CP는 Core 선행, GX는 Graphics 확장, RB는 RenderBase 단계, RG는 RenderGraph 단계, SX는 셰이더 시스템 개정이다. 셰이더 시스템의 P·SH, 에셋 시스템의 A, 리플렉션의 B는 그 문서 ID 그대로 쓴다. 규모는 S(며칠)·M(1–2주)·L(그 이상)의 대략값이다.

| ID | 작업 | 상태 | 의존 | 막는 것 | 규모 | 상세 |
|---|---|---|---|---|---|---|
| CP1 | 전용 lane 실행기, 스레드 이름·우선순위·스택 크기 | 구현 | Thread·Sync(구현) | RB1 | M | 4.3절 |
| CP2 | 직렬 작업 체인·ParallelFor·작업 문맥 표지 | 구현 | WorkerScheduler(구현) | RG3, RB3 일괄 생성 선택 | M | 4.3절 |
| CP3 | 프레임 arena | 구현 | Allocator(구현) | RG1, RB4(GLES `PerDraw`) | M | 4.3절 |
| CP4 | 콘솔 설정(타입 있는 변수·명령·출처·렌더 snapshot) | 구현(ConfigSystem CF2, 프레임 packet 연결은 RB1). 상세 스펙은 [CVar](CVarSystem.ko.md)(CVR1–CVR8, 구현 전 — 12절) | CP8 | RB1 설정 snapshot, RG1 디버그 설정 | M | 12절 |
| CP5 | 다중 스레드 CPU 구간 계측·카운터·GPU 상관 | 구현(TR1–TR4, GPU 기록 형은 RB7에서 구현) | TraceCapture·TraceAnalysis | RB7 | M | 13절, [TraceSystem](TraceSystem.ko.md) |
| CP6 | 엔진 종료 단계 hook(기존 `EngineSubsystem`·`EngineLoopHooks` 확장) | 구현 | Engine(구현) | RB1 종료 순서 | S | 4.3절 |
| CP7 | 엔진 전역 `Name` pool 결정 | 결정(2026-11-18): 전역 pool 없음, 렌더 이름은 정적 문자열 + 64 bit 해시 | Name(구현) | RB3 | S | 4.3절 |
| CP8 | Config 계층(UE-C21) | 구현(ConfigSystem CF1) | — | CP4 | M | 12절 |
| GX1 | 장치 능력 `DeviceCaps` | 구현(1단계: 전역 한도·기능·MSAA 표본 수. 셰이더 형식 목록·형식 정의 상한 대조는 SH2) | A0-G G1, 셰이더 시스템 P6a, PixelFormat PF2 | RB3, RG1, RG4 | M | 4.4절 |
| GX2 | 자원 desc·view 확장, 샘플러 위치(R12)·비교 샘플러 | 구현(1단계: 버퍼 용도·버퍼 view·ReadBuffer. 텍스처 차원·MSAA 텍스처·텍스처 view 범위·샘플러는 다음 단계) | A0-G | RB2, RG1 | L | 4.4절 |
| GX3 | 파이프라인 완성(SH2 범위 밖 부분) | 미설계 | SH2·P2·P14 | RB4, RB5, RB6 | L | 4.4절 |
| GX3a | 정점 배치 완성: 정점 원소 형식 20종·다중 stream·인스턴스 stream·index 버퍼·instanced·indexed draw·셰이더 입력 대조, CPU 패킹 helper | 구현 | — | RB5, 메시 에셋 | M | 10.4절 |
| GX4 | 명령 기록 확장 | 미설계 | GX2·GX3 | RB5, RB6, RG1 | L | 4.4절 |
| GX5 | 명시 접근 상태와 전이 | 구현(최소: 전이 API·D3D12·기록 장치. 분리 전이는 RG2, pipe는 RG4) | GX2·GX4 | RG1 | M–L | 4.4절 |
| GX6 | 파라미터 바인딩 런타임(상수 블록·묶음 설정·공유 slot) | 미설계 | P2·SH2 | RB4 | L | 4.4절, 9절 |
| GX7 | 제출·완료·회수(계획 11.2), descriptor 회수, 스레드 안전 fence 대기 | 구현(lease는 RB2·RG1) | — | RB1·RB2 | L | 4.4절, 6절 |
| GX8 | 업로드 ring·비동기 readback | 구현 | GX7 | RB2, RB4, RG2 | M | 4.4절 |
| GX9 | timestamp query·debug marker·CPU·GPU 시간 대응 | 구현(`GpuScope`는 RB7에서 구현) | GX7 | RB7 | M | 4.4절, 13절 |
| GX10 | 장치 소유 스레드 이관 | 구현 | GX11 | RB1 | S | 5.4절 |
| GX11 | 장치 스레드 모델 | 구현 | GX7 | GX10, RB1–RB3 | M | 5.4절 |
| GX12 | transient heap·aliasing barrier | 후속 | GX5 | RG5 | L | 4.4절 |
| GX13 | subpass·framebuffer fetch(tiled GPU 병합용) | 후속 | GX4·GX5 | RG3 병합 | M | 4.4절 |
| RB1 | 렌더 lane·프레임 slot·전달 타입 등록·완료 통지·`RenderHandle` 예약 | 구현(설정 snapshot 포함, 12절) | CP1·CP6·GX7·GX10·GX11 | RB2–RB7 | M | 5절 |
| RG1 | 최소 렌더 그래프 | 구현(명시 접근 목록 경로. 파라미터 구조체 경로는 RB4·SH2 뒤, (g)는 D3D12·D3D11(각 WARP 포함)·OpenGL ES 두 context) | RB7·GX5·CP3(RB4·GX1·GX2·GX4는 범위를 줄여 앞당김) | RG2 | L | 7·8절 |

### 4.3 Core 선행 작업

**CP1 전용 lane 실행기.** 스레드 하나가 직렬 FIFO 큐를 소비하는 일반 실행기다. Audio·Physics lane(계획 8.1절)도 같은 형태가 필요하므로 Core Task에 둔다(공용 기능 중복 금지).
- 요건: 이동 전용 작업, 다중 생산자, 시작·종료 handshake를 이벤트로 처리, 종료 때 Drain 뒤 Join, 인라인 모드, "지금 lane 작업을 실행 중인가" 판별.
- 인라인 모드도 큐를 쓴다. `Post`는 큐에 넣고, 가장 바깥 `Post`가 큐가 빌 때까지 실행한다(trampoline). 그래서 lane 작업 안에서 다시 `Post`해도 실행 순서가 lane 모드(FIFO 맨 뒤)와 같다. "lane 작업 실행 중" 판별은 스레드 ID가 아니라 실행 중 표지로 한다. 그래야 인라인 모드에서도 lane 전용 API를 lane 작업 밖에서 부르는 잘못이 드러난다.
- 스레드 이름·우선순위·스택 크기를 함께 넣는다(`PlatformConcurrency.ko.md:42` 후속 해소). affinity는 후속이다.
- 계획 8.1절 `TaskDesc.lane`(`TaskLane`) 모델과의 관계: `DedicatedLane`은 Render·Audio처럼 전용 스레드가 필요한 lane의 실행기이고, Worker lane은 `WorkerScheduler`다. 15절에서 계획 8.1절에 적는다.
- UE 대응: `RC/Private/RenderingThread.cpp:321, 227–262`, `Core/Public/Async/TaskGraphInterfaces.h:63,353`.
- 완료 기준: 다중 생산자 FIFO 순서, 재진입 `Post` 순서가 두 모드에서 같음, 종료 Drain(남은 작업 0), 인라인·스레드 모드 결과 동일, lane 작업 밖 lane 전용 API 호출 거부. 순서는 sleep 없이 이벤트 gate로 만든다.
- **구현(2026-11-16).** `Task/DedicatedLane.h`(`DedicatedLane`·`DedicatedLaneConfig`·`DedicatedLaneMode::Thread/Inline`·`Post`·`Flush`·`Shutdown`·`IsExecutingTask`), 작업 lane 표지 `Task/TaskLane.h`(`TaskLane`·`CurrentTaskLane`·`TaskLaneScope`), `Platform/Thread.h`의 `ThreadOptions`·`ThreadPriority`·`CopyCurrentThreadName`. 계약은 [WorkerScheduler](../Implementation/WorkerScheduler.ko.md) "전용 lane 실행기"·[플랫폼 동시성](../Implementation/PlatformConcurrency.ko.md) "스레드 옵션"절. 5.3절의 `RenderLane::Drain`은 이 lane의 `Flush`(호출 전 작업 완료 대기) 위에 RenderBase가 만든다.

**CP2 직렬 작업 체인·ParallelFor.** 이전 작업을 선행으로 거는 직렬 체인(UE 그래프 설정 task `RC/Public/RenderGraphBuilder.h:256–284`), 범위 분할 병렬 실행, 작업 문맥 표지(어느 lane의 일인지)다. **구현(2026-11-16).** `Task/SerialTaskChain.h`(`SerialTaskChain`·`Submit`·`Last`·`Wait`), `Task/ParallelFor.h`(`ParallelFor`·`ParallelForOptions`), `WorkerScheduler::Submit`의 `WorkerSubmitOptions{orderDependencies, lane}`. 체인은 직전 작업을 **순서 전용 선행**(결과 무관)으로 건다. 일반 선행으로 걸면 앞 작업 하나의 실패가 체인 뒤 전체를 `DependencyFailed`로 만들기 때문이다(UE pipe도 앞 작업 결과와 무관하게 다음을 실행한다). 작업 문맥 표지는 CP1의 `TaskLane`을 worker 작업에도 붙이고, ParallelFor helper는 호출자의 표지를 물려받는다. 계약은 [WorkerScheduler](../Implementation/WorkerScheduler.ko.md) "직렬 작업 체인"·"범위 병렬 실행"절.

**CP3 프레임 arena.** 그래프·프레임 수명 객체(pass·자원·파라미터 구조체·GLES `PerDraw` 상수 사본)를 할당하고, 수명이 끝날 때 등록된 소멸자를 부른 뒤 한 번에 비운다. 여러 스레드가 블록 단위로 할당한다. 기존 `ArenaAllocator`는 반환 안 된 할당이 있으면 Reset을 거부하므로(`ArenaAllocator.h:23–24`) 별도 타입이 필요하다. UE 대응: `Core/Public/Misc/MemStack.h:77`, `RC/Public/RenderGraphAllocator.h:23,129`, `Core/Public/Experimental/ConcurrentLinearAllocator.h:769`. **구현(2026-11-16).** `Core/Memory/FrameArena.h`(`FrameArena`·`FrameArenaWriter`·`FrameArenaConfig`·`TryAllocate`·`TryNew`·`TryNewArray`·`Reset`·`Trim`). 개별 반환이 없으므로 `Allocator` 인터페이스를 구현하지 않고, 프레임 packet의 가변 길이 목록은 `TryNewArray`로 만든다. 계약은 [할당기](../Implementation/Allocators.ko.md) "FrameArena"절.

**CP6 엔진 종료 단계 hook.** 렌더 lane 종료(5.5절)는 계획 3.2절 Shutdown 순서의 "GPU fence 완료"부터 "owner/retire/queue closure 확인"까지에 걸친다. 새 이벤트 체계를 만들지 않고 기존 `EngineSubsystem`·`EngineLoopHooks`(`Engine.h:27–69`)에 그 단계를 드러내는 hook을 더한다. **구현(2026-11-17).** `EngineLoopHooks`에 `BeginShutdown`·`CompleteInFlightWork`·`DrainShutdownCleanup`·`VerifyShutdownClosure`·`StopExecutors`, `EngineSubsystem`에 `BeginShutdown`·`CompleteInFlightWork`를 더했다. `Engine::Shutdown` 순서는 차단 → client 종료·World 파괴 → 진행 중 작업 완료 → Subsystem 정리 → [최종 GC → 정리 drain]을 둘 다 0이 될 때까지(`MaxShutdownCollectPasses` = 8, 넘으면 `Busy`) → closure 확인 → 실행기 종료 → Registry·타입 정리다. 정리 drain·closure·실행기 hook은 실행 파일 hook에만 둔다. Subsystem은 정리(`Deinitialize`) 뒤에는 불리지 않기 때문이다. 실패한 단계도 뒤 단계를 건너뛰지 않고 첫 오류를 돌려준다. 계약은 [렌더 기반 구현](../Implementation/RenderBase.ko.md) 3절.

**CP7 Name pool.** 셰이더 타입·형식·pass 이름을 `Name`으로 다룰지, 정적 문자열과 SHA-256으로 다룰지 정한다. 셰이더 시스템은 키에 이름 정렬·SHA-256을 쓰므로(12.2절) 런타임 조회만 정하면 된다. **결정(2026-11-18, 권장안).** 엔진 전역 `Name` pool을 두지 않는다.
- 렌더 런타임의 셰이더 타입·셰이더 형식·pass·정점 입력 타입·파라미터 구조체 이름은 정적 등록 항목의 문자열 리터럴(프로그램 수명)과 그 이름의 64 bit 해시로 다룬다. 해시는 Core `HashStringBytesIgnoringAsciiCase`(`Core/Hash.h`, constexpr)이고, 런타임 조회 표의 키다.
- 등록 때 같은 해시에 다른 이름이 있으면 등록이 실패한다(충돌을 조용히 넘기지 않는다, 오류 문구는 영어). 같은 이름을 두 번 등록해도 실패다.
- 해시는 실행 중 조회용일 뿐이다. 저장·쿠킹 형식(셰이더 라이브러리 항목, job 키)은 셰이더 시스템 12.2절대로 이름 문자열 정렬과 SHA-256을 쓴다. 그래서 해시 함수를 바꿔도 쿠킹 결과는 바뀌지 않는다.
- 이유: `Name`은 pool 세션 domain과 lease를 가진 실행 중 토큰이다(`Core/Text/Name.h`, `NamePool.h`). 정적 등록과 여러 스레드 조회에 pool 수명 관리가 끼어든다. 렌더 이름은 코드가 정하는 닫힌 집합이라 pool이 필요 없다. UE도 셰이더 타입 표를 `FName`이 아니라 64 bit 해시 `FHashedName`으로 찾는다(`Runtime/RenderCore/Public/Shader.h:1277` `GetNameToTypeMap`, 해시는 대문자 이름의 CityHash64 `Runtime/Core/Private/Serialization/MemoryImage.cpp:1210–1252`).
- 머티리얼 파라미터 이름처럼 콘텐츠가 정하는 이름은 이 결정의 범위가 아니다([머티리얼 시스템](MaterialSystem.ko.md)이 정한다).

### 4.4 Graphics 확장(GX)

RenderCore가 쓰는 RHI 기능군과 Rocks Graphics의 차이다. "SH2" 열은 셰이더 시스템 SH2가 이미 계획한 것(○), 다른 선행이 일부 다루는 것(△), 셰이더 시스템에 없는 것(×)이다.

| 기능군 | UE 5.8 근거(`RHI/Public`) | RenderCore가 쓰는 곳 | Rocks | SH2 | GX |
|---|---|---|---|---|---|
| 장치 능력 | `RHIFeatureLevel.h:109`, `RHIShaderPlatform.h:82`, `DataDrivenShaderPlatformInfo.h:19`, `RHIGlobals.h:87`(RenderCore가 서로 다른 이름 26개를 읽는다) | `RC/Private/GlobalShader.cpp:484,526`, 그래프 barrier 판단 | 없음(`DeviceInfo`는 backend·이름·검증·소프트웨어만, `DeviceTypes.h:174–179`) | △ P6a·A0-G G1 | GX1 |
| 자원 desc | `RHIResources.h:1466`(버퍼), `:1965`(텍스처) | `RC/Private/GlobalRenderResources.cpp:59,86`, `RenderGraphResourcePool.cpp:190` | 일부 | △ A0-G(형식·차원) | GX2 |
| view(SRV·UAV) | `RHIResources.h:2713, 3359, 3369` | `GlobalRenderResources.cpp:99`, `DynamicBufferAllocator.cpp:188` | 없음(텍스처 하나 = SRV 하나 고정) | × | GX2 |
| 상태 객체 | `DynamicRHI.h:232–241` | `RC/Private/ClearQuad.cpp:44–68` | 일부(샘플러는 텍스처 소속, 블렌드 bool, 래스터·깊이 고정) | ○ 래스터·깊이, × 블렌드 MRT·스텐실 | GX2·GX3 |
| 그래픽·compute 파이프라인 | `RHIResources.h:4655, 1111`, `PipelineStateCache.h:173` | `ClearQuad.cpp:65,95` | 일부(원문 컴파일 + 생성, compute 없음) | ○ 코드로 생성·layout, × compute·캐시 | GX3, RB4 |
| 명령 목록 | `RHICommandList.h:2493, 3590, 4388, 5047` | `PixelShaderUtils.cpp:24`, `RenderGraphUtils.cpp:401,432` | 일부(삼각형 Draw만) | × | GX4 |
| 병렬 기록·제출 | `RHICommandList.h:4440–4458`, `RHIResources.h:5646` | `RenderGraphBuilder.cpp:2093, 2995` | 없음 | × | GX11, RG3 |
| 접근 상태·전이 | `RHIAccess.h:14–60`, `RHITransition.h:118, 355–404`, `RHIPipeline.h:12–20` | `RenderGraphPass.cpp:270–312` | 내부 고정만 | × | GX5 |
| render pass | `RHIResources.h:5346` | `RenderGraphBuilder.cpp:3450` | 일부(색 1개 + clear) | × | GX4 |
| 파라미터 묶음 설정 | `RHIShaderParameters.h:246–330`, `RHIContext.h:341, 749` | `ShaderParameterStruct.cpp:975–990` | 없음(`SetConstants(DrawConstants)` 80 byte) | △ P2·SH3 | GX6 |
| 상수 버퍼·layout·수명 | `RHIResources.h:1183`, `RHIDefinitions.h:517–525`, `DynamicRHI.h:1165` | `ShaderParameterMetadata.cpp:207, 920` | 없음(D3D12 루트 상수 20개만, `D3D12Device.cpp:1086–1121`) | △ P2·SH2 | GX6 |
| transient 할당 | `RHITransientResourceAllocator.h:535–555`, 기본 nullptr `DynamicRHI.h:312`(D3D12·Vulkan만 구현) | `RenderGraphBuilder.cpp:591` | 없음 | × | GX12 |
| 업로드·lock | `RHICommandList.h:753, 846` | `GlobalRenderResources.cpp:335, 835` | 일부(생성 때만) | × | GX8 |
| readback | `RHIGPUReadback.h:132,151` | `GPUMessaging.cpp:121`, `RenderGraphUtils.cpp:829` | 일부(시험용 동기) | × | GX8 |
| query | `DynamicRHI.h:1322`, `RHIResources.h:2454, 2490` | `RenderGraphEvent.cpp:166` | 없음 | × | GX9 |
| GPU fence 객체 | `RHIResources.h:2397` | `DumpGPU.cpp:1133` | 내부 전용 | × | GX7 |
| 자원 수명 | `RHIResources.h:53, 65–87`, 지연 삭제 `RHI/Private/RHIResources.cpp:46–58` | `RenderGraphBuilder.cpp:332` | 일부(slot+generation, D3D12 fence 뒤 회수) | × | GX7 |
| breadcrumb·GPU 이벤트 | `RHIBreadcrumbs.h:1300`, `GPUProfiler.h:92–236` | `RenderGraphBuilder.cpp:1769–1778` | 없음 | × | GX9 |
| multi-GPU mask | `MultiGPU.h:32` | `GPUMask` 43줄(8개 파일) | 해당 없음 | — | 넣지 않는다(Windows 데스크톱에서만 켜지고 꺼지면 mask가 사소해진다, `RHI.Build.cs:23`, `MultiGPU.h:19,233`) |

각 GX의 내용:

- **GX1 장치 능력.** 셰이더 형식 목록(셰이더 시스템 12.3절 런타임 형식 선택), 형식별 지원 표(A0-G G1), 전역 한도(dispatch 그룹 수·view 크기·MRT 수·상수 버퍼 크기·정점 속성 수), 동작 표시(자원 생성 스레드 안전 여부(GX11), native split barrier·async compute·transient aliasing·timestamp·tiled GPU 여부). 형식 정의(셰이더 시스템 12.1절)는 "쿠킹 때 가정하는 상한"이고 `DeviceCaps`는 "실행 장치의 실제 값"이다. 시작 때 장치 값이 형식 정의 상한 이상인지 검사하고, 아니면 그 형식을 고르지 않는다. 픽셀 형식별 능력(`PixelFormatCaps`·MSAA 표본 수)과 backend 형식 대응 표는 [픽셀 형식](PixelFormat.ko.md) 7·8절(PF3)이 정한다.
  - **구현(2026-11-18, 1단계).** `Graphics/DeviceCaps.h`의 `DeviceCaps`(`DeviceLimits`·`DeviceFeatures`)를 `Device::Caps()`(스레드 안전, 장치 수명 동안 불변)로, 형식별 MSAA 표본 수를 `Device::GetPixelFormatSampleCounts`(비트 n = 2^n, `SampleCountBit`)로 공개한다. 2표본 이상이면 그 형식의 `PixelFormatCaps::Multisample`이 켜진다(PixelFormat.ko.md 14.3절). `DeviceInfo::apiVersion`(D3D feature level, Vulkan apiVersion, GL major × 100 + minor × 10)을 더했다.
    - 한도: 단계(VS·PS·CS)별 상수 버퍼·SRV 텍스처(typed buffer SRV 포함)·샘플러·구조화·raw 버퍼·storage image·GLES uniform vec4, 파이프라인 합(Vulkan `maxDescriptorSet*`, D3D binding tier heap, GLES `GL_MAX_COMBINED_*`), `perStageResources`(Vulkan `maxPerStageResources`), `pipelineUavs`(형식 정의 `uavTotal`), `boundDescriptorSets`, `colorAttachments`, 상수 버퍼·루트 상수 크기, 상수·storage·texel view 시작 정렬, storage view·texel 원소·버퍼 최대 크기, 정점 속성·stream, 단계 사이 vec4, compute 그룹 수·크기·invocation·shared memory, 텍스처 크기(2D·3D·cube·층), 이방성. backend에 해당 없는 값은 `NotApplicableLimit`(0)다.
    - 기능: compute, 깊이 clamp, VS layer 출력, 64 bit atomic, 간접 인자 첫 인스턴스, multi draw indirect, cube array, sRGB 짝 view, VS·PS storage 쓰기, HDR 출력, D3D12 binding tier, D3D 셰이더 모델. Vulkan은 지원하면 그 기능·확장을 켜고 켠 것만 참이다(`depthClamp`·`multiDrawIndirect`·`drawIndirectFirstInstance`·`imageCubeArray`·`vertexPipelineStoresAndAtomics`·`fragmentStoresAndAtomics`, `VK_EXT_shader_viewport_index_layer`, `VK_KHR_shader_atomic_int64` + `VK_EXT_shader_image_atomic_int64`, UE `VulkanExtensions.cpp:536, 560`과 같은 조건).
    - 출처: D3D12는 d3d12.h 상수와 resource binding tier·`CheckFeatureSupport`(FEATURE_LEVELS·SHADER_MODEL·OPTIONS·OPTIONS9, UE `D3D12Adapter.cpp:1392`), D3D11은 d3d11.h 상수와 feature level(FL 11.0은 UAV 8·VS UAV 없음)·OPTIONS3, Vulkan은 `VkPhysicalDeviceLimits`와 maintenance3 `maxMemoryAllocationSize`, GLES는 `glGetIntegerv`·`glGetIntegeri_v`·`glGetInteger64v`와 ES 확장 이름(`GL_EXT_depth_clamp`, UE `OpenGLES.cpp:185`). HDR 출력은 DXGI 출력 색 공간(UE `WindowsD3D12Device.cpp:573–602`)이고 Vulkan·GLES는 surface 질의 전에는 몰라 거짓이다. 조회 수단이 없는 GLES 버퍼 최대 크기는 보수적 128 MiB, D3D 버퍼 최대 크기는 보장식 min(max(128, 전용 VRAM × 0.25), 2048) MiB다.
    - 측정(Sandbox `--self-test`, 2026-11-18): Renderer RQ11·RQ22 판단 근거.

      | 장치 | MRT | 단계당 SRV 텍스처 | 단계당 샘플러 | set sampled image | set sampler | perStageResources | VS·PS storage block | MSAA(RGBA8Unorm·D32Float) |
      |---|---|---|---|---|---|---|---|---|
      | D3D12 GTX 1060(tier 3, SM 6.7) | 8 | 1000000 | 2048 | — | — | — | 1000000 | 1/2/4/8 |
      | D3D12 WARP(tier 3) | 8 | 1000000 | 2048 | — | — | — | 1000000 | 1/2/4/8/16 |
      | D3D11 GTX 1060·WARP(FL 11.1) | 8 | 128 | 16 | — | — | — | 128 | 1/2/4/8(WARP 16까지) |
      | Vulkan GTX 1060 | 8 | 1048576 | 1048576 | 1048576 | 1048576 | 4294967295 | 1048576 | 1/2/4/8 |
      | Vulkan lavapipe(llvmpipe) | 8 | 1015808 | 1015808 | 1015808 | 1015808 | 1015808 | 1015808 | 1/4/8 |
      | GLES ES 3.2 context(GTX 1060) | 8 | 32 | 32 | — | — | — | 16·16 | 1/2/4/8 |
      | GLES GL 4.3 core context | 8 | 32 | 32 | — | — | — | 16·16 | 1/2/4/8/16/32 |

      Vulkan 상수 버퍼는 GTX 1060 단계당 15·lavapipe 1015808, push constant 256 byte, storage·texel view 시작 정렬 16(GTX 1060 상수 256·lavapipe 16)이다. typed UAV load(`R32Float`·`RG16Float`)는 D3D·Vulkan 모두 있고 GLES는 `RG16Float`이 Table 8.26 image 형식이 아니라 없다. 64 bit atomic은 D3D12·Vulkan(두 장치) 참, D3D11·GLES 거짓이다.
- **GX2 자원 desc·view.** 버퍼 용도(정점·index·상수·structured·raw·간접 인자·UAV 조합), 텍스처 차원(A0-G)·깊이/스텐실 형식·MSAA·용도 플래그(샘플·렌더 타깃·깊이·UAV)·mip·array·clear 값, SRV/UAV view 객체(mip·slice 범위). 샘플러는 결정 R12(9.6절): 텍스처 desc의 샘플러는 기본값이고, 묶을 때 파라미터 멤버가 다른 샘플러를 지정할 수 있다. 비교 샘플러(깊이 비교 함수)를 `SamplerDesc`에 더한다(SX13, 그림자는 계획 11.1절 필수). 깊이/스텐실 형식·스텐실 view plane·sRGB 짝 view 규칙은 [픽셀 형식](PixelFormat.ko.md) 9·10절이다.
  - **구현(2026-11-18, 1단계: 버퍼).** `BufferUsage`를 비트 묶음으로 바꿨다(`Vertex`·`Index`·`Constant`·`Indirect`·`ShaderRead`·`ShaderWrite`·`Structured`·`Raw`, UE `EBufferUsageFlags` 대응)고 `BufferDesc`에 `stride`(구조화 원소)를 더했다. 장치 무관 규칙 `ValidateBufferDesc`(상수는 홀로·16 byte 배수, 구조화는 stride·셰이더 용도 필요·정점·index·간접과 묶지 않음(D3D11 제약), raw는 셰이더 용도·4 byte 배수)와 장치 한도(`bufferBytes` 초과 `Unsupported`). 버퍼 view는 `BufferViewHandle`·`BufferViewDesc{kind(Typed·Structured·Raw), access(Read·ReadWrite), format, offset, elementCount}`와 `Device::CreateBufferView`·`Retire(BufferViewHandle)`이다. 규칙은 장치 무관 `ResolveBufferView`(UE `RHIResources.cpp:176–260`과 같은 규칙: typed는 구조화 버퍼 아님, raw 시작 16 byte, offset만 주고 개수 0 금지)와 장치 `CheckBufferViewLimits`(장치 시작 정렬 위반 `InvalidArgument`, `TexelBufferRead`·`TexelBufferWrite` 능력·원소 수·storage 범위 밖 `Unsupported`)다. view는 버퍼 자원을 붙들어 버퍼를 먼저 `Retire`해도 산다(D3D는 COM 참조, Vulkan·GLES는 남은 view 수를 세어 마지막 view가 놓일 때 버퍼를 회수한다). `Device::ReadBuffer`(동기, 시험·캡처용)를 더했다.
    - 그리기 묶기(개발 경로 고정 배치, GX6 전): `CommandContext::SetBufferView(slot, view)`, slot 0–3(`MaxDrawBufferViews`) = HLSL `t1–t4`(VS·PS 모두), GLES typed는 텍스처 유닛 PS 1–4·VS 17–20(`samplerBuffer`), 구조화·raw는 storage block binding 0–3(raw block 이름 `type_ByteAddressBuffer…`). 파이프라인 생성이 셰이더 리플렉션(D3D `D3DReflect`, Vulkan 개발 훅의 SPIR-V 리플렉션에 원문 register·자원 모양·원소 타입을 더함, GLES program introspection)으로 slot 배치(`DrawBufferSlots`)를 얻고, 배치 밖 자원(다른 register·space, 그래픽 단계 UAV)은 `InvalidArgument`다. 그리기는 읽는 slot마다 같은 종류(typed는 같은 값 계열)의 Read view가 묶였을 때만 실행하고 아니면 무시한다(`IgnoredDraws`, `DrawBufferSlotsSatisfied`). ReadWrite view는 그래픽 단계에 묶지 않는다(UAV는 compute만, ShaderSystem 7.2절 4항).
    - backend: D3D12는 slot마다 root descriptor table 하나(SRV 칸을 그대로 가리킨다, 빈 slot은 null SRV), ShaderWrite 버퍼에 `ALLOW_UNORDERED_ACCESS`, 버퍼 상태는 암묵 승격(`ReadBuffer`는 쌓인 업로드를 먼저 제출해 복사 대상 → 원본 재승격을 피한다). D3D11은 bind·misc flag(UE `D3D11Buffer.cpp:38–83`), raw SRV는 `BUFFEREX`, 상수 버퍼는 전체 갱신만(부분 갱신 `Unsupported`, debug layer 288). Vulkan은 파이프라인마다 리플렉션으로 set layout·파이프라인 layout을 만들고(push constant 범위는 공용 layout과 같다), 그리기 직전 descriptor set을 명령 버퍼 기록의 pool에서 받아 쓴다(기록을 다시 쓸 때 pool reset, 텍스처마다의 고정 set을 없앴다). typed view 형식은 버퍼의 모든 texel usage를 지원해야 한다(VUID-VkBufferViewCreateInfo-format-08778, 그래서 쓰기 능력은 읽기도 되는 형식만 공개). 업로드 끝 barrier에 간접 인자·상수·compute 단계를 더했다. GLES는 버퍼 텍스처(`glTexBufferRange`)와 `glBindBufferRange`(GL_SHADER_STORAGE_BUFFER)이고 texel buffer 능력은 ES 3.2 버퍼 텍스처 형식 표(RQ9, PixelFormat.ko.md 14.3절)다.
    - 시험: EngineTests `BufferDesc_*`·`BufferView_*`·`DrawBufferSlots_*`(규칙·장치 한도), Sandbox `--self-test`의 RunCapsChecks(한도가 API 최소 보장 이상, 형식마다 Multisample ⇔ 2표본 이상, 표본 수 ⇔ 렌더 타깃·깊이)와 RunBufferChecks(용도 8종 생성·전체·부분 ReadBuffer·`UpdateBuffer`·staging 복사 바이트 대조, 형식마다 typed SRV·UAV 생성 ⇔ `TexelBufferRead`·`TexelBufferWrite`, view·생성·ReadBuffer 음성 대조, VS·PS가 typed float·typed uint·구조화·raw view를 읽어 그린 픽셀 대조(오프셋 view 포함), 버퍼를 view보다 먼저 놓은 그리기, slot 불일치 그리기 4종 무시, 배치 밖 register·PS UAV 거부, 검증 계층 오류 0). UAV 쓰기·간접 인자 소비는 compute·간접 명령이 있는 2단계(GX3·GX4)에서 시험한다.
    - 남은 것(2단계 이후): compute 파이프라인·dispatch와 UAV 묶기(GX3·GX4), 상수 버퍼 묶기(GX6), 간접 draw·dispatch(GX4), 텍스처 쪽 GX2(차원·MSAA 텍스처 생성·텍스처 view 범위·비교 샘플러), D3D12Device.cpp(약 4000줄) 기능별 분할.
    - GX1·GX2 리뷰 반영(2026-11-18): Vulkan 업로드 묶음은 앞 복사가 쓴 구간을 원본으로 읽을 때도 barrier를 둔다(초기 데이터 직후 `ReadBuffer`의 RAW, synchronization validation으로 확인). GLES는 성분 대응 형식(A8)을 texel buffer 능력에서 빼고(26·10), 그래픽 단계 image uniform·유닛 0 밖 텍스처 sampler를 거부한다(storage block의 readonly 여부는 GL introspection이 알려 주지 않아 계약 주석에 적었다). 상수 버퍼 한도는 루트 상수를 빼고(D3D11 13, D3D12 Tier 1·2는 table CBV 14 그대로), `DeviceFeatures`에 RT tier(`rayTracingTier`·`rayTracingPipeline`·`rayQuery`)를 더했다. VS layer 출력 기능을 layer·viewport 둘을 뜻하는 `vertexViewportLayerOutput`으로 바꾸고(ShaderSystem 7.2절 7항, D3D12는 UE와 같이 늘 참(OPTIONS 값은 GS 에뮬레이션 여부일 뿐), D3D11 OPTIONS3, Vulkan 확장 + `multiViewport`, GLES 거짓) viewport 수 한도 `DeviceLimits::viewports`(D3D 16, Vulkan `maxViewports`·`multiViewport` 없으면 1, GLES 1)를 더했다, D3D12 FL 11.0 VS storage image 0, 12_2를 모르는 런타임의 feature level 재질의, Vulkan `atomic64`는 `shaderInt64`도 요구, `DeviceInfo::apiVersion`은 instance 요청 판과의 최소, 프레임 descriptor pool은 두 배씩 늘리고 바닥나면 경고 로그. 구조화 view stride를 셰이더 선언 stride(D3D `NumSamples`, SPIR-V ArrayStride, GL `TOP_LEVEL_ARRAY_STRIDE`)와 대조하고(`DrawBufferSlots::structureStrides`, 공용 `AddDrawBufferSlot`), D3D11·D3D12 공용 DXGI 도우미는 `DxgiGraphics` 모듈로 옮겼다. Sandbox caps 시험은 D3D11 FL 11 사양값과 D3D12 binding tier 표 값을 단언한다.
- **GX3 파이프라인 완성.** SH2가 하는 일(컴파일된 코드로 셰이더 생성·파이프라인 layout·root signature·래스터·깊이)에 더해 MRT 블렌드(타깃별)·스텐실, compute 파이프라인, 다중 stream 정점 배치(10.2절), 속성 없는 파이프라인(11.2절), 기본도형 종류(삼각형 목록·띠, 선, 점)를 넣는다. SH2 범위를 넓힐지는 결정 R8이다.
  - **구현(2026-11-18, 2단계).** 고정 기능 상태는 `Graphics/PipelineState.h`다: `PrimitiveTopology`(삼각형 목록·띠, 선 목록·띠, 점), `CompareFunction`(Never–Always, 깊이 기본 `GreaterEqual` = reverse-Z), `StencilOperation`·`StencilFace`·`DepthStencilState`(깊이 시험·쓰기·비교, 스텐실 시험·읽기·쓰기 mask·앞·뒷면 연산), `BlendFactor`·`BlendOperation`·`ColorWriteMask`·`ColorTargetBlend`(`AlphaBlend()`), `RasterState`(컬, 깊이 bias 상수·기울기·상한, 깊이 clamp). `PipelineDesc`는 `topology`·`colorTargetCount`(0–8)·`colorFormats[8]`·`blend[8]`(타깃별)·`alphaToCoverage`·`depthFormat`·`depthStencil`·`raster`를 받고 정점 입력이 없는 배치(속성 없는 파이프라인)를 그대로 받는다. 장치 무관 규칙 `ValidatePipelineDesc`(위반 `InvalidArgument`: 타깃 수·무효 형식·색도 깊이도 없음·깊이 없이 깊이·스텐실 시험·스텐실 plane 없는 형식의 스텐실·모르는 enum)와 능력 규칙 `CheckPipelineFeatures`(`Unsupported`: `colorAttachments` 초과, 능력 없이 `depthClamp`·`depthBiasClamp`·`alphaToCoverage`·타깃별 다른 블렌드)를 backend가 부르고, 형식이 렌더 타깃·깊이·블렌드가 되는지는 형식 능력으로 `Unsupported`다. `DeviceFeatures`에 `depthBiasClamp`(D3D 참, Vulkan 기능, GLES `GL_EXT_polygon_offset_clamp`)·`alphaToCoverage`(모두 참)·`independentBlend`(D3D·GLES 참, Vulkan 기능)를 더했다.
    - compute: `Device::CreateComputePipeline(ComputePipelineDesc{shader})`, 진입점 `CSMain`(GLSL ES는 `main`). 배치(GX6 전 고정): 상수 b0(GLSL `cu_h[5]`), 읽기 view slot 0–3 = t1–t4(그래픽과 같은 `SetBufferView`), 쓰기 view slot 0–3 = u0–u3(`SetWritableBufferView`, `RWBuffer`·`RWStructuredBuffer`·`RWByteAddressBuffer`; GLSL ES typed는 image unit slot, 구조화·raw는 storage binding 4 + slot). 리플렉션이 쓰기 slot 배치(`DrawBufferSlots` 재사용)를 만들고 배치 밖은 `InvalidArgument`다. Vulkan 개발 훅은 compute 단계(set 0)와 쓰기 storage·storage texel buffer를 받는다. 모든 파이프라인 layout의 push constant 범위는 VS·PS·CS 공용이다.
    - 깊이·스텐실 쓰기 여부 `PipelineWritesDepthStencil`(읽기 전용 깊이 attachment에서 그런 그리기는 무시).
- **GX4 명령 기록 확장.** index·instanced·간접 draw, 버퍼 없는 draw, dispatch(간접 포함), 버퍼·텍스처 복사, UAV clear, viewport·scissor, attachment 시작·끝(색 MRT·깊이·load/store·clear 값, mip·slice), 그리기별 상수의 임의 크기 바이트(`SetConstants` 고정 타입 일반화, SX2), 여러 기록 객체와 제출 순서. 지금의 `BeginFrame`·`BeginOffscreen`(색 1개 + clear 고정)을 attachment 시작으로 바꾼다.
  - **구현(2026-11-18, 2단계).** `Device::BeginPass(PassDesc)`: 색 attachment 0–8(`ColorAttachment{texture, load, store, clear}`), 깊이 attachment(`DepthAttachment{texture, depthLoad·Store, clearDepth(기본 0 = reverse-Z 먼 평면), stencilLoad·Store, clearStencil, readOnly}`), 모두 같은 크기·중복 없음(위반 `InvalidArgument`, `colorAttachments` 초과 `Unsupported`). attachment가 없는 패스는 compute·복사 전용이고 그 안의 그리기는 무시된다. `BeginOffscreen`은 `BeginPass`의 비가상 도우미가 됐다. 시작 때 attachment를 렌더 타깃·깊이 쓰기(읽기 전용이면 깊이 읽기) 상태로 바꾸고(GX5) load 동작을 한 뒤 viewport·scissor를 대상 전체, 스텐실 기준을 0으로 둔다. 정수 형식 타깃의 clear 값은 실수를 정수로 바꾼 값이다(D3D와 같다). mip·slice 지정은 텍스처 쪽 GX2와 함께다.
    - `CommandContext`: `Dispatch(x, y, z)`(그룹 수는 `computeGroupCount` 안), `DispatchIndirect`·`DrawIndirect`·`DrawIndexedIndirect`(Indirect 용도 버퍼, offset 4 byte 배수, 인자 구조체 `DispatchIndirectArguments`·`DrawIndirectArguments`·`DrawIndexedIndirectArguments`), `SetWritableBufferView`, `SetViewport`·`SetScissor`(원점 왼쪽 위, 픽셀), `SetStencilReference`, `CopyBuffer`(4 byte 배수, 같은 버퍼끼리 `InvalidArgument`), `CopyTexture`(mip 0 전체, 같은 크기·형식, view·패스 attachment 아님, 단일 plane), `ClearWritableBufferView`(raw·R32Uint·R32Sint typed의 32-bit word, 구조화 view는 D3D12가 받지 않아 모든 backend `Unsupported`). 조건이 맞지 않는 그리기·dispatch(파이프라인 종류, attachment, view 배치, 읽기 전용 깊이에 쓰는 파이프라인, 인자 범위)는 무시하고 `IgnoredDraws`를 올린다. 버퍼 없는 그리기는 1단계 그대로다. 그리기별 상수 일반화(SX2)와 여러 기록 객체는 이번 범위 밖이다.
    - GX5 전이: D3D12는 기록마다 버퍼 상태를 추적한다(첫 사용은 COMMON 암묵 승격, 읽기끼리는 합침, 그 밖은 transition, UAV → UAV는 UAV barrier). 그리기 직전 정점·index·SRV view, dispatch 직전 읽기·쓰기 view, 간접 인자, 복사 원본·대상을 그 상태로 맞추므로 compute가 쓴 버퍼를 같은 기록에서 SRV·간접 인자로 읽는다. Vulkan은 버퍼를 쓰는 명령(dispatch·복사·fill) 앞뒤에 전역 메모리 barrier를 두고(렌더 그래프 밖 기록의 보수적 순서, render pass를 닫았다가 LOAD로 다시 연다), GLES는 dispatch 뒤 `glMemoryBarrier(GL_ALL_BARRIER_BITS)`, D3D11은 드라이버가 맡는다(CS UAV를 dispatch 뒤 풀고 그래픽 SRV는 그리기 직전에 묶는다). 텍스처 복사는 mip 0을 복사 원본·대상 상태로 바꾸고 그대로 둔다.
    - backend 요점: D3D12는 compute root signature(b0 상수 + t1–t4·u0–u3 table), `ExecuteIndirect` command signature 셋, 읽기 전용 DSV(깊이·스텐실 읽기 전용 flag), UAV clear용 CPU 전용 UAV descriptor pool. D3D11은 파이프라인마다 blend·래스터(scissor 켬)·깊이 상태 객체, 읽기 전용 DSV를 깊이 텍스처마다. Vulkan은 render pass 열쇠(형식·load·store·읽기 전용 깊이)와 attachment view 묶음의 framebuffer 캐시(attachment 텍스처를 놓을 때 회수), 깊이·스텐실 attachment용 view(두 aspect), 동적 스텐실 기준. GLES는 MRT·깊이 패스에 장치 framebuffer 하나를 다시 붙여 쓰고(EndFrame이 뗀다) `glClearBuffer*`·`glBlendFuncSeparatei`·`glColorMaski`·`glStencil*Separate`·`glPolygonOffset(Clamp)`로 상태를 걸며, index 간접 그리기는 GL 인자에 index 버퍼 offset을 더할 곳이 없어 index offset 0만 받는다(그 밖은 무시).
    - 시험: Sandbox `--self-test`의 RunCommandChecks(D3D12·D3D12 WARP·D3D11·D3D11 WARP·Vulkan GTX 1060·lavapipe·GLES ES 3.2·GL 4.3 core 모두): compute가 구조화·typed UAV에 쓰고 같은 패스의 VS·PS가 SRV로 읽은 픽셀과 UAV 바이트, compute가 쓴 인자로 간접 그리기·index 간접 그리기·간접 dispatch(그룹 수 표시), MRT 8(RGBA8 6·R32Uint·RGBA16Float)의 타깃별 clear·더하기 블렌드·쓰기 mask 값, 깊이 GreaterEqual·Equal·쓰기, 스텐실 Replace·Equal과 기준 값, scissor, 알파 블렌드 조합 픽셀, 읽기 전용 깊이 attachment(시험 통과·쓰는 그리기 무시), 띠·선·점과 viewport, 버퍼 복사·raw·typed UAV clear·텍스처 복사·render target history 복사 바이트, attachment 없는 패스, 규칙·능력 위반의 명시 실패와 `IgnoredDraws`, 검증 계층 오류 0. EngineTests는 RecordingDevice가 새 계약을 따른다.
    - 측정 메모: lavapipe(mesa 26.2.7)는 깊이 없는 단일 RGBA8 대상에 화면 정렬 사각형만 그리면 llvmpipe linear 경로를 타고, 이 경로가 셰이더를 AoS u8로 컴파일하면서 `load_ubo`의 정수 offset까지 0~1 상수로 변환해 0으로 만든다. 그래서 PS가 push constant 첫 vec4를 읽는다(mesa `lp_bld_nir_aos.c` `emit_load_const`, `lp_state_fs_analysis.c` 상수 검사). `LP_PERF=no_rast_linear`로 사라지고 GTX 1060·D3D12·GLES는 맞아 mesa 버그로 확정했다(2026-11-18, upstream main 미수정). 시험은 정점 5개 띠(마지막 삼각형이 사각형 판정을 깬다)로 그린다. 깊이 없는 단일 RGBA8·BGRA8 대상, 화면 정렬 사각형, 상수가 모두 [0,1]인 PS가 겹치는 다른 시험도 같은 오류가 날 수 있다.
- **GX5 명시 접근 상태와 전이.** 추상 접근 비트(읽기: 정점·index·상수·SRV(그래픽·compute)·간접 인자·복사 원본·present / 쓰기: RTV·DSV·UAV·복사 대상 / 깊이 읽기 전용), pipe(그래픽·async compute), subresource 범위, 처음 상태 unknown·discard. 지금 D3D12의 휴지 상태 관례를 걷어 내고 전이를 호출자(렌더 그래프)가 정한다. 전이 API는 begin·end를 나눠 받되, native split을 지원하지 않는 backend는 end 지점에서 한 번에 낮춘다(UE D3D12도 native split barrier를 쓰지 않는다: `D3D12RHI/Private/D3D12LegacyBarriers.cpp:1111–1127, 1543–1660`, 표시 기본값 false `RHIGlobals.h:799`). backend lowering:
  - D3D12: resource barrier와 enhanced barrier 가운데 무엇을 쓸지는 backend 내부 결정이다(UE도 RHI 안에서 고른다, `D3D12RHI/Private/Windows/WindowsD3D12BarriersFactory.h:13–21`). enhanced barrier는 Agility SDK를 Runtime 의존으로 만들므로 셰이더 시스템 결정 3과 함께 정한다.
  - Vulkan: pipeline barrier.
  - OpenGL ES: 전이는 대부분 생략하고, UAV(image·storage) 쓰기 뒤 읽기에는 접근 비트에서 고른 `glMemoryBarrier` bit를 둔다(UE는 dispatch 앞뒤에 전체 bit, `OpenGLDrv/Private/OpenGLCommands.cpp:2671–2673`).
  - D3D11·Metal: SH5에서 정한다(미확인).
  - **구현(2026-11-17, RG1 최소 범위).** `Graphics/ResourceAccess.h`(`ResourceAccess`·mask·`CanCombineAccess`·`SubresourceRange`·`ResourceTransition`)와 `CommandContext::Transition(Span<const ResourceTransition>)`. 장치는 텍스처 mip마다 상태를 추적하고 `BeginFrame`·`BeginOffscreen`이 대상을 렌더 타깃으로 바꾼다(attachment 전이, GX4 attachment 시작 전까지). before `Unknown`은 장치 추적 상태에서 바꾼다는 뜻이다. D3D12: 휴지 상태(PSR) 관례를 없애고 mip별 `committed`(제출 끝)·`current`(기록 중) 추적, legacy resource barrier, UAV → UAV는 UAV barrier, 업로드 기록은 `committed` 기준 상태 중립, 검증 장치는 before 불일치를 `ValidationErrorCount`에 센다. 버퍼는 암묵 승격·복귀에 맡겨 UAV barrier만 낸다. 그래프 밖 그리기(`SetTexture`)는 암묵 전이로 메운다. OpenGL·D3D11은 빈 override(드라이버 암묵 전이), Vulkan은 같은 추적에서 pipeline barrier로 낮추고 버퍼는 쓰기가 끼는 전이만 메모리 barrier로 낸다(render pass 안 barrier는 pass를 닫고 다시 연다). 분리 전이(begin·end)와 pipe는 넣지 않았다(RG2·RG4). 계약은 [렌더 기반 구현](../Implementation/RenderBase.ko.md) 8절.
- **GX6 파라미터 바인딩 런타임.** 9.3–9.5절을 Graphics 쪽에서 받는 부분이다: 상수 블록 생성(수명 3종)·갱신, 파라미터 묶음 설정, 공유 상수 slot 표.
- **GX7 제출·완료·회수.** 계획 11.2절 계약(`Submit`·`Retire`·`GpuFencePoint`·`RenderResourceLease`·device epoch)을 Graphics `Device`에 들인다.
  - OpenGL ES도 fence sync로 같은 계약을 따르고 "즉시 삭제" 예외를 없앤다.
  - descriptor(D3D12 heap slot, Vulkan descriptor set)도 마지막 사용 fence 뒤에 회수한다(계획 11.2절).
  - 다른 스레드가 fence 완료를 기다리는 스레드 안전 API와 완료 이벤트를 둔다. Core `WorkerTaskEvent`에 잇는다(`AsyncIoEvent`와 같은 방식). 장치 손실은 기다리는 쪽을 오류로 깨운다.
  - 장치가 내부에서 프레임 2개를 돌리는 지금 방식은 프레임 slot(5.3절)이 넘겨받는다.
  - **구현(2026-11-17).** `Device::EndFrame`이 `GpuFencePoint`(device epoch·queue·value)를 돌려주고, `Destroy`를 `Retire`로 바꿔 자원·descriptor를 마지막 사용 fence 뒤에 해제한다(last-use는 장치 내부 집계: 기록 중 프레임이면 그 fence, 아니면 그때까지의 마지막 제출). 스레드 안전 `LastSubmittedFence`·`IsFenceComplete`·`WaitFence`·`NotifyFence`·`IsLost`는 공용 `FenceTimeline`(backend마다 관찰 스레드 하나)으로 구현했고, `Graphics/GpuFenceEvent.h`가 `WorkerTaskEvent`에 잇는다. OpenGL은 `glFenceSync`와 공유 context 관찰 스레드로 같은 계약을 따른다. 장치 손실은 기다리는 쪽을 `Destroyed`로 깨운다(Core에 장치 손실 전용 오류 코드가 없다). D3D12의 고정 프레임 2개는 기록 저장소 pool(상한 8)로 바꿨다. 남은 것: `RenderResourceLease`(병렬 기록·그래프 import가 생기는 RB2·RG1), 호출자 last-use 하한 인자(단일 queue에서는 내부 집계보다 늦을 수 없어 받지 않는다).
- **GX8 업로드·readback.** 프레임 slot별 업로드 ring(map 쓰기 → 복사), staging과 fence 완료 이벤트에 의한 비동기 readback. `ReadPixels`(시험용 동기)는 남겨 두되 프레임 경로에서 쓰지 않는다.
  - **구현(2026-11-17).** 업로드 ring은 프레임 slot별 고정 구간이 아니라 fence로 닫는 묶음의 원형 할당(`Graphics/UploadRing.h`, 기본 4 MiB)이다. 묶음은 GPU 완료 뒤에만 회수하고, 다 회수돼도 위치를 0으로 되돌리지 않아 결과가 GPU 완료 시점에 좌우되지 않는다. 자리가 없으면 소유 스레드는 열린 기록을 먼저 제출하고 가장 오래된 묶음을 기다리며, 그래도 안 되면 전용 staging으로 올린다(`UploadStats::dedicatedUploads`). 생성 초기 데이터와 `UpdateBuffer`·`UpdateTexture` 갱신은 별도 업로드 기록에 쌓이고 소유 스레드의 다음 제출(`EndFrame`·`SubmitUploads`·`WaitIdle`) 앞에 실행된다. 스트리밍 staging pool(`Graphics/StagingPool.h`, 기본 예산 256 MiB)은 CPU 가시 메모리를 IO 목적지로 주고(`AcquireStaging`), `UploadTextureFromStaging`·`UploadBufferFromStaging` 뒤 `ReleaseStaging`하면 업로드 fence가 끝난 뒤 재사용 목록으로 돌아간다. 비동기 readback은 `RequestReadback` → 완료 지점 → `TryReadback`(비차단) → `ReleaseReadback`이고, 렌더 lane은 `RenderLane::RequestReadback`·`TakeReadback`과 `RenderNoticeKind::ReadbackReady`로 넘긴다. D3D12는 내부 잠금으로 `concurrentResourceCreation`을 열었다. OpenGL은 갱신을 즉시 올리고 staging은 CPU 메모리, readback은 pixel pack buffer이며 생성은 소유 스레드 전용 그대로다. 렌더 lane 내용 갱신 작업은 `RenderLane::UpdateBuffer`·`UpdateTexture`다.
- **GX9 timestamp·marker.** 13절.
  - **구현(2026-11-17).** Graphics 계약만 두었다(`GpuScope`는 RB7). `CommandContext::WriteTimestamp`(번호 반환)·`PushMarker`·`PopMarker`, `Device::CollectTimestamps`(비차단)·`CalibrateClocks`, `DeviceInfo`의 `timestamps`·`timestampFrequency`·`timestampDisjointDetection`·`clockCalibration`·`debugMarkers` 공개. 프레임 slot 링은 backend 공용 `Graphics/TimestampRing.h`(slot `TimestampFrameSlots` = 4, slot당 `MaxTimestampsPerFrame` = 256)이며 fence 완료 뒤에만 값을 내고 빈 slot이 없으면 기다리지 않고 `Busy`다. 렌더 lane은 프레임 GPU 완료 때 결과를 `RenderNoticeKind::GpuTimestamp`(frame = 번호, value = tick, 끊김이면 `Cancelled`)로 같은 프레임의 `FrameCompleted` 앞에 넣는다. D3D12: TIMESTAMP query heap과 제출 전 resolve, `GetClockCalibration`(QPC를 ns로), PIX 형식 `BeginEvent`·`EndEvent`. OpenGL: `GL_TIMESTAMP` query(counter bit가 0이면 "없음"), `GL_EXT_disjoint_timer_query`가 있을 때만 끊김 감지, `glGetInteger64v(GL_TIMESTAMP)`와 CPU 시각(앞뒤 가운데) 쌍, debug group. 프레임 끝에 열린 marker는 장치가 닫고 짝 없는 pop은 무시하고 `IgnoredDraws`를 올린다.
- **GX10·GX11.** 5.4절. **구현(2026-11-17).** `Device::IsOwnerThread`·`ReleaseOwnership`·`AcquireOwnership`·`RejectedCalls`, `DeviceInfo::concurrentResourceCreation`(D3D12는 GX8 업로드 경로와 함께 true, OpenGL은 false). 소유 스레드 전용 호출은 Debug·Development에서 `WrongThread`(반환값 없는 호출은 무시·계수), Release는 검사 생략이다. OpenGL은 이관 때 context를 떼고 새 스레드에 붙인다.
- **GX12 transient heap(후속).** heap 배치, aliasing barrier(acquire·discard). RG5가 쓴다.
- **GX13 subpass·framebuffer fetch(후속).** Vulkan subpass, GLES framebuffer fetch 확장. RG3의 render pass 병합이 쓴다.

## 5. 렌더 lane

### 5.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 스레드 | 전용 OS 스레드 하나를 task graph의 렌더 스레드로 붙이고 그 큐를 처리한다 | `RC/Private/RenderingThread.cpp:321, 227–262` |
| 시작 | 스트리밍 정지 → flush → 게임 스레드가 RHI 소유권을 놓음 → RHI 스레드 모드 결정 → 스레드 생성·붙음 대기 → 설정 전파기 등록 → 펜스 왕복 → heartbeat 스레드 | `:561–654`(소유권 `:582`) |
| 종료 | heartbeat 정지 → flush → RHI 스레드 삭제 → 렌더 루프 탈출 작업 → Join → 게임 스레드가 소유권을 다시 잡음 → 지연 정리 묶음 삭제 | `:668–760`(소유권 `:746`) |
| 스레드 없는 경로 | 렌더 스레드가 없으면 명령을 바로 실행한다 | `RC/Public/RenderingThread.h:149, 504–525`, `Launch/Private/LaunchEngineLoop.cpp:2869–2872` |
| 명령 | 이동 전용 함수 세 형태(인자 없음·명령 목록·immediate 명령 목록) | `RC/Public/RenderingThread.h:180–184, 1087–1089` |
| 명령 큐 | 데스크톱 기본 모드: 뮤텍스로 보호하는 큐에 넣고, 비어 있다 처음 찰 때만 렌더 스레드용 task 하나를 띄워 FIFO로 비운다. 모바일 셰이더 플랫폼과 렌더하지 않는 앱: 명령마다 task 하나 | `RC/Private/RenderingThread.cpp:129–149, 1905–1935`, `RC/Public/RenderingThread.h:394–435, 508–515` |
| 보조 직렬 pipe | worker에서 직렬 실행하는 pipe. Engine 모듈의 SkeletalMesh와 플러그인 Niagara·Cable·Groom이 쓴다 | `RC/Public/RenderingThread.h:561`, `RC/Private/RenderingThread.cpp:2380–2416`, `Engine/Private/Rendering/RenderCommandPipes.cpp:5`, `Plugins/FX/Niagara/…/NiagaraComponent.cpp:58`, `Plugins/Runtime/CableComponent/…/CableComponent.cpp:20`, `Plugins/Runtime/HairStrands/…/GroomResources.cpp:45` |
| 지연 기록 명령 목록 | 다른 스레드에서 잠금 없이 기록해 두었다가 나중에 끼워 넣는다 | `RC/Public/RenderingThread.h:725–740, 803–832` |
| 동기화 | 펜스는 깊이 셋(렌더 스레드 처리·GPU 제출·스왑체인 flip). 게임 대기는 1 ms 단위로 기다리며 메시지 펌프와 상태 검사를 돌리고, 120초를 넘기면 hang 보고 후 종료 | `RC/Public/RenderCommandFence.h:14–53`, `RC/Private/RenderingThread.cpp:977–1051, 1089, 1096–1257` |
| 프레임 지연 한도 | 렌더 스레드 펜스는 N−1 프레임, 더 깊은 단계는 설정에 따라 기본 N−2까지 겹친다 | `:2435–2451, 2454–2572` |
| 실패 | Windows SEH로 잡아 상태를 기록하고 게임 스레드가 감지해 종료 | `:381–408, 816–842` |
| 렌더 스레드 tickable | heartbeat 스레드가 sleep 폴링으로 tick 명령을 넣는다 | `:466–484`, `RC/Public/TickableObjectRenderThread.h:61–95` |

### 5.2 Rocks 구조

```text
게임 lane                                렌더 lane(DedicatedLane)                   장치
RenderNotices 비우기(완료 결과 수신)  ◀── 완료 통지
FrameSlot 확보(메시지 펌프하며 대기)
World → FramePacket 추출
RenderLane::Post(프레임 packet) ──────▶ 그래프 구성·기록 ──────────────────────▶ Submit → GpuFencePoint
RenderLane::Post(자원·Retire) ────────▶ DeviceResourceOwner 갱신                   │
RenderLane::Post(표면 작업) ──────────▶ 스왑체인 생성·resize·파괴                  │
                                       fence 완료 이벤트 수신 → slot 완료·회수 ◀──┘
```

- 렌더 lane은 CP1 `DedicatedLane`의 인스턴스다. lane 작업 종류는 넷으로 닫는다: 프레임 packet 처리, 장치 자원 생성·갱신·`Retire`, 표면 작업(스왑체인 생성·resize·파괴, 표면 소실·재생성), lane 표지.
- **전달 가능한 타입.** lane 작업이 받는 값 타입은 "렌더 전달 가능" 표시를 명시 등록한 것만 허용한다(등록되지 않은 타입은 컴파일 오류). 등록 타입이 리플렉션 스키마를 가지면 Development에서 Object 참조 필드가 없는지 검사한다. 원시 포인터는 C++ 타입 검사로 걸러낼 수 없으므로(trivially copyable 구조체 안의 포인터) 이 두 단계가 강제할 수 있는 범위다. 프레임 packet의 가변 길이 목록은 그 프레임 slot의 프레임 arena(CP3)에 둔다.
- **인라인 모드**는 CP1 인라인 모드(trampoline)를 쓴다. 두 모드의 결과(장치 호출 열, 자원 상태)가 같아야 한다.
- **완료 통지 큐** `RenderNotices`: readback 결과, timestamp, `Retire` 회수 완료, 셰이더 공급 완료 같은 결과를 렌더 lane이 넣고, 게임 lane이 프레임 시작 때 비운다. 게임 쪽 CPU 데이터를 GPU 완료 뒤까지 붙잡아야 하는 경우(ObjectRuntime `BackendLease`, `ObjectRefs.h:216–220`) 그 해제는 게임 쪽 통합 계층(WorldIntegration)이 이 통지를 받아 한다. RenderBase는 ObjectRuntime에 의존하지 않는다.
- **fence 관찰.** 렌더 lane은 GX7 완료 이벤트를 lane 작업으로 받는다(폴링하지 않는다). 그래서 lane이 놀고 있어도 slot 완료와 회수가 진행된다.
- 보조 직렬 pipe와 지연 기록 명령 목록은 후속이다. 병렬 장면 준비(계획 8.4절)가 생길 때 CP2 직렬 체인으로 만든다.

### 5.3 프레임 slot과 대기 규칙

- 프레임 slot은 2–3개다(계획 8.2절, 개수는 설정). slot마다 완료 표지가 둘이다: **lane 처리 완료**와 **GPU 완료**(그 프레임의 모든 queue `GpuFencePoint` 완료). UE 펜스 깊이 셋 가운데 앞의 둘에 해당한다.
- 게임 lane은 프레임 시작 때 slot을 확보한다. 빈 slot이 없으면 가장 오래된 slot의 GPU 완료를 기다린다. 렌더 lane은 게임 lane을 기다리지 않으므로 한 방향 대기다.
- **대기 중 메시지 펌프.** 게임 lane이 창 스레드이면 slot 대기는 OS 메시지를 펌프하는 이벤트 대기다. DXGI의 present·resize는 창 스레드로 동기 메시지를 보내므로, 창 스레드가 펌프 없이 막히면 교착이 난다(Microsoft DXGI 문서 "Multithread Considerations"). UE도 대기 중 메시지를 펌프한다(5.1절 동기화 행).
- **예외.** (1) 표면 소실 handshake: Android가 surface를 거둘 때 앱 스레드는 렌더 lane의 surface 사용 중단(표면 작업 완료)을 동기로 기다려야 한다(P13). (2) 전체 비우기 `RenderLane::Drain`: 종료·장치 재생성·시험 전용이며 호출 이유(열거)를 인자로 받아 계측에 남긴다. 일반 자원 해제는 `Retire`로 하고 비우기를 쓰지 않는다. 자원 해제 뒤 비우기까지 하는 API는 두지 않는다.
- 대기 시간 상한(UE 120초 hang 판정, 5.1절)은 진단 전용이다. 시간 초과는 hang 보고 후 실패로 끝내되 기능 판정에 쓰지 않고, 디버거 연결·명시 정지 계수가 있으면 끈다. 시험은 시간 초과에 기대지 않는다.
- 렌더 설정(CP4)은 프레임 packet에 snapshot으로 담는다. 렌더 쪽 사본 주소에 쓰는 전파는 하지 않는다.

### 5.4 장치 스레드 모델(GX10·GX11)

지금 `Device`와 최소 렌더러(현 Sandbox `SampleRenderer`)는 "생성한 스레드에서만 사용"이고(`Device.h:43`) D3D12·OpenGL backend 어디에도 호출 스레드 검사가 없다. 지연 셰이더 생성·병렬 기록·게임 lane handle 예약을 하려면 계약을 다음처럼 나눈다.

| 호출 종류 | 규칙 |
|---|---|
| 제출·present·fence 신호·장치 상태 변경 | 소유 스레드(렌더 lane) 전용 |
| 기록 객체 | 기록하는 스레드마다 하나. 기록 객체는 소유 스레드가 제출한다 |
| 자원·셰이더·파이프라인 생성 | backend가 `DeviceCaps`로 스레드 안전 여부를 공개한다. D3D12·Vulkan·Metal은 스레드 안전 생성(backend 내부 잠금). OpenGL ES는 context가 스레드에 묶이므로 소유 스레드에서만 만들고, 다른 스레드의 생성 요청은 lane 작업으로 넘긴다 |
| handle 예약 | 게임 lane은 장치 handle이 아니라 RenderBase의 `RenderHandle`을 예약받는다. RenderBase 표가 `RenderHandle` → 장치 handle을 잇고, 장치 내부 slot 표는 소유 스레드만 만진다 |
| 소유권 이관(GX10) | 소유 스레드가 놓고 새 스레드가 잡는다. 그 사이 소유 스레드 전용 호출은 `WrongThread`로 거부한다(Development 검사, Release에서는 검사 생략) |

렌더 lane 시작 때 장치 소유권을 lane으로 넘기고, 종료 때 되돌린다(UE도 시작·종료 때 소유권을 넘긴다, `RC/Private/RenderingThread.cpp:582, 746`, `RHI/Public/DynamicRHI.h:1497`). 장치를 렌더 lane 안에서 만드는 방식도 가능하지만 창·스왑체인 생성이 게임 lane 쪽 PlatformWindow와 엮이므로 이관 방식을 기본으로 한다.

### 5.5 시작·종료 순서

**시작:** 창·장치 생성(게임 lane) → `DeviceCaps` 조회 → 렌더 lane 시작(스레드 이름 붙임) → 장치 소유권 이관 → 스왑체인 생성(표면 작업) → 엔진 셰이더 집합 열기와 검사(9.2절) → 내장 자원 팩 생성·업로드(6.3절, fallback 셰이더 포함) → fence → `BuiltinReady` → 일반 에셋·프레임 시작.

**종료**(계획 3.2절 Shutdown 순서의 "GPU fence 완료"부터 "owner/retire/queue closure 확인"까지, CP6 hook):
1. 새 프레임 packet 차단.
2. 마지막 slot의 GPU 완료 대기.
3. 최종 GC와 `Retire` drain을 closure가 닫힐 때까지 반복한다. 최종 GC가 에셋을 파괴해 생긴 `Retire`도 이 반복에서 처리된다. 렌더 lane은 이 단계 내내 살아 있다.
4. 엔진 셰이더 집합·내장 자원 `Retire`, 파이프라인 캐시 비우기, 회수 완료 대기.
5. 스왑체인 파괴(표면 작업).
6. 렌더 lane Drain(남은 작업 0 확인).
7. 장치 소유권을 게임 lane으로 되돌림 → lane Join.
8. 장치 `WaitIdle`·소멸 → 창 파괴.

실패한 단계는 뒤 단계를 건너뛰지 않고 오류로 보고한다. 계획 3.2절은 timeout을 실패로 보며 강제 free하지 않는다.

### 5.6 실패 전파

렌더 lane의 장치 손실·치명 오류는 lane 상태에 기록하고, 게임 lane은 다음 slot 확보 결과(`Result`)와 완료 통지로 관찰한다. 기다리던 fence 대기는 GX7이 오류로 깨운다. 렌더 lane 스레드의 치명 오류 포착은 플랫폼 Fatal 처리(`Platform/Fatal.h`) 쪽 일이다. 장치 손실 뒤 재생성은 계획 11.2절(device epoch, 구 장치 recovery domain)을 따른다.

### 5.7 완료 기준(RB1)

**구현(2026-11-17).** 새 모듈 `Engine/Source/Runtime/RenderBase`(의존 Graphics·Task·MetaSchema)에 `RenderLane`·`FrameTicket`·`RenderLaneProcessor`·`ROCKS_RENDER_TRANSFERABLE`·`RenderNotices`·`RenderHandleTable`·`RenderBlob`을 두었다. 아래 기준은 기록 backend(`RenderBase/Tests/RecordingDevice`) CPU 시험과 Sandbox self-test(D3D12·WARP·OpenGL, Thread·Inline)로 확인한다. 구현 메모:
- 작업 함수는 캡처 없는 함수 포인터만 받는다. 캡처로 게임 주소를 넘기는 길을 막기 위해서다. lane 쪽 객체는 `RenderLaneContext::Processor`로 얻는다.
- `ObjectRef`는 이동할 수 없으므로 그 필드를 가진 struct는 등록해도 C++ 타입 검사가 먼저 거부한다. Development 스키마 검사는 이동할 수 있는 `WeakRef`·참조 컨테이너 필드를 거부한다(중첩 struct는 보지 않는다).
- slot의 GPU 완료 표지는 그 프레임 작업 안의 마지막 제출 fence이며 `NotifyFence`로 받는다. fence 완료는 lane inbox로 들어가 lane 작업(Inline 모드는 기다리는 게임 lane이 실행하는 lane 작업)이 장치 회수 → 통지 순서로 처리한다.
- 대기 중 펌프는 1 ms 간격(UE와 같다)이며 펌프 hook이 없으면 이벤트만 기다린다. 대기 진단 상한(120초)의 디버거 연결 감지는 아직 없다.
- `Start`는 시작 작업 완료를 기다리지 않는다(첫 `BeginFrame`이 기다린다). 그래서 이관 중에 게시한 표면 작업도 lane이 소유권을 얻은 뒤 FIFO로 실행된다.
- 설정 snapshot(12절)은 `RenderLane::AttachSettings(ticket, RenderSettingsRef)`로 slot에 담고 `RenderFrameContext::Settings`로 읽는다(개정 8).

- 인라인 모드와 lane 모드에서 같은 프레임 packet 열이 같은 장치 호출 열을 낸다(기록 backend 대조). lane 작업 안의 재진입 `Post` 순서도 같다.
- slot 2개 설정에서 GPU 완료 이벤트를 gate로 막으면 게임 lane이 세 번째 slot 확보에서 대기하고, gate를 열면 진행한다. 대기 중 메시지 펌프가 도는지 계수로 확인한다.
- 등록되지 않은 값 타입을 받는 lane 작업은 컴파일 오류다. Object 참조 필드가 있는 등록 타입은 Development 검사가 거부한다.
- 종료 순서: 진행 중 packet과 최종 GC가 만든 `Retire`가 있는 상태의 종료에서 남은 작업 0, 해제 누락 0, 장치 소유권 복귀, 스왑체인이 창보다 먼저 파괴.
- 소유권 이관 중 resize 요청이 표면 작업으로 처리된다(거부 0).
- 장치 손실 주입 뒤 다음 slot 확보가 오류를 돌려주고, 기다리던 fence 대기가 깨어난다.

## 6. 장치 자원 수명

### 6.1 UE 5.8 방식

- 렌더 쪽 자원 기반 객체가 장치 자원 초기화·해제 가상 함수와 초기화·해제·갱신 함수를 가진다(`RC/Public/RenderResource.h:36, 83–108`). 초기화 상태로 소멸하면 Fatal이다(`RC/Private/RenderResource.cpp:247–256`).
- 게임 스레드는 자원 포인터를 캡처한 명령으로 초기화·갱신·해제를 넣는다(`:290–390`).
- 전역 자원은 단계 목록에 올렸다가 RHI 준비 뒤 일괄 초기화한다(`:121–140`, 호출 예 `D3D12RHI/Private/Windows/WindowsD3D12Device.cpp:2029`). 장치 재생성 때는 목록 전체를 해제하고(`:113–118`), 기능 수준이 바뀌면 그 수준이 다른 자원만 다시 만든다(`:142–162`). iOS는 첫 초기화 뒤 목록을 비운다(`:136–139`).
- 지연 정리는 프레임 끝 동기 한 번 뒤 delete(`RC/Private/RenderingThread.cpp:1323–1419`), RHI 자원은 참조 0에서 지연 삭제 목록(`RHI/Private/RHIResources.cpp:46–58`).

### 6.2 Rocks 규칙

- **장치 자원 소유자.** 렌더 lane 쪽 객체로, 장치 handle과 상태(`Pending`·`Ready`·`Failed`·`Retired`)를 가진다. 게임 lane은 생성 요청 때 곧바로 `RenderHandle`을 예약받고(5.4절), 실제 장치 자원 생성은 렌더 lane 작업이 한다. 생성이 실패하면 `Failed`가 되고 완료 통지로 알린다.
- **준비 전 사용.** `Pending`·`Failed` handle을 그리기에 쓰면 그 그리기는 내장 대체 자원(형식·차원이 맞는 typed fallback, 계획 9.6절)을 쓰고 진단 계수를 올린다.
- **회수.** 게임 lane의 해제 요청은 `Retire` lane 작업이다. 게임 lane은 last-use를 모르므로 `Retire`는 handle만 받고, last-use는 렌더 lane과 장치의 내부 집계가 정한다(계획 11.2절 "RHI 내부 last-use 집계가 최종 기준"). `Retire`는 새 lease를 막고, 이미 승인된 기록(그래프 import·기록 packet)의 lease와 마지막으로 쓴 모든 queue의 `GpuFencePoint`가 끝난 뒤 장치 자원을 해제하며, 회수 완료를 통지한다.
- **소멸 진단.** `Ready` 상태로 소유자가 파괴되면 모든 구성에서 `Retire` 경로로 넘기고, Development에서는 진단을 더한다(누수 Gate, 계획 29.6절).
- **장치 재생성.** 장치 손실 뒤 새 device epoch에서 소유자 목록을 다시 초기화한다. 구 장치 handle은 stale이다(계획 11.2절 device epoch, 9.6절 BR04). 장치 능력이 바뀌는 경우(장치 교체)도 같은 경로다.
- **갱신.** 버퍼·텍스처 내용 갱신은 GX8 업로드 ring을 쓰는 lane 작업이다. 같은 프레임 안 갱신 순서는 lane FIFO가 보장한다.

### 6.3 내장 자원 팩 초기화

5.5절 시작 순서에서 엔진 셰이더 집합 검사 뒤, `BuiltinReady` 전에 만든다. 계획 9.6절 팩이 fallback 셰이더(cooked shader variant)를 포함하므로 셰이더 집합 검사가 먼저다. UE와 다른 점: 정적 초기화 목록에 기대지 않고, 생성 실패 시 `BuiltinReady`를 게시하지 않으며 시작이 실패한다(UE에는 준비 완료 gate와 생성 실패 경로가 없다). 1차 내용은 11.1절이다.

### 6.4 완료 기준(RB2)

- 계획 11.2절 API04(recording·cancel·late submit·자원 교체·builtin·GPU completion 수명), 9.6절 BR02–BR04, BR01은 11.1절 1차 팩 범위로 판정한다(error material·글꼴은 그 단계에서 범위를 넓힌다).
- `Pending`·`Failed` handle 그리기의 대체 자원 사용과 계수.
- `Retire` 뒤 lease가 남은 동안 회수 0건, fence 완료 이벤트 뒤 회수(이벤트를 gate로 막아 단언).
- 장치 재생성 뒤 구 handle 사용이 stale 오류.

### 6.5 텍스처 플랫폼 데이터 입력 계약

AssetSystem A5가 쿠킹하는 텍스처 플랫폼 데이터를 렌더(GPU 업로드·텍스처 스트리밍)가 읽는 계약이다. 에셋 세션과 합의했다(2026-11-16). 분담은 다음과 같다. 압축(BC·ASTC·ETC2)·색 보정은 에셋 빌드(A 단계, 에셋 너겟 키)가 맡는다. GPU 업로드·스트리밍·mip 상주는 렌더가 맡는다. 무손실 원본 bulk는 에디터 전용이며 쿠킹 데이터에 들어가지 않으므로 렌더는 읽지 않는다. 플랫폼 데이터는 TargetProfile마다 만들고 에셋 너겟 키로 캐시한다(형식 선택이 프로필에 걸린다).

**UE 5.8 대응.**
- 플랫폼 데이터는 크기·cube 여부·slice 수·픽셀 형식·플랫폼별 추가 데이터(mip tail 수 포함)·mip 배열을 가진다(`Engine/Classes/Engine/Texture.h:880–895`).
- mip 하나는 크기(SizeX·SizeY, depth 또는 array 크기를 담는 SizeZ)와 bulk 하나를 가진다(`Engine/Public/TextureResource.h:45–57`).
- cube는 한 mip의 bulk 안에 face를 이어 둔다(`Engine/Private/TextureCube.cpp:392`). face 순서는 +X·−X·+Y·−Y·+Z·−Z다(`RHI/Public/RHIDefinitions.h:506–515`).
- 쿠킹은 mip을 셋으로 나눈다. optional(선택 다운로드)·스트리밍(별도 파일)·inline(에셋과 함께 상주)이다. inline 꼬리는 기본 7개와 형식 mip tail 수 가운데 큰 값이다(`Engine/Private/TextureDerivedData.cpp:1644, 3374–3392`, `Engine/Private/TextureDerivedDataTask.h:32`).

**메타.** 텍스처마다 다음을 가진다.
- 차원(2D·Cube·3D·2DArray·CubeArray).
- 형식·sRGB·swizzle. 모두 GraphicsTypes 값이다.
- 기준 크기: 폭·높이, 그리고 depth(3D) 또는 slice 수(array). cube는 face 6 × cube 수다.
- mip 수와 상주 꼬리 mip 수.
- mip마다: 크기(폭·높이·depth), 바이트 크기(패딩 제외), 덩어리 hash(Core 공용 hash. 스트리밍 전송이 읽은 뒤 대조한다, [텍스처 스트리밍](TextureStreaming.ko.md) 5.1절), 위치(상주 꼬리 덩어리 안의 오프셋, 또는 스트리밍 덩어리 id·오프셋).
- 렌더는 상주 mip 수를 다시 계산하지 않고 메타 값을 쓴다.

**바이트 배치.**
- mip-major: 큰 mip(0)부터 둔다. 스트리밍이 mip 하나의 모든 slice를 함께 올리기 때문이다.
- 한 mip 안의 순서:
  - array는 slice 0부터.
  - cube는 face +X·−X·+Y·−Y·+Z·−Z 순(UE와 같다).
  - cube array는 cube 0의 face 6개, 다음 cube 순.
  - 3D는 z 0부터.
- 2D 이미지 하나 안은 첫 행이 위이고 행 사이 padding이 없다. 지금 `TextureDesc`의 초기 픽셀 규칙(`Engine/Source/Runtime/Graphics/Public/Graphics/DeviceTypes.h:108–128` 주석)을 확장한 것이다.
- 블록 압축 형식은 블록 행 단위다. 크기는 ceil(폭/블록 폭) × ceil(높이/블록 높이) × 블록 바이트다.
- 장치 정렬(D3D12 행 pitch 256 byte, placed 자원 정렬 등)은 렌더 업로드(GX8)가 맞춘다. 에셋 데이터는 장치 정렬을 담지 않는다.

**스트리밍 단위와 정렬.**
- 스트리밍 mip은 mip마다 따로 읽을 수 있는 덩어리 하나다.
- 상주 꼬리(가장 작은 mip들)는 한 덩어리로 묶어 에셋과 함께 읽는다.
- 상주 꼬리 mip 수는 max(설정한 최소 상주 mip 수, 형식이 요구하는 mip tail 수)이고, 쿠킹 때 정해 메타에 적는다. 최소 상주 mip 수의 기본값은 7이고, 블록 압축 형식은 가장 작은 스트리밍 mip이 블록 4×4 이상이 되도록 늘린다(UE와 같다, [텍스처 스트리밍](TextureStreaming.ko.md) 4.3절).
- 스트리밍 덩어리의 시작 오프셋은 파일 안에서 4 KiB 정렬이고 패딩은 0으로 채운다. 메타의 크기는 패딩을 뺀 실제 크기다. 나중에 unbuffered 비동기 읽기나 mmap을 쓸 여지를 남기려는 것이고, 지금 직접 I/O를 쓰지는 않는다. 상주 꼬리 덩어리 안의 mip 오프셋은 16 byte 정렬이다.
- 덩어리를 패키지 안에 둘지 별도 파일에 둘지는 AssetSystem A1 패키지 형식이 정한다.

**직렬화.** 형식·sRGB·swizzle·차원은 GraphicsTypes가 가진 고정 번호표와 안정 이름(텍스트·JSON용)으로 기록하고 enum 순서 값을 쓰지 않는다. 번호는 한 번 정하면 재사용하지 않는다. 렌더 쪽 장치 형식 변환도 이 번호표를 기준으로 한다.

**렌더 쪽 검사.** 텍스처를 만들 때 다음을 확인한다.
- 메타의 mip 크기·바이트 크기가 위 공식과 맞는지. 다르면 `CorruptEntry`다.
- 차원·형식 조합을 장치가 지원하는지(`DeviceCaps`, GX1). 지원하지 않으면 `UnsupportedCapability`다.
- 상주 꼬리만으로 GPU 자원을 만들 수 있는지(가장 큰 상주 mip 기준으로 만들고, 스트리밍 mip은 상주가 늘 때 채운다).

**후속.** optional mip(선택 다운로드), 가상 텍스처(AssetSystem 12.4절), 플랫폼 tiling 배치. 블록 압축 형식 표(BC1–7·ASTC·ETC2와 GLES·Android 가용성)는 압축 단계 착수 전에 에셋 세션과 함께 확정한다. 형식 정의·블록 크기·고정 번호표·backend 가용성은 [픽셀 형식](PixelFormat.ko.md) 5·6·8·12절이 정하고, 프로필별 형식 선택과 인코더가 에셋 세션 몫으로 남는다. 메타의 "sRGB" 항목은 픽셀 형식 결정 PX1에 따라 형식에 포함되거나 따로 남는다.

## 7. 렌더 그래프

### 7.1 UE 5.8 방식

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| 생성 | 스택 객체, prologue 감시 pass, async compute·병합·병렬 허용과 transient 할당기 결정. 병합은 tiled GPU 전용이고 async compute와 함께 켜지지 않는다 | `RC/Public/RenderGraphBuilder.h:50`, `RC/Private/RenderGraphBuilder.cpp:586–648`, `RC/Private/RenderGraphPrivate.h:224–234` |
| pass 추가 | 이름·파라미터 구조체(그래프 수명 할당, 넘긴 뒤 수정 금지)·플래그·람다. 파라미터 없는 pass는 자동 유지 | `RenderGraphBuilder.h:180, 203–207, 220–233`, `RenderGraphBuilder.inl:249–283` |
| 자원 수집 | 구조체 layout의 그래프 자원 목록을 훑어 subresource별 접근 상태를 모은다 | `RenderGraphBuilder.cpp:2384–2532`, `RC/Public/RenderGraphResources.h:72–131` |
| culling | 자원별 마지막 생산자로 간선을 만들고, 유지 pass·외부 출력·내보내기에서 역방향 DFS로 살린다. pass 추가 때 점진 계산 | `RenderGraphBuilder.cpp:1199–1325, 2329–2382` |
| 컴파일 | culling 정리 → render pass 병합(같은 타깃의 연속 raster pass) → async compute fork/join(수명을 구간 전체로 연장) | `:1327–1675` |
| barrier | 같은 자원의 병합 가능한 연속 상태를 합치고 바뀌는 곳에 전이. 같은 pipe면 생산 pass epilogue에서 begin, 소비 pass prologue에서 end | `:3783–3928, 4439–4584`, `RC/Private/RenderGraphResources.cpp:69–152` |
| 수명·할당 | 참조 수 0인 pass에서 해제, 처음 쓰는 pass에서 할당. 풀 자원 → transient 자원 → 첫/마지막 전이 → 전이 객체 미리 생성 | `RenderGraphBuilder.cpp:1962–2044, 3590–3780, 3930–4044` |
| 실행 | pass를 선언 순서대로 실행한다(재배치 없음). pipe 전환 → prologue barrier → attachment 시작 → 람다 → 끝 → epilogue barrier | `:2104–2167, 3424–3528` |
| 풀 | 텍스처는 렌더 타깃 풀, 버퍼는 그래프 버퍼 풀. 해제 fence와 새 할당 fence가 겹치지 않으면 같은 프레임 안에서도 같은 desc 자원을 통째로 재사용(aliasing 아님). 렌더 타깃 풀은 3프레임 이상 안 쓴 것을 풀 크기가 하한 설정을 넘을 때만 오래된 순으로 해제하고, 버퍼 풀은 30프레임 안 쓴 것을 해제한다 | `RC/Private/RenderTargetPool.cpp:211–245, 402–436`, `RC/Private/RenderGraphResourcePool.cpp:110–145, 214, 228` |
| transient aliasing | RHI transient 할당기(D3D12·Vulkan만 구현, 나머지는 풀). 대상은 UAV 버퍼와 공유 아닌 텍스처 | `RHI/Public/RHITransientResourceAllocator.h:535–555`, `RenderGraphBuilder.cpp:502–535` |
| 업로드 | 그래프 실행 앞부분에서 풀 버퍼를 lock으로 채운다 | `RenderGraphBuilder.h:309–333`, `RenderGraphBuilder.cpp:2691–2839` |
| pass 종류 | Raster·Compute·AsyncCompute·Copy(그래픽 pipe)·NeverCull·SkipRenderPass·NeverMerge·NeverParallel·Readback(=Copy+NeverCull) | `RC/Public/RenderGraphDefinitions.h:129–160` |
| task 기반 설정 | 설정 task·명령 목록 설정 task, task로 기록하는 dispatch pass, 기다리지 않는 비동기 pass 표지 | `RenderGraphBuilder.h:239–284`, `RenderGraphDefinitions.h:65–107` |
| 디버그 | 즉시 실행 모드, 검증(한 pass 안 상충 접근 `RenderGraphValidation.cpp:583`, 생산 전 읽기 `:653`, 생산 전 내보내기 `:427`, 파라미터 없는 pass의 플래그 `:597`), 전이 로그, 자원 덮어쓰기, 이벤트 구간. 그래프 설정 변수 31개 | `RC/Private/RenderGraphPrivate.cpp:18–426` |

### 7.2 계획 11.3절과의 대조

| 계획 11.3·11.5절 | UE 그래프 | Rocks 결정 |
|---|---|---|
| DAG 검증 | 재배치·위상 정렬 없음. 의존은 앞쪽 pass만 가리키므로 순환이 생기지 않는다 | 선언 순서 실행, 재배치 없음(결정 R5). "DAG 검증"을 "의존 정합성 검증"으로 고친다. 계획 11.4절 "current color를 다시 자기 조명 입력으로 쓰는 순환 거부"는 한 pass 안 같은 subresource 읽기·쓰기 상충 검사로 한다 |
| 미사용 pass 제거 | 점진 culling | 같다 |
| lifetime 분석 | 참조 수 | 같다 |
| barrier·queue wait | begin·end 분리, cross-pipe fence | 같다. native split이 없는 backend는 end 지점에서 낮춘다(GX5) |
| transient alias | 지원 RHI만, 없으면 풀 | 같다(RG5). alias를 끈 1차에도 **풀**이 필요하다(계획에 없던 단계) |
| copy queue → compute overlap → alias 순서 | 그래프 pipe는 그래픽·async compute 둘뿐. Copy pass는 그래픽 pipe. 업로드는 그래프 앞 lock | copy queue 위치는 결정 R6(권장: 그래프 밖 업로드·스트리밍 계층). UE 대응이 없으므로 자체 설계로 표시한다 |
| 계획에 없음 | render pass 병합(tiled GPU 전용) | OpenGL ES·Android가 필수이므로 RG3에 넣는다(GX13) |
| 계획에 없음 | 병렬 기록·람다 형태에 따른 실행 방식 | RG3 |
| D3D11/OpenGL 직렬 lowering(11.5) | OpenGL 전이는 빈 함수, dispatch 앞뒤 memory barrier | 같다(GX5) |
| enhanced barriers(11.3) | RHI 안에서 고른다 | 그래프 쪽 접근 상태는 하나, backend가 lowering(GX5) |

### 7.3 Rocks 구조

- 그래프 객체 하나가 한 프레임(또는 한 offscreen 작업)의 pass를 모은다. 렌더 lane이 프레임 packet을 처리하면서 만든다. 그래프 수명 할당은 CP3 프레임 arena다.
- 자원: `GraphTexture`·`GraphBuffer`와 view(SRV·UAV, mip·slice 범위).
- pass: 이름·파라미터 구조체·종류(Raster·Compute·AsyncCompute·Copy)·플래그(`HasSideEffects`·attachment 생략·병합 금지·병렬 금지)·기록 함수. 파라미터 구조체가 의존 선언이다(9.6절). 파라미터 없는 pass, readback·present pass는 `HasSideEffects`다(계획 11.3절).
- 접근 상태는 멤버 종류와 pass 종류의 조합으로 정한다(Raster의 SRV는 그래픽 SRV, Compute의 SRV·UAV는 compute 접근, Copy는 복사 원본·대상). UAV는 CS만 허용되므로(셰이더 시스템 7.2절 4항) Raster pass에 UAV 쓰기 멤버가 오면 검증 오류다.
- 공유 값(`GraphSharedData`): 타입별 값 하나씩, 그래프 수명.
- 즉시 실행 디버그 모드: pass를 추가하는 즉시 수집·할당·전이·실행한다. 지연 모드와 결과가 같아야 한다.
- 이벤트 구간: pass마다 `GpuScope`(13절).

### 7.4 그래프 자원 수명

- **import.** `ImportTexture`·`ImportBuffer`는 처음 접근 상태와 함께 장치 자원을 들이며 그 자원의 `RenderResourceLease`를 얻는다. `Retire`된 자원이면 lease를 얻지 못하고 import가 오류를 돌려준다. 호출자는 내장 typed fallback을 import하거나 그 pass를 추가하지 않는다. 그래프는 조용히 대체하지 않는다.
- **export.** `ExportTexture`·`ExportBuffer`는 마지막 접근 상태(기본 SRV)를 정하고, 풀 자원의 사용권을 호출자의 `DeviceResourceOwner`로 넘긴다. 자원은 풀 소속 그대로다. 소유자의 `Retire`는 장치 해제가 아니라 마지막 사용 `GpuFencePoint`가 끝난 뒤 `GraphResourcePool` 반납이고, 아래 풀 축출 규칙은 반납 뒤부터 적용된다(결정 R15, 개정 16). history(이전 프레임 결과)가 이 경로다(7.5절).
- **풀 축출.** `GraphResourcePool`의 자원은 마지막 사용 `GpuFencePoint`가 완료되었고, 설정한 프레임 수 동안 쓰이지 않았고, 풀 메모리가 예산을 넘을 때 오래된 순으로 `Retire`한다. 계획 8.2절 "경과 프레임 수만으로 회수하지 않는다"에 따라 fence 완료가 필수 조건이다.
- **풀 키.** desc(크기·형식·용도·mip·array·샘플 수)이며 샘플러는 넣지 않는다(결정 R12).

### 7.5 렌더러 사용 요구(Renderer RQ15)

[렌더러](Renderer.ko.md) 15.1절이 렌더 그래프에 요구하는 네 가지다. 렌더 그래프 설계 문서가 따로 없으므로 이 절에 둔다(개정 15). 구현은 8절 RG1의 남은 것과 Renderer RS0이다.

**UE 5.8 근거.**

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 그래프 공유 값 | 타입마다 정적 등록한 구조체 하나. 이미 있는데 만들거나 없는데 `GetChecked`하면 assert | `RC/Public/RenderGraphBlackboard.h:15–20, 56–61, 120–126`, 등록 예 `Renderer/Private/SceneTextures.cpp:82` |
| 장면 텍스처 소유 | view family(Rocks `ViewSet`)가 장면 텍스처 묶음 하나를 가지고 그래프 구성 때 채운다 | `Renderer/Private/SceneTextures.cpp:735–738` |
| history | 이전 결과를 외부 텍스처로 등록해 읽고, 새 결과를 추출 예약한다 | 읽기 `Renderer/Private/PostProcess/TemporalAA.cpp:803`, `Renderer/Private/HZB.cpp:97`. 추출 `Renderer/Private/PostProcess/TemporalAA.cpp:1003`, `Renderer/Private/DeferredShadingRenderer.cpp:607` |
| history 수명 | history는 풀 render target의 참조 계수 포인터다. 새 history로 바꾸면 이전 것은 참조가 풀 하나만 남아 다음 할당이 다시 쓴다 | `Renderer/Private/SceneRendering.h:943–950`, `RC/Private/RenderTargetPool.cpp:202, 754–760` |
| mip view | HZB pass 하나가 위 mip의 SRV를 읽고 아래 mip 여럿을 mip별 UAV로 쓴다. texture view가 없는 장치는 텍스처 전체 SRV를 쓴다 | `Renderer/Private/SceneTextureReductions.cpp:210, 214, 345, 355`. OpenGL ES는 texture view 없음(`OpenGLDrv/Private/OpenGLDevice.cpp:900–901`) |
| view 여럿 | 장면 렌더러(view family 하나)마다 그래프 하나이고, 그 안에서 기능 함수가 view를 돈다. 이름 구간은 형식 문자열로 만든다 | `Renderer/Private/SceneRenderBuilder.cpp:873`, `Renderer/Private/LightGridInjection.cpp:1100`, `RC/Public/RenderGraphEvent.h:479` |

**Rocks 규칙.**
- **`GraphSharedData`.** 타입마다 값 하나, 그래프 수명이다(프레임 arena). 타입은 정적 등록하고, 등록하지 않은 타입은 컴파일 오류다. 이미 있는 타입을 만들면 검증 오류, 없는 값을 반드시 읽는 조회도 검증 오류이고, 있을 때만 읽는 조회는 null을 돌려준다. 값에는 그 그래프의 handle만 담는다(다른 그래프 handle은 7.3절 검증이 거부한다). 값은 그래프가 파괴될 때 렌더 lane에서 파괴된다(8절 구현 메모의 pass 함수와 같다).
- **`SceneTextureSet` 전달(개정 16).** 프레임 packet 하나에 `ViewSet`이 여럿일 수 있다(EditorWorld·PIE 등, Renderer 4.2·5.1절). 그래서 렌더러는 packet 전체의 장면 텍스처를 (ViewSet 번호, view 번호)로 찾는 표 하나(`SceneTextureTable`)를 `GraphSharedData`에 둔다. `ViewSet`마다 값을 두면 타입별 값 하나 규칙과 부딪치기 때문이다. view마다 크기가 다를 수 있어 장면 텍스처는 view마다 따로다(Renderer 5.1절). `ViewSet` 단위 값(GPU Scene 자원·공유 그림자)도 같은 방식으로 ViewSet 번호를 키로 한 표에 둔다. pass는 표에서 자기 (ViewSet, view)의 `SceneTextureSet`을 꺼내 공유 자원 구조체(9.6절)를 채운다. 장면 텍스처를 만들기 전에 꺼내면 검증 오류다(생산 전 읽기와 같은 성격).
- **history export·import.** `ViewHistory` 소유자(`DeviceResourceOwner`)가 history 텍스처를 가진다(Renderer 5.1절).
  - 이번 프레임은 이전 history를 `ImportTexture`(처음 상태 SRV)로 읽고, 새 history를 그래프 텍스처로 만들어 `ExportTexture`한다. 같은 텍스처를 한 프레임에 읽고 쓰지 않는다(이중 버퍼, UE TAA와 같다).
  - export는 그래프 실행이 성공했을 때만 소유자에게 넘어간다. 검증 실패로 그래프를 버리면(8절) export가 없고, `ViewHistory`는 그 history를 무효로 표시해 다음 프레임에 reset한다. 이전 프레임의 history를 한 번 더 쓰지 않는다(시간 순서가 어긋난 history를 쓰지 않는다).
  - **풀 반납(결정 R15).** history 텍스처는 풀 자원이다. 새 history export가 성공하면 소유자는 이전 history를 `Retire`하고, 그 자원은 마지막 사용 fence 뒤 `GraphResourcePool`로 돌아가 같은 desc의 다음 할당이 다시 쓴다(7.4절). 그래서 매 프레임 history를 만들고 놓아도 정상 상태에서는 장치 생성·해제 없이 2–3장이 돈다. UE도 같은 구조다(위 표 "history 수명"). import lease가 있으므로 GPU가 읽는 동안 반납되지 않는다.
  - history가 없는 프레임(첫 프레임·cut·크기 변경, Renderer 5.1절)은 import하지 않고, history 없음 퍼뮤테이션이나 내장 자원을 명시적으로 묶는다(7.4절 "조용히 대체하지 않는다").
- **mip view 그래프 자원.** `GraphTextureView`는 (텍스처, mip 범위, slice 범위, SRV·UAV)다. 상태 추적과 상충 검사는 subresource 단위이므로 한 Compute pass가 mip k의 SRV와 mip k+1…k+n의 UAV를 함께 가질 수 있다(HZB). 범위가 겹치면 한 pass 안 상충 접근이다. 장치 view는 GX2의 텍스처 view(mip·slice 범위)로 낮춘다.
  - texture view가 없는 backend(OpenGL ES)는 SRV에 텍스처 전체를 묶는다. 그래서 mip view를 읽는 셰이더는 그래프가 view마다 주는 **view 기준 mip**(view를 지원하는 backend는 0, 아니면 view의 첫 mip)을 상수로 받아 명시 LOD로 읽는다(`SampleLevel`·`Load`). 크기 조회(`GetDimensions`)도 같은 기준 mip을 명시한다. 텍스처 전체가 묶인 GLES에서 mip 0 크기를 읽지 않기 위해서다(개정 16). backend마다 원문을 바꾸지 않는다. UAV는 GLES image 묶기가 level 하나를 받으므로 그대로 한 mip이다.
  - 같은 텍스처의 mip k 표본과 mip k+1 image 쓰기를 한 dispatch에서 하는 것이 ES 3.2에서 정의된 동작인지는 사양 대조가 남았다(**미확인**). RG1 GLES 시험에서 확인하고, 안 되면 GLES는 mip마다 pass를 나눈다.
- **view 여럿을 한 그래프에.** 프레임 packet 하나가 그래프 하나다(7.3절). 그 안에 packet이 담은 `ViewSet` 여럿과 각 `ViewSet`의 view 여럿이 모두 들어간다. UE는 view family마다 그래프 하나다.
  - 선언 순서(개정 16): `ViewSet` A 공통 pass(장면 변경 적용·GPU Scene 업로드·sky LUT·공유 그림자, Renderer 6.2절 F1–F3·F7) → A의 view들(view 순서, view마다 그 view의 pass 전체) → `ViewSet` B 공통 pass → B의 view들 …. UE는 기능 함수 안에서 view를 도는 pass 단위다. Rocks가 view 단위인 이유: 한 view의 그래프 자원 수명이 그 view 구간에서 끝나므로 다음 view가 같은 desc 자원을 프레임 안 풀로 다시 쓸 수 있다(RG2). 선언 순서가 실행 순서다(결정 R5).
  - **그래프 구간 `GraphScope`.** 그래프에 pass 여러 개를 감싸는 이름 구간을 둔다(`GpuScope` 중첩, 13절). `ViewSet` 구간·view 구간·기능 구간(예: 그림자)에 쓴다. 구간 이름은 정적 이름(`TraceScopeName`, CP7 정적 이름 + 64 bit 해시)이므로 view 구간은 이름 `View`, ViewSet 구간은 `ViewSet`이고, 번호는 구간을 연 직후 trace 표지(`ROCKS_TRACE_MARK`, [다중 스레드 trace](TraceSystem.ko.md) 표지 규칙)로 남긴다(개정 16). 캡처 도구의 GPU marker에는 번호가 보이지 않는다. UE는 형식 문자열로 구간 이름을 만든다(`RC/Public/RenderGraphEvent.h:479`).
  - view마다 다른 자원은 다른 그래프 자원이다. view 사이에 공유하는 자원(shadow atlas·GPU Scene)은 한 번 만들거나 import하고 여러 view의 pass가 읽는다. 공유할지(같은 light/caster revision 등)는 렌더러가 정한다(Renderer 5.1절).
- **완료 기준(RG1 남은 것에 더한다).** `GraphSharedData` 중복 생성·없는 값 필수 조회의 음성 대조와 다른 그래프 handle을 담은 값 거부, `ViewSet` 둘·view 셋인 packet의 `SceneTextureTable` 조회, 검증 실패 그래프의 export 0건과 history 무효 표시, history 반납 뒤 같은 desc 할당의 재사용(장치 생성 0건, fence gate 전에는 재사용 0건), 같은 텍스처의 mip k SRV + mip k+1 UAV pass 통과와 범위가 겹치는 pass 음성 대조, GLES view 기준 mip 경로의 HZB 축소 픽셀 대조(D3D12·Vulkan과 같은 결과), packet의 pass 순서·구간 중첩·trace 표지 기록, RG2에서 view 사이 풀 재사용 기록.

## 8. 렌더 그래프 단계와 완료 기준

공통: CPU 시험은 장치 호출을 기록만 하는 기록 backend로 결정적으로 단언한다. GPU 시험은 검증 계층 오류 0(D3D12 debug layer, Vulkan validation, GL debug output — `ValidationErrorCount`, `Device.h`)과 Sandbox self-test 픽셀 대조(채널 ±8, 2%)다.

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| RG1 | 최소 그래프: 단일 graphics queue, aliasing·병합 끔, 단일 스레드 기록. 자원 생성·import·export, 파라미터 구조체 의존, 점진 culling, 참조 수 수명, 프레임 간 desc 풀과 축출, subresource 상태 추적과 읽기 상태 병합, prologue 즉시 전이, 즉시 실행 모드, 검증, 이벤트 구간 | RB4·RB7·GX1·GX2·GX4·GX5·CP3 | (a) 미사용 체인 제거와 `HasSideEffects`·외부 출력·export 보존 (b) 기대 전이 열 일치, 읽기→읽기 전이 0, UAV→UAV는 생략 표시가 없을 때만 (c) 할당·해제 pass 번호 (d) 즉시·지연 모드 명령 열 동치 (e) 음성 검증: 생산 전 읽기, 한 pass 안 상충 접근, Raster pass의 UAV, `Retire`된 자원 import (f) 풀 축출이 fence 완료 전에는 일어나지 않음(fence gate) (g) D3D12·Vulkan·GLES 미리보기 픽셀 대조 |
| RG2 | 프레임 안 풀 재사용(수명이 겹치지 않는 같은 desc), begin·end 분리 전이, 그래프 안 업로드 | RG1·GX8 | 겹치지 않는 수명의 같은 desc 자원이 실제 객체 하나를 공유, 분리 전이의 begin·end 위치가 기대 열과 일치(native split 없는 backend는 end 하나로 낮춘 열과 일치), 분리 전후 픽셀 동일 |
| RG3 | 병렬 기록, render pass 병합(tiled GPU 형식만) | RG2·CP2·GX11·GX13(병합) | 병렬·직렬 결과 동일, 병합 구간 전이 0, GLES에서 병합 켬·끔 픽셀 동일 |
| RG4 | async compute overlap(fork/join, 수명 연장, 병합과 배타) | RG3·GX1(async compute 표시) | 계획 API06 직렬·비동기 결과 일치, 실제 queue 겹침 계측 |
| RG5 | transient aliasing. 지원하지 않는 backend는 풀 | RG4·GX12 | alias 켬·끔 픽셀 동일, transient 최대 사용량 감소 계측, 검증 오류 0 |
| (결정 R6) | copy queue | — | 결정 뒤 단계 위치를 정한다 |

**구현(2026-11-17, RG1).** 새 모듈 `Engine/Source/Runtime/RenderGraph`(의존 RenderBase·Graphics·TraceCapture, 3절 배치). 계약은 [렌더 기반 구현](../Implementation/RenderBase.ko.md) 9절이다. 선행 가운데 RB4(파라미터 구조체)·GX1·GX2·GX4는 아직 없으므로 범위를 줄여 앞당겼다(사용자 지시 "렌더 그래프도 미리 하자").
- 내부 접근 모델: pass마다 자원 접근 목록(`GraphResourceAccess`: 자원·subresource 범위·접근 상태·`NoUavBarrier` 표시)과 렌더 타깃 하나. 지금은 명시 선언 builder `GraphPassAccesses`가 목록을 만든다. 파라미터 구조체(결정 R4, 9.6절)가 들어오면 구조체 layout을 훑는 수집기가 같은 목록을 만들어 형식 지운 진입점 `RenderGraph::AddPassWithAccesses`로 넣는다(UE도 내부는 접근 목록이다, `RenderGraphBuilder.cpp:2384–2532`). 그래서 culling·수명·전이·검증은 수집 경로와 무관하다.
- 완료 기준 상태:
  - (a) 구현·시험: 점진 culling(부수 효과·스왑체인 타깃·import 자원 쓰기는 AddPass 때, export 마지막 생산자는 Execute 때 살린다), 생산자 체인 보존.
  - (b) 구현·시험: 기대 전이 열(attachment 포함), 읽기 → 읽기 전이 0(연속 읽기를 한 상태로 합쳐 첫 pass에서 한 번), UAV → UAV는 생략 표시가 둘 다 없을 때 UAV barrier, mip 범위별 추적과 연속 mip 묶기, 장치에 내린 열 일치와 기록 장치 검증 오류 0.
  - (c) 구현·시험: 첫·마지막 사용 pass와 참조 수(`GraphLifetime`), 제거된 pass는 수명에 들지 않음.
  - (d) 구현·시험: 동치를 "전이를 뺀 장치 호출 열이 같고, 두 모드 모두 각 pass가 맞는 상태를 보며(기록 장치 검증 오류 0), 최종 상태가 같다"로 정했다. 읽기 병합은 앞을 내다봐야 하므로 즉시 모드는 전이가 더 많다. 병합이 없는 그래프는 전이까지 같다.
  - (e) 구현·시험: 생산 전 읽기, 한 pass 안 상충 접근, Raster pass의 UAV, Retire된 자원 import(lease 실패), 그 밖에 생산 전 export·다른 그래프 handle·pass 종류 위반.
  - (f) 구현·시험: 풀 축출은 마지막 사용 fence가 끝나기 전에는 일어나지 않는다(기록 장치 `HoldCompletions`·`CompleteAll` gate).
  - (g) D3D12(하드웨어·WARP)·D3D11(하드웨어·WARP)·OpenGL ES 미리보기(ES 3.2 profile·GL 4.3 core context)·Vulkan(GTX 1060·lavapipe, 검증 층 켬)에서 Sandbox self-test 픽셀 대조(그래프 텍스처 → SRV 합성, culling, history export → import)와 검증 오류 0(D3D12·Vulkan은 전이 before 불일치 포함).
- 스펙과 다르게 한 점:
  - 의존 선언은 파라미터 구조체가 아니라 명시 접근 목록이다(위). 결정 R4는 그대로이고 수집기가 같은 목록을 만든다.
  - GX5는 최소 범위다: begin·end 분리 전이와 pipe가 없고, Raster pass 렌더 타깃 전이(attachment)는 장치 `BeginOffscreen`·`BeginFrame`이 낸다(GX4 attachment 시작이 기록 안으로 들어오면 그래프가 `Transition`으로 낸다). 만든 자원의 첫 전이 before는 `Unknown`(내용 불필요, 장치 추적 상태)이다.
  - 실행 단위: Raster pass마다 장치 기록 하나를 열고(`BeginOffscreen`은 clear만 한다), Compute·Copy pass는 열린 기록에 이어 쓴다. 열린 기록이 없는 Compute·Copy pass와 attachment load(`GraphLoadAction::Load`)는 `Unsupported`다(GX4).
  - 자원 desc는 지금 Graphics desc 그대로다(형식은 [픽셀 형식](PixelFormat.ko.md) PF1의 `PixelFormat`). 그래프 텍스처는 RenderTarget 용도·번호표 안의 비압축 형식만, import 텍스처는 mip 수를 `GraphImportOptions::mipLevels`로 받는다(장치에 desc 조회가 없다). 풀 byte 추정은 `PixelFormatInfo` 기준 `MipChainBytes`다. GX2(차원·array·샘플 수·버퍼 용도) 뒤에는 풀 키 비교 함수와 desc 검사만 넓히면 된다.
  - 풀 키에 샘플러를 넣었다(7.4절은 제외). 지금 장치는 텍스처 desc의 샘플러로 묶으므로 빼면 다른 샘플러로 읽힌다. 결정 R12의 바인딩 때 샘플러 지정이 들어오면 뺀다.
  - 해제 pass 번호는 계산·진단만 하고 풀 반환은 그래프 끝이다(프레임 안 재사용은 RG2 완료 기준이다).
  - import lease는 `RenderHandleTable`에 두었다(`AcquireLease`·`ReleaseLease`·`TakeReleasedRetire`, RB2의 `DeviceResourceOwner` 전). lease가 남은 handle의 Retire는 장치 Retire를 미루고 렌더 lane이 lane 작업 끝에 잇는다. export는 새 `RenderHandle`로 소유를 넘기고 그 handle의 Retire가 수명을 정한다.
  - 검증은 모든 구성에서 켜져 있고 실패하면 그래프 전체를 버린다(UE는 디버그 빌드 검사). 그래프 설정 변수(CP4 연결: 즉시 모드·검증 전환)는 아직 없다.
  - `GraphSharedData`(7.3절)는 넣지 않았다.
  - GPU 계측: 그래프의 장치 기록 전체를 GPU 프레임 하나로 묶는다(첫 기록에서 `GpuTrace::BeginFrame`, 마지막 기록에서 `EndFrame`). pass마다 `CpuScope`와 `GpuScope`(pass 이름은 정적 `TraceScopeName`)를 둔다(13.3절 스펙과 다른 점 (4) 해소).
  - 렌더 lane 연결: `RenderFrameContext::Arena()`(그 slot의 FrameArena)를 더했다. 그래프 수명 표는 그 arena에 두고, pass 함수(람다)는 그래프 파괴 때 렌더 lane에서 부순다(arena Reset은 게임 lane에서 일어나기 때문이다).
- 남은 것(RG1): 파라미터 구조체 수집기(RB4·SH2·P7), `GraphSharedData`, attachment load·MRT·깊이·렌더 타깃 없는 기록(GX4), 버퍼 용도·텍스처 차원·형식 확장(GX2·픽셀 형식), Vulkan 픽셀 대조, 그래프 설정 변수(CP4), 캡처 도구 수동 확인 기록(13.3절). 렌더러 사용 요구(7.5절: `GraphSharedData` 규칙, mip view와 view 기준 mip, 그래프 구간 `GraphScope`, export 성공 조건)도 RG1에 넣는다. export한 풀 자원의 `Retire`를 장치 해제가 아니라 fence 뒤 풀 반납으로 바꾼다(결정 R15, 지금 구현은 새 `RenderHandle`의 `Retire`가 장치 자원을 놓는다).

## 9. 셰이더 런타임

셰이더 시스템 스펙은 오프라인 컴파일·쿠킹·바인딩 규약·라이브러리 형식과 조회 키까지 정한다. RenderCore의 런타임 객체는 거의 없다. 이 절이 그 빈자리를 채우고, 셰이더 시스템 문서를 고쳐야 할 곳은 9.9절에 모은다.

### 9.1 UE 5.8 흐름

| 단계 | UE 5.8 | 근거 |
|---|---|---|
| 등록 | 셰이더 타입 하나 = 단계 하나, 정적 등록. 타입 종류는 Global·Material·MeshMaterial 등 7종. 타입 함수표에 컴파일 거르기와 PSO 미리 준비 여부 | `RC/Public/GlobalShader.h:433`, `RC/Public/Shader.h:1219–1226, 1256–1257` |
| 퍼뮤테이션 | 차원 도메인. specialization constant 차원도 있다 | `RC/Public/ShaderPermutation.h:272, 381–387, 636, 651` |
| 셰이더 객체 | 바인딩 표·파라미터 정보·UB 목록·타입 참조·자원 번호. 바인딩 표는 셰이더 생성자에서 컴파일 결과의 파라미터 지도로 만들어 셰이더 맵과 함께 저장(오프라인) | `Shader.h:733–785, 950–995`, `RC/Public/ShaderParameterStruct.h:50–56` |
| 셰이더 맵 | 내용(셰이더 객체·바인딩·타입 → 셰이더 표, memory image) / 포인터 표 / 코드 / 장치 셰이더 배열. 쿠킹된 경우 코드는 라이브러리에서 붙이고, 없으면 Error 로그를 남기고 내용을 확정하지 않는다 | `Shader.h:2535–2636`, `RC/Private/ShaderMap.cpp:291–376` |
| 엔진 셰이더 맵 로드 | 셰이더 타입 초기화 직후. 쿠킹 실행은 캐시 파일과 "Global" 라이브러리. 없고 컴파일도 안 되면 창 지원 플랫폼은 오류 대화상자 뒤 종료 요청, 나머지는 Fatal | `Launch/Private/LaunchEngineLoop.cpp:3301–3307`, `Engine/Private/ShaderCompiler/ShaderCompiler.cpp:5314–5316, 5518–5528, 5715–5724`, `RC/Private/ShaderLibrary/ShaderCodeLibrary.cpp:2272` |
| 장치 셰이더 | 처음 쓸 때 만든다(원자 포인터, 잠금 밖 생성·경쟁에서 진 쪽 해제, 생성 뒤 코드 해제). 로드 때 모두 만드는 선택(기본 끔) | `Shader.h:405–417`, `RC/Private/ShaderResource.cpp:566–710`, `ShaderCompiler.cpp:532–536` |
| 라이브러리 API | 열기·닫기·자원 로드·preload·셰이더 생성. 셰이더 맵 hash → 셰이더 인덱스 범위로 묶는다 | `RC/Public/ShaderCodeLibrary.h:213–288`, `RC/Public/ShaderCodeArchive.h:48–79, 206–230` |
| 파라미터 메타데이터 | 구조체 매크로가 멤버별 정렬 검사와 멤버 기술을 만든다. 용도는 파라미터 구조체·UB·데이터 기반 UB. UB는 생성 HLSL을 include하고, 파라미터 구조체는 생성 HLSL 없이 이름으로 매칭한다(느슨한 전역, 형식에 따라 루트 cbuffer로 옮김) | `RC/Public/ShaderParameterMacros.h:1308–1495`, `RC/Public/ShaderParameterMetadata.h:139–176`, `RC/Private/ShaderParameters.cpp:85–99`, `RC/Private/ShaderParameterStruct.cpp:251–330`, `RC/Private/ShaderParameterParser.cpp:1145–1290` |
| 그리기 때 설정 | 바인딩 표대로 상수 칸·자원 칸을 묶음 객체에 넣어 한 번에 올린다. Development에서 구조체 layout hash가 다르면 실패 | `ShaderParameterStruct.cpp:520–527, 912–990`, `RHI/Public/RHIShaderParameters.h:246–330` |
| UB 런타임 | 수명 3종, layout 객체, 렌더 자원 래퍼. 정적 slot(최대 255)을 명령 목록에 묶고 셰이더별 slot 표로 연결. OpenGL은 slot별로 저장했다가 파이프라인 설정 때 셰이더 slot 표대로 묶고, 그리기 때 UB 고유 ID를 비교해 바뀐 것만 packed 배열에 복사 | `RHI/Public/RHIDefinitions.h:517–525, 721`, `RC/Public/UniformBuffer.h:20–77`, `ShaderParameterMacros.h:1511–1590`, `RHI/Public/RHIResources.h:823–835`, `D3D12RHI/Private/D3D12Commands.cpp:180–211, 420–427`, `OpenGLDrv/Private/OpenGLCommands.cpp:896–909, 2574–2589`, `OpenGLDrv/Private/OpenGLShaders.cpp:3289–3370` |
| PSO | 초기화자(정점 선언·셰이더·상태·RT 형식). 캐시는 RHI 모듈에 있고 비동기 미리 준비·캐시 확인을 둔다. 기록 파일 캐시와 시작 precompile | `RHIResources.h:4440, 4655, 4835–4842`, `RHI/Public/PipelineStateCache.h:165–280`, `RHI/Public/PipelineFileCache.h:314–405`, `RC/Public/ShaderPipelineCache.h:4–66` |

### 9.2 셰이더 집합

- **내용.** 셰이더 라이브러리(셰이더 시스템 12.2절)에서 연 항목마다: 키(셰이더 타입, 정점 입력 타입, 퍼뮤테이션 ID, 단계 — 셰이더 시스템 12.2절 키에 SX4의 정점 입력 타입을 더한 것), 코드 참조, 정규화 바인딩 layout(`ShaderBindingLayout`, 셰이더 시스템 7.2절 3항), 파라미터 레이아웃 서명, 파라미터 바인딩 지도(9.3절), 장치 셰이더 자리.
- **표현.** UE frozen memory image를 들이지 않는다. 정규화 표는 Core Archive·Record로 읽는다. memory image의 이득에 대한 Epic의 설계 근거는 확인하지 못했고(미확인), 로드 비용이 문제가 되면 측정 M3로 다시 본다.
- **수명.** 셰이더 집합은 `DeviceResourceOwner`다. 쓰는 쪽(파이프라인 캐시 항목, 기록 packet)은 집합의 lease를 잡는다. `Retire`된 집합은 새 lease와 새 장치 셰이더 생성을 막는다.
- **장치 셰이더 생성.** 처음 쓸 때 만든다. 만드는 스레드는 GX11 규칙을 따른다: 스레드 안전 생성 backend는 요청한 스레드에서, OpenGL ES는 렌더 lane 작업으로. 경쟁 시 생성이 두 번 일어날 수 있으나 게시는 한 번이고 진 쪽은 `Retire`한다(UE와 같다, `ShaderResource.cpp:579–657`). 만든 뒤 코드 바이트를 놓는다. 로드 때 일괄 생성 선택을 콘솔 설정으로 두며 병렬 일괄 생성은 CP2를 쓴다.
- **런타임 확인 기능(개정 16).** 셰이더 시스템 7.2절 7항의 런타임 확인 기능(`vertexViewportLayerOutput`)을 요구하는 셰이더는 라이브러리 항목에 그 기능 비트가 있다(셰이더 시스템 12.2절). 셰이더 집합은 장치 셰이더를 만들기 전에 이 비트를 `DeviceCaps.features`와 대조하고, 없으면 만들지 않고 `Unsupported`를 돌려준다. 그 셰이더를 쓰는 파이프라인 캐시 항목도 만들지 않는다. 완결성 검사(아래)는 장치와 무관하게 그 항목이 라이브러리에 있는지 본다(쿠킹은 형식 기준이다). 호출자는 능력을 먼저 확인해 대체 경로를 고른다.
- **엔진 셰이더 집합과 완결성 검사.** 렌더 lane 시작 단계(5.5절)에서 엔진 셰이더 집합을 연다. 형식마다 공급 경로가 둘이다.
  - **쿠킹 라이브러리 형식**(셰이더 시스템 12.3절 엔진 내장 라이브러리): 등록된 엔진 셰이더 타입 × 정점 입력 타입 × 그 형식에서 컴파일되는 퍼뮤테이션(P14 거르기 결과) 전체가 라이브러리에 있는지 검사한다. 하나라도 없으면 시작이 `IncompatibleCook`으로 실패한다. UE는 엔진 셰이더 맵이 없으면 종료하고(창 지원 플랫폼은 오류 대화상자 뒤 종료 요청, 9.1절) 개별 셰이더는 로드 때 오류다. Rocks는 정적 등록으로 전체를 알 수 있으므로 시작 때 결정적으로 실패시킨다.
  - **필요 시 컴파일 형식**(`GLES31_PC`, 개발 트리에서 내장 라이브러리가 없는 형식, 셰이더 시스템 12.3절): Runtime 모듈은 Developer 셰이더 컴파일러에 의존할 수 없으므로 RenderBase가 **셰이더 공급자** interface를 정의하고, Development 실행 파일이 Developer/ShaderCompiler 구현을 등록한다(SX14). 집합은 "공급 대기" 상태로 열리고, 공급자가 엔진 셰이더 전체를 컴파일해 완료 통지를 보내면 같은 완결성 검사를 한 뒤 `Ready`가 된다. 공급자가 없는 실행 파일(Release)에서 이 형식이 선택되면 `UnsupportedCapability`다.
- **타입 있는 셰이더 참조** `TypedShader<T>`: (셰이더 타입, 집합)의 쌍. 렌더 그래프 pass와 그리기 유틸리티가 이것으로 셰이더를 고른다.
- **라이브러리 런타임 API.** 열기·닫기, 엔진 → 프로젝트 순 조회(같은 키가 둘 이상이면 쿠킹 오류), 열 때 레이아웃 서명 검사(9.3절). 플러그인 라이브러리 분할과 묶음 단위 preload는 셰이더 시스템 12.3절대로 후속이다.

### 9.3 파라미터 바인딩 지도와 묶음 설정

- **바인딩 지도.** 파라미터 구조체의 멤버 오프셋 → (단계, 종류, 단계 안 순번)이다. 상수 멤버는 (상수 버퍼 순번, 바이트 오프셋, 크기), 자원 멤버는 (종류, 순번), 상수 블록 참조 멤버는 (상수 버퍼 순번), 그래프 자원 멤버는 (종류, 순번, 그래프 자원 표시)다.
- **만드는 시점.** 셰이더 집합을 열 때 라이브러리의 바인딩 layout과 파라미터 구조체 스키마로 만든다(결정 R11). 스키마는 MetaSchema `TypeOf<T>()`(`Meta/GeneratedType.h`)로 얻는다. RenderBase는 MetaSchema에 의존하고 ObjectRuntime에는 의존하지 않는다(계획 3.1절 렌더 층 "Object 없음"). 스키마를 오래 붙잡는 경우의 타입 lease 수명은 집합 수명과 같게 한다. 만드는 비용은 측정 M3로 확인하고, 문제가 되면 라이브러리 부분으로 굽는다(UE는 오프라인에서 만들어 셰이더 맵에 굽는다).
- **레이아웃 서명 런타임 검증.** 셰이더 시스템은 레이아웃 서명을 job 키에만 넣는다(10절 8항, 13절). 그러면 실행 파일과 쿠킹 라이브러리의 빌드가 어긋날 때 잘못된 오프셋으로 조용히 올라간다. 라이브러리 항목에 서명을 넣고(SX6), 셰이더 집합을 열 때 실행 파일의 구조체 서명과 다르면 `IncompatibleCook`이다. UE는 그리기 때 Development에서만 검사한다(`ShaderParameterStruct.cpp:520–527`). Rocks는 열 때 모든 구성에서 검사한다.
- **묶음 설정** `ParameterBatch`: 상수 칸·자원·상수 블록 참조를 모아 장치에 한 번에 올리는 기록 객체(GX6). Development에서 null 자원·종류 불일치를 검증한다. P7(셰이더 시스템 13절) 전에는 손으로 쓴 레이아웃 표(7.2절 5항)를 입력으로 쓴다.
- **그리기별 바인딩(개정 16, Renderer RQ7).** 그리기마다 바뀌는 상수·자원(머티리얼 파라미터와 텍스처, 정적 조명 `PrecomputedLighting`)은 공유 slot이나 pass 구조체가 아니라 그리기 항목(`MeshDrawItem`)이 가진 바인딩으로 `ParameterBatch`에 넣는다. 상수는 상수 블록 참조, 자원은 그리기별 자원 구조체 `DrawResourceStruct`(9.6절)다. 그래프 의존은 그리기마다 선언하지 않는다. pass가 그 pass의 그리기들이 쓸 수 있는 import 자원 집합(예: 상주 라이트맵 전체, 스트리밍 바인딩 snapshot의 텍스처)을 접근만 선언(9.6절 표)으로 한 번 선언한다. 그래서 pass 구조체가 그리기 수만큼 커지지 않고, 그래프는 그 집합을 pass 동안 SRV 상태로 둔다. 머티리얼 텍스처가 이미 쓰는 경로와 같다(Renderer 14.5절). UE도 라이트맵 자원 구조체와 정적 조명 상수를 그리기 바인딩으로 그리기마다 넣는다(`Renderer/Private/LightMapRendering.cpp:312–333, 346–360`, 구조체 `Engine/Public/SceneManagement.h:725`).

### 9.4 상수 블록

- 수명 3종: `PerDraw`(그리기 한 번), `PerFrame`(프레임 slot 하나, slot GPU 완료 때 회수), `Persistent`(명시 `Retire`).
- backend 표현:
  - D3D12·Vulkan·Metal·D3D11: GPU 버퍼. `PerDraw`·`PerFrame`은 업로드 ring(GX8)에서 할당한다.
  - OpenGL ES: CPU 사본(셰이더 시스템 7.2절 4항 에뮬레이션). `PerDraw`·`PerFrame` 사본은 그래프가 pass를 실행할 때까지 살아 있어야 하므로 프레임 arena(CP3)에 둔다.
- **GLES 업로드 판단.** GL uniform은 program 상태다. 그래서 "버퍼가 바뀌었는가"가 아니라 (program, 상수 버퍼 순번)마다 마지막으로 올린 (상수 블록 identity, version)을 기억하고, 그리기 때 다르면 복사 표대로 `glUniform4fv/4iv/4uiv`로 올린다. 반례: 블록 B를 program P1에 올린 뒤 P2로 바꾸면 B가 그대로여도 P2에는 올려야 한다. 공유 slot도 같다. UE도 그리기 때 UB 고유 ID를 비교해 바뀐 것만 복사한다(9.1절 UB 런타임 행).
- layout 객체: 파라미터 구조체 스키마에서 만든다(크기·멤버 오프셋·자원 멤버 목록·그래프 자원 멤버 목록). 렌더 그래프가 이 목록만 훑어 의존을 만든다(UE layout이 그래프 자원 오프셋을 미리 계산해 두는 것과 같다, `RHI/Public/RHIResources.h:1183–1245`).

### 9.5 공유 상수 slot

뷰·장면처럼 pass 전체에 한 번 묶는 상수 블록이다. 렌더 그래프·장면 렌더러 단계에서 필요하다.
- slot은 정적 등록(이름, 파라미터 구조체 identity)이다. 최대 수는 형식 정의 상한 안이다.
- 셰이더 쪽: 컴파일러가 셰이더마다 정한 상수 버퍼 번호와 slot을 잇는 표가 필요하므로 `ShaderBindingLayout`의 상수 버퍼 항목에 **파라미터 구조체 identity**를 둔다(SX8).
- 명령 쪽: 기록 객체에 slot 표를 묶고, 파이프라인 설정 때 셰이더의 slot 표로 적용한다(UE와 같다, 9.1절 UB 런타임 행). GLES도 같은 방식이고 업로드는 9.4절 판단을 따른다(UE OpenGL과 같다).
- **렌더러 slot(Renderer RQ7, 개정 15·16).** 렌더러가 등록하는 공유 상수 slot은 아래 다섯이다(Renderer 15.2절). 선언(이름·파라미터 구조체 identity·수명)은 장치 없는 `RendererShaders`(Renderer 3.2절)에 정적으로 둔다. 쿠커는 이 선언으로 바인딩 layout의 구조체 identity를 맞추고(SX8), RenderBase slot 표는 시작 때 정적 등록 표로 받는다(10.2절 정점 입력 타입 등록과 같은 시점, 늦은 등록은 거부).

  | slot | 상수 내용(Renderer 15.2절) | 수명 | 묶는 단위 | 짝 공유 자원 구조체(9.6절) |
  |---|---|---|---|---|
  | `View` | 행렬(현재·이전, jitter 유무), 카메라 이동 보정 상위·하위, render 크기, 시간, 노출, mip bias, 정적 간접 배율 둘 | `PerFrame` | view | — |
  | `Scene` | 인스턴스 수 | `PerFrame` | `ViewSet`(장면 snapshot 하나) | `SceneResources`: GPU Scene typed buffer, 인스턴스 번호 목록(10.5절) |
  | `ForwardLight` | grid 크기·Z 인자, local 광원 수, directional 목록(최대 4) | `PerFrame` | view | `ForwardLightResources`: 광원 데이터·grid typed buffer |
  | `Shadow` | CSM 행렬·분할, atlas 크기 | `PerFrame` | view | `ShadowResources`: shadow atlas(비교 샘플러 짝) |
  | `Fog` | height fog 인자 | `PerFrame` | view | `FogResources`: aerial perspective LUT |

  - `PrecomputedLighting`(정적 조명 상수, 라이트맵·프로브 텍스처)은 개정 16에서 slot에서 뺐다. 그리기마다 바뀌므로 그리기별 바인딩(9.3절)의 상수 블록 하나와 `DrawResourceStruct` 하나다.
  - `SceneTextures`는 상수 부분이 없어 slot이 아니다. 공유 자원 구조체 `SceneTextureResources`(깊이·GBuffer·AO·velocity·HZB)만 있다.
  - **상수 버퍼 예산(개정 16).** pass 셰이더 하나가 쓰는 상수 버퍼는 공유 slot 5 + `PrecomputedLighting` 1 + 머티리얼 상수 블록 1 + 머티리얼 파라미터 컬렉션 최대 2(머티리얼 시스템 9.7절, 각각 cbuffer) + pass 파라미터 구조체 1 = 최대 10이다. 형식 정의 `constantBuffers`(GLES·Vulkan·Metal 12, D3D11 13, D3D12 14, 셰이더 시스템 12.1절) 안이고, 루트 상수는 따로다(셰이더 시스템 7.2절 2항). 이 수는 job 형식 상한으로 워커가 검사한다(셰이더 시스템 7.2절 4항).
  - **GLES uniform 예산.** GLES는 상수 버퍼와 루트 상수가 모두 uniform `vec4` 배열이므로(9.4절), PS 에뮬레이션 `vec4` 224(ES 3.2 최소 보장)를 공유 slot·`PrecomputedLighting`·머티리얼 블록·컬렉션·pass 상수·루트 상수(8 `vec4`)의 **합**으로 검사한다. 렌더러 쪽 합은 구조체를 정한 뒤 정하고(Renderer 15.2절 추정 약 100), 머티리얼 블록과 컬렉션 크기 상한은 그 나머지 안에서 정한다(머티리얼 시스템 9.7절).
  - HLSL 이름은 slot 이름과 같은 cbuffer 이름이다(SX1 "파라미터 구조체 하나 = 이름 있는 cbuffer 하나"). 원문은 공용 `.rsh`의 선언을 include한다. UE도 정적 slot을 이름으로 묶는다(`RC/Public/ShaderParameterMacros.h:1511–1590`).
  - 렌더러는 `ViewSet`마다 `Scene`을, view마다 나머지 slot을 기록 객체의 slot 표에 다시 묶는다(7.5절 선언 순서). GLES는 9.4절의 (program, 순번)별 identity·version 판단으로 바뀐 것만 올린다.
  - 머티리얼 파라미터 컬렉션의 동적 slot(머티리얼 시스템 MRB4)은 이 정적 표 밖이며 머티리얼 단계에서 정한다. 위 예산에는 넣는다.

### 9.6 파라미터 구조체와 렌더 그래프

셰이더 시스템 13절은 파라미터 구조체 멤버를 "셰이더 전용 정렬 타입 + 셰이더 자원 값 타입"으로 닫아 두었다. 렌더 그래프가 같은 구조체를 의존 선언으로 쓰려면(결정 R4) 멤버 종류가 다음과 같아야 한다.

| 멤버 종류 | 뜻 | UE 5.8 대응 |
|---|---|---|
| 상수(정렬 타입) | 상수 버퍼 멤버 | `SHADER_PARAMETER` |
| 장치 자원(텍스처·버퍼 SRV·UAV) | 그래프 밖 영구 자원. 쓰기 자원이 있으면 그 pass는 외부 출력(자동 `HasSideEffects`) | `SHADER_PARAMETER_TEXTURE`·`_SRV`·`_UAV`, `RC/Private/ShaderParameterMetadata.cpp:652–669, 822` |
| 그래프 자원(텍스처·버퍼 SRV·UAV) | 그래프가 추적하는 자원 | `SHADER_PARAMETER_RDG_TEXTURE`·`_SRV`·`_UAV`·`_RDG_BUFFER_*`, `ShaderParameterMacros.h:1697–1760` |
| 샘플러 지정(텍스처 멤버에 붙음) | 그 텍스처를 읽을 샘플러. 없으면 텍스처 desc의 기본 샘플러 | `SHADER_PARAMETER_SAMPLER`, `:1677` |
| 접근만 선언 | 셰이더에 묶지 않고 의존만 선언(복사·간접 인자 등) | `RDG_TEXTURE_ACCESS`·`RDG_BUFFER_ACCESS`, `:1868–1884` |
| 렌더 타깃 slot 묶음 | 색 MRT·깊이 attachment, load/store, mip·slice | `RENDER_TARGET_BINDING_SLOTS`, `:1900` |
| 상수 블록 참조 | 별도 상수 버퍼(그래프 소유 또는 영구) | `SHADER_PARAMETER_RDG_UNIFORM_BUFFER` `:1768`, `SHADER_PARAMETER_STRUCT_REF` `:1854` |
| 펼쳐 넣기 | 다른 파라미터 구조체를 평탄화해 포함 | `SHADER_PARAMETER_STRUCT_INCLUDE`, `:1840` |

규칙:
- **샘플러(결정 R12).** HLSL 쪽 텍스처·샘플러 짝 규칙(셰이더 시스템 7.2절 9항, 텍스처 `X` ↔ `XSampler` 1:1)은 그대로 둔다. 생성기는 텍스처 멤버마다 짝 `XSampler` 선언을 자동으로 낸다. 바뀌는 것은 샘플러 **값**의 출처다: 텍스처 desc의 샘플러는 기본값이고, 파라미터 멤버의 샘플러 지정이 있으면 그것을 쓴다. 그래서 같은 렌더 타깃을 pass마다 point·linear로 다르게 읽을 수 있다. 깊이 비교(그림자) 샘플러는 `SamplerComparisonState XSampler`로 짝을 맺는다(SX13). UE는 샘플러를 별개 멤버로 두고 공유도 허용한다. 한 샘플러를 여러 텍스처가 공유하는 것과 정적 샘플러는 셰이더 시스템대로 후속이다.
- "상수 블록 참조 = 별도 cbuffer", "펼쳐 넣기 = 평탄화"로 정하면 셰이더 시스템 7.2절 5항의 금지(cbuffer 안 구조체 배열·2단 중첩)와 충돌하지 않는다.
- 그래프 수명 구조체는 프레임 arena에 할당하고 pass에 넘긴 뒤 바꾸지 않는다.
- 셰이더가 쓰지 않는 멤버는 바인딩 layout으로 걸러 의존에서 뺀다(UE `RC/Private/RenderGraphUtils.cpp:22–101`). 그래서 바인딩 지도에 멤버 오프셋이 함께 있어야 한다(9.3절).
- **공유 자원 구조체 `SharedResourceStruct`(Renderer RQ7, 개정 15·16).** 공유 상수 slot은 상수 블록이라 자원을 담지 못한다(GLES는 uniform `vec4` 배열, 9.4절). 그래서 렌더러 자원은 상수 부분(9.5절 slot)과 자원 부분으로 나누고, 자원 부분은 공유 자원 구조체로 정의해 그 자원을 쓰는 pass의 파라미터 구조체가 포함한다.
  - **정의.** 그래프 자원·장치 자원·샘플러 지정 멤버만 가진 파라미터 구조체다. 상수 멤버와 렌더 타깃 slot은 넣지 않는다(상수는 slot 쪽, 렌더 타깃은 pass 쪽이다).
  - **포함.** 같은 구조체를 여러 pass 구조체가 멤버 하나로 포함한다. 값은 handle 복사다. 렌더러는 구조체 값을 그래프 구성 때 한 번 채우고(장면 텍스처는 `GraphSharedData`의 `SceneTextureTable`에서, 7.5절) pass마다 복사해 넣는다.
  - **이름(개정 16).** 생성 HLSL 전역 자원 이름은 `<포함 멤버 이름>_<자원 멤버 이름>`이다. pass 구조체가 `SceneTextureResources` 타입 멤버를 `SceneTextures`라는 이름으로 포함하면 `SceneTextures_Depth`, 짝 샘플러는 `SceneTextures_DepthSampler`다(셰이더 시스템 7.2절 9항). 공유 자원 구조체는 렌더러가 정한 표준 멤버 이름(`SceneTextures`·`SceneResources`·`ForwardLight`·`Shadow`·`Fog`)으로만 포함해 모든 셰이더에서 같은 자원이 같은 이름이다. 접두어는 UE 중첩 구조체의 `멤버_` 규칙과 같다(`RC/Private/ShaderParameterStruct.cpp:123–130`). UE의 펼쳐 넣기(`SHADER_PARAMETER_STRUCT_INCLUDE`)는 접두어 없이 평탄화하지만(`:100–107`), Rocks는 한 pass에 둘을 포함해도 이름이 겹치지 않게 접두어를 쓴다. 그래도 겹치면 생성기 오류다.
  - **의존은 쓰는 멤버만.** 바로 위 미사용 멤버 거르기가 포함 멤버에도 적용된다. `SceneTextureResources`를 포함한 pass가 깊이만 읽으면 깊이에만 의존한다. 깊이를 attachment로 쓰는 pass가 같은 구조체를 포함해도 셰이더가 깊이를 읽지 않으면 상충이 아니고, 읽으면 렌더 그래프가 한 pass 안 상충 접근으로 거부한다(7.2절, Renderer 6.2절 순환 금지).
  - **단계 상한.** 포함 멤버도 그 단계의 SRV 텍스처 수에 든다. 렌더러 예산표(Renderer 7.4절)는 셰이더가 실제로 읽는 멤버로 센다.
  - **영구 자원.** 그래프 밖 자원(스트리밍 텍스처 등)은 import한 그래프 자원으로 넣는다(Renderer 15.1절). 구조체 값의 수명은 그래프 수명이다.
  - **view마다.** view가 여럿이면 view마다 다른 값을 담은 pass 구조체가 따로 생긴다(7.5절).
- **그리기별 자원 구조체 `DrawResourceStruct`(개정 16).** 그리기마다 바뀌는 자원(라이트맵·프로브 텍스처 등 `PrecomputedLighting` 자원)을 담는 구조체다. 멤버 종류는 공유 자원 구조체와 같되 그래프 자원은 import 자원만 가리킨다. pass 구조체에 넣지 않고 그리기 항목의 바인딩(9.3절)으로 그리기마다 묶는다. 그래프 의존은 pass가 그 그리기들이 쓸 수 있는 import 자원 집합을 접근만 선언으로 한 번 선언한다(9.3절). HLSL 이름 규칙은 위와 같다(표준 멤버 이름 `PrecomputedLighting`).

### 9.7 파이프라인 캐시

- 파이프라인 객체는 셰이더(VS·PS 또는 CS), 정점 배치, 래스터·깊이·블렌드 상태, 렌더 타깃 형식·샘플 수, 기본도형 종류를 담는다. 키는 그 내용과 셰이더 집합 epoch이며 계획 14.1절 PSO 캐시 키 항목(셰이더 hash·root signature·형식·샘플 수)을 포함한다. 키 hash는 Core `Core/Hash.h`, 디스크에 남기는 키는 SHA-256을 쓴다(공용 기능 중복 금지). root signature 캐시(셰이더 시스템 7.2절 4항)와 같은 계층에 둔다.
- 항목은 셰이더 집합의 lease를 잡는다. 집합이 `Retire`되면 그 집합 epoch의 항목도 함께 `Retire`한다.
- UE는 이 캐시를 RHI 모듈에 둔다(`RHI/Public/PipelineStateCache.h`). Rocks는 RenderBase에 둔다: Graphics는 만들기만 하고, 캐시·수명·통계는 렌더 쪽 일이다.
- `PipelineDesc`의 원문 경로는 SH2에서 없어진다(셰이더 시스템 결정 9).
- 후속: 비동기 미리 준비(`PipelinePrewarm`, 정점 입력 타입의 정적 배치를 쓴다, 10.2절), 사용 기록 파일과 시작 precompile, 드라이버 캐시 연계.

### 9.8 셰이더 교체

셰이더 시스템 SH4(감시 재컴파일·PSO 교체)와 계획 14.1절 "hot reload 실패 시 이전 검증 버전 유지"를 받는 규칙이다.
- 새 셰이더는 새 셰이더 집합(새 epoch)으로 연다. 완결성·서명 검사를 통과해야 교체 후보가 된다.
- 교체는 프레임 경계의 lane 작업이다. 이전 집합은 `Retire`되고, 진행 중 프레임은 이전 집합 lease로 끝까지 그린다. 파이프라인 캐시 항목은 epoch 키 때문에 섞이지 않는다.
- 검사가 실패하면 이전 집합을 유지하고 진단을 낸다.

### 9.9 셰이더 시스템 문서 개정(SX)

| ID | 셰이더 시스템 위치 | 개정 | 근거 |
|---|---|---|---|
| SX1 | 7.2절 1항(이름 없는 전역 uniform 금지) | 파라미터 구조체 하나 = 이름 있는 cbuffer 하나라는 규칙, 그 cbuffer와 루트 상수의 관계, 구조체 안 자원 멤버의 HLSL 생성 규칙(전역 `Texture2D X; SamplerState XSampler;`는 `$Globals`가 아니므로 1항 위반이 아님)을 적는다 | UE는 파라미터 구조체 상수를 느슨한 전역으로 쓰고 이름으로 매칭한다(`ShaderParameterStruct.cpp:251–330`). Rocks는 생성 cbuffer라 모순은 없으나 규칙이 비어 있다 |
| SX2 | 7.2절 2항(루트 상수) | 파라미터 구조체 상수가 루트 상수로 가는지 cbuffer로 가는지 정하는 규칙(결정 R7). Graphics `SetConstants(DrawConstants)`를 파이프라인 layout이 아는 크기의 바이트로 일반화 | UE의 셰이더별 상수 cbuffer(`ShaderParameterMetadata.h:173–176`)와 루트 상수(4 DWORD)는 다르다 |
| SX3 | 13절(P7) | 9.6절 멤버 종류 표와 규칙(그래프 자원·샘플러 지정·접근 선언·렌더 타깃 slot·상수 블록 참조·펼쳐 넣기·샘플러 짝 자동 생성). `ValueKind` 추가 항목이 바뀐다. 셰이더 시스템 결정 4와 같은 기한(P14 전) | 정하지 않으면 레이아웃 서명 정의와 `ValueKind`를 다시 고쳐야 한다 |
| SX4 | 3절(셰이더 맵 키), 12.2절(셰이더 타입·키) | 정점 입력 타입 축(10.2절): 런타임 조회 키 (셰이더 타입, 정점 입력 타입, 퍼뮤테이션 ID, 단계), 셰이더 맵 키에도 정점 입력 타입(고정 가상 경로의 include 해석이 달라지므로), 거르기는 셰이더 타입 조건 AND 정점 입력 타입 조건 | UE `RC/Public/Shader.h:644–671`, `Engine/Private/Materials/MeshMaterialShader.cpp:178–213`. **반영(셰이더 시스템 개정 16, 12.2절)**: 조회 키·라이브러리 항목 키에 정점 입력 타입, 셰이더 타입 거르기 AND 정점 입력 타입 거르기 |
| SX5 | 12.2절(라이브러리 형식) | 항목 표에 묶음 id 열(엔진 셰이더 = 0, 머티리얼은 셰이더 집합 id). 라이브러리 형식 버전 1을 확정하기 전에 넣는다 | UE 라이브러리는 셰이더 맵 hash → 셰이더 인덱스 범위로 묶는다(`ShaderCodeArchive.h:48–51, 221–230`) |
| SX6 | 12.2절 | 항목마다 파라미터 레이아웃 서명. 열 때 검사(9.3절) | — |
| SX7 | 12.2·12.3절 | 엔진 셰이더 집합 완결성 검사(쿠킹 라이브러리 형식은 시작 때, 필요 시 컴파일 형식은 공급 완료 뒤)를 완료 기준으로(9.2절) | — |
| SX8 | 7.2절 3항(`ShaderBindingLayout`) | 상수 버퍼 항목에 파라미터 구조체 identity, 자원 항목에 파라미터 구조체 멤버 오프셋 | 9.5·9.6절 |
| SX9 | 7.2절 7항(정점 입력) | `ATTRIBUTEn` 상한 n ≤ 15(16개), 정점 배치 ⊇ 셰이더 입력 마스크 검사, 형식 기본 타입과 셰이더 입력 타입 일치 검사(정규화 → float, 정수 → int·uint, GLES 정수 attribute는 `glVertexAttribIPointer`) | 10.2절. 16은 Vulkan `maxVertexInputAttributes` 최소 보장(Vulkan 사양 Required Limits)과 OpenGL ES 3.x `GL_MAX_VERTEX_ATTRIBS` 최소값(glGet 참조 "must be at least 16")이다. UE는 선언당 17개(`RHI/Public/RHIDefinitions.h:269`) |
| SX10 | 16절 SH2 | SH2 완료 기준에 지연 생성 경쟁 시험(게시 1회, 명시 gate), 완결성 음성 대조, 서명 불일치 음성 대조, 파이프라인 캐시 적중 계수를 더한다. SH2 범위는 결정 R8 | — |
| SX11 | 6절 4항(구현 순서) | 셰이더 런타임 객체(RB3)를 SH2 안에 넣는다(결정 R8 권장안) | — |
| SX12 | 2절(SM6 기능) | specialization constant 차원은 후속으로 적는다 | UE `ShaderPermutation.h:636, 651` |
| SX13 | 7.2절 9항(텍스처·샘플러 짝) | 짝 상대로 `SamplerComparisonState XSampler`를 허용한다. 샘플러 값은 파라미터 멤버 지정 또는 텍스처 desc 기본값(결정 R12). **A0-G 착수 전**에 AssetSystem A0-G 샘플러 확장에 비교 함수를 넣는다 | 계획 11.1절 그림자 필수, `GraphicsTypes/Public/GraphicsTypes/Sampler.h:42–52`에 비교 모드 없음 |
| SX14 | 12.3절(`GLES31_PC`·필요 시 컴파일), 16절 SH1 | 셰이더 공급자 interface(9.2절)를 SH1 산출물로. Development 실행 파일만 등록, Release 정책 검사(9절)는 그대로 | Runtime이 Developer에 의존할 수 없다 |

### 9.10 완료 기준

- RB3(셰이더 집합): 첫 사용 생성 경쟁(두 스레드가 gate로 동시에 요청해도 게시 1회, 진 쪽 `Retire`), 완결성 검사 음성 대조(타입 하나를 뺀 라이브러리로 시작 실패), 서명 불일치 음성 대조, 공급자 경로(공급 대기 → 완료 → 검사 → `Ready`, 공급자 없는 실행 파일의 `UnsupportedCapability`), 라이브러리 조회 순서와 중복 키 오류, `Retire`된 집합의 새 장치 셰이더 생성 거부.
- RB4(바인딩·상수 블록·파이프라인 캐시): 셰이더 시스템 7.3절 (a)·(d)를 런타임 경로로, 파이프라인 캐시 적중·미적중 계수, 집합 `Retire` 때 캐시 항목 회수, `PerFrame` 상수 블록의 slot 회수(fence gate), GLES 업로드 판단(program 전환 뒤 같은 블록을 다시 올리는지, 바뀌지 않은 블록은 올리지 않는지 호출 수 계측, 7.3절 (i)), null·종류 불일치 검증의 음성 대조, 교체(9.8절) 성공·실패.

## 10. 정점 입력 타입

### 10.1 UE 5.8 방식

- 클래스마다 정적 타입 객체(이름·`.ush` 원문 경로·기능 플래그 22종·파라미터 생성·layout·정점 요소·컴파일 거르기·컴파일 환경 수정)를 등록한다(`RC/Public/VertexFactory.h:135–160, 561–578`). 생성자는 확장자 검사·이름 hash 등록을 하고, 셰이더 타입 초기화 뒤에 생기면 check 실패다(`RC/Private/VertexFactory.cpp:102–161`).
- 메시 셰이더는 셰이더 타입과 정점 입력 타입이 둘 다 허용해야 컴파일된다(`Engine/Private/Materials/MeshMaterialShader.cpp:178–213`). 셰이더 키에 정점 입력 타입이 별도 축이다(`RC/Public/Shader.h:644–671`).
- 공용 메시 셰이더는 고정 가상 경로 `/Engine/Generated/VertexFactory.ush`를 include하고, 컴파일 환경이 이를 그 타입의 `.ush`로 잇는다(`VertexFactory.h:457–476`). `.ush`는 고정 함수 인터페이스(입력 구조체·중간값·월드 위치·보간값)를 구현한다(`Engine/Shaders/Private/BasePassVertexShader.usf:47–86`). 원문 hash는 셰이더 맵 의존이다(`VertexFactory.h:442, 593–610`).
- stream 성분(버퍼·offset·stride ≤ 255·원소 타입·용도: instancing·override·manual fetch, `RC/Public/VertexStreamComponent.h:10–72`)을 모아 같은 버퍼·stride·용도를 합치고 정점 요소(stream·offset·타입·attribute 번호·stride·인스턴스 여부, `RHI/Public/RHI.h:171–182`)를 만든다(`VertexFactory.h:799–813`). attribute 번호는 셰이더 `ATTRIBUTEn`과 같다(`Engine/Shaders/Private/LocalVertexFactory.ush:52–106`, `Engine/Private/LocalVertexFactory.cpp:520–610`).
- 정점 배치는 기본·위치 전용·위치+법선 전용(깊이 pass) 셋이다(`VertexFactory.cpp:398–413`). 원소 타입 20종(`RHIDefinitions.h:477–500`의 `VET_Float1`–`VET_UInt`).
- manual vertex fetch는 ES3.1 초과 기능 수준에서만이다. GLES는 늘 고정 입력 경로다(`VertexFactory.cpp:180–184, 262–276`).
- PSO 미리 준비를 지원하는 타입은 인스턴스 없이 정적 정점 요소 목록을 낸다(`VertexFactory.h:331, 393, 686–688`).

### 10.2 Rocks 규칙

- **등록.** 이름, `.rsh` 가상 경로, 기능 표시, 거르기 함수, 정적 정점 배치(인스턴스 데이터와 무관), 소유 파라미터 구조체를 가진다. 정적 등록 표이고, 셰이더 타입 등록(셰이더 시스템 12.2절)과 같은 시점에 닫힌다. 늦게 로드되는 모듈의 등록은 거부한다.
- **세 가지 정점 입력.** (1) **정점 입력 없음**: 전체화면·compute·유틸리티 셰이더(속성 없는 파이프라인, 11.2절). (2) **엔진 기본 정점 입력 타입**: 내장 메시와 fallback 메시 셰이더(계획 9.6절)가 쓰는 고정 배치(위치·UV·색, 지금 Sandbox 샘플 렌더러의 "Rocks 정점 형식", `Programs/Sandbox/Private/SampleRenderer.h` `BuiltinShaderHlsl` 주석). (3) **메시 정점 입력 타입**: 메시 렌더링 단계에서 정의한다.
- **셰이더 키 축.** 셰이더 시스템 키에 정점 입력 타입을 더한다(SX4, 결정 R9 권장안 (a) 키 필드). 거르기는 셰이더 타입 조건 AND 정점 입력 타입 조건이다. 완결성 검사(9.2절)는 타입 × 정점 입력 타입 × 퍼뮤테이션을 열거한다.
- **include 연결.** 메시 셰이더는 고정 가상 경로 `/Engine/Generated/VertexInput.rsh`를 include한다. 워커 입력을 만드는 메인 프로세스 전처리기가 이 경로를 그 정점 입력 타입의 `.rsh`로 잇는다(셰이더 시스템 결정 6). 그래서 셰이더 맵 키에 정점 입력 타입이 들어가야 한다(SX4). `.rsh`가 구현할 함수 인터페이스(입력 구조체, 월드 위치, 단계 사이 변수 채우기)는 첫 메시 셰이더 착수 전에 정한다.
- **정점 배치.** 속성마다 (location, 형식, stream 번호, offset), stream마다 (stride, 정점/인스턴스, 인스턴스 step 1). Metal 정점 buffer 상한(셰이더 시스템 7.2절 4항: 정점 stream 8개)과 맞춰 stream ≤ 8이다. 같은 버퍼·stride·용도의 stream 합치기는 UE와 같다.
- **검사.** 파이프라인 생성 때 정점 배치 ⊇ 셰이더 입력 마스크, 형식 기본 타입 일치(SX9). 위치 전용 같은 부분 배치는 깊이 pass 단계에서(후속).
- **형식.** `VertexFormat`은 Float2/3/4뿐이다(`DeviceTypes.h:27–31`). 메시 에셋 단계 전에 패킹 법선(8 bit·10:10:10:2), half UV, 8 bit 정규화 색, 정수형을 더한다(UE `PackedNormal.h`·`RHIDefinitions.h:484–498`). A0-G(텍스처·샘플러) 범위 밖이므로 메시 에셋 착수 전 별도 결정이다(15절).
- **manual vertex fetch**는 후속이다(GLES 불가, UE도 같다).

### 10.3 완료 기준(RB5)

등록 결정성(등록 순서와 무관한 키), 늦은 등록 거부, 키 축 포함 라이브러리 왕복, 정점 배치 ⊇ 입력 마스크·타입 불일치의 음성 대조, 다중 stream·인스턴스 stream 픽셀 대조(D3D12·Vulkan·GLES 미리보기).

### 10.4 정점 배치와 정점 원소 형식(GX3a)

정점 입력 타입(10.2절)의 바탕인 Graphics 쪽 정점 배치를 셰이더 시스템보다 먼저 완성한다(2026-11-16 사용자 지시 "하시오", "이번에는 제대로 해야함"). 셰이더와 독립이고, A0-G 착수 전에 Graphics 계약 변경과 함께 합친다. 정점 입력 타입 등록·셰이더 키 축(RB5)은 셰이더 시스템 P14·SH2 뒤다.

**정점 원소 형식.** UE 5.8 정점 원소 타입 20종 전부를 둔다.
- 형식은 메시 에셋이 직렬화하므로 GraphicsTypes에 둔다. 텍스처 형식과 같은 방식으로 고정 번호표와 안정 이름을 쓰고 enum 순서 값은 직렬화하지 않는다(6.5절과 같은 규칙).
- "셰이더 타입"은 VS 입력 변수의 기본 타입이다. 정규화·float 형식은 float 계열, 정규화 없는 정수 형식은 int·uint 계열이어야 한다.
- UE 대응 근거:
  - D3D12: `D3D12RHI/Private/D3D12VertexDeclaration.cpp:27–41`
  - Vulkan: `VulkanRHI/Private/VulkanRHIPrivate.h:405–450`
  - Metal: `Apple/MetalRHI/Private/MetalVertexDeclaration.cpp:26–59`
  - OpenGL: `OpenGLDrv/Private/OpenGLVertexDeclaration.cpp:49–77`
  - 목록: `RHI/Public/RHIDefinitions.h:477–500`

| 번호 | Rocks | UE 5.8 | 바이트 | 셰이더 타입 | D3D12 | Vulkan | Metal | OpenGL ES |
|---|---|---|---|---|---|---|---|---|
| 1 | `Float1` | `VET_Float1` | 4 | float | `R32_FLOAT` | `R32_SFLOAT` | `Float` | `GL_FLOAT`×1 |
| 2 | `Float2` | `VET_Float2` | 8 | float2 | `R32G32_FLOAT` | `R32G32_SFLOAT` | `Float2` | `GL_FLOAT`×2 |
| 3 | `Float3` | `VET_Float3` | 12 | float3 | `R32G32B32_FLOAT` | `R32G32B32_SFLOAT` | `Float3` | `GL_FLOAT`×3 |
| 4 | `Float4` | `VET_Float4` | 16 | float4 | `R32G32B32A32_FLOAT` | `R32G32B32A32_SFLOAT` | `Float4` | `GL_FLOAT`×4 |
| 5 | `Byte4Snorm` | `VET_PackedNormal` | 4 | float4 | `R8G8B8A8_SNORM` | `R8G8B8A8_SNORM` | `Char4Normalized` | `GL_BYTE`×4 정규화 |
| 6 | `UByte4` | `VET_UByte4` | 4 | uint4 | `R8G8B8A8_UINT` | `R8G8B8A8_UINT` | `UChar4` | `GL_UNSIGNED_BYTE`×4 정수 |
| 7 | `UByte4Unorm` | `VET_UByte4N` | 4 | float4 | `R8G8B8A8_UNORM` | `R8G8B8A8_UNORM` | `UChar4Normalized` | `GL_UNSIGNED_BYTE`×4 정규화 |
| 8 | `Bgra8Unorm` | `VET_Color` | 4 | float4 | `B8G8R8A8_UNORM` | `B8G8R8A8_UNORM` | `UChar4Normalized_BGRA` | 미지원(`UnsupportedCapability`) |
| 9 | `Short2` | `VET_Short2` | 4 | int2 | `R16G16_SINT` | `R16G16_SINT` | `Short2` | `GL_SHORT`×2 정수 |
| 10 | `Short4` | `VET_Short4` | 8 | int4 | `R16G16B16A16_SINT` | `R16G16B16A16_SINT` | `Short4` | `GL_SHORT`×4 정수 |
| 11 | `Short2Snorm` | `VET_Short2N` | 4 | float2 | `R16G16_SNORM` | `R16G16_SNORM` | `Short2Normalized` | `GL_SHORT`×2 정규화 |
| 12 | `Short4Snorm` | `VET_Short4N` | 8 | float4 | `R16G16B16A16_SNORM` | `R16G16B16A16_SNORM` | `Short4Normalized` | `GL_SHORT`×4 정규화 |
| 13 | `Half2` | `VET_Half2` | 4 | float2 | `R16G16_FLOAT` | `R16G16_SFLOAT` | `Half2` | `GL_HALF_FLOAT`×2 |
| 14 | `Half4` | `VET_Half4` | 8 | float4 | `R16G16B16A16_FLOAT` | `R16G16B16A16_SFLOAT` | `Half4` | `GL_HALF_FLOAT`×4 |
| 15 | `UShort2` | `VET_UShort2` | 4 | uint2 | `R16G16_UINT` | `R16G16_UINT` | `UShort2` | `GL_UNSIGNED_SHORT`×2 정수 |
| 16 | `UShort4` | `VET_UShort4` | 8 | uint4 | `R16G16B16A16_UINT` | `R16G16B16A16_UINT` | `UShort4` | `GL_UNSIGNED_SHORT`×4 정수 |
| 17 | `UShort2Unorm` | `VET_UShort2N` | 4 | float2 | `R16G16_UNORM` | `R16G16_UNORM` | `UShort2Normalized` | `GL_UNSIGNED_SHORT`×2 정규화 |
| 18 | `UShort4Unorm` | `VET_UShort4N` | 8 | float4 | `R16G16B16A16_UNORM` | `R16G16B16A16_UNORM` | `UShort4Normalized` | `GL_UNSIGNED_SHORT`×4 정규화 |
| 19 | `Rgb10A2Unorm` | `VET_URGB10A2N` | 4 | float4 | `R10G10B10A2_UNORM` | `A2B10G10R10_UNORM_PACK32` | `UInt1010102Normalized` | `GL_UNSIGNED_INT_2_10_10_10_REV`×4 정규화 |
| 20 | `UInt1` | `VET_UInt` | 4 | uint | `R32_UINT` | `R32_UINT` | `UInt` | `GL_UNSIGNED_INT`×1 정수 |

- OpenGL ES의 정규화 없는 정수 형식은 정수 경로(`glVertexAttribIFormat`, ES 3.1 vertex attrib binding. 옛 API로는 `glVertexAttribIPointer`)로 설정한다(float 경로로 설정하면 float로 변환되어 int 입력과 맞지 않는다).
- `Bgra8Unorm`의 OpenGL ES 처리:
  - ES에는 BGRA 정점 배열이 없다. UE는 이 경우 RGBA로 읽어 채널이 뒤바뀐다(`OpenGLVertexDeclaration.cpp:56–64`).
  - Rocks는 이 형식을 OpenGL backend(ES 3.2 공통 API, 셰이더 시스템 5.2절 G2)에서 미지원으로 공개한다. 이 형식이 든 파이프라인 생성은 `UnsupportedCapability`다(지금 Graphics 오류 코드로는 `Unsupported`, [OpenGL ES backend](OpenGLESGraphics.ko.md) 5.1절).
  - 메시 쿠커는 GLES 형식 프로필에서 색을 `UByte4Unorm`으로 쓴다.
  - 지원 여부는 backend별 형식 지원 표(GX1, A0-G G1과 같은 방식)로 조회한다.
- 정규화 변환 규칙은 그래픽 API 표준을 따른다. snorm은 max(c / (2^(b−1) − 1), −1), unorm은 c / (2^b − 1)이다. 32-bit 정수 형식은 정규화하지 않는다.

**정점 배치.**
- **속성** = (location 0–15, 형식, stream 0–7, offset)이다.
  - location은 HLSL `ATTRIBUTEn`의 n이다. 지금 원문 셰이더의 `TEXCOORDn`도 이번에 `ATTRIBUTEn`으로 바꾼다(셰이더 시스템 7.2절 7항).
  - 속성 수 상한은 16이다. Vulkan `maxVertexInputAttributes`와 OpenGL ES `GL_MAX_VERTEX_ATTRIBS`의 최소 보장이고, UE는 17이다.
- **stream** = (stride, 정점·인스턴스 구분)이다.
  - stream 상한은 8이다(Metal 정점 buffer 상한, 셰이더 시스템 7.2절 4항).
  - 인스턴스 stream은 인스턴스마다 한 번 진행한다. 진행 간격 1만 지원한다. UE도 instancing 여부 하나만 둔다(`RHI/Public/RHI.h:171–182` `bUseInstanceIndex`).
- **한계**:
  - offset ≤ 2047, stride ≤ 2048, 둘 다 4 byte 배수다. Vulkan `maxVertexInputAttributeOffset`·`maxVertexInputBindingStride`, OpenGL ES `GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET`·`GL_MAX_VERTEX_ATTRIB_STRIDE`의 최소 보장이고, Metal stride는 4 byte 배수다.
  - UE stream 성분은 offset·stride ≤ 255다(`RenderCore/Public/VertexStreamComponent.h:57–58`). Rocks는 장치 최소 보장 쪽을 쓴다.
  - 한계 값은 이름 있는 상수로 둔다.
- **배치 자체 검사**(파이프라인 생성 때, 위반은 `InvalidArgument`):
  - location이 겹치지 않는다.
  - offset + 형식 크기 ≤ stride.
  - 선언된 stream마다 속성이 하나 이상 있다.
  - 속성이 없는 배치(정점 입력 없음, 11.2절)도 유효하다.
- **셰이더 입력 대조**(파이프라인 생성 때):
  - 셰이더가 읽는 모든 location이 배치에 있어야 하고(배치 ⊇ 입력), 형식의 셰이더 타입 계열(float·int·uint)이 셰이더 입력 타입과 맞아야 한다. 위반은 `InvalidArgument`이고 진단 문자열에 location·형식·셰이더 타입을 적는다.
  - 셰이더가 읽지 않는 속성은 허용한다. 깊이 pass처럼 위치만 읽는 셰이더가 같은 배치를 쓰기 때문이다.
  - 셰이더 시스템 SH2 전에는 다음으로 입력을 얻는다.
    - D3D12: 컴파일한 VS 입력 signature(시맨틱 `ATTRIBUTE` 번호·성분 타입)
    - OpenGL: 링크 뒤 active attribute(location·타입)
  - SH2 뒤에는 쿠킹된 `ShaderBindingLayout`의 입력 마스크·타입을 쓴다(셰이더 시스템 7.2절 3·7항).

**버퍼와 그리기**(GX4 일부를 함께 당긴다).
- 버퍼 용도에 index를 더한다. index 형식은 UInt16·UInt32다.
- 기록 객체에 다음을 둔다.
  - `SetVertexBuffers(첫 stream, {버퍼, offset} 목록)`
  - `SetIndexBuffer(버퍼, 형식, offset)`
  - `Draw(정점 수, 인스턴스 수, 첫 정점)`
  - `DrawIndexed(index 수, 인스턴스 수, 첫 index)`
- base vertex·base instance는 0으로 고정한다. `SV_VertexID`·`SV_InstanceID`의 base 포함 여부가 backend마다 다르기 때문이다(셰이더 시스템 7.2절 7항).
- 정점 버퍼 없는 그리기(속성 없는 배치)를 허용한다. 지금은 정점 버퍼가 없으면 그리기를 무시한다(`Device.h:18–20`).
- 묶이지 않은 stream이나 범위를 넘는 index는 Development 진단이다. 범위 검사는 index 버퍼 내용을 CPU에서 아는 경우에만 한다.

**CPU 패킹 helper.**
- 메시 쿠커와 시험이 쓰는 변환을 Core 수학에 둔다(공용 기능 중복 금지).
  - float ↔ half(IEEE binary16, round-to-nearest-even, 비정규수·무한대·NaN 보존)
  - float → snorm8·unorm8·snorm16·unorm16(위 정규화 규칙의 역, 반올림, clamp)
  - 10:10:10:2 패킹
  - 법선·탄젠트를 `Byte4Snorm`에 담는 규칙(w에 탄젠트 부호. UE `RenderCore/Public/PackedNormal.h`와 같은 의미)
- 변환 결과는 호스트와 무관하게 비트가 같아야 한다.

**완료 기준.**
- 형식 20종 각각에 대해 다음을 확인한다.
  - CPU 시험: 크기·셰이더 타입 계열·번호표 왕복.
  - 픽셀 대조: 그 형식으로 색이나 위치를 넣은 그리기를 D3D12(WARP 포함)와 OpenGL에서 기대 픽셀과 대조한다. `Bgra8Unorm`의 OpenGL은 `UnsupportedCapability` 음성 대조다.
- 다중 stream(위치 stream 0, 색 stream 1), 인스턴스 stream(인스턴스별 offset·색), indexed draw(UInt16·UInt32), 정점 버퍼 없는 그리기를 픽셀 대조한다.
- 음성 대조:
  - location 중복, offset·stride 한계·정렬 위반, offset + 크기 > stride
  - 셰이더가 읽는 location이 배치에 없음
  - int 입력에 정규화 형식
- half 변환은 65536개 전체 왕복과 경계값(반올림 경계·비정규수·무한대·NaN)을 시험한다. 정규화 패킹은 경계값과 clamp를 시험한다.
- 검증 계층 오류 0.

**구현(2026-11-17).** `GraphicsTypes/VertexFormat.h`(형식 20종·고정 번호표·안정 이름·크기·성분·셰이더 타입 계열·정규화 여부, Vulkan·Metal 대응은 주석 표), `Graphics/DeviceTypes.h`의 `VertexAttribute`·`VertexStream`·`VertexLayout`·한계 상수·`ValidateVertexLayout`·`BufferUsage::Index`·`IndexFormat`·`VertexBufferBinding`, `CommandContext::SetVertexBuffers`·`SetIndexBuffer`·`Draw(정점 수, 인스턴스 수, 첫 정점)`·`DrawIndexed`, `Core/Math/VertexPacking.h`(snorm·unorm 8·16, 10:10:10:2, `PackNormal`·`PackTangent`, half는 기존 `Float16`). 셰이더 입력 대조는 D3D12가 `D3DReflect` 입력 signature(시맨틱 `ATTRIBUTEn`·성분 타입), OpenGL이 링크 뒤 active attribute로 한다. 원문 셰이더의 정점 입력 시맨틱은 `ATTRIBUTEn`으로 바꿨다. 구현 메모:
- "UnsupportedCapability"는 Core 오류 코드에 없어 `ErrorCode::Unsupported`로 공개한다.
- index 값 범위 검사는 하지 않는다. 장치가 index 버퍼 내용을 CPU에 남기지 않기 때문이다(메시 쿠커·RB5에서 내용을 아는 쪽이 검사한다). 묶이지 않은 stream은 그리기를 무시하고 `IgnoredDraws`를 올린다.
- 정규화 패킹 반올림은 가장 가까운 정수(가운데는 0에서 먼 쪽)이며 double로 곱해 호스트와 무관한 결과를 낸다.

### 10.5 인스턴스 데이터 읽기(Renderer RQ8)

메시 정점 입력 타입(10.2절 (3))이 그리는 인스턴스의 변환을 얻는 규약이다(개정 15). 인스턴스 데이터 내용(현재·이전 local-to-world 상위·하위, bounds, 표시, custom data)은 렌더러 GPU Scene이 정하고(Renderer 5.1절, 14.7절 MRD7), 이 절은 셰이더가 그것을 찾는 길만 정한다. 구현은 Renderer RS0이다.

**UE 5.8.** 정점 셰이더는 그리기마다 상수인 instance id offset(인스턴스 step 0인 정점 stream)과 `SV_InstanceID`를 더한 자리에서 instance id 버퍼를 읽어 인스턴스 번호를 얻고, 그 번호로 GPU Scene 인스턴스 데이터를 읽는다(`Engine/Shaders/Private/SceneData.ush:1340–1390`, 목록 읽기 `:1379`). ES3.1은 primitive uniform buffer 경로를 따로 둔다(`:1293–1316`).

**Rocks 규칙.**
- **인스턴스 데이터는 typed buffer.** GPU Scene 인스턴스 데이터는 `RGBA32Float` typed buffer이고 VS가 typed buffer SRV로 읽는다(Renderer 결정 RN9). 인스턴스 데이터를 정점 stream(10.4절 인스턴스 stream)으로 넣지 않는다. GLES VS의 storage block 최소 보장이 0이라 구조화 버퍼를 쓸 수 없고, 모든 backend를 한 경로로 둔다. GLES에서 typed buffer SRV는 VS 텍스처 유닛을 차지한다(셰이더 시스템 7.2절 4항).
- **인스턴스 번호.** `instanceIndex = InstanceIndices[instanceListOffset + SV_InstanceID]`다. `InstanceIndices`는 `R32Uint` typed buffer(공유 자원 구조체 `SceneResources`, 9.5·9.6절)이고 `instanceListOffset`은 루트 상수다. 그리기는 늘 첫 인스턴스 0으로 하므로 `SV_InstanceID`는 그리기 안 번호 0…n−1이다. base instance가 시스템 값에 들어가는지가 backend마다 다른 문제를 피한다(셰이더 시스템 7.2절 7항). GLES 3.1 간접 draw의 base instance 0 제약에도 그대로 맞는다(Renderer 15.2절).
- **늘 목록을 거친다(결정 R13).** CPU 가시성만 쓰는 단계(Renderer RS0–RS3)에도 렌더러가 그리기 항목마다 인스턴스 번호 목록을 프레임 업로드로 쓰고, GPU culling(RS4)은 같은 목록을 GPU가 쓴다. 셰이더 경로가 하나라 RS4 전후로 pass 셰이더가 바뀌지 않는다. UE도 일반 경로는 늘 instance id 버퍼를 거친다.
- **용량(개정 16).** typed buffer view 하나의 원소 수는 `DeviceCaps.limits.texelBufferElements` 이하다(GX1: D3D 2^27, Vulkan `maxTexelBufferElements` 최소 보장 65536, GLES `GL_MAX_TEXTURE_BUFFER_SIZE` 최소 보장 65536). 인스턴스 데이터 용량 = `texelBufferElements` ÷ 인스턴스당 `float4` 수(약 10–12로 추정, RS0에서 확정)다. 최소 보장 장치에서는 약 5,000 인스턴스이고 D3D는 사실상 제한이 없다. 실제 기기 값은 Renderer 19.2절 측정으로 본다. UE도 typed buffer view의 원소 상한을 장치 값으로 두고(`RHI/Public/RHIGlobals.h:321`, Vulkan `VulkanRHI/Private/VulkanRHI.cpp:1005`, OpenGL ES `OpenGLDrv/Private/OpenGLDevice.cpp:921–924`) 넘는 view 생성을 검증 오류로 본다(`RHI/Private/RHIValidation.cpp:1061–1062`).
- **넘침(결정 R16).** typed buffer를 다른 자원으로 몰래 바꾸지 않는다.
  - 인스턴스 데이터: 용량을 넘는 인스턴스 추가는 `Unsupported`로 실패하고 `RenderNotices`로 게임 lane에 알리며 계수를 올린다. 이미 있는 인스턴스는 그대로 그린다. 조용히 빠뜨리지 않는다(계획 13.1절).
  - `InstanceIndices`: 프레임 목록이 원소 상한을 넘으면 목록을 상한 크기 조각으로 나누고, 그리기 항목은 자기 조각의 view를 그리기별 바인딩(9.3절)으로 묶는다. 셰이더 경로는 그대로다.
  - light grid: 목록 용량을 원소 상한 이하로 두고, 넘치면 Renderer 8.1절의 넘침 보수 대체(그 셀에서 전체 순회)를 쓴다.
  - 최소 보장 기기가 대상 Android 기기에 실제로 있으면 GPU Scene 저장 형태(Renderer 결정 RN9, 2D 텍스처 대체 등)를 다시 연다. 재검토 조건은 RN9의 측정 MR4에 더했다.
- **루트 상수 배치.** 메시 pass 셰이더의 루트 상수 구조체는 앞 16 byte에 `MeshDrawConstants`(첫 `uint`가 `instanceListOffset`, 나머지 12 byte는 예약 padding)를 펼쳐 넣는다. 정점 입력 `.rsh`가 이 이름으로 읽는다. 나머지 112 byte는 pass가 쓴다(루트 상수 128 byte, 셰이더 시스템 7.2절 2·5항). UE는 같은 값을 정점 stream으로 넘긴다. Rocks는 루트 상수가 모든 형식에 있으므로(GLES는 uniform 에뮬레이션) 정점 stream·attribute 자리를 쓰지 않는다(stream 8개·attribute 16개 상한, 10.2절·SX9).
- **`.rsh` 인터페이스.** 메시 정점 입력 `.rsh`의 함수 인터페이스(10.2절 include 연결)에 "인스턴스 번호 → 현재·이전 변환" 단계를 넣는다. 읽기 함수는 GPU Scene 배치를 아는 렌더러 공용 `.rsh`가 제공하고 정점 입력 `.rsh`는 그것을 부른다. world 위치는 `(인스턴스 상위 − view 상위) + 상대 변환`으로 카메라 상대 값을 만든다(Renderer 5.1절, `View` slot의 이동 보정). 행렬은 행 벡터 `p·M`이고 셰이더는 `mul(v, M)`이다(셰이더 시스템 7.2절 6항).
- **앞면(개정 16).** 컬 방향은 파이프라인 상태라 한 그리기 안에서 인스턴스마다 바꿀 수 없다. 그래서 local-to-world 행렬식 부호는 인스턴스마다 보고, 부호가 다른 인스턴스는 한 그리기에 묶지 않는다(렌더러 상태 묶음 키에 부호를 넣는다, 셰이더 시스템 7.2절 8항 음수 행렬식). 변환 갱신으로 부호가 바뀐 인스턴스는 그 그리기 항목을 다시 분류한다(재질 교체와 같은 캐시 무효화, Renderer 4.2절).
- 엔진 기본 정점 입력 타입과 정점 입력 없음(10.2절 (1)·(2))은 이 규약 밖이다. 내장 메시는 지금처럼 그리기별 상수의 변환을 쓴다.
- **완료 기준(Renderer RS0에서).** (`instanceListOffset`, `SV_InstanceID`) → 인스턴스 변환이 CPU 참조와 같은지 픽셀 대조(D3D12·Vulkan·GLES 미리보기, GLES는 VS texel buffer 경로), offset이 0이 아닌 그리기, 행렬식이 음수인 인스턴스의 별도 그리기와 컬링 픽셀.

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
| checker·error 텍스처, error material, UI 글꼴 | 9.6절 | UE는 Engine UObject 에셋(`Engine/Classes/Engine/Engine.h:903, 911`) | 에셋·머티리얼·UI 단계에서 팩 범위를 넓힌다 |
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

## 12. 콘솔 설정(CP4·CP8)

- RenderCore는 설정 변수로 기능 전환·디버그 모드·예산을 다룬다(그래프만 해도 31개, `RC/Private/RenderGraphPrivate.cpp:18–426`). Rocks에는 이 체계가 없고 Config 계층도 후속이다(UE-C21·C29).
- 이 문서는 렌더 쪽 요구만 정한다. 체계 자체는 [설정 계층·콘솔 설정 설계](ConfigSystem.ko.md)(CP8·CP4, 단계 CF1–CF5)가 정한다. 계획 24.2절 Config 우선순위(engine default → platform → project → device profile → user → session override), 런타임 변경 가능·재시작 필요 구분, 값 출처 표시가 그 스펙의 범위다.
- 렌더 요구:
  - 타입 있는 변수(bool·int·float·열거·문자열)와 명령, 이름 공간(렌더 그래프·셰이더·렌더 lane 등).
  - **렌더 snapshot.** 렌더 lane이 읽는 변수는 "렌더 읽기" 표시를 가지고, 게임 lane이 프레임 packet을 만들 때 그 값들의 snapshot을 담는다. 렌더 lane은 packet의 값만 읽는다. UE는 렌더 스레드 사본 주소에 쓰는 명령으로 전파한다(`RC/Private/RenderingThread.cpp:493–530`).
  - 셰이더 형식별 값(UE `RC/Public/ShaderPlatformCachedIniValue.h:13–95`)은 셰이더 시스템의 형식 정의(셰이더 시스템 12.1절)나 TargetProfile 쪽으로 두고 콘솔 설정에 두지 않는다. 쿠킹 결과에 영향을 주는 값은 키에 들어가야 하기 때문이다.
  - 헤드리스·시험에서 결정적 값을 고정할 수 있어야 한다.
- **구현(2026-11-17, RB1 설정 snapshot).** 게임 lane은 sync 지점(`ConfigContext::Sync`) 뒤 `ConfigContext::Snapshot()`을 `RenderSettingsRef::Share`로 감싸 `RenderLane::AttachSettings(ticket, ...)`로 그 프레임 slot에 담는다. 렌더 lane은 프레임 작업 안에서 `RenderFrameContext::Settings().Get<config::ConfigSnapshot>()`만 읽는다(registry·문맥은 읽지 않는다). slot의 참조는 그 slot을 다음에 다시 확보할 때(`BeginFrame`, GPU 완료 뒤) 또는 lane 정지 때 게임 lane에서 놓는다. 그래서 snapshot 수명은 그 프레임 slot이 끝날 때까지이고 파괴는 게임 lane에서 일어난다. 시험(`RenderBase/Tests/RenderSettingsTests.cpp`): sync 지점에서 바꾼 값은 다음 packet부터 보이고 이전 packet은 옛 값을 읽는다. GPU 완료 gate가 닫힌 동안 옛 snapshot이 살아 있고, slot을 다시 확보하면 놓인다.
  - 스펙과 다른 점: RenderBase는 ObjectRuntime에 의존하지 않으므로(9.3절) Config 타입을 직접 이름 짓지 않는다. `RenderSettingsRef`는 snapshot의 공유 소유권(`SharedPtr` aliasing)과 타입 표지만 들고, 꺼낼 때 같은 타입으로만 돌려준다(다른 타입이면 null). 타입은 Config를 아는 쪽(실행 파일·렌더러 모듈)이 정한다. 담지 않은 프레임은 빈 참조를 읽는다(직전 프레임 값을 물려받지 않는다).

- **CVar 이관(설계, 구현 전).** [CVar·치트 명령 설계](CVarSystem.ko.md)(개정 2)가 콘솔 설정을 타입 있는 핸들 `CVar<T>`로 바꾼다. 착수 시점은 사용자가 정한다. 그때 RB1 연결은 다음처럼 바뀐다.
  - 렌더 snapshot은 "렌더 읽기" 표시가 있는 값만이 아니라 **모든 CVar**를 담는다(CVar VD6). 설정 struct 필드의 `RenderRead` 블록은 같은 snapshot 안에 남는다.
  - 게임 lane은 `ConfigContext::Snapshot()` 대신 `Acquire()`로 얻은 참조를 지금처럼 `RenderSettingsRef`로 프레임 slot에 담는다(CVar 7.2절).
  - 렌더 lane은 프레임 작업을 시작할 때 packet의 snapshot을 `cvar::SnapshotBinding`으로 그 스레드에 묶고, 작업이 끝날 때 놓는다. 그 사이 렌더 코드는 이름 탐색 없이 `ShadowDistance.Get()`처럼 읽는다(CVar 7.1·16절).
  - 이관 뒤에는 게임 lane이 **모든 프레임**에 snapshot을 담는다. 바뀐 것이 없으면 같은 snapshot 참조다. 그래서 렌더 lane은 늘 묶을 snapshot이 있고, 묶음 없는 `Get()`은 Fatal이라는 CVar 규칙(VD7)과 어긋나지 않는다. 지금의 "담지 않은 프레임은 빈 참조" 규칙은 이관 때 없앤다.
  - 렌더 lane이 만든 worker 작업은 묶인 snapshot의 참조를 작업 문맥으로 넘겨받는다(CVar 10.1절).

## 13. GPU 계측(RB7)

### 13.1 UE 5.8 방식

- GPU 구간 기록은 RHI가 맡고 RenderCore 헤더는 매크로만 둔다: draw event → RHI breadcrumb, GPU stat → RHI GPU 프로파일러(`RC/Public/ProfilingDebugging/RealtimeGPUProfiler.h:102–134`). breadcrumb은 Debug·Development·GPU 프로파일·통계를 켠 비-Shipping 빌드에서 켜진다(`RHI/Public/RHIBreadcrumbs.h:26`, `RHIDefinitions.h:74`).
- 구간 시작·끝마다 프로파일러 이벤트와 timestamp query를 넣는다(D3D12 `D3D12RHI/Private/D3D12CommandContext.cpp:175–176`, OpenGL은 시작 TOP·끝 BOP query와 debug group, `OpenGLDrv/Private/OpenGLDrv.cpp:119–165`). 이벤트 종류는 프레임 경계·작업 시작·끝(CPU·GPU 시간 대응)·fence signal·wait다(`RHI/Public/GPUProfiler.h:92–236`). OpenGL은 GPU가 언제 일하는지 몰라 프레임 전체를 바쁨으로 표시한다(`OpenGLDrv/Private/OpenGLDevice.cpp:1531–1537`).
- CPU·GPU 시간 대응: D3D12는 queue의 clock calibration과 timestamp 주파수(`D3D12RHI/Private/D3D12Submission.cpp:871–876`, copy queue는 어댑터가 지원할 때만), Vulkan은 calibrated timestamps 확장(`VulkanRHI/Private/VulkanDevice.cpp:1702`).
- 동적 해상도 예산은 별도 timestamp 풀을 쓰고 몇 프레임 늦게 비차단으로 회수한다(`RC/Private/RenderGraphEvent.cpp:166, 197–256`).
- 캡처 도구는 모듈식 제공자(시작·끝·프레임 캡처)이고 RenderDoc·PIX·Xcode 플러그인이 구현한다(`RC/Public/IRenderCaptureProvider.h:71–88`, `RC/Public/RenderCaptureInterface.h:20–30`).

### 13.2 Rocks 규칙

- **`GpuScope`.** 이름 구간 하나가 debug marker push·pop과 timestamp 한 쌍을 함께 기록한다. 렌더 그래프는 pass마다 자동으로 구간을 둔다(계획 11.3절 "pass 이름 기록"). Release에서는 marker를 끄고 timestamp는 설정으로 켠다. CPU 쪽 구간은 CP5와 같은 trace에 상관시킨다. 기록 형과 GPU 시간 변환 경계는 [다중 스레드 trace 설계](TraceSystem.ko.md) 9절이다(RB7은 TR2 뒤).
- **timestamp query**(GX9): 프레임 slot 링으로 회수하고 결과는 완료 통지 큐(5.2절)로 넘긴다. 결과가 아직 없으면 "미완료"다. disjoint 표시가 있으면 그 구간을 무효로 한다(계획 29.4절). GLES timestamp는 확장(`GL_EXT_disjoint_timer_query`)이 있을 때만이고, 없으면 GPU 시간을 "없음"으로 공개하며 추정값을 만들지 않는다(UE는 Android에서 같은 확장 함수를 쓴다, `OpenGLDrv/Private/Android/AndroidOpenGLFunctions.h:77`).
- **CPU·GPU 시간 대응**(계획 29.1절 calibration pair): D3D12 queue clock calibration, Vulkan calibrated timestamps(13.1절과 같다). GLES는 `GL_TIMESTAMP` 조회(GLES는 `GL_EXT_disjoint_timer_query`의 `glGetInteger64vEXT`)와 CPU 시각을 잇달아 읽어 쌍을 만들고 조회 지연만큼 오차를 둔다(GX9 결정). 확장이 없으면 "없음"이다. Metal은 SH5 때 정한다.
- **계수**: 그래프 pass 수·draw·dispatch 수·transient 최대 사용량·파이프라인 캐시 적중(계획 11.3절).
- **후속(Developer 모듈)**: 캡처 도구 연동(RenderDoc·PIX), 그래프 자원 덤프, 텍스처 시각화, 셰이더 → CPU 메시지, GPU 오류 주입(개발 빌드 전용, 계획 11.2절 device loss 시험에 쓴다), 긴 GPU 작업 벤치마크.

### 13.3 완료 기준(RB7)

`GpuScope` 중첩이 캡처 도구(PIX·RenderDoc)에서 pass 이름으로 보이는지(수동 확인 기록), timestamp 링 회수가 GPU 완료 이벤트 뒤에만 값을 내는지(gate), GLES 확장 없음에서 "없음" 공개, calibration pair로 CPU·GPU 구간이 같은 시간축에 놓이는지.

**구현(2026-11-17).** `RenderBase/GpuTrace.h`에 `GpuTrace`(queue 하나의 계측 장부)·`GpuScope`(스택 객체)·`GpuTraceSettings`·`RegisterGpuTraceTypes`·`GpuTicksToTraceTicks`를 두었다. RenderBase가 TraceCapture에 의존한다.
- 흐름: 렌더 lane이 프레임 기록 안에서 `GpuTrace::BeginFrame`(queue 정의·calibration·프레임 시작 timestamp) → `GpuScope`(marker push + 시작 timestamp, 소멸 때 끝 timestamp + marker pop) → `EndFrame`(프레임 끝 timestamp)를 부른다. 완료 통지를 받는 쪽(보통 게임 lane)이 `Consume(notices)`로 `GpuTimestamp` 통지를 짝지어, 두 끝이 모두 온 구간만 trace tick으로 바꿔 채널 `gpu`에 쓴다. 완료 전 구간은 pending이고 trace에 나가지 않는다. 끊김(`Cancelled`) 통지가 하나라도 있으면 그 구간은 버리고 `invalid`로 센다.
- GPU 시간은 채널 `gpu`·설정 `timestamps`·장치 `timestamps`·`clockCalibration`이 모두 참일 때만 잰다. 아니면 queue 정의(`Trace.GpuQueue`)의 GPU 시간 칸을 0으로 공개하고 timestamp를 쓰지 않는다(추정값 없음). marker는 설정 `markers`만 따른다. 기본값은 둘 다 Release에서 꺼짐이다(설정으로 켠다).
- calibration: `ClockCalibration`에 `cpuTicks`(`platform::MonotonicTicks`, trace 시간축)와 `uncertaintyNanoseconds`를 더했다. D3D12는 queue clock calibration이 한 번에 읽은 QPC 값과 CPU tick 하나 길이, OpenGL은 `GL_TIMESTAMP` 조회를 CPU tick 두 번으로 감싼 가운데 값과 조회 지연의 절반이다. 쌍은 세션 첫 프레임과 `calibrationIntervalFrames`(기본 60)마다 다시 얻어 정의 기록(`Trace.GpuCalibration`)으로 남기고, 그 사이 구간은 가장 최근 쌍으로 바꾼다(두 시계의 drift는 보정하지 않는다).
- GPU 구간 이름은 CPU 구간과 같은 세션 이름 표(`Trace.ScopeName`)를 쓴다(`TraceResolveScopeName`). 같은 pass 이름은 CPU·GPU에서 같은 id다.
- 시험(`RenderBase/Tests/GpuTraceTests.cpp`, 기록 backend): GPU 완료 gate 전에는 구간 0·pending 3이고 gate 뒤 구간 2·프레임 1, 변환한 tick이 기록된 calibration 쌍으로 계산한 값과 정확히 같고 CPU 구간 시작 ≤ calibration CPU tick ≤ GPU 프레임 시작, 중첩 깊이 0·1과 marker push·push·pop·pop 순서, timestamp 미지원 장치의 "GPU 시간 없음" 공개(구간·calibration 기록 0, timestamp 쓰기 0), Release 기본 설정(marker·timestamp 0), 끊김 통지의 구간 무효.
- 스펙과 다른 점: (1) GPU 구간은 시작·끝 기록 둘이 아니라 두 tick을 가진 기록 하나(`Trace.GpuScope`)다. 두 끝이 모두 온 뒤에만 쓰므로 짝을 잃은 반쪽이 생기지 않는다. (2) "GPU 시간 없음"은 별도 정의 형이 아니라 queue 정의의 GPU 시간 칸(0)이다. (3) 작업 시작·끝·fence signal·wait 사건은 아직 없다(다중 queue와 함께). (4) 렌더 그래프 pass 자동 구간은 RG1에서 붙였다(그래프가 기록 전체를 GPU 프레임 하나로 묶고 pass마다 `GpuScope`를 둔다, 8절). 그래프 밖 렌더 코드는 `BeginFrame`·`GpuScope`·`EndFrame`을 직접 부른다. (5) queue는 `GpuTrace` 하나당 하나다(설정 `queueId`·`queueName`).
- **캡처 도구 수동 확인 절차**(PIX·RenderDoc, 자동 시험 밖): Sandbox를 D3D12(PIX on Windows) 또는 OpenGL(RenderDoc)로 실행해 프레임을 캡처한다. 이벤트 목록에서 `GpuScope` 이름이 중첩 그대로 보이는지(바깥 구간 아래 안쪽 구간), 프레임 끝에 열린 marker가 남지 않는지를 본다. Release 구성에서는 marker가 보이지 않아야 한다. 결과는 날짜·backend·도구 판과 함께 이 절에 적는다(아직 기록 없음).

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

## 15. 다른 문서 개정

이 문서 확정과 함께 고친다. 위치는 절 번호와 인용 문구로 적는다. A0-G에 걸리는 행은 A0-G 착수 전에 먼저 고친다.

| 문서 | 위치 | 현재 | 개정 |
|---|---|---|---|
| AssetSystem | 1절 A0-G(샘플러 확장) | 샘플러 확장 범위 | **A0-G 착수 전**: 비교 함수(SX13), 텍스처 desc 샘플러는 기본값이고 바인딩 때 지정 가능(결정 R12) |
| 〃 | 메시 에셋 단계(정해지면) | 정점 형식 Float만 | 패킹 법선·half·정규화 색 형식(10.2절) |
| 〃 | A5 텍스처 플랫폼 데이터(`TexturePlatformData`) | 형식·swizzle·mip 배치 서술 | **A5 착수 전**: 6.5절 입력 계약(mip-major 배치, 상주 꼬리·스트리밍 덩어리, 4 KiB 정렬, 고정 번호표 직렬화). 에셋 세션이 반영한다 |
| ShaderSystem | 9.9절 SX1–SX14 | — | 표대로. SX3·SX4·SX5·SX6은 P14 전, SX13은 A0-G 전, SX14는 SH1 전 |
| 〃 | 3절 구조 | ShaderCore에 "라이브러리 로드" | ShaderCore는 장치 없는 메타데이터·라이브러리 형식 읽기, 장치 셰이더 객체는 RenderBase |
| 〃 | 12.3절 | `GLES31_PC`·개발 트리는 필요 시 컴파일 | 셰이더 공급자 interface 경로(SX14) |
| EngineImplementationPlan | 3.1절 모듈 표 "RHI", "RenderGraph / Renderer" 행, 3.2절 트리 | RHI/{Null,D3D11,…}, RenderGraph, Renderer | Graphics(기존 이름)와 backend 모듈, RenderBase 행 추가(의존: Graphics·ShaderCore·MetaSchema·Task·Core), RenderGraph는 RenderBase 위 |
| 〃 | 3.2절 Shutdown 순서 | GPU fence 완료 → … → closure 확인 → scheduler 종료 | 렌더 lane은 closure 확인까지 살아 있고 그 뒤 Drain·소유권 반납·Join(5.5절) |
| 〃 | 8.1절 `TaskDesc.lane`·Lane 표 Render 행 | `TaskLane`을 가진 Task 하나의 모델 | 전용 스레드 lane(`DedicatedLane`)과 Worker lane(`WorkerScheduler`)의 관계, lane 작업 네 종류, 전달 가능 타입 등록 |
| 〃 | 8.2절 | in-flight slot | 게임 대기 지점을 프레임 slot 확보로 한정하고 메시지 펌프 대기, 표면 소실 예외(5.3절) |
| 〃 | 9.6절 초기화 | manifest → DeviceCaps → … → BuiltinReady | 렌더 lane 시작 단계에서 엔진 셰이더 집합 검사 → 팩 → fence → BuiltinReady(5.5절) |
| 〃 | 11.2절 RenderDevice·`Retire(ResourceHandle, lastUse)` | 별도 인터페이스 이름, 호출자가 lastUse를 준다 | Graphics `Device`에 Submit·Retire·GpuFencePoint를 들임(GX7). 게임 lane의 `Retire`는 handle만 받는 lane 작업이고 last-use는 내부 집계(6.2절). OpenGL ES도 fence sync |
| 〃 | 11.3절 | "DAG 검증", copy queue → compute overlap → alias | 선언 순서 실행·의존 정합성 검증(결정 R5), 풀 단계, render pass 병합(tiled), copy queue 위치(결정 R6) |
| 〃 | 11.5절 OpenGL 행 | core 4.5급 | 셰이더 시스템 14절 개정과 같이 OpenGL ES. 그래프 lowering 대상도 같다 |
| EngineRuntime | 6절 RunFrame ⑥ "FrameEnd(렌더 추출·제출)" | 게임 스레드 동기 제출 | 프레임 packet 게시와 slot 확보(5.2·5.3절) |
| 〃 | 10.1절 계약 | 생성 스레드 전용 | 장치 스레드 모델(5.4절) |
| 〃 | 11절 "GPU 텍스처는 WorldIntegration 텍스처 캐시가 만든다" | 게임 스레드가 장치 자원을 만든다 | `RenderHandle` 예약과 렌더 lane 생성(6.2절) |
| 〃 | 12절 5·6단계 | 종료 시 GPU idle 대기 → 장치·창 정리 | 5.5절 종료 순서 |
| 〃 | 13절 GPU 시험 | D3D12·OpenGL self-test | 기록 backend CPU 시험, D3D12·Vulkan·GLES 미리보기 |
| 〃 | 14절 제한 | 렌더 스레드는 후속 | RB1 |
| PlatformConcurrency(구현 문서) | :42 | 스레드 이름·우선순위·affinity·스택은 후속 | CP1에서 이름·우선순위·스택 크기 |
| UnrealCoreFeatureAudit | UE-C21·C23·C29 행 | 후속·부분 | CP8·CP5·CP4의 선행 관계를 적는다 |
| RepositoryLayout | 트리 | Engine/Source/Runtime에 렌더 기반 모듈 없음 | RenderBase·RenderGraph·ShaderCore, Developer 디버그 모듈, CP4·CP8이 들어갈 Core 모듈 |

## 16. 결정 필요 사항

| ID | 결정 | 선택지와 권장 | 기한 |
|---|---|---|---|
| R1 | 이름 확정 | 2절 표. 모듈 이름 `RenderBase`(UE 모듈 이름 `RenderCore`는 피한다) | 문서 확정 |
| R2 | 모듈 배치 | 권장: ShaderCore(장치 없음)·RenderBase·RenderGraph 셋으로(1절). 대안: UE처럼 RenderBase 하나에 그래프까지 | 문서 확정 |
| R3 | 게임 대기 규칙 | 권장: 프레임 slot 확보만 막히고 대기 중 메시지 펌프, 예외는 표면 소실 handshake와 종료·재생성·시험 비우기(5.3절). 계획 8.1절 해석의 확인 | 문서 확정 |
| R4 | 파라미터 구조체와 그래프 의존 결합 | 권장: 같은 구조체(UE 방식, 9.6절). 대안: 셰이더 파라미터와 pass 의존 선언을 다른 구조체로 나눔(두 번 적어야 하고 미사용 멤버 거르기가 어려움) | 셰이더 시스템 결정 4와 같은 기한(P14 전) |
| R5 | 그래프 pass 재배치 | 권장: 선언 순서, 재배치 없음(UE와 같다) | RG1 전 |
| R6 | copy queue 위치 | 권장: 그래프 밖 업로드·스트리밍 계층(UE 그래프에 copy pipe 없음). 대안: 그래프 pipe로 | RG4 전 |
| R7 | 파라미터 상수의 루트 상수/cbuffer 배치 | 권장: 선언 명시(구조체 종류에 "그리기별 상수" 표시, 128 byte 상한). 대안: 크기 기준 자동 | SH2 전(SX2) |
| R8 | SH2 범위 | (가) 권장: 셰이더 집합(RB3)과 GX3 가운데 속성 없는 파이프라인·MRT 블렌드·깊이를 SH2에 넣는다(SH2가 쿠킹 라이브러리 로드와 파이프라인 생성을 바꾸므로 같은 변경이어야 한다). compute 파이프라인·스텐실·다중 stream은 GX3로 SH2 뒤. 바인딩 지도·상수 블록·파이프라인 캐시(RB4)는 SH2(손으로 쓴 layout)와 SH3(생성 layout)에 나눠 넣는다. (나) SH2는 셰이더 시스템 원안대로 두고 RB3·GX3 전체를 SH2 뒤 별도 단계로 | SH2 전 |
| R9 | 정점 입력 타입 키 | 권장: (a) 셰이더 키 필드(UE 방식). 대안: (b) 열거형 퍼뮤테이션 차원 | P14 전 |
| R10 | 착수 위치 | **결정됨(2026-11-16).** 고정 순서는 바꾸지 않는다. 화면 마일스톤 = 셰이더 구현 + 렌더 기반(18.1절). 미리 해서 유리한 것은 앞당긴다(사용자 지시 "미리해도 되는게 유리하다면 미리하는것도 좋음"): Core CP1–CP3은 `feature/render-base`에서 착수했고, Graphics GX7–GX11·RB1은 A0-G 뒤(같은 backend 파일)에 한다. 나머지는 마일스톤 안에서 한다 | — |
| R11 | 바인딩 지도 생성 시점 | 권장: 셰이더 집합을 열 때 런타임 리플렉션(MetaSchema)으로 생성, 측정 M3가 나쁘면 라이브러리에 굽기 | RB4 전 |
| R12 | 샘플러 값 위치 | 권장: 텍스처 desc 샘플러는 기본값, 파라미터 멤버가 바인딩 때 지정 가능, 그래프 풀 키에서 제외, 비교 샘플러 추가(9.6절, SX13). 대안: 샘플러를 텍스처 desc에서 완전히 빼고 늘 바인딩 때 지정(에셋 텍스처의 샘플러 설정이 머티리얼 쪽으로 옮겨진다) | **A0-G 착수 전**. 2026-11-16 권장안과 비교 샘플러(SX13)를 AssetSystem A0-G 행에 요구로 반영했다(사용자 지시 "미리해도 되는게 유리하다면 미리하는것도 좋음") |
| R13 | 인스턴스 번호 경로 | **결정됨(2026-11-18, 권장안)**: (가) 늘 인스턴스 번호 목록을 거친다(10.5절, UE와 같다). CPU 단계도 목록을 업로드한다. (나) CPU 단계는 `offset + SV_InstanceID`로 연속 배치를 직접 읽고 GPU culling만 목록(셰이더 경로 둘, RS4에서 pass 셰이더 퍼뮤테이션이 는다) | Renderer RS0 전 |
| R14 | 기본 BRDF LUT 생성·형식 | **결정됨(2026-11-18, 권장안)**: 시작 때 CPU로 만든다(UE와 같다), 128×32 `RG16Float`을 모든 backend에(11.1절). 대안: 미리 계산한 표를 엔진 데이터로 싣는다(시작 비용 없음, 데이터 파일 하나 더), UE처럼 장치마다 `RG16Unorm`·`RG8Unorm`을 고른다(GLES `RG16Unorm`은 확장 조건) | Renderer RS7 전 |
| R15 | export한 history의 수명 | **결정됨(2026-11-18, 권장안)**: (가) export는 풀 자원의 사용권만 넘기고, 소유자의 `Retire`는 마지막 사용 fence 뒤 `GraphResourcePool` 반납이다(7.4·7.5절). UE history도 풀 render target 참조이고 참조가 풀 하나만 남으면 다시 쓴다(`Renderer/Private/SceneRendering.h:943–950`, `RC/Private/RenderTargetPool.cpp:202, 754–760`). (나) `ViewHistory`가 두 장을 소유하고 번갈아 import·쓰기(풀 밖 고정 자원, 크기 변경 때 따로 재생성) | RG1 남은 것 |
| R16 | typed buffer 원소 상한 넘침 | **결정됨(2026-11-18, 권장안)**: (가) 용량은 `DeviceCaps.limits.texelBufferElements`로 계산하고, 인스턴스 데이터는 넘는 추가를 `Unsupported`로 실패·통지, `InstanceIndices`는 조각으로 나눠 그리기별로 묶고, light grid는 기존 넘침 보수 대체(10.5절). 최소 보장 기기가 대상에 있으면 Renderer RN9을 다시 연다. (나) 인스턴스 데이터를 typed buffer 여럿으로 나눔(셰이더가 버퍼를 골라야 하고 SRV 수가 는다, 최소 보장 장치에서 버퍼 수가 16을 넘는다). (다) 2D 텍스처 대체(GLES 한 경로 추가, RN9 개정이 필요해 이 결정 범위 밖) | Renderer RS0 전 |
| R17 | MSAA 색 resolve 수단 | **결정됨(2026-11-18, 권장안)**: (가) temporal이 있으면 셰이더 휘도 가중 고정, temporal이 없으면 데스크톱은 같은 셰이더·tiled GPU는 attachment resolve 허용(11.2절, UE desktop·모바일과 같은 나눔). 깊이·velocity는 늘 셰이더. (나) 모든 경우 셰이더(tiled GPU 대역폭 손해). (다) 모든 경우 하드웨어 resolve(HDR 가장자리 품질, upscaler 입력이 backend마다 다름) | Renderer RS8 전 |

## 17. 측정

| ID | 측정 | 결과가 정하는 것 |
|---|---|---|
| M1 | 렌더 lane 작업 게시·처리 비용, 프레임 packet 크기 | 작업 묶음 단위, packet 복사 방식 |
| M2 | 파이프라인 생성 시간(backend별), 캐시 적중률 | 미리 준비(후속) 시기 |
| M3 | 셰이더 집합 열기 비용(바인딩 지도 생성 포함) | 결정 R11, memory image 재검토 |
| M4 | 그래프 컴파일 비용(pass 수 대비) | 병렬 설정(RG3) 시기 |

## 18. 화면 마일스톤과 구현 순서

### 18.1 화면 마일스톤 범위

**성격.** 처음에는 화면에 최소한으로 그려 시험하는 단계였으나, 그 목적은 의미가 없어졌다. 렌더 기반을 제대로 세우는 단계로 정의한다(2026-11-16 사용자 결정 "원래 목표는 미니멀하게 테스트하고자 했던건데 이제 의미없음. 렌더코어 제대로 구현하는게 나음"). 고정 순서(A0-B → A0-O → B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)에서의 자리는 그대로이고, 렌더 세션이 맡는다.

**결정(2026-11-16).**
- 셰이더 경로: 쿠킹 셰이더 위에서 그린다. 셰이더 시스템 결정 11 (나)다. 지금의 backend별 원문·런타임 컴파일 경로를 거치지 않는다.
- 렌더 기반 구조: 렌더 lane·장치 자원 수명·내장 자원 팩을 마일스톤 안에서 만든다.
- 텍스처 범위: AssetSystem A5의 텍스처 전부를 화면에서 확인한다.
- 검증 backend: 셰이더 경로를 따른다. D3D12·Vulkan·GLES 미리보기다.

**포함.**

| 영역 | 항목 |
|---|---|
| Core | CP1–CP7, CP4·CP8(콘솔 설정·Config, 별도 스펙) |
| 셰이더 시스템 | 6절 4항 순서의 P2·P8·P10·P3·P4·SH0·P6a·P5·SH1·P14·P9·P11·P6b·SH2·P7·SH3·SH4. 결정 2–7과 SX1–SX14를 그 기한에 정한다 |
| Graphics | GX1–GX11. VulkanGraphics(SH2), OpenGLGraphics의 GLES 미리보기 전환(P11) |
| RenderBase | RB1–RB7 |
| RenderGraph | RG1–RG2 |
| 텍스처 표시 | 6.5절 계약으로 쿠킹된 A5 텍스처를 GPU 자원으로 만들어 그린다. 다섯 차원 전부다: 2D 사각형, Cube 하늘 상자, 2DArray·CubeArray는 slice 선택, Volume은 단면. 모든 비압축 형식(HDR은 tone map pass로 표시), 샘플러 설정·swizzle·sRGB, CPU Availability의 대체 표시도 포함한다 |
| 통합 | WorldIntegration 텍스처 컴포넌트가 `RenderHandle`을 예약하고 프레임 packet으로 넘긴다. Sandbox에서 키보드·마우스·게임패드로 텍스처·slice·카메라를 바꾼다 |

**범위 밖(다음 단계).**
- Forward+·Deferred 조명 경로와 GI(계획 11·14절).
- 메시 에셋. 내장 메시만 쓴다.
- 텍스처 압축·색 보정(에셋 세션)과 텍스처 스트리밍(렌더 세션, 마일스톤 다음 단계). 6.5절 계약의 상주 꼬리 덩어리는 마일스톤에서도 그대로 읽는다.
- D3D11·Metal(SH5), Android(SH6).
- RG3–RG5, GX12·GX13.

**완료 기준.**
- 셰이더 시스템 SH0–SH4 완료 기준.
- 이 문서 RB1–RB7·RG1–RG2 완료 기준.
- Sandbox self-test가 다섯 차원·형식별 장면을 D3D12·Vulkan·GLES 미리보기에서 허용 오차 안으로 대조한다. Vulkan·GLES는 CI에서 NOT RUN일 수 있고, 그때는 로컬 결과를 기록한다.
- 검증 계층 오류 0.
- Release 실행 파일에 셰이더 컴파일러가 없다(셰이더 시스템 9절).
- 렌더 lane 모드와 인라인 모드의 결과가 같다.

### 18.2 구현 순서(제안)

괄호는 의존이다. 셰이더 시스템 단계는 그 문서 6절 4항 순서를 따른다.

1. Core: CP1 → CP2·CP3·CP6·CP7(병행) → CP8 → CP4 → CP5.
2. Graphics:
   - GX7 → GX11 → GX10. GX8·GX9는 GX7 뒤 언제든.
   - A0-G 뒤 GX1·GX2.
   - 셰이더 SH2 뒤 GX3 → GX4 → GX5. GX6은 P2·SH2 뒤.
   - 후속: GX12(GX5 뒤), GX13(GX4·GX5 뒤).
3. RenderBase:
   - RB1(CP1·CP6·GX7·GX10·GX11, 설정 snapshot은 CP4 뒤)
   - RB2(GX2·GX7·GX8·GX11)
   - RB3(SH2·P14·P6b·GX1·GX11·CP7, 일괄 생성 선택은 CP2)
   - RB4(GX3·GX6·GX8·CP3, SH2·SH3)
   - RB5(P14·SX4·GX3·GX4)
   - RB6(GX3·GX4·RB2)
   - RB7(GX9·CP5)
4. RenderGraph: RG1(RB4·RB7·GX1·GX2·GX4·GX5·CP3) → RG2(GX8) → RG3(CP2·GX11, 병합은 GX13) → RG4(GX1) → RG5(GX12).

## 19. 개정 이력

- 개정 1(2026-11-16): 초안. UE 5.8.4 RenderCore 다섯 갈래 조사(렌더 스레드·자원 수명, 렌더 그래프, 셰이더 런타임, 정점 입력·공용 자원·계측, RHI·Core 선행 조건)를 종합했다. 사용자 지시 "용어들은 우리 rocks에 맞춰야함"에 따라 UE 이름은 대응표와 근거 열에만 둔다. 사용자 지시 "기존에 언리얼에서 레거시로 하위호환을 해놓은 코드들은 고려할 필요가 없음"에 따라 UE 레거시·버전 이력을 뺐다.
- 개정 2(2026-11-16): 교차 검증 2건(UE 5.8.4 근거 약 160곳·Rocks 근거 약 35곳 사실 확인, 정합성·반례·완결성 검토: 차단 3·중요 15·사소 9)을 반영했다.
  - 차단:
    - 장치 한 스레드 계약과 지연 생성·병렬 기록·handle 예약의 모순 → 장치 스레드 모델 GX11(5.4절)을 두었다. `RenderHandle` 예약 표를 RenderBase에 두고, GLES 생성은 lane 작업으로 넘기며, 지연 생성 기준을 "게시 1회"로 바꿨다.
    - 엔진 셰이더 완결성 검사가 `GLES31_PC`·개발 트리에서 늘 실패하는 문제 → 셰이더 공급자 interface와 "공급 대기" 상태를 두었다(9.2절, SX14).
    - 종료 순서가 계획 3.2절과 맞지 않음 → 렌더 lane이 closure 확인까지 산다. Drain을 소유권 반납 앞에 두고, 스왑체인 → 창 순서를 정했다(5.5절).
  - 중요:
    - 대기 중 메시지 펌프·표면 작업·표면 소실 예외(5.3절)
    - 인라인 모드 trampoline과 실행 중 표지(CP1)
    - GLES 업로드 판단을 (program, 순번)별 identity·version으로(9.4절)
    - 셰이더 집합 lease·epoch·교체 규칙(9.2·9.7·9.8절)
    - 그래프 import·export·풀 축출(7.4절)
    - 샘플러 위치와 비교 샘플러(결정 R12, SX13, A0-G 전 기한)
    - 정점 입력 셋 구분과 맵 키(10.2절, SX4)
    - 셰이더 집합 → 팩 → `BuiltinReady` 순서와 BR01 범위(5.5·6.3절)
    - MetaSchema 의존·ObjectRuntime 비의존·완료 통지 큐(5.2·9.3절)
    - 전달 가능 타입 등록으로 강제 범위를 좁힘
    - fence 완료 이벤트·descriptor 회수(GX7)
    - 의존 표와 18절 일치, GX12·GX13·CP8·RB7 추가
    - SX11·R8 정리
    - 빠진 개정 대상(EngineRuntime 6·11·12·13절, 계획 3.2·8.1·11.2절, ShaderSystem 12.3절, RepositoryLayout)
  - 사소:
    - 절 참조 정정
    - 본문 UE 이름을 근거 `파일:줄`로 대체
    - `KeepAlive`(Rockpile과 충돌) → `HasSideEffects`, `ConstantBuffer`(HLSL과 혼동) → `ConstantBlock`
    - 레거시·버전 표기 제거
    - 상태 `Failed` 추가, 소멸 처리 통일
    - backend white 역할 정리, `Core/Hash.h` 명시, GL debug output 포함, GX7 상태 정정
  - 사실 정정:
    - 보조 pipe 사용처(플러그인 Niagara·Cable·Groom 포함)
    - 풀 축출 조건(렌더 타깃 풀은 하한 초과 시 3프레임, 버퍼 풀은 30프레임)
    - UE clear의 load 동작 조건
    - 엔진 셰이더 맵 부재 처리(창 지원 플랫폼은 종료 요청, `ShaderCompiler.cpp:5715–5724`)
    - 기능 수준 변경 재초기화 범위
    - 그래프 설정 변수 31개
    - 모바일 명령 큐 모드
    - breadcrumb 빌드 조건
    - 정점 원소 타입 20종
    - UE 기본 법선 텍스처 값(128,128,128,255)
    - 줄 번호 11건
  - 미확인 해소:
    - D3D12 split barrier(native 미사용, 표시 기본 false)
    - CPU·GPU calibration API
    - GLES 정적 UB 처리(UE도 slot별 저장 뒤 바뀐 것만 복사)
    - Rocks backend 스레드 검사 없음
    - 정점 속성 최소 16(Vulkan·GLES 사양)
- 개정 3(2026-11-16): 6.5절 텍스처 플랫폼 데이터 입력 계약을 더했다. 사용자 결정 "화면 마일스톤과 텍스처 스트리밍 … 이것도 니가해"로 렌더 세션이 GPU 업로드·스트리밍을 맡고, 압축·색 보정은 에셋 세션이 맡는다. 배치·스트리밍 단위·정렬·직렬화는 에셋 세션과 합의했다. UE 5.8 근거는 플랫폼 데이터·mip 구조·cube face 배치·쿠킹 mip 분할이다. 15절 AssetSystem 행에 A5 착수 전 요구를 더했다.
- 개정 4(2026-11-16): 18.1절 화면 마일스톤 범위를 정의했다. 사용자 결정은 다음과 같다: "원래 목표는 미니멀하게 테스트하고자 했던건데 이제 의미없음. 렌더코어 제대로 구현하는게 나음", 쿠킹 셰이더 위에서(셰이더 시스템 결정 11 (나)), 렌더 기반 구조 포함, A5 텍스처 전부, 검증 backend는 셰이더 경로를 따름. 결정 R10을 이에 맞췄다.
- 개정 5(2026-11-17): GX7·GX10·GX11·CP6·RB1·GX3a 구현을 반영했다. 4.2절 상태 열과 RB1 행, 4.3·4.4·5.7·10.4절 구현 메모를 더했다. 스펙과 다르게 정한 것: 작업 함수를 캡처 없는 함수 포인터로 제한, `Retire`는 handle만 받음(호출자 last-use 하한 없음), D3D12 스레드 안전 생성은 GX8로 미룸, 장치 손실·미지원 능력은 기존 오류 코드(`Destroyed`·`Unsupported`), 정리 drain·closure·실행기 hook은 실행 파일 hook에만. 같은 날 사용자 결정으로 최소 렌더러(내장 메시·원문 셰이더)를 엔진 모듈에서 Sandbox 시험 샘플로 옮겼다.
- 개정 6(2026-11-17): GX8·GX9 구현을 반영했다(4.2절 상태 열, 4.4절 구현 메모, 13.2절 GLES 시간 대응 결정). GX11 상태에서 "D3D12 스레드 안전 생성은 GX8과 함께"를 지웠다(GX8에서 열었다). 스펙과 다르게 정한 것: 업로드 ring은 프레임 slot별 고정 구간 대신 fence로 닫는 묶음의 원형 할당, timestamp 끊김은 `ErrorCode::Cancelled`로 통지, `GpuScope`는 RB7로 남김.
- 개정 8(2026-11-17): RB7 `GpuTrace`·`GpuScope` 구현과 RB1 설정 snapshot 연결을 반영했다(4.2절 CP4·CP5·CP8·GX9·RB1 상태, 5.7·12·13.3절 구현 메모, 캡처 도구 수동 확인 절차). 스펙과 다르게 정한 것: GPU 구간을 두 tick을 가진 기록 하나로, "GPU 시간 없음"을 queue 정의 칸으로, calibration은 세션 첫 프레임과 일정 프레임 간격의 정의 기록으로, 작업·fence 사건과 그래프 자동 구간은 뒤로, 설정 snapshot 참조는 타입 표지를 가진 공유 참조(`RenderSettingsRef`)로 둠.
- 개정 7(2026-11-17): 1절에 좌표·행렬 규약 행을 더했다(사용자 결정: 좌표계·단위·행렬을 UE 5.8과 같게). 12절은 ConfigSystem, 13.2절은 TraceSystem을 참조하게 했다.
- 개정 9(2026-11-17): [픽셀 형식](PixelFormat.ko.md) 설계(개정 1)와 교차 참조를 넣었다(4.2절 GX1 행, 4.4절 GX1·GX2, 6.5절 후속). 사용자 지시 "PixelFormat으로", "언리얼에서 지원하는거 full로 지원"에 따라 형식 부분은 A0-G에서 분리하여 PF1–PF3으로 먼저 한다.
- 개정 10(2026-11-17): GX5(최소)·RG1 구현을 반영했다(2절 용어, 4.2절 GX5·RG1 행, 4.4절 GX5 구현 메모, 8절 구현 메모). 사용자 지시 "렌더 그래프도 미리 하자"에 따라 선행(RB4·GX1·GX2·GX4)이 없는 부분은 범위를 줄였다. 스펙과 다르게 정한 것: 의존 선언을 명시 접근 목록으로(파라미터 구조체 수집기가 같은 목록을 만든다), GX5 분리 전이·pipe 없음과 attachment 전이는 장치 Begin, 즉시·지연 동치의 정의, 풀 키에 샘플러 포함, 풀 반환은 그래프 끝, lease를 `RenderHandleTable`에, 검증 항상 켬, 그래프 기록 전체를 GPU 프레임 하나로. 픽셀 형식 PF1 병합 뒤 그래프 텍스처 desc는 `PixelFormat`이고, 풀 byte 추정은 `MipChainBytes(format, w, h, 1, mips)`(PixelFormatInfo), `CreateTexture`는 번호표 밖·블록 압축 형식을 거부한다.
- 개정 11(2026-11-17): Vulkan GX5 전이 구현을 반영했다(4.4절 GX5 구현 메모, RG1 완료 기준 (g)에 Vulkan 하드웨어·lavapipe). 처리는 [렌더 기반 구현](../Implementation/RenderBase.ko.md) 8절과 [VulkanGraphics](VulkanGraphics.ko.md) 8절.
- 개정 12(2026-11-18): GX1(장치 능력·MSAA 표본 수)·GX2 버퍼 부분(버퍼 용도·버퍼 view·`ReadBuffer`·그리기 slot) 1단계 구현을 반영했다(4.2절 GX1·GX2 상태, 4.4절 구현 메모와 장치 측정 표). 스펙과 다르게 정한 것: 버퍼 view 묶기는 GX6 전 개발 경로 고정 배치(slot 0–3 = `t1–t4`)이고, Vulkan 그리기 descriptor는 파이프라인별 layout과 기록 단위 pool로 바꿨다.
- 개정 13(2026-11-18): [CVar·치트 명령 설계](CVarSystem.ko.md) 개정 2를 반영했다(4.2절 CP4 행, 12절 CVar 이관 메모). RB1의 설정 snapshot 연결은 CVar 구현 때 `Acquire()`·모든 CVar 포함·렌더 lane `SnapshotBinding`으로 바뀐다.
- 개정 14(2026-11-18): CP7(엔진 전역 `Name` pool)을 권장안으로 정했다: 전역 pool 없음, 렌더 이름은 정적 문자열 리터럴 + 64 bit 해시 조회, 등록 때 충돌·중복은 실패, 쿠킹 형식은 이름 정렬·SHA-256 그대로(4.3절, CP 표).
- 개정 15(2026-11-18): [렌더러](Renderer.ko.md) 18절 요구 RQ7·RQ8·RQ15·RQ16·RQ17을 설계로 반영했다(구현은 화면 마일스톤 뒤 렌더러 단계).
  - RQ7: 9.5절에 렌더러 공유 상수 slot 여섯(`View`·`Scene`·`ForwardLight`·`Shadow`·`Fog`·`PrecomputedLighting`)과 상수 버퍼 예산, 9.6절에 자원 묶음 규칙(펼쳐 넣기·이름·쓰는 멤버만 의존·view마다).
  - RQ8: 10.5절 인스턴스 데이터 읽기(GPU Scene typed buffer, 인스턴스 번호 목록 + `SV_InstanceID`, 루트 상수 `MeshDrawConstants`, 행렬식 부호별 그리기, 결정 R13).
  - RQ15: 7.5절 렌더러 사용 요구(`GraphSharedData`와 `SceneTextureSet`, history export 성공 조건과 무효 표시, mip view와 GLES view 기준 mip, view 여럿의 한 그래프와 pass 묶음 구간). 렌더 그래프 별도 문서가 없어 이 문서에 두었다.
  - RQ16: 11.2절 MSAA resolve(색 휘도 가중, 깊이 전경 최댓값, 같은 표본 velocity, coverage 선택 출력), 14절 MSAA resolve 행.
  - RQ17: 11.1절 빈 light grid·빈 그림자 depth(깊이 1)·검은 cube·기본 BRDF LUT(결정 R14).
  - 2절 용어에 `GraphTextureView`·자원 묶음을 더하고, 9.9절 SX4에 셰이더 시스템 개정 16 반영을 적었다.
- 개정 16(2026-11-18): 개정 15 교차 검증 반영.
  - 7.4·7.5절: packet 하나에 `ViewSet`이 여럿인 경우 `SceneTextureTable`을 (ViewSet, view) 키 표 하나로, 선언 순서 A 공통 → A view들 → B 공통 …, history export를 풀 반납으로(결정 R15), GLES mip view의 크기 조회도 기준 mip 명시, 그래프 구간 `GraphScope`의 번호는 trace 표지로.
  - 9.2절: 런타임 확인 기능 비트 대조와 `Unsupported`. 9.3절: 그리기별 바인딩. 9.5절: `PrecomputedLighting`을 slot에서 그리기별 바인딩으로 옮겨 공유 slot 다섯, `Scene`은 `ViewSet` 단위, 상수 버퍼 예산 최대 10(머티리얼 파라미터 컬렉션 2 포함), GLES `vec4`는 합으로 검사. 9.6절: 이름 `<포함 멤버 이름>_<자원 멤버 이름>`(UE 중첩 구조체 규칙), `SharedResourceStruct`·`DrawResourceStruct` 용어.
  - 10.5절: typed buffer 원소 용량과 넘침(결정 R16), 행렬식 부호의 인스턴스 단위 분할과 재분류.
  - 11.1절: 빈 light grid의 셀 좌표 clamp, 검은 cube array 알파 0(UE와 같은 반사 합성 의미). 11.2절: MSAA resolve 범위(깊이 늘, velocity는 temporal 때, 색은 결정 R17), 스텐실 0, UE 줄 인용 정정.
  - 2절 용어(`SharedResourceStruct`·`DrawResourceStruct`·`GraphScope`), GX1 기능 이름 "VS layer 출력".
