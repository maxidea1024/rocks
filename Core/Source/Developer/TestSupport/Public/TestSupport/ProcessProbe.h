#pragma once

#include <Base/Types.h>

// 자식 프로세스 시험(Platform/Process.h)과 그 probe가 쓰는 플랫폼 도구다. probe는 테스트 실행
// 파일을 "--process-probe <모드> ..."로 다시 띄운 자식이다(PlatformBase/Tests/ProcessProbe.cpp).
namespace rocks::test {

// 이 플랫폼에서 Process::Start가 지원되어야 하는지다(Windows·macOS true, Android false). 지원되어야
// 하는 플랫폼에서 Unsupported가 나오면 시험이 실패한다.
bool ProcessSpawnExpected() noexcept;

// 현재 프로세스의 명령줄 인자를 UTF-8로 다시 읽는다. index 0은 실행 파일이다. 부모가 넘긴 인자를
// byte 그대로 돌려주는 시험에 쓴다(Windows의 main argv는 ANSI 코드 페이지라 UTF-8 인자를 잃으므로
// GetCommandLineW에서 다시 나눈다. POSIX는 main의 argv 그대로다).
// buffer에 인자를 NUL로 구분해 쓰고 arguments[i]에 시작 위치를 둔다. 인자 수를 반환하며, 공간이
// 부족하거나 변환에 실패하면 0이다.
usize ReadUtf8Arguments(int argc, char** argv, char* buffer, usize capacity, const char** arguments,
                        usize maxArguments) noexcept;

// 표준 출력(stream 1)·표준 오류(stream 2)에 CRT 버퍼·텍스트 변환 없이 그대로 쓴다. 모두 썼으면
// true다.
bool WriteStandardStream(int stream, const void* data, usize bytes) noexcept;

// 표준 입력에서 최대 capacity 바이트를 읽는다. 읽은 수, EOF면 0, 실패하면 -1이다.
int64 ReadStandardInput(void* buffer, usize capacity) noexcept;

// 현재 실행 파일을 "--process-probe sleep 60000"으로 띄우고 PID를 반환한다(실패하면 0). 표준
// 스트림을 그대로 물려주고, 현재 프로세스와 같은 job(Windows)·프로세스 그룹(POSIX)에 둔다. 부모의
// Process가 자손까지 정리하는지 확인하는 손자 프로세스다.
uint32 SpawnSleepingDescendant() noexcept;

// 프로세스 하나가 끝나기를 기다린다. Windows는 만들 때 handle을 열어 두므로 PID가 재사용되어도 다른
// 프로세스를 보지 않는다(이미 끝났으면 바로 끝난 것으로 본다). POSIX는 PID로 확인한다.
class ProcessWatch {
public:
    explicit ProcessWatch(uint32 processId) noexcept;
    ~ProcessWatch() noexcept;
    ProcessWatch(const ProcessWatch&) = delete;
    ProcessWatch& operator=(const ProcessWatch&) = delete;

    // milliseconds 안에 끝났으면 true다.
    bool WaitGone(uint32 milliseconds) noexcept;

private:
    uint32 processId_ = 0;
    // Windows 프로세스 handle이다. POSIX 구현은 쓰지 않는다.
    [[maybe_unused]] uptr handle_ = 0;
};

// existing의 hard link를 link 경로(UTF-8)에 만든다. 성공하면 true다.
bool MakeHardLink(const char* existing, const char* link) noexcept;

// 현재 프로세스의 우선순위를 nice처럼 큰 값일수록 낮은 우선순위로 돌려준다(POSIX는 nice, Windows는
// 우선순위 클래스를 IDLE 19·BELOW_NORMAL 5·NORMAL 0·ABOVE_NORMAL -5·HIGH -10·REALTIME -20으로).
int32 CurrentPriorityRank() noexcept;

// path(UTF-8)에 파일을 만들어 자식이 물려받을 수 있는(상속 가능·close-on-exec 아님) handle·fd로
// 연다. handle에는 handle·fd 값, identity에는 파일 고유 번호(Windows file index, POSIX inode)를
// 둔다.
bool OpenInheritableFile(const char* path, uptr& handle, uint64& identity) noexcept;
void CloseInheritableFile(uptr handle) noexcept;

// 이 프로세스에서 handle 값이 identity의 파일을 가리키는지다. 자식이 부모의 handle을 물려받았는지
// 확인한다.
bool HandleRefersToFile(uptr handle, uint64 identity) noexcept;

// 자신에게 SIGTERM을 보낸다(기본 처리로 끝난다). 신호가 없는 플랫폼(Windows)은 false다.
bool RaiseTerminateSignal() noexcept;

} // namespace rocks::test
