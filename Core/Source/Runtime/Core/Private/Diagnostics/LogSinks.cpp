#include <Core/Diagnostics/RuntimeLogSink.h>
#include <stdio.h>

namespace rocks {

Result<void> TextLogSink::Write(const LogEvent& event) noexcept {
    static constexpr usize LongestLevelNameBytes = sizeof("Warning") - 1;
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
    append(LogLevelName(event.level));
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
