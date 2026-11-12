#pragma once

#include <Core/Memory/Allocator.h>
#include <Platform/Sync.h>

namespace rocks {

// Validate가 발견한 첫 손상이다. offset은 해당 guard 또는 payload의 시작에서 잰 byte다.
enum class DebugMemoryIssue : uint8 {
    None,
    FrontGuard,
    BackGuard,
    FreedPayload
};

struct DebugMemoryReport {
    DebugMemoryIssue issue = DebugMemoryIssue::None;
    const void* address = nullptr;
    usize offset = 0;
    uint32 tag = 0;
};

struct DebugAllocatorStats {
    AllocationStats allocations;
    usize quarantineBytes = 0;
    usize quarantineAllocations = 0;
};

// 메모리 범위 앞뒤의 guard와 제한된 quarantine의 해제 후 쓰기를 검사한다.
// 사용 예: DebugAllocator debug(backing); auto bytes = debug.Allocate(32, 16);
// 사용을 마치면 객체를 직접 소멸하고 debug.Deallocate(bytes.Value())로 반환한다.
// auto report = debug.Validate(); 성공 후 report.Value().issue를 확인한다.
// 기록과 사용자 저장소는 별도 allocation이며 backing은 이 객체보다 오래 살아야 한다.
//
// 일반 호출은 동기화된다. 사용자가 payload에 접근하는 동안 Validate/Deallocate가 동시에
// 그 메모리를 읽거나 쓰지 않도록 caller가 동기화한다. backing 콜백의 동일 스레드 재진입은
// Busy, Stats/파괴에서는 Fatal이다. backing이 다른 스레드의 이 allocator 호출 완료를 기다리거나
// 서로 다른 allocator를 역순으로 잠그는 것은 교착 위험이 있어 금지한다.
// 파괴 전 모든 호출을 종료하고 모든 live allocation을 반환해야 한다. 기록된 live allocation이
// 있으면 Fatal이다. backend 호출은 caller의 스레드에서 실행되므로 backing의 스레드 계약도 지킨다.
//
// guard는 인접 범위의 일부 손상만 감지한다. 임의 메모리 파괴, 레이스, 악의적인 backing의
// 잘못된 주소, backing에 이미 반환된 포인터 사용, 같은 주소 재사용 후 오래된 포인터는 보장하지
// 않는다. 해제된 payload에 이미 채워진 0xDD와 같은 값을 쓰면 변경을 감지할 수 없다.
// quarantine 한도는 payload byte 기준이며 기록·guard·정렬 여유는 포함하지 않는다.
// 스레드 전용 backing은 FlushQuarantine/파괴도 해당 스레드에서 해야 한다. 파괴 중 backing이
// 격리 저장소의 반환을 거부하면 Fatal이다.
class DebugAllocator final : public Allocator {
public:
    explicit DebugAllocator(Allocator& backing, usize quarantineBytes = 1024 * 1024) noexcept;
    ~DebugAllocator() noexcept override;

    DebugAllocator(const DebugAllocator&) = delete;
    DebugAllocator& operator=(const DebugAllocator&) = delete;
    DebugAllocator(DebugAllocator&&) = delete;
    DebugAllocator& operator=(DebugAllocator&&) = delete;

    // 반환 payload는 0xCD로 채운다. 유효한 정렬의 0-byte 요청은 nullptr다. 실패하면 기존
    // 할당과 통계는 유지한다. 이 함수는 저장소만 제공하며 객체를 생성하지 않는다.
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;

    // 정확한 live 주소만 반환한다. 외부·중복·중간 주소는 역참조하지 않고 NotFound다.
    // guard가 손상됐으면 Fatal이다. quarantine에 들어간 payload는 0xDD로 채운다.
    // backing의 즉시 반환이 실패하면 기존 주소와 payload를 유지하고 그 오류를 반환한다.
    // 한도 초과 시 이전 quarantine을 먼저 비울 수 있다. 반환 실패까지 이미 성공한 이전 정리는
    // 되돌리지 않지만 현재 live allocation은 유지한다. 즉시 반환 경로는 payload를 덮지 않는다.
    Result<void> Deallocate(void* address) noexcept override;

    // 현재 live 및 quarantine을 검사한다. Result는 호출 성공 여부이며 실제 손상 여부는
    // 성공 후 Value().issue로 확인한다. 보고 주소는 식별용이며 접근권을 제공하지 않는다.
    Result<DebugMemoryReport> Validate() const noexcept;

    // quarantine을 오래된 순서로 반환한다. guard/poison 손상은 Fatal이다. backing 반환이
    // 실패하면 오류를 반환하고 해당 블록 이후는 유지한다. 앞서 반환된 블록은 복원하지 않는다.
    Result<void> FlushQuarantine() noexcept;

    // 게시된 사용자 요청과 quarantine payload 크기의 한 시점 snapshot이다.
    DebugAllocatorStats Stats() const noexcept;

private:
    struct Record;
    struct EntryScope;

    static thread_local EntryScope* activeEntry_;

    DebugMemoryReport Inspect(const Record& record, bool quarantined) const noexcept;
    Result<void> EvictOne() noexcept;
    void CheckRecord(const Record& record, bool quarantined) const noexcept;
    void ReleaseRecord(Record* record) noexcept;

    Allocator& backing_;
    const usize quarantineLimit_;
    mutable platform::Mutex gate_;
    Record* live_ = nullptr;
    Record* quarantineHead_ = nullptr;
    Record* quarantineTail_ = nullptr;
    DebugAllocatorStats stats_{};
};

} // namespace rocks
