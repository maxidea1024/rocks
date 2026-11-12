#include <Core/Diagnostics/LogSink.h>
#include <Platform/Clock.h>

namespace rocks {

LogBuffer::LogBuffer() noexcept : owner_(platform::CurrentThreadId()) {
}

Result<void> LogBuffer::Write(LogLevel level, uint32 category, const char* text,
                              DiagnosticKind kind, uint64 duration,
                              bool alreadyTruncated) noexcept {
    if (platform::CurrentThreadId() != owner_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    if (text == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    // 초기화·오류 경로에서 로그 때문에 추가 할당하거나 대기하지 않는다.
    if (size_ == Capacity) {
        ++dropped_;
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    LogRecord& record = records_[size_++];
    record = {};
    record.timestamp = platform::MonotonicNanoseconds();
    record.thread = owner_;
    record.level = level;
    record.category = category;
    record.kind = kind;
    record.duration = duration;
    usize i = 0;
    while (i + 1 < sizeof(record.text) && text[i] != '\0') {
        record.text[i] = text[i];
        ++i;
    }
    record.text[i] = '\0';
    // 이미 잘린 scope 이름도 여기서 다시 정상 문자열로 오인하지 않도록 손실 표시를 합친다.
    record.truncated = alreadyTruncated || text[i] != '\0';
    return Result<void>::Success();
}

Result<void> LogBuffer::Reset() noexcept {
    if (platform::CurrentThreadId() != owner_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }
    size_ = 0;
    dropped_ = 0;
    return Result<void>::Success();
}

const LogRecord* LogBuffer::At(usize index) const noexcept {
    if (platform::CurrentThreadId() != owner_ || index >= size_) {
        return nullptr;
    }
    return &records_[index];
}
} // namespace rocks
