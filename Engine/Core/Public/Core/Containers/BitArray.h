#pragma once

#include <Base/Checked.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

// 단일 비트를 빌린다. 소유 BitArray의 재할당·이동·삭제·소멸 뒤에는 사용하지 않는다.
class BitReference {
public:
    BitReference(uint64& word, uint64 mask) noexcept : word_(&word), mask_(mask) {
    }

    BitReference& operator=(bool value) noexcept {
        if (value) {
            *word_ |= mask_;
        } else {
            *word_ &= ~mask_;
        }
        return *this;
    }

    BitReference& operator=(const BitReference& other) noexcept {
        return *this = static_cast<bool>(other);
    }

    operator bool() const noexcept {
        return (*word_ & mask_) != 0;
    }

private:
    uint64* word_;
    uint64 mask_;
};

// 두 비트 배열 결합 시 결과의 크기와 없는 비트의 값을 명시한다.
enum class BitSizePolicy {
    Maximum,
    Minimum,
    Maintain
};
enum class BitOperation {
    And,
    Or,
    Xor
};

// allocator를 빌려 bit 저장소를 소유한다. 서로 다른 bit라도 같은 word를 공유하므로
// 동시 읽기/쓰기는 caller가 동기화한다. 객체 파괴 전 빌린 BitReference를 사용하지 않는다.
// 성장 실패 시 기존 bit·주소·크기를 보존한다. 재할당/삽입/삭제 뒤 빌린 참조는 무효다.
// StackAllocator처럼 LIFO 순서로 반환해야 하는 allocator는 먼저 최대 용량을 reserve하고
// 그 용량 안에서 사용한다. 기존 storage를 유지한 상태의 성장은 반환 순서 위반이 된다.
class BitArray final {
public:
    static constexpr usize NotFound = MaxSize;

    explicit BitArray(Allocator& allocator) noexcept : allocator_(&allocator) {
    }

    ~BitArray() noexcept {
        Release();
    }

    BitArray(const BitArray&) = delete;
    BitArray& operator=(const BitArray&) = delete;

    BitArray(BitArray&& other) noexcept
        : allocator_(other.allocator_), words_(other.words_), size_(other.size_),
          wordCapacity_(other.wordCapacity_) {
        other.words_ = nullptr;
        other.size_ = 0;
        other.wordCapacity_ = 0;
    }

    BitArray& operator=(BitArray&& other) noexcept {
        if (this != &other) {
            Release();
            allocator_ = other.allocator_;
            words_ = other.words_;
            size_ = other.size_;
            wordCapacity_ = other.wordCapacity_;
            other.words_ = nullptr;
            other.size_ = 0;
            other.wordCapacity_ = 0;
        }
        return *this;
    }

    usize Size() const noexcept {
        return size_;
    }

    usize Capacity() const noexcept {
        return wordCapacity_ * BitsPerWord;
    }

    bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    // capacity는 bit 단위다. 범위를 벗어난 index는 Fatal이며 오류 복구가 필요하면 TryGet을 쓴다.
    BitReference operator[](usize index) noexcept {
        CheckIndex(index);
        return BitReference(words_[index / BitsPerWord], Mask(index));
    }

    bool operator[](usize index) const noexcept {
        CheckIndex(index);
        return Read(index);
    }

    Result<bool> TryGet(usize index) const noexcept {
        if (index >= size_) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<bool>::Success(Read(index));
    }

    Result<void> TrySet(usize index, bool value) noexcept {
        if (index >= size_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        Write(index, value);
        return Result<void>::Success();
    }

    // 잘못된 범위는 기존 bit를 유지한다. count=0은 index==Size()까지 허용한다.
    Result<void> TrySetRange(usize index, usize count, bool value) noexcept {
        if (index > size_ || count > size_ - index) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        for (usize i = 0; i < count; ++i) {
            Write(index + i, value);
        }
        return Result<void>::Success();
    }

    // bit capacity는 64의 배수로 확보한다. 새로운 저장소는 0으로 초기화한다.
    Result<void> TryReserve(usize capacity) noexcept {
        if (capacity <= Capacity()) {
            return Result<void>::Success();
        }
        if (capacity > MaxSize - (BitsPerWord - 1)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        const usize words = (capacity + BitsPerWord - 1) / BitsPerWord;
        if (words > MaxSize / BitsPerWord) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        usize bytes = 0;
        if (!TryMultiply(words, sizeof(uint64), bytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        auto allocation = allocator_->Allocate(bytes, alignof(uint64));
        if (!allocation) {
            return Result<void>::Failure(allocation.Error());
        }
        auto* replacement = static_cast<uint64*>(allocation.Value());
        if (!replacement || reinterpret_cast<uptr>(replacement) % alignof(uint64) != 0) {
            platform::Fatal("BitArray allocator returned invalid storage");
        }
        for (usize i = 0; i < words; ++i) {
            ConstructAt<uint64>(replacement + i, i < wordCapacity_ ? words_[i] : 0);
        }
        FreeWords();
        words_ = replacement;
        wordCapacity_ = words;
        return Result<void>::Success();
    }

    // 늘어나는 bit는 value로 초기화하며 줄어든 영역은 지운다. 실패하면 이전 상태를 유지한다.
    Result<void> TryResize(usize size, bool value = false) noexcept {
        if (size > size_) {
            auto reserved = TryGrowTo(size);
            if (!reserved) {
                return reserved;
            }
            for (usize i = size_; i < size; ++i) {
                Write(i, value);
            }
        } else {
            for (usize i = size; i < size_; ++i) {
                Write(i, false);
            }
        }
        size_ = size;
        return Result<void>::Success();
    }

    // 추가한 첫 bit의 index를 반환한다. count=0은 Size()를 반환한다.
    Result<usize> TryAdd(bool value, usize count = 1) noexcept {
        const usize first = size_;
        usize next = 0;
        if (!TryAddSize(size_, count, next)) {
            return Result<usize>::Failure(ErrorCode::Overflow);
        }
        auto resized = TryResize(next, value);
        return resized ? Result<usize>::Success(first) : Result<usize>::Failure(resized.Error());
    }

    // index 앞에 count개의 bit를 넣는다. 할당 실패·잘못된 index는 기존 순서를 보존한다.
    Result<void> TryInsert(usize index, usize count, bool value) noexcept {
        if (index > size_) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        usize next = 0;
        if (!TryAddSize(size_, count, next)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        if (count == 0) {
            return Result<void>::Success();
        }
        auto reserved = TryGrowTo(next);
        if (!reserved) {
            return reserved;
        }
        for (usize i = size_; i > index; --i) {
            Write(i + count - 1, Read(i - 1));
        }
        for (usize i = 0; i < count; ++i) {
            Write(index + i, value);
        }
        size_ = next;
        return Result<void>::Success();
    }

    // 순서를 보존하여 삭제한다. 잘못된 범위는 변경하지 않는다.
    Result<void> TryRemoveAt(usize index, usize count = 1) noexcept {
        if (index > size_ || count > size_ - index) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (count == 0) {
            return Result<void>::Success();
        }
        const usize next = size_ - count;
        for (usize i = index; i < next; ++i) {
            Write(i, Read(i + count));
        }
        for (usize i = next; i < size_; ++i) {
            Write(i, false);
        }
        size_ = next;
        return Result<void>::Success();
    }

    // UE5.6 BitArray처럼 원래 Size()-count에서 시작하는 bit로 빈 자리를 메운다.
    // 제거 범위와 끝 범위가 겹치면 Array의 RemoveAtSwap과 결과가 다를 수 있다.
    Result<void> TryRemoveAtSwap(usize index, usize count = 1) noexcept {
        if (index > size_ || count > size_ - index) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (count == 0) {
            return Result<void>::Success();
        }
        const usize next = size_ - count;
        const usize tail = size_ - index - count;
        const usize moved = count < tail ? count : tail;
        for (usize i = 0; i < moved; ++i) {
            Write(index + i, Read(size_ - count + i));
        }
        for (usize i = next; i < size_; ++i) {
            Write(i, false);
        }
        size_ = next;
        return Result<void>::Success();
    }

    // 없거나 시작 위치가 범위를 벗어나면 NotFound다. FindAndSet은 발견한 bit만 변경한다.
    usize Find(bool value, usize start = 0) const noexcept {
        for (usize i = start; i < size_; ++i) {
            if (Read(i) == value) {
                return i;
            }
        }
        return NotFound;
    }

    usize FindAndSetFirstZeroBit(usize start = 0) noexcept {
        const usize index = Find(false, start);
        if (index != NotFound) {
            Write(index, true);
        }
        return index;
    }

    bool Contains(bool value) const noexcept {
        return Find(value) != NotFound;
    }

    usize CountSetBits() const noexcept {
        usize count = 0;
        for (usize i = 0; i < size_; ++i) {
            count += Read(i) ? 1 : 0;
        }
        return count;
    }

    // [begin, end)에서 설정된 비트 수를 센다. 잘못된 범위는 오류다.
    Result<usize> TryCountSetBits(usize begin, usize end) const noexcept {
        if (begin > end || end > size_) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }
        usize count = 0;
        for (usize i = begin; i < end; ++i) {
            count += Read(i) ? 1 : 0;
        }
        return Result<usize>::Success(count);
    }

    // 다른 allocator로 독립 복제한다. 실패해도 원본은 바뀌지 않는다.
    Result<BitArray> TryClone(Allocator& allocator) const noexcept {
        BitArray copy(allocator);
        auto resized = copy.TryResize(size_);
        if (!resized) {
            return Result<BitArray>::Failure(resized.Error());
        }
        for (usize i = 0; i < size_; ++i) {
            copy.Write(i, Read(i));
        }
        return Result<BitArray>::Success(Move(copy));
    }

    // bitwise 결합을 이 배열에 적용한다. 부족한 bit는 missingValue로 해석한다.
    // Maximum/Minimum은 두 크기의 큰/작은 값, Maintain은 원래 이 배열의 크기를 유지한다.
    // 자기 자신도 입력할 수 있다. 실패 시 이 배열의 내용·크기·capacity를 보존한다.
    Result<void> TryCombine(const BitArray& other, BitOperation operation,
                            BitSizePolicy policy = BitSizePolicy::Maximum,
                            bool missingValue = false) noexcept {
        if (operation != BitOperation::And && operation != BitOperation::Or &&
            operation != BitOperation::Xor) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        const usize oldSize = size_;
        const usize otherSize = other.size_;
        usize newSize = oldSize;
        if (policy == BitSizePolicy::Maximum) {
            newSize = oldSize > otherSize ? oldSize : otherSize;
        } else if (policy == BitSizePolicy::Minimum) {
            newSize = oldSize < otherSize ? oldSize : otherSize;
        } else if (policy != BitSizePolicy::Maintain) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        auto reserved = TryGrowTo(newSize);
        if (!reserved) {
            return reserved;
        }
        for (usize i = 0; i < newSize; ++i) {
            const bool left = i < oldSize ? Read(i) : missingValue;
            const bool right = i < otherSize ? other.Read(i) : missingValue;
            const bool value = operation == BitOperation::And  ? left && right
                               : operation == BitOperation::Or ? left || right
                                                               : left != right;
            Write(i, value);
        }
        for (usize i = newSize; i < oldSize; ++i) {
            Write(i, false);
        }
        size_ = newSize;
        return Result<void>::Success();
    }

    void Invert() noexcept {
        for (usize i = 0; i < size_; ++i) {
            Write(i, !Read(i));
        }
    }

    // storage를 유지하며 모든 bit를 제거한다. 빌린 참조는 더 이상 사용하지 않는다.
    void Clear() noexcept {
        for (usize i = 0; i < wordCapacity_; ++i) {
            words_[i] = 0;
        }
        size_ = 0;
    }

    // 비트가 없을 때만 storage를 반환한다. 할당기 반환 거부는 소유권 위반으로 Fatal이다.
    Result<void> Trim() noexcept {
        if (size_ != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        FreeWords();
        wordCapacity_ = 0;
        return Result<void>::Success();
    }

private:
    static constexpr usize BitsPerWord = 64;

    // 자동 추가는 여유 공간을 기하급수적으로 늘려 반복 추가에서 매 word마다 전체 복사를
    // 피한다. 명시적 TryReserve는 요청 크기만 64-bit word로 올리는 별도 계약을 유지한다.
    Result<void> TryGrowTo(usize required) noexcept {
        if (required <= Capacity()) {
            return Result<void>::Success();
        }

        constexpr usize maximum = (MaxSize / BitsPerWord) * BitsPerWord;
        if (required > maximum) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        const usize current = Capacity();
        const usize growth = current / 2;
        usize target = 0;
        if (!rocks::TryAdd(current, growth, target) || target > maximum) {
            target = maximum;
        }
        if (target < BitsPerWord) {
            target = BitsPerWord;
        }
        if (target < required) {
            target = required;
        }
        return TryReserve(target);
    }

    static bool TryAddSize(usize a, usize b, usize& out) noexcept {
        return rocks::TryAdd(a, b, out);
    }

    static uint64 Mask(usize index) noexcept {
        return static_cast<uint64>(1) << (index % BitsPerWord);
    }

    bool Read(usize index) const noexcept {
        return (words_[index / BitsPerWord] & Mask(index)) != 0;
    }

    void Write(usize index, bool value) noexcept {
        uint64& word = words_[index / BitsPerWord];
        if (value) {
            word |= Mask(index);
        } else {
            word &= ~Mask(index);
        }
    }

    void CheckIndex(usize index) const noexcept {
        if (index >= size_) {
            platform::Fatal("BitArray index is out of range");
        }
    }

    void FreeWords() noexcept {
        if (words_ && !allocator_->Deallocate(words_)) {
            platform::Fatal("BitArray storage was not owned by its allocator");
        }
        words_ = nullptr;
    }

    void Release() noexcept {
        FreeWords();
        size_ = 0;
        wordCapacity_ = 0;
    }

    Allocator* allocator_;
    uint64* words_ = nullptr;
    usize size_ = 0;
    usize wordCapacity_ = 0;
};

} // namespace rocks
