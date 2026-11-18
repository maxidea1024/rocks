# 로깅 사용 안내와 구현 규약

엔진 코드에서 로그를 남기는 방법과 호스트가 로깅을 설치하는 방법, 지켜야 할 규칙을 정리한다. 설계와 근거는 [로깅 시스템 설계](../Design/LoggingSystem.ko.md)에 있다. 이 문서는 구현된 기능만 적는다.

## 빠른 시작

```cpp
#include <Core/Diagnostics/Log.h>

// 모듈 공개 헤더(<Module>/<Module>Log.h)에 카테고리를 둔다.
inline constexpr LogCategory LogAudio{"Audio"};

Log::Info(LogAudio, "Audio device opened: {Device} at {SampleRate} Hz", name, rate);
Log::Warning(LogAudio, "Voice pool exhausted; dropping {Count} voices", dropped);
Log::Error(LogAudio, "Failed to open {Device}: {Error} (OS error {OsError})", name, error, osError);
```

- 매크로가 없다. 레벨은 함수 이름(`Trace`, `Debug`, `Info`, `Warning`, `Error`, `Fatal`)으로 정한다.
- 반환값은 없다. 로깅 실패는 호출자에게 전파되지 않는다.
- 호출 위치(파일·줄·함수)는 자동으로 붙는다. `__FILE__`을 직접 넘기지 않는다.

## 카테고리

```cpp
inline constexpr LogCategory LogGraphicsD3D12{"Graphics.D3D12"};
```

- 이름은 PascalCase이고, 점으로 계층을 나눈다(`Graphics.D3D12`는 `Graphics` 아래다). 구간은 영숫자만 쓰고, 최대 8구간·63 byte다. 어기면 컴파일 오류(`LogCategoryError_*`)다.
- 객체에는 상태가 없다. 전역 생성자도 없고 초기화 순서 문제도 없다. 같은 이름을 여러 곳에서 정의해도 같은 카테고리다.
- 레벨 설정에서 카테고리를 찾을 때는 대소문자를 무시한다(`graphics.d3d12`도 맞는다). 출력에는 정의한 표기가 나간다.
- 함수 지역 `constexpr LogCategory`로 기록해도 안전하다. 캡처가 객체가 아니라 이름(정적 저장소의 리터럴)을 담기 때문이다.
- 지금 있는 카테고리는 다음과 같다.

  | 카테고리 | 정의 위치 |
  |---|---|
  | `Engine` | `Engine/EngineLog.h` |
  | `World`, `World.Tick` | `World/WorldLog.h` |
  | `Graphics`, `Graphics.D3D12`(`.Validation`), `Graphics.D3D11`(`.Validation`), `Graphics.Vulkan`(`.Validation`), `Shader` | `Graphics/GraphicsLog.h` |
  | `Graphics.OpenGL` | OpenGLGraphics 내부 |
  | `Platform.Window` | `PlatformWindow/Window.h` |
  | `Shader.Tool` | `ShaderToolLoader.h` |

## 메시지 작성 규칙

- **영어로 쓴다.** 템플릿, 자리 이름, 카테고리 이름이 대상이다. 시험이 아닌 소스의 문자열 리터럴에 한글을 쓰면 policy 검사(`source-literal-english`)가 거부한다. 인자로 들어오는 값(이름·경로)은 원문 그대로 둔다. 사용자에게 보이는 문구는 로그가 아니라 지역화 Text 계층을 쓴다.
- **Warning 이상은 원인을 알 수 있게 쓴다.** 한 줄에 다음 넷을 담는다. 언제·어디서(시각, 카테고리, 위치, 스레드, 프레임, 문맥)는 자동으로 붙는다.
  - 무엇이 실패했나
  - 대상
  - 원인: `ErrorCode`와 원본 코드(HRESULT, `GetLastError`, errno)
  - 엔진이 한 조치

  ```cpp
  // 나쁜 예
  Log::Error(LogGraphics, "Device creation failed");
  // 좋은 예
  Log::Warning(LogGraphics, "Failed to create {Backend} device on {Adapter}: {Error} (HRESULT {HResult:#x}); falling back to {Fallback}",
               backend, adapter, error, hr, fallback);
  ```

- **실패를 확정하는 계층에서 한 번만 남긴다.** 저수준 API는 오류와 원본 정보를 반환값이나 report(`DeviceCreateReport`, `WindowCreateReport`)로 넘기기만 한다. 복구 여부나 fallback을 정하는 계층이 기록한다.
- **정상 결과는 로그가 아니다.** NotFound·Busy·Unsupported·취소, 호출자가 fallback할 수 있는 하위 실패는 Error로 남기지 않는다.
- **반복되는 실패는 첫 번째와 요약만 남긴다.** 예: World Tick 실패는 World마다 첫 Error와 "N more times" 요약 한 번. 예상 가능한 재시도(World 파괴 지연)는 프레임마다 Warning을 내지 않는다.
- **레벨 기준**

  | 레벨 | 쓰는 경우 | 쓰지 않는 경우 |
  |---|---|---|
  | Trace | 매 프레임·매 객체 수준 상세 | 계측 목적(그건 TraceCapture) |
  | Debug | 개발 중 상태, GC 통계, 첫 재시도 | 정상 Tick, 모든 Component 생성 |
  | Info | 수명 주기, 선택 결과, 복구 완료 | 프레임마다 반복되는 사건 |
  | Warning | 계속 실행 가능하나 사람이 봐야 함 | 정상적인 NotFound·Unsupported |
  | Error | 이 계층에서 실패가 확정됨 | fallback 가능한 하위 실패 |
  | Fatal | 불변식이 깨져 종료 | 입력 오류·할당 실패(`Result`로 처리) |

- 비밀번호·토큰·키·사용자 입력 전문을 기록하지 않는다.

## 템플릿 문법과 검증 범위

| 문법 | 뜻 |
|---|---|
| `{Name}` | 이름 있는 자리. 인자에 순서대로 묶이고, 이름은 속성 키가 된다. 영문자로 시작한다 |
| `{Name:spec}` | 기존 Rocks 서식 spec(`#x`, `.3f`, `>8` 등) |
| `{}` · `{0}` | 위치 자리. 속성 키는 `Arg0`, `Arg1`… 이름 자리와 한 템플릿에 섞을 수 없다 |
| `{{` · `}}` | 중괄호 문자 |

- **컴파일 타임에 검증하는 것**(리터럴 템플릿): 중괄호 짝, 이름·위치 혼용, 자리 수와 인자 수(위치 자리는 모든 인자를 한 번 이상 써야 한다), 위치 번호 범위, 이름 문법, UTF-8, **내장 타입**의 spec, 같은 사용자 타입 인자에 서로 다른 spec. 어기면 영어 이름의 오류 함수(`LogTemplateError_*`)가 진단에 나온다.
- **사용자 `Formatter<T>`의 spec:** 특수화에 `static constexpr bool IsValidSpec(StringView spec) noexcept`를 두면 리터럴 템플릿은 컴파일 타임에(`LogTemplateError_InvalidFormatterSpec`), `RuntimeTemplate`은 캡처 때 검사한다(거부되면 `[template error: InvalidFormatterSpec]`). 없으면 런타임에 `Format`이 판단하고, 잘못되면 그 자리만 `<format error>`로 나온다. `Guid`·`DateTime`(`ms`)·`TimeSpan`·`AssetId`는 검사 함수를 가진다.
- **동적 템플릿:** 설정이나 데이터에서 온 문자열은 `Log::Write(level, category, RuntimeTemplate{text}, args...)`로 넘긴다. 검증은 런타임에 하고, 잘못되면 원문에 `[template error: ...]`를 붙여 그대로 기록한다.
- 인자는 최대 16개(`MaxLogArguments`)다.
- 사용자 타입은 기존 `Formatter<T>` 특수화로 지원한다. 값은 기록하는 순간 텍스트로 렌더되며, 이 비용은 호출 스레드가 낸다. 내장 타입(정수·실수·bool·포인터·enum·문자열)은 원시 값으로 복사하고 출력 스레드가 렌더한다.

## 인자 평가와 Lazy

일반 함수 호출이므로 **인자는 레벨이 꺼져 있어도 평가된다.** 꺼진 레벨에서 생략되는 것은 서식화와 큐 기록뿐이다.

```cpp
Log::Debug(LogWorld, "State {State}", BuildWorldStateDump());   // Debug가 꺼져 있어도 BuildWorldStateDump()를 부른다

Log::DebugLazy(LogWorld, [&](LogLine& line) noexcept {           // 꺼져 있으면 람다를 부르지 않는다
    line.Write("State {State}", BuildWorldStateDump());
});
```

- `TraceLazy`·`DebugLazy`의 람다는 레벨이 컴파일·런타임 모두에서 활성일 때만, **호출 중 즉시 한 번** 실행된다. 보관하지 않는다.
- 람다 캡처 초기화(`[x = Build()]`)는 호출 전에 평가된다. 비싼 계산은 본문에 둔다.
- 로그 인자에 프로그램 동작에 필요한 부작용을 넣지 않는다.
- 드물게 직접 판단해야 하면 `Log::IsEnabled(level, category)`를 쓴다.

## 문맥 스코프

```cpp
LogContextScope world("World", world.Name());
LogContextScope stage("Stage", "BeginPlay");
// 이 범위의 모든 기록에 World=Main Stage=BeginPlay가 붙는다
```

- 스레드별 고정 스택이고 깊이는 최대 8(`MaxLogContextDepth`)이다. 값은 기록하는 순간 복사되므로, 범위가 끝난 뒤에도 안전하다. 깊이나 슬롯 공간이 모자라면 안쪽부터 버리고 `ContextTruncated`를 표시한다.
- Engine은 초기화·종료 단계(`Stage`)와 World Tick(`World`) 문맥을 자동으로 넣고, 프레임마다 `Log::SetFrame`으로 프레임 번호를 알린다.

## 레벨: 컴파일 타임과 런타임

| 빌드 구성 | 컴파일에 포함하는 최소 | 런타임 기본 최소 |
|---|---|---|
| Debug | Trace | Info |
| Development | Debug | Info |
| Release | Info | Info |

- **컴파일 타임 하한**(`ROCKS_LOG_COMPILED_MINIMUM`)은 Rocks.Build가 타깃 전체에 같은 값으로 정의한다. 타깃 규칙의 `LogCompiledMinimum`으로만 바꿀 수 있다. 모듈 단위 재정의는 빌드 검증이 거부한다(모듈마다 다르면 ODR 위반). 컴파일에서 빠진 레벨은 런타임 설정으로 되살릴 수 없다.
- **런타임 레벨**은 설치된 `LogSystem`이 가진다.
  - `SetGlobalMinimum(level)`
  - `SetLevel("Graphics", LogLevel::Debug)`: 접두사 단위다. 가장 긴 접두사가 이긴다.
  - `ClearLevel(prefix)`
- 우선순위는 컴파일 타임 하한, 카테고리 재정의, 전역 최소 순이다. Fatal은 어떤 필터로도 막히지 않는다.
- 레벨 변경은 반환된 뒤 시작한 호출부터 적용된다. sink 콜백 안에서 레벨을 바꾸면 `Busy`다.

## 호스트: LogSystem 설치

```cpp
int main() {
    SystemAllocator allocator;
    StderrLogSink console;                                    // 기본 콘솔 레이아웃
    LogSystem logs(allocator, LogSystemConfig::ForEngineHost()); // 명령줄 도구는 ForTool()
    (void)logs.AddSink(console);
    (void)logs.Start();
    LogInstallScope installed(logs);                          // 전역 호출 대상. 범위가 끝나면 이전 대상을 복원
    (void)logs.SetThreadName("GameThread");
    ...
}   // 역순 정리: 설치 해제 → Stop(큐 drain, sink flush, 출력 스레드 join) → sink 파괴
```

| 모드 | 동작 | 기본으로 쓰는 호스트 |
|---|---|---|
| `LogMode::Async` | 호출 스레드는 캡처와 큐 넣기만 한다. 서식화와 sink 출력은 출력 스레드(`LogWriter`)가 한다 | tick이 있는 엔진 호스트(`ForEngineHost`) |
| `LogMode::Sync` | 호출 스레드가 렌더하고 sink까지 부른다. 큐와 출력 스레드가 없다 | 명령줄 도구(`ForTool`) |
| `LogMode::Manual` | Async와 같은 큐지만 출력 스레드 없이 `Pump()`로 비운다 | 시험 |

- Engine은 LogSystem을 만들거나 설치하지 않는다. 호스트가 한다.
- **설치 전·해제 후 호출도 안전하다.** Warning 이상은 OS 쓰기로 stderr에 바로 나가고, 그 아래 레벨은 버리고 개수만 센다. 그 수는 다음 설치 때 한 번 알린다.
- **큐가 가득 찼을 때:** 생산자는 기다리지 않는다. 큐가 90%를 넘으면 Info 이하를 거부하고, 남은 10%는 Warning 이상 전용이다. 그래도 차면 버리고 레벨별로 센다. 공간이 생기면 "dropped since the last report"를 한 줄 남긴다. 도구 호스트는 `overflow = LogOverflow::WaitForSpace`로 손실 대신 대기를 고를 수 있다(게임 스레드에서는 쓰지 않는다).
- `Flush(timeout)`은 이 호출 전에 넣은 기록이 모두 sink에 도달할 때까지 기다린다.
- `Stats()`는 손실·잘림·템플릿 오류·sink 실패·차단 수와 비상 모드 여부를 보여 준다.
- **Stop의 제한 시간:** drain이 `drainTimeoutMilliseconds`를 넘기면 남은 수를 알리고 진행한다. 출력 스레드가 sink에서 끝내 돌아오지 않으면 스레드와 메모리를 남겨 둔 채 반환한다(`Stats().writerAbandoned`). 이 경우 호스트는 LogSystem을 파괴하지 말고 프로세스를 끝낸다. 등록한 sink도 그때까지 살아 있어야 하고, 그동안 `AddSink`·`RemoveSink`는 `Busy`다. sink 콜백 안의 `Stop`은 아무것도 하지 않는다.

## sink와 출력 형식

```cpp
auto layout = LogLayout::Parse("[{Timestamp:ms}][{Level:u3}][{Category}] {Message}{Context}{Location}{NewLine}");
if (layout) { TextLogSink sink(output, layout.Value()); ... }
logs.AddSink(sink, {LogLevel::Warning, LogGraphics});   // Graphics·Graphics.* 의 Warning 이상만
```

- 레이아웃 토큰은 다음과 같다.

  | 토큰 | 출력 |
  |---|---|
  | `{Timestamp}`·`{Timestamp:ms}` | UTC ISO 8601(`Z` 포함) |
  | `{Elapsed}` | 경과 시간 |
  | `{Level}`·`{Level:u3}` | 레벨 이름 · `TRC DBG INF WRN ERR FTL` |
  | `{Category}` | 카테고리 이름 |
  | `{EventId}` | 템플릿 해시 |
  | `{Thread}` | 스레드 이름(없으면 id) |
  | `{Frame}` | 프레임 번호 |
  | `{Message}`·`{Template}` | 렌더된 메시지 · 원본 템플릿 |
  | `{Properties}` | 템플릿에 안 쓰인 속성 |
  | `{Context}` | 문맥 값 |
  | `{Location}` | 파일·줄·함수(기본 Warning 이상만) |
  | `{NewLine}` | 줄바꿈 |

- 값이 없는 토큰은 감싸는 `[]`나 앞 공백과 함께 빠진다. 잘못된 패턴은 `Parse`가 `InvalidArgument`로 거부한다.
- 기본 콘솔 출력 예:

  ```
  [2026-11-17T09:30:00.250Z][Warning][World] World Main destroy deferred for 300 frames: external references World=Main Engine/Private/Engine.cpp:262 ProcessDeferred
  ```

- `JsonLinesLogSink`는 한 줄에 객체 하나를 쓴다. 필드는 `t`, `mono`, `lvl`, `cat`, `event`, `tpl`, `msg`, `props`, `ctx`, `src`, `thread`, `frame`, `flags`이다. 숫자와 bool 속성은 JSON 숫자·불리언으로 나간다.
- 소스 경로는 저장소 기준 상대 경로다(빌드가 `-fmacro-prefix-map`을 건다).
- **sink를 직접 만들 때의 계약:**
  - `Write(const LogEvent&)`의 문자열은 콜백 동안만 유효하다. 보관하려면 복사한다.
  - 콜백에서 엔진 lock을 잡지 않는다.
  - sink 콜백이 잡는 lock을 쥔 채로 `AddSink`·`RemoveSink`를 부르지 않는다. `RemoveSink`가 반환되면 진행 중인 콜백은 없다.
  - Sync 모드에서는 호출 스레드가 sink lock을 잡는다. 그러니 sink 콜백이 잡는 lock 안에서 Sync 로그를 남기지 않는다.
  - 같은 sink가 연속 8번 실패하면 차단되고, 256묶음마다 다시 시도한다.

## 성능 특성

- 생산자(호출 스레드)의 비용은 다음과 같다.
  - 활성 검사: 원자 읽기
  - 시계 한 번
  - 값 복사
  - 큐 CAS 한 번
- 생산자 경로에는 sink lock, 힙 할당, 기록마다의 시스템 호출, 내장 타입 서식화가 없다. 예외는 잠든 출력 스레드를 깨우는 생산자 하나가 짧은 이벤트 lock을 한 번 거치는 것이다.
- 엔진 lock(렌더 레인·GC·스케줄러) 안에서 호출해도 된다(Async). 그래도 그 안에서 비싼 인자를 만들지 않는다.
- 실측(Release, LogBench, 한 기기, 보장 아님)은 다음과 같다.

  | 경우 | 평균 | p99 |
  |---|---|---|
  | 컴파일 제외 | 0 ns | — |
  | 런타임 비활성 | 약 5 ns | — |
  | Async 활성 | 114–171 ns | 200–300 ns |
  | Sync 활성 | 772–938 ns | 약 1 µs |

  Sync는 sink 출력까지 호출 스레드가 하므로, 엔진 호스트를 Sync로 두면 메인 틱이 출력을 기다린다.
- Start 이후에는 생산자와 출력 스레드 모두 힙을 할당하지 않는다. 큐 2 MiB, 블록 256 KiB, 렌더 버퍼 64 KiB는 Start에서 확보한다.

## Fatal·assertion·Error의 차이

| 경로 | 용도 | 동작 |
|---|---|---|
| `Log::Error` | 작업 실패를 보고하고 계속 실행 | 일반 기록. 필터 대상 |
| `Log::Fatal` | 복구할 수 없는 불변식 붕괴 | 렌더 → 비상 출력 → (가능하면) 큐에 넣고 제한 시간 flush → **반드시** `platform::Fatal`. 필터·sink 상태와 무관하게 종료한다 |
| `ROCKS_ASSERT` | 프로그래머 오류 검사 | `AssertFailed`가 같은 비상 출력과 flush 시도 뒤 `platform::Fatal` |

- Fatal이 재귀하면(로깅 내부의 assert 등) 바로 `platform::Fatal`로 간다.
- 로깅 경로 자체의 실패(템플릿 오류, 공간 부족, 큐 포화, sink 실패, 출력 스레드 시작 실패)는 버리기·낮추기·집계로 끝난다. 엔진을 종료시키지 않는다.

## 파일 기록과 암호화

```cpp
FileLogSinkConfig file;
file.directory = logDirectory;
file.hostName = "Game";
file.format = LogFileFormat::Text;          // 또는 LogFileFormat::JsonLines
crypto::LogFileSealer sealer(recipientPublicKey);   // 암호화할 때만(기본은 평문)
file.cipher = &sealer;
FileLogSink fileSink(allocator, file);
(void)fileSink.Open();
(void)logs.AddSink(fileSink);
```

- **파일 이름과 회전**
  - 실행마다 새 파일 `<폴더>/<호스트>-YYYYMMDDTHHMMSSmmmZ.log`를 만듭니다. 회전한 파일과 같은 시각에 시작한 다른 실행은 `-2`, `-3` …이 붙습니다.
  - `maxBytes`(기본 16 MiB)를 넘으면 회전하고, `retention`(기본 8, 최대 256)개까지만 남깁니다. 보관 정리는 이 호스트의 이름 모양(`<호스트>-<시각>[-<번호>].log`)만 세므로 `Sandbox`와 `Sandbox-Server`의 파일은 섞이지 않습니다.
- **쓰기:** flush 묶음 단위로 한 번에 씁니다. 렌더는 텍스트 레이아웃 또는 JSON lines sink와 같습니다. 쓰기나 열기가 실패하면 `DependencyFailed`를 돌려주고 다음 묶음에서 다시 엽니다.
- **암호화는 선택이며 기본은 끔입니다.**
  - Core는 Crypto보다 아래 계층이라, 암호화는 호스트가 `crypto::LogFileSealer`를 만들어 `cipher`로 넘깁니다.
  - 봉인: 파일마다 개발자 공개키로 X-Wing KEM 캡슐화를 해서 SecretStream 키를 뽑고, flush 묶음마다 봉인합니다. 정상 종료와 회전 때는 끝 표시(Final)를 씁니다.
  - 게임 바이너리에는 공개키만 들어갑니다. 목표는 사용자가 파일을 열어 읽지 못하게 하는 수준이고, 완벽한 보안이 아닙니다.
  - **키 준비에 실패하면 평문으로 내려가지 않습니다.** 파일을 만들지 않고 sink를 끄며(`IsDisabled()`), 비상 출력으로 한 번 알립니다.
- **파일 형식(버전 1)**
  - 머리 1156 byte: `RKLOGENC`, 버전, KEM 종류, KEM 암호문 1120 byte, SecretStream header
  - 그 뒤로 `length(4) | 봉인된 묶음`이 반복됩니다.
  - 정상 종료면 Final 기록으로 끝납니다.
- **명령(Commandlet)**
  - `log keygen <out-public> <out-private>`: 키 쌍을 만듭니다. 두 경로 중 하나라도 이미 있으면 아무 파일도 남기지 않고 실패합니다(짝이 맞지 않는 키 쌍 방지). **개인키는 저장소나 쿠킹 결과에 넣지 않습니다.**
  - `log decode <file> --key <private> [--json]`: 평문을 표준 출력으로 냅니다. `--json`은 상태 요약(`batches`·`endMarker`·`partialTail`·`error`, 성공·실패 같은 모양)을 표준 오류에 JSON 한 줄로 냅니다.
    - 끝 표시가 없으면(비정상 종료) 마지막 완전한 묶음까지 내고 "no end marker (abnormal termination)"를 알립니다.
    - 변조되었으면 "authentication failed"(종료 코드 1)입니다.
  - `log seal <plain> <out> --public <public>`: 같은 형식으로 봉인합니다(진단·시험용).
- **무할당 범위:** 파일을 연 뒤의 쓰기·봉인 경로는 엔진 allocator와 `operator new`를 쓰지 않습니다. 다만 회전 때 새 파일을 여는 OS 핸들과 새 KEM 캡슐화(Crypto 키 저장소)는 예외입니다.

## 설정 파일로 고르기(LogHost)

엔진 호스트는 `LogHost` 모듈로 설정 시스템의 `log.*` 콘솔 설정을 읽어 LogSystem과 sink를 만든다(Core는 Config·Crypto 아래라 이 모듈이 그 위에서 묶는다). 설정 문맥은 `HostConfig` 모듈의 `HostSettings`가 만들고, `log.*` 등록(`RegisterLogSettings`)도 그 `Initialize`가 파일보다 먼저 한다(Docs/Design/ConfigSystem.ko.md 5.3·5.4절). 시작 뒤 고정 값(`log.mode`·`log.file.*`)은 `CompleteStartup`에서 확정되고, LogHost는 그 뒤에 읽기만 한다. 실제 호스트 코드는 `Programs/Sandbox/Private/EngineHost.cpp`다.

```cpp
HostSettings settings(allocator);                      // LogHost보다 먼저 선언(더 오래 산다)
if (!settings.Initialize(boot) || !settings.LoadFiles() ||        // 1) 등록(log.*·trace.*) 2) 층 파일
    !settings.AddFile(config::ConfigLayer::Project, "Engine", path) ||  // 3) --config
    !settings.ApplyCommandLine(arguments) || !settings.CompleteStartup()) {  // 4) --set 5) 고정
    return 2;                                          // 진단은 HostSettings가 비상 경로로 이미 남겼다
}
LogHost host(allocator);
(void)host.Start(LogSystemConfig::ForEngineHost(), &settings.Context(), "Sandbox");
{
    LogInstallScope installed(host.System());
    (void)host.Watch(settings.Context());              // log.level·log.levels 변경을 프레임 Sync에서 적용
    // ... trace 세션 → 엔진 루프(프레임마다 Config Sync) → 엔진 정리 → trace 정지
}
host.Stop();                                           // WriterAbandoned()면 정리 없이 _Exit
```

| 설정 | 종류 | 기본값 | 변경 |
|---|---|---|---|
| `log.mode` | string | `default`(호스트 프리셋) · `async` · `sync` | 시작 뒤 고정 |
| `log.level` | string | `info` (`trace`…`fatal`) | 실행 중, sync 지점 |
| `log.levels` | string | `""` — `Graphics=warning,Engine.World=debug` | 실행 중, sync 지점 |
| `log.rate_limits` | string | `""` — `Graphics=20,Engine.World.Tick=100`(접두사별 초당 기록 수, 최대 16항목) | 실행 중, sync 지점 |
| `log.file.enabled` | bool | `false` | 시작 뒤 고정 |
| `log.file.directory` | string | `Saved/Logs` | 시작 뒤 고정 |
| `log.file.format` | string | `text` · `jsonlines` | 시작 뒤 고정 |
| `log.file.max_bytes` | int64 | `16777216` (64 KiB–1 TiB) | 시작 뒤 고정 |
| `log.file.retention` | int32 | `8` (1–256) | 시작 뒤 고정 |
| `log.file.encrypt` | bool | `false` | 시작 뒤 고정 |
| `log.file.recipient_key` | string | `""` — X-Wing 공개키 base64(`log keygen`이 출력) | 시작 뒤 고정 |
| `log.console.layout` | string | `""`(기본 콘솔 레이아웃) — 예: `"[{Level:u3}] {Message}{NewLine}"` | 시작 뒤 고정 |
| `log.file.layout` | string | `""`(기본 콘솔 레이아웃). `log.file.format = jsonlines`이면 쓰이지 않는다(Warning 한 번) | 시작 뒤 고정 |

- 레벨 이름과 `log.levels`의 접두사는 대소문자를 가리지 않는다(`"log.level": "Warning"`도 된다). `log.level`에 모르는 이름을 넣으면 설정 단계에서 거부되고(Config 진단) 앞 값이 남는다. `log.levels`의 잘못된 항목은 Warning을 남기고 건너뛴다.
- **rate limit:** `log.rate_limits`(또는 `LogSystem::SetRateLimit`)는 접두사별 1초 창마다 정확히 상한만 통과시킨다. Trace–Warning에만 적용하고 Error·Fatal은 세지 않는다. 버린 수는 접두사마다 1초에 한 번까지 `Rate limit dropped {Dropped} records for {Prefix} (limit {Limit}/s)` Info로 알리고 `Stats().rateLimited`에 쌓인다. 잘못된 항목은 적용 때 Warning 후 건너뛴다(`CheckLogRateLimitsSyntax`가 CVR7의 `Validate`).
- 기록 시각은 `MonotonicTicks`로 캡처하고 출력 때 ns로 바꾼다(`LogEvent::monotonicTicks`·`monotonicNanoseconds`). 시험은 `LogSystemConfig::clock`·`clockFrequency`로 시계를 주입한다.
- 레이아웃 패턴이 잘못되면 `LogHost::Start`가 위치와 이유를 담은 Error를 한 번 남기고 기본 레이아웃으로 계속한다(예: `log.file.layout is not a valid layout at byte 8: unknown layout token; using the default layout`). 설정 단계에서 거부하는 일은 CVar 이관(CVR7) 뒤다. 문법만 보는 상수 평가 함수 `CheckLogLayoutSyntax`·`CheckLogLevelsSyntax`(통과면 빈 view, 아니면 영어 이유)가 그때 `Validate`로 쓰인다.
- `log.level`은 늘 값이 있으므로(기본 `info`) 설정 문맥을 주면 호스트 프리셋의 전역 최소를 덮어쓴다. 설정 문맥 없이 `Start`하면 프리셋 그대로다.
- 멈춘 sink 때문에 `Stop`이 출력 스레드를 남기면 `LogHost`는 LogSystem·sink를 파괴하지 않고 프로세스 끝까지 놓아 두며 `WriterAbandoned()`가 참이 된다(그 뒤 `Start`는 Busy). 호스트는 이때 정리 없이 프로세스를 끝낸다(Sandbox는 `_Exit`). 생성자에 준 allocator도 프로세스 끝까지 살아 있어야 한다.
- 암호화를 켰는데 공개키가 없거나 틀리면 파일을 쓰지 않고 Error를 한 번 남긴다(평문으로 내려가지 않는다).
- Sandbox: `Sandbox --engine --config <Engine.json>`의 `"Settings"` 섹션이 이 값들이다. 예: `{"Settings": {"log.mode": "sync", "log.file.enabled": true}}`.

## 상관 id

```cpp
CorrelationScope request(CorrelationIds{.request = requestId});   // 이 범위의 기록에 request=... 
Log::Info(LogNet, "Request accepted");
```

- `Core/Diagnostics/Correlation.h`의 스레드별 스택(깊이 16)이다. 로그 기록은 맨 위 값(5칸, 40 B)을 캡처 때 복사한다. 비어 있으면 0 B다.
- 출력: 레이아웃 토큰 `{Correlation}`(`session=3 task=1842`, 0인 칸은 뺌), JSON lines `corr` 객체. 기본 콘솔 레이아웃에는 넣지 않았다.
- trace와 같은 스택이다(`TraceContextScope`는 별칭 래퍼). task(`WorkerScheduler`)와 `DedicatedLane::Post`는 trace 채널과 무관하게 상관 id를 넘긴다. lane 작업이 끝나면 lane 스레드의 스택은 원래 깊이로 돌아온다.

## trace로 보내기

- `TraceLogSink`(TraceCapture)를 trace 세션 시작 뒤 `LogSystem::AddSink`로 더하고, `TraceSession::Stop` 전에 `RemoveSink`로 뺀다. 채널 `log`(기본 묶음 `default`에 있다)가 켜져 있을 때만 `Trace.LogMessageSpec`·`Trace.LogMessage`를 쓴다.
- 카테고리 `Trace`·`Trace.*` 기록과 세션이 없을 때의 기록은 보내지 않는다. 메시지는 1024 byte까지다.
- `trace-report`에 `logs`(레벨별 수, Warning 이상 목록)가 나온다.

## 시험에서 쓰기

- `LogMode::Manual` LogSystem과 캡처 sink를 시험 안에서 `LogInstallScope`로 설치한다. 기록한 뒤 `Pump()`를 부르고 단언한다. sleep을 쓰지 않는다.
- 설치 범위가 끝나면 이전 대상이 복원되므로, 레벨이나 sink가 다른 시험으로 새지 않는다.
- 출력 전체를 비교하는 시험은 앞선 "설치 전에 버린 기록" 알림이 섞이지 않도록 먼저 빈 설치로 그 수를 비운다.

## 아직 구현되지 않은 것

- 구조체 펼치기(`{@Obj}`): 후속(GC 세션 병합 뒤)
