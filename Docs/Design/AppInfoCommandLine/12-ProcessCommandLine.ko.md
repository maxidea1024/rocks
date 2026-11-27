# 앱 정보와 명령줄 — 12. 프로세스 명령줄 `ProcessCommandLine`

[목차](README.ko.md)

## 12. 프로세스 명령줄 `ProcessCommandLine`

구현됨(개정 3): `Core/Source/Runtime/Core/Public/Core/App/ProcessCommandLine.h`. 크래시 처리·실행 진입점 설계의 LC3이 만들고 LC4에서 상한을 고쳤다. 캡처는 Launch 진입점(`RunApplication`·`RunEmbeddedApplication`)이 하고, 실행 파일 본문은 `ApplicationContext::Arguments()`로 받는다.

```cpp
namespace rocks {

// 진입점이 한 번 캡처하는 프로세스 명령줄이다. 캡처 뒤 불변이고 모든 스레드가 읽을 수 있다.
class ProcessCommandLine {
public:
    // OS 한계(Windows 32767 UTF-16 단위, Linux·Android ARG_MAX 2 MiB, macOS 1 MiB)에 맞춘 상한이다.
    // 저장소는 정적 BSS이고 쓴 만큼만 실제 메모리가 된다.
    static constexpr usize MaxArguments = 32 * 1024;
    static constexpr usize MaxTotalBytes = 2 * 1024 * 1024;
    static constexpr usize MaxExecutablePathBytes = 96 * 1024;

    // 진입점에서 한 번 부른다. 오류: 두 번째 호출 AlreadyExists, 잘못된 UTF-16·UTF-8 InvalidArgument,
    // 상한 초과 CapacityExceeded, 실행 파일 경로를 얻지 못하면 그 파일 상태의 오류.
    // Windows wmain·wWinMain 진입점: CRT가 나눈 넓은 문자 argv(__wargv)를 UTF-8로 바꾼다.
    static Result<void> Capture(int argc, wchar_t** argv) noexcept;
    // POSIX main 진입점: argv를 UTF-8로 검증해 복사한다(UTF-8이 아니면 InvalidArgument).
    static Result<void> Capture(int argc, char** argv) noexcept;

    // 전제: Capture 성공(아니면 assert 실패).
    static const ProcessCommandLine& Get() noexcept;
    static bool IsCaptured() noexcept;

    // 실행 파일 경로다. argv[0]이 아니라 OS에서 얻는다(Windows GetModuleFileNameW,
    // 그 밖은 platform::GetExecutablePath).
    StringView ExecutablePath() const noexcept;
    Span<const StringView> Arguments() const noexcept;  // 실행 파일 제외, 원문 순서
};

} // namespace rocks
```

- **원문 보존.** 캡처한 인자는 바꾸지 않는다. UE `Set`·`Append`(`CLH` 88–94)에 대응하는 것이 없다. 응답 파일을 펼친 결과는 파서가 따로 만드는 값이고, 보관된 원문은 그대로다.
- **정규화.** UTF-8 검증만 한다. 대소문자·경로 구분자·따옴표는 건드리지 않는다. 따옴표 처리는 OS·CRT가 이미 했다.
- **Windows(개정 3, 크래시 처리 설계 D7).**
  - 진입점은 Launch의 `ROCKS_IMPLEMENT_APPLICATION`이 만드는 `wmain`(콘솔) 또는 `wWinMain`(GUI subsystem)뿐이다. `wWinMain`은 CRT가 이미 나눈 `__argc`·`__wargv`를 쓴다. 둘 다 CRT가 넓은 문자로 나눈 argv를 UTF-8로 바꾸므로 ANSI 변환(5.2절의 깨짐·best-fit 보안 문제)을 거치지 않는다.
  - `main(char**)` 진입점은 Windows에서 지원하지 않는다. 진입점 직접 정의는 policy `process-entry-point`가 막는다. 그래서 개정 2의 `GetCommandLineW` 직접 분해(MSVC CRT 규칙 구현과 순수 함수 공개)는 하지 않는다. 버린 이유: Launch가 모든 Rocks 실행 파일의 진입점을 만들므로 직접 분해가 필요한 진입점이 남지 않고, CRT 규칙을 다시 구현하면 CRT와 어긋날 위험만 남는다.
  - 보조 수단으로 실행 파일 manifest에 `activeCodePage`=`UTF-8`(Windows 10 1903 이상)을 넣는 것을 검토한다. 그러면 남은 ANSI API도 UTF-8이 된다. 다만 OS 버전에 기대므로 캡처 경로의 근거로 삼지 않는다(20절).
- **Android.** argv가 없다. 런처 intent의 추가 인자 문자열을 같은 형태로 넣는 경로는 Android 호스트를 만들 때 정한다(20절).
- **로그용 형태와 비밀 가리기(D16).** 원문은 로그·trace·크래시 보고에 직접 쓰지 않는다. 파서가 스키마를 알고 난 뒤 `RedactedArguments(schema, out)`로 `Secret` 옵션의 값을 `<redacted>`로 바꾼 형태를 만든다. 시작 로그는 이 형태만 쓴다.
  - 파싱 실패로 스키마 판정을 못 한 인자도 원문 대신 `<unparsed>`로 남긴다. 비밀 값이 오타 때문에 그대로 새는 것을 막는다.
  - `--set` 값은 설정 쪽 비밀 표시가 생기면 그 대상의 값을 가린다. 지금 설정에는 비밀 표시가 없다.
- **AppInfo와의 관계.** 둘은 따로 설치되는 불변 값이다. `ProcessCommandLine`은 0단계, AppInfo는 3단계에서 설치된다. AppInfo는 명령줄을 소유하지 않는다. AppInfo의 칸 일부(`sessionId`·`instanceId`·실행 성격)가 파싱 결과에서 온다.
- **하위 프로세스 상속.** UE `RegisterArgument(…, Inherit)`처럼 옵션에 "자식에게 물려준다" 표시를 두는 것은 이번 범위에서 뺀다. 자식을 띄우는 도구(셰이더 컴파일 worker 등)가 생길 때 `Inherit` 지정자로 더한다(20절).
