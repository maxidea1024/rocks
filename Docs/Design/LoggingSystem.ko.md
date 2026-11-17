# 로깅 시스템 설계

기준일: 2026-11-17, 개정 3. 엔진 전반에서 쓰는 **로그**(사람이 읽는 사건 기록)의 호출 API, 카테고리·레벨 필터, 비동기 기록 경로, 출력 형식, 장애 격리, 엔진 적용 범위를 정한다.

요구는 다음 문서에서 온다.
- [엔진 구현 계획](EngineImplementationPlan.ko.md) 29.4절(필수 로깅·프로파일러: level/category, UTC+monotonic, rate limit·중복 집계, compile/runtime level, error/fatal 비상 경로, 임계 구간에서 file IO·heap·lock wait 금지)
- [Core 상세 명세](CoreDetailedSpec.ko.md) D01, [UE Core 기능 감사](UnrealCoreFeatureAudit.ko.md) UE-C22(둘 다 **부분**)
- [trace 설계](TraceSystem.ko.md) — writer 스레드·수동 Pump·세션 소유 모양, `log` 채널, `TraceContext` 상관 id, T8(`ProfileScope` 교체)

UE 근거는 UE 5.8.4(`release`, 로컬 클론)의 현재 기본 경로다. 경로 약어는 다음과 같다.

| 약어 | 경로(`Engine/Source/Runtime/Core/` 기준) |
|---|---|
| `LV` | `Public/Logging/LogVerbosity.h` |
| `LM` | `Public/Logging/LogMacros.h` |
| `LC` | `Public/Logging/LogCategory.h` |
| `SL` | `Public/Logging/StructuredLog.h` |
| `OR` | `Private/Misc/OutputDeviceRedirector.cpp` |
| `ORH` | `Public/Misc/OutputDeviceRedirector.h` |

UE의 매크로 중심 API(`UE_LOG`, `DECLARE_LOG_CATEGORY_*`)는 따르지 않는다. 일반 로그는 함수 호출로 쓴다. 메시지 템플릿과 출력 레이아웃은 Serilog·log4j의 개념을 Rocks 이름으로 가져온다. UE 이름은 대응표(4절)에만 둔다.

상태: **설계 확정**(결정 L1–L13, 17절). 단계는 14절이다. 다른 세션과의 순서는 15절이다.

## 1. 결정 요약

| 항목 | 결정 | UE 5.8 방식과 근거 |
|---|---|---|
| 호출 | 매크로 없이 `Log::Info(LogEngine, "Engine initialized with {Count} subsystems", count)`. 반환은 `void` | UE는 `UE_LOG(Category, Verbosity, Fmt, ...)` 매크로다(`LM:278`, 형식 검증은 `UE_PRIVATE_LOG` 안 `LM:339`). 매크로라 인자 평가까지 지우지만 호출 모양이 무겁다 |
| 비활성 비용 | 비활성 레벨은 메시지 서식화와 큐 기록을 하지 않는다. **일반 함수 인자는 평가된다**(C++ 함수 호출의 성질). 비싼 값은 `Log::DebugLazy` | UE는 `IsSuppressed` 검사 안에서 인자를 평가하므로 꺼진 로그의 인자도 평가하지 않는다(`LM:350–360`, `UE_LOG_ACTIVE` 정의 `LM:262`) |
| 기록 경로 | **모드 선택: 비동기·동기.** 엔진 호스트(게임·에디터처럼 tick이 있음)는 기본이 비동기, 명령줄 도구는 기본이 동기다. 엔진 호스트는 설정 `log.mode`로 바꿀 수 있다(시작 뒤 고정). 비동기에서 호출 스레드는 활성 검사·스레드별 준비 버퍼에 값 캡처·lock-free 유한 큐에 복사만 하고, 서식화·sink 출력은 LogSystem의 출력 스레드가 한다. 동기는 호출 스레드가 렌더하고 sink까지 부른다 | UE는 primary 스레드가 아닌 스레드의 기록을 **크기 제한 없는** `TMpscQueue`에 넣고(`OR:143`) 노드를 동시 선형 할당기로 **기록마다 할당**한다(`OR:49–51, 99`). 여러 스레드에서 안전한 장치는 호출 스레드에서 바로 부른다(분류 `OR:440–446`, 호출 `OR:844, 905`) |
| 큐가 찼을 때 | **기다리지 않고 버린다.** 큐의 마지막 10%는 Warning 이상 전용. 손실은 레벨별로 세고 공간이 생기면 한 줄로 알린다 | UE 큐는 유한하지 않아 메모리가 계속 는다. TraceSystem과 같은 비대기 방침이다 |
| 메모리 | `LogSystem::Start`에서 전부 확보한다. 그 뒤 생산자·출력 스레드 모두 힙 할당 0 | UE는 기록마다 할당한다(위) |
| 메시지 | Serilog식 **메시지 템플릿**: `{Name}`은 순서대로 인자에 묶이고 이름은 속성 키다. spec은 기존 Rocks 서식(`{HResult:#x}`). 템플릿 해시가 사건 id(`EventId`) | UE 5.8 `UE_LOGFMT`가 이름 있는 필드를 가진다(`SL:32–46`). 이것도 매크로다 |
| 출력 형식 | sink마다 **레이아웃 패턴**(`"[{Timestamp:ms}Z][{Level:u3}][{Category}] {Message}{NewLine}"`)과 JSON lines sink | UE 출력 장치는 장치별 고정 형식이다 |
| 카테고리 | 상태 없는 `inline constexpr LogCategory LogWorld{"World"};`. 출력에서 정체성은 이름이다. 런타임 레벨은 설치된 LogSystem의 재정의 표(점 접두사 최장 일치, 원자 해시·레벨로 잠금 없이 조회) | UE `FLogCategoryBase`는 카테고리 객체가 런타임 verbosity를 가지며(`LC:30, 59`) 정적 생성자로 만든다 |
| 컴파일 타임 하한 | 빌드 구성별 하나, **타깃 전체에 같은 값**(`ROCKS_LOG_COMPILED_MINIMUM`) | UE `COMPILED_IN_MINIMUM_VERBOSITY`(`LM:94`)와 카테고리별 `CompileTimeVerbosity`(`LC:30`) |
| 레벨 | `Trace, Debug, Info, Warning, Error, Fatal` | UE `ELogVerbosity`: Fatal, Error, Warning, Display, Log, Verbose, VeryVerbose(`LV:16–56`). Display·Log를 Info 하나로 합친다 |
| 설치 전 기록 | Warning 이상은 비상 stderr로 바로, 그 아래는 개수만 센다. 재생하지 않는다 | UE는 backlog에 모았다가 장치가 붙으면 재생한다(`ORH:95, 103`, `OR:153`) |
| 장애 | 로깅 경로의 어떤 실패도 Fatal·assert로 이어지지 않는다. 의도적 종료는 `Log::Fatal`뿐 | UE도 panic 경로를 따로 둔다(`ORH:166–169`) |
| 메시지 언어 | 영어. 주석은 한국어 | — |
| 파일 암호화 | 파일 sink는 암호화를 설정할 수 있다. **기본은 끔.** 켜면 Crypto `SecretStream`(XChaCha20-Poly1305)으로 flush 묶음마다 봉인하고, 파일 키는 개발자 공개키로 KEM 봉인한다(9.1) | UE 로그 파일은 평문이다 |

## 2. 요구 원칙

이 설계의 모든 결정은 다음 중 하나 이상으로 설명된다.

| 번호 | 원칙 | 주로 다루는 절 |
|---|---|---|
| P1 | **육하원칙으로 원인을 파악할 수 있어야 한다.** 언제·어디서·누가(무엇이)·무엇을·왜·어떻게가 한 기록에서 보인다 | 6.5, 12 |
| P2 | **로깅이 메인 틱을 느리게 하지 않는다.** tick이 있는 엔진 호스트는 기본이 비동기다(명령줄 도구는 동기로 충분하고, 설정으로 바꿀 수 있다 — 5.1). 호출 스레드에는 서식화·I/O·sink lock·힙 할당이 없고 큐가 차도 기다리지 않는다. 예외는 잠든 출력 스레드를 깨우는 생산자 하나의 짧은 말단 lock(8.4)이다 | 8 |
| P3 | **로깅 오류로 엔진이 죽지 않는다.** 실패는 버리기·낮추기·집계로 끝난다 | 10 |
| P4 | **log4j·Serilog식 서식.** 메시지 템플릿·속성, sink 레이아웃 패턴, JSON lines | 6.3, 9 |
| P5 | **메시지는 영어로 쓴다.** 템플릿·자리 이름·카테고리·비상 문구·Fatal·assert 문구. 인자 값은 원문 그대로 | 12 |
| P6 | **무할당·최대한 가볍게.** Start 뒤 힙 할당 0, 생산자 경로는 lock-free | 8.2, 8.6 |
| P7 | **파일 기록은 암호화를 설정할 수 있다(기본 끔).** 사용자가 읽으면 곤란한 로그가 있다. 목표는 사용자가 알아볼 수 없는 수준이다 | 9.1 |

## 3. 현재 상태(개정 1 기준, `80178a3`)

- `Core/Diagnostics/Logger.h`: 동기 fan-out `Logger`(sink 16개, 하나의 mutex 아래 **호출 스레드가 sink를 직접 호출**, 재진입 `Busy`, sink 실패 `DependencyFailed`), `LogMessage`(단조 ns·thread·`uint32 category`·level·message), `LogFilter`(최소 레벨·카테고리 숫자 일치), `TextLogSink`(`snprintf` 접두사 `[Info][3] `, **매 호출 flush**), `StderrLogSink`.
- `LogRecord.h`: `LogLevel { Trace, Info, Warning, Error }` — Debug·Fatal 없음. `LogBuffer`·`ProfileScope`가 같은 열거형을 쓴다.
- `Platform/Fatal.h`: `platform::Fatal(const char*)` — stderr 4096 byte 시도 후 abort. `Core/Assert.h`: `ROCKS_ASSERT` → `AssertFailed(expr, file, line)`.
- `Text/StringBuilder.h`: `{}`·`{0}`·spec 서식, `Formatter<T>` 확장점, 지원하지 않는 타입은 `static_assert`. 포맷 문자열은 `StringView`로 받아 **런타임에만** 해석한다.
- 시험 밖 사용은 `Crypto/Private/Aes/AesModes.cpp:172`의 stderr 한 줄뿐이다. Engine/Source/Runtime 전체에 로그 호출이 없다. Engine을 실행하는 바이너리도 없다(`EngineTests.cpp`만 만든다).
- 실패 문맥 손실: `Engine::Initialize`의 `fail(ErrorCode)`(단계 없음), `ProcessDeferred`의 `(void)destroyed`, D3D12·OpenGL `Fail(const char*)`(HRESULT 버림, 생성 실패 시 `LastError` 소실), Win64 창(`GetLastError` 버림), TickScheduler 순환 거부가 다른 `InvalidArgument`와 구별되지 않음.
- 재사용할 것과 한계:
  - `Core/Text/Json.h`의 `JsonWriter`: 고정 들여쓰기 여러 줄이고, 내부 `Array<Frame>`·중복 키 `HashMap`을 할당한다(`Json.h:373–374`). 한 줄 모드와 무할당 옵션을 추가해야 한다.
  - `DateTime`: ISO 8601, spec 없음, 소수부 가변, `Z` 포함(`DateTime.h:239–241`). 고정 폭 밀리초를 추가해야 한다.
  - `StringBuilder`: 넘치면 잘리지 않고 그 서식 호출 전체가 실패해 호출 전 내용으로 되돌아간다(`StringBuilder.cpp:806–816`). 외부 버퍼를 받는 공개 생성자도 없다. 잘라서 성공하는 고정 버퍼 모드를 추가해야 한다.
  - PlatformBase `Thread`·Atomics. `ManualResetEvent::Signal`은 mutex를 잡는다(`Sync.h:89–92`). `platform::UtcTicks`는 실패하면 Fatal이다(`Clock.h:13–16`).
  - **Core에 lock-free 큐와 캐시 라인 크기 상수는 없다.**
- 기존 `Logger::Write`는 자기 시각·스레드를 찍고(`Logger.cpp:88`) sink 실패를 `DependencyFailed` 하나로 합친다(`:102`). 출력 스레드 안에서 그대로 쓸 수 없으므로 다시 쓴다(8.4).

## 4. 용어 대응

| Rocks | UE 5.8 | Serilog · log4j | 뜻 |
|---|---|---|---|
| `Log` (정적 함수 모음) | `UE_LOG`, `UE_LOGFMT` | `Log.Information` · `logger.info` | 호출 API |
| `LogLevel` | `ELogVerbosity` | `LogEventLevel` · `Level` | 레벨 |
| `LogCategory` | `FLogCategoryBase` | SourceContext · Logger 이름 | 카테고리 |
| `LogSystem` | `FOutputDeviceRedirector` | Logger 파이프라인 · LoggerContext | 기록 경로 소유자 |
| 출력 스레드 | dedicated primary thread(`ORH:121`) | Async sink · AsyncAppender | sink를 부르는 스레드 |
| `RuntimeLogSink` | `FOutputDevice` | Sink · Appender | 출력 대상 |
| 메시지 템플릿 | `UE_LOGFMT` 형식 문자열 | Message template · SLF4J `{}` | 메시지 |
| 레이아웃 패턴 | — | outputTemplate · PatternLayout | 출력 모양 |
| `LogContextScope` | — | `LogContext.PushProperty` · ThreadContext(MDC) | 문맥 속성 |
| `EventId` | — | MessageTemplate 해시(EventType) | 사건 종류 |

## 5. 모듈과 수명

- 위치: Core 모듈 `Core/Diagnostics`. 큐는 `Core/Containers/BoundedMpscQueue.h`(범용), 캐시 라인 크기 상수는 Base. TraceCapture가 Core 위에 서므로 로그는 trace 기반을 쓸 수 없다.
- **소유:** `LogSystem` 객체를 호스트(main·시험)가 명시적으로 소유한다. 전역에는 현재 LogSystem을 가리키는 원자 포인터 하나만 둔다. Engine은 LogSystem을 만들지 않고 설치된 것에 프레임 번호와 문맥만 알린다. 같은 프로세스의 여러 Engine은 로그를 공유한다(stderr처럼 프로세스 단위).
- **시작:** `LogSystem(Allocator&, LogSystemConfig)` → `AddSink` → `Start()`. Start가 큐·블록 풀·출력 버퍼를 확보하고 출력 스레드를 만든다. 출력 스레드는 Task가 아닌 플랫폼 스레드다(스케줄러가 끝난 뒤에도 비워야 하므로, TraceSystem과 같은 이유).
- **설치:** `LogInstallScope installed(logs);` — 범위 동안 전역 대상이 되고 끝나면 이전 대상을 복원한다. 중첩할 수 있다. 해제가 반환되면 sink와 LogSystem을 파괴해도 된다.
  - 진행 중 호출 추적은 **세대 두 개의 전역 카운터 쌍**(SRCU 방식, `constinit`)이다. 호출은 현재 세대를 읽어 그 세대 카운터를 올리고, seq_cst로 포인터를 다시 읽고, 끝나면 내린다.
  - 해제는 포인터를 바꾼 뒤 세대를 뒤집고 **옛 세대 카운터만** 0이 되기를 기다린다. 새 호출은 새 세대로 가므로 계속 로그가 들어와도 해제가 굶지 않는다. 카운터가 LogSystem 밖에 있으므로 파괴된 객체를 증가시키는 일도 없다.
- **정지:** `Stop()`(또는 소멸) = 새 기록 거부 → 큐 drain → sink flush → 출력 스레드 join → 메모리 반납. drain은 제한 시간 안의 best effort이고 남은 수는 비상 경로로 알린다.
### 5.1 기록 모드

| 모드 | 동작 | 기본으로 쓰는 호스트 |
|---|---|---|
| `LogMode::Async` | 8절 경로. 출력 스레드, lock-free 큐, 생산자 비차단 | tick이 있는 엔진 호스트(게임, 에디터, Sandbox `--engine`) |
| `LogMode::Sync` | 호출 스레드가 활성 검사 → 스레드별 준비 버퍼에 렌더(`FixedStringBuilder`, 힙 없음) → sink fan-out(말단 sink mutex) → sink flush 정책까지 한다. 큐·블록 풀·출력 스레드를 만들지 않는다 | 명령줄 도구(Commandlet, 셰이더 컴파일 도구, 벤치, 빌드 도구) |
| `LogMode::Manual` | Async와 같은 큐이지만 출력 스레드 없이 `Pump()`로 비운다 | 시험 |

- 프리셋은 `LogSystemConfig::ForEngineHost()`(Async)와 `LogSystemConfig::ForTool()`(Sync)이다. 호스트가 고른다.
- **설정 파일:** 엔진 호스트는 ConfigSystem 콘솔 설정 `log.mode`(`"async"` · `"sync"`, 플래그 **시작 뒤 고정**)를 읽어 프리셋을 덮어쓴다.
  - 등록과 읽기는 ConfigSystem CF2에 잇는다. CF2 전에는 프리셋 기본값만 적용된다.
  - 명령줄 도구도 같은 키를 읽을 수 있지만, 기본은 Sync다.
- 모드는 `Start`에서 정해지고 실행 중에는 바꾸지 않는다. 바꾸려면 다시 시작한다.
- **Sync에서 같은 것:** 무할당, 장애 격리(10절), 필터·템플릿·레이아웃, 출력 형식. 같은 기록 열은 두 모드에서 같은 출력을 낸다(6절 시험).
- **Sync에서 다른 것:**
  - 느린 sink가 호출 스레드를 늦춘다. 그래서 엔진 호스트를 Sync로 설정하면 메인 틱이 출력을 기다린다(설정 도움말에 적는다).
  - sink 콜백 안의 로그 호출은 스레드별 재진입 표시로 비상 경로로 보낸다.
  - `TextLogSink`는 기록마다 flush한다(도구 출력이 바로 보여야 하므로).
  - `Log::Fatal`의 3단계는 sink에 직접 쓰는 것으로 바뀐다. 다만 이 스레드가 이미 sink 안에 있거나 sink mutex를 잡을 수 없으면 건너뛴다(try-lock).

- **기존 `Logger`:** 공개 동기 API로 남기지 않는다. LogSystem 내부(Private)의 sink fan-out으로 다시 쓴다. `LogEvent`를 그대로 넘기고(시각·스레드를 다시 찍지 않음), sink별 결과를 돌려준다(10절 sink 차단에 필요). 큐를 우회하는 공개 경로는 없다.
- **수동 Pump 모드:** `LogSystemConfig::threadMode = LogThreadMode::Manual`이면 출력 스레드를 만들지 않고 `Pump()`를 부른 스레드에서 비운다. 시험은 sleep 없이 Pump 뒤에 단언한다. 스레드 모드에서는 `Flush()`가 "이 호출 전에 넣은 기록이 모두 sink에 도달함"을 이벤트로 보장한다.
- **비상 출력:** CRT `fwrite`가 아니라 OS 쓰기(`WriteFile`·`write(2)`)를 직접 부른다. `platform::Fatal`과 같은 방식이다(`Win64/Platform.cpp:82–96`). 그래야 출력 스레드의 stderr sink가 잡은 CRT stream lock에 막히지 않는다. lock은 없지만 OS I/O 대기는 남는다. PlatformBase에 `platform::WriteDiagnostic(StringView)`로 공개한다.
- **설치 전·해제 후:** Warning 이상은 고정 1 KiB 스택 버퍼에 렌더해 비상 출력으로 쓴다(힙·lock 없음). 그 아래는 버리고 원자 카운터에 센다. 다음 설치 때 그 수를 한 번 알린다. backlog 재생은 하지 않는다(`LogBuffer`는 단일 스레드 전용이고, 재생에는 중복·순서·넘침 정책이 필요하다).
- **시험 격리:** 시험마다 Manual 모드 LogSystem과 캡처 sink를 `LogInstallScope`로 설치한다. 레벨 재정의·sink·통계는 LogSystem 안에 있으므로 다른 시험으로 새지 않는다.
- `LogBuffer`는 단일 스레드 초기 진단으로 남는다(TraceSystem T8). `ProfileScope`는 TR2에서 `CpuScope`로 바뀐다.

## 6. API

### 6.1 호출

```cpp
Log::Trace(category, template, args...);
Log::Debug(...); Log::Info(...); Log::Warning(...); Log::Error(...);
[[noreturn]] Log::Fatal(category, template, args...);     // 11절
Log::TraceLazy(category, [&](LogLine& line) noexcept { line.Write("...", ...); });
Log::DebugLazy(category, ...);
bool Log::IsEnabled(LogLevel, const LogCategory&) noexcept;
```

- `Log`는 생성할 수 없는 `class`의 정적 함수 템플릿이다(Rocks 네임스페이스는 소문자라 `namespace Log`를 쓰지 않는다). 반환은 `void`다. 결과 확인이 필요한 저수준 계약은 sink 쪽(`RuntimeLogSink`, `Logger`)에 남는다.
- Lazy: 해당 레벨이 컴파일·런타임에서 활성일 때만 람다를 **호출 중 즉시** 한 번 부른다. 보관하지 않는다(게임 상태를 읽어야 하므로). `LogLine::Write`는 한 기록에 이어 붙인다. 람다 캡처 초기화(`[x = Build()]`)는 호출 전에 평가된다.

### 6.2 카테고리

```cpp
// <Module>/<Module>Log.h — 모듈마다 한 헤더
inline constexpr LogCategory LogEngine{"Engine"};
inline constexpr LogCategory LogGraphicsD3D12{"Graphics.D3D12"};
```

- 이름과, `consteval`로 미리 계산한 **점 접두사별 64-bit 해시 배열**(최대 `MaxLogCategoryDepth` = 8구간, `Graphics.D3D12`이면 `Graphics`와 `Graphics.D3D12` 두 개)을 가진다. 전역 생성자·가변 상태가 없다. 이름 문법(`[A-Za-z0-9]+`를 점으로 이음, 빈 구간 없음, 최대 63 byte, 8구간 이하)은 `consteval`로 검사한다. 표기는 PascalCase다.
- 같은 이름을 여러 곳에서 정의하면 같은 카테고리다. 출력에는 실제 이름이 나가므로 출력에서 카테고리가 섞이지 않는다.
- **레벨 필터만 해시로 판정한다**(7절). 서로 다른 접두사의 64-bit 해시가 같으면 한쪽 재정의가 다른 쪽에 적용될 수 있다.
  - 재정의를 넣을 때(mutex 안) 이미 있는 다른 이름과 해시가 같으면 `AlreadyExists`로 거부한다.
  - 표에 없는 이름과의 충돌은 넣을 때 알 수 없다. 이 남는 확률(쌍당 2⁻⁶⁴)은 받아들이고 이 문서에 기록한다.
- TraceSystem 채널(`cpu`, `gc`)과는 다른 이름 공간이다. 설정에서 매칭할 때는 대소문자를 무시한다.

### 6.3 메시지 템플릿

| 문법 | 뜻 |
|---|---|
| `{Name}` | 이름 있는 자리. 인자에 **순서대로** 묶이고 이름은 속성 키다(Serilog 규칙). 이름 문법은 카테고리의 한 구간과 같다 |
| `{Name:spec}` | spec은 기존 Rocks 서식 그대로(`#x`, `.3f`, `>8` …). 사용자 `Formatter<T>`는 spec을 직접 해석한다 |
| `{}` · `{0}` | 기존 위치 자리. 속성 키는 `Arg0`, `Arg1` …. 이름 자리와 한 템플릿에 섞으면 오류 |
| `{{` · `}}` | 중괄호 문자 |

- **파서는 하나다.** `FormatTo`의 문법 해석을 `constexpr` 함수로 분리하고 이름 자리를 추가한다. 컴파일 타임 검증·값 캡처·출력 렌더링이 같은 함수를 쓴다. `StringBuilder::TryAppendFormat`은 지금처럼 이름 자리를 거부한다.
- **컴파일 타임 검증(리터럴):** `LogTemplate<Args...>`의 `consteval` 생성자가 중괄호 짝, 이름·위치 혼용, 자리 수와 인자 수, 위치 번호 범위, **내장 타입**의 spec을 검사한다. 사용자 `Formatter`의 spec은 런타임에 검사한다. 문서와 주석에 "부분 검증"이라고 쓴다.
- consteval 안에서는 `static_assert`로 문자열을 낼 수 없다. 그래서 오류마다 영어 이름의 constexpr 아닌 함수(예: `LogTemplateError_ArgumentCountMismatch()`)를 부르고, 컴파일러 진단에 그 이름이 나오게 한다. 인자 수처럼 타입만으로 판정할 수 있는 것은 `static_assert`의 영어 메시지로도 낸다.
- **동적 템플릿:** `RuntimeTemplate{text}`로 명시한다. 검증은 런타임, 문자열은 기록에 복사한다. 오류는 10절대로 처리한다.
- `EventId`는 리터럴 템플릿의 `consteval` 64-bit 해시다(런타임 템플릿은 런타임 해시). 반복 집계·도구 묶음에 쓴다.
- 구조체 펼치기(Serilog `{@Obj}`)는 후속이다(16절).

### 6.4 호출 위치

```cpp
struct SourceLocation {
    const char* file; const char* function; uint32 line;
    static consteval SourceLocation Current(const char* file = __builtin_FILE(),
                                            const char* function = __builtin_FUNCTION(),
                                            uint32 line = __builtin_LINE()) noexcept;
};
template <typename... Args>
static void Info(const LogCategory&, LogTemplate<TypeIdentityT<Args>...> text, const Args&... args) noexcept;
```

- `LogTemplate`의 `consteval` 생성자가 기본 인자 `SourceLocation::Current()`를 받는다. 기본 인자는 호출 지점에서 평가되므로 로깅 내부가 아닌 실제 호출 위치가 남는다. `TypeIdentityT`(Base/TypeTraits.h)로 템플릿 인자 쪽 추론을 막는다. STL `<source_location>`은 쓰지 않는다.
- 교차 검증에서 clang-cl 21.1.6으로 동작을 확인했다. 실제 호출 위치가 남고, `TypeIdentityT` 추론 차단과 Lazy의 뒤쪽 기본 인자도 동작한다. LG2는 이를 TU 두 개·함수 두 개의 시험으로 고정한다. 람다 안에서 호출하면 함수 이름은 `operator()`다.
- 문자열은 정적 저장소라 프로그램 수명 동안 유효하다(모듈은 정적 링크·언로드 없음). 동적 모듈을 도입하면 다시 검토한다.
- 경로: Rocks.Build가 저장소 기준 상대 경로로 남긴다(LG5).
  - clang-cl은 `-ffile-prefix-map`을 "unknown argument ignored"로 무시하므로 `/clang:-ffile-prefix-map=<저장소 루트>=`로 넘긴다(`ActionGraph.cs:408`의 `/clang:-std=c++23`과 같은 방식).
  - 모듈 인자 검사가 절대 경로 값을 거부하므로(`InputValidation.cs:195`) 모듈이 아니라 toolchain 단에서 넣는다.

### 6.5 문맥(육하원칙의 "누가·어디서")

```cpp
LogContextScope world("World", world.Name());   // 이 범위의 모든 기록에 World=Main
LogContextScope stage("Stage", "BeginPlay");
```

- 스레드별 고정 깊이(8) 스택이다. 이름은 리터럴, 값은 `StringView`이며 **캡처할 때 기록으로 복사**하므로 범위가 끝난 뒤에도 안전하다. 깊이를 넘으면 가장 안쪽을 버리고 `contextTruncated`를 표시한다.
- Engine은 초기화·종료 단계(`Stage`), World Tick(`World`)마다 문맥을 넣고, 프레임 번호를 `LogSystem::SetFrame`으로 알린다.

| 원칙 | 채우는 곳 |
|---|---|
| 언제 | 단조 시각, UTC(출력 스레드 환산), 엔진 프레임 번호 |
| 어디서 | 카테고리, 파일·줄·함수, 스레드 이름(없으면 id), World 문맥 |
| 누가·무엇이 | 문맥과 속성(World, Actor·객체 이름, Subsystem, 장치·어댑터) |
| 무엇을 | 메시지 템플릿, `EventId` |
| 왜 | `ErrorCode`, 원본 코드(HRESULT·`GetLastError`·errno), 진단 원문 |
| 어떻게 | 단계, 취한 조치(fallback·재시도·중단), 수량·소요 시간 |

## 7. 레벨과 필터

- 우선순위: **컴파일 타임 하한 → 카테고리 재정의(점 접두사 최장 일치) → 전역 최소.** 재정의는 전역보다 자세하게도 조용하게도 할 수 있다. Fatal은 어떤 필터로도 막지 않는다.
- 컴파일 타임 하한은 Rocks.Build가 타깃 전체에 하나만 정의한다. 모듈마다 다르면 inline 함수 본문이 TU마다 달라져 ODR 위반이다. 카테고리별 컴파일 타임 하한은 두지 않는다(카테고리를 템플릿 인자로 넘겨야 해서 호출 모양이 무거워진다).
- 꺼진 레벨의 본문은 `if constexpr`로 비운다. 일반 인자는 평가된다. 문자열이 바이너리에서 빠지는지는 Release **최종 링크 결과**로 확인한다(ThinLTO라 `.obj`는 bitcode다).
- 컴파일에서 뺀 레벨은 런타임 설정으로 되살릴 수 없다. `SetLevel`이 그런 값을 받으면 성공하되 한 번 Warning을 남긴다.
- 런타임 변경(`SetGlobalMinimum`, `SetLevel(name, level)`, `ClearLevel`)은 mutex로 직렬화한다. 표 용량(64)을 넘으면 `CapacityExceeded`.
- **읽기는 원자 변수만 본다.** 표의 각 칸은 `Atomic<uint64>` 접두사 해시와 `Atomic<uint8>` 레벨(빈 칸 표시 포함)이다. 생산자는 카테고리가 가진 접두사 해시들을 긴 것부터 표와 대조한다. 이름 문자열은 mutex 안에서만 읽는다(진단·충돌 검사용). 비원자 데이터를 잠금 없이 읽지 않으므로 데이터 경쟁이 없다.
- 전역 최소와 "재정의 수"도 원자 변수다. 재정의가 0개면 표를 보지 않는다.
- 칸을 바꾸는 순서는 레벨 기록 → 해시 공개(release)이고, 지울 때는 해시를 먼저 비운다. 변경이 반환된 뒤 시작한 호출부터 적용된다. 변경 도중 진행 중인 호출은 이전 값 또는 새 값 중 하나를 본다.

| 빌드 구성 | 컴파일 포함 최소 | 런타임 기본 최소 |
|---|---|---|
| Debug | Trace | Info |
| Development | Debug | Info |
| Release | Info | Info |

## 8. 기록 경로

이 절은 Async·Manual 모드의 경로다. Sync는 5.1절이다.

### 8.1 생산자 연산(활성 기록 한 건)

1. 원자 읽기 1회(relaxed, 전체 하한). 비활성이면 끝.
2. 재정의가 있을 때만 카테고리 표 조회(원자 해시 비교).
3. 설치 확인(세대 카운터 증가, 포인터 재확인 — 5절).
4. 단조 시계 1회.
5. **스레드별 준비 버퍼에 캡처한다.** 값, 문맥 스냅샷, 사용자 Formatter 렌더, Lazy 람다 실행이 모두 여기서 일어난다. 준비 버퍼는 `thread_local`의 고정 배열(슬롯 크기)이다. 큰 값은 이때 블록 풀에서 빌린다.
6. 슬롯 예약 → 준비 버퍼 복사(memcpy) → 공개. 예약부터 공개까지는 사용자 코드를 실행하지 않는다.
7. 출력 스레드의 "잠듦" 표시를 `exchange(false)`로 가져온 생산자 **하나만** 깨운다(8.4).

- 5단계를 6단계보다 먼저 하는 이유가 있다. 예약한 슬롯을 연 채로 사용자 코드(Formatter, Lazy)가 돌면, 그 시간만큼 소비자 전체가 멈춘다. 그 안에서 다시 로그를 남기거나 assert가 나면 교착이나 Fatal flush 지연이 생긴다.
- 준비 버퍼 사용 중에 같은 스레드에서 로그가 다시 들어오면(Formatter 안의 로그 등) 바깥 기록을 망가뜨리지 않도록 깊이 1까지 두 번째 준비 버퍼를 쓴다. 그보다 깊으면 비상 경로로 보낸다.

생산자 경로에 없는 것: 내장 타입 서식화, UTF-8 검증(리터럴 템플릿은 컴파일 때, 문자열 값은 출력 스레드에서), 힙 할당, UTC 변환. lock과 시스템 호출은 출력 스레드가 잠들어 있을 때 그것을 깨우는 한 생산자만 한 번 거친다(8.4).

### 8.2 lock-free 유한 MPSC 큐

- `BoundedMpscQueue<T>`: 용량은 2의 거듭제곱, 생성 때 한 번 할당. 슬롯마다 64-bit 순번(Vyukov 계열).
  - `TryPush`: `pos = tail(relaxed)`, `diff = (int64)(slot.sequence(acquire) - pos)`.
    - `diff == 0`이면 `CAS(tail, pos, pos+1)`. 성공하면 기록 → `slot.sequence = pos+1`(release), 실패하면 새 `pos`로 다시.
    - `diff < 0`이면 가득 참 → `false`(대기 없음).
    - `diff > 0`이면 다른 생산자가 앞섰으므로 `tail`을 다시 읽는다.
  - `TryPop`(소비자 하나): `(int64)(slot.sequence(acquire) - (head+1)) == 0`이면 읽고 `slot.sequence = head+Capacity`(release), `head++`. 아니면 비었거나 아직 공개 전이다.
- **진행 보장:** 생산자는 lock-free다. 소비자는 슬롯을 예약한 생산자가 공개하기 전에 선점되면 그 슬롯에서 기다린다. 기다리는 것은 출력 스레드뿐이다. 예약과 공개 사이에는 memcpy만 있으므로(8.1) 대기는 선점 시간으로 한정된다. 소비자 쪽은 엄밀한 lock-free가 아니라고 주석에 적는다.
- 64-bit 순번이라 한 바퀴 ABA는 사실상 없다. 머리·꼬리는 서로 다른 캐시 라인, 슬롯 크기는 캐시 라인 배수. acquire/release 근거를 주석에 적는다.
- 스레드별 SPSC 링(TraceSystem 블록 구조)은 쓰지 않는다. 로그는 빈도가 낮고, 스레드 간 순서를 맞추려면 결국 공유 순번이 필요하다. LG3 측정에서 꼬리 경합이 목표를 넘으면 바꾼다(L9). 큐 인터페이스를 고정해 교체 비용을 LogSystem 안으로 한정한다.

### 8.3 기록 인코딩과 블록 풀

- 슬롯(초기 512 B): 머리(레벨·카테고리 포인터·위치·EventId·단조 시각·스레드·프레임·플래그·값 수) + 값 영역.
- 값: 정수·실수·bool·포인터·enum은 원시 값, 문자열은 바이트 복사, 사용자 `Formatter<T>` 타입은 **캡처할 때 준비 버퍼에 렌더링**한다(객체 수명이 끝날 수 있으므로). 이 비용은 호출 스레드에 남는다.
- 렌더링에는 Core에 새로 추가하는 **잘림 모드 고정 버퍼 builder**(`FixedStringBuilder`, `StringBuilderBase` 파생, LG2)를 쓴다.
  - 외부 버퍼를 받고 할당하지 않는다.
  - 넘치면 UTF-8 문자 경계에서 자르고 **성공**하며, `Truncated()` 표시를 남긴다.
  - 지금의 `StringBuilder`는 넘치면 서식 호출 전체가 실패하고 되돌아가므로(3절) 이 용도에 쓸 수 없다.
  - Formatter가 자기 오류를 반환하면 그 자리만 `<format error>`가 된다(10절).
- 슬롯을 넘는 값(셰이더 진단 등)은 Start에서 확보한 4 KiB 블록을 빌린다. 고갈되면 잘라서 `truncated`.
- 블록 풀은 lock-free 스택(Treiber)이다.
  - 머리는 인덱스와 태그를 64-bit 원자 하나에 묶는다. 성공한 CAS마다 태그를 올려 ABA를 막는다.
  - 다음 인덱스 배열 `next[]`도 원자 변수다(빌리는 쪽이 읽는 동안 다른 스레드가 쓸 수 있으므로).
- 런타임 템플릿 문자열도 값 영역(또는 블록)에 복사한다.

### 8.4 출력 스레드

- **깨우기**(Dekker 순서, 양쪽에 seq_cst fence)는 다음과 같다. 주기 타이머는 쓰지 않는다.
  - 출력 스레드: `event.Reset()` → `sleeping.store(true)` → **fence** → 큐가 비었는지 다시 확인한다. 비어 있으면 `event.Wait()`, 아니면 `sleeping.store(false)` 후 처리한다.
  - 생산자: 공개(release) → **fence** → `sleeping.exchange(false)`가 `true`를 돌려준 생산자 하나만 `event.Signal()`한다.
  - 이 순서라 한쪽은 반드시 상대의 기록을 본다(신호 유실 없음).
  - Warning 이상도 같은 규칙이다. 출력 스레드가 깨어 있으면 신호하지 않는다.
- **깨우기 비용:** `ManualResetEvent::Signal`은 mutex를 잡는다(`Sync.h:89–92`). 잠든 출력 스레드를 깨우는 생산자 하나가 잠듦 주기마다 짧은 lock과 시스템 호출을 한 번 거친다. 나머지 생산자는 lock이 없다. LG3 측정에서 이 비용이 목표를 넘으면 PlatformBase에 주소 대기(WaitOnAddress·futex) 이벤트를 추가한다.
- **묶음 처리:** 큐에서 꺼내 템플릿 렌더링 → UTC 환산 → 다시 쓴 fan-out(5절, `LogEvent` 통과·sink별 결과) → sink. 묶음 끝에 flush하고, Warning 이상이 든 묶음은 바로 flush한다.
- **UTC:** 출력 스레드가 (단조, UTC) 기준점 쌍을 묶음마다 다시 맞춰 환산한다. `DateTime::UtcNow`와 `platform::UtcTicks`는 실패하면 Fatal이다. 그래서 PlatformBase에 실패를 돌려주는 `platform::TryUtcTicks`를 추가하고(LG3), 실패하면 UTC를 비운다.
- 순서: 큐 예약 순번이 전역 순서다. 한 스레드 안의 순서는 항상 보존된다.
- sink 추가·제거는 명령으로 출력 스레드에 넘긴다. `RemoveSink`는 출력 스레드가 확인한 뒤 반환한다. sink 콜백은 출력 스레드(Manual이면 Pump 스레드)에서만 실행된다. 그래서 생산자는 sink lock을 잡지 않고, 엔진 lock을 쥔 채 로그를 남겨도 교착이 없다.

### 8.5 포화

- 큐 사용량이 90%를 넘으면 Info 이하를 거부한다. 남은 10%는 Warning 이상 전용이다. 그래도 차면 버린다.
  - 사용량은 `tail - head`를 relaxed로 읽은 근사값이다. 그래서 동시에 검사를 통과한 생산자 수만큼 Info가 예약 칸에 들어갈 수 있다. 예약 칸(410개)이 생산자 스레드 수보다 훨씬 크므로 허용한다.
- 손실은 레벨별 원자 카운터로 센다. 출력 스레드가 공간을 되찾으면 `"{Dropped} log records dropped ({Trace} Trace, {Debug} Debug, {Info} Info, {Warning} Warning, {Error} Error)"`를 한 번 남긴다.
- 생산자는 기본적으로 기다리지 않는다. 오프라인 도구·Commandlet처럼 손실이 더 나쁜 호스트는 `LogSystemConfig::overflow = LogOverflow::WaitForSpace`를 고를 수 있다(게임 스레드에서는 쓰지 않는다, L7).

### 8.6 메모리 예산(초기값, LG3 측정 후 조정)

| 메모리 | 출처 |
|---|---|
| 큐 | Start 1회, 4096 × 512 B = 2 MiB |
| 큰 값 블록 | Start 1회, 64 × 4 KiB = 256 KiB |
| 문맥 스택 · 준비 버퍼 | 스레드별 `thread_local` 고정 배열(정적 TLS, 힙 없음, 스레드당 약 1.5 KiB) |
| 재정의·스레드 이름 표 | LogSystem 안 고정 배열 |
| 출력 렌더 버퍼 | Start 1회, 64 KiB(레이아웃·JSON 공용, `FixedStringBuilder`) |
| JSON writer 작업 공간 | Start 1회. `JsonWriter` 무할당 옵션(고정 깊이 frame, 중복 키 색인 끔)이 쓰는 고정 영역 |
| stderr | CRT 버퍼(우리 할당 없음) |

- **목표치(보장 아님, LG3에서 Release로 측정해 보고):** 비활성 호출은 분기 하나 수준, 활성 호출(내장 타입 인자) p99 1 µs 미만, Start 뒤 할당 0회. 넘기면 원인과 대안을 함께 낸다.

## 9. 출력

- `RuntimeLogSink`는 `Write(const LogEvent&)`와 `Flush()`를 가진다. `LogEvent`는 렌더된 메시지와 원본(레벨, 카테고리 이름, 템플릿, EventId, 속성 목록, 문맥, 위치, 스레드 이름·id, 프레임, 단조 시각, UTC, 플래그)을 담는다. 모든 문자열은 **콜백 동안만** 유효하다. 보관하려면 복사한다.
- **레이아웃 패턴:** sink를 만들 때 한 번 해석해 토큰 목록으로 바꾼다. 잘못된 패턴은 생성에서 `Result`로 실패한다(기록 때 해석하지 않는다).

| 토큰 | 출력 |
|---|---|
| `{Timestamp}` · `{Timestamp:ms}` | UTC ISO 8601(`Z` 포함) · 밀리초 3자리 고정 폭(`Z` 포함) |
| `{Elapsed}` | 시작 이후 단조 경과(초.밀리초) |
| `{Level}` · `{Level:u3}` | `Warning` · `WRN` |
| `{Category}` · `{EventId}` | 카테고리 이름 · 16진 |
| `{Thread}` · `{Frame}` | 스레드 이름(없으면 id) · 프레임 |
| `{Message}` · `{Template}` | 렌더된 메시지 · 원본 템플릿 |
| `{Properties}` · `{Context}` | 템플릿에 안 쓰인 속성 · 문맥(`World=Main Stage=BeginPlay`) |
| `{Location}` | `Engine/Private/Engine.cpp:118 Initialize`. sink 설정으로 최소 레벨(기본 Warning) |
| `{NewLine}` | 줄바꿈 |

- 기본 콘솔 레이아웃: `[{Timestamp:ms}][{Level}][{Category}] {Message}{Context}{Location}{NewLine}` (빈 토큰은 앞 공백·괄호와 함께 생략).
- `TextLogSink`·`StderrLogSink`는 레이아웃을 받고 묶음 flush로 바뀐다. 기존 `[Info][3] ` 형식과 `LoggerOutputTests.cs`는 새 형식으로 고친다(하위 호환 없음).
- `JsonLinesLogSink`: 한 줄에 객체 하나(`t`, `mono`, `lvl`, `cat`, `event`, `tpl`, `msg`, `props`, `ctx`, `src`, `thread`, `frame`, `flags`).
  - Core `JsonWriter`에 옵션을 추가해 재사용한다: 한 줄(compact) 출력, 고정 깊이 frame 배열, 중복 키 색인 끔.
  - 지금 `JsonWriter`는 `Array<Frame>`과 중복 키 `HashMap`을 할당한다(`Json.h:373–374`). 로그 속성 이름의 중복은 템플릿 검증에서 이미 막힌다.
  - 출력은 렌더 버퍼(`FixedStringBuilder`)에 쓴다. 이 경로에서 할당이 0인지는 LG3·LG4 무할당 시험이 확인한다.
- `DateTime` 서식에 고정 폭 밀리초 표기를 추가한다(기존 `{}` 출력은 그대로).

### 9.1 파일 sink와 암호화

- `FileLogSink`(Core): 실행마다 새 파일(`<로그 폴더>/<호스트>-<UTC 시작 시각>.log`), 크기 상한을 넘으면 회전(`log.file.maxBytes`), 보관 개수 상한. 쓰기는 PlatformBase 파일 API를 쓴다. flush 묶음 단위로 쓰고, 버퍼는 Start에서 확보한다(무할당).
- **암호화는 선택이고 기본은 끔이다**(`log.file.encrypt = false`). Core는 Crypto보다 아래 계층이라 Core에는 인터페이스만 둔다.

```cpp
// Core: 파일 sink가 flush 묶음의 바이트를 넘기는 변환 지점
class LogFileCipher {
public:
    virtual ~LogFileCipher() noexcept = default;
    virtual usize HeaderBytes() const noexcept = 0;
    virtual usize SealOverhead() const noexcept = 0;
    virtual Result<usize> TryWriteHeader(Span<uint8> out) noexcept = 0;            // 파일 처음 한 번
    virtual Result<usize> TrySeal(Span<const uint8> batch, bool last, Span<uint8> out) noexcept = 0;
};
```

- 구현 `crypto::LogFileSealer`는 Crypto 모듈에 둔다(Crypto는 Core 위). 자체 암호 구현은 없다.
  - **파일 키:** 파일마다 Crypto KEM(X-Wing, `Kem.h`)으로 개발자 **공개키**에 캡슐화한 공유 비밀에서 KDF 타입 파생으로 `SecretStreamKey`를 뽑는다. 파일 머리에는 KEM 암호문과 secretstream header를 둔다.
  - 게임 바이너리에는 공개키만 들어간다. 그래서 바이너리를 분석해도 기존 파일을 열 수 없다. 여는 쪽은 개인키를 가진 개발자 도구뿐이다.
  - "사용자가 알아볼 수 없는 수준"이라는 요구보다 강하다. 그래도 고른 이유는 비용이 같기 때문이다. 파일당 KEM 한 번이고, 기록당 비용은 대칭 봉인뿐이다. 고정 키를 바이너리에 숨기는 방식(L13 대안)보다 키 관리가 단순하다(L13 결정).
  - **봉인 단위:** flush 묶음 하나가 secretstream 메시지 하나다(tag `Message`, 정상 종료 때 `Final`). 묶음당 17 byte가 늘어난다. 비정상 종료로 `Final`이 없는 파일도 마지막으로 쓴 묶음까지 복호된다. 복호 도구는 "끝 표시 없음(비정상 종료)"을 알린다.
  - 봉인은 출력 스레드에서 한다(Async). 메인 틱 비용은 0이다. Sync 모드(도구)에서는 호출 스레드가 낸다. 출력 버퍼는 Start에서 확보한다. 키 상태는 Crypto 키 저장소 슬롯에 있고 파일을 열 때 한 번 만든다.
  - 암호화를 켰는데 키 준비(KEM·키 저장소)에 실패하면 **평문으로 내려가지 않는다.** 파일 sink를 끄고 비상 경로로 한 번 알린다(P3: 엔진은 계속 실행, 민감 로그는 평문으로 남지 않음).
- **공개키 공급:** 엔진 호스트 설정 `log.file.recipientKey`(공개키, base64)를 쓴다. 프로젝트 설정 파일에 두며 비밀이 아니다. 키 쌍 생성과 개인키 보관은 개발자 도구(Commandlet `log keygen`)가 한다. 개인키는 저장소·쿠킹 결과에 넣지 않는다.
- **복호 도구:** Commandlet `log decode <file> --key <개인키 파일>`가 평문 텍스트나 JSON lines로 출력한다.
- 콘솔(stderr) sink는 암호화하지 않는다. 출시 빌드에서 콘솔 출력이 문제라면 콘솔 sink 자체를 끈다(호스트 설정).

```
[2026-11-17T02:23:45.123Z][Info][Engine] Engine initialized with 3 subsystems
[2026-11-17T02:23:47.456Z][Warning][World] World Main destroy deferred for 300 frames: external references World=Main (Engine/Private/Engine.cpp:262 ProcessDeferred)
```

## 10. 장애 격리

로깅 코드는 입력·환경 때문에 생기는 조건으로 Fatal·`ROCKS_ASSERT`를 부르지 않는다. 의도적 종료는 `Log::Fatal`뿐이다. 로그를 모두 꺼도, 모두 실패해도 엔진 상태 전이는 같다.

| 실패 | 동작 |
|---|---|
| 런타임 템플릿 오류 | 원본 템플릿 + `[template error: <code>]`로 기록, `templateError` 플래그 |
| 사용자 Formatter 실패 | 그 자리만 `<format error>`, 나머지 렌더 |
| 값이 슬롯·블록보다 큼 / 블록 고갈 | 잘라서 `truncated` |
| 큐 포화 | 8.5대로 버리고 레벨별 집계 |
| sink 실패 | 다른 sink는 계속. 같은 sink가 연속 `SinkFailureLimit`(8)번 실패하면 차단하고 비상 경로로 한 번 알림. `SinkRetryBatches`(256)묶음마다 한 번 재시도 |
| 출력 스레드 안(sink 콜백 등)에서 로그 호출 | 큐에 넣지 않고 비상 경로로만. 재귀 없음, 집계 |
| 준비 버퍼 안(Formatter·Lazy)에서 로그 호출 | 깊이 1까지 두 번째 준비 버퍼, 그보다 깊으면 비상 경로(8.1) |
| 출력 스레드 시작 실패 | Start는 성공하고 **동기 비상 모드**로 낮춤(Warning 이상만 호출 스레드에서 stderr). 한 번 알림 |
| 종료 drain 시간 초과 | 남은 수를 비상 경로로 알리고 진행 |
| 설치 전·해제 후 | 5절 |
| 비상 경로 자체 실패 | 무시(더 내려갈 단계가 없다) |

- 통계 `LogSystemStats`: 레벨별 손실, 잘림, 템플릿 오류, sink 실패·차단, 재진입, 설치 전 버림. 시험과 진단 명령이 읽는다.

## 11. Fatal과 assertion

`Log::Fatal`(`[[noreturn]]`)은 다음 순서로 동작한다.
1. 호출 스레드에서 고정 스택 버퍼로 동기 렌더한다.
2. 비상 출력(OS 쓰기, 5절)으로 바로 쓴다.
3. 큐에 넣을 수 있으면 넣고, 출력 스레드에 flush를 요청해 `FatalFlushTimeout`만큼 기다린다(best effort).
4. **무조건** `platform::Fatal`을 부른다. 필터·sink 상태와 무관하게 종료한다.

3단계를 건너뛰는 경우:
- 출력 스레드 자신이 Fatal을 부를 때
- Manual 모드(기다릴 출력 스레드가 없음)
- 출력 스레드가 동기 비상 모드일 때

- `AssertFailed`도 2–3단계를 거친 뒤 지금처럼 `platform::Fatal`로 끝난다.
- **재귀 차단:** 스레드별 "Fatal 진행 중" 표시를 둔다. 로깅 내부에서 assert가 나면 `AssertFailed` → 로그 → assert로 돌 수 있는데, 표시가 이미 서 있으면 바로 `platform::Fatal`로 간다.

## 12. 작성 규칙

- **영어(P5).** 템플릿, 자리 이름, 카테고리 이름, 비상 문구, `Log::Fatal`·assert 문구는 영어. 주석은 한국어. 인자 값(이름·경로)은 원문 UTF-8 그대로. 사용자에게 보이는 문구는 로그가 아니라 지역화 Text 계층.
- **육하원칙(P1).** Warning 이상은 한 줄에 **무엇이 실패했나 · 대상 · 원인(ErrorCode·원본 코드) · 엔진이 한 조치**를 담는다. 언제·어디서는 자동이다. 대상이 문맥에 이미 있으면 반복하지 않는다.
  - 나쁜 예: `"Device creation failed"`
  - 좋은 예: `"Failed to create {Backend} device on {Adapter}: {Error} (HRESULT {HResult:#x}); falling back to {Fallback}"`
- **레벨:** Trace = 매 프레임·매 객체 상세(계측 목적이면 trace). Debug = 개발 중 상태, GC 통계, 첫 재시도. Info = 수명 주기, 선택 결과, 복구 완료(프레임마다 반복 금지). Warning = 계속 실행 가능하나 사람이 봐야 함(fallback, 지속 지연, 순환 거부). Error = 이 계층에서 실패가 확정됨. Fatal = 불변식 붕괴로 종료.
- **정상 결과는 로그가 아니다.** NotFound·Busy·Unsupported·취소, 호출자가 fallback할 수 있는 하위 실패는 Error로 남기지 않는다.
- **한 번만 기록한다.** 저수준 API는 오류와 원본 정보를 반환값·report로 넘긴다. 실패를 확정하거나 fallback을 고르는 계층이 한 번 기록한다. 같은 EventId 반복은 처음 한 번과 개수 요약만.
- **인자는 평가된다.** 비싼 값은 Lazy로. 동작에 필요한 부작용을 로그 인자에 넣지 않는다.
- **민감 정보 금지.** 비밀번호·토큰·키·사용자 입력 전문을 기록하지 않는다(계획 29.4절).
- **lock:** 생산자는 sink lock을 잡지 않는다. 잠든 출력 스레드를 깨울 때 이벤트 lock을 한 번 짧게 잡는데, 이것은 말단 lock이라 다른 lock을 잡지 않는다. 그래서 엔진 lock 안에서 로그를 남겨도 교착이 없다. sink는 엔진 lock을 잡지 않는다.

## 13. 엔진 적용

현재 구현된 경로에만 넣는다. 기록 위치는 실패 문맥을 가진 유일한 계층이다.

| 대상 | 레벨 | 템플릿 · 속성 | 기록 위치 |
|---|---|---|---|
| 초기화 시작·완료 | Info | `Engine initialized with {SubsystemCount} subsystems in {Ms} ms` | `Engine::Initialize`, 단계마다 `Stage` 문맥 |
| 초기화 실패 | Error | `Engine initialization failed at {Stage}: {Error}` — Stage = TypeRegistry·ModuleRegistration·RegisterTypes·ObjectRegistry·Subsystems·ClientStart | Engine(호스트는 재기록하지 않음) |
| Teardown 정리 실패 | Fatal | `Engine {Registry} could not shut down after initialization failure at {Stage}: {Error}` | 기존 `platform::Fatal` 대체 |
| Run 종료 | Info | `Engine run finished after {Frames} frames: {Reason}` | `Engine::Run` |
| World Tick 실패 | Error | `World {World} tick failed: {Error}` (EventId 반복 요약) | `Engine::RunFrame` |
| World 파괴 지연 | Debug → Warning → Info | 첫 지연 Debug, `WorldDestroyWarnFrames`(300) 넘으면 Warning 1회, 해소되면 `World {World} destroyed after {Frames} deferred frames` | `ProcessDeferred`, WorldContext에 프레임 수·경고 여부 |
| 종료 | Info / Error | 실패 시 `{Worlds}`, `{PendingDestroys}`, `{LiveObjects}`, `{Error}` | `Engine::Shutdown` |
| CreateWorld 내부 실패 | Error | `World {World} failed during {Stage}: {Error}` | Engine |
| Tick 순환 거부 | Warning | `Rejected tick prerequisite {Before} -> {After}: would create a cycle` | 이름을 가진 World·Actor 경계. TickScheduler 오류 코드 분리 |
| 장치 생성 성공 | Info | `Created {Backend} device on {Adapter} (validation {Validation})` | 생성 함수 |
| 장치 생성 실패 | Warning / Error | fallback이면 Warning, 모든 후보 실패면 Error. HRESULT·단계 포함 | 호출자(선택자), `DeviceCreateReport` |
| Present 실패·장치 제거 | Error | `Device removed during present: {Reason} (HRESULT {HResult:#x})`, lost 전이 때 1회 | 장치 내부 |
| 셰이더 컴파일 실패 | Error | `Shader {Entry} ({Target}) failed to compile: {Diagnostics}` | Renderer |
| 검증 계층 메시지 | Warning | 원문, 메시지 ID마다 1회, `Graphics.D3D12.Validation` | 장치 내부 |
| 창 생성 실패 | Error | `Window creation failed at {Call}: {OsError}` | 호스트, `WindowCreateReport` |
| ShaderToolLoader | Error / Debug | 필수 도구 실패 Error, 선택 도구 없음 Debug | 호출자 |
| GC·Registry | Debug / Error | Collect 통계 Debug, 종료 실패 Error | Engine |

```cpp
struct DeviceCreateReport {   // 호출자 스택, 힙 없음
    ErrorCode code = ErrorCode::None;
    int64 nativeCode = 0;     // HRESULT, GL error
    char stage[48]{};
    char detail[512]{};
    bool truncated = false;
};
Result<UniquePtr<Device>> CreateD3D12Device(Allocator&, const DeviceDesc&, DeviceCreateReport* report = nullptr) noexcept;
```

장치 안 `Fail(const char*)`은 `Fail(stage, HRESULT)`로 바꿔 원본 코드를 보존하고 `LastError()`에도 덧붙인다. 창은 `WindowCreateReport`. 데스크톱 OpenGL은 제외 방향이므로 OpenGL은 생성 실패 진단만 최소로 넣는다.

## 14. 단계와 완료 기준

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| LG0 | 이 문서 | — | 교차 검증 반영 |
| LG1 | `LogLevel`(Debug·Fatal), `LogCategory`(접두사 해시), `SourceLocation`, 기록 모델(속성·문맥·프레임·스레드 이름), Logger·sink의 `uint32` 카테고리 교체(`AesModes.cpp` 포함), Base 캐시 라인 상수. **`LogBuffer`·`ProfileScope`의 카테고리 타입은 그대로 둔다**(15절) | LG0 | 기존 Logger·LogBuffer·ProfileScope·LoggerOutputTests 통과 |
| LG2 | 공용 `constexpr` 서식 파서·이름 자리, `FixedStringBuilder`(잘림 모드), `LogTemplate` 검증, 위치 수집 시험, EventId, 값 캡처 인코딩, Lazy, 컴파일 실패 시험 체계(Rocks.ToolTests가 clang-cl `-fsyntax-only`) | LG1 | 템플릿·위치(TU 2개)·Lazy 0회/1회·잘림·컴파일 실패 시험 |
| LG3 | `BoundedMpscQueue`, 블록 풀, 준비 버퍼, `LogSystem`(Async·Sync·Manual 모드, 프리셋, 출력 스레드·깨우기·Pump·Flush·포화·sink 명령·통계), fan-out 재작성, 세대 카운터 `LogInstallScope`, `platform::WriteDiagnostic`·`TryUtcTicks`, 설치 전후 경로, 장애 격리, `Log::Fatal`·`AssertFailed` | LG2 | 큐 스트레스·포화·wrap·멈춘 생산자 결정적 시험, **무할당 시험**(호출 수 세는 할당기로 Start 뒤 0회), 깨우기 유실 없음, 해제 기아 없음, Sync와 Manual의 출력 동일·Sync 무할당, 장애 주입, TSan·Asan, 호출 비용 측정 보고 |
| LG4 | 레이아웃 해석기, Text·Stderr sink 개편(묶음 flush), `JsonWriter` 옵션(한 줄·고정 frame·중복 색인 끔)·`JsonLinesLogSink`, `DateTime` 고정 폭 밀리초, `LogContextScope` | LG3 | 토큰별 결정적 출력, 잘못된 패턴 실패, JSON 왕복, 무할당 |
| LG5 | `ROCKS_LOG_COMPILED_MINIMUM`, toolchain 단 `/clang:-ffile-prefix-map=`, Release 문자열 제외 검사, 템플릿 비ASCII 정책 검사 | LG2 | 구성별 빌드, 최종 링크 결과 검사 |
| LG6 | Engine·World 적용(13절) | LG4, **15절 조건** | 단계별 실패 주입 → Stage, 파괴 지연 1000프레임 → Warning 정확히 1회·해소 Info 1회 |
| LG7 | Graphics·창·셰이더 도구(report, HRESULT, 장치 제거 사유, 검증 메시지, 셰이더 진단) | LG4, **15절 조건** | 실패 주입 시험, Sandbox 자체 시험 |
| LG8 | 호스트: Sandbox 헤드리스 `--engine`(Async, L1), Commandlet 등 명령줄 도구(Sync). 사용 가이드 `Docs/Implementation/Logging.ko.md`, FoundationStatus·UE-C22·D01 갱신 | LG6, LG7 | Sandbox 실행으로 시작·종료·실패 원인 추적 확인 |
| LG9 | 설정 `log.mode`·`log.file.*`(그리고 레벨 재정의) 등록·읽기 | LG8, LG10, ConfigSystem CF2 | 설정 파일 값으로 모드·레벨·파일 기록이 바뀌고, 실행 중 변경은 거부(시작 뒤 고정) |
| LG10 | `FileLogSink`(실행별 파일·크기 회전·보관 상한), `LogFileCipher` 인터페이스(Core), `crypto::LogFileSealer`(Crypto: KEM 캡슐화 + SecretStream 묶음 봉인), Commandlet `log keygen`·`log decode` | LG4 | 평문·암호 파일 왕복, `Final` 없는 파일 부분 복호·경고, 변조 검출, 키 준비 실패 시 평문으로 내려가지 않음, 무할당(파일 열기 뒤) |

검증: 커밋마다 Debug 빌드·format·변경 부분 시험. 전체 체인(Asan·TSan)은 LG3과 LG8 끝에 한 번씩. 메인 트리 구현은 에이전트 하나가 순서대로 하고 커밋은 경로를 지정한다.

## 15. 다른 세션과의 조율

`feature/engine-runtime` 밑에서 그래픽 관련 세션이 여럿 진행 중이다. 2026-11-17 기준 `feature/render-lane`(아직 병합 전)이 이 문서의 적용 대상 파일 다수를 고쳤다.

| 파일 | render-lane | 로깅 단계 |
|---|---|---|
| `Engine/Private/Engine.cpp`, `Engine/Public/Engine/Engine.h`, `Engine/Tests/EngineTests.cpp` | 고침 | LG6 |
| `D3D12Graphics/Private/Win64/D3D12Device.cpp`, `OpenGLGraphics/Private/Win64/OpenGLDevice.cpp`, `Graphics/Public/Graphics/Device.h` | 고침 | LG7 |
| `Renderer/Renderer.Build.cs`(`Renderer.cpp`는 안 고침) | 고침 | LG7 |
| `Programs/Sandbox/*` | 크게 고침 | LG7·LG8 |
| `PlatformBase/Public/Platform/Thread.h`(`ThreadOptions` 이름·우선순위) | 고침 | LG3 출력 스레드 이름 |

규칙:
- **LG0–LG5는 Core·Base·PlatformBase·빌드만 고친다.** 그래픽·Engine 파일을 건드리지 않으므로 지금 진행한다.
  - PlatformBase에서는 `WriteDiagnostic`·`TryUtcTicks` 추가만 한다. render-lane이 고친 `Thread.h`·`Thread.cpp`·`Concurrency.cpp`는 건드리지 않는다.
- **trace 세션(`feature/trace-capture`)과의 겹침:**
  - TR2는 `ProfileScope.h`·`ProfileScope.cpp`를 지우고 `Core/Tests/ReentryTests.cpp`의 사용을 `CpuScope`로 바꾼다(T8). 그래서 로깅은 `ProfileScope`와 `LogBuffer`의 카테고리 타입(`uint32`)을 바꾸지 않는다. `LogLevel`에 값을 더하는 것만으로는 두 파일에 영향이 없다. `LogBuffer` 카테고리 정리는 TR2 병합 뒤 별도로 판단한다.
  - TR1은 PlatformBase `Clock.h`와 플랫폼별 `Platform.cpp`의 시계 부분(`MonotonicTicks`·`MonotonicTickFrequency`)을 고쳤다. LG3의 `TryUtcTicks`는 같은 파일이다. LG3 착수 때 TR1 병합 여부를 확인한다. 미병합이면 파일 끝 쪽에 따로 추가해 충돌을 줄이고, trace 세션에 알린다.
  - 병합은 먼저 끝나는 쪽이 먼저 한다.
- LG1은 `Crypto/Private/Aes/AesModes.cpp:172–176`(유일한 시험 밖 `StderrLogSink` 사용)을 고친다. `feature/crypto` 재병합 때 작은 충돌이 날 수 있으므로 커밋 메시지에 적는다.
- **LG6–LG8은 render-lane이 `feature/engine-runtime`에 병합된 뒤 시작한다.** 병합 전에 시작해야 하면 해당 세션과 먼저 파일 단위로 맞춘다.
- 출력 스레드 이름은 render-lane의 `ThreadOptions::name`이 병합되면 그것으로 붙인다. 그 전에는 이름 없이 시작하고 로그 쪽 스레드 이름 표에만 등록한다.
- 그래픽 세션에 요청하는 것:
  1. 새로 쓰는 실패 경로에서 **원본 오류(HRESULT·`GetLastError`·GL error)를 버리지 않는다.** 13절 `DeviceCreateReport` 모양을 따르면 LG7 충돌이 작아진다.
  2. 새 진단을 `printf`·`fprintf(stderr)`로 넣지 않는다. LG3 전이라면 `TODO(log)` 표시만 남긴다. LG3 뒤에는 `Log::*`를 12절 규칙(영어·육하원칙·결정 계층에서 한 번)으로 쓴다.
  3. 렌더 스레드·레인 lock 안에서 로그를 남겨도 된다. 생산자는 sink lock을 잡지 않는다. 잠든 출력 스레드를 깨우는 한 번의 짧은 이벤트 lock만 있다(8.4). 렌더 스레드에서도 할당은 없다.
  4. 병합하면 이 세션(로깅)에 알린다.
- 이 문서의 개정과 LG3·LG4 완료는 그래픽 세션에 알린다.

## 16. 범위 밖·후속

- 시간 기준 회전·압축 보관, TraceSystem `log` 채널 sink와 `TraceContext` 상관 id 연결(TR 단계 뒤), ConfigSystem에서 레벨·레이아웃 읽기(CF 단계 뒤).
- 리플렉션 기반 구조체 펼치기(`{@Obj}`), 사용자 `Formatter<T>` spec의 컴파일 타임 검증 규약.
- 카테고리별 일반 rate limit 설정(이번에는 EventId 반복 요약만).
- WorkerScheduler 지연·포화 진단(Engine이 스케줄러를 소유할 때).

## 17. 결정 사항

| 번호 | 결정 | 상태 |
|---|---|---|
| L1 | Engine 실행 호스트는 Sandbox 헤드리스 `--engine` | 결정됨(2026-11-17, 권장안) |
| L2 | Release 컴파일 포함 최소는 Info | 결정됨(2026-11-17, 권장안) |
| L3 | 카테고리 표기는 PascalCase 점 경로, 설정 매칭은 대소문자 무시, trace 채널과 별도 이름 공간 | 결정됨(2026-11-17, 권장안) |
| L4 | `Log::Fatal` 공개(11절 계약) | 결정됨(2026-11-17, 권장안) |
| L5 | 시각은 UTC ISO 8601 + `Z`, 로컬 변환은 viewer | 결정됨(2026-11-17, 권장안) |
| L6 | 착수: 지금. LG0–LG5 먼저, LG6–LG8은 render-lane 병합 뒤 | 결정됨(2026-11-17, 사용자 착수 지시) |
| L7 | 포화 기본은 버리고 집계(Warning 예약 10%), 도구 호스트만 `WaitForSpace` 선택 가능 | 결정됨(2026-11-17, 권장안) |
| L8 | 메시지는 영어 | 결정됨(2026-11-17, 사용자 지시) |
| L9 | 큐는 단일 lock-free 유한 MPSC로 시작, 측정에서 경합이 목표를 넘으면 스레드별 링으로 | 결정됨(2026-11-17, 사용자 지시·권장안) |
| L10 | 하위 호환 없음: `uint32` 카테고리·`LogFilter`·기존 출력 형식을 바로 교체 | 결정됨(2026-11-17, 초기 단계 원칙) |
| L11 | 비동기는 선택 사항이다. 명령줄 도구는 기본 Sync, tick이 있는 엔진 호스트(게임·에디터)는 기본 Async이며, 설정 파일 `log.mode`로 동기·비동기를 고른다(시작 뒤 고정) | 결정됨(2026-11-17, 사용자 지시) |
| L12 | 파일 기록 암호화는 선택이고 **기본은 끔**이다(`log.file.encrypt`). 켜면 Crypto `SecretStream`으로 flush 묶음 단위로 봉인한다. 키 준비에 실패하면 평문으로 내려가지 않고 파일 sink를 끈다 | 결정됨(2026-11-17, 사용자 지시) |
| L13 | 파일 키는 파일마다 개발자 공개키로 KEM(X-Wing) 캡슐화한다. 바이너리에는 공개키만 들어간다. 대안은 바이너리에 고정 대칭키를 숨기는 것이다(요구 수준은 충족하지만 바이너리 분석으로 풀린다) | 결정됨(2026-11-17, 사용자 확인 — 권장안) |

## 18. 개정 이력

- 개정 1(2026-11-17): 초안·확정. 로깅 작업 요청문을 저장소 코드와 계획 29.4절·TraceSystem·ConfigSystem에 대조하고, 사용자 원칙 P1–P6(육하원칙, 메인 틱 비차단 비동기, 장애 격리, Serilog·log4j식 서식, 영어, 무할당 lock-free)을 반영했다. UE 5.8.4 `Logging/*`·`OutputDeviceRedirector`를 확인했다. 교차 검증(리뷰어 1명, clang-cl 21.1.6 실측 포함)을 반영했다. 바뀐 점은 다음과 같다.
  - 준비 버퍼 뒤 예약·복사·공개
  - `FixedStringBuilder` 잘림 모드
  - `JsonWriter` 무할당 옵션
  - Dekker 깨우기 순서와 깨우기 lock 인정
  - 원자 해시 재정의 표
  - 세대 카운터 해제
  - Fatal 재귀 차단과 OS 직접 쓰기
  - `/clang:-ffile-prefix-map`
  - `TryUtcTicks`
  - fan-out 재작성
  - UE 인용 줄 정정
  - render-lane 표 정정과 crypto 조율
- 개정 2(2026-11-17): 기록 모드(5.1절, L11)를 추가했다. 명령줄 도구는 Sync, 엔진 호스트는 Async가 기본이며, 설정 `log.mode`로 고른다. 이를 위해 LG9(ConfigSystem CF2 연결)를 추가했다. trace 세션 답신에 따라 LG1에서 `LogBuffer`·`ProfileScope` 카테고리 교체를 뺐다(TR2가 `ProfileScope`를 지움). `Clock.h`가 TR1과 겹치는 것도 15절에 적었다.
- 개정 3(2026-11-17): P7과 파일 sink·암호화(9.1절, L12·L13, LG10)를 추가했다. 기본은 암호화 끔이고, 켜면 KEM 봉인 파일 키와 SecretStream 묶음 봉인을 쓴다.
