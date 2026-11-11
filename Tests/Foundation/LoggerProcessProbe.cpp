#include <Core/Diagnostics/Logger.h>
#include <stdio.h>

namespace rocks::test {
bool ConfigureLoggerFailureProbe() noexcept;

// 오직 별도 child에서 실행한다. stderr를 쓸 수 없는 상태로 만들어 실패 전파를 검사한다.
int RunLoggerFailureProbe() noexcept {
    if (!ConfigureLoggerFailureProbe()) {
        return 2;
    }
    Logger logger;
    StderrLogSink sink;
    if (!logger.Add(sink).IsOk()) {
        return 3;
    }
    const auto result = logger.Write(LogLevel::Error, 0, "broken pipe");
    if (!logger.Remove(sink).IsOk()) {
        return 4;
    }
    return result.Error() == ErrorCode::DependencyFailed ? 0 : 5;
}
} // namespace rocks::test
