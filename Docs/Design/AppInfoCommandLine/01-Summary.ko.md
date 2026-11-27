# 앱 정보와 명령줄 — 1. 요약

[목차](README.ko.md)

## 1. 요약

- **앱 정보는 부트 때 한 번 채우고 불변이다.** Rocks는 `AppInfo`라는 불변 레코드를 둔다. 호스트 부트가 명령줄·빌드 정보·설정 부트 결과로 한 번 채우고, `AppInfoScope`로 프로세스에 설치한 뒤에는 바꾸지 않는다.
  - UE `FApp`처럼 아무 데서나 `Set`하는 전역은 두지 않는다. UE에서는 `SetProjectName`·`SetBenchmarking`·`SetGraphicsRHI`·`SetVolumeMultiplier`를 여러 스레드·모듈이 동기화 없이 쓰고 읽는다(3절).
- **프레임 시간은 따로 둔다.** Core 값 타입 `FrameTiming`(double 초, UE `FAppTime`과 같은 단위)이다.
  - 엔진 `RunFrame`이 프레임마다 게임 lane에서 새 값을 만든다.
  - 렌더 lane은 프레임 packet 안 사본을, worker는 작업을 만든 lane의 값을 물려받아 읽는다.
  - 그 밖의 스레드는 프레임 번호(relaxed 원자 값)와 단조 시계만 읽는다. UE 5.8의 `FAppTime` 상속 구조(`AT` 17–21, `ATC` 25–67)와 같은 방향이다.
- **실행 성격은 명령줄과 빌드 기본값에서만 정한다.** 대상은 `--unattended`·`--benchmark`·`--deterministic` 같은 것이다. 설정 파일·CVar로 정하지 않는다. 봉인 전(Fatal 처리)부터 필요하고, 출시 빌드의 세션 층 제한(Config D5)과 부딪히지 않기 때문이다(10절).
  - 뜻·기본값·출시 빌드에서 받는지는 UE 5.8과 같다. 고정 step 기본 길이는 1/30이고, `--fps`는 길이만 바꾸며, 고정 step·고정 시드는 출시 빌드에서도 동작한다(10.2절).
- **빌드 정보는 Rocks.Build가 생성 TU로 주입한다.** 이미 있는 `<exe>.BuildInfo.cpp` 옆에 커밋·브랜치·구성·대상 종류를 담은 별도 생성 TU를 만든다. 날짜는 빌드 시각이 아니라 커밋 시각을 쓴다(재현성 — 11절).
- **프로세스 명령줄 보관.** `ProcessCommandLine`이 진입점에서 한 번 UTF-8로 캡처하고 불변으로 둔다. Windows는 넓은 문자 원문에서 바꾼다(ANSI argv는 비 ASCII 인자를 깨뜨린다). 로그에는 `Secret` 옵션 값을 가린 형태만 남긴다(12절).
- **C++ 명령줄 파서는 자체 구현이다(확정).** 외부 라이브러리는 검토하고 제외했다. 이유는 크기다(사용자 결정 2026-11-18).
  - 선언은 리플렉션 지정자 문법을 쓴다. 옵션 struct 하나를 `ROCKS_STRUCT(Command(...))`·`ROCKS_PROPERTY(Help=..., Alias=...)`로 선언한다.
  - MetaCompiler는 Command struct마다 명령줄 스키마와 **필드별 바인더 코드**를 생성한다. 런타임은 그 스키마와 바인더로 파싱·검증·도움말을 모두 만든다(D19).
  - 값 변환은 Core `NumberText`와 생성 enum 표가 맡는다. ReflectionText·TypeRegistry·ObjectRegistry에 기대지 않는다. 그래서 리플렉션을 쓰지 않던 낮은 계층 도구(벤치·FoundationTests)도 옮길 수 있다.
  - Command struct는 직렬화하지 않으므로 identity 면제다. `rocksids` 항목과 `ids add`가 필요 없다(D30).
  - 실행 함수는 결과 struct를 `const&`로 받는다. 옵션마다 `GetValue`를 부르는 C#보다 꺼내 쓰기가 짧다(15절).
- **C# 도구는 System.CommandLine을 파싱에만 쓴다.** 외부 패키지(MIT)를 처음 들여온다. 별도 어셈블리 `Rocks.Tooling.Cli`의 `RocksCli`가 도움말·오류 출력을 모두 그린다.
- **양쪽 출력은 "Rocks 명령줄 출력 규약"(14.4·14.5절)을 따른다.** 규약은 이 문서가 직접 정의한다. 줄 끝 `\n`, UTF-8, 영어, 너비·열 규칙, 오류 문구, 종료 코드(0·1·2)가 그 내용이다. C++과 C#은 각자 이 규약대로 렌더한다. 같은 시험 명령을 양쪽으로 정의해 도움말·오류 출력이 byte 단위로 같은지 golden 시험으로 확인한다(17절).
