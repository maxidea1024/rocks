# 앱 정보와 명령줄 — 13. 명령줄 파서(C++) — 선언

[목차](README.ko.md)

## 13. 명령줄 파서(C++) — 선언

### 13.1 원칙

- 편의성의 기준점은 C#의 System.CommandLine이다. 명령 하나를 만들 때의 코드 양, 옵션 선언 한 줄로 파싱·검증·도움말이 나오는지, 형식 변환이 자동인지, 결과를 꺼내 쓰기가 편한지를 C#과 같은 수준 이상으로 맞춘다(15절 비교).
- **정의 한 곳.** 명령·하위 명령·옵션·위치 인자마다 이름·별칭·형식·기본값·필수 여부·설명·예시를 옵션 struct에 한 번 선언한다. 그 정의 하나로 파싱·검증·도움말을 모두 만든다. 설명을 두 번 쓰지 않는다.
- **외부 라이브러리 없음.** CLI11·cxxopts·argparse·Lyra·Boost.Program_options를 검토하고 제외했다. 이유는 크기다(사용자 결정 2026-11-18).
- **리플렉션은 선언 문법만 쓴다(D19·D30).** 선언은 `ROCKS_STRUCT`·`ROCKS_PROPERTY` 지정자 문법으로 하지만, Command struct는 TypeRegistry에 등록하는 리플렉션 타입이 아니다.
  - MetaCompiler는 Command struct에서 명령줄 스키마와 필드별 바인더만 생성한다. FieldSchema·`GeneratedType<S>` 등록·stable ID를 내지 않는다.
  - 그래서 실행 파일은 ObjectRuntime·ReflectionText를 링크하지 않아도 된다. 선언 매크로(`Meta/Annotations.h`) 때문에 MetaSchema 헤더에만 기대고, MetaSchema는 Core에만 의존한다. Core만 링크하는 벤치와 FoundationTests Core 계층도 옮길 수 있다(5.5절 "제약: 계층").
  - 개정 1의 "ReflectionText 대입 하나 = 옵션 하나" 방식은 버렸다. 원소 추가 표기가 없고 같은 경로 두 번이 오류이며, Validate 이유를 전할 수 없고, enum·bool 표기가 C#과 맞지 않기 때문이다(5.5절 "제약" 행).

### 13.2 선언 예

```cpp
// 명령줄 전용 enum이다. 이름 표만 생성하고 identity가 필요 없다(D30).
ROCKS_ENUM(Command)
enum class TraceReportFormat : uint8 {
    Text ROCKS_ENUMERATOR(Name = "text"),
    Json ROCKS_ENUMERATOR(Name = "json")
};

// 명령 하나 = 옵션 struct 하나. 도움말 문자열은 실행 중 출력이라 영어다.
ROCKS_STRUCT(Command(Name = "trace-report", Help = "Convert a trace file into a text report.",
                     Examples = ("trace-report run.rtrace",
                                 "trace-report run.rtrace report.json --format json")))
struct TraceReportOptions {
    ROCKS_GENERATED_BODY()

    explicit TraceReportOptions(Allocator& allocator) noexcept : input(allocator), channel(allocator) {
    }

    ROCKS_PROPERTY(Argument, Help = "Trace file to read")
    String input;

    ROCKS_PROPERTY(Argument, Help = "Report file to write; standard output when omitted")
    Optional<String> output;

    ROCKS_PROPERTY(Alias = "-f", Help = "Report format")
    TraceReportFormat format = TraceReportFormat::Text;

    ROCKS_PROPERTY(Help = "Only include these channels")
    Array<String> channel;
};
```

- 옵션 이름은 필드 이름(또는 `Name=`)을 kebab-case로 바꿔 만든다. 예: `channel` → `--channel`, `maxFrames` → `--max-frames`.
- 받는 필드 형식과 값 표기(C#도 같은 표기만 받는다 — 16.2절):

| 형식 | 표기 | 변환 |
|---|---|---|
| `bool` | 값 없는 플래그. 명시할 때는 `--flag=true`·`--flag=false`만(소문자 정확 일치) | 생성 바인더 |
| `int8`–`int64`, `uint8`–`uint64` | 10진. 전체 소비, 꼬리 문자 거부, 형식 범위를 넘으면 범위 오류 | Core `TryParseInt64`·`TryParseUInt64` 뒤 형식 범위 확인 |
| `float`·`double` | Core `TryParseDouble`·`TryParseFloat` 문법. 유한한 값만 받는다(바인더가 `nan`·무한대를 거부한다) | Core `NumberText` |
| `String` | 그대로(UTF-8) | 복사 |
| `Guid` | 36자 소문자 하이픈 표기 | Base `Guid` 텍스트 변환(새 공용 함수 — 7.1절) |
| 명령줄 enum(`ROCKS_ENUM(Command)`) 또는 일반 `ROCKS_ENUM` | 도구 이름 정확 일치, 대소문자 구분. 명령줄 enum의 도구 이름은 `[a-z][a-z0-9-]*`(kebab-case 허용). 일반 enum은 기존 `[A-Za-z0-9_]+` 규칙(`Specifiers.cs:323`)을 따르므로 `-`를 쓸 수 없다 | 생성 enum 표 |
| `Optional<T>` | T의 표기. 주지 않으면 빈 값 | — |
| `Array<T>` | 옵션을 반복할 때마다 원소 하나를 더한다. 쉼표로 나누지 않는다 | 원소마다 T 변환 |

- 그 밖의 형식(Name·struct·Map·ObjectRef 등)은 RMC 오류다. Name은 NamePool이 필요해 Core만 링크하는 도구가 쓸 수 없다.
- enum을 대소문자 구분으로 정한 이유: 옵션 이름이 대소문자를 구분하므로 한 규칙으로 맞춘다. 틀린 대소문자에는 이름 제안이 붙는다(`'JSON' is not a valid value for '--format'. Did you mean 'json'?`).
- 기본값은 생성자를 실행한 결과다(멤버 초기화식). 도움말의 `[default: …]`는 기본 인스턴스를 생성 바인더의 필드별 출력 함수로 써서 만든다. 수는 Core 숫자 출력(최단 왕복), enum은 도구 이름, bool은 `true`·`false`다.
- 필드 사이 검증은 Command 전용 hook `ValidateCommand`를 쓴다(14.2절). 예: Sandbox의 `--frames`는 `--engine`과 함께만 쓴다.

### 13.3 지정자(MetaCompiler에 더할 것)

| 대상 | 지정자 | 값 | 뜻 | 검사(RMC) |
|---|---|---|---|---|
| STRUCT | `Command(...)` 묶음 | 아래 키 | 이 struct는 명령줄 정의다. `Settings(...)`와 같은 묶음 지정자다 | `Settings`·`Version`·`Atomic`·`Binds`와 함께 쓰지 않는다. base 없는 비템플릿 struct, `S() noexcept` 또는 `explicit S(Allocator&) noexcept` |
| `Command` 안 | `Name` | 문자열 | 명령 경로. 하위 명령은 공백으로 잇는다(`"log keygen"`). 각 마디는 commandlet 이름 문법(소문자·숫자·`-`) | 문법, 예약어 `help` |
| `Command` 안 | `Help` | 문자열 | 한 줄 설명(명령 목록과 Description 절) | 비어 있지 않다 |
| `Command` 안 | `Examples` | 문자열 목록 `("…", "…")` | 예시 줄. 프로그램 이름을 빼고 쓴다 | **새 값 형태**: 지정자 문법에 문자열 목록을 더한다(`Specifiers.cs` 86–92·337행의 값 분류) |
| `Command` 안 | `Global` | 플래그 | 공용 옵션 struct(모든 명령에서 받는다 — System.CommandLine의 Recursive) | `Name`·`Group`과 함께 쓰지 않는다 |
| `Command` 안 | `Group` | 플래그 | 하위 명령 묶음의 설명만 선언한다(`Command(Name = "log", Group, Help = "Create log keys and decode or seal log files.")`). 필드가 없는 struct다 | 필드가 있으면 오류. 같은 이름의 묶음이 이미 자동으로 생기면 그 설명이 된다 |
| PROPERTY | `Help` | 문자열 | 옵션·인자 설명. 계획된 함수 리플렉션의 `ROCKS_FUNCTION(…, Help = "…")`([CVar](../CVarSystem.ko.md) 14.3절)과 같은 키 이름이다 | `Hidden`이 아니면 필수 |
| PROPERTY | `Alias` | 문자열(쉼표 목록) | 다른 이름. `-f`(한 글자) 또는 `--other-name` | 문법, 같은 명령·공용 옵션과 겹침 |
| PROPERTY | `Argument` | 플래그 | 위치 인자. 선언 순서대로 받는다 | `Alias`와 함께 못 쓴다. 필수 인자는 선택 인자 앞, `Array` 위치 인자는 마지막 하나만 |
| PROPERTY | `Mandatory` | 플래그 | 반드시 줘야 하는 옵션. 기존 `Required`(참조 non-null)와 뜻이 달라 다른 이름을 쓴다 | `Optional`·`Array`·`bool`에는 못 쓴다 |
| PROPERTY | `ValueName` | 문자열 | 도움말의 `<값>` 자리 이름. 없으면 옵션 이름(대시 뺀 것) | 문법 |
| PROPERTY | `Range` | 문자열 `"1..1000000"`, 한쪽만 `"0<..1000"`·`"1.."` | 수 범위. `<`는 그 끝을 뺀다. MetaCompiler가 필드 형식으로 양 끝을 해석해 검사한다 | 수 형식만, 최소 ≤ 최대 |
| PROPERTY | `Hidden` | 플래그 | 도움말에 보이지 않는다(받기는 한다) | — |
| PROPERTY | `Secret` | 플래그 | 로그용 명령줄에서 값을 가린다(12절) | `bool`에는 못 쓴다 |
| PROPERTY | `NotInRelease` | 플래그 | Release 구성에서 받지 않는다(10.2절). 기존 설정 지정자 `DevelopmentOnly`(설정 전용, `Cheat`로 바뀔 예정 — 5.5절)와 다른 Command 전용 키다 | — |
| PROPERTY | `Remaining` | 플래그 | 해석하지 않은 남은 인자를 원문 순서대로 받는다(14.1절, D29) | `Array<String>`만, 명령마다 하나, `Array` 위치 인자와 함께 못 쓴다, `Global`에는 못 쓴다 |

- **Command 키는 Command struct 안에서만.** `Command` 묶음이 없는 struct의 필드에 위 PROPERTY 키(`Help`·`Alias`·`Argument`·`Mandatory`·`ValueName`·`Range`·`Hidden`·`Secret`·`NotInRelease`·`Remaining`)를 쓰면 RMC 오류다(설정 지정자의 RMC2210과 같은 방식). 거꾸로 Command struct의 필드에는 설정 지정자와 직렬화 지정자(`Transient`·`SaveGame` 등)를 쓸 수 없다.
- **위치 인자 필수 규칙.** `Optional`·`Array`가 아닌 위치 인자는 필수다. 따로 표시하지 않는다.
- **도움말은 문서 주석에서 가져오지 않는다(D18).** 손으로 쓴 주석은 한국어이고 도움말은 실행 중 출력이라 영어다. 문서 주석을 가져오면 이 규칙이 깨진다. 주석 추출은 MetaCompiler에 Clang 주석 노드 수집을 새로 넣는 일이기도 하다.
- **identity 면제(D30).** Command struct·명령줄 enum은 직렬화하지 않으므로 stable ID가 없다.
  - `rocksids` 항목이 필요 없고 `ids add`를 부르지 않는다. 필드를 더하거나 이름을 바꿔도 빌드만 하면 된다.
  - 모듈 안 중복 판정과 descriptor 왕복에는 MetaCompiler가 정규 이름(`rocks::HostCommandLineOptions`)에서 만든 키를 쓴다. 이 키는 저장하지 않는다.
  - Command 선언만 있는 모듈은 표지 헤더만 있으면 되고(빌드가 찾는다 — [리플렉션·빌드 보강](../ReflectionFoundations.ko.md) 1.10절) `<Name>.rocksids.json`이 없어도 된다(RMC2100 면제). 반대로 `rocksids`에 Command struct 항목이 있으면 RMC 오류다.
  - 실행 파일 모듈(Programs 아래)도 리플렉션 헤더를 가질 수 있다(`ModuleDirectory`가 있으면 빌드가 찾는다). 지금 그런 모듈이 없으므로 CL1에서 Rocks.Build 경로를 함께 시험한다.
- **생성 코드.** Command struct마다 `GeneratedCommand<S>`를 낸다(`CppEmitter.cs` 796–815의 `Settings()` 선례와 같은 자리).
  - `Schema()` → `CommandLineSchema`(아래).
  - 필드마다 바인더 함수: 값 텍스트 하나를 그 필드 형식으로 바꿔 넣거나(배열은 원소 추가) 형식 오류를 진단에 남긴다. 함수표 하나로 묶어 스키마 필드 순서로 찾는다.
  - 필드마다 기본값 출력 함수(도움말 `[default: …]`)와 `Range` 검사 함수.
  - `ValidateCommand`가 있으면 그것을 부르는 함수.
  - 생성 헤더는 `CommandLine/CommandLineSchema.h`만 include한다. ObjectRuntime 헤더를 보지 않는다.
  - 모듈 사이 import descriptor 왕복(`Programs/Rocks.MetaCompiler/ModuleDescriptor.cs:31,243–244,676–677`)에 Command 항목을 더한다(공용 옵션 struct를 다른 모듈의 명령이 쓴다).
  - Game 대상의 Release 생성(`ROCKS_BUILD_RELEASE && ROCKS_TARGET_KIND_GAME`)에서는 `Help`·`Examples`·`ValueName` 칸을 빈 값으로 낸다(7.2절). `NotInRelease` 플래그는 모든 구성에서 스키마에 남는다. 받을지는 파서가 정한다(14.2절).

```cpp
// CommandLine/CommandLineSchema.h (생성 코드가 채운다). 모든 view는 생성 코드의 정적 저장소를 빌린다.
struct CommandLineFieldSchema {
    Span<const char> field;      // C++ 필드 이름(진단·시험용)
    Span<const char> option;     // "--max-frames", 위치 인자·Remaining은 빈 값
    Span<const char> aliases;    // "-n" 또는 "-n,--count"
    Span<const char> valueName;
    Span<const char> help;
    Span<const char> range;      // "1..1000000" 또는 빈 값
    Span<const Span<const char>> enumValues; // enum 필드의 도구 이름(이름순), 그 밖은 빈 값
    CommandLineValueKind kind;   // Bool·Int·UInt·Float·String·Guid·Enum
    CommandLineFieldFlags flags; // Argument·Mandatory·Hidden·Secret·NotInRelease·Remaining·Optional·Array
};

struct CommandLineSchema {
    Span<const char> name;       // "trace-report", 공용 옵션은 빈 값
    Span<const char> help;
    Span<const Span<const char>> examples;
    CommandLineSchemaFlags flags; // Global·Group
    Span<const CommandLineFieldSchema> fields;
    const CommandLineBinder* binder; // 생성 바인더 함수표(필드 순서)
};
```

- 문자열 칸은 `SettingsSchema`(`Meta/SettingsSchema.h:55–69`)와 같이 `Span<const char>`다. 필드는 stable ID가 없으므로 `Guid` 대신 스키마 안 순번(배열 index)으로 가리킨다.

### 13.4 콘솔 명령과의 관계

콘솔 명령 인자 struct(`trace.enable channels=cpu,gpu`)는 리플렉션 struct이고 ReflectionText로 대입한다(5.5절). 명령줄 Command struct는 리플렉션 타입이 아니고 생성 바인더로 대입한다. 문법도 다르다. 콘솔은 `name=value`, 명령줄은 `--name value`다. Config D9(콘솔 명령과 commandlet registry는 따로)를 유지한다. 다만 `Help` 지정자는 같은 키이므로, 콘솔 `help <명령>`이 나중에 같은 문자열을 쓸 수 있다.
