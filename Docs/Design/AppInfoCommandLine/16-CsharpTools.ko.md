# 앱 정보와 명령줄 — 16. C# 도구

[목차](README.ko.md)

## 16. C# 도구

### 16.1 파서 선택(D26)

| 항목 | System.CommandLine | 자체 작은 라이브러리 |
|---|---|---|
| 기능 | 선언형 옵션·인자·하위 명령, 자동 도움말, 형식 변환, 응답 파일, 이름 제안, 탭 완성 지시자 | 모두 새로 만든다 |
| 안정성 | 안정판 있다. NuGet 기준 3.0.0(2026-11-10), 2.0.x 줄(2.0.15)도 유지된다(판 번호·날짜는 미확인 — 20절) | 우리 책임 |
| 라이선스 | MIT | — |
| AOT·트리밍 | NuGet 페이지가 "trimming과 AOT 완전 지원"으로 적는다. 2.0부터 리플렉션 바인딩을 뺐다 | — |
| 의존 | net10.0 대상은 의존 없음 | 없음 |
| 이전 비용 | 도구 셋(Build.Cli·MetaCompiler·ToolTests)과 그 안의 하위 명령 묶음 둘(MetaCompiler `ids`·ToolTests `scaffold`)의 손 루프를 명령 정의로 바꾼다. 손 루프 코드가 줄어든다 | 같은 이전 + 라이브러리 작성 |
| 단점 | 저장소 첫 외부 NuGet 패키지다. restore가 필요하다(오프라인 빌드는 NuGet 캐시·사내 feed). 공급망 관리(버전 고정·lock 파일)가 생긴다. 메이저 판 사이 API 변화가 있었다(2.0 beta 동안 큰 변경) | 기능 차이를 계속 메워야 한다 |

**결정: System.CommandLine을 파싱에만 쓴다.** 사용자가 외부 패키지를 허용했다(2026-11-18). 선언형 명령 모델·하위 명령·형식 변환·필수·arity는 이 패키지를 쓴다. 도움말·오류·응답 파일·이름 제안은 출력 규약을 지키려고 `RocksCli`가 맡는다(16.2절).

들여올 때 지킬 것:
- **중앙 버전 관리.** 저장소 루트 `Directory.Packages.props`(Central Package Management)에 버전 하나를 고정한다. 착수 때의 최신 안정판을 쓴다.
- **lock 파일.** `packages.lock.json`과 `RestoreLockedMode`(CI)를 켜 공급망을 고정한다.
- **NuGet 감사와 경고 처리.** `Programs/Directory.Build.props`는 `TreatWarningsAsErrors`·`MSBuildTreatWarningsAsErrors`를 켠다. 그대로 두면 NuGet 감사 경고가 빌드 오류가 된다. 오프라인이면 NU1900이 나고, 취약점이 새로 공개되면 NU1901–NU1904가 난다. 같은 소스의 빌드 결과가 시간·네트워크에 따라 달라진다. 그래서 `NuGetAudit=false`로 두고, 감사는 CI의 별도 작업(`dotnet list package --vulnerable`)으로 돌린다. 감사 결과는 빌드를 막지 않고 보고로 남긴다.
- **빌드 증거.** `SourceSnapshot`의 루트 파일 목록(`Programs/Rocks.Tooling.Common/BuildEvidence.cs:28–29`)에 `Directory.Packages.props`와 `packages.lock.json`을 더한다. 빼면 패키지 판을 바꿔도 증거 digest가 그대로다.
- **policy.** 지금 policy에 PackageReference를 막거나 0건을 전제한 검사는 없다(`git grep` 확인, 2026-11-18). 대신 허용 목록 검사를 새로 둔다. PackageReference는 `Directory.Packages.props`에 있는 패키지만 쓸 수 있고, 각 패키지는 라이선스 기록이 있어야 한다.
- **라이선스 기록 자리.** native 외부 코드는 `ThirdParty.json`이 맡지만 NuGet 패키지는 대상이 아니다. `Programs/ThirdPartyPackages.json`(패키지 이름·판·SPDX 라이선스·출처 URL·도입 결정 문서)을 두고 위 policy 검사가 읽는다. 설치 SDK에 실을 때는 이 목록에서 고지 파일을 만든다.
- **Rocks.Build의 offline bootstrap**(`ThirdPartyBootstrapCommand.cs`)과 NuGet 캐시 관계를 정한다. 빌드 도구 자신(Rocks.Build.Cli)이 패키지를 쓰므로 첫 restore는 네트워크가 필요하다. 오프라인 기계는 NuGet 캐시나 사내 feed를 쓴다.
- **설치 SDK.** `Rocks.Build.Cli`가 `Rocks.Tooling.Cli`(그리고 System.CommandLine.dll)를 쓰므로 SDK의 `Tools/Rocks.Build.Cli`에 두 DLL을 함께 싣는다. 게임 규칙 프로젝트가 참조하는 `Rocks.Build.Abstractions`·`Rocks.Build`·`Rocks.Tooling.Common`(`InstalledSdk.cs:293–294`)은 System.CommandLine에 의존하지 않아야 한다. 게임 규칙 컴파일이 패키지 restore 없이 끝나야 하기 때문이다. 이 의존 방향을 policy 검사(어셈블리 참조 확인)로 막는다.

### 16.2 `RocksCli`(새 어셈블리 Rocks.Tooling.Cli)

System.CommandLine은 토큰을 명령 모델에 맞춰 나누고 값을 변환하는 데만 쓴다. 출력은 모두 `RocksCli`가 출력 규약(14.4·14.5절)대로 그린다. 그래서 System.CommandLine 판이 바뀌어도 출력 byte는 바뀌지 않는다.

**파서 설정 범위**(C++ 문법 규칙 14.1절과 같은 입력 집합을 받도록):

| 설정 | 값 | 이유 |
|---|---|---|
| 문화권 | 진입 때 `CultureInfo.DefaultThreadCurrentCulture`·`DefaultThreadCurrentUICulture`와 현재 스레드의 두 값을 `InvariantCulture`로 둔다 | System.CommandLine 리소스 문구와 수 형식이 OS 언어를 따르지 않게 한다 |
| 위성 리소스 | 도구 csproj에 `<SatelliteResourceLanguages>en</SatelliteResourceLanguages>` | ko 등 현지화 리소스 DLL을 출력에 싣지 않는다 |
| POSIX 묶음 | 끈다(`-abc`를 받지 않는다) | 14.1절 |
| `:` 구분자 | `--name:value`를 받지 않는다. 설정으로 끌 수 없는 판이면 토큰을 넘기기 전에 `RocksCli`가 `--name:` 형태를 알 수 없는 옵션으로 거부한다 | 구분자는 `=` 하나 |
| bool 값 소비 | bool 옵션의 arity를 0으로 둔다(`--flag false`의 `false`를 먹지 않는다). `--flag=true`·`--flag=false`는 `RocksCli`가 `=` 형태만 따로 받는다 | 14.1절 |
| 지시자 `[...]` | 루트 명령의 지시자를 모두 뺀다 | 14.1절 |
| 응답 파일 | System.CommandLine의 응답 파일 처리를 끄고, `RocksCli`가 14.7절 문법으로 먼저 펼친다 | 양쪽 같은 토큰 열 |
| 알 수 없는 토큰 | `TreatUnmatchedTokensAsErrors`는 명령에 `Remaining`이 있을 때만 끄고, 남은 토큰은 `ParseResult.UnmatchedTokens`에서 원문 순서로 꺼낸다 | D29 |
| 값 변환 | 수·bool·enum·Guid 옵션에 사용자 파서를 붙인다(`NumberText`와 같은 문법, enum 도구 이름 정확 일치). 기본 `Enum.TryParse`(대소문자 무시·숫자 허용)·`int.Parse`(문화권·앞뒤 공백 허용)를 쓰지 않는다 | 13.2절 표 |
| 기본 도움말·오류 action | 쓰지 않는다. `--help`·`-h`·`-?`·`--version` action과 파싱 오류 처리를 `RocksCli`의 것으로 바꾼다 | 14.4·14.5절 |

**일**

| 일 | 방법 |
|---|---|
| 출력 writer | stdout·stderr를 `UTF8Encoding(false)`, `NewLine = "\n"`인 writer로 감싼다 |
| 오류 출력·종료 코드 | `ParseResult.Errors`의 종류(알 수 없는 토큰·값 없음·변환 실패·필수 누락·명령 없음·중복)를 14.4절 표의 문구로 다시 만든다. System.CommandLine 문구는 쓰지 않는다. `error: ` 접두 + 짧은 사용법 + `--help` 안내, 종료 2 |
| 도움말 렌더러 | 명령 모델(`Command`·`Option`·`Argument`의 이름·별칭·설명·기본값·필수·숨김)을 읽어 14.5절 배치 규칙대로 그린다. Examples 절은 `AddExamples`가 등록한 줄이다 |
| 이름 제안 | 14.4절과 같은 거리 함수·문턱 상수·동점 규칙을 옮긴다. System.CommandLine 자체 제안은 쓰지 않는다 |
| `help <명령>` | 첫 토큰이 `help`면 `<명령…> --help`로 바꿔 파싱한다 |
| `--version` | 11.2절 모양(엔진 버전·구성·커밋·날짜)으로 쓴다. 값은 MSBuild가 넣는 `AssemblyInformationalVersion`과 `Build/Build.version`에서 온다 |
| 실행 실패 | action 안의 처리되지 않은 예외는 `error: ` 한 줄 + 종료 1(스택은 `--verbose`일 때만) |

- 크기: 900줄 이하(7.2절). 개정 1의 400줄은 System.CommandLine 기본 출력을 쓴다는 전제였다. 도움말 렌더러(약 300줄), 오류 문구 변환(약 150줄), 사용자 파서(약 150줄), 응답 파일·이름 제안(약 150줄), 설정·진입점(약 150줄)으로 다시 추정했다.

### 16.3 이전 대상

| 도구 | 지금 | 이전 뒤 |
|---|---|---|
| Rocks.Build.Cli | `build`·`test`·`sdk`·`game`·`bootstrap`, `--key value`만, 도움말 없음 | 하위 명령 다섯, `--offline`은 bool 플래그, 필수 옵션 표시 |
| Rocks.MetaCompiler | 단일 명령 + `ids` 하위 명령 7개, `RMC1000` 오류 | 기본 명령(빌드가 부르는 경로) + `ids …`. 빌드 도구가 내는 `RMC` 진단 코드는 그대로 두고 인자 오류만 공용 모양으로 바꾼다 |
| Rocks.ToolTests | 하위 명령 12개 + `scaffold plan`·`apply`·`validate` | 같은 구조, `--root`는 공용 옵션(Recursive) |
| 시험 실행 파일들(Rocks.Build.Tests 등) | `args.Contains` | 이전하지 않아도 된다. 바꾸면 같은 층을 쓴다 |
