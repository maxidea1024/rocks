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
    TypeMismatch,
    // 인증·무결성 검증 실패다. 메시지에서 온 byte의 tag·MAC·서명·commitment 불일치, 잘린 스트림,
    // 잘못된 키, 복호 경로의 형식 위반이 모두 이 값 하나다(어느 검사가 실패했는지 드러내지 않는다).
    // 호출자가 소유한 버퍼의 길이 오류 같은 API 오용은 InvalidArgument로
    // 구분한다(Docs/Design/Crypto.ko.md 2·11절).
    AuthenticationFailed,
    // 의존 그래프에 간선을 더하면 순환이 생겨 거부했다(예: Tick 선행 의존). 상태는 바뀌지 않는다.
    // 다른 InvalidArgument(자기 자신·다른 단계)와 구분해 호출자가 순환만 따로 진단할 수 있다.
    CycleDetected,
    // 대상 장치·backend·기능 집합이 이 요청에 필요한 능력을 갖추지 않았다(예: 셰이더 형식을 만들 수
    // 없는 backend, 아직 계약만 있는 ReadyMask::Render). 인자는 올바르며 능력이 있는 대상에서는
    // 성공한다. 기능 자체를 구현하지 않은 Unsupported와 구분한다(Docs/Design/AssetSystem.ko.md 1절
    // A1, ShaderSystem.ko.md 5.2절).
    UnsupportedCapability,
    // 한 번만 등록할 수 있는 자리에 이미 등록되어 있다(예: LoadTicket 하나에 OnReady 두 번). 상태는
    // 바뀌지 않으며 두 번째 등록의 인자(callback 소유권 등)는 호출자에게
    // 남는다(AssetSystem 11.1절).
    AlreadyRegistered,
    // 요청이 속한 수명 범위(AssetScope 등)가 닫혔다(Revoke). 그 범위의 새 접근·발급은 거부되고 진행
    // 중이던 callback은 이 값을 한 번 받는다(AssetSystem 11.1절).
    ScopeClosed,
    // 대상이 아직 준비되지 않았다(로드 중이거나 상주하지 않음). 호출자가 명시 실패 정책을 골랐을 때
    // 돌려준다 — 보류·우선 로드는 다른 정책이다(AssetSystem 11.7절 9).
    NotReady
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
    case ErrorCode::AuthenticationFailed:
        return "AuthenticationFailed";
    case ErrorCode::CycleDetected:
        return "CycleDetected";
    case ErrorCode::UnsupportedCapability:
        return "UnsupportedCapability";
    case ErrorCode::AlreadyRegistered:
        return "AlreadyRegistered";
    case ErrorCode::ScopeClosed:
        return "ScopeClosed";
    case ErrorCode::NotReady:
        return "NotReady";
    }
    return "Unknown";
}

} // namespace rocks
