#include "LogcatSink.h"

#include <Base/PlacementNew.h>
#include <Core/Diagnostics/LogEvent.h>
#include <Platform/Memory.h>
#include <Platform/SystemLog.h>

namespace rocks::detail {

namespace {

// 렌더 버퍼다("[Category] message"). 메시지가 더 길면 머리말 줄 뒤에 조각으로 나눠 쓴다.
constexpr usize LogcatRenderBytes = 1024;

// UTF-8 연속 바이트(10xxxxxx)다. 조각을 글자 중간에서 자르지 않는다.
constexpr uint8 Utf8ContinuationMask = 0xC0;
constexpr uint8 Utf8ContinuationBits = 0x80;

platform::SystemLogLevel SystemLevelOf(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return platform::SystemLogLevel::Verbose;
    case LogLevel::Debug:
        return platform::SystemLogLevel::Debug;
    case LogLevel::Info:
        return platform::SystemLogLevel::Info;
    case LogLevel::Warning:
        return platform::SystemLogLevel::Warning;
    case LogLevel::Error:
        return platform::SystemLogLevel::Error;
    case LogLevel::Fatal:
        return platform::SystemLogLevel::Fatal;
    }

    return platform::SystemLogLevel::Info;
}

// 기록 하나를 OS 시스템 로그 줄로 쓰는 sink다. 스택 버퍼만 써서 여러 LogSystem이 동시에 불러도
// 된다.
class SystemLogSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        const platform::SystemLogLevel level = SystemLevelOf(event.level);
        char line[LogcatRenderBytes];
        usize used = 0;
        auto append = [&](const char* text, usize bytes) noexcept {
            const usize room = sizeof(line) - 1 - used;
            const usize count = bytes < room ? bytes : room;
            platform::MemoryCopy(line + used, text, count);
            used += count;
        };
        append("[", 1);
        append(event.category.Data(), event.category.ByteCount());
        append("] ", 2);
        const char* message = event.message.Data();
        usize remaining = event.message.ByteCount();
        if (used + remaining < sizeof(line)) {
            append(message, remaining);
            line[used] = '\0';
            platform::WriteSystemLogLine(level, line);
            return Result<void>::Success();
        }

        // 긴 메시지: 머리말 줄 뒤에 MaxSystemLogLineBytes 조각으로 나눠 쓴다.
        line[used] = '\0';
        platform::WriteSystemLogLine(level, line);
        char chunk[platform::MaxSystemLogLineBytes + 1];
        while (remaining != 0) {
            const usize limit = remaining < platform::MaxSystemLogLineBytes
                                    ? remaining
                                    : platform::MaxSystemLogLineBytes;
            usize bytes = limit;
            while (bytes < remaining && bytes != 0 &&
                   (static_cast<uint8>(message[bytes]) & Utf8ContinuationMask) ==
                       Utf8ContinuationBits) {
                --bytes;
            }

            if (bytes == 0) {
                bytes = limit;
            }

            platform::MemoryCopy(chunk, message, bytes);
            chunk[bytes] = '\0';
            platform::WriteSystemLogLine(level, chunk);
            message += bytes;
            remaining -= bytes;
        }

        return Result<void>::Success();
    }
};

alignas(SystemLogSink) unsigned char systemLogSinkStorage[sizeof(SystemLogSink)];
SystemLogSink* systemLogSink = nullptr;

} // namespace

RuntimeLogSink* AndroidLogcatSink() noexcept {
    if (!platform::HasSystemLog()) {
        return nullptr;
    }

    if (systemLogSink == nullptr) {
        systemLogSink = ::new (systemLogSinkStorage) SystemLogSink();
    }

    return systemLogSink;
}

} // namespace rocks::detail
