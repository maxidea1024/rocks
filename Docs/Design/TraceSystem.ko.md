# 다중 스레드 trace 설계

기준일: 2026-11-18, 개정 7. 여러 스레드의 CPU 구간·카운터·표지·task 수명·GPU 구간을 하나의 시간축에 기록하는 **trace 수집**(TraceCapture)과, 이를 읽는 **trace 분석**(TraceAnalysis)을 정한다.

[렌더 기반](RenderBase.ko.md)의 선행 작업 CP5(다중 스레드 CPU 구간 계측·카운터·GPU 상관)이고 RB7(`GpuScope`, RenderBase 13절)의 기반이다. 요구는 다음 문서에서 온다.
- [엔진 구현 계획](EngineImplementationPlan.ko.md) 29.1절(계측 체계), 29.4절(필수 로깅·프로파일러), 29.5절(OB01–OB06)
- [입력 상세 계획](CoreObjectInputDetailedPlan.ko.md) C18(loss-aware trace·headless report)

UE 근거는 UE 5.8.4(`release`, 로컬 클론)의 현재 기본 경로다. 경로 약어는 다음과 같다.

| 약어 | 경로(`Engine/Source/` 기준) |
|---|---|
| `TL` | `Runtime/TraceLog/` |
| `Core` | `Runtime/Core/` |
| `RHI` | `Runtime/RHI/` |
| `TA` | `Developer/TraceAnalysis/Private/Analysis/` |
| `TS` | `Developer/TraceServices/Private/Analyzers/` |

계획 29.4절대로 형식·API·분석기는 독립 구현이다. UE trace 파일과 호환하지 않는다. 옛 stats 경로와 옛 GPU profiler는 보지 않았다.

상태: **설계 확정**(결정 T1–T8), **TR1–TR4 구현**(2026-11-17, `feature/trace-capture`). 단계는 10절이다. 구현하며 구체화한 점은 각 절의 "TR1 구현" 항목에 적었다.

## 1. 결정 요약

| 항목 | 결정 | UE 5.8 방식과 근거 |
|---|---|---|
| 기록 단위 | 스레드마다 고정 크기 블록에 잠금 없이 이어 쓰고, 쓴 끝을 release-store로 공개한다. writer 하나가 공개된 부분만 읽는다 | UE와 같다. TLS 블록 4 KiB, 헤더는 블록 끝, `Committed`(`TL Public/Trace/Detail/Writer.inl:18–35`, `TL Private/Trace/BlockPool.cpp:81–86`) |
| 메모리가 다 찼을 때 | **기다리지 않고 버린다.** 스레드·채널별 손실 수와 손실 구간을 기록한다 | UE는 생산자가 최대 4초 spin하고 그다음 모든 채널을 끄는 panic이다(`BlockPool.cpp:191–209`, `TL Private/Trace/Trace.cpp:165–172`). 이벤트별 손실 수는 없다. 계획 29.4절은 실행 스레드 비차단·손실 표시를 요구하고, 오디오·할당기 같은 임계 구간이 trace 때문에 기다리면 안 된다 |
| 시간 | 모든 기록은 단조 tick 하나를 쓴다. 스트림 머리에 기준 tick과 주파수를 둔다. GPU 시간은 **기록 전에** 이 tick으로 바꾼다 | UE와 같다. `Cycles64`=QPC, `NewTrace(StartCycle, CycleFrequency)`(`TL Private/Trace/Writer.cpp:968–998`). GPU→CPU 변환은 RHI 제출 단계에서 한다(`D3D12RHI/Private/D3D12Submission.cpp:868–877, 1229–1238`, `RHI Public/GPUProfiler.h:83–89`) |
| 채널 | 이름 있는 켜기·끄기 단위다. 꺼져 있으면 relaxed 읽기와 분기 하나로 끝난다. **모듈 등록 함수에서 명시적으로 등록하고, 등록 전에는 꺼져 있다** | UE는 정적 생성자로 등록하고 정적 초기화 동안에는 모두 켜져 있다(`TL Public/Trace/Detail/Channel.inl:16–19`, `TL Private/Trace/Channel.cpp:148–198`). Rocks는 전역 생성자를 금지한다 |
| 정의 기록 | 이벤트 스키마·구간 이름·카운터·스레드 정보처럼 늦게 연결해도 필요한 기록은 세션 소유 저장소에 보관하고, 새 출력이 붙으면 다시 보낸다. **상한을 두고**, 넘친 수를 기록한다 | UE important 이벤트와 캐시는 상한 없이 프로세스 수명 내내 커진다(`TL Private/Trace/Important/Cache.cpp:68–89, 127–187`) |
| 순서 | 스레드 사이 순서가 필요한 기록(task 의존·카운터·fence)만 전역 순서 번호를 받는다. CPU 구간은 tick으로 정렬한다 | UE Sync/NoSync 구분과 같다(`TL Public/Trace/Detail/LogScope.inl:115–138`) |
| CPU 구간 인코딩 | 스레드별 작은 배치에 시작·끝을 tick 차이 varint로 쌓았다가 이벤트 하나로 낸다 | UE `EventBatchV3`(`Core Private/ProfilingDebugging/CpuProfilerTrace.cpp:48–63, 167–198`) |
| 동적 이름 | 세션 전역 이름 표(삽입만 잠금)와 스레드별 앞단 캐시를 둔다. 같은 이름은 id 하나다 | UE는 스레드별 맵이라 같은 이름이 스레드 수만큼 생긴다(`CpuProfilerTrace.cpp:661–792`) |
| 출력 | 파일, 최근 기록 고리(메모리), 사용자 콜백. 실시간 네트워크 연결은 후속(10절)이다 | UE 파일·TCP·relay·tail(`Writer.cpp:1343–1422`, `TL Private/Trace/Tail.cpp`) |
| 압축 | 전송 덩어리를 Core Compression의 LZ4 raw block으로 압축한다 | UE는 384 B를 넘는 덩어리를 LZ4로 압축(`Writer.cpp:536–589`). Rocks는 자체 구현 금지, 공용 Compression을 쓴다 |
| writer 구동 | 전용 writer 스레드가 주기적으로 비운다. 시험·헤드리스는 수동 펌프 모드로 결정적으로 비운다 | UE 17 ms 주기와 수동 `Trace::Update`(`Writer.cpp:1046–1115`, `TL Public/Trace/Trace.h:310–314`) |
| 출시 빌드 | 수집 코드는 컴파일에서 빠진다. 출시 빌드에는 crash breadcrumb만 둔다(후속 설계) | UE `UE_TRACE_ENABLED=0`(`TL Public/Trace/Config.h:12–22`), 계획 29.1절 |

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| trace 세션 `TraceSession` | TraceLog 전역 상태 | 한 실행의 수집 소유자(채널 표·블록 풀·정의 저장소·출력) |
| 스레드 블록 `ThreadBlock` | `FWriteBuffer` | 한 스레드가 쓰는 고정 크기 기록 블록 |
| 블록 풀 `BlockPool` | block pool | 상한이 있는 블록 재사용 저장소 |
| 채널 `TraceChannel` | `FChannel` | 켜기·끄기 단위 |
| 채널 묶음 `TracePreset` | channel preset | 이름 하나로 켜는 채널 목록 |
| 정의 기록 `DefinitionRecord` | important event | 늦게 붙은 출력에도 다시 보내는 기록 |
| 순서 번호 `OrderNumber` | serial(Sync) | 스레드 사이 순서가 필요한 기록의 전역 번호 |
| CPU 구간 `CpuScope` | `TRACE_CPUPROFILER_EVENT_SCOPE` | 이름 있는 스레드 안 시간 구간 |
| 카운터 `TraceCounter` | counters trace | 시간에 따라 바뀌는 수치 |
| 표지 `TraceMark` | bookmark | 한 시점의 이름 있는 사건 |
| 시간 띠 `TraceSpan` | region | 스레드와 무관한 이름 있는 시간 구간 |
| 문맥 `TraceContext` | metadata trace stack | task 경계를 넘겨 전파하는 상관 id 묶음 |
| 최근 기록 고리 `RecentRing` | tail | 최근 압축 덩어리를 보관하는 고정 크기 고리 |
| 스냅샷 `TraceSnapshot` | snapshot | 고리와 정의 기록을 파일 하나로 내보내기 |
| trace 분석 `TraceAnalysis` | TraceAnalysis·TraceServices | 스트림을 읽어 시간축 모델로 만드는 Developer 모듈 |
| Rocks Lens | Unreal Insights | 오프라인·라이브 viewer(후속) |

## 3. 모듈과 수명

**모듈**
- `TraceCapture`(Core Runtime): 세션·채널·블록·writer·출력·CPU 구간·카운터·표지·시간 띠·문맥.
  - 기반 모듈: Base·PlatformBase·Compression·Core(로그) 위에 선다.
  - Task 모듈은 TraceCapture를 쓰는 쪽이다(7절). TraceCapture는 Task에 의존하지 않고, writer 스레드도 Task가 아니라 플랫폼 스레드다. 일반 scheduler가 끝난 뒤에도 비워야 하기 때문이다(입력 계획 :427).
  - **모듈 순환 해소**(2026-11-17): Compression의 Task 의존 부분(비동기 API·블록 컨테이너 병렬 API)을 모듈 `AsyncCompression`으로 떼었다([압축](Compression.ko.md) 4절). Compression은 Core와 codec에만 의존하므로 계층은 아래부터 Compression, TraceCapture, Task, AsyncCompression 순이고, TR3에서 Task가 TraceCapture를 써도 순환이 없다.
- `TraceAnalysis`(Core Developer): 스트림 읽기·모델·report. 실행 파일에는 들어가지 않는다.
- Rocks Lens(viewer): 후속. 계획 29.4절의 단계 R2·M3에 맞춘다.

**세션 수명**
- 시작은 `TraceSession::Start(설정)`이다. 이때 블록 풀·정의 저장소·writer 스레드를 만든다. 전역 싱글턴 대신 엔진 또는 시험이 소유하고, 기록 API가 쓰는 현재 세션 포인터 하나만 원자적으로 공개한다. 세션이 없으면 모든 기록은 분기 하나로 끝난다.
- 종료는 계획 29.4절 순서를 따른다.
  1. 새 기록 입장 차단
  2. 생산자 종료 확인(등록된 스레드의 블록 은퇴)
  3. 마지막 비우기
  4. 출력 닫기
  5. writer join
  6. 풀 해제
- **TR1 구현**: 2단계는 "진행 중인 기록 호출이 모두 끝남"을 확인한다. 스레드마다 TLS 칸에 진행 중 표시(busy)를 두고, 기록 호출은 표시를 올린 뒤(seq_cst) 현재 세션을 읽는다. Stop은 현재 세션을 거둔 뒤(seq_cst) 모든 TLS 칸의 표시가 내려가기를 기다린다. 아직 살아 있는 스레드의 블록은 그 시점 내용으로 마감하고, 그 스레드의 다음 기록은 세션 serial이 달라 새 세션에 다시 등록된다. Thread 모드에서는 3·4단계를 writer 스레드가 하고 Stop이 join한다.
- 스레드 종료 때 그 스레드 블록을 은퇴로 표시한다. writer가 남은 내용을 보낸 뒤 블록을 풀에 돌려준다(UE `TlsBuffer.cpp:429–439`와 같다). TR1은 소멸자가 있는 `thread_local` 칸으로 종료를 안다.

## 4. 기록 구조

### 4.1 스레드 블록과 블록 풀

- **블록**
  - 크기는 4 KiB이고, 머리 정보는 블록 끝에 둔다. 쓰기 위치는 앞에서 뒤로 자란다.
  - 생산자는 쓴 끝(`committed`)을 release-store로 공개한다. writer는 `[reaped, committed)`만 읽는다.
  - 블록 크기와 페이지 크기(블록 16개)는 이름 있는 상수다.
- **블록 넘김**
  - 기록 하나가 남은 공간에 들어가지 않으면 블록을 은퇴시키고 다음 블록을 받는다.
  - 가변 길이 부분(문자열·배열)은 여러 블록에 걸쳐 나눠 쓴다(UE `TL Private/Trace/Field.cpp:56–84`와 같다).
  - 고정부가 블록보다 큰 이벤트 형은 등록 때 오류다. UE는 기록 때 멈춘다(`LogScope.inl:99–107`).
  - **TR1 구현**: 블록 머리는 24 B(시작 위치·다음 블록·자유 목록 칸)다. 스레드 스트림은 스레드가 공개한 byte 누계로 위치를 매기고, 생산자는 기록을 다 쓴 뒤 누계를 release로 공개한다. 블록보다 큰 기록은 필요한 블록을 모두 먼저 받고, 하나라도 못 받으면 기록 전체를 버린다(일부만 쓰지 않는다). 기록 하나의 상한은 1 MiB(`TraceFormat::MaxRecordBytes`)이고 넘으면 손실로 센다.
- **풀**
  - 잠금 없는 자유 목록이고, 새 페이지 확보만 잠금 하나로 직렬화한다.
  - **상한**은 설정 `trace.pool.maxBytes`(기본 64 MiB)다. 시작 때부터 적용한다. UE는 writer가 돌 때만 상한을 적용한다(`BlockPool.cpp:45`).
- **상한에 닿으면**
  - 블록을 받지 못한 스레드는 그 기록을 버리고, 스레드 손실 수(채널별)를 늘린다.
  - 손실이 시작된 tick도 기억한다. 다음에 블록을 받으면 첫 기록으로 손실 기록(채널별 수, 시작·끝 tick)을 남긴다.
  - 열린 CPU 구간의 끝이 손실되면 분석기는 그 구간을 "끝 없음"으로 표시하고 길이를 지어내지 않는다(계획 29.4절).
  - **TR1 구현**: 손실 상태인 스레드는 다음 기록 때 항상 새 블록을 받아 그 첫 기록으로 손실 기록(`Trace.Loss`)을 쓴다. 손실 상태로 끝난 스레드는 손실 기록을 쓸 기회가 없으므로, writer가 그 스레드의 은퇴 블록을 돌려준 뒤 대신 쓴다. Stop의 마지막 비우기도 남은 손실 기록을 쓴다.
- **통계 기록**
  - writer가 주기적으로 남기는 항목: 풀 사용량·최대 사용량, 출력 대역폭, 정의 저장소 크기, 손실 합계.
  - 풀 사용량이 설정 비율(기본 80%)을 넘으면 경고 로그를 낸다(UE `Core Private/ProfilingDebugging/TraceAuxiliary.cpp:1160–1189`).

### 4.2 이벤트 형과 스키마

- **선언**
  - 이벤트 형은 `TraceEventType` 선언 하나다. 담는 것: 이름, 채널, 필드 표(constexpr 필드 이름·타입·offset), 순서 번호 필요 여부, 정의 기록 여부.
  - 고정부는 trivially copyable struct이고, 기록은 고정부 memcpy와 가변부 덧붙임이다.
  - 필드 타입: bool, 정수 8–64비트, float, double, 문자열(UTF-8), 정수·실수 배열, id 참조.
  - 필드 수 상한은 이름 있는 상수(32)다.
- **id 발급**
  - 형 id는 등록 때 발급한다. 처음 쓸 때 지연 발급하지 않는다(UE `TL Private/Trace/EventNode.cpp:78–120`와 다르다).
  - 등록은 모듈 등록 함수에서 한다. 같은 이름의 다른 스키마를 등록하면 오류다.
  - **TR1 구현**: 같은 이름·같은 스키마라도 다른 객체면 `AlreadyExists`다(다른 스키마는 `TypeMismatch`). 머리(형 id·순서 번호)·고정부·가변 길이 칸의 합이 블록 하나(정의 형은 정의 기록 상한 16 KiB)에 들어가야 한다. 형 id 1–15는 세션 내부 형(`Trace.ChannelInfo`·`Trace.ChannelState`·`Trace.ThreadInfo`·`Trace.Loss`·`Trace.DefinitionLoss`)이고, 그 스키마도 다른 형처럼 스키마 스트림으로 나간다. 사용자 형은 16부터 4095까지다.
- **자기 기술**
  - 스키마는 정의 기록으로 스트림에 낸다. 분석기는 사전 지식 없이 읽는다(UE `NewEvent`, `TL Private/Trace/EventNode.cpp:122–224`와 같다).
  - 모듈을 내린 뒤에도 분석할 수 있도록, 이름·스키마 문자열은 등록 때 세션 소유 저장소로 복사한다. 모듈 주소를 다시 읽지 않는다(입력 계획 :427).
- **리플렉션 연동**: 리플렉션 struct에서 필드 표를 생성하는 방식은 결정 T6(12절)이다.

### 4.3 정의 기록

- **대상**: 이벤트 스키마, 채널 표·켜짐 변경, CPU 구간 이름, 카운터 정의, 스레드 이름·묶음, 표지 형식, 모듈·빌드 id.
- **기록 방식**
  - 정의 기록은 스레드 블록이 아니라 세션의 공유 정의 버퍼에 다중 생산자 bump로 쓴다.
  - writer는 이를 정의 스트림으로 내보내면서, 압축한 사본을 정의 저장소에 보관한다.
  - 새 출력(파일 열기·스냅샷)이 붙으면 정의 저장소 전체를 먼저 보낸다.
- **상한**
  - 정의 저장소 상한은 설정 `trace.definitions.maxBytes`(기본 8 MiB)다.
  - 넘으면 새 정의 기록을 그 출력에만 보내고 저장소에는 넣지 않는다. 넘친 수를 통계 기록에 남긴다.
  - 그 뒤에 붙은 출력은 일부 정의가 빠졌다는 표시를 받는다.
- **TR1 구현**
  - 정의 버퍼 크기는 세션 설정 `definitionBufferBytes`(기본 256 KiB)다. 기록마다 앞에 u32 머리(크기·준비 비트)를 두고 생산자가 내용을 쓴 뒤 머리를 release로 쓴다. writer는 준비된 기록까지만 비운다.
  - 버퍼가 차면 정의 기록은 기다리지 않고 버린다. writer가 누계를 `Trace.DefinitionLoss` 기록으로 정의 스트림에 낸다. 정의가 버려진 적이 있으면 그 뒤 붙은 출력도 "정의 일부 없음" 표시를 받는다.
  - 등록 API(채널 등록)는 버퍼가 차 있으면 버리지 않고 `CapacityExceeded`를 돌려준다. 다음 펌프 뒤 다시 등록할 수 있다.
  - 저장소 상한은 압축된 덩어리 크기로 비교하고, 펌프 한 번에 비운 정의 묶음 단위로 저장하거나 버린다. 그래서 저장소에는 온전한 기록만 남는다.
  - writer는 펌프 시작 때 스레드마다 공개 위치를 먼저 읽고, 그다음 스키마·정의를 비운다. 정의를 쓰는 중인 다른 스레드가 있어 끝까지 비우지 못하면 스레드 기록을 다음 펌프로 미룬다. 그래서 기록은 그 기록이 쓰는 스키마·정의보다 먼저 나가지 않는다.

### 4.4 시간

- **플랫폼 API**: 단조 tick API `platform::MonotonicTicks()`와 `platform::MonotonicTickFrequency()`를 더한다.
  - 같은 원천: `MonotonicNanoseconds`(`Core/Source/Runtime/PlatformBase/Public/Platform/Clock.h:10`)와 같은 원천이어야 한다.
  - 원천별 플랫폼: Windows QPC, macOS·iOS `mach_absolute_time`, Linux·Android `clock_gettime(CLOCK_MONOTONIC)`.
  - 시험: 두 API의 일관성 시험을 둔다.
- 기록은 tick을 담는다. 스트림 머리 기록이 기준 tick, 주파수, 시작 UTC(`UtcTicks`), 세션 GUID, 빌드 id를 담는다.
- GPU 시간은 RB7이 calibration pair로 tick으로 바꾼 뒤 기록한다(9절). 다른 시계(오디오 sample, 서버 tick)는 변환 표 기록을 따로 두고, 같은 숫자로 섞지 않는다(계획 29.1절).

### 4.5 순서 번호

- 순서 번호가 필요한 이벤트 형의 기록만 전역 원자 카운터에서 번호를 받는다.
- 분석기는 스레드 스트림들을 순서 번호 기준으로 합친다(UE `TA Engine.cpp:3499–3540`).
- 출력이 중간에 붙어 번호에 구멍이 생기면 writer가 동기 표지를 낸다. 분석기는 구멍을 "앞부분 없음"으로 표시한다(UE `Writer.cpp:761–781`와 같다).
- **TR1 구현**: 동기 표지는 스트림 기록이 아니라 출력 머리의 칸(출력이 붙을 때의 순서 번호)이다. 그보다 작은 번호의 기록은 그 출력에 없을 수 있다. 순서 번호는 relaxed fetch-add로 발급한다(happens-before가 있는 두 사건은 같은 원자 칸의 수정 순서를 따른다).

## 5. 채널과 묶음

- **채널 등록**
  - 채널은 이름·설명·읽기 전용 여부로 등록한다.
  - 이름은 소문자 점 경로이며, 설정 이름 문법(ConfigSystem 5.1절)과 같은 영역 이름 공간을 쓴다(`cpu`, `task`, `gpu`, `frame`, `log`, `mark`, `counter`, `span`, `context`, `memory.alloc`, `io`, `gc`, `asset`). `span`·`context`는 TR2에서 더했다.
  - UE식 대소문자·접미사 무시 해시(`Channel.cpp:52–84`)는 두지 않는다. 이름은 정확히 일치해야 한다.
- **켜기·끄기**
  - 상태는 원자 정수이고, 읽기는 relaxed다. 여러 채널 조건은 AND로 결합한다.
  - 변경은 정의 기록(채널 변경)으로 스트림에 남긴다.
  - 읽기 전용 채널은 세션 시작 뒤 고정이다.
- **묶음(preset)**
  - 데이터로 정의한다: `Engine/Config/Engine.json`의 `"TracePresets": { "default": [ ... ] }`. 기본 묶음은 `default` = cpu·task·gpu·frame·log·mark·counter다.
  - 설정 계층의 배열 연산으로 프로젝트가 고칠 수 있다.
- **켜는 경로**
  - 명령줄 `--trace=<채널|묶음>,…`
  - 콘솔 명령 `trace.enable`·`trace.disable`·`trace.file`·`trace.stop`·`trace.snapshot`(ConfigSystem CF2 뒤)
  - 코드 API
- **구현(TR5).** 설정 계층과 잇는 일은 새 모듈 `TraceControl`(Config·TraceCapture 위)이 맡는다. TraceCapture는 Core만 안다.
  - 묶음은 설정 struct `TracePresetSettings`(branch `Engine`, 섹션 `TracePresets`)의 필드다. 필드 하나가 묶음 하나이고 도구 이름이 묶음 이름이다(`default`). 기본 목록은 `Engine/Config/Engine.json`에 있다. 프로젝트는 같은 섹션에 배열 연산(`$append`·`$remove` 등)을 쓴다. 배열 연산은 struct 필드에만 되므로(ConfigSystem 4.2절) 묶음을 Map으로 두지 않았다. 묶음을 더하면 필드와 이름 표 항목을 함께 더한다.
  - 이름 목록은 쉼표로 나눈 채널·묶음 이름이고, 앞의 `-`는 끄기다(`--trace=default,-log`). 묶음 안의 모르는 이름(아직 없는 채널)은 세기만 하고, 직접 적은 모르는 이름은 경고 로그를 한 번 남긴다.
- **표준 채널(개정 7):** `cpu`·`counter`·`mark`·`span`·`frame`·`context`·`task`·`stats`·`log`(TraceCapture가 세션 Start 때 꺼진 상태로 등록), `gpu`(RenderBase). `log`는 로깅 LG15에서 더했다(6.6절).
  - 콘솔 명령 인자는 리플렉션 명령행 문법이다: `trace.enable channels=cpu,gpu`, `trace.disable channels=log`, `trace.file path=<경로>`, `trace.stop`(trace.file이 붙인 출력만 뗀다), `trace.snapshot path=<경로>`.
  - 세션 설정은 설정 struct `TraceCaptureSettings`(섹션 `Trace`)다. 콘솔 이름은 `trace.pool.max_bytes`·`trace.pool.warn_percent`·`trace.recent.bytes`·`trace.writer.period_ms`·`trace.stats.interval_pumps`(재시작 필요)·`trace.snapshot.lock_timeout_ms`다(콘솔 이름 문법이 소문자·밑줄이라 이 문서의 낙타 표기와 다르다). 콘솔 값 종류에 부호 없는 정수가 없어 부호 있는 칸이고, 세션 설정으로 옮길 때 음수는 0이다.

## 6. 기록 종류

### 6.1 CPU 구간

- **범위 객체 `CpuScope`**
  - 정적 이름: 호출 지점마다 이름 id를 정적 변수에 캐시한다. 경쟁이 나면 같은 id로 수렴하게 한다. 이름 표의 삽입은 같은 문자열에 같은 id를 돌려준다.
  - 동적 이름: 세션 이름 표에서 찾는다.
  - 채널 상태는 생성 때 한 번 읽는다. 그래서 끝 기록은 시작이 기록된 경우에만 낸다.
- **스레드별 배치**
  - 크기는 256 B다. 시작은 `(Δtick<<2)|1`과 이름 id varint, 끝은 `Δtick<<2`다.
  - 배치가 차거나 깊이가 0으로 돌아오면 이벤트 하나로 낸다. 배치의 첫 tick은 절대값이다(UE와 같다, `CpuProfilerTrace.cpp:187–198`).
- **기존 `ProfileScope`**: `Core/Diagnostics/ProfileScope.h`는 이것으로 바꾸고 지운다(하위 호환 없음). 한 스레드 `LogBuffer` 진단은 로그 쪽에 남긴다.

- **TR2 구현**
  - 배치(256 B)는 스레드 TLS 칸에 있어 시작·끝마다 잠금도 세션 접근도 없다. 세션이 바뀌었는지는 세션 메모리 대신 전역 `currentSessionSerial`로 안다. 배치를 낼 때만 진행 중 표시(종료 2단계)를 거쳐 세션에 쓴다.
  - 정적 이름은 `TraceScopeName`(호출 지점 정적 객체, 보통 `ROCKS_TRACE_CPU_SCOPE("이름")`)이 (세션 serial 하위 32비트, 이름 id)를 한 칸에 캐시한다.
  - 이름은 1024 byte(`MaxScopeNameBytes`)까지 쓰고, 넘으면 앞부분만 쓰고 이름 정의(`Trace.ScopeName`)에 잘림을 표시한다(옛 `ProfileScope`의 잘림 표시를 잇는다).
  - 세션 Stop 때 아직 내지 않은 살아 있는 스레드의 배치(열린 바깥 구간 등)는 버려진다. 다른 스레드의 TLS를 Stop이 안전하게 비울 수 없기 때문이다. 스레드 종료 때는 낸다.
  - 동적 이름 표(결정 T4)는 칸 수를 세션 설정 `nameTableCapacity`(기본 16384, 2의 거듭제곱)로 고정한 열린 주소법 표다. 읽기는 잠금 없고 삽입만 잠근다. 칸의 3/4이 차면 새 이름은 넣지 않고 그 구간은 기록하지 않는다(통계 `scopeNamesRejected`). 스레드 앞단 캐시는 64칸이며, 표가 가진 이름 byte와 비교하므로 해시 충돌에도 맞다.
  - `ProfileScope`(`Core/Diagnostics/ProfileScope.h`)는 지웠다(T8). 사용처는 Core 시험 두 곳이었다.

### 6.2 카운터

- 정의 기록은 id·이름·정수/실수·표시 단위(개수·바이트·백분율)다.
- 값 기록은 tick·id·값이고, 순서 번호를 받는다.
- 값이 바뀔 때만 내는 형과 매번 내는 형을 둔다(UE `Core Public/ProfilingDebugging/CountersTrace.h:96–122`와 같다).
- 채널이 나중에 켜지면 첫 값 기록 전에 정의 기록을 낸다.

- **TR2 구현**: `TraceCounter`(정적 객체, `Set`·`Add`·`SetFloat`·`AddFloat`). 현재 값은 채널·세션과 무관하게 객체가 갖고, 채널 `counter`가 켜져 있을 때만 값 기록(`Trace.CounterInteger`·`Trace.CounterFloat`, 순서 번호)을 낸다. 카운터 id는 세션마다 첫 값 기록 때 잠금 아래에서 매기고 정의(`Trace.CounterDefinition`)를 먼저 쓴다. "바뀔 때만" 형은 여러 스레드가 동시에 바꾸면 같은 값을 한 번 더 낼 수 있다(빠뜨리지는 않는다).

### 6.3 표지·시간 띠·프레임

- **표지**: 정의 기록(호출 지점 id·형식 문자열·파일·줄)과 값 기록(tick·형식 인자)으로 나눈다.
- **시간 띠**
  - 스레드와 무관하다. 시작·끝은 id로 짝짓고, id는 세션 원자 카운터에서 받는다.
  - 이름만으로 짝짓는 방식은 두지 않는다. 같은 이름 띠가 겹치면 모호하기 때문이다.
- **프레임 경계**
  - 프레임 종류별(게임·렌더) 시작·끝 tick과 프레임 번호다.
  - 렌더 프레임 번호는 RenderBase의 프레임 slot 세대와 같은 값이다.

- **TR2 구현**
  - 표지: `ROCKS_TRACE_MARK("형식 {}", 인자…)`가 호출 지점 정적 정의(`TraceMarkSite`)를 둔다. 인자는 tag + 값(Int64·UInt64·Float64·Bool·String)으로 모아 512 B까지 담는다. 자리표시 채우기는 분석기가 한다.
  - 시간 띠: `TraceBeginSpan(이름)`이 (띠 id, 세션 serial)을 돌려주고 `TraceEndSpan`이 같은 세션일 때만 끝을 낸다. 이름은 CPU 구간과 같은 이름 표를 쓴다.
  - 채널: 시간 띠는 새 채널 `span`, 프레임 경계는 `frame`, 표지는 `mark`다. 5절 영역 목록에 `span`·`context`를 더한다.

### 6.4 문맥

- **`TraceContext`**: 상관 id 묶음이다(세션·task·에셋·entity·요청). 로그와 trace가 같은 상관 id를 공유한다(계획 29.4절).
- **전파**
  - 문맥은 스레드별 스택이다.
  - task를 만들 때 현재 문맥의 사본 id를 받는다. 실행 때 그 문맥을 복원하고, 끝나면 되돌린다(UE `Core Public/ProfilingDebugging/MetadataTrace.h:53–92`의 Save·Restore와 같다).
  - 문맥 정의는 정의 기록이고, 복원은 id만 기록한다.

- **TR2 구현**
  - `TraceContextScope`(쌓기·복원), `TraceCaptureContext`(저장), `TraceCurrentContext`(조회)다. 스택은 TLS 칸(깊이 16)이라 세션이 없어도 동작한다. 넘친 쌓기는 세기만 한다.
  - 저장한 사본(`TraceContextToken`)은 id만이 아니라 값도 함께 가진다. 다른 스레드의 스택에 값을 다시 놓아야 로그가 같은 상관 id를 쓸 수 있고, 세션이 바뀌어도 복원할 수 있기 때문이다. 같은 세션이면 복원은 정의를 다시 쓰지 않고 id만 기록한다(`Trace.ContextPush`). 문맥 id는 세션마다 1부터 매기는 u32다.
  - 기록 채널은 새 채널 `context`다.

- **로깅 LG14(개정 7):** 스택은 Core `CorrelationScope`(`Core/Diagnostics/Correlation.h`, 깊이 `MaxCorrelationDepth` = 16) 하나로 옮겼다. 로그 기록도 같은 스택의 맨 위 값을 캡처한다(LoggingSystem.ko.md 14.1.4, L14).
  - `TraceContextValues`는 `CorrelationIds`, `TraceContextToken`은 `CorrelationToken`(값 + 확장 칸 2개 + valid)의 별칭이다. `TraceContextScope`·`TraceCaptureContext`·`TraceCurrentContext`는 같은 이름·의미의 얇은 래퍼다.
  - Core는 trace를 모른다. 스택 칸의 확장 칸 `ext[2]`는 Core에게 뜻 없는 u64 두 칸이고, trace가 `ext[0]` = 정의 캐시(세션 serial 하위 32비트 << 32 | 문맥 id), `ext[1]` = 쌓기를 기록한 세션 serial로 쓴다.
  - 쌓기·내리기 관찰자(원자 포인터 하나)를 세션 Start가 건다(같은 관찰자 재설정은 무동작, 프로세스 수명 동안 유지, 해제는 다른 스레드가 쌓지 않는 종료 단계에서만). 관찰자는 지금까지의 `TraceContextScope`가 하던 기록(정의·`Trace.ContextPush`·`Trace.ContextPop`)을 같은 조건(채널 `context`·세션)으로 낸다. 그래서 공개 `CorrelationScope`로 쌓아도 trace 기록이 같다.
  - 기록 형식과 깊이 상한·넘침 규칙은 바뀌지 않았다. TR2·TR3 시험은 고치지 않고 통과한다.

### 6.5 스레드 정보

- 스레드 등록 기록은 trace 스레드 id·OS 스레드 id·이름·묶음·정렬 순위다.
- 엔진이 만드는 모든 장수 스레드는 시작 때 등록한다: 게임 lane, 렌더 lane, worker, IO, writer, 오디오.
- 스레드 정보는 정의 기록이라 늦게 붙은 출력도 받는다.
- trace 스레드 id는 세션 카운터다. OS 스레드 id는 재사용될 수 있어 키로 쓰지 않는다(`Platform/Clock.h:17–19` 주석과 같다).

- **TR2 구현**
  - `TraceSetThreadInfo(이름, 묶음, 정렬 순위)`가 값을 스레드 TLS에 보관한다. 지금 세션이 있으면 바로 등록해 내고, 이후 어느 세션에 등록되든 `Trace.ThreadInfo`에 넣는다(고정부에 정렬 순위, 가변부에 이름·묶음, 각 63 byte까지). 같은 스레드의 정보가 다시 나오면 나중 것이 이긴다.
  - 스레드 상태 칸은 재사용한다. 끝난 스레드의 칸은 writer가 마지막 내용까지 비운 뒤 재사용 목록에 넣고, 새 스레드가 그 칸을 세대(generation)를 올려 다시 쓴다. writer는 세대가 바뀌면 자기 칸을 되돌린다. trace 스레드 id는 재사용하지 않는다. 통계 `threadSlots`가 칸 수다.


### 6.6 로그(로깅 LG15, 개정 7)

- 채널 `log`. 기록은 `TraceLogSink`(TraceCapture, `RuntimeLogSink` 구현)가 쓴다. 엔진 호스트가 trace 세션을 시작한 뒤 LogSystem에 더하고, 정지 전에 뺀다. 채널이 꺼져 있으면 sink 비용은 분기 하나다.
- `Trace.LogMessageSpec`(정의): u64 spec id(호출 지점 = EventId·카테고리·위치) | u64 EventId | u32 줄 | u8 레벨, 가변 카테고리 | 템플릿 | 파일 | 함수. 위치는 파일·함수 문자열의 내용 해시다(포인터가 아니므로 inline 함수·템플릿이 여러 TU에서 인스턴스화되어도 같은 지점은 같은 spec이다). 세션마다 호출 지점당 한 번이다(sink의 고정 중복 표 4096칸, 3/4까지). 정의 쓰기가 성공한 뒤에만 표에 넣는다.
- **동봉:** 표가 찼거나, spec이 정의 기록 상한(`MaxDefinitionRecordBytes`, 16 KiB)을 넘거나(쓰기 전에 검사해 바로 동봉), 정의 버퍼가 차서 쓰기가 실패하면 정의 대신 그 기록에 카테고리·템플릿을 싣는다. 동봉한 템플릿도 1024 byte에서 UTF-8 경계로 자르고 잘림 비트를 켠다. **동봉한 기록은 위치(파일·줄·함수)를 잃는다.** 동봉 실패는 sink 계수(`SpecTableOverflows`·`SpecWriteFailures`)로만 세고 세션의 "버린 정의" 수(`definitionRecordsDropped`)에는 넣지 않는다(정보가 기록에 남는다). 버퍼 부족은 다음 기록에서 다시 시도한다.
- `Trace.LogMessage`(스레드 스트림, 순서 번호): u64 캡처 tick | u64 생산자 OS 스레드 id | u64 프레임 | u64 spec id | u64 EventId | u64 × 5 상관 id | u32 LogEventFlags | u8 레벨 | u8 상관 id 마스크 | u8 trace 쪽 잘림 비트(1 메시지, 2 동봉 템플릿), 가변 렌더된 메시지(최대 1024 byte, UTF-8 경계에서 자름) | 카테고리 | 템플릿(뒤 둘은 동봉할 때만).
  - tick은 로그 캡처 tick 그대로다(로그가 `MonotonicTicks`로 캡처한다, 변환 오차 없음).
  - sink는 로그 출력 스레드에서 불리므로 생산자 스레드·시각·상관 id를 기록에 명시한다. 스레드는 `Trace.ThreadInfo`의 OS id로 대응한다.
  - 속성·문맥은 보내지 않는다(메시지에 렌더되어 있다).
- **재귀 차단:** 스레드별 재진입 표시, 카테고리 `Trace`·`Trace.*` 기록 제외(TraceWriter의 `Log::Warning(LogTrace, ...)`가 다시 trace로 돌지 않게), 세션이 없으면 보내지 않는다.
- **손실:** 다른 기록과 같은 비대기 규칙이다. 버퍼가 없으면 버리고 채널 손실과 sink `Lost()`로 센다.
- **분석:** TraceAnalysis 모델 `Logs()`·`LogSpecs()`(spec id 순, 이진 탐색)·`LogCategoryOf`, `trace-report` 판 4의 `logs`(레벨별 수와 Warning 이상 목록, 시각은 정수 ns, 로그가 없으면 키 없음).
- **호스트 순서:** trace 시작 → `AddSink` … 엔진 정리 → `LogSystem::Flush` → `RemoveSink` → trace 정지(Sandbox `--engine` 연결은 예정, `--trace-file <path>`로 파일 출력).

## 7. task 연동

- Task 모듈(`TaskScheduler`·`WorkerScheduler`)이 다음 기록을 낸다. 모두 순서 번호를 받는다(UE `Core Private/Async/TaskTrace.cpp:16–73`과 같은 사건 집합).
  - 만듦·실행 가능·의존 추가(앞 → 뒤)
  - 시작·끝·완료·취소
  - 기다림 시작(기다리는 task 배열)·기다림 끝
- task id는 64비트 세션 카운터다. task 객체 주소는 쓰지 않는다.
- 실행 구간은 CPU 구간 "task 실행"으로 감싸서 CPU 시간축과 task 그래프를 잇는다.
- 실행 전 문맥 복원은 6.4절을 따른다.
- 채널 `task`가 꺼져 있으면 Task 모듈의 추가 비용은 분기 하나다. 시험은 Task 시험에 채널을 켠 변형을 더해 측정한다.
- **TR3 구현**
  - 사건은 형 하나 `Trace.Task`(Ordered)에 종류 칸을 두어 남긴다(만듦·실행 가능·의존 추가·시작·끝·완료·취소·기다림 시작/끝). 실패 완료는 오류를 이름 문자열로 남긴다(ErrorCode 선언 순서는 영속 형식이 아니다). 기다림 사건의 task 칸은 기다리는 쪽에서 실행 중인 task(task 밖이면 0)이고 관련 칸이 기다리는 대상이다.
  - task 신원은 `TraceTaskId`(세션 64비트 id + 세션 serial)다. 만들 때 채널이 꺼져 있으면 id가 0이라, 그 task의 모든 사건 지점(제출·실행·끝 상태·기다림)이 분기 하나로 끝난다. 신원과 문맥 사본은 scheduler가 작업을 records에 공개하기 전에 정해 worker와 경쟁하지 않는다.
  - 실행 가능: 제출 때 의존이 모두 성공했으면 바로, 아니면 마지막 의존이 성공으로 끝날 때 scheduler gate 안에서 낸다(worker의 시작보다 앞선다).
  - 실행 구간 `TraceTaskScope`: 저장한 문맥 복원 → 시작 → CPU 구간 `Task.Run`(채널 cpu가 켜져 있을 때) → 끝 순이다.
  - 문맥(상관 id)은 task 채널과 무관하게 늘 저장·복원한다(로깅 L15, 개정 7). 채널이 꺼져 있고 스택이 비어 있으면 분기 하나다(`TraceCreateTask`). 채널이 꺼져 있으면 사본만 받고 task 기록은 하지 않는다. `DedicatedLane::Post`도 같은 규칙으로 전파하고, lane 작업이 끝나면 lane 스레드의 스택은 원래 깊이로 돌아온다.
  - InlineExecutor는 만듦부터 완료까지 한 스레드에서 차례로 남긴다.

## 7.1 기록 경로 비용(TR2 검토)

- **가변 필드 배치 캐시**: 형 등록 때 가변 필드 수와 원소 크기를 `TraceEventType`에 적어 두고(형 id를 release로 공개하기 전), 기록 경로는 id를 acquire로 읽은 뒤 그 값으로 인자를 검사한다. 기록마다 필드 표를 훑지 않는다.
- **진행 중 표시의 seq_cst 저장은 유지한다.** 검토한 대안과 판단은 다음과 같다.
  - 비대칭 fence(Windows `FlushProcessWriteBuffers`, Linux·Android `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)`, Apple 대응 없음): 기록 쪽은 relaxed 저장과 컴파일러 장벽만 남고 Stop이 비용을 낸다. Apple에는 문서화된 API가 없고 mprotect 요령은 IPI 동작에 기댄다. Android는 커널·seccomp 정책에 따라 membarrier를 못 쓸 수 있다. 모든 대상에서 정확성을 보장할 수 없어 바꾸지 않았다.
  - 세대(epoch) 방식: 스레드가 현재 세대를 공개하는 저장과 세션 읽기 사이에도 같은 store→load 순서가 필요하므로 비용이 줄지 않는다.
  - 대신 CPU 구간은 시작·끝을 TLS 배치에 쌓아 표시를 거치지 않는다(배치를 낼 때만 거친다). ARM64에서 seq_cst 저장·읽기는 stlr·ldar라 release·acquire와 비용이 거의 같고, x86의 비용은 xchg 하나다.
  - Debug 측정(Windows x64, `TraceDisabledChannelCostIsMeasured`): 꺼진 채널 약 3.5 ns, 켜진 기록 1회(고정부 8 byte, 순서 번호) 약 71 ns, 정적 이름 CPU 구간 1쌍(쌍마다 배치 이벤트를 내는 최악) 약 99 ns.

## 7.2 분석(TR4 구현)

- **모듈** `TraceAnalysis`(Core Developer): `TraceStreamDecoder`(머리·덩어리·스키마·기록 해석, TR1 시험 reader를 옮김), `TraceModel`(모델), `WriteTraceReportJson`(결정적 JSON). 형식의 단일 출처는 `TraceCapture/TraceFormat.h`다. 도구 타깃(Commandlet)과 시험만 링크한다.
- **스키마 해석**: 표준 형은 형 id가 아니라 이름으로 찾는다. 그래서 등록 순서가 다른 두 빌드의 capture도 같은 모델이 된다(OB06).
- **손상 입력**(OB03): 스트림 해석은 Complete·Truncated(기록 중간에서 끝남)·UnknownType(정의 누락으로 기록 크기를 모름)·OutOfMemory 상태를 돌려주고, 멈춘 앞까지의 기록은 쓴다. 모델 구축은 머리가 없을 때만 실패한다.
- **스레드 병합**: 순서 번호가 있는 기록(task·카운터)을 모든 스레드 스트림에서 모아 순서 번호로 정렬해 합친다. 출력이 붙을 때의 순서 번호 이상에서 빠진 번호는 순서 구멍으로 센다.
- **손실을 사이에 둔 구간 짝짓기**: 스레드 스트림의 손실 기록을 만나면 그때 열린 구간을 모두 "끝 없음"으로 닫는다. 손실 사이에 시작·끝이 사라졌을 수 있어, 손실 뒤의 끝과 짝짓지 않는다(보수적, 길이를 지어내지 않는다). 시작 없는 끝은 세기만 하고 스트림 끝에서 열린 구간도 끝 없음이다.
- **report**(`trace-report <입력> [<출력>]`, Commandlet): session·summary(잘림·정의 일부 없음·중간에 붙음·손실·버린 정의·이름 정의 누락·읽지 못한 스트림·시작 없는 끝·끝 없는 구간·순서 구멍·표지·채널별 손실)·threads(구간 이름별 횟수·끝 없음·합계·p50/p95/p99, nearest-rank, 정수 ns)·tasks(수·실행·실패·취소·간선·임계 경로)·counters다. 키 순서 고정, 스레드는 id 순, 구간 이름은 이름 byte 순, 실수 서식 없음(실수 카운터는 값 bit 16진)이라 같은 입력에서 같은 byte다.
- **임계 경로**: 가장 늦게 완료한 task에서 시작해, 그 task의 의존 가운데 가장 늦게 완료한 앞 task로 거슬러 올라간다(같으면 작은 id). 실행 시간 합과 첫 만듦부터 마지막 완료까지의 시간을 함께 낸다.
- **시험 입력**: golden은 tick·순서·형 id를 정해 형식을 직접 쓰는 시험 writer로 만든다(실제 세션은 시각이 실행마다 다르다). 실제 세션 capture로는 결정성과 중간 연결을 확인한다.

## 8. 출력·최근 기록 고리·스냅샷

**전송 덩어리**
- writer는 스레드 스트림·정의 스트림·스키마 스트림을 덩어리 `[u16 크기][u16 스트림 id]`로 나눠 낸다.
- 덩어리가 이름 있는 임계값(384 B)을 넘으면 LZ4 raw block으로 압축하고 표시 비트를 켠다. 압축은 Core Compression을 쓴다.
- **TR1 구현**: 크기 칸의 최상위 비트가 압축 표시이고, 압축 덩어리 내용은 u16 원본 크기 + LZ4 raw block이다. 덩어리 원본은 16 KiB 이하다. 압축해도 줄지 않으면 원본 그대로 쓴다. 스트림 id는 스키마 0, 정의 1, trace 스레드 id t는 t + 1이다. 정확한 byte 배치는 `TraceCapture/TraceFormat.h`가 단일 출처다.

**파일 형식**
- 머리: magic `RKTR`, 형식 버전, 머리 기록(4.4절)
- 그 뒤: 덩어리 열
- 확장자: `.rtrace`
- 같은 바이트 열을 파일·콜백·고리에 쓴다. TR1은 파일과 콜백 출력을 구현했다(고리는 TR5).
- 출력 열기·쓰기·닫기가 실패하면 원본 오류(파일 상태와 OS 오류 코드, 콜백이 돌려준 오류)를 통계(`lastOutputFileStatus`·`lastOutputCallbackError`)에 남긴다. 로그 알림은 로깅 시스템 연결 때 더한다(`TODO(log)`).
- 비정상 종료로 잘린 파일은 마지막 온전한 덩어리까지 읽고 "잘림"으로 표시한다(OB03·OB05).

**최근 기록 고리**
- 압축 덩어리 단위의 고정 크기 고리다. 기본 크기는 설정 `trace.recent.bytes` = 4 MiB, 에디터 32 MiB.
- 넘치면 가장 오래된 덩어리부터 버리고 버린 범위를 센다.
- 고리는 출력이 없어도 돈다. 그래서 "문제가 난 뒤 최근 N초"를 낼 수 있다.

**스냅샷**
- 순서: 머리 → 스키마 전체 → 정의 저장소 → 고리 → 동기 표지. 결과는 파일 하나다.
- writer 갱신 잠금은 제한 시간(기본 1초) 안에서만 시도한다. 얻지 못하면 "불완전"을 표시하고 가진 것만 쓴다.
- crash 처리 경로는 잠긴 힙·registry를 다시 돌지 않는다(계획 29.4절).

**구현(TR5)**
- 고리(`TraceRecent.cpp`)는 writer만 쓰는 byte 고리이고 칸 하나는 `[u8 표시][덩어리]`다. 스레드 스트림 덩어리만 담는다(스키마는 등록 표, 정의는 정의 저장소에서 온다). 출력이 없어도 펌프가 스레드 기록을 덩어리로 만들어 고리에 넣는다. 최소 크기는 32 KiB(`MinRecentBytes`, 가장 큰 덩어리 하나가 들어간다)이고 0이면 고리가 없다. 넘침은 버린 덩어리 수·byte 수 누계로 센다(통계 `recentDroppedChunks`·`recentDroppedBytes`).
- 덩어리 경계는 기록 경계와 다르다. 그래서 칸 표시에 "이 스레드 스트림이 여기서 기록 경계로 시작한다"(펌프마다 그 스레드의 첫 덩어리)를 두고, 스냅샷은 앞 덩어리를 잃은 스트림의 덩어리를 다음 경계 덩어리까지 뺀다(동기 표지의 `skippedChunks`).
- 스냅샷(`TraceSession::WriteSnapshot`, 파일판·콜백판): writer 잠금을 제한 시간(기본 1000 ms, `trace.snapshot.lock_timeout_ms`) 안에서 `TryLock`과 양보로 시도한다. 얻으면 한 번 펌프한 뒤 머리(`HeaderFlagSnapshot`, 넘친 적이 있으면 `HeaderFlagRecentDropped`) → 지금까지 낸 스키마 → 정의 저장소 → 고리 → 동기 표지 순으로 메모리에 만들고, 잠금을 놓은 뒤 출력한다. 못 얻으면 머리(`HeaderFlagSnapshotIncomplete`) → 스키마(등록 표 잠금을 같은 제한 시간 안에 얻었을 때) → 동기 표지만 쓴다. 실패가 아니다.
- 동기 표지는 세션 내부 형 `Trace.SnapshotSync`(id 6, 정의 스트림)이다: tick·다음 순서 번호·넘침 누계(덩어리·byte)·뺀 덩어리 수·완전 여부. 고리가 넘친 적이 있으면 머리의 순서 번호 칸을 스냅샷 시점 번호로 두어 분석기가 앞쪽 순서 번호 구멍을 손실로 세지 않게 한다.
- 수집 통계 기록 `Trace.Stats`(표준 채널 `stats`, 꺼진 상태로 등록)는 펌프한 스레드가 `statsIntervalPumps`(기본 60, `trace.stats.interval_pumps`) 펌프마다 낸다: 풀 사용·최대·상한, 출력 byte 누계, 정의 저장소 byte, 손실·버린 정의 누계, 고리 사용·버린 byte. 대역폭은 분석기가 두 표본의 출력 byte 차와 시간 차로 구한다(report `capture` 절). 시간이 아니라 펌프 수로 세서 Manual 시험이 결정적이다. 기본 묶음에는 넣지 않았다.
- 풀 사용률 경고: 펌프 시작 때 사용 byte가 상한의 `poolWarningPercent`(기본 80, `trace.pool.warn_percent`) 이상이면 `Log::Warning`(카테고리 `Trace`)을 한 번 남기고, 아래로 내려간 펌프에서 다시 무장한다(통계 `poolWarnings`).
- 분석: TraceAnalysis 요약에 스냅샷·불완전·넘침·뺀 덩어리 수를, 모델에 수집 통계 요약을 더했다. `trace-report` 판 3이다.

**writer 주기와 수동 펌프**
- writer 주기는 설정 `trace.writer.periodMs`(기본 16)다.
- 시험·헤드리스 결정적 모드는 writer 스레드 없이 `TraceSession::Pump()`를 호출한 쪽 스레드에서 비운다. 시험은 sleep 없이 펌프 뒤에 단언한다.

**네트워크 출력**
- 라이브 연결은 후속이다. Rocks Lens 라이브 단계(M3)와 함께 한다.
- 기본 local이고, 외부 연결은 명시 endpoint·인증·권한을 따른다(계획 29.4절).

## 9. GPU 연동(RB7과의 경계)

- `GpuScope`(RenderBase 13절)는 GX9 timestamp 링이 회수한 GPU 시간을 calibration pair(D3D12 queue clock calibration, Vulkan calibrated timestamps)로 4.4절 tick으로 바꾼다. 그다음 GPU 기록으로 낸다.
- **GPU 기록 형**
  - 정의 기록: queue 정의(id·종류·이름)와 GPU 구간 이름
  - 값 기록: 구간 시작·끝(tick), 작업 시작·끝, fence signal·wait, 프레임 경계
  - UE `RHI Private/GpuProfilerTrace.cpp:26–99`와 같은 사건 집합이다.
- **calibration 기록**
  - calibration 자체도 기록으로 남긴다: GPU tick·CPU tick·GPU 주파수·추정 오차.
  - 분석기는 오차를 표시한다(OB06 "clock 불확실성").
- **변환할 수 없는 경우**: calibration이 없는 backend(GLES 확장 없음 등)의 GPU 구간은 기록하지 않는다. "GPU 시간 없음"을 정의 기록으로 공개한다(RenderBase 13.2절).
- **CPU 쪽 상관**: 렌더 lane의 CPU 구간과 GPU 구간은 같은 pass 이름 id를 쓴다. 기록은 RB7 구현에 속하고, 이 문서는 형과 시간 규칙만 정한다.
- **구현(2026-11-17, RB7).** 형 이름·배치는 `TraceStandardFormat`(`TraceCapture/TraceFormat.h`)에 두고, RenderBase `RegisterGpuTraceTypes`가 채널 `gpu`(꺼진 상태)와 함께 등록한다.
  - `Trace.GpuQueue`(정의): u16 queue id · u8 종류(0 graphics) · u8 GPU 시간(0 없음, 1 있음), 가변 이름.
  - `Trace.GpuCalibration`(정의): u16 queue · u64 GPU tick · u64 CPU trace tick · u64 GPU 주파수 · u64 오차(ns).
  - `Trace.GpuScope`(스레드 스트림): u16 queue · u32 이름 id(`Trace.ScopeName`) · u32 깊이 · u64 프레임 · u64 시작 tick · u64 끝 tick. tick은 쓰기 전에 trace 시간축으로 바꾼 값이다.
  - `Trace.GpuFrame`(스레드 스트림): u16 queue · u64 프레임 · u64 시작 tick · u64 끝 tick.
  - GPU 기록은 완료 통지를 처리한 스레드(`GpuTrace::Consume`을 부른 스레드)의 스트림에 들어간다. 시간은 기록 안의 tick이 정하므로 스트림은 상관없다.
  - 변환: `trace tick = calibration CPU tick + (GPU tick − calibration GPU tick) × trace 주파수 ÷ GPU 주파수`(기준보다 앞선 tick은 그만큼 앞선 값, 0 아래는 0). 1초 단위와 나머지로 나눠 계산해 중간 곱이 넘치지 않는다. 쌍의 CPU 쪽은 `ClockCalibration::cpuTicks`(`platform::MonotonicTicks`)를 그대로 써서 ns 왕복 반올림이 없다.
  - 정의 기록은 세션마다 다시 쓴다(`TraceCurrentSessionSerial`로 새 세션을 안다). calibration은 세션 첫 프레임과 설정 간격마다 쓴다.
  - 스펙과 다른 점: 구간 시작·끝을 따로 쓰지 않고 한 기록에 담는다(두 끝이 모두 온 뒤에만 쓰므로 반쪽이 없다). "GPU 시간 없음"은 queue 정의의 칸이다. 작업 시작·끝·fence signal·wait 사건은 다중 queue와 함께 더한다.
  - 분석: TraceAnalysis 모델에 queue(이름·GPU 시간 여부·calibration 수·최대 오차)·GPU 구간·GPU 프레임을 더했다. `trace-report`는 판 2에서 `gpu` 절(queue별 프레임 시간 통계와 구간 이름별 횟수·합계·p50/p95/p99)을 낸다.

## 10. 단계와 완료 기준

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| TR1(구현 2026-11-17) | 세션·블록·풀(상한·손실)·이벤트 형 등록·정의 기록(상한)·순서 번호·채널·단조 tick API·writer와 수동 펌프·파일 출력·압축 덩어리 | Compression(구현) | 여러 스레드 기록의 바이트 왕복, 풀 상한에서 비차단 손실 수·구간의 정확성(gate로 블록 회수를 막아 결정적 재현), 정의 저장소 넘침 표시, 채널 꺼짐 비용 측정 기록, 스레드 종료 블록 회수, 종료 순서 |
| TR2(구현 2026-11-17) | `CpuScope`(정적·동적 이름)·카운터·표지·시간 띠·프레임 경계·스레드 정보·문맥 스택, `ProfileScope` 교체 | TR1 | 배치 인코딩 왕복, 이름 id 수렴(경쟁 gate), 구간 끝 손실 시 "끝 없음", 문맥 복원 |
| TR3(구현 2026-11-17) | Task 모듈 연동(사건 집합·의존 간선·실행 구간·문맥 전파) | TR2 | 고정 task 그래프의 의존 간선 복원, 직렬 oracle과 실행 집합 일치, 채널 꺼짐 Task 시험 회귀 없음 |
| TR4(구현 2026-11-17) | TraceAnalysis(스트림 읽기·스레드 병합·모델)·`trace-report` Commandlet(스레드별 구간 p50/p95/p99, task 임계 경로, 손실·잘림 표시, 결정적 JSON) | TR3 | golden report 일치, 잘린 파일·중간 연결·정의 누락 입력에서 안전한 분석(OB03), 두 빌드 capture 같은 스키마 해석(OB06 CPU 부분) |
| TR5(구현 2026-11-18) | 최근 기록 고리·스냅샷·통계 기록·콘솔 명령·명령줄·묶음(설정 계층) | TR4, ConfigSystem CF2 | 고리 넘침 범위, 스냅샷 단독 분석 가능, 잠금 제한 시간 초과의 "불완전" 표시(gate) |

- TR1–TR4가 CP5 완료다. RB7은 TR2 뒤에 시작할 수 있고, GPU 기록 형(9절)은 RB7에서 등록한다.
- 오프라인 timeline viewer(Rocks Lens 초판)는 입력 계획 C18의 범위다. 이 문서 다음 개정에서 단계를 정한다.
- 메모리·IO·GC·에셋 채널은 각 시스템이 이 기록 체계 위에 자기 이벤트 형을 등록하는 일이다. 이 문서의 단계에 넣지 않는다.

## 11. 다른 문서 개정

| 문서 | 위치 | 개정 |
|---|---|---|
| RenderBase | 4.2절 CP5 행, 13절 | 이 문서 참조, `GpuScope`의 기록 형과 시간 변환 경계(9절) |
| EngineImplementationPlan | 29.1·29.4절 | 수집 모듈 이름 TraceCapture·TraceAnalysis, 손실 정책(버리기·구간 기록) |
| CoreObjectInputDetailedPlan | C18 행 | TR1–TR5와 viewer 범위 |
| UnrealCoreFeatureAudit | UE-C23 행 | 단계 TR1–TR5 |
| PlatformLayer | Clock 절 | 단조 tick API |

## 12. 결정 사항

| ID | 결정 | 상태 |
|---|---|---|
| T1 | 메모리 상한에서 기다리지 않고 버리며 손실 수·구간 기록(UE stall·panic과 다름) | 결정됨(2026-11-17, 권장안) |
| T2 | 정의 저장소에 상한, 넘침 표시 | 결정됨(2026-11-17, 권장안) |
| T3 | 채널은 명시 등록, 등록 전 꺼짐(전역 생성자 금지) | 결정됨(2026-11-17, 권장안) |
| T4 | 동적 이름은 세션 전역 표 + 스레드 캐시 | 결정됨(2026-11-17, 권장안) |
| T5 | 형식은 독립(`RKTR`, `.rtrace`), UE 호환 없음 | 계획 29.4절에 따름 |
| T6 | 이벤트 형 필드 표: 직접 constexpr 표로 시작하고, 리플렉션 struct에서 생성하는 경로는 후속 | 결정됨(2026-11-17, 권장안) |
| T7 | 네트워크 라이브 출력은 Rocks Lens 라이브 단계(M3)와 함께 | 결정됨(2026-11-17, 권장안) |
| T8 | `ProfileScope`는 `CpuScope`로 바꾸고 지움(하위 호환 없음) | 결정됨(2026-11-17, 권장안), TR2에서 지움 |

## 13. 개정 이력

- 개정 7(2026-11-18, 로깅 세션 작성·rocks-95 합의): 상관 id 스택을 Core `CorrelationScope`로 옮기고 trace는 확장 칸·관찰자로 기록을 유지한다(6.4절). 문맥 전파는 task 채널과 무관하게 늘 하고 `DedicatedLane::Post`도 전파한다(7절). 표준 채널 `log`와 `Trace.LogMessageSpec`·`Trace.LogMessage`, 분석 `Logs()`·report `logs`를 더했다(5절·6.6절, 로깅 LG15). 재리뷰 반영: spec id 위치는 문자열 내용 해시, 정의 상한 초과 spec은 바로 동봉, 동봉 템플릿 1024 byte 상한과 잘림 비트 2, 동봉 실패는 sink만 센다(6.6절).
- 개정 6(2026-11-18): TR5 구현 반영(5절 구현, 8절 구현). 스펙과 다르게 정한 것: 고리의 덩어리에 기록 경계 표시를 두고 경계를 잃은 덩어리는 스냅샷에서 뺀다, 동기 표지를 세션 내부 형 기록(`Trace.SnapshotSync`)으로 둔다, 수집 통계를 표준 채널 `stats`의 기록으로 펌프 수 간격에 낸다(기본 묶음에 넣지 않음), 설정 계층 연결을 새 모듈 `TraceControl`에 둔다, 묶음은 Map이 아니라 설정 struct의 필드다, 콘솔 설정 이름은 밑줄 표기다, `trace.stop`은 세션이 아니라 `trace.file`의 출력만 끝낸다.
- 개정 5(2026-11-17): RB7 GPU 기록 형 구현 반영(9절). 형 4개와 배치, 변환 식, 기록 스트림, 세션별 정의 재기록, 분석 모델과 report 판 2의 `gpu` 절을 적었다. 스펙과 다르게 정한 것: 구간 하나를 두 tick 기록 하나로, "GPU 시간 없음"을 queue 정의 칸으로, 작업·fence 사건은 뒤로. 채널 설명 문자열은 영어로 바꿨다(로그·추적 메시지 영어 규칙).
- 개정 4(2026-11-17): TR3·TR4 구현 반영. task 사건을 종류 칸이 있는 형 하나로 둔 것, 실행 가능 시점, 실행 구간 순서, task 채널이 켜졌을 때만 문맥을 전파하는 것(7절), TraceAnalysis 모듈·이름으로 찾는 스키마·스트림 해석 상태·순서 번호 병합·손실을 사이에 둔 보수적 짝짓기·report 형식·임계 경로 정의(7.2절)를 적었다.
- 개정 3(2026-11-17): TR2 구현 반영. Compression의 Task 의존 부분을 AsyncCompression으로 떼어 모듈 순환을 해소했다. CPU 구간 TLS 배치와 Stop 때 버려지는 배치, 이름 표 크기 설정과 이름 잘림, 카운터·표지·시간 띠·프레임 API, 새 채널 `span`·`context`, 문맥 사본이 값을 가지는 것, 스레드 정보 API와 스레드 상태 칸 재사용, 가변 필드 배치 캐시와 seq_cst 진행 중 표시를 유지한 검토(7.1절), 출력 실패의 원본 오류 보관을 적었다.
- 개정 2(2026-11-17): TR1 구현 반영. 종료 2단계의 확인 방식(TLS 진행 중 표시), 블록 머리·큰 기록 분할·손실 기록 위치, 세션 내부 형, 정의 버퍼 설정과 `Trace.DefinitionLoss`, 동기 표지를 출력 머리 칸으로 둔 것, 덩어리 압축 배치를 적었다. Compression의 Task 의존으로 TR3에서 생길 모듈 순환을 과제로 남겼다.
- 개정 1(2026-11-17): 초안. UE 5.8.4 TraceLog(TLS 블록·풀·writer·스키마·important·채널·tail·snapshot), CPU profiler·카운터·misc·metadata·task·GPU profiler trace, 분석 형식 조사와 Rocks 요구(계획 29절, 입력 계획 C18)를 바탕으로 썼다.
