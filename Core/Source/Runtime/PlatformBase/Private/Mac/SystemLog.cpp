#include <Platform/SystemLog.h>

namespace rocks::platform {

// OS 시스템 로그 sink를 따로 두지 않는 플랫폼이다(로그는 stderr·파일로 간다).

bool HasSystemLog() noexcept {
    return false;
}

void WriteSystemLogLine(SystemLogLevel, const char*) noexcept {
}

} // namespace rocks::platform
