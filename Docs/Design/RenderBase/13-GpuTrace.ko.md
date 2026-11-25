## 13. GPU 계측(RB7)

### 13.1 UE 5.8 방식

- GPU 구간 기록은 RHI가 맡고 RenderCore 헤더는 매크로만 둔다: draw event → RHI breadcrumb, GPU stat → RHI GPU 프로파일러(`RC/Public/ProfilingDebugging/RealtimeGPUProfiler.h:102–134`). breadcrumb은 Debug·Development·GPU 프로파일·통계를 켠 비-Shipping 빌드에서 켜진다(`RHI/Public/RHIBreadcrumbs.h:26`, `RHIDefinitions.h:74`).
- 구간 시작·끝마다 프로파일러 이벤트와 timestamp query를 넣는다(D3D12 `D3D12RHI/Private/D3D12CommandContext.cpp:175–176`, OpenGL은 시작 TOP·끝 BOP query와 debug group, `OpenGLDrv/Private/OpenGLDrv.cpp:119–165`). 이벤트 종류는 프레임 경계·작업 시작·끝(CPU·GPU 시간 대응)·fence signal·wait다(`RHI/Public/GPUProfiler.h:92–236`). OpenGL은 GPU가 언제 일하는지 몰라 프레임 전체를 바쁨으로 표시한다(`OpenGLDrv/Private/OpenGLDevice.cpp:1531–1537`).
- CPU·GPU 시간 대응: D3D12는 queue의 clock calibration과 timestamp 주파수(`D3D12RHI/Private/D3D12Submission.cpp:871–876`, copy queue는 어댑터가 지원할 때만), Vulkan은 calibrated timestamps 확장(`VulkanRHI/Private/VulkanDevice.cpp:1702`).
- 동적 해상도 예산은 별도 timestamp 풀을 쓰고 몇 프레임 늦게 비차단으로 회수한다(`RC/Private/RenderGraphEvent.cpp:166, 197–256`).
- 캡처 도구는 모듈식 제공자(시작·끝·프레임 캡처)이고 RenderDoc·PIX·Xcode 플러그인이 구현한다(`RC/Public/IRenderCaptureProvider.h:71–88`, `RC/Public/RenderCaptureInterface.h:20–30`).

### 13.2 Rocks 규칙

- **`GpuScope`.** 이름 구간 하나가 debug marker push·pop과 timestamp 한 쌍을 함께 기록한다. 렌더 그래프는 pass마다 자동으로 구간을 둔다(계획 11.3절 "pass 이름 기록"). Release에서는 marker를 끄고 timestamp는 설정으로 켠다. CPU 쪽 구간은 CP5와 같은 trace에 상관시킨다. 기록 형과 GPU 시간 변환 경계는 [다중 스레드 trace 설계](../TraceSystem.ko.md) 9절이다(RB7은 TR2 뒤).
- **timestamp query**(GX9): 프레임 slot 링으로 회수하고 결과는 완료 통지 큐(5.2절)로 넘긴다. 결과가 아직 없으면 "미완료"다. disjoint 표시가 있으면 그 구간을 무효로 한다(계획 29.4절). GLES timestamp는 확장(`GL_EXT_disjoint_timer_query`)이 있을 때만이고, 없으면 GPU 시간을 "없음"으로 공개하며 추정값을 만들지 않는다(UE는 Android에서 같은 확장 함수를 쓴다, `OpenGLDrv/Private/Android/AndroidOpenGLFunctions.h:77`).
- **CPU·GPU 시간 대응**(계획 29.1절 calibration pair): D3D12 queue clock calibration, Vulkan calibrated timestamps(13.1절과 같다). GLES는 `GL_TIMESTAMP` 조회(GLES는 `GL_EXT_disjoint_timer_query`의 `glGetInteger64vEXT`)와 CPU 시각을 잇달아 읽어 쌍을 만들고 조회 지연만큼 오차를 둔다(GX9 결정). 확장이 없으면 "없음"이다. Metal은 SH5 때 정한다.
- **계수**: 그래프 pass 수·draw·dispatch 수·transient 최대 사용량·파이프라인 캐시 적중(계획 11.3절).
- **후속(Developer 모듈)**: 캡처 도구 연동(RenderDoc·PIX), 그래프 자원 덤프, 텍스처 시각화, 셰이더 → CPU 메시지, GPU 오류 주입(개발 빌드 전용, 계획 11.2절 device loss 시험에 쓴다), 긴 GPU 작업 벤치마크.

### 13.3 완료 기준(RB7)

`GpuScope` 중첩이 캡처 도구(PIX·RenderDoc)에서 pass 이름으로 보이는지(수동 확인 기록), timestamp 링 회수가 GPU 완료 이벤트 뒤에만 값을 내는지(gate), GLES 확장 없음에서 "없음" 공개, calibration pair로 CPU·GPU 구간이 같은 시간축에 놓이는지.

**구현(2026-11-17).** `RenderBase/GpuTrace.h`에 `GpuTrace`(queue 하나의 계측 장부)·`GpuScope`(스택 객체)·`GpuTraceSettings`·`RegisterGpuTraceTypes`·`GpuTicksToTraceTicks`를 두었다. RenderBase가 TraceCapture에 의존한다. 2026-11-20부터 RenderBase 모듈 수명 객체의 `Startup`이 호스트 trace 세션 서비스에 등록하고 `Shutdown`이 `UnregisterGpuTraceTypes`로 푼다([모듈 로더](../ModuleLoader/03-Design/03.12-HostServices.ko.md) 3.12절).
- 흐름: 렌더 lane이 프레임 기록 안에서 `GpuTrace::BeginFrame`(queue 정의·calibration·프레임 시작 timestamp) → `GpuScope`(marker push + 시작 timestamp, 소멸 때 끝 timestamp + marker pop) → `EndFrame`(프레임 끝 timestamp)를 부른다. 완료 통지를 받는 쪽(보통 게임 lane)이 `Consume(notices)`로 `GpuTimestamp` 통지를 짝지어, 두 끝이 모두 온 구간만 trace tick으로 바꿔 채널 `gpu`에 쓴다. 완료 전 구간은 pending이고 trace에 나가지 않는다. 끊김(`Cancelled`) 통지가 하나라도 있으면 그 구간은 버리고 `invalid`로 센다.
- GPU 시간은 채널 `gpu`·설정 `timestamps`·장치 `timestamps`·`clockCalibration`이 모두 참일 때만 잰다. 아니면 queue 정의(`Trace.GpuQueue`)의 GPU 시간 칸을 0으로 공개하고 timestamp를 쓰지 않는다(추정값 없음). marker는 설정 `markers`만 따른다. 기본값은 둘 다 Release에서 꺼짐이다(설정으로 켠다).
- calibration: `ClockCalibration`에 `cpuTicks`(`platform::MonotonicTicks`, trace 시간축)와 `uncertaintyNanoseconds`를 더했다. D3D12는 queue clock calibration이 한 번에 읽은 QPC 값과 CPU tick 하나 길이, OpenGL은 `GL_TIMESTAMP` 조회를 CPU tick 두 번으로 감싼 가운데 값과 조회 지연의 절반이다. 쌍은 세션 첫 프레임과 `calibrationIntervalFrames`(기본 60)마다 다시 얻어 정의 기록(`Trace.GpuCalibration`)으로 남기고, 그 사이 구간은 가장 최근 쌍으로 바꾼다(두 시계의 drift는 보정하지 않는다).
- GPU 구간 이름은 CPU 구간과 같은 세션 이름 표(`Trace.ScopeName`)를 쓴다(`TraceResolveScopeName`). 같은 pass 이름은 CPU·GPU에서 같은 id다.
- 시험(`RenderBase/Tests/GpuTraceTests.cpp`, 기록 backend): GPU 완료 gate 전에는 구간 0·pending 3이고 gate 뒤 구간 2·프레임 1, 변환한 tick이 기록된 calibration 쌍으로 계산한 값과 정확히 같고 CPU 구간 시작 ≤ calibration CPU tick ≤ GPU 프레임 시작, 중첩 깊이 0·1과 marker push·push·pop·pop 순서, timestamp 미지원 장치의 "GPU 시간 없음" 공개(구간·calibration 기록 0, timestamp 쓰기 0), Release 기본 설정(marker·timestamp 0), 끊김 통지의 구간 무효.
- 스펙과 다른 점: (1) GPU 구간은 시작·끝 기록 둘이 아니라 두 tick을 가진 기록 하나(`Trace.GpuScope`)다. 두 끝이 모두 온 뒤에만 쓰므로 짝을 잃은 반쪽이 생기지 않는다. (2) "GPU 시간 없음"은 별도 정의 형이 아니라 queue 정의의 GPU 시간 칸(0)이다. (3) 작업 시작·끝·fence signal·wait 사건은 아직 없다(다중 queue와 함께). (4) 렌더 그래프 pass 자동 구간은 RG1에서 붙였다(그래프가 기록 전체를 GPU 프레임 하나로 묶고 pass마다 `GpuScope`를 둔다, 8절). 그래프 밖 렌더 코드는 `BeginFrame`·`GpuScope`·`EndFrame`을 직접 부른다. (5) queue는 `GpuTrace` 하나당 하나다(설정 `queueId`·`queueName`).
- **캡처 도구 수동 확인 절차**(PIX·RenderDoc, 자동 시험 밖): Sandbox를 D3D12(PIX on Windows) 또는 OpenGL(RenderDoc)로 실행해 프레임을 캡처한다. 이벤트 목록에서 `GpuScope` 이름이 중첩 그대로 보이는지(바깥 구간 아래 안쪽 구간), 프레임 끝에 열린 marker가 남지 않는지를 본다. Release 구성에서는 marker가 보이지 않아야 한다. 결과는 날짜·backend·도구 판과 함께 이 절에 적는다(아직 기록 없음).
