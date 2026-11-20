#pragma once

#include <Core/App/ProcessCommandLine.h>

// ProcessCommandLine의 고정 크기 저장소다. 프로세스 전역 캡처와 시험이 같은 채우기 규칙을
// 쓴다(시험은 지역 저장소를 채워 전역 상태를 건드리지 않는다).
namespace rocks::detail {

class CommandLineStorage {
public:
    // 진입점 인자를 UTF-8로 옮기고 실행 파일 경로를 OS에서 읽는다. 오류는
    // ProcessCommandLine::Capture와 같다(AlreadyExists 제외). 실패하면 빈 상태다.
    Result<void> Fill(int argc, wchar_t** argv) noexcept;
    Result<void> Fill(int argc, char** argv) noexcept;

    StringView ExecutablePath() const noexcept {
        return StringView(executable_, executableBytes_);
    }

    Span<const StringView> Arguments() const noexcept {
        return Span<const StringView>(arguments_, argumentCount_);
    }

private:
    template <typename Character>
    Result<void> FillFrom(int argc, Character** argv) noexcept;

    void Clear() noexcept;

    StringView arguments_[ProcessCommandLine::MaxArguments];
    char text_[ProcessCommandLine::MaxTotalBytes];
    char executable_[ProcessCommandLine::MaxExecutablePathBytes + 1];
    usize argumentCount_ = 0;
    usize executableBytes_ = 0;
};

} // namespace rocks::detail
