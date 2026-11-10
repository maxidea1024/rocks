#pragma once

#include <Base/Types.h>

namespace rocks {

enum class LogLevel : uint8 {
    Trace,
    Info,
    Warning,
    Error
};
enum class DiagnosticKind : uint8 {
    Message,
    ScopeBegin,
    ScopeEnd
};

// 메시지는 외부 문자열을 참조하지 않고 레코드 안에 복사한다. 시간 값의 단위는 ns이며
// timestamp는 단조 시계 값이다. text는 최대 119바이트로 잘릴 수 있어 UTF-8 문자 경계는 보장하지
// 않는다.
struct LogRecord {
    uint64 timestamp = 0;
    uint64 thread = 0;
    uint64 duration = 0;
    uint32 category = 0;
    LogLevel level = LogLevel::Info;
    DiagnosticKind kind = DiagnosticKind::Message;
    bool truncated = false;
    char text[120]{};
};
} // namespace rocks
