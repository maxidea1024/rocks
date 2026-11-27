# 앱 정보와 명령줄 — 19. 결정 목록

[목차](README.ko.md)

## 19. 결정 목록

권장안으로 확정했다(2026-11-18, 개정 2). **[범위 큼]** 표시는 범위가 커서 착수 전에 사용자가 다시 볼 만한 항목이다.

- **D1 이름과 배치** — `AppInfo`·`RunTraits`·`HostKind`·`ProcessCommandLine`·`FrameTiming`은 Core 런타임 `Core`에 둔다. 빌드 정보는 Core `Build`에, 파서는 새 Core 런타임 모듈 `CommandLine`(의존은 Core만)에, 도움말·`CommandSet`·응답 파일은 새 Core Developer 모듈 `CommandLineTools`(`Core/Source/Developer/`)에 둔다. Engine에는 `FrameTiming` 갱신과 실행 성격 소비만 둔다.
- **D2 수명과 쓰기** — 호스트 부트가 한 번 채우는 불변 레코드다. setter가 없다. 실행 중 바뀌는 값(포커스·볼륨·그래픽 API 이름)은 두지 않는다.
- **D3 접근** — `AppInfoScope`로 프로세스에 설치하고 `AppInfo::Get()`으로 읽는다. 두 번 설치는 Fatal이다(중첩을 허용하는 `LogInstallScope`와 다르다). 설치는 부트 3단계(실행 성격을 정한 직후)다. 인자 전달 방식은 Core 깊은 곳(로그·trace·Fatal)까지 줄줄이 넘겨야 해서 택하지 않았다.
- **D4 프레임 시간 전달 [범위 큼]** — 게임 lane은 `Engine::Timing()`, 렌더 lane은 프레임 packet 사본, worker는 작업 문맥 상속, 그 밖은 `LatestFrameIndex()`와 단조 시계만 읽는다. worker 상속은 CVar 설계의 lane 묶음 장치(Task 모듈, 아직 없음)와 한 장치로 만든다. 프레임마다 게시하는 refcount 묶음에 `FrameTiming`을 넣고 작업에는 묶음 포인터만 넘긴다. 그래서 AP3은 RB1·CVR 작업에 묶인다.
- **D5 `FrameTiming` 칸** — UE `FAppTime` 칸 대응 + `frameIndex`·`averageFrameSeconds`. 고정 step은 `Optional<double>` 하나로 둔다. 단위는 double 초이고 기본 고정 길이는 1/30이다(UE와 같다). 고정 delta의 중간 float 반올림(`UEN` 3019, `LEL` 4730)은 따르지 않는다. timecode 칸은 provider가 생길 때 더한다. `EngineTime`을 대체한다.
- **D6 실행 성격 출처** — 명령줄 + 빌드·호스트 기본값만 쓴다. 설정 파일·CVar로는 정하지 않는다. 보기는 콘솔 `app.info`, 설정 기본값 조정은 등록 코드가 Code 층에 쓴다. BootCVar 방식은 검토하고 제외했다. 봉인 전에 필요하고 출시 빌드 세션 층 제한(Config D5)과 부딪히기 때문이다. 늘 고정 step이어야 하는 게임은 World `TimeStepPolicy`(설정)가 맡는다.
- **D7 실행 성격 목록** — `unattended`·`unattendedInput`·`benchmark`(비출시)·`fixedSeed`·`useFixedTimeStep`·`fixedDeltaSeconds`(기본 1/30)·`headless`. 옵션은 UE와 같은 뜻이다: `--use-fixed-time-step`(UE `-UseFixedTimeStep`), `--deterministic`(고정 step + 고정 시드), `--fixed-seed`, `--fps`(길이만 바꾸고 고정 step을 켜지 않는다, 비출시). 고정 step 계열은 출시 빌드에서도 받는다(UE `WITH_FIXED_TIME_STEP_SUPPORT` 기본 켜짐). 오디오·단일 스레드는 이름만 예약하고, 설치형 여부는 패키징 단계로 미룬다.
- **D8 benchmark 효과** — 고정 step(길이는 `fixedDeltaSeconds`, 기본 1/30), 고정 시드, 포커스 늘 true, 소리 끔(오디오 모듈이 생기면)이다. 출시 빌드에서는 옵션이 없다(UE와 같다).
- **D9 인스턴스·세션** — `instanceId`는 매 실행 무작위 Guid다. `--instance-id`·`--session-id`·`--session-name`은 비출시에서만 받는다(UE `InitializeSession`이 비출시만). `sessionId`가 없으면 단독 실행이다. 세션 소유자·사용자 허가 목록은 두지 않는다.
- **D10 빌드 정보 칸** — 구성·대상 종류·대상 이름·프로젝트 이름·브랜치·커밋·dirty·커밋 시각·빌드 URL·디버그 정보. Horde 실행 job URL·승격 빌드·제품 식별자는 두지 않는다. branch는 `ROCKS_BUILD_BRANCH`로 덮어쓸 수 있다(CI detached HEAD).
- **D11 빌드 날짜** — 빌드 시각(`__DATE__`) 대신 커밋 시각을 쓴다. 재현 빌드를 위해서다. UE와 뜻이 다르다(UE는 컴파일 날짜). 시간 단위·좌표 규칙과 무관한 진단 문자열이라 UE와 다르게 둔다.
- **D12 BuildStamp와 캐시 키** — 별도 생성 TU에 둔다. `ToolBuildId` 입력·쿠킹 byte·에셋 너겟 키에는 넣지 않는다. 커밋이 바뀌면 모든 실행 파일이 다시 링크되는 비용은 AP2에서 측정한다.
- **D13 그래픽 API 이름** — AppInfo에 두지 않는다. Graphics 장치 정보가 공개하고 시작 로그가 남긴다.
- **D14 시작 로그** — LogHost 설치 직후 `App` 카테고리 Info 기록 한 묶음을 남긴다. 내용은 실행 파일, 엔진 버전, 구성, 대상, 커밋, 브랜치, 플랫폼, PID, instance id, 가린 명령줄이다. 문구는 영어다.
- **D15 프로세스 명령줄** — 진입점에서 한 번 캡처하는 불변 UTF-8 배열이다. Windows는 Launch가 만드는 `wmain`·`wWinMain`(CRT `__wargv`)만 쓰고 `main(char**)`는 지원하지 않는다(개정 3, 크래시 처리 설계 D7. `GetCommandLineW` 직접 분해는 하지 않는다). 실행 파일 경로는 OS에서 얻는다(`GetModuleFileNameW`). `activeCodePage` UTF-8 manifest는 보조 수단으로만 검토한다. UE식 `Set`·`Append`가 없다. 하위 프로세스 상속(`Inherit`)은 자식을 띄우는 도구가 생길 때 더한다. 환경 변수 인자(UE `UE-CmdLineArgs`)는 두지 않는다.
- **D16 비밀 가리기** — `Secret` 지정자 옵션의 값을 `<redacted>`로, 파싱하지 못하거나 무시한 인자를 `<unparsed>`로 바꾼다. 로그·trace·크래시 보고에는 가린 형태만 쓴다.
- **D17 C++ 파서 — 자체 구현(확정)** — 외부 라이브러리는 검토하고 제외했다. 이유는 크기다(사용자 결정 2026-11-18).
- **D18 선언 방식** — `ROCKS_STRUCT(Command(...))`·`ROCKS_PROPERTY` 지정자 문법으로 선언하지만, Command struct는 리플렉션 등록 타입이 아니다. 생성물은 명령줄 스키마와 바인더뿐이다. 도움말 문자열은 `Help=` 지정자에서 온다. 문서 주석에서 가져오지 않는다(주석은 한국어, 도움말은 영어). 필수 표시는 기존 `Required`와 겹치지 않게 `Mandatory`로 한다. 출시 빌드 제외는 설정 지정자 `DevelopmentOnly`가 아니라 Command 전용 `NotInRelease`로 한다. 판정은 런타임 규칙(`CommandLineRules::release`)이라 Debug에서 시험한다. Command 키는 Command struct 밖에서 쓰면 RMC 오류다. 묶음 설명은 `Group` struct로 선언한다.
- **D19 값 변환** — MetaCompiler가 Command struct마다 필드별 바인더를 생성한다. 변환은 Core `NumberText`·Base `Guid` 텍스트 변환·생성 enum 표만 쓰고, ReflectionText·TypeRegistry·ObjectRegistry에 기대지 않는다. enum은 도구 이름 정확 일치(대소문자 구분), bool은 `true`·`false`만, 반복 필드는 원소마다 추가한다. 검사 순서는 토큰·변환 → 필수 → 범위 → `ValidateCommand`이고, 앞 단계에 오류가 있으면 `ValidateCommand`를 부르지 않는다. `ValidateCommand`는 `CommandValidation::Fail`로 영어 이유 문자열을 남긴다.
- **D20 공통 문법 규약** — `--kebab-case`, 한 글자 별칭, `--name value`·`--name=value`(`:` 구분자 없음), 값 없는 bool(다음 토큰을 먹지 않음), 반복 = 원소 추가(쉼표로 나누지 않음), 스칼라 중복 오류, `--`, 묶음·`/x`·지시자 없음, 대소문자 구분, 공용 옵션은 어디서나 받는다.
- **D21 도움말 출력 규약** — "Rocks 명령줄 출력 규약"을 이 문서가 정의한다(14.4·14.5절): UTF-8(BOM 없음), 줄 끝 `\n`, 영어, 너비(터미널 열 수 또는 80, [40, 200])와 열 규칙, Description·Usage·Arguments·Options·Commands·Examples 절, `[default: …]`, `(REQUIRED)`. C++과 C#이 각자 이 규약대로 렌더한다. System.CommandLine 기본 도움말은 쓰지 않는다. Game 대상 Release는 도움말 문자열이 없다. stdout, 종료 0.
- **D22 오류 출력과 종료 코드** — 오류 문구는 14.4절 표 하나에서만 만든다(System.CommandLine 문구를 쓰지 않는다). stderr에 `error: ` 접두로 쓰고, 짧은 사용법 한 줄과 `--help` 안내 한 줄을 붙인다. 종료 코드는 0 성공, 1 실행 실패, 2 사용법 오류다. 이름 제안은 Core `EditDistance`(CVar 12절과 같은 함수)를 쓰고, 명령줄 문턱은 이름 붙은 상수 `MinCommandLineSuggestionDistance`(1)·`CommandLineSuggestionLengthDivisor`(3)로 정한다. ShaderToolSpike의 64와 벤치의 1·stdout을 이 규약으로 고친다.
- **D23 응답 파일** — 도구·commandlet만 `@file`을 받는다. 문법은 Rocks가 정한다(14.7절 — escape는 `""`뿐이고 `\`는 글자 그대로). C#도 System.CommandLine 처리를 끄고 같은 문법으로 펼친다. 크기·깊이 상한을 둔다. 게임 호스트는 받지 않는다.
- **D24 commandlet 구조** — `CommandletRegistry`를 Developer `CommandSet`으로 대체한다(하위호환 없음). 명령은 옵션 struct + 실행 함수다. 도움말 목록과 개별 도움말은 등록만으로 생긴다. `--list`는 없앤다. Config의 `RegisterConfigDumpCommandlet`은 `Programs/Commandlet`으로 옮긴다. ShaderSystem·AssetSystem 문서는 CL7에서 고친다.
- **D25 `--set`·`--trace`·`--config`** — 호스트 공용 옵션 struct로 옮겨 같은 파서로 한 번만 읽는다. 값 의미(Config 대입, trace 목록)와 출시 빌드 제한은 그대로다. commandlet에서도 받는다.
- **D26 C# 파서 [범위 큼]** — System.CommandLine을 파싱에만 쓴다(저장소 첫 외부 NuGet 패키지). 출력은 새 어셈블리 `Rocks.Tooling.Cli`의 `RocksCli`가 그린다. 파서 설정은 16.2절 표(InvariantCulture, 위성 리소스 en, 묶음·`:`·bool 값 소비·지시자·자체 응답 파일 끔, 사용자 값 파서)다. 중앙 버전 관리·lock 파일·`NuGetAudit=false`(감사는 CI 별도 작업)·`BuildEvidence` 루트 목록 추가·허용 목록 policy·`Programs/ThirdPartyPackages.json` 라이선스 기록을 쓴다. 설치 SDK에는 두 DLL을 싣고, 게임 규칙이 참조하는 어셈블리는 패키지에 의존하지 않는다. 자체 C# 파서는 택하지 않았다.
- **D27 코드 크기** — 7.2절 상한을 둔다. 도움말 문자열과 도움말 writer를 빼는 기준은 Game 대상의 Release다(`ROCKS_TARGET_KIND_GAME`). Program 대상은 Release에서도 도움말을 싣는다.
- **D28 단계 기호** — AP(앱 정보)·CL(명령줄).
- **D29 알 수 없는 인자 [새 결정]** — 도구·commandlet·Debug·Development 게임 호스트는 오류(종료 2)다. Game 대상 Release 호스트는 경고로 남기고 무시한다. 경고에는 옵션 이름만 쓰고 값과 위치 토큰은 `<unparsed>`로 가린다. 명령은 `Remaining` 필드(`Array<String>`, 하나)로 남은 인자를 원문 순서대로 받을 수 있다.
- **D30 Command struct identity [새 결정]** — Command struct와 명령줄 enum은 직렬화하지 않으므로 identity 면제다. stable ID가 없고, `rocksids` 항목과 `ids add`가 필요 없다. 모듈 안 판정과 descriptor 왕복은 정규 이름에서 만든 키를 쓴다. Command만 있는 모듈은 `rocksids` 파일 없이 `Metadata`만 선언한다. 그래서 리플렉션을 쓰지 않던 낮은 계층 도구도 ObjectRuntime 의존 없이 옮길 수 있다.
