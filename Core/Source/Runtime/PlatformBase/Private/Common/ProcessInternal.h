#pragma once

#include <Platform/Process.h>

// Process의 공용 입력 검사와 RunProcess 출력 누적이다. 프로세스 생성·대기·pipe는
// Private/<플랫폼>/Process.cpp가 맡는다.
namespace rocks::platform::detail {

// 실행 파일·인자·작업 디렉터리의 UTF-8과 NUL, 포인터·개수, 열거 값을 검사한다. 실행 파일의 존재나
// 플랫폼별 금지 문자(Windows '"')는 검사하지 않는다(플랫폼이 검사한다). 실패는 InvalidArgument다.
FileStatus ValidateProcessStartInfo(const ProcessStartInfo& info) noexcept;

// RunProcess 옵션이 info의 스트림 연결과 맞는지 검사한다(입력 데이터는 입력이 Pipe일 때만, 버퍼
// 포인터는 용량이 있을 때 non-null). 실패는 InvalidArgument다.
FileStatus ValidateProcessRunOptions(const ProcessStartInfo& info,
                                     const ProcessRunOptions& options) noexcept;

// info.workingDirectory가 지정되었는지다(null·빈 값은 부모 것을 물려준다).
inline bool HasWorkingDirectory(const ProcessStartInfo& info) noexcept {
    return info.workingDirectory != nullptr && info.workingDirectoryBytes != 0;
}

// RunProcess가 pipe에서 읽은 데이터를 호출자 버퍼에 용량까지 모은다. 넘는 나머지는 버리고
// truncated를 켠다(자식이 막히지 않게 끝까지 읽는 것은 호출자 책임이다).
struct RunSink {
    uint8* buffer = nullptr;
    usize capacity = 0;
    usize bytes = 0;
    bool truncated = false;

    void Append(const void* data, usize count) noexcept;
};

} // namespace rocks::platform::detail
