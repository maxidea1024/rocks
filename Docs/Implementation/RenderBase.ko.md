# 렌더 기반 구현 범위와 계약

기준일: 2026-11-17. [렌더 기반 설계](../Design/RenderBase.ko.md)의 GX7·GX10·GX11·CP6·RB1·GX3a·GX8·GX9·GX5(최소)·RG1을 구현한 범위와 사용 계약이다. 설계 결정·근거는 설계 문서가 기준이고, 이 문서는 지금 코드가 지키는 계약과 아직 하지 않은 것을 적는다.

## 1. 제출·완료·회수(GX7, `Graphics/Device.h`)

- `EndFrame`은 graphics queue 제출의 완료 지점 `GpuFencePoint{deviceEpoch, queue, value}`를 돌려준다. `deviceEpoch`는 장치를 만들 때마다 받는 프로세스 고유 번호(`NewDeviceEpoch`)이므로 다른 장치·재생성 전 장치의 지점은 `WrongContext`다.
- `Retire(handle)`는 자원과 descriptor(D3D12 SRV·RTV 칸)를 회수 대기에 둔다. 해제 fence는 장치 내부 집계로 정한다: 기록 중이면 그 프레임의 fence, 아니면 그때까지 제출한 마지막 fence다. 실제 해제는 소유 스레드의 `EndFrame`·`WaitIdle`·`ReclaimCompleted`가 한다. OpenGL도 같은 규칙이다(즉시 삭제하지 않는다).
- 스레드 안전 fence API: `LastSubmittedFence`·`IsFenceComplete`·`WaitFence`·`NotifyFence`·`IsLost`. 공용 구현은 `Graphics/FenceTimeline.h`이며 backend마다 관찰 스레드 하나가 제출을 순서대로 기다려 완료를 게시한다(D3D12: fence 이벤트, OpenGL: 공유 context의 `glClientWaitSync`).
- 장치 손실: D3D12는 present·signal 실패의 device removed와 fence 완료 값 `UINT64_MAX`, OpenGL은 sync 대기 실패로 감지한다. 기다리던 대기·통지는 `Destroyed`로 깨어나고 `BeginFrame`·`EndFrame`도 `Destroyed`다(Core에 장치 손실 전용 오류 코드가 없어 `Destroyed`를 쓴다).
- Core 작업 이벤트 연결: `Graphics/GpuFenceEvent.h`의 `TrySubmitFenceEvent`(Task/AsyncIoEvent.h와 같은 방식).
- D3D12는 고정 프레임 2개 대신 기록 저장소(command allocator) pool을 쓴다(끝난 제출의 저장소 재사용, 상한 `MaxCommandAllocators` = 8). 프레임 수 제한은 렌더 lane 프레임 slot이 한다.
- `RenderResourceLease`는 RG1에서 `RenderHandleTable`에 두었다(10절). 장치 수준에는 lease가 없고, 소유 스레드의 기록 중 프레임이 그 프레임에서 쓰는 자원을 암묵적으로 붙잡는다. 호출자가 주는 last-use 하한(계획 11.2절 `Retire(handle, lastUse)`)은 단일 queue에서 내부 집계보다 늦을 수 없어 받지 않는다.

## 2. 장치 스레드 모델·소유권 이관(GX11·GX10)

- 소유 스레드 하나가 장치를 쓴다. 처음 소유자는 만든 스레드다. `ReleaseOwnership`(소유 스레드)·`AcquireOwnership`(소유자가 없을 때)로 넘긴다. OpenGL은 넘길 때 context를 떼고 새 스레드에 붙인다.
- 소유 스레드 전용 호출은 Debug·Development 구성에서 다른 스레드면 `WrongThread`다(반환값이 없는 `Retire`·`ReclaimCompleted`는 무시하고 `RejectedCalls`를 올린다). Release 구성은 검사를 생략한다. 기록 객체(`CommandContext`)의 그리기 호출은 검사하지 않는다.
- `DeviceInfo::concurrentResourceCreation`: D3D12는 true(GX8)다. 자원·파이프라인 생성과 내용 갱신·staging 업로드는 장치 내부 잠금 아래 업로드 기록에 쌓이고 소유 스레드의 다음 제출이 실행한다. 다른 스레드는 열린 기록을 제출하지 않으며 ring이 차면 기다리지 않고 전용 staging으로 올린다. OpenGL은 context가 소유 스레드에 묶여 false이고 다른 스레드의 생성은 `WrongThread`다.

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
- OpenGL(ES 3.2, 2026-11-17 P11): 정규화 없는 정수 형식은 `glVertexAttribIFormat`(ES 3.1 vertex attrib binding), 인스턴스 stream은 `glVertexBindingDivisor` 1이다. `Bgra8Unorm`은 `Unsupported`다(Core에 `UnsupportedCapability` 코드가 없다).
- 그리기: `SetVertexBuffers(첫 stream, {버퍼, offset})`·`SetIndexBuffer(버퍼, UInt16|UInt32, offset)`·`Draw(정점 수, 인스턴스 수, 첫 정점)`·`DrawIndexed(index 수, 인스턴스 수, 첫 index)`. base vertex·base instance는 0이다. 배치가 선언한 stream에 버퍼가 없으면 그리기를 무시하고 `IgnoredDraws`를 올린다. index 값 범위는 검사하지 않는다(장치가 내용을 CPU에 남기지 않는다).
- CPU 패킹 `Core/Math/VertexPacking.h`: snorm·unorm 8·16(반올림은 가운데에서 0에서 먼 쪽, double 곱으로 호스트 무관), 10:10:10:2, `PackNormal`·`PackTangent`(Byte4Snorm, w = ±127). half는 기존 `Float16`(round-to-nearest-even, 비정규수·무한대·NaN 보존).

## 6. 업로드·readback(GX8)

- 업로드 ring(`Graphics/UploadRing.h`, `DeviceDesc::uploadRingBytes`, 기본 4 MiB): 연속 구간 할당, 제출 때 fence로 묶음을 닫고 GPU 완료 뒤 회수한다. 열린 묶음은 어떤 fence로도 회수하지 않는다. 다 회수돼도 위치를 이어 써서 링 전체를 돌며, 끝에 자리가 없으면 0으로 감는다(`UploadStats::ringWraps`).
- 내용 갱신: `UpdateBuffer(버퍼, offset, byte)`·`UpdateTexture(텍스처, mip, 픽셀)`. D3D12는 업로드 기록(별도 command list)에 쌓고 다음 제출 앞에 실행한다(`EndFrame`·`SubmitUploads`·`WaitIdle`). 텍스처 행 간격은 256 byte, 배치는 512 byte 정렬로 ring 안에서 맞춘다. OpenGL ES는 즉시 `glBufferSubData`·`glTexSubImage2D`다(그래서 기록 중 갱신은 그 뒤의 그리기부터 보인다, [OpenGL ES backend](../Design/OpenGLESGraphics.ko.md) 6절). 범위 밖·크기 불일치는 `InvalidArgument`다.
- 스트리밍 staging pool(`Graphics/StagingPool.h`, `DeviceDesc::stagingBudgetBytes`, 기본 256 MiB): `AcquireStaging` → CPU가 채움(IO 목적지) → `UploadTextureFromStaging`·`UploadBufferFromStaging` → `ReleaseStaging`. 놓은 staging은 그 업로드의 fence가 끝난 뒤에만 재사용 목록으로 간다. 재사용은 요청 크기 이상·두 배 이하일 때이며 handle은 새로 발급한다(놓은 handle은 stale). 예산을 넘으면 빈 staging부터 지운다. 정렬이 맞지 않는 원본(행 간격·offset)은 ring으로 다시 옮겨 올린다. OpenGL staging은 CPU 메모리이고 업로드가 즉시라 바로 돌아간다.
- 비동기 readback: `RequestReadback(RenderTarget)`이 지금까지의 제출 뒤 복사를 제출하고 완료 지점을 준다. `TryReadback`은 완료 전 false(비차단), 완료 뒤 width×height×4 byte(첫 행이 위)를 쓴다. 기록 중이면 `Busy`. D3D12는 READBACK heap 버퍼, OpenGL ES는 pixel pack buffer이고 fence 뒤 `glMapBufferRange`로 읽는다. `ReadPixels`는 시험용 동기 경로로 남는다(D3D12는 같은 readback 경로 + 대기).
- 렌더 lane: `RenderLane::UpdateBuffer`·`UpdateTexture`(lane 작업, FIFO 순서), `RequestReadback`(번호) → `RenderNoticeKind::ReadbackReady`(GPU 완료 뒤) → `TakeReadback`(`RenderBlob`).
- 아직 하지 않은 것: copy queue·프레임당 업로드 대역 상한(스트리밍 단계), 텍스처 배열·3D·압축 형식 업로드(GX2), 업로드 기록의 다른 스레드 제출.

## 7. GPU 계측(GX9)

- 기록: `CommandContext::WriteTimestamp()`가 번호를 돌려준다. 결과는 그 프레임의 GPU 완료 뒤 `Device::CollectTimestamps(out)`(소유 스레드, 비차단, 쓴 순서)가 낸다. 실패: 미지원 `Unsupported`, 기록 밖 `InvalidArgument`, 회수 전 slot만 남음 `Busy`, 프레임당 256 초과 `CapacityExceeded`.
- 프레임 slot 링(`Graphics/TimestampRing.h`): slot 4개 × 256 query. 닫힌 slot은 fence 완료를 본 뒤에만 읽고, 원본이 아직 결과를 내지 않으면(OpenGL query 가용 전) 다음 회수에서 다시 묻는다. 제출이 실패한 slot의 결과는 무효(disjoint)로 나온다.
- 공개: `DeviceInfo::timestamps`·`timestampFrequency`(D3D12 queue 주파수, OpenGL 10^9)·`timestampDisjointDetection`(OpenGL ES는 `GL_EXT_disjoint_timer_query`가 있을 때만. 그 확장이 없는 ES 장치는 timestamp 자체가 없고, Windows 미리보기의 데스크톱 core context는 GL 3.3 timer query라 끊김 표시가 없다)·`clockCalibration`·`debugMarkers`. 지원하지 않으면 값을 지어내지 않는다.
- CPU·GPU 시간 대응: `CalibrateClocks()`가 `{gpuTicks, cpuNanoseconds(platform::MonotonicNanoseconds 시간축), gpuFrequency}`를 주고 `GpuTicksToCpuNanoseconds`로 옮긴다. D3D12는 `GetClockCalibration`(QPC → ns, `TicksToNanoseconds`), OpenGL은 `glGetInteger64v(GL_TIMESTAMP)` 앞뒤 CPU 시각의 가운데다. drift 보정은 없으므로 프레임마다 새로 얻는다.
- debug marker: `PushMarker(이름)`·`PopMarker()`. 이름은 255 byte(UTF-8 글자 경계)로 자른다(`ClipDebugMarkerName`). D3D12는 PIX가 읽는 UTF-16 이벤트, OpenGL은 debug group이다. 프레임 끝에 열린 구간은 장치가 닫고, 짝 없는 pop은 무시하고 `IgnoredDraws`를 올린다.
- 렌더 lane: 프레임 GPU 완료 처리에서 결과를 `RenderNoticeKind::GpuTimestamp`(frame = 번호, value = tick, 끊김이면 error `Cancelled`)로 넣고 그 뒤 `FrameCompleted`를 넣는다.
- 아직 하지 않은 것: `GpuScope`와 그래프 pass 자동 구간·Release 설정(RB7), 캡처 도구(PIX·RenderDoc)에서 pass 이름이 보이는지의 수동 확인 기록.

## 8. 명시 접근 상태와 전이(GX5 최소, `Graphics/ResourceAccess.h`)

- 접근 비트 `ResourceAccess`(UE `ERHIAccess` 대응): 읽기 `VertexOrIndex`·`Constant`·`IndirectArgs`·`SrvGraphics`·`SrvCompute`·`CopySource`·`Present`·`DepthRead`, 읽기·쓰기 `UavGraphics`·`UavCompute`, 쓰기 `RenderTarget`·`DepthWrite`·`CopyDestination`, 내용 버림 `Discard`, 모름 `Unknown`(before 전용). 묶음 mask와 `CanCombineAccess`(한 상태로 성립하는 조합: 읽기 전용끼리, UAV끼리, 깊이끼리, 서로 다른 쓰기 금지).
- `SubresourceRange{firstMip, mipCount, firstSlice, sliceCount}`(개수 `RemainingSubresources` = 끝까지). 지금 텍스처는 slice 하나다.
- `CommandContext::Transition(Span<const ResourceTransition>)`: 텍스처 또는 버퍼 하나, 범위, before·after. 기록 중에만 유효하고 begin·end를 한 지점에서 한다(분리 전이는 RG2, pipe 구분은 RG4). before가 `Unknown`이면 장치가 추적한 상태에서 바꾼다. after가 `Unknown`이거나 성립하지 않는 조합·stale handle이면 그 전이만 무시하고 `IgnoredDraws`를 올린다.
- 장치는 텍스처 mip마다 현재 상태를 추적한다. `BeginFrame`·`BeginOffscreen`이 대상을 `RenderTarget`으로 바꾸고(attachment 전이), `EndFrame`은 offscreen 대상을 그대로 두며 스왑체인만 present로 돌린다. 휴지 상태(PSR) 관례는 없앴다.
- backend 처리:
  - D3D12: 추적 상태(mip별 `committed`·`current`)에서 after의 legacy resource barrier. 모든 mip이 같은 상태면 barrier 하나(ALL_SUBRESOURCES). UAV → UAV는 UAV barrier. 검증을 켠 장치에서 before가 추적 상태와 다르면 `ValidationErrorCount`를 올리고 `LastError`에 남긴다(GPU에는 추적 상태를 쓴다). 업로드 기록(초기 데이터·갱신·staging·readback)은 프레임 기록보다 먼저 실행되므로 `committed`에서 바꿨다가 되돌린다(상태 중립). 프레임 기록은 제출 때 `current`를 `committed`로 확정하고 제출하지 못하면 되돌린다. 렌더 타깃 텍스처는 셰이더 읽기 상태(PSR|NPSR, `SrvGraphics`와 같다)로 만든다. `SetTexture`가 셰이더 읽기 상태가 아닌 텍스처를 받으면 암묵 전이로 메운다(그래프를 거치지 않는 그리기용). `CopyPassTarget`·readback은 원본·목적지를 추적 상태에서 바꿨다가 되돌리고, 이미 복사 상태면 barrier를 내지 않는다. 버퍼는 COMMON 암묵 승격·제출 끝 복귀에 맡기므로 UAV barrier 외에는 내지 않는다(버퍼 쓰기 명령이 생기는 GX4에서 추적을 더한다). enhanced barrier는 쓰지 않는다(셰이더 시스템 결정 3과 함께 정한다).
  - OpenGL ES(공통 층 `GlesDevice`, WGL·EGL 플랫폼 층 공용): 아무것도 하지 않는다(드라이버 암묵 전이, UE OpenGL RHI도 같다). UAV(image·storage buffer) 쓰기 뒤 읽기의 `glMemoryBarrier`는 그런 자원이 생기는 GX2에서 after의 접근 비트로 bit를 골라 넣는다.
  - D3D11: 아무것도 하지 않는다(드라이버 암묵 전이, UAV hazard도 드라이버가 처리한다. UE D3D11 RHI도 같다).
  - Vulkan(구현 중, 다른 작업): 같은 API를 pipeline barrier로 낮춘다(image layout과 access·stage mask, before `Unknown`은 장치 추적 layout). UAV → UAV는 shader write → shader read/write memory barrier다.
  - 기록 backend(`RenderBase/Tests/RecordingDevice`): 같은 추적 규칙에 before 불일치와 셰이더 읽기 상태가 아닌 `SetTexture`를 검증 오류로 센다(암묵 전이를 하지 않는다). `TransitionLog`·`TextureAccess`로 시험이 단언한다.

## 9. 렌더 그래프(RG1, `RenderGraph` 모듈)

- 모듈 의존: RenderBase·Graphics·TraceCapture. 공개 헤더 `RenderGraph/GraphTypes.h`(handle·종류·접근 모델 단위·진단 타입), `GraphPassAccesses.h`(접근 선언 builder·렌더 타깃), `GraphResourcePool.h`, `RenderGraph.h`.
- 내부 접근 모델: pass마다 `GraphResourceAccess{자원, subresource 범위, 접근, 표시}` 목록과 렌더 타깃(`GraphRenderTarget`) 하나. AddPass가 subresource 단위로 펼쳐 같은 subresource의 선언을 합친다(합칠 수 없으면 상충 오류). 지금 입력은 명시 선언 builder `GraphPassAccesses`이고, 형식 지운 진입점 `AddPassWithAccesses(name, kind, flags, Span<const GraphResourceAccess>, GraphRenderTarget, GraphPassExecutor*)`가 경계다. 셰이더 파라미터 구조체(결정 R4)가 들어오면 구조체 layout의 그래프 자원 멤버를 훑는 수집기가 같은 목록을 만들어 이 진입점으로 넣는다(멤버 종류 → 접근: Raster의 SRV `SrvGraphics`, Compute의 SRV·UAV `SrvCompute`·`UavCompute`, 접근만 선언은 그대로, 렌더 타깃 slot은 `GraphRenderTarget`).
- pass 종류: Raster(렌더 타깃 하나 필수, UAV 금지), Compute, Copy(복사 접근만). AsyncCompute는 `Unsupported`(RG4). 표시는 `HasSideEffects`. 접근 표시 `NoUavBarrier`.
- 자원: `CreateTexture`(용도 RenderTarget)·`CreateBuffer`(0으로 채움), `ImportTexture`·`ImportBuffer`(`RenderHandle`의 lease, 처음·마지막 상태, mip 수), `ExportTexture`·`ExportBuffer`(마지막 상태 기본 `SrvGraphics`·`VertexOrIndex`, 풀 자원은 새 `RenderHandle`로 소유 이전, 실패한 그래프의 export handle은 Failed).
- 점진 culling: 쓰기 접근이 subresource의 마지막 생산자가 되고 뒤 접근은 그 생산자에 간선을 단다. 부수 효과 pass·스왑체인 타깃·import 자원에 쓰는 pass는 AddPass 때, export 자원의 마지막 생산자는 Execute 때 살리고 간선을 거꾸로 따라 살린다. Immediate 모드는 culling하지 않는다.
- 수명: 남은 pass의 참조 수, 첫·마지막 사용 pass(`GraphLifetime`). 첫 사용 pass 직전에 풀에서 받고, 풀 반환은 그래프 끝이다(프레임 안 재사용은 RG2).
- 전이: subresource마다 추적한다. Deferred는 연속한 pass의 접근을 합칠 수 있으면(UE `IsMergeAllowed`: 성립하는 조합, 다음이 UAV면 둘 다 `NoUavBarrier`, 표시가 같음) 한 구간으로 넓혀 그 첫 pass prologue에서 한 번 바꾼다. 그래서 읽기 → 읽기 전이가 없고 UAV → UAV는 생략 표시가 없을 때만 UAV barrier다. 필요한 전이만 남겨 같은 pass·자원·전후 상태의 연속 mip을 범위로 묶는다. 만든 자원의 첫 전이 before는 `Unknown`이다(내용이 필요 없다, 장치 추적 상태에서 바꾼다). Raster pass 렌더 타깃(mip 0)의 전이는 attachment로 표시하고 장치 `BeginOffscreen`이 낸다. 그래프 끝에 export·import 마지막 상태로 바꾼다(실행한 pass가 쓰지 않은 subresource는 그대로).
- 실행: 선언 순서. Raster pass마다 장치 기록(`BeginOffscreen`·`BeginFrame` … `EndFrame`)을 열고, Compute·Copy pass는 열린 기록에 이어 쓴다(열린 기록이 없으면 `Unsupported`, GX4). prologue 전이를 `Transition` 한 번으로 내고 pass 함수를 부른다. pass마다 CPU 구간(`CpuScope`, pass 이름)과, `RenderGraphDesc::trace`가 있으면 `GpuScope`(marker·timestamp)를 둔다. 그래프의 기록 전체가 GPU 프레임 하나다(`GpuTrace::BeginFrame`·`EndFrame`을 그래프가 부른다).
- Immediate 모드: AddPass가 그 pass를 할당·전이(합치지 않음)·실행한다. 전이를 뺀 장치 호출 열과 각 pass가 보는 상태는 Deferred와 같다.
- 검증(모든 구성): 생산 전 읽기, 한 pass 안 상충 접근, Raster pass의 UAV, pass 종류에 맞지 않는 접근, 렌더 타깃 없는 Raster·렌더 타깃 있는 Compute·Copy, Retire된(또는 모르는) handle import, 생산 전 export, 다른 그래프의 handle, 이중 export. 실패한 호출은 오류를 돌려주고 `LastError`(영어)·로그(`RenderGraph` 카테고리 Warning)에 남기며 그 뒤 `Execute`는 첫 오류로 실패한다(그래프 전체를 버린다). 정리(열린 기록 제출·풀 반환·lease 반환·export handle Failed)는 실패해도 끝까지 한다.
- 풀(`GraphResourcePool`): 키는 텍스처 desc(크기·`PixelFormat`·용도·mip·샘플러)·버퍼 크기·용도. byte 추정은 `MipChainBytes`(PixelFormatInfo)다. 그래프 텍스처는 번호표 안의 비압축 형식만 받는다. 그래프 사이에서 GPU 완료를 기다리지 않고 다시 쓴다(단일 queue). 축출은 쓰지 않는 중·마지막 사용 fence 완료·`retainFrames`(기본 3) 경과를 모두 만족하는 것을 풀 byte 합이 `budgetBytes`(기본 256 MiB)를 넘는 동안 오래된 순으로 장치 Retire한다. 그래프 Execute 끝에 부른다. `Clear`는 쓰지 않는 자원 전부(종료).
- lease: `RenderHandleTable::AcquireLease`(Ready만, Retired·모름은 `StaleHandle`)·`ReleaseLease`·`LeaseCount`. lease가 남은 handle의 `BeginRetire`는 Retired로 막고 장치 handle을 넘기지 않는다(`deferred`). 마지막 lease가 풀리면 `TakeReleasedRetire`가 장치 handle을 넘기고, 렌더 lane은 프레임·자원 작업 끝에 그것을 장치 Retire와 fence 통지로 잇는다.
- 렌더 lane 연결: `RenderFrameContext::Arena()`(그 프레임 slot의 FrameArena)와 `MakeRenderGraphDesc(context, pool, trace, mode)`. 그래프 수명 표는 모두 그 arena에 두고, pass 함수(람다 캡처)는 그래프 파괴 때 렌더 lane에서 부순다. 그래프의 마지막 제출이 그 slot의 GPU 완료 표지가 된다.
- 아직 하지 않은 것: 셰이더 파라미터 구조체 수집기(RB4·SH2), 그래프 공유 값(`GraphSharedData`), attachment load·MRT·깊이·렌더 타깃 없는 기록(GX4), 버퍼 용도·텍스처 차원 확장(GX2·픽셀 형식), 콘솔 설정으로 즉시 모드·검증 전환(CP4 연결), 그래프 안 업로드·프레임 안 풀 재사용·분리 전이(RG2), 병렬 기록·병합(RG3), async compute(RG4), aliasing(RG5).

## 10. 검증

- CPU: `FenceTimeline`·`DeviceOwnership`·`VertexFormat` 번호표·`ValidateVertexLayout` 한계·정점 패킹(half 65536개 왕복 포함)·Engine 종료 hook·`RenderLane`(기록 backend 대조, slot 대기와 펌프, 등록 타입·Object 참조 검사, 엔진 종료 순서, 소유권 이관 중 표면 작업, 장치 손실, readback·timestamp 통지가 GPU 완료 gate 뒤에만 오는지, timestamp 미지원 "없음" 공개)·`RenderHandleTable`·업로드 ring(fence gate·정렬·한도·wrap)·staging pool(회수·재사용·예산)·timestamp 링(fence gate·NotReady 재시도·`Busy`·순서·무효 표시)·calibration 변환 시험. 모두 이벤트 gate로 순서를 만들고 sleep에 기대지 않는다.
- GPU(`Sandbox --self-test`): D3D12(하드웨어·WARP)·OpenGL ES 미리보기(ES 3.2 profile·GL 4.3 core context)에서 정점 형식 20종 픽셀 대조(OpenGL `Bgra8Unorm`은 음성 대조)·다중 stream·인스턴스 stream·indexed(UInt16·UInt32)·정점 버퍼 없는 그리기·셰이더 입력 음성 대조, fence 대기·통지·작업 이벤트·회수, 소유권 이관 뒤 다른 스레드의 그리기, 렌더 lane Thread·Inline 모드로 엔진 루프 프레임(offscreen 픽셀 대조·숨김 창 스왑체인 present·resize 표면 작업·종료 hook)을 확인한다. GX8: 텍스처·정점 버퍼 내용 갱신, ring wrap(256 KiB × 20회, 전용 staging 0), staging 직접·재배치 업로드와 fence 뒤 회수·재사용, 비동기 readback = `ReadPixels`, 다른 스레드 생성(D3D12 그리기까지, OpenGL `WrongThread`). GX9: timestamp 쌍(끝 ≥ 시작), slot 수보다 많은 프레임의 순서대로 회수, calibration으로 옮긴 GPU 구간이 앞뒤 CPU 시각 사이(여유 1 ms), marker 짝 맞춤. RG1(D3D12·WARP·D3D11·WARP·OpenGL ES 두 context): 그래프 텍스처에 그린 장면을 다음 pass가 SRV로 읽어 import 출력에 합성(픽셀), 쓰이지 않는 pass 제거, export한 history를 다음 그래프가 import해 같은 결과, 풀 재사용, 검증 오류 0(D3D12는 명시 전이 before 불일치 포함).
- CPU(RG1, `RenderGraph/Tests`, 기록 backend): culling(부수 효과·외부 출력·export 보존, 생산자 체인), 기대 전이 열(읽기 → 읽기 0, UAV → UAV 생략 표시, attachment, mip 범위), 수명 pass 번호와 풀 재사용, 즉시·지연 동치(전이를 뺀 호출 열·검증 0·최종 상태, 병합 없는 그래프는 전이까지), 음성 검증, 풀 축출 fence gate, import lease와 Retire 미룸(표·렌더 lane), export → import, pass마다 GpuScope, 렌더 lane 프레임 작업 안 그래프.
