#include <Core/Diagnostics/Logger.h>
#include <Platform/Clock.h>
#include <stdio.h>

namespace rocks {
namespace {
thread_local bool inSinkCallback = false;

bool IsValidLevel(LogLevel level) noexcept {
    return static_cast<uint8>(level) <= static_cast<uint8>(LogLevel::Error);
}

class CallbackGuard {
public:
    CallbackGuard() noexcept {
        inSinkCallback = true;
    }
    ~CallbackGuard() noexcept {
        inSinkCallback = false;
    }
    CallbackGuard(const CallbackGuard&) = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;
};
} // namespace

Result<void> Logger::Add(RuntimeLogSink& sink, LogFilter filter) noexcept {
    if (inSinkCallback) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (!IsValidLevel(filter.minimum)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    platform::LockGuard lock(mutex_);
    for (usize index = 0; index < count_; ++index) {
        if (sinks_[index].sink == &sink) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }
    if (count_ == MaxSinks) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    sinks_[count_++] = {&sink, filter};
    return Result<void>::Success();
}

Result<void> Logger::Remove(RuntimeLogSink& sink) noexcept {
    if (inSinkCallback) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    platform::LockGuard lock(mutex_);
    for (usize index = 0; index < count_; ++index) {
        if (sinks_[index].sink == &sink) {
            for (usize next = index + 1; next < count_; ++next) {
                sinks_[next - 1] = sinks_[next];
            }
            sinks_[--count_] = {};
            return Result<void>::Success();
        }
    }
    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> Logger::SetFilter(RuntimeLogSink& sink, LogFilter filter) noexcept {
    if (inSinkCallback) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (!IsValidLevel(filter.minimum)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    platform::LockGuard lock(mutex_);
    for (usize index = 0; index < count_; ++index) {
        if (sinks_[index].sink == &sink) {
            sinks_[index].filter = filter;
            return Result<void>::Success();
        }
    }
    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> Logger::Write(LogLevel level, uint32 category, StringView message) noexcept {
    if (inSinkCallback) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (!IsValidLevel(level) || !message.ValidateUtf8().IsOk()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    platform::LockGuard lock(mutex_);
    const LogMessage record{platform::MonotonicNanoseconds(), platform::CurrentThreadId(), category,
                            level, message};
    bool failed = false;
    for (usize index = 0; index < count_; ++index) {
        const Slot& slot = sinks_[index];
        if (static_cast<uint8>(level) < static_cast<uint8>(slot.filter.minimum) ||
            (slot.filter.matchCategory && category != slot.filter.category)) {
            continue;
        }
        CallbackGuard guard;
        if (!slot.sink->Write(record).IsOk()) {
            failed = true;
        }
    }
    return failed ? Result<void>::Failure(ErrorCode::DependencyFailed) : Result<void>::Success();
}

Result<void> TextLogSink::Write(const LogMessage& message) noexcept {
    static const char* const names[] = {"Trace", "Info", "Warning", "Error"};
    if (!IsValidLevel(message.level) || !message.message.ValidateUtf8().IsOk() ||
        output_.write == nullptr || output_.flush == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    platform::LockGuard lock(mutex_);
    char prefix[64]{};
    const int count =
        snprintf(prefix, sizeof(prefix), "[%s][%u] ", names[static_cast<uint8>(message.level)],
                 static_cast<unsigned>(message.category));
    if (count < 0 || static_cast<usize>(count) >= sizeof(prefix) ||
        !output_.write(output_.context, StringView(prefix, static_cast<usize>(count))).IsOk() ||
        !output_.write(output_.context, message.message).IsOk() ||
        !output_.write(output_.context, "\n").IsOk() || !output_.flush(output_.context).IsOk()) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
    return Result<void>::Success();
}

namespace {
Result<void> WriteStderr(void*, StringView bytes) noexcept {
    if (bytes.ByteCount() != 0 &&
        fwrite(bytes.Data(), 1, bytes.ByteCount(), stderr) != bytes.ByteCount()) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
    return Result<void>::Success();
}

Result<void> FlushStderr(void*) noexcept {
    return fflush(stderr) == 0 ? Result<void>::Success()
                               : Result<void>::Failure(ErrorCode::DependencyFailed);
}
} // namespace

StderrLogSink::StderrLogSink() noexcept : sink_({nullptr, WriteStderr, FlushStderr}) {
}

Result<void> StderrLogSink::Write(const LogMessage& message) noexcept {
    return sink_.Write(message);
}
} // namespace rocks
