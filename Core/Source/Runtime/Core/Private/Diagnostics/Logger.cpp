#include <Core/Diagnostics/Logger.h>
#include <Platform/Clock.h>
#include <stdio.h>

namespace rocks {
namespace {
thread_local bool inSinkCallback = false;

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
    if (!IsValidLogLevel(filter.minimum)) {
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
    if (!IsValidLogLevel(filter.minimum)) {
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

Result<void> Logger::Write(LogLevel level, const LogCategory& category, StringView message,
                           SourceLocation where) noexcept {
    if (inSinkCallback) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (!IsValidLogLevel(level) || !message.ValidateUtf8().IsOk()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    platform::LockGuard lock(mutex_);
    LogEvent event;
    event.level = level;
    event.category = category.Name();
    event.message = message;
    event.location = where;
    event.threadId = platform::CurrentThreadId();
    event.monotonicNanoseconds = platform::MonotonicNanoseconds();
    bool failed = false;
    for (usize index = 0; index < count_; ++index) {
        const Slot& slot = sinks_[index];
        // Fatal은 어떤 필터로도 막지 않는다(설계 7절). 레벨 하한은 Fatal이 가장 크므로 늘 통과하고,
        // 카테고리 조건만 건너뛰면 된다.
        if (static_cast<uint8>(level) < static_cast<uint8>(slot.filter.minimum) ||
            (level != LogLevel::Fatal && !slot.filter.category.Matches(category))) {
            continue;
        }

        CallbackGuard guard;
        if (!slot.sink->Write(event).IsOk()) {
            failed = true;
        }
    }
    return failed ? Result<void>::Failure(ErrorCode::DependencyFailed) : Result<void>::Success();
}

Result<void> TextLogSink::Write(const LogEvent& event) noexcept {
    static constexpr StringView names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Fatal"};
    static constexpr usize LongestLevelNameBytes = sizeof("Warning") - 1;
    static_assert(sizeof(names) / sizeof(names[0]) == LogLevelCount,
                  "every LogLevel needs a text name");
    // 카테고리도 문법(ASCII 영숫자·점, 길이 상한)을 검사한다. 손으로 만든 LogEvent가 줄바꿈 등을
    // 넣어 다른 기록처럼 보이는 줄을 만들지 못하게 한다.
    if (!IsValidLogLevel(event.level) ||
        ValidateLogCategoryName(event.category) != LogCategoryNameError::None ||
        !event.message.ValidateUtf8().IsOk() || output_.write == nullptr ||
        output_.flush == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(mutex_);
    // "[" 레벨 "][" 카테고리 "] "를 한 번의 write로 내기 위해 고정 버퍼에 모은다. 레벨 이름은
    // LongestLevelNameBytes, 카테고리는 MaxLogCategoryNameBytes 이하라 넘치지 않는다.
    char prefix[sizeof("[][] ") - 1 + LongestLevelNameBytes + MaxLogCategoryNameBytes]{};
    usize count = 0;
    const auto append = [&](StringView text) noexcept {
        for (usize index = 0; index < text.ByteCount(); ++index) {
            prefix[count++] = text.Data()[index];
        }
    };

    append("[");
    append(names[static_cast<uint8>(event.level)]);
    append("][");
    append(event.category);
    append("] ");
    if (!output_.write(output_.context, StringView(prefix, count)).IsOk() ||
        !output_.write(output_.context, event.message).IsOk() ||
        !output_.write(output_.context, "\n").IsOk() || !output_.flush(output_.context).IsOk()) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

namespace {
Result<void> WriteStderr(void*, StringView bytes) noexcept {
    if ((bytes.ByteCount() != 0 &&
         fwrite(bytes.Data(), 1, bytes.ByteCount(), stderr) != bytes.ByteCount()) ||
        ferror(stderr) != 0) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
    return Result<void>::Success();
}

Result<void> FlushStderr(void*) noexcept {
    return fflush(stderr) == 0 && ferror(stderr) == 0
               ? Result<void>::Success()
               : Result<void>::Failure(ErrorCode::DependencyFailed);
}
} // namespace

StderrLogSink::StderrLogSink() noexcept : sink_({nullptr, WriteStderr, FlushStderr}) {
}

Result<void> StderrLogSink::Write(const LogEvent& event) noexcept {
    return sink_.Write(event);
}
} // namespace rocks
