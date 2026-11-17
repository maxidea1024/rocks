# 렌더 기반 구현 범위와 계약

기준일: 2026-11-17. [렌더 기반 설계](../Design/RenderBase.ko.md)의 GX7·GX10·GX11·CP6·RB1·GX3a·GX8·GX9를 구현한 범위와 사용 계약이다. 설계 결정·근거는 설계 문서가 기준이고, 이 문서는 지금 코드가 지키는 계약과 아직 하지 않은 것을 적는다.

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
- OpenGL: 정규화 없는 정수 형식은 `glVertexArrayAttribIFormat`, 인스턴스 stream은 binding divisor 1이다. `Bgra8Unorm`은 `Unsupported`다(Core에 `UnsupportedCapability` 코드가 없다).
- 그리기: `SetVertexBuffers(첫 stream, {버퍼, offset})`·`SetIndexBuffer(버퍼, UInt16|UInt32, offset)`·`Draw(정점 수, 인스턴스 수, 첫 정점)`·`DrawIndexed(index 수, 인스턴스 수, 첫 index)`. base vertex·base instance는 0이다. 배치가 선언한 stream에 버퍼가 없으면 그리기를 무시하고 `IgnoredDraws`를 올린다. index 값 범위는 검사하지 않는다(장치가 내용을 CPU에 남기지 않는다).
- CPU 패킹 `Core/Math/VertexPacking.h`: snorm·unorm 8·16(반올림은 가운데에서 0에서 먼 쪽, double 곱으로 호스트 무관), 10:10:10:2, `PackNormal`·`PackTangent`(Byte4Snorm, w = ±127). half는 기존 `Float16`(round-to-nearest-even, 비정규수·무한대·NaN 보존).

## 6. 업로드·readback(GX8)

- 업로드 ring(`Graphics/UploadRing.h`, `DeviceDesc::uploadRingBytes`, 기본 4 MiB): 연속 구간 할당, 제출 때 fence로 묶음을 닫고 GPU 완료 뒤 회수한다. 열린 묶음은 어떤 fence로도 회수하지 않는다. 다 회수돼도 위치를 이어 써서 링 전체를 돌며, 끝에 자리가 없으면 0으로 감는다(`UploadStats::ringWraps`).
- 내용 갱신: `UpdateBuffer(버퍼, offset, byte)`·`UpdateTexture(텍스처, mip, 픽셀)`. D3D12는 업로드 기록(별도 command list)에 쌓고 다음 제출 앞에 실행한다(`EndFrame`·`SubmitUploads`·`WaitIdle`). 텍스처 행 간격은 256 byte, 배치는 512 byte 정렬로 ring 안에서 맞춘다. OpenGL은 즉시 `glNamedBufferSubData`·`glTextureSubImage2D`다. 범위 밖·크기 불일치는 `InvalidArgument`다.
- 스트리밍 staging pool(`Graphics/StagingPool.h`, `DeviceDesc::stagingBudgetBytes`, 기본 256 MiB): `AcquireStaging` → CPU가 채움(IO 목적지) → `UploadTextureFromStaging`·`UploadBufferFromStaging` → `ReleaseStaging`. 놓은 staging은 그 업로드의 fence가 끝난 뒤에만 재사용 목록으로 간다. 재사용은 요청 크기 이상·두 배 이하일 때이며 handle은 새로 발급한다(놓은 handle은 stale). 예산을 넘으면 빈 staging부터 지운다. 정렬이 맞지 않는 원본(행 간격·offset)은 ring으로 다시 옮겨 올린다. OpenGL staging은 CPU 메모리이고 업로드가 즉시라 바로 돌아간다.
- 비동기 readback: `RequestReadback(RenderTarget)`이 지금까지의 제출 뒤 복사를 제출하고 완료 지점을 준다. `TryReadback`은 완료 전 false(비차단), 완료 뒤 width×height×4 byte(첫 행이 위)를 쓴다. 기록 중이면 `Busy`. D3D12는 READBACK heap 버퍼, OpenGL은 pixel pack buffer다. `ReadPixels`는 시험용 동기 경로로 남는다(D3D12는 같은 readback 경로 + 대기).
- 렌더 lane: `RenderLane::UpdateBuffer`·`UpdateTexture`(lane 작업, FIFO 순서), `RequestReadback`(번호) → `RenderNoticeKind::ReadbackReady`(GPU 완료 뒤) → `TakeReadback`(`RenderBlob`).
- 아직 하지 않은 것: copy queue·프레임당 업로드 대역 상한(스트리밍 단계), 텍스처 배열·3D·압축 형식 업로드(GX2), 업로드 기록의 다른 스레드 제출.

## 7. GPU 계측(GX9)

- 기록: `CommandContext::WriteTimestamp()`가 번호를 돌려준다. 결과는 그 프레임의 GPU 완료 뒤 `Device::CollectTimestamps(out)`(소유 스레드, 비차단, 쓴 순서)가 낸다. 실패: 미지원 `Unsupported`, 기록 밖 `InvalidArgument`, 회수 전 slot만 남음 `Busy`, 프레임당 256 초과 `CapacityExceeded`.
- 프레임 slot 링(`Graphics/TimestampRing.h`): slot 4개 × 256 query. 닫힌 slot은 fence 완료를 본 뒤에만 읽고, 원본이 아직 결과를 내지 않으면(OpenGL query 가용 전) 다음 회수에서 다시 묻는다. 제출이 실패한 slot의 결과는 무효(disjoint)로 나온다.
- 공개: `DeviceInfo::timestamps`·`timestampFrequency`(D3D12 queue 주파수, OpenGL 10^9)·`timestampDisjointDetection`(OpenGL은 `GL_EXT_disjoint_timer_query`가 있을 때만)·`clockCalibration`·`debugMarkers`. 지원하지 않으면 값을 지어내지 않는다.
- CPU·GPU 시간 대응: `CalibrateClocks()`가 `{gpuTicks, cpuNanoseconds(platform::MonotonicNanoseconds 시간축), gpuFrequency}`를 주고 `GpuTicksToCpuNanoseconds`로 옮긴다. D3D12는 `GetClockCalibration`(QPC → ns, `TicksToNanoseconds`), OpenGL은 `glGetInteger64v(GL_TIMESTAMP)` 앞뒤 CPU 시각의 가운데다. drift 보정은 없으므로 프레임마다 새로 얻는다.
- debug marker: `PushMarker(이름)`·`PopMarker()`. 이름은 255 byte(UTF-8 글자 경계)로 자른다(`ClipDebugMarkerName`). D3D12는 PIX가 읽는 UTF-16 이벤트, OpenGL은 debug group이다. 프레임 끝에 열린 구간은 장치가 닫고, 짝 없는 pop은 무시하고 `IgnoredDraws`를 올린다.
- 렌더 lane: 프레임 GPU 완료 처리에서 결과를 `RenderNoticeKind::GpuTimestamp`(frame = 번호, value = tick, 끊김이면 error `Cancelled`)로 넣고 그 뒤 `FrameCompleted`를 넣는다.
- 아직 하지 않은 것: `GpuScope`와 그래프 pass 자동 구간·Release 설정(RB7), 캡처 도구(PIX·RenderDoc)에서 pass 이름이 보이는지의 수동 확인 기록.

## 8. 검증

- CPU: `FenceTimeline`·`DeviceOwnership`·`VertexFormat` 번호표·`ValidateVertexLayout` 한계·정점 패킹(half 65536개 왕복 포함)·Engine 종료 hook·`RenderLane`(기록 backend 대조, slot 대기와 펌프, 등록 타입·Object 참조 검사, 엔진 종료 순서, 소유권 이관 중 표면 작업, 장치 손실, readback·timestamp 통지가 GPU 완료 gate 뒤에만 오는지, timestamp 미지원 "없음" 공개)·`RenderHandleTable`·업로드 ring(fence gate·정렬·한도·wrap)·staging pool(회수·재사용·예산)·timestamp 링(fence gate·NotReady 재시도·`Busy`·순서·무효 표시)·calibration 변환 시험. 모두 이벤트 gate로 순서를 만들고 sleep에 기대지 않는다.
- GPU(`Sandbox --self-test`): D3D12(하드웨어·WARP)·OpenGL에서 정점 형식 20종 픽셀 대조(OpenGL `Bgra8Unorm`은 음성 대조)·다중 stream·인스턴스 stream·indexed(UInt16·UInt32)·정점 버퍼 없는 그리기·셰이더 입력 음성 대조, fence 대기·통지·작업 이벤트·회수, 소유권 이관 뒤 다른 스레드의 그리기, 렌더 lane Thread·Inline 모드로 엔진 루프 프레임(offscreen 픽셀 대조·숨김 창 스왑체인 present·resize 표면 작업·종료 hook)을 확인한다. GX8: 텍스처·정점 버퍼 내용 갱신, ring wrap(256 KiB × 20회, 전용 staging 0), staging 직접·재배치 업로드와 fence 뒤 회수·재사용, 비동기 readback = `ReadPixels`, 다른 스레드 생성(D3D12 그리기까지, OpenGL `WrongThread`). GX9: timestamp 쌍(끝 ≥ 시작), slot 수보다 많은 프레임의 순서대로 회수, calibration으로 옮긴 GPU 구간이 앞뒤 CPU 시각 사이(여유 1 ms), marker 짝 맞춤.
