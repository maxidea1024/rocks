#include <Platform/SystemLog.h>

#include <android/log.h>

namespace rocks::platform {

namespace {

// logcat tag다.
constexpr char SystemLogTag[] = "Rocks";

int PriorityOf(SystemLogLevel level) noexcept {
    switch (level) {
    case SystemLogLevel::Verbose:
        return ANDROID_LOG_VERBOSE;
    case SystemLogLevel::Debug:
        return ANDROID_LOG_DEBUG;
    case SystemLogLevel::Info:
        return ANDROID_LOG_INFO;
    case SystemLogLevel::Warning:
        return ANDROID_LOG_WARN;
    case SystemLogLevel::Error:
        return ANDROID_LOG_ERROR;
    case SystemLogLevel::Fatal:
        return ANDROID_LOG_FATAL;
    }

    return ANDROID_LOG_INFO;
}

} // namespace

bool HasSystemLog() noexcept {
    return true;
}

void WriteSystemLogLine(SystemLogLevel level, const char* text) noexcept {
    __android_log_write(PriorityOf(level), SystemLogTag, text);
}

} // namespace rocks::platform
