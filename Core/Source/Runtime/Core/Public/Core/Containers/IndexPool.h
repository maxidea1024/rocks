#pragma once

#include <Base/NumericLimits.h>
#include <Base/TypeTraits.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

// 크기가 고정된 원소를 평탄한 연속 저장소에 두고 uint32 색인으로 할당·반환하는 풀이다. 반환한
// 색인은 재사용 목록(LIFO)으로 돌아가 다음 할당에 먼저 쓰인다. 포인터가 아니라 색인을 돌려주므로
// 저장소가 커져 재배치되어도 색인은 유효하고, 색인끼리 연결한 자료구조(노드 블록·청크 연결 목록)를
// 그대로 둘 수 있다. 풀 자체도 이동할 수 있다.
//
// PoolAllocator와의 차이: PoolAllocator는 Allocator 인터페이스로 포인터를 돌려주고 모든 호출을
// 동기화하며 이동할 수 없다. IndexPool은 단일 소유자의 자료구조 내부 저장소이고 동기화하지 않는다.
//
// 계약
// - T는 trivially copyable·trivially destructible이어야 한다(원소를 바이트로 옮겨도 되는 레코드).
//   할당한 원소는 값 초기화(T{})된 상태다.
// - 색인은 [0, SlotCount())이며 MaxElements 미만이다. 생성자의 maxElements로 상한을 더 낮출 수 있다
//   (예: 색인 × 8 + 1이 uint32에 들어가야 하는 노드 블록). 상한을 넘는 확보는 Overflow다.
// - 확보(TryReserveAdditional) 뒤의 AllocateReserved와 Free는 할당하지 않으므로 실패하지 않는다.
// 여러
//   저장소를 함께 바꾸는 caller는 필요한 수를 모두 먼저 확보한 뒤 바꿔 실패 시 상태를 보존한다.
// - 원소 참조·포인터는 저장소가 커지면(확보·TryAllocate) 무효다. 색인은 Free·Clear 전까지 유효하다.
// - operator[]는 범위만 검사하고 살아 있는지는 검사하지 않는다(검색 경로 비용). 반환한 색인에 다시
//   접근하는 것은 caller의 계약 위반이며, 살아 있는지 확인이 필요하면 IsLive를 쓴다.
// - 제거해도 저장소를 줄이지 않는다. TryShrink만 용량을 사용 높이(SlotCount)에 맞춘다.
// - 이동하면 원소·색인·재사용 목록이 대상으로 옮겨 가고 원본은 비어 같은 allocator로 다시 쓸 수
// 있다.
// - 동시 접근의 동기화는 caller 책임이다. 읽기만 하는 여러 스레드는 동시에 operator[] const를 써도
//   된다.
//
// 사용 예:
//   IndexPool<Chunk> chunks(allocator);
//   auto reserved = chunks.TryReserveAdditional(2);   // 실패하면 아무것도 바뀌지 않았다
//   if (!reserved) {
//       return reserved;
//   }
//   const uint32 first = chunks.AllocateReserved();   // 실패하지 않는다
//   const uint32 second = chunks.AllocateReserved();
//   chunks[first].next = second;
//   ...
//   chunks.Free(second);   // 다음 할당이 second를 다시 준다
template <typename T>
class IndexPool {
    static_assert(IsTriviallyCopyableV<T>, "IndexPool elements must be trivially copyable");
    static_assert(IsTriviallyDestructibleV<T>, "IndexPool elements must be trivially destructible");

    // links_[i]가 Live면 i가 할당되어 있다. 그 밖에는 재사용 목록의 다음 색인(EndOfList 포함)이다.
    static constexpr uint32 EndOfList = NumericLimits<uint32>::Max();
    static constexpr uint32 Live = EndOfList - 1;

public:
    // 색인 칸에 쓰는 "없음" 표지다. 어떤 할당도 이 값을 돌려주지 않는다.
    static constexpr uint32 NoIndex = NumericLimits<uint32>::Max();
    // 만들 수 있는 원소 수의 상한이다(색인은 이보다 작다).
    static constexpr uint32 MaxElements = Live;

    // maxElements는 원소 수 상한이며 0이거나 MaxElements보다 크면 Fatal이다.
    explicit IndexPool(Allocator& allocator, uint32 maxElements = MaxElements) noexcept
        : elements_(allocator), links_(allocator), maxElements_(maxElements) {
        if (maxElements == 0 || maxElements > MaxElements) {
            platform::Fatal("IndexPool maxElements must be in [1, MaxElements]");
        }
    }

    IndexPool(const IndexPool&) = delete;
    IndexPool& operator=(const IndexPool&) = delete;

    IndexPool(IndexPool&& other) noexcept
        : elements_(Move(other.elements_)), links_(Move(other.links_)),
          firstFree_(other.firstFree_), freeCount_(other.freeCount_),
          maxElements_(other.maxElements_) {
        other.firstFree_ = EndOfList;
        other.freeCount_ = 0;
    }

    IndexPool& operator=(IndexPool&& other) noexcept {
        if (this != &other) {
            elements_ = Move(other.elements_);
            links_ = Move(other.links_);
            firstFree_ = other.firstFree_;
            freeCount_ = other.freeCount_;
            maxElements_ = other.maxElements_;
            other.firstFree_ = EndOfList;
            other.freeCount_ = 0;
        }
        return *this;
    }

    ~IndexPool() noexcept = default;

    Allocator& GetAllocator() const noexcept {
        return elements_.GetAllocator();
    }

    // 지금까지 만든 원소 자리 수(할당 중 + 재사용 목록)다. 색인은 이보다 작다.
    uint32 SlotCount() const noexcept {
        return static_cast<uint32>(elements_.Size());
    }

    // 할당 중인 원소 수다.
    uint32 LiveCount() const noexcept {
        return SlotCount() - freeCount_;
    }

    // 재사용 목록 길이다.
    uint32 FreeCount() const noexcept {
        return freeCount_;
    }

    // 재할당 없이 담을 수 있는 원소 자리 수다.
    usize Capacity() const noexcept {
        return elements_.Capacity();
    }

    uint32 MaxElementCount() const noexcept {
        return maxElements_;
    }

    // 할당하지 않고 이어서 AllocateReserved를 부를 수 있는 횟수다.
    uint64 ReservedAvailable() const noexcept {
        return static_cast<uint64>(freeCount_) + (elements_.Capacity() - elements_.Size());
    }

    // 다음 count번의 AllocateReserved가 할당 없이 성공하도록 저장소를 확보한다. 이미 충분하면
    // 아무것도 하지 않고, 모자라면 Array와 같은 기하 성장 규칙으로 늘린다. 오류: 원소 수가
    // maxElements를 넘게 되면 Overflow, 할당 실패는 allocator 오류다. 실패하면 원소· 색인·재사용
    // 목록은 그대로다(일부 내부 배열의 용량만 늘어 있을 수 있다).
    Result<void> TryReserveAdditional(uint32 count) noexcept {
        if (count <= freeCount_) {
            return Result<void>::Success();
        }
        const uint64 required = static_cast<uint64>(elements_.Size()) + (count - freeCount_);
        if (required > maxElements_) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        if (required <= elements_.Capacity()) {
            return Result<void>::Success();
        }
        usize capacity =
            ArrayGrowthCapacity(elements_.Capacity(), static_cast<usize>(required), sizeof(T));
        if (capacity > maxElements_) {
            capacity = maxElements_;
        }
        return ReserveSlots(capacity);
    }

    // 원소 자리 용량을 정확히 capacity 이상으로 만든다(성장 규칙 없이, 미리 크기를 아는 일괄
    // 예약용). 오류: capacity가 maxElements를 넘으면 Overflow, 할당 실패는 allocator 오류이며
    // 상태를 보존한다.
    Result<void> TryReserve(uint32 capacity) noexcept {
        if (capacity <= elements_.Capacity()) {
            return Result<void>::Success();
        }
        if (capacity > maxElements_) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        return ReserveSlots(capacity);
    }

    // 원소 하나를 할당해 값 초기화하고 색인을 돌려준다. 확보와 할당을 한 번에 한다. 오류는
    // TryReserveAdditional과 같으며 실패하면 상태를 보존한다.
    Result<uint32> TryAllocate() noexcept {
        auto reserved = TryReserveAdditional(1);
        if (!reserved) {
            return Result<uint32>::Failure(reserved.Error());
        }
        return Result<uint32>::Success(AllocateReserved());
    }

    // 확보한 자리에서 원소 하나를 할당한다(재사용 목록 우선, LIFO). 할당하지 않는다. 확보한 자리가
    // 없으면(ReservedAvailable() == 0) 계약 위반이므로 Fatal이다.
    uint32 AllocateReserved() noexcept {
        uint32 index = 0;
        if (firstFree_ != EndOfList) {
            index = firstFree_;
            firstFree_ = links_[index];
            --freeCount_;
            elements_[index] = T{};
        } else {
            if (elements_.Size() == elements_.Capacity()) {
                platform::Fatal("IndexPool::AllocateReserved without a reserved element");
            }
            index = static_cast<uint32>(elements_.Size());
            if (!elements_.TryEmplaceBack() || !links_.TryPushBack(EndOfList)) {
                platform::Fatal("IndexPool reserved storage failed to grow");
            }
        }
        links_[index] = Live;
        return index;
    }

    // 원소를 재사용 목록에 돌려준다. 할당하지 않으므로 실패하지 않는다. 살아 있지 않은 색인(범위
    // 밖, 이미 반환한 색인)은 Fatal이다.
    void Free(uint32 index) noexcept {
        if (!IsLive(index)) {
            platform::Fatal("IndexPool::Free of an index that is not allocated");
        }
        links_[index] = firstFree_;
        firstFree_ = index;
        ++freeCount_;
    }

    // index가 지금 할당되어 있으면 true다. 범위 밖은 false다.
    bool IsLive(uint32 index) const noexcept {
        return index < elements_.Size() && links_[index] == Live;
    }

    // 범위만 검사한다(범위 밖은 Fatal). 반환한 색인의 원소는 다음 할당에서 덮어쓰인다.
    T& operator[](uint32 index) noexcept {
        return elements_[index];
    }

    const T& operator[](uint32 index) const noexcept {
        return elements_[index];
    }

    // 모든 원소를 반환한다. 색인은 다시 0부터 발급하고 저장소 용량은 유지한다. 이전 색인은 모두
    // 무효다.
    void Clear() noexcept {
        elements_.Clear();
        links_.Clear();
        firstFree_ = EndOfList;
        freeCount_ = 0;
    }

    // 저장소 용량을 사용 높이(SlotCount)에 맞춘다. 살아 있는 원소가 없으면 먼저 Clear해 저장소를
    // 모두 돌려준다. 재사용 목록 가운데의 빈 자리는 색인이 바뀌면 안 되므로 줄이지 않는다. 오류:
    // 재할당 실패는 allocator 오류이며 상태를 보존한다(한쪽 배열만 줄었을 수 있으나 내용은 같다).
    Result<void> TryShrink() noexcept {
        if (LiveCount() == 0) {
            Clear();
        }
        auto elements = elements_.TryShrink();
        if (!elements) {
            return elements;
        }
        return links_.TryShrink();
    }

private:
    // links_를 먼저 늘린다. elements_ 확보가 실패해도 links_ 용량이 더 클 뿐 불변 조건(links_ 용량
    // ≥ elements_ 용량, AllocateReserved가 둘 다 할당 없이 늘릴 수 있음)은 유지된다.
    Result<void> ReserveSlots(usize capacity) noexcept {
        auto links = links_.TryReserve(capacity);
        if (!links) {
            return links;
        }
        return elements_.TryReserve(capacity);
    }

    Array<T> elements_;
    Array<uint32> links_;
    uint32 firstFree_ = EndOfList;
    uint32 freeCount_ = 0;
    uint32 maxElements_;
};

} // namespace rocks
