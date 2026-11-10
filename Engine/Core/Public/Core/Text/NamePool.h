#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Platform/Sync.h>

namespace rocks {

struct NamePoolConfig {
    uint64 maxEntries = 65536;
    // entry metadata와 마지막 NUL을 포함한 요청 storage byte 예산이다. allocator 자체 비용 제외.
    // 게시된 entry와 아직 게시되지 않은 후보 예약의 합에 적용하여 경합 시 임시 할당도 제한한다.
    usize maxStorageBytes = 16 * 1024 * 1024;
    // 충돌 회귀를 위한 축소 mask다. 0이면 모든 이름이 같은 hash이며 byte 비교로 구분한다.
    // 기본값을 성능/보안 보장으로 해석하지 않는다. hash와 id는 공개 저장 형식이 아니다.
    uint64 hashMask = static_cast<uint64>(-1);
};

// gate에서 복사한 bookkeeping snapshot이다. pending은 실제 할당 전 예약도 포함한다.
// reclaiming 중 게시 count/byte는 detach한 전체 묶음 값이며 회수 완료 시 함께 0이 된다.
// 한 snapshot만으로 다른 caller의 종료나 allocator 전체 메모리 상태를 증명하지 않는다.
struct NamePoolStats {
    uint64 domain = 0;
    uint64 entryCount = 0;
    usize storageBytes = 0;
    uint64 pendingEntries = 0;
    usize pendingStorageBytes = 0;
    uint64 activeOperations = 0;
    uint64 activeLeases = 0;
    bool accepting = false;
    bool reclaiming = false;
};

// 유효 UTF-8을 immutable entry로 intern한다. case-sensitive byte 동등 비교이며 정규화하지 않는다.
// 64 bucket/단일 gate의 기준 구현이다. bucket 분리는 후속 shard화 여지를 남기지만 현재 병렬 shard
// 잠금은 구현하지 않았다. hash 충돌은 전체 byte 비교로 해결하며 성능 우위를 보장하지 않는다.
// pool은 allocator를 빌린다. 병행 사용에는 thread-safe allocator가 필요하다. allocator의 호출과
// 후보/entry 해제는 gate 밖에서 한다. 콜백은 Stats/Shutdown에 재진입 가능하지만 pool 파괴는 금지다.
class NamePool {
public:
    explicit NamePool(Allocator& allocator) noexcept;
    ~NamePool() noexcept;
    NamePool(const NamePool&) = delete;
    NamePool& operator=(const NamePool&) = delete;
    NamePool(NamePool&&) = delete;
    NamePool& operator=(NamePool&&) = delete;

    // 중지된 pool만 초기화한다. 새 process-local domain으로 이전 token을 구별한다.
    // domain 소진은 Overflow이며 wrap/reuse하지 않는다. 생성/초기화는 entry storage를 할당하지
    // 않는다.
    Result<void> Initialize(NamePoolConfig config = {}) noexcept;

    // 호출 중 source storage가 유효하고 변하지 않아야 한다. 빈 문자열과 embedded NUL도 유효하다.
    // 후보 할당 전후 중복을 검사한다. OOM/한도/종료 실패 시 게시된 entry는 변하지 않는다.
    // 다른 후보의 예약 때문에 잠시 CapacityExceeded일 수 있다. 성공한 동일 byte 입력의 Name은 같다.
    Result<Name> TryIntern(StringView source) noexcept;

    // 현재 domain과 게시 id를 함께 검사한다. foreign/이전 token은 StaleHandle이다.
    // gate에서 lease를 먼저 확보하므로 성공 후 Shutdown은 lease 반환까지 Busy다.
    Result<NameLease> Acquire(Name name) noexcept;

    // admission을 먼저 닫는다. operation cleanup 또는 lease가 남으면 Busy이며 닫힌 상태를 유지한다.
    // lease 반환/호출 종료 후 다시 호출한다. 성공은 모든 entry 해제 완료를 뜻한다.
    // 소멸 전 모든 caller를 join해야 하며, 소멸 시 Busy는 수명 계약 위반으로 Fatal 처리한다.
    Result<void> Shutdown() noexcept;
    NamePoolStats Stats() const noexcept;

private:
    friend class NameLease;
    struct Entry;
    enum class State {
        Stopped,
        Initializing,
        Running,
        Closing,
        Reclaiming
    };
    static constexpr usize BucketBits = 6;
    static constexpr usize BucketCount = static_cast<usize>(1) << BucketBits;

    Entry* FindBytes(StringView source, uint64 hash) const noexcept;
    void FinishOperation(usize reservedBytes) noexcept;
    void ReleaseLease() noexcept;
    void FreeEntry(Entry* entry) noexcept;

    Allocator* allocator_;
    mutable platform::Mutex gate_;
    Entry* buckets_[BucketCount]{};
    NamePoolConfig config_{};
    NamePoolStats stats_{};
    State state_ = State::Stopped;
    uint64 nextSerial_ = 1;
};

} // namespace rocks
