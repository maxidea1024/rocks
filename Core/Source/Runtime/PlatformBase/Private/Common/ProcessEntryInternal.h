#pragma once

#include <Platform/ProcessEntry.h>

// 종료 요청 처리의 공용 상태다. OS 처리 문맥(Windows 콘솔 제어 스레드, POSIX 신호 처리기)의 등록·
// 해제는 Private/<플랫폼>/ProcessEntry.cpp가 맡고, 첫 요청·두 번째 요청 판정은 여기서 한 번만 한다.
namespace rocks::platform::detail {

// 요청 하나를 받은 뒤 OS 처리기가 할 일이다.
enum class TerminationRequestOutcome : uint8 {
    Handled,      // 처리기를 불렀다. 프로세스를 끝내지 않는다
    PassToSystem, // 처리기가 없거나 두 번째 이후 요청이다. OS 기본 동작(즉시 종료)으로 넘긴다
};

// 처리기 칸을 handler로 채운다(비어 있을 때만). 받은 요청 수를 0으로 둔다. 이미 차 있거나 handler가
// nullptr이면 false다.
bool AcquireTerminationRequestHandler(TerminationRequestHandler handler) noexcept;

// 처리기 칸을 비우고 받은 요청 수를 지운다.
void ReleaseTerminationRequestHandler() noexcept;

// OS 처리 문맥에서 요청 하나를 판정한다. 첫 요청이면 처리기를 부르고 Handled다(원자 연산만 쓰므로
// async-signal-safe). 시험은 OS 신호 없이 이 판정을 직접 부른다.
TerminationRequestOutcome DispatchTerminationRequest() noexcept;

} // namespace rocks::platform::detail
