#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

namespace rocks {

// 진입점이 한 번 캡처하는 프로세스 명령줄이다(UE FCommandLine 대응,
// Docs/Design/AppInfoCommandLine.ko.md 12절의 부분 구현,
// Docs/Design/CrashHandlingAndLaunch.ko.md 7.3절). 캡처 뒤 불변이고 모든 스레드가 읽을 수 있다.
// 공통 실행 계층(Launch RunApplication)이 진입점 인자로 캡처하므로 프로그램은 Get()만 쓴다. 파서가
// 아니다: 인자를 나누거나 해석하지 않고 OS·CRT가 나눈 원문을 UTF-8로 보관한다.
//
// 진입점 형태(D7): Windows는 wmain·wWinMain의 넓은 문자 인자만 받는다(좁은 문자 argv는 ANSI
// 변환으로 비 ASCII 인자가 깨진다). POSIX는 main(char**)이다. 플랫폼에 맞지 않는 판을 부르면
// Unsupported다. POSIX 인자는 UTF-8이어야 한다. UTF-8이 아닌 인자(다른 locale 인코딩의 파일 이름
// 등)는 InvalidArgument이고 프로그램이 시작하지 않는다(대체 문자로 바꾸면 경로가 다른 파일을
// 가리키게 되므로 바꾸지 않는다, 설계 25절 LC4).
//
// 저장소는 정적이다(인자 view MaxArguments개, 텍스트 MaxTotalBytes, 실행 파일 경로). 할당하지
// 않는다.
//
// 사용 예(Launch가 캡처한 뒤 아무 스레드에서):
//   const ProcessCommandLine& commandLine = ProcessCommandLine::Get();
//   for (const StringView argument : commandLine.Arguments()) { ... }
class ProcessCommandLine {
public:
    // 인자 수 상한(실행 파일 제외)과, 인자 UTF-8 byte 합의 상한(각 인자 끝 NUL 포함)이다. Windows
    // 명령줄은 32767 UTF-16 단위라 인자는 최대 16384개(한 글자 인자와 공백)이고 UTF-8로 96 KiB와 끝
    // NUL이다. 그래서 Windows는 OS가 넘길 수 있는 모든 명령줄을 받는다. POSIX는
    // ARG_MAX(Linux·Android 2 MiB 이상, macOS 1 MiB, 환경 변수 포함)라 짧은 인자는 수십만 개까지 올
    // 수 있다. 텍스트는 2 MiB로 받지만 인자 수는 32768개까지만 받는다(view 저장소 512 KiB를 정적
    // BSS로 두는 비용과의 절충). 넘으면 CapacityExceeded이고 프로그램이 시작하지 않는다(xargs는
    // -n으로 나눠 넘긴다). 저장소는 쓴 만큼만 실제 메모리가 된다.
    static constexpr usize MaxArguments = 32 * 1024;
    static constexpr usize MaxTotalBytes = 2 * 1024 * 1024;
    // 실행 파일 경로의 UTF-8 byte 상한이다. Windows 경로 최대 32767 UTF-16 단위가 UTF-8로 최대
    // 3배가 되는 길이를 담는다.
    static constexpr usize MaxExecutablePathBytes = 96 * 1024;

    // 진입점에서 한 번 부른다. argv[0]은 실행 파일이고 Arguments에 넣지 않는다.
    // 오류:
    // - AlreadyExists: 이미 캡처했다(또는 다른 스레드가 캡처하는 중이다). 보관된 값은 그대로다.
    // - InvalidArgument: argc가 1보다 작거나 argv·인자가 nullptr, 짝 없는 UTF-16 surrogate,
    //   잘못된 UTF-8
    // - CapacityExceeded: MaxArguments·MaxTotalBytes·MaxExecutablePathBytes 초과
    // - Unsupported: 이 플랫폼의 진입점 형태가 아니다(Windows의 char 판, POSIX의 wchar_t 판)
    // - 그 밖: 실행 파일 경로를 OS에서 얻지 못한 파일 오류(FileErrorCode)
    // 실패하면 캡처하지 않은 상태로 남는다(다시 부를 수 있다).
    static Result<void> Capture(int argc, wchar_t** argv) noexcept;
    static Result<void> Capture(int argc, char** argv) noexcept;

    // 캡처한 명령줄이다. 전제: Capture 성공(아니면 assert 실패). 반환 참조는 프로세스 수명 동안
    // 유효하다.
    static const ProcessCommandLine& Get() noexcept;

    // 캡처를 마쳤는가다(아무 스레드에서 부를 수 있다).
    static bool IsCaptured() noexcept;

    // 실행 파일의 절대 경로다(UTF-8). argv[0]이 아니라 OS에서 얻는다(Windows GetModuleFileNameW, 그
    // 밖은 platform::GetExecutablePath).
    StringView ExecutablePath() const noexcept {
        return executablePath_;
    }

    // 프로그램 인자다(실행 파일 제외, 원문 순서). 빈 인자도 그대로 있다. 각 view의 Data()는 NUL로
    // 끝난다(C 문자열을 받는 API에 그대로 넘길 수 있다).
    Span<const StringView> Arguments() const noexcept {
        return arguments_;
    }

private:
    friend class ProcessCommandLineAccess;

    StringView executablePath_;
    Span<const StringView> arguments_;
};

} // namespace rocks
