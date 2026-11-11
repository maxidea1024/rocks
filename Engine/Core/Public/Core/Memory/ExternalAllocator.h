#pragma once

#include <Core/Lifetime/ModuleLease.h>
#include <Core/Memory/Allocator.h>
#include <Platform/Sync.h>

namespace rocks {

// 외부 라이브러리에 맞는 래퍼 함수를 제공한다. context는 정적 backend이면 nullptr도 된다.
// callback과 context, module은 ExternalAllocator 및 모든 진행 중 호출보다 오래 살아야 한다.
// 외부 모듈의 함수라면 module에 해당 모듈 lease를 넣는다. 정적 링크는 빈 lease도 허용한다.
// allocate는 서로 겹치지 않는 정렬된 저장소 또는 오류를 반환해야 한다. deallocate가 실패하면
// 주소와 내용은 그대로 유효해야 한다. callback은 noexcept이며 동시 호출을 지원해야 한다.
struct ExternalAllocatorBackend {
    using Allocate = Result<void*> (*)(void*, usize, usize, uint32) noexcept;
    using Deallocate = Result<void> (*)(void*, void*) noexcept;

    void* context = nullptr;
    Allocate allocate = nullptr;
    Deallocate deallocate = nullptr;
    ModuleLease module;
};

// 외부 allocator를 전역 malloc 교체 없이 Rocks Allocator로 연결한다.
// 예: ExternalAllocator allocator(Move(backend)); Array<int32> values(allocator);
// 실제 공급자 라이브러리와 위 callback 구현은 별도로 연결해야 한다. callback이 없으면
// IsAvailable()은 false이며 비영 할당은 Unsupported다. 외부 라이브러리를 자동 로드하지 않는다.
// 기록 잠금은 짧게 보유하며 backend 호출은 잠금 밖에서 병렬 실행한다. 동일 스레드에서 같은
// allocator로 재진입하면 Busy다. 같은 주소의 동시 반환 중 추가 반환도 Busy다.
// backend 콜백이 다른 스레드의 같은 allocator 작업 완료를 기다리면 교착할 수 있어 금지한다.
// 선택적 metadata allocator도 이 객체보다 오래 살아야 하며 병렬 호출을 지원해야 한다.
// metadata allocator 및 그 callback은 이 allocator에 의존하는 순환 구성을 만들면 안 된다.
// metadata와 backend는 서로의 살아 있는 저장소와 겹치지 않는 메모리를 반환해야 한다.
// 악의적이거나 이 계약을 위반하는 공급자의 모든 오류를 격리하지는 못한다.
// 객체 파괴 전 모든 호출을 끝내고 받은 주소를 반환해야 한다. 추적 중 작업이나 살아 있는
// allocation은 Fatal이다. 조회·조기 반환 호출까지 파괴와 경합하지 않도록 caller가 보장한다.
class ExternalAllocator final : public Allocator {
public:
    // metadata를 생략하면 플랫폼 할당을 사용한다. 별도로 지정하면 기록 확보 실패를
    // 주입할 수 있다. 기록 확보 실패 시 backend는 호출하지 않는다.
    explicit ExternalAllocator(ExternalAllocatorBackend backend,
                               Allocator* metadataAllocator = nullptr) noexcept;
    ~ExternalAllocator() noexcept override;

    ExternalAllocator(const ExternalAllocator&) = delete;
    ExternalAllocator& operator=(const ExternalAllocator&) = delete;
    ExternalAllocator(ExternalAllocator&&) = delete;
    ExternalAllocator& operator=(ExternalAllocator&&) = delete;

    bool IsAvailable() const noexcept;

    // 유효한 alignment의 0-byte 요청은 nullptr다. 오류 시 backend 오류를 보존한다.
    // 반환된 주소는 이 allocator로만 반환하고, 객체 소멸·동시 사용 종료는 caller 책임이다.
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;

    // nullptr는 성공한다. backend가 없으면 비null 반환은 Unsupported다. 사용 가능한 backend의
    // 소유하지 않는 주소와 중복 반환은 역참조 없이 NotFound다.
    // backend 반환 실패 시 주소와 소유 기록을 유지한다. raw 주소가 재사용된 뒤에는 이전
    // 포인터와 새 allocation을 구분할 수 없다.
    Result<void> Deallocate(void* address) noexcept override;

    // 한 시점에 게시된 payload 기준 통계다. 호출 중 allocation이나 메타데이터는 포함하지 않는다.
    // 스냅샷을 다른 스레드의 호출 완료나 allocator 파괴 가능 여부로 사용하면 안 된다.
    AllocationStats Stats() const noexcept;

private:
    struct Record;
    struct EntryScope;
    struct FlightScope;

    static thread_local EntryScope* activeEntry_;
    Result<void> BeginFlight() noexcept;
    void EndFlight() noexcept;
    Result<void*> AllocateRecord() noexcept;
    void FreeRecord(void* storage) noexcept;

    ExternalAllocatorBackend backend_;
    Allocator* metadataAllocator_;
    mutable platform::Mutex gate_;
    platform::ConditionVariable changed_;
    Record* records_ = nullptr;
    AllocationStats stats_{};
    uint64 inFlight_ = 0;
};

} // namespace rocks
