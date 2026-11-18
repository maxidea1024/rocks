#pragma once

#include <Base/Checked.h>
#include <Core/Diagnostics/LogCategory.h>
#include <Core/Memory/Allocator.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <SpatialIndex/LooseOctree.h>

namespace rocks::spatial {

inline constexpr LogCategory LogSpatialIndex{"SpatialIndex"};

// 프로세스 전역에서 한 번도 쓰지 않은 변경 세대를 돌려준다(1부터 늘어난다). 트리마다 세대를 따로
// 세면 이동 대입이나 같은 주소에 다시 만든 트리가 우연히 같은 세대를 가져 다른 트리에서 모은 노드
// 묶음을 받아들일 수 있다. 전역 단조 계수는 생성·이동·대입·변경마다 새 값을 주므로
// SpatialNodeBatch의 (트리 주소, 세대) 쌍이 다른 트리 상태와 겹치지 않는다. 값이 겹치지 않기만 하면
// 되고 다른 메모리와의 순서는 필요 없으므로(묶음의 발행·소비 순서는 읽기 범위의 atomic이 맡는다)
// relaxed 증가로 충분하다.
uint64 NextMutationEpoch() noexcept;

// 옥트리가 받은 allocator를 감싸 내부 컨테이너의 할당·해제 횟수와 살아 있는·피크 byte를
// 센다(6.10절). 해제는 주소만 받으므로 크기를 알 수 없어, 할당마다 사용자 영역 앞에 머리 정보(요청
// 크기, 머리 크기)를 둔다. 머리 크기는 max(정렬, 16)이라 사용자 영역의 정렬이 지켜진다. 0 byte
// 할당은 그대로 넘긴다. 변경 단계에서만 불리므로(한 스레드) 동기화하지 않는다. 계수는 읽기 단계에서
// 읽는다.
class detail::CountingAllocator final : public Allocator {
public:
    explicit CountingAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    CountingAllocator(const CountingAllocator&) = delete;
    CountingAllocator& operator=(const CountingAllocator&) = delete;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (bytes == 0) {
            return backing_.Allocate(0, alignment, tag);
        }
        const usize header = alignment > HeaderBytes ? alignment : HeaderBytes;
        usize total = 0;
        if (!TryAdd(bytes, header, total)) {
            return Result<void*>::Failure(ErrorCode::Overflow);
        }
        auto allocated = backing_.Allocate(total, header, tag);
        if (!allocated) {
            return allocated;
        }
        if (allocated.Value() == nullptr) {
            platform::Fatal("Allocator returned null for a nonzero allocation");
        }
        unsigned char* user = static_cast<unsigned char*>(allocated.Value()) + header;
        usize* meta = reinterpret_cast<usize*>(user) - 2;
        meta[0] = bytes;
        meta[1] = header;
        ++allocations_;
        liveBytes_ += bytes;
        peakBytes_ = liveBytes_ > peakBytes_ ? liveBytes_ : peakBytes_;
        return Result<void*>::Success(user);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == nullptr) {
            return backing_.Deallocate(nullptr);
        }
        const usize* meta = reinterpret_cast<const usize*>(address) - 2;
        const usize bytes = meta[0];
        const usize header = meta[1];
        auto released = backing_.Deallocate(static_cast<unsigned char*>(address) - header);
        if (released) {
            ++deallocations_;
            liveBytes_ -= bytes;
        }
        return released;
    }

    uint64 Allocations() const noexcept {
        return allocations_;
    }

    uint64 Deallocations() const noexcept {
        return deallocations_;
    }

    usize LiveBytes() const noexcept {
        return liveBytes_;
    }

    usize PeakBytes() const noexcept {
        return peakBytes_;
    }

private:
    // 머리 정보(usize 두 칸)가 들어가고 흔한 최대 정렬(16)을 지키는 최소 머리 크기다.
    static constexpr usize HeaderBytes = 16;

    Allocator& backing_;
    uint64 allocations_ = 0;
    uint64 deallocations_ = 0;
    usize liveBytes_ = 0;
    usize peakBytes_ = 0;
};

// 변경 함수 하나의 범위다(6.9절 위반 검출). writer 표시를 세운 뒤 reader 수를 읽고,
// SpatialReadScope는 reader 수를 올린 뒤 writer 표시를 읽는다. 두 쪽 모두 쓰기 뒤 읽기를 seq_cst로
// 하므로 겹치면 적어도 한쪽이 상대를 본다.
class LooseOctree::WriterGuard {
public:
    explicit WriterGuard(LooseOctree& tree) noexcept : tree_(tree) {
        if (platform::AtomicExchange(&tree.writer_, 1u) != 0) {
            platform::Fatal("LooseOctree mutation overlaps another mutation");
        }
        if (platform::AtomicLoad(&tree.readers_) != 0) {
            platform::Fatal("LooseOctree mutation while a read scope is open");
        }
        // 이전 읽기 단계에서 모은 노드 묶음을 낡은 것으로 만든다.
        tree.mutationEpoch_ = NextMutationEpoch();
    }

    // 변경이 끝날 때마다 배열별 예약 용량 피크를 갱신한다.
    ~WriterGuard() noexcept {
        tree_.NotePeaks();
        platform::AtomicStore(&tree_.writer_, 0u);
    }

    WriterGuard(const WriterGuard&) = delete;
    WriterGuard& operator=(const WriterGuard&) = delete;

private:
    LooseOctree& tree_;
};

// 삽입 위치 계획이다. 트리를 바꾸지 않고 계산한다(6.5절 수용 판정).
struct LooseOctree::Placement {
    // 루트 확장 한도 안에 들어가지 않는다. 범위 밖 목록에 넣는다.
    bool outOfRange = false;
    // 빈 트리라 루트를 이 프록시 중심의 initialRootLevel 격자 셀로 다시 잡는다.
    bool reset = false;
    int64 resetOrigin[3] = {0, 0, 0};
    // 루트 확장 단계 수다.
    uint32 expansions = 0;
    // 확장에 새 블록이 필요한가(루트에 내용이 있다).
    bool expansionNeedsBlocks = false;
};

} // namespace rocks::spatial
