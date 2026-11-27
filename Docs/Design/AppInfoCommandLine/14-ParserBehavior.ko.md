# 앱 정보와 명령줄 — 14. 명령줄 파서(C++) — 동작

[목차](README.ko.md)

## 14. 명령줄 파서(C++) — 동작

### 14.1 문법 규칙(양쪽 공통 — D20)

| 규칙 | 내용 |
|---|---|
| 옵션 철자 | `--kebab-case`. 한 글자 별칭은 `-x`. 대소문자를 구분한다 |
| 값 | `--name value`와 `--name=value` 둘 다. 구분자는 `=` 하나다(`--name:value`는 받지 않는다). 값이 `-`로 시작하면 `=` 형태만 된다(음수 등) |
| bool | `--flag`는 true. 명시하려면 `--flag=true`·`--flag=false`. 다음 토큰을 값으로 먹지 않는다(`--flag false`의 `false`는 위치 인자다) |
| 반복 | `Array<T>` 필드는 옵션을 반복할 때마다 원소를 하나 더한다. 쉼표로 나누지 않는다(값 안의 쉼표는 그 값의 일부 — `--set=log.levels=a:debug,b:info`). 스칼라 옵션을 두 번 주면 오류다 |
| 위치 인자 | 옵션이 아닌 토큰을 선언 순서대로 채운다. 옵션과 섞어 써도 된다 |
| `--` | 그 뒤는 모두 위치 인자다(남으면 `Remaining`) |
| 하위 명령 | 첫 위치 토큰부터 명령 경로를 찾는다(`log keygen`). 공용 옵션은 명령 앞뒤 어디서나 받는다 |
| 묶음 `-abc`, `/x`, `[directive]` | 받지 않는다 |
| 알 수 없는 인자(D29) | 명령에 `Remaining` 필드가 있으면 알 수 없는 옵션과 남는 위치 토큰을 원문 순서대로 그 필드에 넣는다. 없으면 정책을 따른다: 도구·commandlet·Debug·Development 게임 호스트는 오류(종료 2), **Game 대상 Release 호스트는 경고**로 남기고 무시한다. 경고에는 옵션 이름(`-`로 시작하는 토큰의 `=` 앞)만 쓰고 값과 위치 토큰은 `<unparsed>`로 가린다(9단계 로그 — 8.3절) |
| 응답 파일 | `@path`(도구·commandlet만 — 14.7절) |
| 도움말 | `--help`·`-h`·`-?`, `help [명령…]` |
| 버전 | `--version`(최상위만) |

- Game 대상 Release를 경고로 둔 이유: 스토어 런처와 플랫폼이 실행 파일에 자기 인자를 붙인다. 출시 게임이 그 인자 때문에 시작하지 못하면 안 된다. 개발 빌드는 오타를 잡으려고 오류로 둔다.
- `Remaining` 쓰임: FoundationTests의 `process-probe <mode> …`처럼 뒤 인자를 다른 해석기(probe mode)에 넘기는 명령.

### 14.2 값 변환과 바인딩(D19)

- **생성 바인더.** 옵션 값 하나는 생성 바인더 함수 호출 하나다. 바인더는 값 텍스트를 필드 형식으로 바꿔 넣는다. `Array` 필드는 원소 하나를 더하고, 스칼라 필드는 넣는다. 변환은 13.2절 표대로 Core `NumberText`·Base `Guid` 텍스트 변환·생성 enum 표만 쓴다. ReflectionText·TypeRegistry·ObjectRegistry를 만들지도 부르지도 않는다.
- **검사 순서.** 다음 순서로 진행하고, 각 단계의 진단은 모두 모은다.
  1. **토큰 단계.** 명령 경로를 찾고, 토큰마다 필드를 찾아 바인더를 부른다. 이 단계의 오류는 알 수 없는 옵션·명령·인자, 값 없음, 형식 오류, 스칼라 중복, Release의 `NotInRelease` 옵션이다. Release의 `NotInRelease` 옵션은 알 수 없는 옵션으로 다룬다.
  2. **필수 단계.** `Mandatory` 옵션과 필수 위치 인자가 채워졌는지 본다.
  3. **범위 단계.** 값이 들어온 필드의 `Range`를 본다. 기본값은 범위를 보지 않는다. 기본값이 범위 밖이면 MetaCompiler가 거부한다.
  4. **검증 단계.** `ValidateCommand` hook이 있으면 부른다. 1–3단계에 오류가 하나라도 있으면 부르지 않는다. 덜 채워진 struct를 검증하지 않기 위해서다.
- **`ValidateCommand` hook.** Command struct의 static 멤버다. 영어 이유 문자열을 진단으로 남긴다. 리플렉션 struct의 `Validate`(`Result<void>`만 돌려준다 — 5.5절)는 Command struct에 쓸 수 없다(RMC).

```cpp
struct SandboxOptions {
    // …
    static void ValidateCommand(const SandboxOptions& options, CommandValidation& validation) noexcept {
        if (options.frames.HasValue() && !options.engine) {
            validation.Fail("Option '--frames' requires '--engine'.");
        }
    }
};
```

- `validation.Fail`의 문자열은 그대로 `error: ` 뒤에 나온다. 문구 규칙은 14.4절을 따른다(영어, 끝에 마침표, 옵션 이름은 작은따옴표).
- **공용 옵션과 명령 옵션을 한 번에.** 파서는 대상 struct 여럿을 받는다. 보통 공용 옵션 하나와 명령 하나다. 두 struct 사이의 조합 검사는 실행 함수가 한다. `ValidateCommand`는 struct 하나만 본다.

```cpp
// 런타임 API(CommandLine 모듈). 도움말은 그리지 않는다(짧은 사용법 줄만 만든다).
struct CommandLineRules {
    bool release = IsReleaseBuild;                     // NotInRelease 옵션을 거부한다. 시험은 Debug에서 true로 준다
    UnknownArgumentPolicy unknown = UnknownArgumentPolicy::Error; // Game 대상 Release 호스트는 WarnAndIgnore
};

// target은 GeneratedCommand<S>의 스키마와 S 값 하나를 묶는다: CommandLineTarget::Of(options).
Result<CommandLineOutcome> ParseCommandLine(Span<const StringView> arguments,
                                            Span<const CommandLineTarget> targets,
                                            const CommandLineRules& rules,
                                            CommandLineDiagnostics& diagnostics) noexcept;

enum class CommandLineOutcome : uint8 {
    Parsed,       // targets가 채워졌다(경고만 있을 수 있다)
    HelpRequested,
    VersionRequested,
    UsageError    // diagnostics에 오류가 있다. 종료 코드 2
};
```

- `rules.release`가 런타임 값이므로, `NotInRelease` 동작은 Debug에서 `release = true`로 시험한다(17절). 도움말 문자열을 빼는 것은 컴파일 시점 일이라 MetaCompiler 생성 결과 시험으로 확인한다.
- 결과는 옵션 struct 값이다. 실행 함수는 `const Options&`로 받는다.

### 14.3 호스트 공용 옵션과 `--set`·`--trace`·`--config`(D25)

```cpp
// HostConfig 모듈. 모든 호스트(게임·Sandbox·RocksCommandlet)가 받는 공용 옵션이다.
ROCKS_STRUCT(Command(Global))
struct HostCommandLineOptions {
    ROCKS_GENERATED_BODY()

    explicit HostCommandLineOptions(Allocator& allocator) noexcept;

    ROCKS_PROPERTY(Help = "Override a setting for this run", ValueName = "target=value")
    Array<String> set;

    ROCKS_PROPERTY(Help = "Read an extra project-layer config file", ValueName = "file")
    Optional<String> config;

    ROCKS_PROPERTY(Help = "Enable trace channels or presets", ValueName = "channels")
    Array<String> trace;

    ROCKS_PROPERTY(Help = "Project directory", ValueName = "path")
    Optional<String> project;

    ROCKS_PROPERTY(Help = "Never show dialogs or wait for user input")
    bool unattended = false;

    ROCKS_PROPERTY(Help = "Use a fixed time step and a fixed random seed")
    bool deterministic = false;

    ROCKS_PROPERTY(NotInRelease, Range = "0<..", ValueName = "rate",
                   Help = "Fixed time step rate in frames per second")
    Optional<double> fps;

    // … 10.2절 나머지
};
```

- `--set`·`--trace`·`--config`를 같은 파서로 옮긴다. 인자 배열은 한 번만 읽는다.
  - `HostSettings::ApplyCommandLine`은 원시 인자 대신 `Span<const String>`(파싱한 `set` 배열)을 받는다. `--set=`를 두 번 훑는 일(5.4절)이 없어진다.
  - `TraceController::ApplyCommandLine`도 `trace` 배열을 받는다.
- **값의 의미는 그대로다.** `--set` 값은 지금처럼 Config가 ReflectionText 대입으로 해석하고 출처는 `--set`이다. 명령줄 파서는 `--set` 값을 문자열로만 옮긴다. `--trace` 값은 지금처럼 쉼표 목록을 TraceController가 나눈다. 바뀌는 것은 인자를 찾는 곳뿐이다.
- `--set=a=b`와 `--set a=b`를 둘 다 받는다(첫 `=`까지가 옵션 이름).
- 출시 빌드의 `--set` 제한(Config D5)은 파서가 아니라 Config가 그대로 판정한다.
- 도움말에서 공용 옵션은 각 명령의 Options 절에 함께 나온다.

### 14.4 출력 규약: 공통과 오류(D21·D22)

**Rocks 명령줄 출력 규약**은 이 절과 14.5절이다. C++(`CommandLine`·`CommandLineTools`)과 C#(`RocksCli`)은 각자 이 규약대로 렌더한다. 어느 쪽도 다른 라이브러리의 기본 출력을 따라가지 않는다. System.CommandLine의 기본 도움말·오류 출력은 쓰지 않는다(16.2절). 모양은 System.CommandLine 기본 도움말에서 가져왔지만, 기준은 이 규약이다.

| 항목 | 규약 |
|---|---|
| 인코딩 | UTF-8, BOM 없음. C++은 표준 스트림에 byte 그대로 쓰고, Windows 콘솔이면 넓은 문자 API로 쓴다. C#은 stdout·stderr writer를 `new UTF8Encoding(false)`로 만든다 |
| 줄 끝 | 모든 플랫폼에서 `\n` 하나. C++은 CRT 텍스트 모드 변환을 거치지 않는다. C#은 writer의 `NewLine`을 `"\n"`으로 둔다 |
| 언어 | 영어. 문화권 형식을 쓰지 않는다(수는 13.2절 표기). 사용자가 준 토큰·값을 다시 보일 때는 작은따옴표로 감싸고, 제어 문자(U+0000–U+001F, U+007F)는 `\xHH`로 쓴다 |
| 출력 대상과 종료 코드 | 아래 표 |
| 너비 | 터미널이면 그 열 수(C++ `platform::TerminalColumns(stream)`, C# 출력을 돌리지 않았을 때 `Console.WindowWidth`), 터미널이 아니면(파일·파이프) 80. 범위는 [40, 200]으로 자른다. 시험은 너비를 인자로 준다 |
| 글자 수 | UTF-8 코드 포인트로 센다. 도움말 문자열은 영어 ASCII이고 기본값 표시만 비 ASCII일 수 있다 |

| 결과 | 출력 | 종료 코드 |
|---|---|---|
| 성공 | 명령 출력 | 0 |
| 도움말·`--version` | stdout | 0 |
| 실행 실패(명령이 실패를 돌려줌) | stderr `error: <이유>` 한 줄 | 1 |
| 사용법 오류 | stderr: 오류 줄들 + 짧은 사용법 줄 + 안내 줄 | 2 |
| 무시한 알 수 없는 인자(Game 대상 Release) | 로그 Warning(stderr가 아니다) | 영향 없음 |

오류 문구(양쪽 같음, C++·C# 모두 이 표에서만 문구를 만든다):

| 종류 | 문구 |
|---|---|
| 알 수 없는 옵션 | `Unrecognized option '--fromat'. Did you mean '--format'?` |
| 알 수 없는 명령 | `Unrecognized command 'trace-reprot'. Did you mean 'trace-report'?` |
| 남는 위치 인자 | `Unrecognized argument 'extra'.` |
| 값 없음 | `Option '--format' requires a value.` |
| 형식 오류 | `Cannot parse 'forty-two' for option '--frames' as uint64.`(형식 이름은 C++ 필드 형식 이름: `bool`·`int32`·`uint64`·`float`·`double`·`string`·`guid`) |
| 범위 밖 | `Value '0' for option '--frames' is out of range 1..1000000000.` |
| enum 값 오류 | `'xml' is not a valid value for '--format'. Allowed values: json, text.` 이름 제안이 있으면 `Did you mean 'json'?`를 대신 붙인다 |
| bool 값 오류 | `'yes' is not a valid value for '--unattended'. Allowed values: false, true.` |
| 필수 옵션 없음 | `Option '--out' is required.` |
| 필수 인자 없음 | `Required argument 'input' was not provided.` |
| 명령 없음 | `Required command was not provided.` |
| 스칼라 중복 | `Option '--format' was specified more than once.` |
| 옵션 사이 검증 | `ValidateCommand`가 `Fail`로 준 문자열 그대로 |
| 응답 파일 | `Cannot read response file 'x.rsp': NotFound.`(14.7절) |
| 무시 경고(로그) | `Ignored unrecognized option '--epicapp'.`, `Ignored unrecognized argument '<unparsed>'.` |

- 오류는 stderr에 `error: ` 접두로 모두 쓴다. 그다음 그 명령의 짧은 사용법 한 줄과 안내 한 줄을 쓴다. 종료 코드는 2다.

```
error: Unrecognized option '--fromat'. Did you mean '--format'?
Usage: RocksCommandlet trace-report <input> [<output>] [options]
Run 'RocksCommandlet trace-report --help' for more information.
```

- **이름 제안(공용 함수).** 거리 계산은 Core `EditDistance`(Damerau-Levenshtein, 인접 전치 하나 — OSA) 하나다. [CVar](../CVarSystem.ko.md) 12절의 `ReportUnresolved`와 같은 함수다. 문턱은 호출자가 이름 붙은 상수로 준다.
  - 명령줄 문턱: `max(MinCommandLineSuggestionDistance, 길이 / CommandLineSuggestionLengthDivisor)`. 값은 1과 3이다. 짧은 옵션 이름에 CVar 문턱(3)을 쓰면 `--fps`→`--out` 같은 엉뚱한 제안이 나온다.
  - 후보는 그 명령의 옵션·별칭과 공용 옵션이다. 명령 오류면 형제 명령, enum 값 오류면 enum 값이다. 대상은 대시를 뗀 이름이고 ASCII 대소문자를 무시하고 비교한다. 가장 가까운 것을 고르고, 같은 거리면 사전순 첫째다. 후보가 없으면 제안 문장을 붙이지 않는다.
  - C#은 같은 알고리즘·문턱을 `RocksCli`에 옮겨 쓰고, 같은 사례 표로 시험한다(17절).
- 진단은 `CommandLineDiagnostics`에 모두 모은다(첫 오류에서 멈추지 않는다). 출력 순서는 14.2절의 단계 순서이고, 같은 단계 안에서는 인자 순서다.

### 14.5 출력 규약: 도움말(D21)

절은 Description·Usage·Arguments·Options·Commands·Examples 순서다. 비어 있는 절은 쓰지 않는다. 절 머리 다음 줄부터 두 칸 들여 쓰고, 절 사이에는 빈 줄 하나를 둔다.

**최상위**(`RocksCommandlet --help`, `RocksCommandlet help`). 이 예는 10.2절 실행 성격 옵션을 줄였다:

```
Description:
  Rocks command-line tools.

Usage:
  RocksCommandlet [command] [options]

Options:
  --set <target=value>  Override a setting for this run
  --config <file>       Read an extra project-layer config file
  --trace <channels>    Enable trace channels or presets
  --project <path>      Project directory
  --unattended          Never show dialogs or wait for user input
  --version             Show version information
  -?, -h, --help        Show help and usage information

Commands:
  config-dump                     Print the effective settings as JSON.
  core-check                      Run the core self-check.
  log                             Create log keys and decode or seal log files.
  trace-report <input>            Convert a trace file into a text report.
    [<output>]
```

이 예는 너비 80이다. Commands 절의 왼쪽 열은 32(80의 40%)이고, `trace-report <input> [<output>]`(31자)은 두 칸 간격을 둘 수 없어 이름 칸을 접었다.

**명령 상세**(`RocksCommandlet trace-report --help`, `RocksCommandlet help trace-report`):

```
Description:
  Convert a trace file into a text report.

Usage:
  RocksCommandlet trace-report <input> [<output>] [options]

Arguments:
  <input>   Trace file to read
  <output>  Report file to write; standard output when omitted

Options:
  -f, --format <json|text>  Report format [default: text]
  --channel <channel>       Only include these channels
  --set <target=value>      Override a setting for this run
  --config <file>           Read an extra project-layer config file
  --trace <channels>        Enable trace channels or presets
  --project <path>          Project directory
  --unattended              Never show dialogs or wait for user input
  -?, -h, --help            Show help and usage information

Examples:
  RocksCommandlet trace-report run.rtrace
  RocksCommandlet trace-report run.rtrace report.json --format json
```

**하위 명령 묶음**(`RocksCommandlet log --help`): Description(`Command(Name = "log", Group, Help = …)`의 설명)·Usage(`RocksCommandlet log [command] [options]`)·Options·Commands(`keygen`·`decode`·`seal`) 절이 나온다.

배치 규칙:
- **너비.** 14.4절 공통 표를 따른다.
- **왼쪽 열.** 너비는 `min(가장 긴 이름 칸 + 2, 전체 너비의 40%)`다(소수점 아래는 버린다). 절마다 따로 정한다. 이름 칸 + 두 칸 간격이 열보다 길면 이름 칸을 공백 경계에서 접고, 이어지는 줄은 두 칸 더 들여 쓴다(위 `trace-report <input> [<output>]`).
- **오른쪽 열.** 설명은 공백 경계에서 접는다. 한 단어가 열보다 길면 그 단어만 열 너비에서 자른다.
- **이름 칸 모양.** 별칭이 먼저이고 긴 이름이 뒤다(`-f, --format`). 값 자리는 `<ValueName>`이다. enum은 `<값|값>`(도구 이름, 이름순)이고, bool은 값 자리가 없다. 위치 인자는 `<이름>`, 선택 위치 인자는 Usage에서 `[<이름>]`, 반복 위치 인자는 `<이름>...`이다.
- **정렬.** 옵션은 선언 순서이고, 공용 옵션이 그 뒤에, 그다음 `--version`(최상위만)·`--help`가 온다. 명령 목록은 이름순이다. `Hidden` 옵션은 쓰지 않는다. Release 구성에서는 `NotInRelease` 옵션도 쓰지 않는다.
- **필수 표시.** 필수 옵션은 설명 끝에 ` (REQUIRED)`를 붙인다.
- **기본값 표시.** `[default: …]`는 기본값이 빈 문자열·빈 배열·빈 Optional·false가 아닐 때만 붙인다. 값은 13.2절의 필드별 출력 함수로 쓴다.
- **출력 대상.** 도움말은 stdout이고 종료 코드 0이다.
- **Game 대상 Release.** 게임 호스트는 `CommandLineTools`를 링크하지 않고 도움말 문자열도 비어 있다. `--help`는 `Help is not available in Release builds.` 한 줄을 stdout에 쓰고 종료 0이다. Program 대상의 Release는 도움말이 그대로 나온다(7.2절).

### 14.6 commandlet 구조(D24)

```cpp
// Programs/Commandlet의 등록 함수. 명령 하나에 한 줄이다.
Result<void> RegisterCommands(CommandSet& commands) noexcept {
    if (Result<void> added = commands.Add<TraceReportOptions>(&RunTraceReport); !added) {
        return added;
    }

    // config-dump 등록은 Config(Runtime)에서 이 실행 파일로 옮겼다(7.1절, CL4).

    if (Result<void> added = commands.Add<ConfigDumpOptions>(&RunConfigDump); !added) {
        return added;
    }

    return Result<void>::Success();
}

// 실행 함수: 파싱·검증이 끝난 옵션 struct를 받는다. 반환은 0..125다.
Result<int32> RunTraceReport(CommandContext& context, const TraceReportOptions& options) noexcept;
```

- `CommandSet`(Developer `CommandLineTools`)이 하는 일:
  - 명령 등록. 이름·설명·예시는 스키마에서 온다.
  - 하위 명령 묶음(`log keygen`의 `log`)을 자동으로 만든다. 묶음 설명은 `Command(..., Group)` struct를 `commands.AddGroup<LogGroup>()`으로 등록해 준다(13.3절).
  - 공용 옵션 struct를 연결한다.
  - `CommandLineRules`를 정한다: 도구는 `UnknownArgumentPolicy::Error`이고 `release`는 실제 구성이다.
  - `--help`·`help <명령…>`·`--version`을 처리한다.
  - 응답 파일을 펼친다.
  - 진단을 출력하고 종료 코드를 정한다(0 성공, 1 실행 실패, 2 사용법 오류).
- 명령이 등록되면 최상위 도움말의 Commands 목록과 그 명령의 상세 도움말이 저절로 생긴다. 별도 도움말 코드가 없다.
- `CommandContext`는 다음을 준다.
  - 표준 출력·오류 writer(지금 `CommandletOutput`의 역할)
  - 할당기
  - 부트가 끝난 `HostSettings`
  - `AppInfo`
- 단일 소유 스레드 규칙, 이름 문법(소문자·숫자·`-`), 반환 범위 0..125는 지금 `CommandletRegistry`(`Commandlet.h:24–57`)와 같다. 개수·길이 상한은 이름 붙은 상수다.
- `--list`는 없앤다. `help`가 목록이다.
- 명령의 실행 함수는 옵션 struct를 받으므로 `log` 하위 명령의 "인자 위치 고정 비교"(5.2절) 같은 코드가 없어진다.

### 14.7 응답 파일(D23)

- `@path` 토큰을 그 파일의 토큰으로 바꾼다. `CommandSet`(도구·commandlet)만 한다. 게임 호스트는 받지 않는다. 게임은 긴 인자 목록이 필요 없고, 받지 않으면 출시 빌드가 임의 파일을 읽는 경로가 하나 줄어든다.
- 토큰 문법은 Rocks가 정한다. C#도 System.CommandLine의 응답 파일 처리를 끄고 `RocksCli`가 같은 문법으로 펼친다(16.2절).
  - 파일은 UTF-8이다(BOM이 있으면 뗀다). 줄 끝은 `\n`·`\r\n`을 받는다.
  - 앞뒤 공백을 뗀 줄이 비었거나 `#`로 시작하면 건너뛴다.
  - 줄을 공백(스페이스·탭)으로 나눈다. 큰따옴표로 묶은 부분은 공백을 포함한 한 토큰의 일부이고 따옴표는 지운다. 따옴표 안에서 `""`는 따옴표 글자 하나다. `\`는 어디서나 글자 그대로다. 그래서 `"C:\dir\"`가 그대로 `C:\dir\`이 된다(escape를 `\`로 하면 `\`로 끝나는 Windows 경로가 깨진다).
  - 닫히지 않은 따옴표는 오류다(`x.rsp:3`).
- 상한(이름 붙은 상수):
  - 파일 크기 1 MiB
  - 중첩 깊이 4
  - 펼친 뒤 인자 수 `ProcessCommandLine::MaxArguments`
- 순환(같은 파일을 다시 펼침)은 오류다.
- 파일을 못 읽으면 `Cannot read response file 'x.rsp': NotFound.`이고 종료 2다. 원래 오류 코드 이름을 붙인다.
- 펼친 위치는 진단에 `x.rsp:3`처럼 파일·줄로 나온다.
