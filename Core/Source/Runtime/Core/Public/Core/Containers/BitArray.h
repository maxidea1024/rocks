#pragma once

#include <Base/Checked.h>
#include <Core/Containers/ContainerDetail.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

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
    static constexpr usize NotFound = IndexNone;

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
        return BitReference(words_[index >> WordShift], Mask(index));
    }

    bool operator[](usize index) const noexcept {
        CheckIndex(index);
        return Read(index);
    }

    // ---- STL 호환 읽기 반복자 ----
    // 각 bit를 bool 값으로 읽는 random-access 반복자다(range-for: for (bool bit : bits)). bit를
    // 바꾸려면 operator[]의 BitReference를 사용한다. 원소 수 변경·재할당·소멸은 반복자를
    // 무효화하며, 무효 위치의 역참조는 BitArray의 범위 검사로 Fatal이다.
    class ConstIterator {
    public:
        using value_type = bool;
        using difference_type = isize;
        using reference = bool;

        ConstIterator() noexcept = default;

        bool operator*() const noexcept {
            return (*bits_)[index_];
        }

        bool operator[](difference_type offset) const noexcept {
            return *(*this + offset);
        }

        ConstIterator& operator++() noexcept {
            ++index_;
            return *this;
        }

        ConstIterator operator++(int) noexcept {
            ConstIterator previous = *this;
            ++index_;
            return previous;
        }

        ConstIterator& operator--() noexcept {
            --index_;
            return *this;
        }

        ConstIterator operator--(int) noexcept {
            ConstIterator previous = *this;
            --index_;
            return previous;
        }

        ConstIterator& operator+=(difference_type offset) noexcept {
            index_ = static_cast<usize>(static_cast<difference_type>(index_) + offset);
            return *this;
        }

        ConstIterator& operator-=(difference_type offset) noexcept {
            return *this += -offset;
        }

        friend ConstIterator operator+(ConstIterator iterator, difference_type offset) noexcept {
            return iterator += offset;
        }

        friend ConstIterator operator+(difference_type offset, ConstIterator iterator) noexcept {
            return iterator += offset;
        }

        friend ConstIterator operator-(ConstIterator iterator, difference_type offset) noexcept {
            return iterator -= offset;
        }

        friend difference_type operator-(const ConstIterator& left,
                                         const ConstIterator& right) noexcept {
            return static_cast<difference_type>(left.index_) -
                   static_cast<difference_type>(right.index_);
        }

        friend bool operator==(const ConstIterator& left, const ConstIterator& right) noexcept {
            return left.bits_ == right.bits_ && left.index_ == right.index_;
        }

        friend bool operator<(const ConstIterator& left, const ConstIterator& right) noexcept {
            return left.index_ < right.index_;
        }

        friend bool operator>(const ConstIterator& left, const ConstIterator& right) noexcept {
            return right < left;
        }

        friend bool operator<=(const ConstIterator& left, const ConstIterator& right) noexcept {
            return !(right < left);
        }

        friend bool operator>=(const ConstIterator& left, const ConstIterator& right) noexcept {
            return !(left < right);
        }

    private:
        friend class BitArray;

        ConstIterator(const BitArray* bits, usize index) noexcept : bits_(bits), index_(index) {
        }

        const BitArray* bits_ = nullptr;
        usize index_ = 0;
    };

    using value_type = bool;
    using size_type = usize;
    using difference_type = isize;
    using const_iterator = ConstIterator;

    ConstIterator begin() const noexcept {
        return ConstIterator(this, 0);
    }

    ConstIterator end() const noexcept {
        return ConstIterator(this, size_);
    }

    ConstIterator cbegin() const noexcept {
        return begin();
    }

    ConstIterator cend() const noexcept {
        return end();
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
        SetRange(index, count, value);
        return Result<void>::Success();
    }

    // bit capacity는 64의 배수로 확보한다. 새로운 저장소는 0으로 초기화한다.
    Result<void> TryReserve(usize capacity) noexcept {
        if (capacity <= Capacity()) {
            return Result<void>::Success();
        }
        if (capacity > MaxSize - WordMask) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        const usize words = WordCount(capacity);
        if (words > (MaxSize >> WordShift)) {
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
        // 배열 수명을 한 번에 시작한다. 값 초기화와 기존 값 복사를 중복 수행하지 않는다.
        ::new (replacement) uint64[words];
        const usize used = WordCount(size_);
        platform::MemoryCopy(replacement, words_, used * sizeof(uint64));
        platform::MemoryZero(replacement + used, (words - used) * sizeof(uint64));
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
            SetRange(size_, size - size_, value);
        } else {
            SetRange(size, size_ - size, false);
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
        MoveBits(index + count, index, size_ - index);
        SetRange(index, count, value);
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
        MoveBits(index, index + count, next - index);
        SetRange(next, count, false);
        size_ = next;
        return Result<void>::Success();
    }

    // UE5.8 BitArray처럼 원래 Size()-count에서 시작하는 bit로 빈 자리를 메운다.
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
        MoveBits(index, next, moved);
        SetRange(next, count, false);
        size_ = next;
        return Result<void>::Success();
    }

    // 없거나 시작 위치가 범위를 벗어나면 NotFound다. FindAndSet은 발견한 bit만 변경한다.
    usize Find(bool value, usize start = 0) const noexcept {
        if (start >= size_) {
            return NotFound;
        }

        const usize endWord = WordCount(size_);
        usize index = start >> WordShift;
        uint64 candidates =
            (value ? words_[index] : ~words_[index]) & (AllBits << (start & WordMask));
        for (;;) {
            if (index + 1 == endWord && (size_ & WordMask) != 0) {
                candidates &= LowMask(size_ & WordMask);
            }

            if (candidates != 0) {
                return (index << WordShift) +
                       platform::PlatformMath::CountTrailingZeros64(candidates);
            }

            if (++index == endWord) {
                break;
            }

            candidates = value ? words_[index] : ~words_[index];
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
        return CountRange(0, size_);
    }

    // [begin, end)에서 설정된 비트 수를 센다. 잘못된 범위는 오류다.
    Result<usize> TryCountSetBits(usize begin, usize end) const noexcept {
        if (begin > end || end > size_) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<usize>::Success(CountRange(begin, end));
    }

    // 다른 allocator로 독립 복제한다. 사본의 capacity는 Size()를 64-bit word로 올린 정확한
    // 크기다(성장 slack 없음). 사용 word를 한 번에 복사하며 크기 밖 bit는 원본처럼 0이다.
    // 실패해도 원본은 바뀌지 않는다.
    Result<BitArray> TryClone(Allocator& allocator) const noexcept {
        BitArray copy(allocator);
        auto reserved = copy.TryReserve(size_);
        if (!reserved) {
            return Result<BitArray>::Failure(reserved.Error());
        }

        platform::MemoryCopy(copy.words_, words_, WordCount(size_) * sizeof(uint64));
        copy.size_ = size_;
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
        switch (operation) {
        case BitOperation::And:
            CombineWords<BitOperation::And>(other, newSize, missingValue);
            break;
        case BitOperation::Or:
            CombineWords<BitOperation::Or>(other, newSize, missingValue);
            break;
        case BitOperation::Xor:
            CombineWords<BitOperation::Xor>(other, newSize, missingValue);
            break;
        }
        if (newSize < oldSize) {
            SetRange(newSize, oldSize - newSize, false);
        }
        size_ = newSize;
        MaskLastWord();
        return Result<void>::Success();
    }

    void Invert() noexcept {
        const usize words = WordCount(size_);
        for (usize i = 0; i < words; ++i) {
            words_[i] = ~words_[i];
        }
        MaskLastWord();
    }

    // storage를 유지하며 모든 bit를 제거한다. 빌린 참조는 더 이상 사용하지 않는다.
    void Clear() noexcept {
        platform::MemoryZero(words_, WordCount(size_) * sizeof(uint64));
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
    static constexpr usize WordShift = 6;
    static constexpr usize WordMask = BitsPerWord - 1;
    static constexpr uint64 AllBits = NumericLimits<uint64>::Max();
    static_assert((usize{1} << WordShift) == BitsPerWord);

    static constexpr usize WordCount(usize bits) noexcept {
        return (bits >> WordShift) + ((bits & WordMask) != 0 ? 1 : 0);
    }

    // count==64에서도 64-bit shift를 평가하지 않는다.
    static constexpr uint64 LowMask(usize count) noexcept {
        return count == BitsPerWord ? AllBits : (uint64{1} << count) - 1;
    }

    void MaskLastWord() noexcept {
        if ((size_ & WordMask) != 0) {
            words_[size_ >> WordShift] &= LowMask(size_ & WordMask);
        }
    }

    // 목적지는 한 word 안의 범위다. 범위 밖 비트는 그대로 둔다.
    void WriteBits(usize index, usize count, uint64 value) noexcept {
        const usize offset = index & WordMask;
        const uint64 mask = LowMask(count) << offset;
        uint64& word = words_[index >> WordShift];
        word = (word & ~mask) | ((value << offset) & mask);
    }

    // 최대 64bit를 읽는다. 필요한 경우에만 다음 word를 읽어 capacity 밖 접근을 막는다.
    uint64 ReadBits(usize index, usize count) const noexcept {
        const usize offset = index & WordMask;
        const usize word = index >> WordShift;
        uint64 value = words_[word] >> offset;
        if (count > BitsPerWord - offset) {
            value |= words_[word + 1] << (BitsPerWord - offset);
        }

        return value & LowMask(count);
    }

    void SetRange(usize index, usize count, bool value) noexcept {
        if (count == 0) {
            return;
        }

        const usize offset = index & WordMask;
        if (offset != 0) {
            const usize first = count < BitsPerWord - offset ? count : BitsPerWord - offset;
            WriteBits(index, first, value ? AllBits : 0);
            index += first;
            count -= first;
        }

        const usize whole = count >> WordShift;
        platform::MemorySet(words_ + (index >> WordShift), value ? 0xff : 0,
                            whole * sizeof(uint64));
        index += whole << WordShift;
        count &= WordMask;
        if (count != 0) {
            WriteBits(index, count, value ? AllBits : 0);
        }
    }

    // memmove와 같이 중첩 방향에 맞춰 읽기부터 수행한다. 단위는 목적지의 word 경계다.
    void MoveBits(usize destination, usize source, usize count) noexcept {
        if (destination == source) {
            return;
        }
        while (count != 0) {
            const bool backward = destination > source;
            const usize available = backward ? ((destination + count - 1) & WordMask) + 1
                                             : BitsPerWord - (destination & WordMask);
            const usize chunk = count < available ? count : available;
            const usize offset = backward ? count - chunk : 0;
            const uint64 value = ReadBits(source + offset, chunk);
            WriteBits(destination + offset, chunk, value);
            count -= chunk;
            if (!backward) {
                source += chunk;
                destination += chunk;
            }
        }
    }

    usize CountRange(usize begin, usize end) const noexcept {
        usize count = 0;
        while (begin < end) {
            const usize available = BitsPerWord - (begin & WordMask);
            const usize chunk = end - begin < available ? end - begin : available;
            count += platform::PlatformMath::PopCount64(ReadBits(begin, chunk));
            begin += chunk;
        }

        return count;
    }

    uint64 ExtendedWord(usize index, bool missing) const noexcept {
        const usize first = index << WordShift;
        if (first >= size_) {
            return missing ? AllBits : 0;
        }

        const usize remaining = size_ - first;
        const uint64 mask = LowMask(remaining < BitsPerWord ? remaining : BitsPerWord);
        return (words_[index] & mask) | (missing ? ~mask : 0);
    }

    template <BitOperation Operation>
    void CombineWords(const BitArray& other, usize size, bool missing) noexcept {
        const usize words = WordCount(size);
        for (usize i = 0; i < words; ++i) {
            const uint64 left = ExtendedWord(i, missing);
            const uint64 right = other.ExtendedWord(i, missing);
            if constexpr (Operation == BitOperation::And) {
                words_[i] = left & right;
            } else if constexpr (Operation == BitOperation::Or) {
                words_[i] = left | right;
            } else {
                words_[i] = left ^ right;
            }
        }
    }

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

        const usize words =
            detail::GrowthCapacityWithin(wordCapacity_, WordCount(required), MaxSize >> WordShift);
        return TryReserve(words << WordShift);
    }

    static bool TryAddSize(usize a, usize b, usize& out) noexcept {
        return rocks::TryAdd(a, b, out);
    }

    static uint64 Mask(usize index) noexcept {
        return uint64{1} << (index & WordMask);
    }

    bool Read(usize index) const noexcept {
        return (words_[index >> WordShift] & Mask(index)) != 0;
    }

    void Write(usize index, bool value) noexcept {
        uint64& word = words_[index >> WordShift];
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
