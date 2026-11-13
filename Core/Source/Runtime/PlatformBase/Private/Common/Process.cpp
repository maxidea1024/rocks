#include "ProcessInternal.h"
#include "FileInternal.h"

#include <Platform/Memory.h>

namespace rocks::platform {
namespace detail {

namespace {

// 인자는 빈 값도 허용한다. 비어 있지 않으면 포인터가 있어야 하고 UTF-8·NUL 규칙을 지킨다.
bool ValidText(const char* text, usize bytes) noexcept {
    if (bytes == 0) {
        return true;
    }

    return text != nullptr && ValidFilePath(text, bytes);
}

bool ValidMode(ProcessStreamMode mode) noexcept {
    return mode == ProcessStreamMode::Null || mode == ProcessStreamMode::Inherit ||
           mode == ProcessStreamMode::Pipe;
}

} // namespace

FileStatus ValidateProcessStartInfo(const ProcessStartInfo& info) noexcept {
    if (info.executable == nullptr || info.executableBytes == 0 ||
        !ValidFilePath(info.executable, info.executableBytes)) {
        return {FileCode::InvalidArgument, 0};
    }

    if (info.argumentCount != 0 && info.arguments == nullptr) {
        return {FileCode::InvalidArgument, 0};
    }

    for (usize i = 0; i < info.argumentCount; ++i) {
        if (!ValidText(info.arguments[i].text, info.arguments[i].bytes)) {
            return {FileCode::InvalidArgument, 0};
        }
    }

    if (info.workingDirectoryBytes != 0 &&
        !ValidText(info.workingDirectory, info.workingDirectoryBytes)) {
        return {FileCode::InvalidArgument, 0};
    }

    if (!ValidMode(info.input) || !ValidMode(info.output) || !ValidMode(info.error)) {
        return {FileCode::InvalidArgument, 0};
    }

    if (info.priority != ProcessPriority::Normal && info.priority != ProcessPriority::Low) {
        return {FileCode::InvalidArgument, 0};
    }

    return {};
}

FileStatus ValidateProcessRunOptions(const ProcessStartInfo& info,
                                     const ProcessRunOptions& options) noexcept {
    if (options.inputBytes != 0 &&
        (options.input == nullptr || info.input != ProcessStreamMode::Pipe)) {
        return {FileCode::InvalidArgument, 0};
    }

    if ((options.outputCapacity != 0 && options.output == nullptr) ||
        (options.errorCapacity != 0 && options.error == nullptr)) {
        return {FileCode::InvalidArgument, 0};
    }

    // 모으지 않는 스트림에 준 버퍼는 호출 오류다(입력과 같은 규칙). 합친 오류는 출력 버퍼로 온다.
    const bool outputCollected = info.output == ProcessStreamMode::Pipe;
    const bool errorCollected = info.error == ProcessStreamMode::Pipe && !info.mergeErrorIntoOutput;
    if ((!outputCollected && (options.output != nullptr || options.outputCapacity != 0)) ||
        (!errorCollected && (options.error != nullptr || options.errorCapacity != 0))) {
        return {FileCode::InvalidArgument, 0};
    }

    return {};
}

void RunSink::Append(const void* data, usize count) noexcept {
    const usize room = capacity - bytes;
    const usize taken = count < room ? count : room;
    MemoryCopy(buffer + bytes, data, taken);
    bytes += taken;
    if (taken != count) {
        truncated = true;
    }
}

} // namespace detail

Process::Process(Process&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        Close();
        state_ = other.state_;
        other.state_ = nullptr;
    }

    return *this;
}

Process::~Process() noexcept {
    Close();
}

} // namespace rocks::platform
