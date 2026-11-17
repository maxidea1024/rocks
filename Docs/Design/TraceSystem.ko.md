# 다중 스레드 trace 설계

기준일: 2026-11-17, 개정 1(초안). 여러 스레드의 CPU 구간·카운터·표지·task 수명·GPU 구간을 하나의 시간축에 기록하는 **trace 수집**(TraceCapture)과, 이를 읽는 **trace 분석**(TraceAnalysis)을 정한다.

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

상태: **설계 확정**(결정 T1–T8). 단계는 10절이다.

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
- 스레드 종료 때 그 스레드 블록을 은퇴로 표시한다. writer가 남은 내용을 보낸 뒤 블록을 풀에 돌려준다(UE `TlsBuffer.cpp:429–439`와 같다).

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
- **풀**
  - 잠금 없는 자유 목록이고, 새 페이지 확보만 잠금 하나로 직렬화한다.
  - **상한**은 설정 `trace.pool.maxBytes`(기본 64 MiB)다. 시작 때부터 적용한다. UE는 writer가 돌 때만 상한을 적용한다(`BlockPool.cpp:45`).
- **상한에 닿으면**
  - 블록을 받지 못한 스레드는 그 기록을 버리고, 스레드 손실 수(채널별)를 늘린다.
  - 손실이 시작된 tick도 기억한다. 다음에 블록을 받으면 첫 기록으로 손실 기록(채널별 수, 시작·끝 tick)을 남긴다.
  - 열린 CPU 구간의 끝이 손실되면 분석기는 그 구간을 "끝 없음"으로 표시하고 길이를 지어내지 않는다(계획 29.4절).
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

## 5. 채널과 묶음

- **채널 등록**
  - 채널은 이름·설명·읽기 전용 여부로 등록한다.
  - 이름은 소문자 점 경로이며, 설정 이름 문법(ConfigSystem 5.1절)과 같은 영역 이름 공간을 쓴다(`cpu`, `task`, `gpu`, `frame`, `log`, `mark`, `counter`, `memory.alloc`, `io`, `gc`, `asset`).
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

### 6.2 카운터

- 정의 기록은 id·이름·정수/실수·표시 단위(개수·바이트·백분율)다.
- 값 기록은 tick·id·값이고, 순서 번호를 받는다.
- 값이 바뀔 때만 내는 형과 매번 내는 형을 둔다(UE `Core Public/ProfilingDebugging/CountersTrace.h:96–122`와 같다).
- 채널이 나중에 켜지면 첫 값 기록 전에 정의 기록을 낸다.

### 6.3 표지·시간 띠·프레임

- **표지**: 정의 기록(호출 지점 id·형식 문자열·파일·줄)과 값 기록(tick·형식 인자)으로 나눈다.
- **시간 띠**
  - 스레드와 무관하다. 시작·끝은 id로 짝짓고, id는 세션 원자 카운터에서 받는다.
  - 이름만으로 짝짓는 방식은 두지 않는다. 같은 이름 띠가 겹치면 모호하기 때문이다.
- **프레임 경계**
  - 프레임 종류별(게임·렌더) 시작·끝 tick과 프레임 번호다.
  - 렌더 프레임 번호는 RenderBase의 프레임 slot 세대와 같은 값이다.

### 6.4 문맥

- **`TraceContext`**: 상관 id 묶음이다(세션·task·에셋·entity·요청). 로그와 trace가 같은 상관 id를 공유한다(계획 29.4절).
- **전파**
  - 문맥은 스레드별 스택이다.
  - task를 만들 때 현재 문맥의 사본 id를 받는다. 실행 때 그 문맥을 복원하고, 끝나면 되돌린다(UE `Core Public/ProfilingDebugging/MetadataTrace.h:53–92`의 Save·Restore와 같다).
  - 문맥 정의는 정의 기록이고, 복원은 id만 기록한다.

### 6.5 스레드 정보

- 스레드 등록 기록은 trace 스레드 id·OS 스레드 id·이름·묶음·정렬 순위다.
- 엔진이 만드는 모든 장수 스레드는 시작 때 등록한다: 게임 lane, 렌더 lane, worker, IO, writer, 오디오.
- 스레드 정보는 정의 기록이라 늦게 붙은 출력도 받는다.
- trace 스레드 id는 세션 카운터다. OS 스레드 id는 재사용될 수 있어 키로 쓰지 않는다(`Platform/Clock.h:17–19` 주석과 같다).

## 7. task 연동

- Task 모듈(`TaskScheduler`·`WorkerScheduler`)이 다음 기록을 낸다. 모두 순서 번호를 받는다(UE `Core Private/Async/TaskTrace.cpp:16–73`과 같은 사건 집합).
  - 만듦·실행 가능·의존 추가(앞 → 뒤)
  - 시작·끝·완료·취소
  - 기다림 시작(기다리는 task 배열)·기다림 끝
- task id는 64비트 세션 카운터다. task 객체 주소는 쓰지 않는다.
- 실행 구간은 CPU 구간 "task 실행"으로 감싸서 CPU 시간축과 task 그래프를 잇는다.
- 실행 전 문맥 복원은 6.4절을 따른다.
- 채널 `task`가 꺼져 있으면 Task 모듈의 추가 비용은 분기 하나다. 시험은 Task 시험에 채널을 켠 변형을 더해 측정한다.

## 8. 출력·최근 기록 고리·스냅샷

**전송 덩어리**
- writer는 스레드 스트림·정의 스트림·스키마 스트림을 덩어리 `[u16 크기][u16 스트림 id]`로 나눠 낸다.
- 덩어리가 이름 있는 임계값(384 B)을 넘으면 LZ4 raw block으로 압축하고 표시 비트를 켠다. 압축은 Core Compression을 쓴다.

**파일 형식**
- 머리: magic `RKTR`, 형식 버전, 머리 기록(4.4절)
- 그 뒤: 덩어리 열
- 확장자: `.rtrace`
- 같은 바이트 열을 파일·콜백·고리에 쓴다.
- 비정상 종료로 잘린 파일은 마지막 온전한 덩어리까지 읽고 "잘림"으로 표시한다(OB03·OB05).

**최근 기록 고리**
- 압축 덩어리 단위의 고정 크기 고리다. 기본 크기는 설정 `trace.recent.bytes` = 4 MiB, 에디터 32 MiB.
- 넘치면 가장 오래된 덩어리부터 버리고 버린 범위를 센다.
- 고리는 출력이 없어도 돈다. 그래서 "문제가 난 뒤 최근 N초"를 낼 수 있다.

**스냅샷**
- 순서: 머리 → 스키마 전체 → 정의 저장소 → 고리 → 동기 표지. 결과는 파일 하나다.
- writer 갱신 잠금은 제한 시간(기본 1초) 안에서만 시도한다. 얻지 못하면 "불완전"을 표시하고 가진 것만 쓴다.
- crash 처리 경로는 잠긴 힙·registry를 다시 돌지 않는다(계획 29.4절).

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

## 10. 단계와 완료 기준

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| TR1 | 세션·블록·풀(상한·손실)·이벤트 형 등록·정의 기록(상한)·순서 번호·채널·단조 tick API·writer와 수동 펌프·파일 출력·압축 덩어리 | Compression(구현) | 여러 스레드 기록의 바이트 왕복, 풀 상한에서 비차단 손실 수·구간의 정확성(gate로 블록 회수를 막아 결정적 재현), 정의 저장소 넘침 표시, 채널 꺼짐 비용 측정 기록, 스레드 종료 블록 회수, 종료 순서 |
| TR2 | `CpuScope`(정적·동적 이름)·카운터·표지·시간 띠·프레임 경계·스레드 정보·문맥 스택, `ProfileScope` 교체 | TR1 | 배치 인코딩 왕복, 이름 id 수렴(경쟁 gate), 구간 끝 손실 시 "끝 없음", 문맥 복원 |
| TR3 | Task 모듈 연동(사건 집합·의존 간선·실행 구간·문맥 전파) | TR2 | 고정 task 그래프의 의존 간선 복원, 직렬 oracle과 실행 집합 일치, 채널 꺼짐 Task 시험 회귀 없음 |
| TR4 | TraceAnalysis(스트림 읽기·스레드 병합·모델)·`trace-report` Commandlet(스레드별 구간 p50/p95/p99, task 임계 경로, 손실·잘림 표시, 결정적 JSON) | TR3 | golden report 일치, 잘린 파일·중간 연결·정의 누락 입력에서 안전한 분석(OB03), 두 빌드 capture 같은 스키마 해석(OB06 CPU 부분) |
| TR5 | 최근 기록 고리·스냅샷·통계 기록·콘솔 명령·명령줄·묶음(설정 계층) | TR4, ConfigSystem CF2 | 고리 넘침 범위, 스냅샷 단독 분석 가능, 잠금 제한 시간 초과의 "불완전" 표시(gate) |

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
| T8 | `ProfileScope`는 `CpuScope`로 바꾸고 지움(하위 호환 없음) | 결정됨(2026-11-17, 권장안) |

## 13. 개정 이력

- 개정 1(2026-11-17): 초안. UE 5.8.4 TraceLog(TLS 블록·풀·writer·스키마·important·채널·tail·snapshot), CPU profiler·카운터·misc·metadata·task·GPU profiler trace, 분석 형식 조사와 Rocks 요구(계획 29절, 입력 계획 C18)를 바탕으로 썼다.
