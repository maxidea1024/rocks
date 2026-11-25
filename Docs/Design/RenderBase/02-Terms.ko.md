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
