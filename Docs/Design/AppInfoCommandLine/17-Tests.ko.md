# 앱 정보와 명령줄 — 17. 시험 계획

[목차](README.ko.md)

## 17. 시험 계획

무작위 사례는 고정 seed이고 타이밍에 기대는 시험은 없다. 스레드 시험의 순서는 명시 gate로 만든다.

- **AppInfo**
  - 만들기의 문자열 복사와 수명(입력 버퍼를 지운 뒤에도 값이 같다), 상한 초과 `CapacityExceeded`(자르지 않는다), 프로젝트 이름 문자 거부
  - `AppInfoScope` 중첩 Fatal과 설치 전 `Get()` assert. 둘 다 FoundationTests process-probe로 자식 프로세스에서 확인하고, 대화상자 없이 끝나야 한다
  - 설치 뒤 여러 스레드 읽기(TSan, macOS CI)
  - 시험용 교체 scope(이전 값 되돌림)
  - 부트 4단계 설정 등록 코드가 `AppInfo::Get().Traits()`를 읽을 수 있는지(8.3절 순서)
- **실행 성격(UE와 같은지)**
  - 10.2절 표의 모든 플래그와 조합. `--deterministic`이 고정 step(1/30)과 고정 시드를 켜는지, `--fps 60`만 주면 고정 step이 꺼진 채 `fixedDeltaSeconds`만 1/60인지, `--fps`와 `--use-fixed-time-step`을 함께 주면 1/60 고정 step인지, `--benchmark`가 `UsesFixedTimeStep()`과 고정 시드를 켜는지
  - Release 규칙(`CommandLineRules::release = true`, Debug에서 시험): `--benchmark`·`--fps`·`--instance-id`·`--session-id`·`--session-name`은 알 수 없는 옵션이고, `--deterministic`·`--fixed-seed`·`--use-fixed-time-step`·`--unattended`·`--headless`는 받는지
  - 시험 실행기 기본 `unattended`
  - unattended에서 Fatal이 대화상자 없이 끝나는지(process-probe). 대화상자 억제를 PlatformBase로 합친 뒤 TestSupport·ShaderToolSpike 경로도 같은 API를 쓰는지
  - Sandbox 자체 시험의 자식이 `--deterministic`으로 고정 step을 받는지
- **FrameTiming**
  - 고정 step에서 `currentSeconds`가 이전 + 고정 delta이고 `accumulatedSeconds`가 delta 합인지(비트 같음)
  - 고정 delta가 double 그대로인지(1/30 비트 비교)
  - 렌더 lane packet의 값이 packet을 만든 프레임 값과 같은지
  - worker가 제출한 프레임의 값을 보는지. 제출 뒤 게임 lane이 다음 프레임으로 넘어가도록 gate를 둔다. sleep을 쓰지 않는다
  - `LatestFrameIndex` 단조 증가
- **빌드 정보**
  - 생성 TU 값(구성·대상·커밋 길이·날짜 형식)
  - git이 없는 환경(빈 값, 빌드 성공), detached HEAD에서 `ROCKS_BUILD_BRANCH` 사용
  - 커밋만 바꾼 두 빌드의 `ToolBuildId` 같음
  - 같은 커밋 두 번 빌드의 BuildStamp 오브젝트 byte 같음(재현성)
  - 대상 종류 정의가 Game·Program 대상에 정확히 하나씩 들어가는지
  - `--version` 출력 golden
- **ProcessCommandLine**
  - Windows `wmain`(콘솔) 실행 파일과 `wWinMain`(GUI subsystem) 실행 파일 각각에 한글·이모지·전각 `－`·`＂`가 든 인자를 `Process::Start`로 넘겨 UTF-8 원문 왕복(best-fit 변환이 없는지). 구현됨: `Native.Launch.unicode-arguments`·`windowed-subsystem`(크래시 처리 LC3)
  - (개정 3에서 뺌) Windows 직접 분해 함수의 표 시험. 직접 분해를 하지 않는다(12절)
  - `ExecutablePath`가 argv[0]과 다르게 띄운 경우에도 실제 경로인지
  - 상한, 두 번 캡처, 잘못된 UTF-16(짝 없는 서로게이트)
  - 로그용 형태에서 `Secret` 값이 `<redacted>`, 파싱 실패 인자가 `<unparsed>`
- **파서(C++)**
  - 14.1절 문법 표의 모든 줄(각 규칙의 성공·실패). `--name:value`·`-abc`·`[directive]` 거부, `--flag false`의 `false`가 위치 인자가 되는지
  - 13.2절 형식 표의 모든 형식: 수 형식 범위 넘침, 꼬리 문자 거부, `nan`·무한대 거부, enum 대소문자 구분과 제안, bool `true`·`false`만, Guid 표기, `Array` 원소 추가(값 안 쉼표와 `\`로 끝나는 경로 보존)
  - 14.2절 검사 순서: 토큰 단계 오류가 있으면 `ValidateCommand`를 부르지 않는지, 필수 → 범위 → 검증 순서의 진단 순서, `ValidateCommand`의 이유 문자열이 그대로 나오는지
  - 14.4절 문구 표의 모든 종류, 이름 제안 문턱 경계(거리 = 문턱, 문턱 + 1)와 동점 규칙, 진단 여러 개의 순서
  - 공용 옵션 위치(명령 앞·뒤), `Range`(양 끝·한쪽·끝 제외 `<`), `Mandatory`
  - 알 수 없는 인자 정책(D29): 도구는 종료 2, `WarnAndIgnore`는 경고 진단과 `<unparsed>` 가림, `Remaining` 필드가 원문 순서로 받는지(`--` 뒤 포함)
  - `--set`·`--trace`·`--config`를 옮긴 뒤의 기존 Config·Trace 시험 통과와 `--set` 값의 출처 `--set` 유지
  - 링크 시험: Core만 링크하는 실행 파일(MathBench)이 `CommandLine` 모듈과 생성 바인더만으로 빌드되는지(ObjectRuntime·ReflectionText 기호 없음)
- **응답 파일**
  - 주석·따옴표·`""`·`\`로 끝나는 경로·빈 줄·닫히지 않은 따옴표, 중첩 깊이 4와 5, 순환, 크기 상한, 못 읽는 파일의 오류 코드 이름, 진단 위치 `파일:줄`
  - C#과 같은 입력 파일로 같은 토큰 열이 나오는지
- **MetaCompiler**
  - 13.3절 RMC 검사 각각의 실패 사례(`Alias`+`Argument`, 위치 인자 순서, `Array` 위치 인자 둘, `Mandatory`+`Optional`, `Range` 형식·순서·기본값이 범위 밖, 이름·별칭 겹침, `Command` 이름 문법, `Help` 누락, 받지 않는 필드 형식, `Remaining` 둘·형식, `Group`에 필드, Command 키를 Command 밖에서 사용, Command struct에 설정·직렬화 지정자, Command struct에 리플렉션 `Validate`)
  - identity 면제: Command만 있는 모듈이 `rocksids` 없이 빌드되는지, `rocksids`에 Command 항목이 있으면 오류인지, 필드를 더해도 `ids add` 없이 빌드되는지
  - 실행 파일 모듈(Programs 아래)의 리플렉션 헤더 탐색 경로
  - descriptor 왕복(다른 모듈의 공용 옵션 struct)
  - 생성 코드: `ROCKS_BUILD_RELEASE && ROCKS_TARGET_KIND_GAME`에서 도움말 문자열이 빈 값이고, Program 대상 Release에서는 남는지(생성 결과 golden)
- **출력 규약 golden(C++)**
  - 대상: 최상위, 명령 상세, 하위 명령 묶음(묶음 설명), `help <명령>`, 오류 출력(14.4절 표의 모든 종류)
  - 너비 60·80·120 각각, 그리고 [40, 200] 자르기
  - 긴 이름 칸 접기, 긴 설명 접기, 열보다 긴 단어, enum 목록, `(REQUIRED)`, `[default: …]`, 숨김 옵션 없음, Release 규칙에서 `NotInRelease` 옵션 없음
  - 줄 끝이 `\n`뿐이고 BOM이 없는지(Windows 포함), 제어 문자 `\xHH` 표기
  - golden 파일은 `Tests/Golden/`에 두고, 갱신은 명시 플래그로만 한다
- **양쪽 같음 golden(CL6)**
  - 같은 시험 명령 묶음(최상위 + 하위 명령 둘 + 공용 옵션 + enum + 필수 + 위치 인자 + `Remaining`)을 C++ 시험 fixture와 C# 시험 프로젝트에 각각 정의한다
  - 같은 입력에서 도움말·오류·`--version` 출력이 byte 단위로 같은지 본다. 프로그램 이름 자리는 시험이 같은 이름을 준다
  - 받는 값 집합이 같은지: 같은 수·bool·enum·Guid 표기 사례 표를 양쪽에 넣어 성공·실패가 같은지 본다
  - C# 시험은 `ko-KR` 문화권으로도 한 번 돌려 출력이 같은지 본다(현지화 리소스·수 형식이 새지 않는지)
  - System.CommandLine 판을 올리면 이 시험이 차이를 잡는다
- **C# 패키지 관리**
  - 오프라인(캐시만) restore에서 빌드가 감사 경고로 실패하지 않는지
  - `Directory.Packages.props`·`packages.lock.json`을 바꾸면 `SourceSnapshot` digest가 바뀌는지
  - 허용 목록 policy: 목록에 없는 PackageReference와 라이선스 기록 없는 패키지 거부, 게임 규칙 참조 어셈블리가 System.CommandLine을 참조하면 거부
- **크기**
  - 7.2절 표의 측정(Release Sandbox·Release MathBench 크기 비교, Game 대상 Release 링크 맵에 `CommandLineTools` 기호 없음)을 단계 완료 보고에 넣는다
