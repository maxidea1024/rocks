# 렌더 기반 구현 범위와 계약

기준일: 2026-11-17. [렌더 기반 설계](../Design/RenderBase.ko.md)의 GX7·GX10·GX11·CP6·RB1·GX3a를 구현한 범위와 사용 계약이다. 설계 결정·근거는 설계 문서가 기준이고, 이 문서는 지금 코드가 지키는 계약과 아직 하지 않은 것을 적는다.

## 1. 제출·완료·회수(GX7, `Graphics/Device.h`)

- `EndFrame`은 graphics queue 제출의 완료 지점 `GpuFencePoint{deviceEpoch, queue, value}`를 돌려준다. `deviceEpoch`는 장치를 만들 때마다 받는 프로세스 고유 번호(`NewDeviceEpoch`)이므로 다른 장치·재생성 전 장치의 지점은 `WrongContext`다.
- `Retire(handle)`는 자원과 descriptor(D3D12 SRV·RTV 칸)를 회수 대기에 둔다. 해제 fence는 장치 내부 집계로 정한다: 기록 중이면 그 프레임의 fence, 아니면 그때까지 제출한 마지막 fence다. 실제 해제는 소유 스레드의 `EndFrame`·`WaitIdle`·`ReclaimCompleted`가 한다. OpenGL도 같은 규칙이다(즉시 삭제하지 않는다).
- 스레드 안전 fence API: `LastSubmittedFence`·`IsFenceComplete`·`WaitFence`·`NotifyFence`·`IsLost`. 공용 구현은 `Graphics/FenceTimeline.h`이며 backend마다 관찰 스레드 하나가 제출을 순서대로 기다려 완료를 게시한다(D3D12: fence 이벤트, OpenGL: 공유 context의 `glClientWaitSync`).
- 장치 손실: D3D12는 present·signal 실패의 device removed와 fence 완료 값 `UINT64_MAX`, OpenGL은 sync 대기 실패로 감지한다. 기다리던 대기·통지는 `Destroyed`로 깨어나고 `BeginFrame`·`EndFrame`도 `Destroyed`다(Core에 장치 손실 전용 오류 코드가 없어 `Destroyed`를 쓴다).
- Core 작업 이벤트 연결: `Graphics/GpuFenceEvent.h`의 `TrySubmitFenceEvent`(Task/AsyncIoEvent.h와 같은 방식).
- D3D12는 고정 프레임 2개 대신 기록 저장소(command allocator) pool을 쓴다(끝난 제출의 저장소 재사용, 상한 `MaxCommandAllocators` = 8). 프레임 수 제한은 렌더 lane 프레임 slot이 한다.
- 아직 하지 않은 것: `RenderResourceLease`(기록 lease)는 병렬 기록·그래프 import가 생기는 RB2·RG1에서 둔다. 지금은 소유 스레드의 기록 중 프레임이 그 프레임에서 쓰는 자원을 암묵적으로 붙잡는다. 호출자가 주는 last-use 하한(계획 11.2절 `Retire(handle, lastUse)`)은 단일 queue에서 내부 집계보다 늦을 수 없어 받지 않는다.

## 2. 장치 스레드 모델·소유권 이관(GX11·GX10)

- 소유 스레드 하나가 장치를 쓴다. 처음 소유자는 만든 스레드다. `ReleaseOwnership`(소유 스레드)·`AcquireOwnership`(소유자가 없을 때)로 넘긴다. OpenGL은 넘길 때 context를 떼고 새 스레드에 붙인다.
- 소유 스레드 전용 호출은 Debug·Development 구성에서 다른 스레드면 `WrongThread`다(반환값이 없는 `Retire`·`ReclaimCompleted`는 무시하고 `RejectedCalls`를 올린다). Release 구성은 검사를 생략한다. 기록 객체(`CommandContext`)의 그리기 호출은 검사하지 않는다.
- `DeviceInfo::concurrentResourceCreation`은 두 backend 모두 false다. D3D12는 초기 데이터 업로드가 소유 스레드의 queue 제출이므로 업로드 경로(GX8)와 함께 연다.

## 3. 엔진 종료 단계 hook(CP6, `Engine/Engine.h`)

`Engine::Shutdown` 순서: `BeginShutdown`(Subsystem 역순 → hooks) → client 종료·World 파괴 → `CompleteInFlightWork`(Subsystem 역순 → hooks) → Subsystem 정리 → [엔진 Registry 최종 GC → `hooks.DrainShutdownCleanup`]을 둘 다 0이 될 때까지(최대 `MaxShutdownCollectPasses` = 8) → `VerifyShutdownClosure` → `StopExecutors` → Registry·타입 정리. 실패한 단계도 뒤 단계를 건너뛰지 않고 첫 오류를 돌려주며 상태는 `Stopped`가 된다. World 파괴가 외부 보유로 실패하면 지금처럼 `Stopping`에 남고, 다시 부르면 이미 부른 hook은 부르지 않는다.

## 4. 렌더 lane(RB1, `RenderBase` 모듈)

- 모듈 의존: Graphics·Task·MetaSchema(·Core). ObjectRuntime·World·Engine을 모른다.
- `RenderLane`: CP1 `DedicatedLane`(Thread·Inline) 위의 렌더 lane. `Start`가 Thread 모드에서 장치 소유권을 lane으로 넘기고, lane이 소유권 획득 → `RenderLaneProcessor::OnLaneStart`(스왑체인·내장 자원) → 제출 fence 대기 → 준비 완료를 게시한다. `Start`는 기다리지 않고 첫 `BeginFrame`이 기다린다.
- 작업 네 종류: `SubmitFrame`(프레임 packet), `PostResource`·`Retire`(자원), `PostSurface`·`PostSurfaceAndWait`(표면, 뒤의 것은 표면 소실 handshake), `PostMarker`(lane 표지). 값은 `ROCKS_RENDER_TRANSFERABLE`로 등록한 타입만 받고(등록하지 않으면 overload가 없어 컴파일 오류), 작업 함수는 캡처 없는 함수 포인터다. 리플렉션 스키마가 있는 등록 타입은 Development에서 ObjectRef·WeakRef 필드가 있으면 게시를 `InvalidArgument`로 거부한다(중첩 struct는 보지 않는다). `ObjectRef`는 이동할 수 없으므로 그 필드를 가진 struct는 C++ 타입 검사가 먼저 거부한다.
- 프레임 slot 2–3개: `BeginFrame`이 다음 slot(가장 오래된 slot)의 GPU 완료를 기다린다. slot마다 lane 처리 완료(`framesProcessed`)와 GPU 완료(`framesCompleted`) 표지가 있다. GPU 완료는 그 프레임 작업 안의 마지막 제출 fence로 정하고 `NotifyFence`로 받는다(폴링 없음). 대기 중 펌프 hook(`RenderLanePump`)을 `FrameWaitPumpIntervalMs`(1 ms)마다 부른다. 대기 진단 상한은 `DefaultHangTimeoutMs`(120초, 0이면 끔)이며 넘으면 `DependencyFailed`와 `hangReports` 계수다. 디버거 연결 감지로 끄는 것은 아직 없다.
- fence 완료 통지는 lane의 inbox로 들어가 lane 작업(Inline 모드는 기다리는 게임 lane이 실행하는 lane 작업)이 장치 `ReclaimCompleted` → 완료 통지 게시 순서로 처리한다.
- `RenderNotices`: FrameCompleted·ResourceReady·ResourceFailed·ResourceRetired·DeviceLost·LaneFailed. `RenderHandleTable`: Pending·Ready·Failed·Retired, 잠금으로 게임·렌더 lane이 함께 쓴다.
- 종료 단계 함수: `BlockFrames` → `WaitFramesIdle` → `DrainRetires`(반복) → `CloseResources`(OnLaneStop·WaitIdle·closure 확인: 남은 handle·Retire·장치 회수 대기 0) → `Stop`(Drain·남은 통지 0·소유권 반납·Join). `Shutdown`이 차례로 모두 부른다. 실패는 lane 상태(`Failure`)와 통지로 드러나고 다음 `BeginFrame`이 그 오류를 돌려준다.
- Sandbox self-test는 시험 샘플 렌더러(`Programs/Sandbox/Private/SampleRenderer`, 엔진 모듈이 아니다)를 lane 쪽 처리기가 소유하고 프레임 packet 작업 안에서 그린다.
- 아직 하지 않은 것: 설정 snapshot(CP4), `Pending`·`Failed` handle 그리기의 대체 자원·계수(RB2), 장치 재생성 경로, 보조 직렬 pipe·지연 기록 명령 목록.

## 5. 정점 배치(GX3a)

- 정점 원소 형식 20종은 `GraphicsTypes/VertexFormat.h`다. 열거자 값이 고정 번호표(1–20)이고 `VertexFormatName`·`ParseVertexFormatName`이 안정 이름이다. 크기·성분 수·셰이더 타입 계열(float·int·uint)·정규화 여부를 질의한다. Vulkan·Metal 대응은 헤더 주석 표로만 둔다.
- 배치(`Graphics/DeviceTypes.h`): 속성(location 0–15·형식·stream 0–7·offset), stream(stride·정점/인스턴스), 한계 `MaxVertexAttributes` 16·`MaxVertexStreams` 8·`MaxVertexAttributeOffset` 2047·`MaxVertexStride` 2048·`VertexLayoutAlignment` 4. `ValidateVertexLayout`이 파이프라인 생성 때 자체 검사를 한다(위반 `InvalidArgument`). 속성 없는 배치도 유효하다.
- 셰이더 입력 대조: D3D12는 `D3DReflect`의 VS 입력 signature(시맨틱 `ATTRIBUTEn`, 성분 타입), OpenGL은 링크 뒤 active attribute(location·타입)로 셰이더가 읽는 location이 배치에 있고 셰이더 타입 계열이 같은지 본다. 위반은 `InvalidArgument`이고 `LastError`에 location·형식·셰이더 타입을 적는다. 셰이더가 읽지 않는 속성은 허용한다.
- OpenGL: 정규화 없는 정수 형식은 `glVertexArrayAttribIFormat`, 인스턴스 stream은 binding divisor 1이다. `Bgra8Unorm`은 `Unsupported`다(Core에 `UnsupportedCapability` 코드가 없다).
- 그리기: `SetVertexBuffers(첫 stream, {버퍼, offset})`·`SetIndexBuffer(버퍼, UInt16|UInt32, offset)`·`Draw(정점 수, 인스턴스 수, 첫 정점)`·`DrawIndexed(index 수, 인스턴스 수, 첫 index)`. base vertex·base instance는 0이다. 배치가 선언한 stream에 버퍼가 없으면 그리기를 무시하고 `IgnoredDraws`를 올린다. index 값 범위는 검사하지 않는다(장치가 내용을 CPU에 남기지 않는다).
- CPU 패킹 `Core/Math/VertexPacking.h`: snorm·unorm 8·16(반올림은 가운데에서 0에서 먼 쪽, double 곱으로 호스트 무관), 10:10:10:2, `PackNormal`·`PackTangent`(Byte4Snorm, w = ±127). half는 기존 `Float16`(round-to-nearest-even, 비정규수·무한대·NaN 보존).

## 6. 검증

- CPU: `FenceTimeline`·`DeviceOwnership`·`VertexFormat` 번호표·`ValidateVertexLayout` 한계·정점 패킹(half 65536개 왕복 포함)·Engine 종료 hook·`RenderLane`(기록 backend 대조, slot 대기와 펌프, 등록 타입·Object 참조 검사, 엔진 종료 순서, 소유권 이관 중 표면 작업, 장치 손실)·`RenderHandleTable` 시험. 모두 이벤트 gate로 순서를 만들고 sleep에 기대지 않는다.
- GPU(`Sandbox --self-test`): D3D12(하드웨어·WARP)·OpenGL에서 정점 형식 20종 픽셀 대조(OpenGL `Bgra8Unorm`은 음성 대조)·다중 stream·인스턴스 stream·indexed(UInt16·UInt32)·정점 버퍼 없는 그리기·셰이더 입력 음성 대조, fence 대기·통지·작업 이벤트·회수, 소유권 이관 뒤 다른 스레드의 그리기, 렌더 lane Thread·Inline 모드로 엔진 루프 프레임(offscreen 픽셀 대조·숨김 창 스왑체인 present·resize 표면 작업·종료 hook)을 확인한다.
