#pragma once

#include <Base/Types.h>

namespace rocks {

// 예외 대신 API 경계를 통과하는 실패 분류다. None만 성공 상태를 나타낸다.
// 세부 원인·재시도 가능성·실패 후 소유권은 각 API 계약에서 별도로 정한다.
// enum의 선언 순서를 영속 저장 포맷이나 외부 ABI 버전으로 간주하지 않는다.
enum class ErrorCode : uint32 {
    None,
    InvalidArgument,
    OutOfMemory,
    Overflow,
    Busy,
    NotFound,
    AlreadyExists,
    StaleHandle,
    WrongThread,
    WrongContext,
    Destroyed,
    Unsupported,
    CapacityExceeded,
    DependencyFailed,
    Cancelled,
    ShuttingDown,
    TypeMismatch
};

// 진단 출력용 열거자 이름이다(예: ErrorCode::NotFound → "NotFound"). 모르는 값은 "Unknown"이다.
// 반환 문자열은 정적 수명이며 NUL로 끝난다.
//
// 사용 예:
//   printf("load failed: %s\n", ErrorCodeName(result.Error()));
constexpr const char* ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None:
        return "None";
    case ErrorCode::InvalidArgument:
        return "InvalidArgument";
    case ErrorCode::OutOfMemory:
        return "OutOfMemory";
    case ErrorCode::Overflow:
        return "Overflow";
    case ErrorCode::Busy:
        return "Busy";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::AlreadyExists:
        return "AlreadyExists";
    case ErrorCode::StaleHandle:
        return "StaleHandle";
    case ErrorCode::WrongThread:
        return "WrongThread";
    case ErrorCode::WrongContext:
        return "WrongContext";
    case ErrorCode::Destroyed:
        return "Destroyed";
    case ErrorCode::Unsupported:
        return "Unsupported";
    case ErrorCode::CapacityExceeded:
        return "CapacityExceeded";
    case ErrorCode::DependencyFailed:
        return "DependencyFailed";
    case ErrorCode::Cancelled:
        return "Cancelled";
    case ErrorCode::ShuttingDown:
        return "ShuttingDown";
    case ErrorCode::TypeMismatch:
        return "TypeMismatch";
    }
    return "Unknown";
}

} // namespace rocks
