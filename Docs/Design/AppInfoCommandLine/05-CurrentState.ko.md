# 앱 정보와 명령줄 — 5. Rocks 현황

[목차](README.ko.md)

## 5. Rocks 현황

### 5.1 앱 정보 쪽

| 항목 | 현황 | 근거 |
|---|---|---|
| 프로젝트 이름·디렉터리 | `HostSettingsOptions`(`mode`·`engineDir`·`projectDir`·`projectName`·`userDataDir`·`savedDir`·등록 콜백 둘)가 있다. `engineDir`만 호스트가 채운다(`FindEngineDirectory`). 나머지는 채우는 호스트가 없다 | `HostSettings.h:30–43`, Commandlet `Programs/Commandlet/Private/Common/CommandletHost.cpp:166–175`, Sandbox `EngineHost.cpp:182–186` |
| 부트 순서 | 등록 → 층 파일 → `--config` → `--set` → `CompleteStartup` → LogHost → trace·`--trace` → 엔진 루프 | [설정 계층](../ConfigSystem.ko.md) 5.4절, `EngineHost.cpp:181–236` |
| 프레임 시간 | `EngineTime{frameIndex, deltaSeconds, realTimeSeconds, averageFrameSeconds}`, 모두 double 초. `RunFrame` 2단계에서 단조 시계 차로 계산한다. `EngineConfig::fixedFrameDeltaSeconds > 0`이면 그 값을 쓴다 | `Engine/Source/Runtime/Engine/Public/Engine/Engine.h:172–173,209–216`, `Engine/Source/Runtime/Engine/Private/Engine.cpp:452–463` |
| 프레임 번호 공유 | `Log::SetFrame`이 LogSystem의 `frame_`을 relaxed 원자 값으로 둔다 | `Core/Source/Runtime/Core/Private/Diagnostics/LogSystem.cpp:1006–1007` |
| World 시간 | World마다 `GameClock`(pause·배율·고정 step) | [엔진 런타임](../EngineRuntime/05-Time.ko.md) 5절 |
| 빌드 정보 | 구성·플랫폼 매크로, 로그 하한 매크로, 생성 TU의 `ToolchainFingerprint`·`ToolBuildId`·`CurrentEngineVersion`. 대상 종류·브랜치·커밋·날짜는 없다 | `Programs/Rocks.Build/BuildInfo.cs:298–370,596–616`, `ActionGraph.cs:376–486` |
| 실행 성격 | `Unattended`·`nullrhi`·`FixedSeed`가 코드에 없다. 실행 모드 구분은 `LogSystemConfig::ForTool()`(Sync)과 `ForEngineHost()`(Async)뿐이다 | `Core/Source/Runtime/Core/Public/Core/Diagnostics/LogSystem.h:104–112` |
| 대화상자 억제 | PlatformBase 공용 API가 없다. 같은 Windows 억제 코드(`_set_abort_behavior`·`_CrtSetReportMode`·`SetErrorMode`)가 두 곳에 복사돼 있다 | `Core/Source/Developer/TestSupport/Private/Win64/HostFailure.cpp:19–34`, `Programs/ShaderToolSpike/Private/Win64/SpikeHostWin64.cpp:13–18` |
| 프레임 값 상속 | Task 모듈에 작업 문맥 상속이 없다. CVar 설계가 "worker는 Task 생성 때 만든 lane의 묶음을 넘긴다"를 계획으로 적었다 | `Core/Source/Runtime/Task/Public/Task/WorkerScheduler.h:31`, [CVar](../CVarSystem.ko.md) 10.1·16절 |

### 5.2 C++ 명령줄 도구

공용 명령줄 파서가 없다. 공용 구조는 두 가지다. `CommandletRegistry`는 하위 명령 선택과 `--help`·`--list`만 맡는다. 나머지는 인자 배열 전체에서 자기 접두사만 골라 쓰는 필터 셋(`--set=` 둘, `--trace=` 하나)이다.

| 도구 | 하위 명령 | 값 형식 | 반복 | 필수 검사 | `--help` | 알 수 없는 인자 | `@file` | 파서 위치 |
|---|---|---|---|---|---|---|---|---|
| RocksCommandlet 진입 | 있음(registry) | — | — | 인자 128개·4096 B·UTF-8 | 명령 목록만("이름<TAB>설명"), 명령별 도움말 없음 | "Unknown command. Use --list." 종료 2 | 없음 | `Programs/Commandlet/Private/Win64/Main.cpp:58–82`, `Core/Source/Runtime/Core/Private/Commandlet/Commandlet.cpp:97–165` |
| `core-check` | 없음 | — | — | 인자 0개 | 없음 | 종료 2 | 없음 | `CommandletHost.cpp:17–21` |
| `trace-report` | 없음 | 위치 `<in> [<out>]` | — | 개수 1~2 | 사용법 문자열 | 종료 2 | 없음 | `CommandletHost.cpp:58–63` |
| `log` | `keygen`·`decode`·`seal` | `--key <f>`·`--public <f>`를 **고정 위치**로 비교, `--json` | 없음 | 개수 비교 | 있음(손으로 쓴 문자열) | Usage 전체를 stderr, 종료 2 | 없음 | `Programs/Commandlet/Private/Common/LogCommandlet.cpp:238,313,366–389` |
| `config-dump` | 없음 | — | — | 인자 0개 | 없음 | 종료 2 | 없음 | `Core/Source/Runtime/Config/Private/ConfigDump.cpp:97–108` |
| Sandbox | 사실상 모드 둘 | `--frames N`·`--config <f>`(공백), `--set=`·`--trace=`(`=` 결합) | `--set`·`--trace` 64개까지 | `--frames` 1 이상 | 없음 | 한 줄 Usage, 종료 2 | 없음 | `Programs/Sandbox/Private/Main.cpp:24–105` |
| ShaderToolSpike | 없음 | `--out <d>`(필수), `--sm 6_x` | 없음 | 있음 | 없음 | Usage, **종료 64** | 없음 | `Programs/ShaderToolSpike/Private/Common/Main.cpp:33–52` |
| CompressionBench | 없음 | 값 다섯 개 | 없음 | 범위. **숫자 뒤 문자를 받아들인다**(`--size 12abc` → 12) | 없음 | usage를 **stdout**, 종료 1 | 없음 | `Programs/CompressionBench/Private/Main.cpp:562–602` |
| CryptoBench·MathBench | 없음 | 값 둘 | 없음 | `ParseNumber`(두 파일이 글자까지 같은 복사본) | 없음 | stdout, 종료 1 | 없음 | `Programs/CryptoBench/Private/Main.cpp:58–71`, `Programs/MathBench/Private/Main.cpp:91–104` |
| LogBench | 없음 | 위치 `[samples]` | — | 범위 | 없음 | stdout, 종료 2 | 없음 | `Programs/LogBench/Private/Main.cpp:129–145` |
| FoundationTests | `--process-probe <mode> ...`(mode 뒤 인자는 mode마다 자유 형식) | 값 다섯 개(`strcmp`) | 없음 | `ParseUnsigned` | 없음 | stderr, 종료 2 | 없음 | `Programs/FoundationTests/Private/Main.cpp:115–385` |

엔진 런타임에서 명령줄을 읽는 곳은 셋이다. Config `AddCommandLineAssignments`(`Core/Source/Runtime/Config/Private/ConfigLoader.cpp:222–252`), HostSettings `ApplyCommandLine`(`Core/Source/Runtime/HostConfig/Private/HostSettings.cpp:216–241` — `--set=`를 **한 번 더 훑는다**), TraceController `ApplyCommandLine`(`Core/Source/Runtime/TraceControl/Private/TraceController.cpp:248–272`)이다. Commandlet 호스트는 `ApplyCommandLine`을 부르지 않아 `--set`·`--trace`를 쓸 수 없다(`CommandletHost.cpp:166–175`).

**프로세스 명령줄 보관이 없다.** PlatformBase에 `GetCommandLine` 류가 없다. 진입점마다 처리가 다르다.
- RocksCommandlet은 `wmain`에서 UTF-16을 UTF-8로 바꾼다.
- Sandbox·ShaderToolSpike·벤치·FoundationTests는 `main(int, char**)`이다(RocksCommandlet의 Mac 진입점도 같다). Windows에서 이 argv는 시스템 ANSI 코드 페이지 문자열이다. Rocks.Build는 컴파일 인자 `/utf-8`과 기본 manifest(`/MANIFEST:EMBED`, `ActionGraph.cs:310,429`)만 주고 `activeCodePage` 설정은 없다.
  - **비 ASCII 인자는 깨진다.** Rocks는 argv를 UTF-8로 취급한다. 한국어 로캘에서도 CP949 바이트는 잘못된 UTF-8이므로, ACP가 UTF-8이 아닌 한 모든 비 ASCII 인자가 깨진다. 코드 페이지에 없는 글자(이모지 등)는 CRT 변환에서 이미 사라진다.
  - **보안 문제.** CRT의 넓은 문자 → ANSI 변환은 best-fit 대응을 쓴다. 전각 `－`(U+FF0D)·`＂`(U+FF02) 같은 글자가 ASCII `-`·`"`로 바뀌어, 사용자가 값으로 준 글자가 옵션이나 따옴표로 해석될 수 있다.

### 5.3 C# 도구

외부 파서 패키지가 없다. 모든 csproj·props에 `PackageReference`가 0건이고 `Directory.Packages.props`도 없다. **`--help`를 지원하는 C# 도구는 0개다.**

| 도구 | 하위 명령 | 값 형식 | 반복 | 필수 검사 | `--help` | 알 수 없는 인자 | `@file` | 파서 위치 |
|---|---|---|---|---|---|---|---|---|
| Rocks.Build.Cli | `build`·`test`·`sdk`·`game`·`bootstrap` | `--key value`만. `--offline`도 `true`/`false` 값이 있어야 한다 | 중복 거부 | 명령별 허용 키·필수 키 | 없음 | `ROCKS-BUILD-ERROR: Unknown, duplicate, or incomplete option: X` 종료 2 | 없음 | `Programs/Rocks.Build.Cli/Program.cs:35–66,382–395` |
| Rocks.MetaCompiler | `ids` 분기 | `--key value`·`--key=value` 둘 다 | 6종 반복 | 필수 셋 + 경로 검증 | 없음 | `error RMC1000: unknown option X` + `hint:` 종료 1 | 없음 | `Programs/Rocks.MetaCompiler/Program.cs:7–37`, `CompilerOptions.cs:45–260` |
| MetaCompiler `ids` | 7개 | `--key value` | 있음 | `One()` | 없음 | "Unknown ids option --X" | 없음 | `Programs/Rocks.MetaCompiler/IdentityCommands.cs:7–227` |
| Rocks.ToolTests | 12개 | 둘 다 | `--clang-arg`만 | `Required()`·`Optional()` | 없음 | "Unknown argument: X" 등, 종료 2 | 없음 | `Programs/Rocks.ToolTests/Program.cs:9–145`, `TestSupport.cs:278–325` |
| ToolTests `scaffold` | `plan`·`apply`·`validate` | `--key value` | 없음 | `Get()` | 없음 | "Invalid scaffold option" | 없음 | `Programs/Rocks.ToolTests/Program.cs:147–197` |

### 5.4 중복 패턴

| 패턴 | 횟수 | 곳 |
|---|---|---|
| C++ `for (index = 1; index < argc)` + 문자열 비교 + `index + 1 < argc` 값 확인 | 6 | Sandbox, ShaderToolSpike, CompressionBench, CryptoBench, MathBench, FoundationTests |
| C++ 10진수 손 파싱 | 함수 4 + 인라인 4 | Sandbox `ParseFrames`, CryptoBench·MathBench `ParseNumber`, FoundationTests `ParseUnsigned`, CompressionBench 인라인(꼬리 문자 허용 결함), LogBench |
| C++ 접두사 필터 | 4 | Sandbox(`--set=`·`--trace=`), HostSettings·ConfigLoader(`--set=` 두 번), TraceController(`--trace=`) |
| 손으로 쓴 usage 문자열 + 종료 코드 불일치(1·2·64) + 출력 대상 불일치(stdout·stderr) | 8 도구 | 5.2절 표 |
| C# 키-값 쌍 루프(`index += 2`) | 4 | Build.Cli, MetaCompiler ids, ToolTests scaffold, IntegrationTests |
| C# `--key`·`--key=value` 겸용 루프 | 2 | MetaCompiler, ToolTests |
| C# 하위 명령 `switch (args[0])` + "Unknown command" | 4 | Build.Cli, MetaCompiler ids, ToolTests, scaffold |

### 5.5 리플렉션 기반

옵션 struct 선언에 쓸 기반을 확인했다. 지정자 파서는 `Programs/Rocks.MetaCompiler/Specifiers.cs`이고 표에 없는 키는 빌드 오류다. 개정 2에서 바인딩을 생성 바인더로 바꾼 이유(아래 표의 "제약" 행들)도 여기에 모았다.

| 필요한 것 | 지금 | 근거 |
|---|---|---|
| struct 선언 | `ROCKS_STRUCT(...)`·`ROCKS_PROPERTY(...)`. base 없는 비템플릿 struct이고 `S() noexcept` 또는 `explicit S(Allocator&) noexcept`가 필요하다 | `Core/Source/Runtime/MetaSchema/Public/Meta/Annotations.h:17,42–61,76` |
| struct 수준 묶음 지정자 | `Settings(Branch=…, Section=…)`가 선례다. 생성 코드는 `GeneratedType<S>::Settings()` → `SettingsSchema`다 | `Specifiers.cs:168–193`, `Core/Source/Runtime/MetaSchema/Public/Meta/SettingsSchema.h:56–69`, `Programs/Rocks.MetaCompiler/CppEmitter.cs:796–815` |
| 필드 이름 | `Name=`(도구 이름, ASCII 식별자)·`EffectiveToolName` | `Specifiers.cs:204–259`, `Core/Source/Runtime/MetaSchema/Public/Meta/TypeSchema.h:864–890` |
| 설명·별칭·위치 인자·예시·비밀·범위 | **없다.** `Tooltip`·`Description`·`ClampMin` 같은 키가 없고 문서 주석도 가져가지 않는다. `Required`는 "참조 non-null" 뜻이라 명령줄 필수와 다르다 | `Specifiers.cs`, `TypeSchema.h:60–68`, `SchemaValidator.cs:650` |
| 기본값 | 멤버 초기화식을 읽지 않는다. 기본값은 생성자를 실행한 결과다 | [런타임 클래스 메타데이터](../RuntimeClassMetadata.ko.md) 47–76행 |
| 값 변환 | ReflectionText `ParseTextScalar`·`ParseTextValue`·`FormatTextValue`·`PrepareAssignments`. 명령행 대입을 전제로 설계됐다(`TextSourceKind::Assignment`) | `Core/Source/Runtime/ReflectionText/Public/ReflectionText/TextValue.h:268–302`, `TextAssignments.h:66,84–86,288` |
| struct 사이 검증 | `static Result<void> Validate(const S&) noexcept` hook을 MetaCompiler가 인식한다(CV3) | `Programs/Rocks.MetaCompiler/ClangAstParser.cs:269–300`, `CppEmitter.cs:788–790` |
| 인자 struct 선례 | 콘솔 명령 인자 struct `TraceChannelArguments`와 `ConfigContext::RegisterConsoleCommand<Args>` | `Core/Source/Runtime/TraceControl/Public/TraceControl/TraceSettings.h:52–69`, `Core/Source/Runtime/Config/Public/Config/ConfigContext.h:280–294` |
| 출시 빌드 메타 문자열 제외 | 조건이 없다. category·displayName·toolName이 늘 실린다 | `CppEmitter.cs:877–891` |
| 오류 처리 | `Result<T>`·`ErrorCode`. 예외·RTTI는 빌드 플래그로 금지한다(`/EHs-c-`·`/GR-`, `-fno-exceptions`·`-fno-rtti`) | `Core/Source/Runtime/Core/Public/Core/Result.h:12`, `Error.h:10–35`, `ActionGraph.cs:439–440,472` |
| 제약: 반복 대입 | ReflectionText 대입 표기는 `<경로>=<값>`·Unset·Erase뿐이고 원소 추가 표기가 없다(`$append`는 Config JSON 층 전용). 같은 경로 두 번은 `DuplicateAssignment`다. 목록 텍스트는 `,;`만 escape하고 그 밖의 `\`는 글자 그대로라, `\`로 끝나는 원소(`C:\dir\`)를 이어 붙이면 다음 쉼표와 합쳐진다 | `ReflectionText/TextAssignments.h:40–46,265`, `TextValue.h:20–24`, [설정 계층](../ConfigSystem.ko.md) 104행 |
| 제약: Validate 이유 | struct `Validate` hook은 `Result<void>`만 돌려준다. 대입 묶음은 `Publish` 안에서 hook을 부르고 고정 문구 "the value failed its struct validation hook"과 InvalidArgument로 바꾼다. 사람에게 보일 이유 문자열이 없다 | `ClangAstParser.cs:268–271`, `ReflectionText/Private/TextAssignments.cpp:884–896`, `Object/ValueCopy.h:333` |
| 제약: enum·bool 표기 | ReflectionText enum 입력은 대소문자 무시·접두 일치(`j` → `json`), bool은 on·off도 받는다. 일반 enum 도구 이름은 `[A-Za-z0-9_]+`라 `-`를 쓸 수 없다 | `TextValue.h:12–14`, `Specifiers.cs:323` |
| 제약: 설정 지정자 | `DevelopmentOnly`는 설정 지정자다(`HasSettings`). Settings struct 밖에서 쓰면 RMC2210이다. CVar VD5·Config 개정 4가 `Cheat`로 바꾸기로 했다 | `Specifiers.cs:71`, `SchemaValidator.cs:1059–1066`, [CVar](../CVarSystem.ko.md) VD5 |
| 제약: identity | Metadata가 있는 모듈은 `<Name>.rocksids.json`이 필요하고(RMC2100), 새 선언은 `ids add`로 등록해야 한다. 일반 빌드는 manifest를 바꾸지 않는다. 지금 Programs 아래 실행 모듈에는 Metadata가 하나도 없고 HostConfig에도 없다 | `ClangAstParser.cs:566–582`, [리플렉션 기반](../ReflectionFoundations.ko.md) 33행 |
| 제약: 계층 | MathBench·LogBench는 Core만, CryptoBench는 Crypto, CompressionBench는 Compression, ShaderToolSpike는 ShaderToolLoader만 링크한다. FoundationTests Core 계층은 ReflectionText를 일부러 빼고 링크한다 | 각 `Programs/*/*.Build.cs`, `Programs/FoundationTests/FoundationTests.Build.cs:63` |
| 수 변환 | Core `NumberText`의 `TryParseInt64`·`TryParseUInt64`·`TryParseDouble` 등. 전체 소비, 꼬리 문자 거부, 범위 넘침 `Overflow` | `Core/Source/Runtime/Core/Public/Core/Text/NumberText.h:28–41` |
| Guid 텍스트 | Base `Guid`에 텍스트 변환이 없다. 36자 소문자 표기는 ReflectionText 안에만 있다 | `Core/Source/Runtime/Base/Public/Base/Guid.h` |
