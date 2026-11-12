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

} // namespace rocks
