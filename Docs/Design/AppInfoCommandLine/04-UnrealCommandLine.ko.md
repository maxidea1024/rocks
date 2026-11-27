# 앱 정보와 명령줄 — 4. UE 5.8 명령줄 조사

[목차](README.ko.md)

## 4. UE 5.8 명령줄 조사

| 대상 | 내용 | 근거 |
|---|---|---|
| `FCommandLine` 저장 | 고정 크기 `TCHAR` 배열 넷(현재·원본·로그용 둘), 각 `MaxCommandLineSize = 16384` | `CLH` 48, 185–191 |
| 바꾸기 | `Set`·`Append`로 실행 중 바꾼다. 응답 파일 펼치기(`-CmdLineFile=*.txt`, `LEL` 1093–1170)와 게임 이름 처리(`LEL` 2361·2401)가 다시 `Set`한다. 첫 `Set`은 `PreInitPreStartupScreen`(`LEL` 1808) | `CLH` 88–94 |
| 환경 변수 추가 | 비출시·에디터 데이터 빌드는 환경 변수 `UE-CmdLineArgs`를 ` -EnvAfterHere ` 뒤에 덧붙인다. 출시 게임은 악용을 막으려고 받지 않는다 | `LEL` 1814–1827 |
| 로그 가리기 | 컴파일 때 정한 목록(`FILTER_COMMANDLINE_LOGGING`)의 인자를 로그용 사본에서 지운다. 허용 목록 모드(`UE_COMMAND_LINE_USES_ALLOW_LIST`)도 있다 | `CLC` 221–278 |
| 하위 프로세스 상속 | `RegisterArgument(이름, 문맥 플래그 + Inherit, 설명)`, `BuildSubprocessCommandLine` | `CLH` 24–41, 102–120 |
| 토큰 나누기 | `FCommandLine::Parse` → Tokens·Switches | `CLH` 136 |
| `FParse::Value` | 명령줄 **문자열 전체**에서 `Key=`를 대소문자 무시로 찾는다(`FCString::Strifind`). 정의되지 않은 인자·오타는 아무 진단 없이 무시된다 | `PAR` 30–83, `PARC` 265 |
| `FParse::Param` | 공백 뒤 `-`·`/`로 시작하는 단어 전체를 대소문자 무시로 찾는다 | `PAR` 127, `PARC` 325–350 |
| `FParse::Bool`·`Token`·`Command`·`QuotedString`·`GrammarBasedCLIParse` | 값 읽기·토큰·콘솔 명령·따옴표·문법 기반 콜백 | `PAR` 28, 85, 117–132, 223 |
| `UCommandlet` 도움말 | `HelpDescription`·`HelpUsage`·`HelpWebLink`·`HelpParamNames`·`HelpParamDescriptions` 칸을 손으로 채운다. 파싱과 따로 놀아 둘이 어긋날 수 있다 | `CMD` 46–62 |
| `UCommandlet::ParseCommandLine` | `-`로 시작하면 Switch, 아니면 Token. `Key=Value` Switch는 Params 맵으로 옮긴다 | `CMD` 115–160 |
| `ApplyCommandLineSwitches` | 에디터 전용. Switch를 UObject 프로퍼티에 리플렉션으로 대입한다 | `CMD` 160–170, `Runtime/Engine/Private/Commandlets/Commandlet.cpp:41` |

**관찰과 Rocks 방향**
- UE는 인자를 선언하지 않고 조회하는 쪽이 문자열로 찾는다. 그래서 알 수 없는 인자·오타·형식 오류가 조용히 넘어간다. 도움말은 파싱과 별도 칸이다.
- Rocks는 선언 한 곳(옵션 struct)에서 파싱·검증·도움말을 모두 만든다. 알 수 없는 인자는 오류다.
- 선언된 필드에 인자를 대입하는 발상은 UE `ApplyCommandLineSwitches`와 같다. Rocks는 이것을 에디터 전용 보조가 아니라 기본 경로로 삼는다. 다만 실행 중 리플렉션 조회가 아니라 MetaCompiler가 생성한 필드별 바인더로 대입한다(D19).
- 환경 변수로 인자를 더하는 `UE-CmdLineArgs`는 대응하지 않는다. CI·개발자는 응답 파일(`@file`, 도구만)이나 실행 명령줄을 쓴다. 원문 보존(12절)과 비밀 가리기가 한 경로로 남는다.
