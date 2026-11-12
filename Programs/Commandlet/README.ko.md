# Rocks headless commandlet

`Commandlet` 타깃은 창, 렌더러, GPU, 오디오 장치 없이 실행되는 Core 기반 콘솔 프로그램이다. 현재 엔진 부트와 에셋 시스템은 연결되지 않았으며 cook 등의 명령은 제공하지 않는다. `core-check`는 Core 할당과 해제만 검사한다.

```text
dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=Commandlet
RocksCommandlet --help
RocksCommandlet --list
RocksCommandlet core-check
dotnet run --project Programs/Rocks.Commandlet.Tests -- --executable <빌드된 실행 파일의 절대 경로>
```

인수는 UTF-8로 명시적으로 전달한다. 잘못된 인코딩이나 길이 초과도 사용법 오류 2로 처리한다. Windows 콘솔 출력은 Unicode API를, 리디렉션은 UTF-8 바이트를 사용한다. 실제 Windows 콘솔의 Unicode 표시는 별도 실기 검증이 필요하다. macOS 프로세스는 닫힌 pipe에 의한 SIGPIPE를 무시하여 출력 실패를 처리한다. 성공은 0, 명령 사용법과 알 수 없는 명령은 2, 내부 실행·출력 오류는 1이다. 등록된 명령은 0부터 125까지의 종료 코드를 반환할 수 있다. `--help`와 `--list`에는 추가 인수를 허용하지 않는다.

등록 및 실행은 registry를 생성한 스레드에서 수행한다. 이름과 설명은 registry가 복사하며 callback과 context는 빌려 쓰므로 마지막 실행이 끝날 때까지 유효해야 한다. 출력은 주입된 동기 callback을 이용하며 callback이 보관할 데이터는 직접 복사한다. 등록 가능 명령은 64개, 전체 인수는 128개, 인수 하나는 4096 UTF-8 바이트까지다. 실행 중 재진입·등록 및 다른 스레드의 접근은 명시적인 오류로 거부한다.
