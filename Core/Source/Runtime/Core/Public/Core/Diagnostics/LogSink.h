#pragma once

#include <Core/Diagnostics/LogRecord.h>
#include <Core/Result.h>

namespace rocks {

// 힙·백그라운드 스레드·Task 없이 사용할 초기 진단 버퍼다. 생성 스레드가 모든 접근을 소유한다.
// 내부 동기화가 없으며 Size/Dropped도 소유 스레드에서 읽어야 한다.
class LogBuffer {
public:
    static constexpr usize Capacity = 128;
    LogBuffer() noexcept;
    // text를 즉시 복사한다. 포화 시 기존 기록을 덮지 않고 Dropped만 증가시켜 실패를 반환한다.
    // 잘못된 스레드·null 문자열은 상태를 바꾸지 않는다. alreadyTruncated는 상위 단계의 손실 표시다.
    Result<void> Write(LogLevel, uint32 category, const char* text,
                       DiagnosticKind kind = DiagnosticKind::Message, uint64 duration = 0,
                       bool alreadyTruncated = false) noexcept;
    // 기록 수와 손실 카운터를 초기화한다. 이전 At 결과의 논리 유효성도 여기서 끝난다.
    Result<void> Reset() noexcept;
    // 버퍼를 소유하지 않는 읽기 뷰다. Reset/버퍼 소멸 뒤 보관하지 않는다.
    // 다른 스레드 또는 범위 밖 접근은 nullptr를 반환한다.
    const LogRecord* At(usize index) const noexcept;

    usize Size() const noexcept {
        return size_;
    }

    uint64 Dropped() const noexcept {
        return dropped_;
    }

private:
    uint64 owner_;
    usize size_ = 0;
    uint64 dropped_ = 0;
    LogRecord records_[Capacity]{};
};
} // namespace rocks
