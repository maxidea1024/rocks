# Rocks headless commandlet

`Commandlet` 타깃은 창, 렌더러, GPU, 오디오 장치 없이 실행되는 Core 기반 콘솔 프로그램이다. 엔진 부트는 연결되지 않았고 cook 등의 명령은 아직 없다. `core-check`는 Core 할당과 해제만 검사한다. `trace-report <입력.rtrace> [<출력.json>]`은 trace 파일의 결정적 JSON report(스레드별 구간 이름의 횟수·합계·p50/p95/p99, task 임계 경로, 손실·잘림·정의 누락 요약, [trace 설계](../../Docs/Design/TraceSystem.ko.md) 10절 TR4)를 쓴다. 출력 경로가 없으면 표준 출력이다. 파일·분석 오류는 원본 파일 오류 코드와 함께 알리고 1을 반환한다.

`log`는 암호화한 로그 파일 도구다([로깅 설계](../../Docs/Design/LoggingSystem.ko.md) 9.1절, LG10). `log keygen <공개키 파일> <개인키 파일>`은 X-Wing 키 쌍을 만들고(기존 파일을 덮어쓰지 않는다), `log decode <파일> --key <개인키 파일> [--json]`은 평문을 표준 출력으로 낸다. 끝 표시가 없으면 마지막 완전한 묶음까지 낸 뒤 "no end marker (abnormal termination)"를 알리고, 변조는 오류 1이다. `--json`은 상태를 표준 오류에 JSON 객체 한 줄로 쓴다. `log seal <평문> <출력> --public <공개키 파일>`은 같은 형식으로 봉인한다(진단·시험용). **개인키는 저장소와 쿠킹 결과에 넣지 않는다.**

`content-sign`은 쿠킹 매니페스트 서명 도구다([콘텐츠 서명 설계](../../Docs/Design/ContentSigning.ko.md) 5·7절, A1f). `content-sign keygen <키 파일> <공개 파일> --password-file <경로>`은 Ed25519 키를 비밀번호(Argon2id)로 감싼 키 파일과 신뢰 목록에 넣을 공개 줄(`<keyId hex> <공개키 hex>`)을 만든다(기존 파일을 덮어쓰지 않는다). `content-sign sign <in.rman> <out.rsig> --key <키 파일> --password-file <경로> --version <u64> --product <id> --channel <이름>`은 매니페스트를 확인한 뒤 서명 봉투를 임시 파일에 쓰고 원자적으로 바꾼다. `content-sign verify <in.rsig> --trust <공개 파일> --product <id> --channel <이름> [--minimum-version <u64>]`은 엔진과 같은 순서로 검증한다. 비밀번호는 파일로만 받는다(끝의 줄바꿈 하나는 뺀다). **키 파일은 저장소와 쿠킹 결과에 넣지 않는다.**

에셋 명령([에셋 시스템 설계](../../Docs/Design/AssetSystem/13-CommandLine.ko.md) 13절)은 프로젝트(`--project=<dir>`, 없으면 현재 디렉터리에서 위로 `*.rocksproject`)의 에디터 패키지를 다룬다. `asset-list [--type=<Type>] [--under=<path>]`은 색인 항목을 경로 순으로 쓰고(`--type`은 그 클래스와 파생), `asset-index-verify`는 진단을 낸다. `asset-new <Type> <path> [<경로>=<값>…]`·`asset-set <path|AssetId> <대입>… [--force]`는 쓰기 잠금(`--wait=<초>`)을 잡고 대입을 모두 검증한 뒤 저장하며, `asset-info <path|AssetId> [--json] [--all]`은 `asset-set`에 그대로 넣을 수 있는 `<경로>=<값>` 줄(또는 JSON)을 쓴다. 값 표기는 ReflectionText(리플렉션 확장 10절)이고 참조는 논리 경로나 AssetId다. 대입 목록 옵션 `--unset=<경로 목록>`·`--erase=<경로 목록>`을 받는다. 에셋 명령은 실행 때 에셋 타입 모듈(지금 Animation — Skeleton·AnimationClip)을 시작해 타입을 등록한다. 진단은 표준 오류에 `<명령>: <코드>: <대상>: <설명>` 한 줄씩이다.

```text
dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=Commandlet
RocksCommandlet --help
RocksCommandlet --list
RocksCommandlet core-check
RocksCommandlet trace-report capture.rtrace report.json
RocksCommandlet asset-new Skeleton /Game/Characters/Hero
RocksCommandlet asset-set /Game/Characters/Hero useRetargetModesFromCompatibleSkeleton=on
RocksCommandlet asset-info /Game/Characters/Hero --json
dotnet run --project Programs/Rocks.Commandlet.Tests -- --executable <빌드된 실행 파일의 절대 경로>
```

진입점은 공통 실행 계층 `Launch`다(`Private/Common/Main.cpp`의 `ROCKS_IMPLEMENT_APPLICATION`, [실행 진입점 설계](../../Docs/Design/CrashHandlingAndLaunch.ko.md)). Launch가 명령줄을 UTF-8로 캡처하고, 크래시 수집(덤프 도우미, `Saved/Crashes/RocksCommandlet-<시작 UTC>-<pid>/`)과 도구 로그(Sync LogSystem·stderr sink)를 설치한다. 플랫폼 출력 채널은 `Private/<플랫폼>/StandardOutput.cpp`에 있다. 인수는 UTF-8로 명시적으로 전달한다. 잘못된 인코딩이나 길이 초과도 사용법 오류 2로 처리한다. 잘못된 인코딩(Windows 짝 없는 UTF-16, macOS 잘못된 UTF-8)은 Launch가 명령줄을 캡처하지 못한 경우라 stderr 쓰기가 실패해도 2다(그 밖의 사용법 오류는 오류 문구를 쓰지 못하면 1이다). Windows 콘솔 출력은 Unicode API를, 리디렉션은 UTF-8 바이트를 사용한다. 실제 Windows 콘솔의 Unicode 표시는 별도 실기 검증이 필요하다. macOS 프로세스는 닫힌 pipe에 의한 SIGPIPE를 무시하여 출력 실패를 처리한다. 성공은 0, 명령 사용법과 알 수 없는 명령은 2, 내부 실행·출력 오류는 1이다. 등록된 명령은 0부터 125까지의 종료 코드를 반환할 수 있다. `--help`와 `--list`에는 추가 인수를 허용하지 않는다.

등록 및 실행은 registry를 생성한 스레드에서 수행한다. 이름과 설명은 registry가 복사하며 callback과 context는 빌려 쓰므로 마지막 실행이 끝날 때까지 유효해야 한다. 출력은 주입된 동기 callback을 이용하며 callback이 보관할 데이터는 직접 복사한다. 등록 가능 명령은 64개, 전체 인수는 128개, 인수 하나는 4096 UTF-8 바이트까지다. 실행 중 재진입·등록 및 다른 스레드의 접근은 명시적인 오류로 거부한다.
