# 로깅 시스템 설계

기준일: 2026-11-18, 개정 12. 엔진 전반에서 쓰는 **로그**(사람이 읽는 사건 기록)의 호출 API, 카테고리·레벨 필터, 비동기 기록 경로, 출력 형식, 장애 격리, 엔진 적용 범위를 정한다.

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
| `SLC` | `Private/Logging/StructuredLog.cpp` |
| `LT` | `Private/Logging/LogTrace.cpp` |
| `ODF` | `Private/Misc/OutputDeviceFile.cpp` |
| `FH` | `Private/Misc/FileHelper.cpp` |

UE의 매크로 중심 API(`UE_LOG`, `DECLARE_LOG_CATEGORY_*`)는 따르지 않는다. 일반 로그는 함수 호출로 쓴다. 메시지 템플릿과 출력 레이아웃은 Serilog·log4j의 개념을 Rocks 이름으로 가져온다. UE 이름은 대응표(4절)에만 둔다.

상태: LG0–LG15·LG17–LG19 **구현 완료**(결정 L1–L27, 구현과 다르게 정한 점은 14.2절). LG16(`{@Obj}`)은 GC 세션의 MetaCompiler·ObjectRuntime 변경 병합 뒤로 미뤘다. 단계는 14절, 다른 세션과의 순서는 15절, 결정은 17절이다.

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
- **설정 파일:** 엔진 호스트는 ConfigSystem 콘솔 설정 `log.mode`(`"default"` · `"async"` · `"sync"`, 플래그 **시작 뒤 고정**)를 읽는다. 기본값 `default`는 호스트 프리셋을 그대로 쓰고, `async`·`sync`만 프리셋을 덮어쓴다(기본값을 `async`로 두면 도구 프리셋까지 덮기 때문이다).
  - 등록과 읽기는 `LogHost` 모듈(Core·Config·Crypto 위)이 한다(LG9). 설정 이름 전체는 9.2절.
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
- `LogBuffer`는 단일 스레드 초기 진단으로 남는다(TraceSystem T8). `ProfileScope`는 TraceCapture 병합(`2ff7fbc`)으로 삭제되었고 `CpuScope`가 대신한다.

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
- 구조체 펼치기(Serilog `{@Obj}`)는 LG16이다(14.1.6절). 사용자 `Formatter`의 spec 컴파일 타임 검증은 LG11이다(14.1.1절).

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
  - `-fmacro-prefix-map=<저장소 루트>/=`를 쓴다(clang-cl은 `/clang:-fmacro-prefix-map=<루트>\=`). `-ffile-prefix-map`은 디버그 정보 경로까지 바꿔 디버거의 소스 찾기를 깨므로 쓰지 않는다. clang-cl은 `/clang:` 없이 넘기면 "unknown argument ignored"로 무시한다(`ActionGraph.cs:408`의 `/clang:-std=c++23`과 같은 방식).
  - 매핑은 `__FILE__`, `__builtin_FILE()`, 헤더 본문 모두에 적용된다(clang-cl 21.1.6 확인). Windows 결과의 구분자는 `\`다. 설치 SDK 게임 빌드는 SDK 디렉터리에도 같은 매핑을 건다. 빌드 ID는 이 인자의 루트를 `src:`로 정규화해 호스트에 무관하다(LG5, `4c94a2d`).
  - Release exe에 남는 절대 경로는 링커가 쓰는 PDB 경로뿐이다(범위 밖).
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

- 슬롯(초기 512 B): 머리(레벨·카테고리 이름 포인터와 길이·위치·EventId·단조 시각·스레드·프레임·플래그·값 수) + 값 영역.
  - 카테고리는 `LogCategory` 객체 포인터가 아니라 **이름**을 담는다. 이름은 consteval 생성자가 받은 문자열 리터럴이라 정적 저장소이므로, 함수 지역 `constexpr LogCategory`로 기록해도 렌더 때 유효하다(LG2에서 객체 포인터로 인한 수명 결함을 확인함, LG3에서 바꿈).
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

- `FileLogSink`(Core): 실행마다 새 파일(`<로그 폴더>/<호스트>-<UTC 시작 시각>.log`), 크기 상한을 넘으면 회전(`log.file.max_bytes`), 보관 개수 상한(`log.file.retention`). 보관 정리는 `<호스트>-<YYYYMMDDTHHMMSSmmmZ>[-<숫자>].log` 모양에 정확히 맞는 파일만 센다(같은 폴더의 다른 호스트 파일을 지우지 않는다). 쓰기는 PlatformBase 파일 API를 쓴다. flush 묶음 단위로 쓰고, 버퍼는 Start에서 확보한다(무할당).
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

- **보안 목표:** 완벽한 보안이 아니다. 사용자가 파일을 열어 읽을 수 없으면 된다. 풀려도 된다(사용자 확인). 그래서 그 이상의 강화는 하지 않는다: 키 회전, 보낸 쪽 서명, 안전 삭제, 메타데이터(파일 크기·시각) 은닉.
- 구현 `crypto::LogFileSealer`는 Crypto 모듈에 둔다(Crypto는 Core 위). 자체 암호 구현은 없다.
  - **파일 키:** 파일마다 Crypto KEM(X-Wing, `Kem.h`)으로 개발자 **공개키**에 캡슐화한 공유 비밀에서 KDF 타입 파생으로 `SecretStreamKey`를 뽑는다. 파일 머리에는 KEM 암호문과 secretstream header를 둔다.
  - 게임 바이너리에는 공개키만 들어간다. 그래서 바이너리를 분석해도 기존 파일을 열 수 없다. 여는 쪽은 개인키를 가진 개발자 도구뿐이다.
  - "사용자가 알아볼 수 없는 수준"이라는 요구보다 강하다. 그래도 고른 이유는 비용이 같기 때문이다. 파일당 KEM 한 번이고, 기록당 비용은 대칭 봉인뿐이다. 고정 키를 바이너리에 숨기는 방식(L13 대안)보다 키 관리가 단순하다(L13 결정).
  - **봉인 단위:** flush 묶음 하나가 secretstream 메시지 하나다(tag `Message`, 정상 종료 때 `Final`). 묶음당 17 byte가 늘어난다. 비정상 종료로 `Final`이 없는 파일도 마지막으로 쓴 묶음까지 복호된다. 복호 도구는 "끝 표시 없음(비정상 종료)"을 알린다.
  - 봉인은 출력 스레드에서 한다(Async). 메인 틱 비용은 0이다. Sync 모드(도구)에서는 호출 스레드가 낸다. 출력 버퍼는 Start에서 확보한다. 키 상태는 Crypto 키 저장소 슬롯에 있고 파일을 열 때 한 번 만든다.
  - 암호화를 켰는데 키 준비(KEM·키 저장소)에 실패하면 **평문으로 내려가지 않는다.** 파일 sink를 끄고 비상 경로로 한 번 알린다(P3: 엔진은 계속 실행, 민감 로그는 평문으로 남지 않음).
- **공개키 공급:** 엔진 호스트 설정 `log.file.recipient_key`(공개키, base64)를 쓴다. `log keygen`이 이 설정 줄을 그대로 출력한다. 프로젝트 설정 파일에 두며 비밀이 아니다. 키 쌍 생성과 개인키 보관은 개발자 도구(Commandlet `log keygen`)가 한다. 개인키는 저장소·쿠킹 결과에 넣지 않는다.
- **복호 도구:** Commandlet `log decode <file> --key <개인키 파일>`가 평문 텍스트나 JSON lines로 출력한다.
- 콘솔(stderr) sink는 암호화하지 않는다. 출시 빌드에서 콘솔 출력이 문제라면 콘솔 sink 자체를 끈다(호스트 설정).

```
[2026-11-17T02:23:45.123Z][Info][Engine] Engine initialized with 3 subsystems
[2026-11-17T02:23:47.456Z][Warning][World] World Main destroy deferred for 300 frames: external references World=Main (Engine/Private/Engine.cpp:262 ProcessDeferred)
```

### 9.2 설정(`LogHost`)

`LogHost` 모듈(`Core/Source/Runtime/LogHost`, 의존 Core·Config·Crypto)이 아래 콘솔 설정을 등록하고(`RegisterLogSettings`, 전역 생성자 없음), 그 값으로 LogSystem과 stderr·파일 sink를 만든다. Core는 Config·Crypto에 의존하지 않는다. 이름은 Config 이름 규칙(소문자 점 경로)을 따른다.

| 이름 | 종류 | 기본값 | 허용 값·범위 | 변경 |
|---|---|---|---|---|
| `log.mode` | string | `default` | `default`(호스트 프리셋)·`async`·`sync` | 시작 뒤 고정 |
| `log.level` | string | `info` | `trace`·`debug`·`info`·`warning`·`error`·`fatal`(대소문자 무시, 그 밖은 설정 단계에서 거부) | 실행 중, sync 지점 |
| `log.levels` | string | `""` | `"Graphics=warning,Engine.World=debug"`(접두사·레벨 대소문자 무시) | 실행 중, sync 지점 |
| `log.file.enabled` | bool | `false` | | 시작 뒤 고정 |
| `log.file.directory` | string | `Saved/Logs` | | 시작 뒤 고정 |
| `log.file.format` | string | `text` | `text`·`jsonlines` | 시작 뒤 고정 |
| `log.file.max_bytes` | int64 | `16777216` | 65536–1099511627776 | 시작 뒤 고정 |
| `log.file.retention` | int32 | `8` | 1–256 | 시작 뒤 고정 |
| `log.file.encrypt` | bool | `false` | | 시작 뒤 고정 |
| `log.file.recipient_key` | string | `""` | X-Wing 공개키 base64(비밀 아님, 개인키 금지) | 시작 뒤 고정 |
| `log.console.layout` | string | `""`(기본 콘솔 레이아웃) | 레이아웃 패턴(9절) | 시작 뒤 고정 |
| `log.file.layout` | string | `""`(기본 콘솔 레이아웃) | 레이아웃 패턴. `log.file.format = jsonlines`이면 쓰이지 않는다 | 시작 뒤 고정 |
| `log.rate_limits` | string | `""` | `"Graphics=20,Engine.World.Tick=100"`(접두사별 초당 기록 수 1–1000000, 접두사 대소문자 무시, 최대 16항목) | 실행 중, sync 지점 |
| `log.file.max_age_minutes` | int32 | `0`(끔) | 0–10080 | 시작 뒤 고정 |
| `log.file.compress` | bool | `false` | | 시작 뒤 고정 |

- **카테고리별 재정의**는 문자열 하나(`log.levels`, `접두사=레벨` 목록)다. Map 설정은 리플렉션 생성 struct가 필요해 쓰지 않았다. 잘못된 항목은 Warning을 남기고 건너뛴다.
- **실행 중 변경:** `LogHost::Watch`가 `log.level`·`log.levels`를 구독하고 Config sync 지점에서 다시 적용한다. 다시 적용할 때 앞의 재정의를 지운다. 시작 뒤 고정 항목은 Config `StartupOnly` 플래그와 호스트의 `CompleteStartup`으로 거부된다.
- **실패:** 암호화를 켰는데 키가 없거나 틀리면 파일 sink를 만들지 않고 Error를 한 번 남긴다(평문으로 기록하지 않음, L12). 파일 폴더를 열지 못하면 Error를 남기고 파일 없이 계속한다. `LogHost::Start`는 설치 전에 돌기 때문에 이 Error는 설치 전 비상 경로(stderr)로 나간다.
- **호스트 순서(Sandbox `--engine --config <Engine.json>`, ConfigSystem 5.3·5.4절):** `HostSettings`(HostConfig 모듈)가 문맥을 만든다.
  - 설정 단계: 등록(`RegisterLogSettings` → `trace.*` → 호스트) → 층 파일 → `--config` → `--set` → Sync → `CompleteStartup`(`log.mode`·`log.file.*` 고정).
  - 실행 단계: `LogHost::Start(preset, &context, hostName)` → 설치 → `Watch` → trace 세션 → 엔진 루프(프레임마다 Config Sync가 `log.level`·`log.levels` 변경을 적용).
  - 종료 단계: 엔진 정리 → `LogSystem::Flush` → trace sink(`TraceLogSink`) 제거 → trace 정지 → 설치 해제 → `LogHost::Stop`(`WriterAbandoned()`면 정리 없이 종료). trace sink는 trace 시작 직후 더한다(14.1.5).
  - 크래시 수집: 처리기 설치·제거는 Launch가 `platform::InstallCrashReporting`·`RemoveCrashReporting`으로 한다([크래시 처리·실행 진입점](CrashHandlingAndLaunch.ko.md)). 로깅은 첫 설치 때 종료 콜백 하나만 등록한다(14.1.8). 호스트가 따로 할 일은 없다.
  - 설치 전 설정 오류는 비상 경로(stderr)로 한 번 남는다. `HostSettings`는 LogHost보다 오래 산다.

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
| 종료 drain 시간 초과 | 포기 표시로 출력 스레드를 다음 기록에서 멈추게 하고, 남은 수를 비상 경로로 알린 뒤 진행한다. 그래도 sink에서 돌아오지 않으면 스레드와 메모리를 남겨 둔 채 Stop이 반환한다(`Stats().writerAbandoned`, 이후 Start는 Busy). 소멸자는 그 스레드를 Join하므로, 이 경우 호스트는 LogSystem을 파괴하지 않고 프로세스를 끝낸다(PlatformBase Thread에 detach가 없음) |
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
| 초기화 실패 | Error | `Engine initialization failed at {Stage}: {Error}` — Stage = TypeRegistry·ModuleRegistration·RegisterTypes·ObjectRegistry·Subsystems·PermanentRegion(GC 세션 영구 창 닫기, 병합 뒤)·ClientStart | Engine(호스트는 재기록하지 않음) |
| Teardown 정리 실패 | Fatal | `Engine {Registry} could not shut down after initialization failure at {Stage}: {Error}` | 기존 `platform::Fatal` 대체 |
| Run 종료 | Info | `Engine run finished after {Frames} frames: {Reason}` | `Engine::Run` |
| World Tick 실패 | Error | World별 첫 실패만 `World {World} tick failed: {Error}`, 회복·파괴 때 "N more times (last: X)" 요약 1회 | `Engine::RunFrame` |
| World 파괴 지연 | Debug → Warning → Info | 첫 지연 Debug, `WorldDestroyWarnFrames`(300) 넘으면 Warning 1회, 해소되면 `World {World} destroyed after {Frames} deferred frames` | `ProcessDeferred`, WorldContext에 프레임 수·경고 여부 |
| 종료 | Info / Error | 실패 시 `{Worlds}`, `{PendingDestroys}`, `{LiveObjects}`, `{Error}` | `Engine::Shutdown` |
| CreateWorld 내부 실패 | Error | `World {World} failed during {Stage}: {Error}` | Engine |
| Tick 순환 거부 | Warning | `Rejected tick prerequisite {Before} -> {After}: would create a cycle`. TickScheduler는 `ErrorCode::CycleDetected`로 구별하고, 새 `World::AddActorTickPrerequisite`가 기록한다. Actor에 이름이 없어 `타입#slot`으로 표기 | 이름을 가진 World·Actor 경계. TickScheduler 오류 코드 분리 |
| 장치 생성 성공 | Info | `Created {Backend} device on {Adapter} (validation {Validation})` | 생성 함수 |
| 장치 생성 실패 | Warning / Error | fallback이면 Warning, 모든 후보 실패면 Error. HRESULT·단계 포함 | 호출자(선택자), `DeviceCreateReport` |
| Present 실패·장치 제거 | Error | `Device removed during present: {Reason} (HRESULT {HResult:#x})`, lost 전이 때 1회 | 장치 내부 |
| 셰이더 컴파일 실패 | Error | 장치가 LastError에 `Shader {entry} ({target}) failed to compile (HRESULT ..): {진단 최대 1 KiB}`를 남기고, 파이프라인 생성을 결정하는 쪽(지금은 Sandbox `SampleRenderer`)이 `... pipeline could not be built on {Backend}: {Error}: {Diagnostics}`로 기록한다. entry·target을 구조화해 내보내려면 Device API 변경이 필요해 보류 | 결정하는 호출자 |
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
| LG2 | 공용 `constexpr` 서식 파서·이름 자리, `FixedStringBuilder`(잘림 모드), `LogTemplate` 검증, 위치 수집 시험, EventId, 값 캡처 인코딩과 렌더(왕복), 컴파일 실패 시험 체계(Rocks.ToolTests가 clang-cl `-fsyntax-only`) | LG1 | 템플릿·위치(TU 2개)·캡처 왕복·잘림·컴파일 실패 시험 |
| LG3 | `Log::*` 진입 함수와 Lazy, `BoundedMpscQueue`, 블록 풀, 준비 버퍼, `LogSystem`(Async·Sync·Manual 모드, 프리셋, 출력 스레드·깨우기·Pump·Flush·포화·sink 명령·통계), fan-out 재작성, 세대 카운터 `LogInstallScope`, `platform::WriteDiagnostic`·`TryUtcTicks`, 설치 전후 경로, 장애 격리, `Log::Fatal`·`AssertFailed` | LG2 | 큐 스트레스·포화·wrap·멈춘 생산자 결정적 시험, **무할당 시험**(호출 수 세는 할당기로 Start 뒤 0회), Lazy 0회/1회, 깨우기 유실 없음, 해제 기아 없음, Sync와 Manual의 출력 동일·Sync 무할당, 장애 주입, TSan·Asan, 호출 비용 측정 보고 |
| LG4 | 레이아웃 해석기, Text·Stderr sink 개편(묶음 flush), `JsonWriter` 옵션(한 줄·고정 frame·중복 색인 끔)·`JsonLinesLogSink`, `DateTime` 고정 폭 밀리초, `LogContextScope` | LG3 | 토큰별 결정적 출력, 잘못된 패턴 실패, JSON 왕복, 무할당 |
| LG5 | `ROCKS_LOG_COMPILED_MINIMUM`(타깃 `LogCompiledMinimum` 재정의, 모듈 재정의 거부), toolchain 단 `-fmacro-prefix-map`, Release 문자열 제외 검사. 한글 템플릿은 `source-literal-english` 정책이 막으므로 음성 대조만 추가 | LG2 | 구성별 빌드, 최종 링크 결과 검사 |
| LG6 | Engine·World 적용(13절) | LG4, **15절 조건** | 단계별 실패 주입 → Stage, 파괴 지연 1000프레임 → Warning 정확히 1회·해소 Info 1회 |
| LG7 | Graphics·창·셰이더 도구(report, HRESULT, 장치 제거 사유, 검증 메시지, 셰이더 진단) | LG4, **15절 조건** | 실패 주입 시험, Sandbox 자체 시험 |
| LG8 | 호스트: Sandbox 헤드리스 `--engine`(Async, L1), Commandlet 등 명령줄 도구(Sync). 사용 가이드 `Docs/Implementation/Logging.ko.md`, FoundationStatus·UE-C22·D01 갱신 | LG6, LG7 | Sandbox 실행으로 시작·종료·실패 원인 추적 확인 |
| LG9 | 설정 `log.mode`·`log.level`·`log.levels`·`log.file.*` 등록·읽기(`LogHost` 모듈, 9.2절) | LG8, LG10, ConfigSystem CF2 | 설정 파일 값으로 모드·레벨·파일 기록이 바뀌고, 레벨은 sync 지점에서만 바뀌며, 시작 뒤 고정 항목의 실행 중 변경은 거부 |
| LG10 | `FileLogSink`(실행별 파일·크기 회전·보관 상한), `LogFileCipher` 인터페이스(Core), `crypto::LogFileSealer`(Crypto: KEM 캡슐화 + SecretStream 묶음 봉인), Commandlet `log keygen`·`log decode` | LG4 | 평문·암호 파일 왕복, `Final` 없는 파일 부분 복호·경고, 변조 검출, 키 준비 실패 시 평문으로 내려가지 않음, 무할당(파일 열기 뒤) |
| LG11 | 사용자 `Formatter<T>` spec 컴파일 타임 검증 규약(14.1.1, L20) | LG2 | 잘못된 spec 컴파일 실패 시험, 검사 함수 없는 Formatter 통과, 런타임 템플릿 같은 판정, 무할당 시험 불변 |
| LG12 | `log.console.layout`·`log.file.layout`, `LogLayout::Parse` 오류 위치, 적용 시점 검사(설정 단계 거부는 CVar CVR7, L18), 가이드 갱신(14.1.2, L17·L18) | LG9 | 잘못된 레이아웃은 Error 1회 + 기본 레이아웃, 레이아웃 설정별 결정적 출력, 시작 뒤 변경 거부, 무할당 |
| LG13 | 카테고리별 rate limit `log.rate_limits`, 캡처 시각을 tick으로·시계 주입(14.1.3, L21) | LG12 | 주입 시계로 창별 통과 수가 정확히 상한(단일·다중 생산자), Error·Fatal 제외, 알림 1회, 16칸 초과·잘못된 항목은 적용 시점 Warning 후 건너뜀, 무할당 |
| LG14 | 상관 id: Core 스택(`CorrelationScope`, 확장 칸, 관찰자 hook), 기록 캡처·`{Correlation}`·JSON `corr`, task·전용 lane 상시 전파(14.1.4, L14·L15) | LG4, **15.1절 trace 조율** | 중첩·넘침·스레드 격리, 범위가 끝난 뒤 렌더해도 같은 값(Manual), 공개 `CorrelationScope`로 쌓은 값이 로그·trace에 같게 실림, trace 세션 없이 task 전파, trace 기존 시험 통과, 무할당, 생산자 비용 측정 |
| LG15 | trace `log` 채널: `TraceLogSink`, `Trace.LogMessageSpec`·`Trace.LogMessage`, TraceAnalysis·`trace-report`, 호스트 연결(14.1.5, L16) | LG13, LG14, **15.1절 trace 조율** | 기록 → 분석 왕복(spec 1회, 순서·스레드·tick·상관 id 일치), 채널 꺼짐 기록 0, 중복 표 넘침·세션 교체, 무할당(초기화 뒤), Sandbox trace 산출물 |
| LG16 | 구조체 펼치기 `{@Obj}`: `LogStructure<T>` 확장점, 캡처 시점 복사, ObjectRuntime 일반 특수화·생성 필드 방문 함수(14.1.6, L19) | LG11, **15.1절 리플렉션 조율** | 캡처 뒤 원본 변경·파괴에도 출력 불변, 상한 잘림, 텍스트·JSON 결정적 출력, 생성 struct 왕복, 특수화 없는 타입 컴파일 실패, 무할당 |
| LG17 | 시간 기준 회전 `log.file.max_age_minutes`, 압축 `log.file.compress`(묶음별 zstd frame), 봉인과의 순서(14.1.7, L22·L23) | LG10, **15.1절 Compression 조율(고정 작업 공간 문맥)** | `LogEvent`를 sink에 직접 넣어 회전 결정적 시험, 압축 왕복·잘린 끝 frame 복원, 봉인+압축 왕복·변조 검출, 보관 정리, 무할당(Open 뒤) |
| LG18 | 종료 경로의 마지막 로그: `LogSystem::FlushForTermination`, 크래시 수집기 종료 콜백 등록·Fatal 표시(14.1.8, L24·L25). 처리기는 수집기 소유(개정 12) | LG3, **15.1절 크래시 수집 조율** | 자식 프로세스 시험으로 `platform::Fatal`·접근 위반·stack overflow·SIGSEGV 직전 Async 기록 보존, 정확한 종료 코드, 멈춘 sink에서도 종료, 대화상자 억제 |
| LG19 | Commandlet `log view`(거르기·여러 파일·평문·zst·봉인)(14.1.9, L26) | LG14, LG17 | 고정 입력 파일의 필터 결과 결정적 비교(Rocks.Commandlet.Tests) |

검증: 커밋마다 Debug 빌드·format·변경 부분 시험. 처음에는 전체 체인(Asan·TSan)을 LG3과 LG8 끝에 한 번씩 돌리기로 했으나, 2026-11-17 저장소 검증 기준이 바뀌어(Asan은 필요할 때만) 돌리지 않았다. 메인 트리 구현은 에이전트 하나가 순서대로 하고 커밋은 경로를 지정한다.

LG11–LG19의 검증도 같다: Debug 빌드, format, policy, 바뀐 부분 시험. 무할당 시험은 호출 수를 세는 할당기로 Start(또는 sink 초기화) 뒤 0회를 단언한다. 시간에 기대는 sleep 시험은 쓰지 않고 시각은 주입한다.

### 14.1 후속 단계 설계(개정 9, LG11–LG19)

개정 8의 16절 후속 항목을 단계로 설계했다. 순서는 의존 관계다.
- LG11·LG12·LG13은 Core·LogHost만으로 끝난다(Config는 고치지 않는다, L18).
- LG14·LG15는 trace 세션, LG16은 리플렉션 세션, LG17은 Compression 소유 세션, LG18은 PlatformBase와 맞춘다(15.1절).
- WorkerScheduler 진단은 선행 조건이 갖춰지지 않아 단계로 두지 않았다(16절).
- **LG18(종료 경로 flush, L24·L25)과 LG19(`log view`, L26)는 개정 8의 16절 밖, 새 범위다**(2026-11-18 사용자 진행 지시·L25 선택으로 확정). 저장소 상태 문서의 "crash 수집·viewer" 가운데 로깅 범위로 할 만한 것만 골랐다.

새 설정은 9.2절 표에 있다(LG12·LG13·LG17).

#### 14.1.1 LG11 사용자 `Formatter<T>` spec 컴파일 타임 검증

지금 `LogTemplate`의 consteval 생성자는 내장 타입 spec만 검사한다(`AnalyzeLogTemplate`, `IsValidBuiltinFormatSpec`). 사용자 Formatter 자리는 같은 자리에 같은 spec을 쓰는지만 보고(`ConflictingFormatterSpec`), spec 자체는 캡처 때 `Formatter<T>::Format`이 판단한다.

- **규약:** `Formatter<T>`는 선택 멤버 `static constexpr bool IsValidSpec(StringView spec) noexcept`를 가질 수 있다. false면 새 오류 `LogTemplateError::InvalidFormatterSpec`이고, 기존 방식대로 표지 함수 `detail::LogTemplateError_InvalidFormatterSpec()`가 컴파일러 진단에 이름을 남긴다.
- **리터럴 템플릿:** consteval 생성자는 인자 타입을 알므로 Custom 자리마다 `Formatter<T>::IsValidSpec`을 직접 부른다(컴파일 타임).
- **런타임 템플릿(`RuntimeTemplate`):** 타입 정보가 없으므로 검사 함수를 넘겨야 한다. 공용 `FormatArg`(일반 서식과 함께 쓰는 형식)는 늘리지 않는다. 로그 캡처 전용으로 인자별 함수 포인터 배열(`bool (*)(StringView) noexcept`, 없으면 nullptr)을 `LogArgKindOf`와 같은 자리에서 만들어 `AnalyzeLogTemplate`의 런타임 호출에 넘긴다. 정적 배열이라 할당이 없다.
- **없으면** 지금처럼 캡처 때 판단한다(부분 검증, `<format error>`). 감지는 concept `detail::HasFormatterSpecCheck<T>`다.
- consteval이 아니라 constexpr인 이유: 같은 함수를 런타임 템플릿 검증에도 써야 한다. Formatter는 공용 `ParseFormatSpec`(constexpr)을 재사용할 수 있다.
- 저장소 안에서 spec을 받는 Formatter는 이 단계에서 `IsValidSpec`을 갖춘다. `StringBuilder::TryAppendFormat`(일반 서식)의 동작은 바꾸지 않는다.
- 완료 기준: 잘못된 spec이 컴파일 실패 시험(Rocks.ToolTests, clang-cl `-fsyntax-only`)에서 `InvalidFormatterSpec` 이름으로 거부, `IsValidSpec` 없는 Formatter는 그대로 통과, 같은 템플릿을 `RuntimeTemplate`으로 쓰면 같은 판정, 생산자 무할당 시험 불변.

#### 14.1.2 LG12 설정 검증 hook과 레이아웃 설정

- **설정 검증의 자리(L18, 개정 10):** Config에 validate hook을 더하지 않는다. CVar 설계(CVarSystem.ko.md 개정 2, VD10)가 Config 5절 콘솔 설정을 대체하고, 핸들의 `Validate`(`StringView (*)(const T&) noexcept`)로 설정 단계 거부를 제공하며, CVR7에서 `log.*`를 그 핸들로 옮긴다. 지금 hook을 더하면 CVR7에서 버려진다. 그래서 이 단계는 LogHost가 **적용 시점**에 검사하고, 설정 단계 거부는 CVR7 이관 때 `Validate`로 옮긴다(15.1절 CVar 항목).
- **거부 범위(Config 동작 그대로):** 검증에 실패한 항목이 하나라도 있으면 **그 층 입력 전체**가 거부된다(`ConfigContext.cpp:731–738`의 `ValidateEntry` → `InvalidArgument`). 설정 파일 하나에 잘못된 값이 있으면 그 파일 층이 통째로 빠지고 호스트가 거부를 알린다(`HostSettings.cpp:207–213`). 콘솔 변경은 그 한 줄이 거부된다. 어느 쪽이든 앞 값이 유지된다.
- **레이아웃 설정:** `log.console.layout`·`log.file.layout`. `LogHost::Start`가 `LogLayout::Parse`를 부르고, 실패하면 Error를 한 번 남기고 기본 레이아웃으로 계속한다(P3). 이유 문장을 위해 `Parse`에 선택 출력 `LogLayoutParseError { usize offset; ErrorCode code; }`를 더한다(예: `unknown layout token near offset 14`). 빈 값은 기본 콘솔 레이아웃이다.
- **시작 뒤 고정:** sink는 생성 때 레이아웃을 해석해 값으로 가진다(9절). 실행 중에 바꾸려면 sink를 빼고 다시 넣어야 하고, 그 사이 기록의 모양이 섞인다. 얻는 것보다 복잡하다.
- `log.file.format = jsonlines`인데 `log.file.layout`이 비어 있지 않으면 `LogHost::Start`가 Warning을 한 번 남긴다(`log.file.layout is ignored because log.file.format is jsonlines`). 두 설정에 걸친 검사라 validate가 아니라 Start에서 한다.
- **`log.levels`:** 지금 동작(잘못된 항목·적용 실패 항목은 적용 시점 Warning 후 건너뜀)을 유지한다. CVR7에서 문법 오류(`접두사=레벨` 모양, 레벨 이름, 접두사 문법)는 `Validate`로 설정 단계 거부로 바뀌고, 적용 실패(재정의 표 용량 64 초과, 접두사 해시 충돌 `AlreadyExists`, sink 콜백 안 `Busy`)만 적용 시점 Warning으로 남는다. 그때 9.2절과 가이드를 이 구분으로 고친다.
- `log.rate_limits`(LG13)도 `log.levels`와 같다. 16항목 초과와 잘못된 항목은 지금은 적용 시점 Warning 후 건너뛰고, CVR7에서 설정 단계 거부로 옮긴다.
- UE: 줄 머리 모양은 고정이고(`[시각][프레임]Category: Verbosity: `, `Private/Misc/OutputDeviceHelper.cpp:12–75`) 시각 종류만 설정으로 고른다(`ELogTimes` `Public/Misc/OutputDevice.h:109–129`, `[LogFiles] LogTimes`·`-LogTimes`는 `Runtime/Launch/Private/LaunchEngineLoop.cpp:6262–6318`). Rocks는 패턴 전체를 설정으로 받는다(P4).
- 완료 기준: 잘못된 레이아웃은 Error 1회 + 기본 레이아웃(엔진 계속), 올바른 레이아웃이 콘솔·파일 출력 모양을 바꾼다(결정적 출력 비교), `Parse` 오류 위치 단위 시험, 시작 뒤 변경 거부, Start 뒤 무할당.

#### 14.1.3 LG13 카테고리별 rate limit

지금은 같은 EventId 반복 요약(12절)과 큐 포화 처리(8.5)만 있다. 특정 카테고리가 많이 쏟아질 때 상한을 정하는 수단이 없다.

- **설정:** `log.rate_limits = "Graphics=20,Engine.World.Tick=100"`. 접두사별 초당 기록 수이고, 실행 중 sync 지점에서 다시 적용한다(`log.levels`와 같은 경로, 16항목 초과·잘못된 항목은 Warning 후 건너뜀 — CVR7에서 설정 단계 거부).
- **표:** LogSystem 안 고정 표 `MaxLogRateLimits`(16칸). 칸마다 원자 변수로 접두사 해시, 상한, **창 상태 `Atomic<uint64>` 하나**(상위 32-bit 창 번호 = 초 단위 단조 시각, 하위 32-bit 창 안 통과 수), 버린 누계를 둔다. 조회는 레벨 재정의 표와 같은 최장 접두사 일치이고, 칸을 바꾸는 순서도 같다(7절). 표가 비면(원자 개수 0) 생산자는 보지 않는다.
- **생산자(8.1):** 레벨 검사를 통과한 뒤, 캡처 전에 본다. 단조 시각 읽기를 이 앞으로 당긴다(어차피 한 번 읽는다). 창 번호는 `ticks / 주파수`(초)의 하위 32-bit다(136년에 한 번 돈다).
  - 상태를 읽어 창 번호가 지금과 다르면 CAS로 `(지금 창, 1)`을 넣는다. 성공하면 통과, 실패하면 다시 읽는다.
  - 같은 창이고 수가 상한보다 작으면 CAS로 수를 1 올린다. 성공하면 통과, 실패하면 다시 읽는다.
  - 같은 창이고 수가 상한이면 버리고 그 칸의 버린 수를 올린다.
  - 창 번호와 수를 한 원자 값으로 바꾸므로 **창마다 통과 수가 정확히 상한 이하**다. lock·할당이 없다. CAS 재시도는 같은 칸을 동시에 쓰는 생산자 사이에서만 생긴다.
- **적용 레벨:** Trace–Warning. **Error·Fatal은 세지도 버리지도 않는다.** 원인 기록을 잃지 않기 위해서다(P1). Error 폭주는 EventId 반복 요약과 Warning 예약 칸(8.5)이 다룬다.
- **알림:** 출력 쪽이 묶음 끝·`Pump`(통과한 기록이 없어도)·`Stop`에서 칸별 버린 수의 증가분을 보고 `Rate limit dropped {Dropped} records for {Prefix} (limit {Limit}/s)`를 Info로 한 번 남긴다. 같은 칸은 1초에 한 번까지다. `LogSystemStats::rateLimited`는 모든 칸의 합계다.
- **창은 앞으로만 옮긴다:** 늦게 온 생산자의 과거 시각은 지금 창의 수로 판정한다(32-bit 순환 차이가 양수일 때만 새 창).
- **설정 다시 적용:** `log.rate_limits` 내용이 그대로면 아무것도 하지 않고, 바뀌면 빠진 접두사만 지우고 남은 접두사는 상한만 바꾼다(창·수 유지). 칸을 다시 쓰면 지난 버림 수는 알린 것으로 친다.
- **캡처 시각과 시험용 시계:** 이 단계에서 기록 머리의 시각을 `MonotonicNanoseconds` 대신 `MonotonicTicks`로 캡처한다(나눗셈이 없어 더 싸다). 출력 스레드가 `LogEvent`를 만들 때 ns로 바꾼다. 두 값은 같은 원천이고 `Clock.h`가 정한 관계(ticks / 주파수 × 10⁹, 소수 버림)를 그대로 쓰므로 출력은 지금과 같다. `LogEvent`에는 ns와 함께 원래 tick도 둔다(LG15). `LogSystemConfig::clock`(tick을 돌려주는 함수 포인터, 기본 `platform::MonotonicTicks`)과 `clockFrequency`를 더해 캡처 시각을 주입한다.
- UE에는 카테고리별 rate limit이 없다. `FLogSuppressionInterface`는 카테고리 verbosity 변경(`[Core.Log]`, `-LogCmds=`, `Log` 명령)만 한다(`Public/Logging/LogSuppressionInterface.h`). `UE_CALL_ONCE`(`Public/Misc/CoreMiscDefines.h:181`)는 로그 전용이 아닌 1회 호출이다.
- 완료 기준(Manual, 주입 시계): 상한 3/s에 10건 → 정확히 3건 통과·7건 버림, 다음 창에서 다시 3건, 여러 생산자 스트레스에서도 창마다 통과 수가 정확히 상한(상한보다 많이 넣을 때), Error는 영향 없음, 알림 정확히 1회, 실행 중 설정 변경이 sync 지점에서만 적용, 16칸 초과 설정 거부, 무할당.

#### 14.1.4 LG14 상관 id

현재: 상관 id는 TraceCapture에 있다.
- `TraceContextValues { session, task, asset, entity, request }`(각 `uint64`, 0은 없음, `TraceCapture/TraceContext.h`)와 스레드별 고정 깊이 스택(`MaxContextDepth` 16, 세션 없이도 동작)이다. API는 `TraceContextScope`·`TraceCaptureContext`·`TraceCurrentContext`다.
- 저장 사본 `TraceContextToken`은 값과 함께 trace 정의 캐시(`cache` = 세션 serial 하위 32-bit << 32 | 문맥 id)를 가진다. 스택 칸은 그 칸의 쌓기를 기록했는지도 기억한다(내림 기록 짝).
- TraceSystem 6.4절은 "로그와 trace가 같은 상관 id를 쓴다"고 하고, trace와 별개로 로그 상관 id를 늘 전파할지는 로깅 연결 때 정한다고 남겼다(TR3 주석, TraceSystem.ko.md 7절).
- Core는 TraceCapture 아래라 이 스택을 읽을 수 없다(5절).

**스택을 Core로 내린다(L14).** `Core/Diagnostics/Correlation.h`:
- `struct CorrelationIds { uint64 session, task, asset, entity, request; }`(0은 없음, 40 B)
- 스택 칸은 `CorrelationIds ids`와 **상위 계층용 확장 칸 `uint64 ext[2]`**다. Core는 확장 칸의 뜻을 모르고, 쌓을 때 0으로 두고 사본·복원 때 그대로 옮기기만 한다.
- `CorrelationScope`(RAII 쌓기·내리기, `thread_local` 고정 깊이 16, 넘침은 세기만), `CorrelationToken CaptureCorrelation()`(맨 위 칸 사본 56 B + valid), `CorrelationScope(const CorrelationToken&)`(복원), `bool CurrentCorrelation(CorrelationIds&)`.
- **쌓기·내리기 관찰자 hook:** 원자 포인터 하나 `SetCorrelationObserver(const CorrelationObserver*)`. 관찰자는 `OnPush(CorrelationEntry&, bool restored)`·`OnPop(const CorrelationEntry&)` 함수 포인터 둘이다. 쌓기·내리기 때만 acquire 읽기 한 번과 (있으면) 간접 호출을 한다. **로그 기록마다 드는 비용은 0이다**(생산자는 스택만 읽는다).

**trace는 이 둘로 지금 동작을 그대로 지킨다.**
- TraceCapture가 관찰자를 건다(표준 형식 등록 때, 프로세스 수명).
  - `OnPush`: 채널 `context`가 켜져 있고 세션이 있으면 지금 `TraceContextScope`가 하던 일을 한다. 정의가 없으면 `Trace.ContextDefinition`을 쓰고 `ext[0]`에 cache를 둔다. 복원이면 cache가 같은 세션인지 보고 id만 쓴다. 그다음 `Trace.ContextPush`를 쓰고 `ext[1]`에 "쌓기 기록함"을 둔다.
  - `OnPop`: `ext[1]`이 서 있을 때만 `Trace.ContextPop`을 쓴다.
- `TraceContextValues`는 `CorrelationIds`의 별칭, `TraceContextToken`은 `CorrelationToken`(값 + `ext[0]` cache + valid)이 된다. `TraceContextScope`·`TraceCaptureContext`·`TraceCurrentContext`는 같은 이름·의미의 얇은 래퍼로 남는다. TraceCapture의 스레드 슬롯에 있던 `contexts[]`·`contextDepth`는 지운다.
- **`TraceTaskInfo`는 trace cache를 계속 가진다.** `context` 필드가 `CorrelationToken`(확장 칸 포함)이라 task 복원 때 정의를 다시 쓰지 않는 지금 최적화가 유지된다.
- 공개 `CorrelationScope`로 쌓아도 관찰자가 같은 기록을 내므로 로그와 trace의 상관 id가 같다.
- **"trace 기록 형식 불변, 기존 시험 통과"의 근거:**
  - 기록을 내는 코드(정의·쌓기·내리기, cache 판정)가 관찰자 안으로 자리만 옮긴다. 기록 필드·순서·조건(채널 `context`, 세션 유무)은 같다.
  - 깊이 상한(16)과 넘침 처리(쌓지 않고 세기만, 짝 내림도 무동작)는 Core 스택이 같은 규칙으로 한다. trace 통계의 넘침 수는 Core 카운터를 읽는다.
  - 공개 래퍼의 시그니처·의미가 같으므로 TR2·TR3 시험(`TraceStandardTests.cpp`의 문맥·task 시험)은 고치지 않고 통과해야 한다. 이것을 LG14 완료 기준으로 둔다.
- **다른 안과 비교**(trace가 Core에 "현재 상관 id 읽기" 함수 포인터를 설치하고 스택은 TraceCapture에 둔다):
  - 활성 로그 기록마다 간접 호출이 하나 생긴다. 이 설계는 TLS 읽기 하나다.
  - 로그의 상관 id가 TraceCapture의 설치 시점에 묶인다(설치 전 기록에는 없다). Commandlet도 TraceAnalysis를 통해 TraceCapture를 링크하므로 "trace 없는 도구"가 문제는 아니다. 그래도 Core의 로그가 상위 모듈의 설치에 기대는 의존이 남는다.
  - 상관 id는 로그와 trace가 함께 쓰는 개념이라(계획 29.4절) 둘 아래인 Core가 소유하고, trace는 확장 칸과 관찰자로 자기 상태만 덧붙인다.
- **캡처:** 생산자 5단계(8.1)에서 스택이 비어 있지 않으면 맨 위 `ids` 40 B를 기록 머리 뒤에 복사하고 플래그 `HasCorrelation`을 세운다. 비어 있으면 0 B다. TLS 읽기 한 번, 할당 없음. Sync도 같다.
- **출력:** `LogEvent::correlation`(+ 있음 표시). 레이아웃 토큰 `{Correlation}` → `session=3 task=1842`(0인 칸은 빼고, 모두 0이면 토큰을 생략). JSON lines는 `corr` 객체(`{"session":3,"task":1842}`). 기본 콘솔 레이아웃은 바꾸지 않는다.

**전파(L15).**
- 지금 TR3 훅(`TraceCapture/Private/TraceTasks.cpp`)은 task 채널이 켜졌을 때만 문맥을 저장·복원한다. 상관 id 전파는 trace 채널과 무관하게 **항상** 한다.
  - task를 만들 때 token을 복사하고 실행할 때 scope로 복원한다. trace 기록(`Task.*`, 문맥 쌓기 기록)만 채널을 따른다.
  - **스택이 비어 있으면 분기 하나**다(복사·복원 없음). TraceSystem.ko.md 7절의 "채널 꺼짐 비용 분기 하나" 요구는 상관 id를 쓰지 않는 실행에서 그대로 지켜진다.
  - 상관 id가 있으면 task당 56 B 복사와 TLS 쌓기·내리기가 든다. LG14에서 측정해 보고한다.
  - 빈 경로 구현(재리뷰 N1): `CorrelationScope(const CorrelationToken&)`의 생성·소멸은 헤더 인라인이고 빈 사본이면 분기 하나다(느린 경로는 `detail::PushCorrelation`·`PopCorrelation`). `TraceTaskScope`는 기록 여부와 문맥 유무를 한 판정으로 묶어 생성·소멸이 분기 하나씩이다.
  - **측정**(LogBench Release, Windows clang-cl, 개발 기기, 묶음 평균, 시계 해상도 100 ns라 한 건 분위수는 0 또는 100 ns로 양자화된다. 보장값이 아니다): 빈 전파(깊이 검사) 0.0 ns, 빈 `TraceTaskScope`(`TraceCreateTask` + 생성·소멸, 채널 꺼짐) 1.3–1.6 ns, 사본 + 복원·내림 1.1–1.4 ns. 활성 Info 기록은 상관 id 없음 125.3 ns·있음 125.1 ns(Async 생산자, 차이는 잡음 안), Sync 831.7·809.1 ns. 실제 `WorkerScheduler` Submit → 실행 → Wait 왕복(worker 1, 20000건)은 p50 기준 빈 스택 1.8–1.9 µs, 상관 id 2.1 µs다. 왕복은 깨우기를 포함해 흔들림이 커서(평균 2.1–4.3 µs) 전파 비용(수 ns)은 그 잡음보다 훨씬 작다.
- **`DedicatedLane::Post`도 전파한다.** 렌더·IO 같은 전용 lane에 넘긴 일도 게임 스레드의 요청·asset id를 이어받아야 원인을 따라갈 수 있다(P1). 지금 `Task/Private/DedicatedLane.cpp`에는 trace 문맥 처리가 없다. 같은 규칙(비면 분기 하나)으로 넣는다.
- `LogContextScope`(이름=문자열 문맥, 6.5절)와는 별개다. 상관 id는 trace와 공유하는 숫자 5칸 고정이고, 문맥은 사람이 읽는 속성이다.
- **L14–L16은 trace 세션 합의가 필요하다**(15.1절).
- UE: `UE_LOG_CONTEXT(Name, Value)`는 범위에 들어갈 때 값을 compact binary로 직렬화해 thread-local 목록에 두고(`SL:143`, `FLogContext` `SL:523–535`), 기록 때 필드로 복사한다. 로그와 trace가 공유하는 상관 id는 없다(`Logging.LogMessage`에 문맥·id가 없다, `LT:32–36`).
- 완료 기준: Core 시험(중첩·넘침·스레드 격리·확장 칸 사본·관찰자 호출 순서), 로그 기록에 맨 위 값이 실리고 범위가 끝난 뒤 렌더해도 같다(Manual, 결정적), 공개 `CorrelationScope`로 쌓은 값이 로그와 trace 문맥 기록에 같게 나온다, trace 세션 없이 task·lane 전파, trace 기존 시험 무수정 통과, 무할당 시험, 생산자 비용 측정(상관 없음·있음, task 제출 비용).

#### 14.1.5 LG15 trace `log` 채널

현재: TraceSystem은 `log`를 채널 이름 공간과 기본 preset에만 적었고(TraceSystem.ko.md 5절), 코드에는 채널도 기록 형식도 없다. 표준 채널은 cpu·counter·mark·span·frame·context·task·stats이다(`TraceStandardTypes.cpp`).

- **`TraceLogSink`**(TraceCapture 모듈, `RuntimeLogSink` 구현, L16). 채널 `log`는 TraceCapture가 표준 형식 등록에서 기본 꺼짐으로 등록한다(T3). `Write`는 채널이 꺼져 있으면 relaxed load 한 번으로 돌아온다.
- **기록 형식**(이름은 trace 표기를 따른다):
  - `Trace.LogMessageSpec`(정의 기록): spec id(u64, **호출 지점** = EventId·카테고리·위치), EventId(u64), 줄(u32), 레벨(u8), 카테고리 이름, 템플릿, 파일, 함수. 세션마다 호출 지점당 한 번 쓴다(같은 템플릿이라도 카테고리·위치가 다르면 다른 spec, UE의 로그 지점 주소 키와 같은 구별). 중복은 sink가 가진 고정 열린 주소 표(4096칸, 3/4까지 채움)로 막는다. 정의 쓰기가 **성공한 뒤에만** 표에 넣는다. 표가 차거나 정의 쓰기가 실패하면 정의 스트림을 더 쓰지 않고 그 기록에 카테고리·템플릿을 싣고 센다(`SpecTableOverflows`·`SpecWriteFailures`). 세션이 바뀌면 표를 비운다.
  - `Trace.LogMessage`(Ordered): tick, 생산자 OS 스레드 id, 프레임, spec id, EventId, 상관 id 5칸, 플래그, 레벨, 상관 id 마스크, trace 쪽 잘림, 가변 렌더된 메시지(최대 1024 B, UTF-8 경계) | 카테고리 | 템플릿(뒤 둘은 spec을 쓰지 못했을 때만).
  - **tick은 변환 없이 캡처 tick 그대로다.** LG13에서 기록 머리가 `MonotonicTicks`를 캡처하므로 trace의 다른 기록과 같은 시간 축이고 오차가 없다. (ns에서 tick으로 되돌리면 ns가 소수 버림 값이라 최대 1 ns에 해당하는 tick 차가 생긴다. 그래서 그 안을 쓰지 않는다.)
  - 생산자 스레드·시각·상관 id를 기록에 명시하는 이유: sink는 로그 출력 스레드에서 불리므로 trace 스레드 슬롯과 시각이 출력 스레드 것이 된다. trace 문맥 스택 기록으로 추정하지 않는다. OS 스레드 id는 `Trace.ThreadInfo`의 OS id로 대응한다.
  - 속성·문맥은 이번에 보내지 않는다. 메시지에 이미 렌더되어 있다(16절).
- **호스트:** trace 세션을 시작한 뒤 `LogHost::System().AddSink(traceSink)`, trace를 멈추기 전에 `RemoveSink`. 9.2절 종료 순서는 "엔진 정리 → trace sink 제거 → trace 정지 → 설치 해제 → `LogHost::Stop`"이 된다. 레벨 거르기 설정은 따로 두지 않는다. LogSystem 필터를 통과한 기록이 채널 `log`가 켜진 동안 들어간다.
- **분석:** TraceAnalysis가 두 기록을 읽고 `trace-report`가 레벨별 수와 Warning 이상 목록을 낸다(trace 세션 소유, 15.1절).
- UE: `LogChannel`과 `Logging.LogCategory`·`Logging.LogMessageSpec`·`Logging.LogMessage`(`LT:13–36`). spec은 로그 지점 주소마다 한 번 쓴다(`SLC:1268–1277`). 엔진 경로는 형식 문자열을 `%s`로 두고 서식된 메시지를 인자 하나로 보낸다(`SLC:1274`, `SLC:1281–1286`). Rocks는 실제 템플릿을 spec에 담아 EventId로 묶어 볼 수 있게 한다.
- 완료 기준: Manual LogSystem과 시험 trace 세션으로 기록하고 TraceAnalysis로 읽는 왕복(spec 한 번, 메시지 순서·스레드·tick·상관 id 일치), 채널이 꺼지면 기록 0, 중복 표 넘침 경로, 세션 교체 뒤 spec 다시 씀, 초기화 뒤 무할당, Sandbox `--engine --trace=default` 산출물에 로그 기록.

#### 14.1.6 LG16 구조체 펼치기 `{@Obj}`

- **문법:** 이름 자리 앞 `@`(`{@Player}`). spec은 쓸 수 없다(`{@Player:x}`는 템플릿 오류). `@`는 로그 템플릿에서만 받는다(일반 서식 파서는 지금처럼 거부).
- **확장점(Core):** `template <class T> struct LogStructure;` 특수화가 `static void Write(LogStructureWriter& out, const T& value) noexcept`를 가진다. `LogStructureWriter`는 Core 타입이다. 필드(이름 + 값), 중첩 객체 시작·끝, 배열 시작·끝을 받아 **캡처 버퍼(준비 버퍼·블록)에 바로 인코딩**한다. `@` 자리 인자에 특수화가 없으면 영어 `static_assert`로 거부한다.
- **캡처 시점 값 복사(L19):** 렌더는 출력 스레드가 나중에 하므로 그때 객체가 살아 있다는 보장이 없다(스택 객체, GC). 게임 스레드 밖에서 객체를 읽으면 경쟁이기도 하다. 그래서 생산자 5단계에서 값을 복사한다. 필드 이름은 생성 코드의 정적 리터럴 포인터를 담고, 값은 원시 값·문자열 바이트를 복사한다. UE도 호출 지점에서 필드를 compact binary로 직렬화해 기록이 사본을 가진다(`SLC:1398–1418`, `SerializeLogFields` `SLC:1302`, 사용자 타입은 `SerializeForLog` `SL:297`).
- **상한:** 깊이 4, 필드 32, 배열은 앞 8원소와 전체 개수. 넘치면 `truncated`. 크기는 슬롯·블록 한도(8.3)를 따른다.
- **리플렉션 연결과 의존 방향:** Core는 확장점만 정의한다(Core는 리플렉션 모듈 아래다).
  - 리플렉션 타입의 일반 특수화는 **ObjectRuntime** 헤더 `Object/LogStructure.h`가 `requires` 조건으로 제공한다. 객체 참조(`ObjectRefs.h`)·`AssetId`·약한 참조와 생성 필드 접근(`GeneratedFieldAccess.h`)이 ObjectRuntime에 있어서다. MetaSchema에는 `GeneratedType<T>`와 스키마만 있다.
  - 필드 값 읽기는 **MetaCompiler가 새로 생성하는 constexpr 필드 방문 함수**(`GeneratedType<T>::VisitFields(const T&, Visitor&)`, 멤버 포인터로 직접 읽기)로 한다.
  - 지금 공개된 값 읽기(`PathRead`·`FieldValue`)는 ObjectRegistry가 필요하고 문자열을 할당한다(`Object/RuntimeTypeOps.h`, `Object/PropertyPath.h`). 그래서 생산자 경로에 쓸 수 없다. `TypeOf<T>()`는 registry 없이 필드 이름·종류를 주지만 값 접근이 없다.
- **종류:** Bool·정수·실수는 원시 값, Enum은 생성 이름(없으면 정수), Utf8String은 바이트 복사, 중첩 Struct는 재귀, 배열은 위 상한. Name·Text·AssetId 등 다른 종류는 `Formatter`가 있으면 캡처 때 그 렌더 문자열을 담고, 없으면 `<unsupported>`. 객체 참조는 역참조하지 않고 id만 담는다(GC·스레드 안전).
- **출력:** 텍스트는 Serilog 모양 `Player { Name: "Ann", Hp: 30 }`, JSON lines는 `props` 안 중첩 객체. `LogProperty`에 Structure 종류와 자식 범위(평탄 배열 + 부모 번호)를 더한다(`LogEvent` 변경).
- 완료 기준: 캡처 뒤 원본을 바꾸거나 파괴해도 출력 불변(결정적), 깊이·필드·배열 상한 잘림, 텍스트·JSON 결정적 출력, MetaCompiler 생성 struct 왕복, 특수화 없는 타입의 컴파일 실패 시험, 무할당.

#### 14.1.7 LG17 시간 기준 회전과 압축 보관

- **시간 회전(L22):** `log.file.max_age_minutes`(0 = 끔). 묶음을 쓰기 전에, 그 묶음 첫 기록의 단조 시각이 "**그 파일 첫 기록의 단조 시각** + 상한"을 넘으면 회전한다(크기 회전과 같은 자리, 9.1절). 벽시계 날짜 경계가 아니라 경과 시간이다. 시계 조정에 흔들리지 않고, 기록 시각만으로 판정하므로 시험이 결정적이다. 파일 이름 규칙(UTC 시작 시각 + `-N`)은 그대로다.
- **압축(L23):** `log.file.compress = true`이면 flush 묶음마다 **독립 zstd frame**으로 압축해 쓴다. 파일은 `<호스트>-<시각>[-N].log.zst`다.
  - 이어 붙인 zstd frame은 그 자체로 올바른 zstd 스트림이라 `zstd -d`로 풀린다. 비정상 종료로 마지막 frame이 잘려도 앞 frame은 풀린다.
  - 자체 구현은 하지 않는다. Compression 모듈의 zstd(`CompressionFormat::Zstd`)를 쓴다. Compression은 Core 위라서 Core에는 `LogFileCipher`처럼 인터페이스 `LogBatchCompressor`(`Bound`·`TryCompress`)만 두고, 구현은 LogHost(의존에 Compression 추가)가 둔다.
  - **무할당은 지금 Compression API로는 보장되지 않는다.** 재사용 문맥 `Compressor`도 호출마다 pledged size를 바꾸고(`Compression/Private/ZstdCodec.cpp:312–315` `ZSTD_CCtx_setPledgedSrcSize`), zstd가 작업 공간을 다시 잡을 수 있다. 그래서 **Compression 모듈에 고정 작업 공간 문맥을 요청한다**(15.1절, LG17 선행 조건): 최대 묶음 크기(`batchBytes`)와 고정 매개변수(수준·window)로 `ZSTD_estimateCCtxSize` 계열 크기를 구해 한 번 확보하고 `ZSTD_initStaticCCtx`로 만든다. 그 크기를 넘는 요청은 할당하지 않고 실패한다. 출력 버퍼(압축 bound)도 sink Open 전에 확보한다.
  - 수준은 zstd 3(기본값). 출력 스레드 비용을 측정해 보고한다.
  - 닫힌 파일을 나중에 통째로 압축하지 않는 이유: 출력 스레드가 수십 ms 멈추거나 별도 스레드·임시 파일이 필요하고, 비정상 종료한 파일은 압축되지 않는다. UE는 이전 로그를 `-backup-` 사본으로 남기고(`ODF:420–435`) 시작 때 날짜·개수로 지울 뿐이다(`PurgeLogsDays`·`MaxLogFilesOnDisk`, `FH:1284–1360`). 압축과 실행 중 회전은 없다.
- **암호화와의 관계:** **압축한 뒤 봉인한다**(암호문은 압축되지 않는다). 압축 frame 하나가 secretstream 메시지 하나다. 봉인 파일 머리(`LogFileSealer`)에 압축 표시를 더한다. 봉인 파일은 압축 여부와 상관없이 `.log`다(일반 도구로 읽을 수 없으므로 `.zst`를 붙이지 않는다). 압축 뒤 크기가 내용에 따라 달라 암호문 길이가 내용을 조금 드러낸다. 9.1절 보안 목표(사용자가 읽을 수 없으면 된다)에서는 받아들인다.
- 보관 정리는 `.log`와 `.log.zst`를 함께 센다(이름 모양이 정확히 맞는 것만). `log decode`는 봉인+압축 파일을 풀고, `log view`(LG19)는 `.log.zst`도 읽는다.
- 완료 기준: `LogEvent`를 sink에 직접 넣어(단조 시각을 정해 둔 기록) 회전 결정적 시험 — 시계 주입이 필요 없다, 압축 파일을 Compression `DecompressStream`으로 풀어 왕복, 잘린 마지막 frame에서 앞부분 복원, 봉인+압축 왕복·변조 검출, 보관 정리(다른 호스트 파일 보존), Open 뒤 무할당.

#### 14.1.8 LG18 종료 경로의 마지막 로그

개정 8의 16절 밖, 새 범위다(2026-11-18 사용자 진행 지시·L25 선택으로 확정). **개정 12부터 처리기는 크래시 수집기가 소유한다**([크래시 처리·실행 진입점](CrashHandlingAndLaunch.ko.md) 19절). 로깅은 수집기의 종료 콜백에 flush 하나만 꽂는다. minidump·스택 수집·덤프와 flush의 순서는 그 문서가 정한다.

- `Log::Fatal`과 assert는 비상 출력 뒤 자기 스레드에서 best effort flush를 한다(11절). `platform::Fatal`을 바로 부르는 코드(UniquePtr 반환 실패 등)와 접근 위반·치명 신호는 이 경로를 지나지 않는다. 이들의 큐에 남은 기록은 수집기의 종료 콜백이 비운다.

**종료 콜백 등록(L24).**
- Core는 **첫 `LogInstallScope` 설치 때 정적 콜백 하나를 `platform::RegisterTerminationCallback`(`Platform/CrashReporting.h`)으로 한 번 등록하고 해제하지 않는다.** user는 nullptr다. 수집기 설치 전에도 등록할 수 있으므로 호스트의 설치 순서와 무관하다.
- 콜백은 크래시 처리 스레드(수집기의 W)에서 프로세스당 한 번 불린다. 콜백은 `LogReadScope`(세대 카운터)로 그때 설치된 LogSystem을 lock 없이 찾는다. 없으면 `Skipped`다.
- `LogInstallScope`는 등록·해제를 하지 않는다. 설치·해제가 겹치거나 시스템이 바뀌어도 콜백은 그 순간 설치된 것만 본다.
- 등록이 실패하면(칸 4개가 다 참) 비상 경로에 Error를 한 번 남기고 계속한다. 그 프로세스는 크래시 때 로그 flush가 없다.
- 수집기를 설치하지 않은 프로세스에서는 콜백이 불리지 않는다. Rocks 실행 파일은 Launch를 거쳐 설치하므로, 남는 것은 C# 도구와 외부 호스트뿐이다.

**콜백 본문(`detail::FlushInstalledLogForTermination(const TerminationContext&)`).**
- **출력 스레드 판정:** `failingThreadId`나 `stoppedThreadIds`(개수는 원자로 읽음)에 출력 스레드가 있으면 큐를 비울 주체가 없다. 기다리지 않고 비상 출력만 하고 `Skipped`다. 판정은 helper 하나가 두 칸을 함께 본다.
- **대기 시간:**
  - 기본은 `fatalFlushTimeoutMilliseconds`와 공용 마감까지 남은 시간 중 짧은 쪽이다.
  - `Log::Fatal`·assert가 flush를 시도했으면(아래 Fatal 표시) 실패 스레드의 대기가 제한 시간에 끊겼을 수 있다. 그 뒤에 들어온 다른 스레드의 기록도 있을 수 있다. 그래서 바로 건너뛰지 않고 남은 큐를 한 번 더 기다린다. 상한은 "공용 마감 - 여유"다.
  - 콜백은 등록 역순으로 불리고 마감을 함께 쓴다. 여유는 로그 뒤에 불리는 콜백(수집기가 설치 때 먼저 등록한 것 등)의 몫이다.
  - 마감이 이미 지났으면 `Skipped`다. 밀리초 변환은 `NumericLimits<uint32>::Max()`에서 자른다.
- **결과 값:** `LogSystem::FlushForTermination(timeout, failingThreadId)`이 `TerminationCallbackResult`를 바로 돌려준다. 결과는 크래시 기록에 남으므로 "기다렸지만 못 비움"과 "할 수 없었음"을 가른다.
  - `Completed`: 다 비웠다.
  - `TimedOut`: Async에서 마감까지 기다렸지만 큐가 남았다.
  - `Skipped`: 시작 전, Manual, 비상 모드, sink 콜백 안, 출력 스레드 자신, 출력 스레드가 실패하거나 멈춘 스레드, Sync에서 sink lock을 TryLock으로 못 잡음(쥔 스레드가 멈췄다).
- **`FlushForTermination`은 막히는 lock을 쓰지 않는다.** UE Panic(`OR:981–1016`, lock을 제한 시간만 기다린 뒤 진행)과 같은 모양이다.
  - Async: 출력 스레드를 `ManualResetEvent::TrySignal`(이벤트 mutex를 TryLock으로 몇 번 시도)로 깨운다. 그런 다음 lock 없이 큐 완료 위치를 원자로 확인하며 기다린다(양보하며 단조 시계로 상한 확인). PlatformBase에 주소 대기(WaitOnAddress·futex)가 생기면 깨우기를 그것으로 바꾼다.
  - Sync: sink mutex를 TryLock으로 잡아 flush하고, 못 잡으면 건너뛴다.
- 콜백은 할당하지 않는다(수집기 계약 7).

**Fatal 표시.**
- 프로세스 원자 표시 `detail::MarkFatalFlushAttempted()`·`FatalFlushAttempted()`를 둔다. `Log::Fatal`·assert가 설치된 LogSystem에 Fatal 기록을 넘기기 직전에 세운다. 콜백은 이 표시를 보고 대기 상한을 정한다(위).
- 스레드별 `fatalInProgress`(`Core/Private/Diagnostics/Log.cpp`)는 같은 스레드의 재귀를 막는 용도로만 남는다. 콜백이 도는 W에서는 보이지 않으므로 중복 판정에 쓰지 않는다. 중복 호출 자체는 수집기가 막는다(콜백은 프로세스당 한 번).
- assert는 `platform::FatalAt(message, file, line)`으로 끝나고, 수집기의 종료 사유는 `Assert`다.

**한계.**
- Async 큐에서 칸을 예약한 생산자가 게시 전에 멈추면(그 짧은 구간에서 크래시), 출력 스레드는 그 칸을 넘지 못한다. 콜백은 상한까지 기다린 뒤 `TimedOut`이고, 그 칸 뒤의 기록은 잃는다. 이 구간은 복사 한 번이라 드물다. 고치려면 큐가 멈춘 칸을 건너뛰어야 하는데, 생산자가 살아 있는지 가릴 수 없어 하지 않는다.
- 봉인 파일의 끝 표시: 비정상 종료에서는 `Final`을 쓰지 않는다. "끝 표시 없음 = 비정상 종료" 판정이 맞기 때문이다(9.1절).
- 크래시 기록에서 로그 파일을 찾을 수 있게, `FileLogSink`가 파일을 열 때와 회전할 때 출력 스레드에서 `SetCrashAnnotation(CrashAnnotation::LogFile, 경로)`로 현재 경로를 알린다. 칸 API는 크래시 수집 LC1b에서 생기며, 로깅은 그 뒤에 붙인다(15.1절).
- UE: 크래시 보고 전에 `GLog->Panic()`을 부른다(`Private/Windows/WindowsPlatformCrashContext.cpp:1833–1837`). Panic은 출력 장치 lock을 최대 1초 기다린 뒤 그대로 flush로 넘어간다(`OR:981–1016`).

**버린 대안(개정 9–11의 설계).**
- `platform::SetTerminationHook`(원자 포인터 하나)과 `LogInstallScope`의 hook 교체·앞 hook 연쇄를 버렸다. 등록자가 여럿이면 연쇄 순서가 설치·해제 순서에 묶이고, 해제가 늦은 hook을 덮을 수 있다. 수집기의 칸 등록이 이를 대신한다.
- 로깅 자체의 OS 예외·신호 처리기와 flush 전용 스레드를 버렸다. 처리기와 전용 스레드(W)·stack 보장·`sigaltstack`은 수집기가 하나만 둔다. 덤프와 flush의 순서도 한 곳에서 정해야 하기 때문이다.
- 스레드별 `fatalInProgress`로 중복 flush를 판정하던 방식을 버렸다. 콜백이 실패 스레드가 아닌 W에서 돌기 때문이다.

- 완료 기준(자식 프로세스 시험 `LoggerOutputTests`, 모든 모드가 수집기를 설치):
  - `platform::Fatal`·접근 위반·stack overflow(Windows)·SIGSEGV(POSIX) 직전의 Async 기록이 파일·stderr에 남는다. Windows는 정확한 종료 코드를 단언한다.
  - 출력 스레드가 멈춘 sink 안에서 Fatal이 나도 프로세스가 끝난다. 단언은 종료 코드와 내용이다.
  - 단위 시험: 설치된 시스템이 없거나 마감이 지났으면 `Skipped`, 설치돼 있으면 `Completed`이고 기록이 sink에 남는다. 출력 스레드가 멈춘 목록에 있으면 `Skipped`, Manual이면 `Skipped`다.
  - abort·WER 대화상자를 억제한다.

#### 14.1.9 LG19 `log view`

개정 8의 16절 밖, 새 범위다(2026-11-18 사용자 진행 지시로 확정).

- Commandlet `log view <파일|폴더> [--key <개인키>] [--level <최소>] [--category <접두사>] [--event <id>] [--since <UTC>] [--until <UTC>] [--grep <문자열>] [--correlation <칸>=<id>] [--json]`.
- 평문 `.log`(text·jsonlines), `.log.zst`, 봉인 파일을 읽는다. 폴더를 주면 같은 호스트 파일을 시작 시각·번호 순으로 이어 읽는다.
- 필드 거르기는 JSON lines 파일에서 모두 된다. text 파일은 기본 레이아웃일 때 레벨·카테고리·시각과 `--grep`만 된다(레이아웃을 바꾼 text 파일은 `--grep`만). 분석이 목적이면 `log.file.format = jsonlines`를 권한다(가이드에 적는다).
- 전문 viewer(Lens, 계획 M3)는 범위 밖이다(16절).
- 완료 기준: 고정 입력 파일(평문·zst·봉인)의 거르기 결과를 결정적으로 비교한다(Rocks.Commandlet.Tests).

### 14.2 구현과 다르게 정한 점(개정 11)

- **LG12:** `LogLayoutParseError`에 영어 리터럴 이유 `reason`을 더했다. 레이아웃 오류 Error는 `LogHost::Start`(설치 전)에서 나므로 stderr 비상 경로로만 나간다(파일에는 남지 않는다). 문법 검사는 `LogLayout::Parse`와 같은 상수 평가 함수(`detail::ScanLogLayout`)를 쓰고, `CheckLogLayoutSyntax`·`CheckLogLevelsSyntax`(LogHost)를 공개했다(CVR7 `Validate`용).
- **LG13:** API는 `LogSystem::SetRateLimit(prefix, perSecond)`·`ClearRateLimits()`, 통계는 `LogSystemStats::rateLimited`다. 시계 주입은 `LogSystemConfig::clock`(tick)·`clockFrequency`이고 `LogEvent`에 `monotonicTicks`를 더했다. `CheckLogRateLimitsSyntax`를 공개했다.
- **LG14:** 확장 칸 규칙을 정했다. `ext[0]`(`CorrelationCarriedExtension`)은 사본에 실려 다니고, `ext[1]`(`CorrelationPushExtension`)은 쌓기마다 Core가 0으로 시작한다(사본에 실린 쌓기 serial이 짝 없는 `ContextPop`을 만들지 않게). `CorrelationDepth()`는 인라인 TLS 읽기다. 상관 id 칸 이름·순서(`CorrelationFieldNames`)는 Correlation.h 하나에 둔다. 관찰자는 `TraceSession::Start`마다 다시 걸고(같으면 무동작) 떼지 않으며, 걸지 못하면 Warning을 남기고 센다. WorkerScheduler는 추적 여부와 상관없이 `TraceTaskScope`를 만들어 상관 id를 복원한다. `DedicatedLane::Post`는 사본을 잠금 밖에서 만든다.
- **LG15:** 기록 이름은 trace 표기 `Trace.LogMessageSpec`·`Trace.LogMessage`다. spec 키를 EventId에서 호출 지점(EventId·카테고리·위치)으로 바꿨고, 표가 찬 뒤에는 정의를 쓰지 않고 기록에 싣는다(14.1.5). `trace-report`는 판 4이고 `logs`의 시각은 정수 ns다. Sandbox에 `--trace-file <path>`를 더했다. 엔진 호스트는 trace가 돌 때 `TraceLogSink`를 더하고 끝낼 때 Flush → `RemoveSink` → trace 정지 순서로 뺀다. 자체 검사는 `--engine --trace=default --trace-file`로 자식을 돌려 trace 파일에 로그 기록("Engine run finished")이 있는지 확인한다(TraceAnalysis가 링크되지 않는 Release에서는 NOT RUN). spec id의 위치는 파일·함수 문자열의 내용 해시다. 정의 기록 상한(16 KiB)을 넘는 spec은 쓰기 전에 가려 바로 동봉하고, 동봉한 템플릿은 1024 byte에서 자른다(잘림 비트 2). 동봉 실패는 sink만 센다(TraceSystem 6.6절).
- **LG17:** 압축은 범용 `FixedWorkspaceCompressor`(Compression, zstd 고정 작업 공간·만든 뒤 무할당)를 쓰고, Core 인터페이스 `LogBatchCompressor`는 `LogFileCipher.h`에 있다. 봉인 파일 머리의 예약 1 byte를 `flags`로 쓴다(`LogFileFlagCompressedBatches`). 시간 회전 설정은 `FileLogSinkConfig::maxAgeNanoseconds`다.
- **LG18(개정 11 당시, 개정 12에서 수집기로 옮김):** 종료 hook 이름은 `platform::SetTerminationHook`, 처리기 API는 `platform::InstallCrashHandler`·`RemoveCrashHandler`·`PrepareThreadForCrashHandling`(`Platform/CrashHandler.h`)이다. `SetThreadStackGuarantee`·`sigaltstack`은 엔진 스레드만이 아니라 `platform::Thread`로 만든 모든 스레드가 시작 때 확보한다. POSIX 처리기는 이 환경에서 컴파일을 확인하지 못했다(Windows만 빌드·시험).
- **LG19:** `--json` 출력 선택은 두지 않았다(입력 JSON lines 줄을 그대로 낸다). 폴더는 `*.log`·`*.log.zst`를 읽는다. 봉인 여부는 magic(`RKLOGENC`)으로 정하고 키 없는 봉인 파일은 경고 뒤 건너뛴다. 압축 파일은 스트림으로 풀어(파일당 1 GiB 상한) 잘린 frame 앞까지 보여 준다. `--event`는 16진 숫자로 비교한다. 기본 레이아웃 기록의 이어진 줄은 앞 기록의 판정을 따른다. 시험용으로 `log pack`(묶음별 zstd frame)과 `log seal --compress`를 더했다.
- **LG18(리뷰):** POSIX 대체 stack은 정적 TLS가 아니라 스레드를 준비할 때 잡고 끝날 때 돌려주며(`ReleaseThreadCrashHandling`), 이미 있으면 덮어쓰지 않는다. 이 함수들은 개정 12에서 `Platform/CrashReporting.h`로 옮겨졌다(동작 같음).
- **LG18(개정 12, 크래시 수집 LC1a):** `Platform/CrashHandler.h`·`SetTerminationHook`·`TerminatingThreadId`·`LogTerminationHook`·`SetChainedTerminationHook`·`LogInstallScope`의 hook 교체를 지웠다. 코드 변경은 크래시 수집 세션(rocks-36)이 LC1a에서 함께 했고, 로깅이 리뷰했다. 리뷰 반영: `FlushForTermination`이 `TerminationCallbackResult`를 돌려줌(할 수 없었음은 `Skipped`, 기다렸지만 남음만 `TimedOut`), 출력 스레드 판정 helper 하나, 재대기 상한 "마감 - 여유", 밀리초 변환 상한, 멈춘 출력 스레드·Manual 시험.

## 15. 다른 세션과의 조율

`feature/engine-runtime` 밑에서 그래픽 관련 세션이 여럿 진행 중이다.

**현황(2026-11-17, 개정 4):** 아래 셋이 engine-runtime에 합쳐졌고, `feature/logging`이 이를 병합했다(`4be868c`). 그래서 아래 표의 "병합 전" 조건은 모두 풀렸다.
- render-lane(`f2cce3b`): 최소 Renderer가 Sandbox `SampleRenderer`로 옮겨졌고 `ThreadOptions::name`이 생겼다.
- Config CF1·CF2(`c9fea72`)
- TraceCapture TR1–TR4(`2ff7fbc`): `ProfileScope`를 삭제했고 `Clock.h`에 `MonotonicTicks`를 추가했다.

아래 표는 병합 전 겹침 기록이다.

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

### 15.1 후속 단계 조율(개정 9)

LG11–LG19 중 다른 세션 소유 모듈을 고쳐야 하는 곳이다. 그 단계는 해당 세션과 맞춘 뒤 시작한다.

| 단계 | 모듈(소유) | 필요한 변경 | 비고 |
|---|---|---|---|
| CVR7 | CVar(CVarSystem 세션) | `log.*` 이관 때 `log.levels`·`log.rate_limits`·`log.console.layout`·`log.file.layout`에 `Validate`(문법 오류 설정 단계 거부), `log.level`은 enum 표. 문법 검사는 상수 평가 함수 `constexpr StringView CheckLogLevelsSyntax(StringView)`·`CheckLogRateLimitsSyntax`·`CheckLogLayoutSyntax`(통과면 빈 view, 아니면 영어 리터럴 이유)를 그대로 쓴다(LG12·LG13 구현) | LG12는 Config를 고치지 않는다(L18). 지금 `ConsoleSettingDefinition::allowedValuesIgnoreCase`(LG8·LG9 리뷰)도 CVR7에서 enum 표로 대체 |
| LG14 | TraceCapture·Task(trace 세션) | 상관 id 스택을 Core `CorrelationScope`(확장 칸 `ext[2]`, 관찰자 hook)로 옮기고 `TraceContext*`는 관찰자·래퍼로 바꾼다. TR3 훅(`TraceTasks.cpp`)과 `DedicatedLane::Post`가 trace 채널과 무관하게 상관 id를 전파(비면 분기 하나) | L14–L16 합의 필요. TraceSystem.ko.md 6.4절·7절 개정. 기록 형식 불변, TR2·TR3 시험 무수정 통과 |
| LG15 | TraceCapture·TraceAnalysis·`trace-report`(trace 세션) | 채널 `log` 등록, `Trace.LogMessageSpec`·`Trace.LogMessage` 형식, 분석·보고 | `TraceLogSink`는 로깅이 쓰고 형식 표는 trace 쪽 문서에 둔다 |
| LG15 | 엔진 호스트(Sandbox `EngineHost`), HostConfig(rocks-95) | trace 세션 시작 뒤 sink 추가, 정지 전 제거 | `HostSettings`는 설정 문맥만 만들므로 고칠 것이 없다. 새 `log.*` 설정은 LogHost가 등록한다 |
| LG16 | ObjectRuntime·MetaCompiler(리플렉션 세션) | MetaCompiler가 `GeneratedType<T>::VisitFields`를 생성, ObjectRuntime에 `Object/LogStructure.h`(일반 특수화) | 객체 참조·AssetId·약한 참조·`GeneratedFieldAccess`가 ObjectRuntime에 있다. 생성 코드 크기 증가를 리플렉션 세션이 판단한다 |
| LG17 | Compression(Compression 소유 세션) | 고정 작업 공간 zstd 압축 문맥: 최대 입력 크기·고정 매개변수로 `ZSTD_estimateCCtxSize` 계열 크기를 한 번 확보하고 `ZSTD_initStaticCCtx`, 이후 할당 없음(넘치면 실패) | LG17 선행 조건. 지금 `Compressor`는 호출마다 pledged size를 바꿔 작업 공간을 다시 잡을 수 있다(`ZstdCodec.cpp:312–315`) |
| LG17 | Crypto(`LogFileSealer`, 로깅이 LG10에서 작성) | 봉인 파일 머리에 압축 표시 | crypto 세션에 알린다 |
| LG18 | PlatformBase 크래시 수집(rocks-36, CrashHandlingAndLaunch.ko.md 19절) | `platform::RegisterTerminationCallback`·`TerminationContext`, `ManualResetEvent::TrySignal`. 처리기·크래시 처리 스레드·stack 보장·`sigaltstack`은 수집기 소유 | 개정 12에서 이관 완료(LC1a). 남은 것: `FileLogSink`의 `SetCrashAnnotation(CrashAnnotation::LogFile, 경로)`는 LC1b의 칸 API 뒤. 주소 대기(WaitOnAddress·futex)가 생기면 깨우기를 그것으로 바꾼다 |

## 16. 범위 밖·후속

개정 8의 후속 항목은 14.1절 LG11–LG19로 옮겼다. 남은 것은 다음과 같다.

- **WorkerScheduler 지연·포화 진단:** 단계로 두지 않는다(L27). 선행 조건이 둘 있다.
  1. Engine(또는 엔진 호스트)이 WorkerScheduler를 소유해야 한다. 지금 Engine에는 스케줄러가 없고 실행기를 끝내는 `StopExecutors` hook만 있다(`Engine/Engine.h`). 인스턴스는 시험·도구의 지역 객체뿐이다(Sandbox `GraphicsSelfTest.cpp`, CompressionBench).
  2. `WorkerSchedulerStats`(`Task/WorkerScheduler.h`)에 대기 시간·포화·거부 수가 있어야 한다. 지금은 worker 수·진행 중 수·예약 수·세대·accepting뿐이고, 꽉 찼다는 사실은 `Submit`의 `CapacityExceeded`로만 알 수 있다.
  두 조건이 생기면 "포화 지속" Warning(rate limit 적용)과 주기 통계 Debug로 설계한다.
- **crash 수집 전반:** minidump, 스택 수집, crash reporter, 사전 확보 breadcrumb 저장소(계획 29.4절). 로깅 범위는 종료 경로의 flush(LG18)까지다. GPU 진단은 사용자가 보류했다.
- **전문 viewer:** Lens 통합(계획 M3). 로깅은 명령줄 `log view`(LG19)까지 한다.
- trace로 속성·문맥 보내기(LG15에서 뺐다), 실행 중 레이아웃 변경(LG12에서 시작 뒤 고정).

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
| L14 | 상관 id 스택은 Core가 소유한다(`CorrelationIds` 5×u64 + 상위 계층용 확장 칸 `ext[2]`, `CorrelationScope`, 쌓기·내리기 관찰자 hook). TraceCapture의 `TraceContext*`는 관찰자와 래퍼로 기존 cache·기록을 유지한다. 로그는 생산자에서 맨 위 40 B를 복사한다(비면 0 B, 할당 없음) | 결정됨(2026-11-18, 권장안 — trace 세션 합의 필요) |
| L15 | 상관 id 전파(task·`DedicatedLane::Post`)는 trace 채널과 무관하게 늘 한다. 스택이 비면 분기 하나다. trace 기록만 채널을 따른다 | 결정됨(2026-11-18, 권장안 — trace 세션 합의 필요) |
| L16 | trace `log` 채널은 TraceCapture의 `TraceLogSink`가 쓴다. `Trace.LogMessageSpec`(세션·호출 지점(EventId·카테고리·위치)당 1회, 쓰기 성공 뒤 표에 넣음, 표가 차면 기록에 카테고리·템플릿을 실음)과 `Trace.LogMessage`(캡처 tick·생산자 스레드·spec id·상관 id 명시, 렌더된 메시지)다. 기록 머리는 tick을 캡처한다(LG13). 속성은 보내지 않는다 | 결정됨(2026-11-18, 권장안 — trace 세션 합의 필요) |
| L17 | 레이아웃 설정 `log.console.layout`·`log.file.layout`은 시작 뒤 고정이고, 잘못된 패턴은 지금은 Start에서 Error 1회 + 기본 레이아웃이고, CVR7 이관 뒤 설정 단계에서 거부한다(L18). jsonlines 파일에서 `log.file.layout`은 Warning 후 무시 | 결정됨(2026-11-18, 권장안) |
| L18 | Config에 검증 hook을 더하지 않는다. 설정 단계 거부는 CVar `Validate`(CVarSystem VD10)로 CVR7 이관 때 한다. 그 전까지 레이아웃 오류는 Start에서 Error 1회 + 기본 레이아웃, `log.levels`·`log.rate_limits` 오류는 적용 시점 Warning 후 건너뜀 | 결정됨(2026-11-18, 권장안 — 개정 10에서 CVar 설계 확정에 맞춰 바꿈) |
| L19 | `{@Obj}`는 캡처 시점에 값을 복사한다. Core는 `LogStructure<T>` 확장점만 두고, 일반 특수화는 ObjectRuntime이 MetaCompiler 생성 필드 방문 함수로 registry·할당 없이 읽는다. 객체 참조는 역참조하지 않는다 | 결정됨(2026-11-18, 권장안 — 리플렉션 세션 합의 필요) |
| L20 | 사용자 Formatter spec 검증은 선택 멤버 `static constexpr bool IsValidSpec(StringView)`이다. 리터럴 템플릿은 컴파일 타임에, 런타임 템플릿은 로그 캡처 전용 함수 포인터 배열로 검사하고, 없으면 캡처 때 판단한다 | 결정됨(2026-11-18, 권장안) |
| L21 | 카테고리별 rate limit은 접두사별 1초 창이고, 창 번호(32-bit)와 수(32-bit)를 `Atomic<uint64>` 하나에 묶어 CAS로 바꾼다(창마다 통과 수 정확히 상한). 최대 16칸, Trace–Warning에만 적용, Error·Fatal 제외. 버린 수는 출력 스레드가 1초에 한 번까지 알린다 | 결정됨(2026-11-18, 권장안) |
| L22 | 시간 기준 회전은 벽시계 날짜가 아니라 그 파일 첫 기록 뒤 경과 시간(기록의 단조 시각)으로 판정한다 | 결정됨(2026-11-18, 권장안) |
| L23 | 압축은 flush 묶음마다 독립 zstd frame(Compression 모듈, `.log.zst`)이다. 암호화와 함께면 압축한 뒤 봉인한다. 닫힌 파일 후처리 압축은 하지 않는다. 무할당을 위해 Compression에 고정 작업 공간 문맥을 요청한다 | 결정됨(2026-11-18, 권장안 — Compression 세션 합의 필요) |
| L24 | 크래시·`platform::Fatal`에서 큐에 남은 기록을 flush한다. 개정 12부터 크래시 수집기의 종료 콜백(`platform::RegisterTerminationCallback`)에 Core가 첫 `LogInstallScope` 설치 때 정적 콜백 하나를 한 번 등록하고 해제하지 않는다. 콜백은 크래시 처리 스레드에서 `LogReadScope`로 설치된 시스템을 찾아 `FlushForTermination`을 부르고, 결과를 `TerminationCallbackResult`로 돌려준다. 막히는 lock 없이 TryLock·원자 완료 확인·제한 시간 대기만 쓴다. 실패하거나 멈춘 스레드가 출력 스레드면 기다리지 않고 비상 출력만 한다. `Log::Fatal`·assert는 프로세스 원자 표시를 세우고, 콜백은 그때 "마감 - 여유"까지 한 번 더 기다린다. 개정 9–11의 `platform::SetTerminationHook`·hook 연쇄는 버렸다(14.1.8). 개정 8의 16절 밖, 새 범위(2026-11-18 사용자 진행 지시·L25 선택으로 확정) | 결정됨(2026-11-18, 개정 12 변경) |
| L25 | OS 예외·치명 신호에서도 로그를 flush한다(사용자 선택 A). 개정 11까지는 로깅의 최소 처리기(Windows unhandled exception filter·flush 전용 스레드, POSIX `sigaltstack`·`sem_post`)였다. 개정 12부터 처리기·크래시 처리 스레드·stack 보장은 크래시 수집기(CrashHandlingAndLaunch.ko.md 19절)가 하나만 소유하고, 로깅은 L24 콜백만 꽂는다. 덤프와 flush의 순서는 그 문서가 정한다. 수집기를 설치하지 않은 프로세스는 크래시 flush가 없다. 비용은 기록당 0, 크래시 때 공용 마감(기본 2초) 안 | 결정됨(2026-11-18, 사용자 선택 A, 개정 12 변경) |
| L26 | 로그 보기는 Commandlet `log view`(거르기·여러 파일·평문·zst·봉인)까지 한다. 전문 viewer는 Lens(M3). 개정 8의 16절 밖, 새 범위(2026-11-18 사용자 진행 지시·L25 선택으로 확정) | 결정됨(2026-11-18) |
| L27 | WorkerScheduler 지연·포화 진단은 단계로 두지 않는다. Engine의 스케줄러 소유와 스케줄러 통계 확장이 선행 조건이다(16절) | 결정됨(2026-11-18, 권장안) |

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
- 개정 4(2026-11-17): 15절에 render-lane·Config·TraceCapture 병합 현황을 적었다(LG3 착수 조건 충족). 셰이더 컴파일 실패 로그 위치를 Sandbox `SampleRenderer`로 옮겼다. 캡처가 카테고리 객체 포인터 대신 이름을 담도록 바꿨다(8.3).
- 개정 5(2026-11-17): LG5 구현 반영 — 소스 경로는 `-fmacro-prefix-map`(디버그 정보 경로 보존), 컴파일 타임 하한은 타깃 단위 재정의·모듈 재정의 거부, 한글 템플릿은 `source-literal-english` 정책이 막으므로 별도 검사 없음.
- 개정 6(2026-11-17): LG6 구현과 LG3–LG5 리뷰 반영 — `ErrorCode::CycleDetected`, `World::AddActorTickPrerequisite`, Tick 실패 첫 1회+요약, `Log::SetFrame`, `SetGlobalMinimum`의 `Result<void>` 반환과 sink 안 레벨 변경 Busy, Sync Fatal은 `Mutex::TryLock`, Stop 시간 초과 포기 처리.
- 개정 7(2026-11-18): LG7 구현 반영 — `CreateD3D12Device`·`Window::Create`에 report 인자(기본 nullptr), `Graphics/GraphicsLog.h` 카테고리와 `ValidationMessageFilter`, 셰이더 실패는 장치 LastError + 호출자 기록, Sandbox는 backend별 실행이라 "건너뛴 backend Warning·모두 실패 Error", OpenGL 기존 로그는 규칙 충족으로 유지.
- 개정 8(2026-11-18): LG8·LG9 구현 반영 — 설정 이름을 Config 규칙에 맞춰 `log.file.max_bytes`·`log.file.recipient_key`로 바꿨다. `log.mode` 기본값 `default`(호스트 프리셋)를 더했다. 카테고리 재정의는 문자열 `log.levels`이고, 설정 처리는 새 `LogHost` 모듈에 둔다(9.2절). 보관 정리는 파일 이름 모양이 정확히 맞는 것만 센다. Asan·TSan 전체 체인은 저장소 검증 기준 변경으로 생략했다. Templates/Game은 tick·Engine이 없어 엔진 호스트로 다루지 않는다.
- 개정 9(2026-11-18): 후속 설계 확정 — 16절 항목을 LG11–LG19로 설계했다(14.1절, 결정 L14–L27). 상관 id 스택을 Core로 내리고 trace `log` 채널은 `TraceLogSink`가 쓴다. 레이아웃 설정과 Config 검증 hook, 카테고리별 rate limit, `{@Obj}` 캡처 시점 복사와 생성 필드 방문 함수, Formatter spec 검증 규약, 시간 회전과 묶음별 zstd 압축(압축 뒤 봉인), `platform::Fatal` hook, `log view`를 다룬다. WorkerScheduler 진단은 선행 조건만 적었다. UE 5.8.4 `StructuredLog`·`LogTrace`·`OutputDeviceFile`·`FileHelper`·`OutputDeviceRedirector::Panic`을 확인했다. 다른 세션 조율은 15.1절이다. 리뷰 반영: 상관 id 스택에 확장 칸·관찰자 hook(H1), rate limit 창·수 단일 원자 값(M1), Compression 고정 작업 공간 문맥 요청(M2), 종료 hook `const char*`·막히지 않는 flush(M3), `log.levels` 거부 범위(M5), `{@Obj}` 특수화를 ObjectRuntime으로(M6), 런타임 템플릿 spec 검사 경로·전파 분기 하나·`DedicatedLane` 전파·tick 캡처·LG17 시험 방식·validate reason(Low). L25는 사용자 선택 A로 확정했고, L24·L26은 개정 8의 16절 밖 새 범위로 표시했다.
- 개정 11(2026-11-18): LG11–LG15·LG17–LG19 구현 반영(14.2절). LG11–LG13·LG17–LG19 리뷰와 LG14·LG15 리뷰(rocks-95) 반영: spec 키를 호출 지점으로, 정의 쓰기 성공 뒤 표에 넣기, 표가 차면 기록에 싣기, 상관 id 확장 칸 규칙, 종료 순서(Flush → trace sink 제거 → trace 정지), 이름을 `Trace.LogMessageSpec`·`Trace.LogMessage`로. LG16은 GC 세션 병합 뒤로 미뤘다. LG14·LG15 재리뷰 반영: 빈 전파 경로 인라인(분기 하나), spec id 위치는 문자열 해시, 정의 상한 초과 spec 즉시 동봉·동봉 템플릿 상한, 측정 숫자(14.1.4).
- 개정 10(2026-11-18): CVar 설계 확정(CVarSystem 개정 2, Config 개정 4)에 맞춰 L18을 바꿨다. Config에 validate hook을 더하지 않고, 설정 단계 거부는 CVR7의 CVar `Validate`로 미룬다. LG12는 Config 조율 없이 LogHost 적용 시점 검사로 한다. 15.1절 Config 항목을 CVar CVR7 항목으로 바꿨다.
- 개정 12(2026-11-18): LG18 처리기를 크래시 수집기로 옮긴 것을 반영했다(14.1.8, L24·L25, 9.2절 호스트 순서, 14.2절, 15.1절). 로깅은 첫 `LogInstallScope` 설치 때 종료 콜백 하나를 등록하고 해제하지 않는다. `Log::Fatal`·assert의 프로세스 Fatal 표시, `failingThreadId`·`stoppedThreadIds`로 출력 스레드 판정, `FlushForTermination`의 결과 값을 정했다. `SetTerminationHook`·hook 연쇄·로깅 자체 처리기와 스레드별 중복 판정을 버린 이유와, 예약 뒤 게시 전 멈춘 칸의 한계를 적었다. 코드는 크래시 수집 LC1a(rocks-36)가 함께 바꿨고 로깅이 리뷰했다.
