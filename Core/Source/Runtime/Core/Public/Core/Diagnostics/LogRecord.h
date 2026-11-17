#pragma once

#include <Base/Types.h>

namespace rocks {

// 로그 기록의 심각도다. 값이 클수록 심각하며, 필터는 "최소 레벨 이상"을 통과시킨다
// (Docs/Design/LoggingSystem.ko.md 7절). 값은 실행 중 비교용이며 파일·네트워크 형식이 아니다.
// - Trace: 단계별 추적처럼 아주 자세한 개발용 기록
// - Debug: 개발 중 원인 파악에 쓰는 자세한 기록
// - Info: 정상 동작의 주요 사건(시작·종료·설정 결과)
// - Warning: 계속 동작하지만 살펴볼 이상(대체 경로 선택·지연)
// - Error: 요청한 작업이 실패한 사건
// - Fatal: 프로세스를 끝내야 하는 사건. 이 값으로 기록해도 기록 경로가 종료하지는 않는다
//   (의도적 종료는 후속 단계의 Log::Fatal이 맡는다).
//
// 사용 예:
//   logger.Write(LogLevel::Warning, LogEngine, "Frame took too long");
enum class LogLevel : uint8 {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Fatal
};

// LogLevel에 정의된 값의 수다. 레벨별 표(이름·개수)의 크기로 쓴다.
inline constexpr usize LogLevelCount = static_cast<usize>(LogLevel::Fatal) + 1;

// LogLevel에 정의된 값인지 확인한다. 외부에서 숫자로 받은 레벨(설정·시험)을 쓰기 전에 검사한다.
constexpr bool IsValidLogLevel(LogLevel level) noexcept {
    return static_cast<usize>(level) < LogLevelCount;
}

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
