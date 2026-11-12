#pragma once

#include <Core/Containers/HashMap.h>

namespace rocks {

// 키를 변경할 수 없도록 읽기 순회만 제공한다. 수명·별칭·동시성 규약은 HashMap과 같다.
template <typename K, typename Hasher = Hash<K>, typename KeyEqual = Equal<K>>
class HashSet {
    struct Marker {};

    // 입력을 조회하는 정책·복제 콜백이 그 입력의 구조를 바꾸지 못하도록 한다.
    struct InputGuard {
        explicit InputGuard(const HashSet& input) noexcept : input_(input) {
            input_.map_.EnterIteration();
        }

        ~InputGuard() noexcept {
            --input_.map_.iterationDepth_;
        }

        InputGuard(const InputGuard&) = delete;
        InputGuard& operator=(const InputGuard&) = delete;

        const HashSet& input_;
    };

public:
    explicit HashSet(Allocator& allocator, Hasher hasher = {}, KeyEqual equal = {}) noexcept
        : map_(allocator, Move(hasher), Move(equal)) {
    }

    HashSet(const HashSet&) = delete;
    HashSet& operator=(const HashSet&) = delete;
    HashSet(HashSet&&) noexcept = default;
    HashSet& operator=(HashSet&&) noexcept = default;

    // 중복이면 AlreadyPresent이며 입력을 소비하지 않는다. 실패 원인은 Result에서 확인한다.
    template <typename KeyArg>
    Result<InsertOutcome> Add(KeyArg&& key) noexcept {
        return map_.Insert(Forward<KeyArg>(key), Marker{});
    }

    // 문자열 소유 키는 StringView 등 정책이 지원하는 이종 키로 조회할 수 있다.
    template <typename Q>
    bool Contains(const Q& key) const noexcept {
        return map_.Contains(key);
    }

    // 반환 키는 읽기 전용이며 재해시·삭제·Clear·Reset에 따라 무효화된다.
    template <typename Q>
    const K* Find(const Q& key) const noexcept {
        return map_.FindStoredKey(key);
    }

    // 해당 키가 존재해 삭제한 경우 true다. 순회 중 호출은 Fatal이다.
    template <typename Q>
    bool Erase(const Q& key) noexcept {
        return map_.Erase(key);
    }

    // predicate가 true인 원소만 제거한다. predicate는 noexcept이며 정확히 bool을 반환한다.
    // 순회 중 호출은 Busy다. 삭제한 키의 빌린 포인터는 무효화된다.
    template <typename Predicate>
    Result<usize> EraseIf(Predicate&& predicate) noexcept {
        static_assert(noexcept(predicate(*static_cast<const K*>(nullptr))));
        static_assert(IsSameV<decltype(predicate(*static_cast<const K*>(nullptr))), bool>,
                      "HashSet EraseIf predicate must return bool");
        return map_.EraseIf([&](const K& key, const Marker&) noexcept { return predicate(key); });
    }

    // other.Contains(key)가 거짓인 이 집합의 원소를 제거한다. 양 집합의 동등 관계가 같을 때
    // 일반적인 교집합이다. 같은 집합을 전달하면 아무것도 제거하지 않는다.
    Result<usize> IntersectWith(const HashSet& other) noexcept {
        if (this == &other) {
            return EraseIf([](const K&) noexcept { return false; });
        }
        InputGuard guard(other);
        return EraseIf([&](const K& key) noexcept { return !other.Contains(key); });
    }

    // other.Contains(key)가 참인 이 집합의 원소를 제거한다. other의 비교 정책이 적용된다.
    // 같은 집합을 전달하면 모든 원소를 제거한다. 순회 중 호출은 Busy다.
    Result<usize> DifferenceWith(const HashSet& other) noexcept {
        if (this == &other) {
            return EraseIf([](const K&) noexcept { return true; });
        }
        InputGuard guard(other);
        return EraseIf([&](const K& key) noexcept { return other.Contains(key); });
    }

    // 두 입력은 변경하지 않는다. resultHasher/resultEqual 기준으로 중복을 걸러 새 allocator의
    // 결과를 만든다. clone은 noexcept Result<K>(const K&)이며 결과 정책에서 원본과 동등하고
    // 같은 해시인 키를 반환해야 한다. 실패하면 임시 결과는 해제되지만 콜백의 외부 부작용은
    // 되돌리지 않는다. 콜백은 입력 키·기존 원소를 직접 수정하거나 구조를 변경하면 안 된다.
    // 다른 비교 정책을 사용하면 수학적 교환법칙이 성립하지 않을 수 있다.
    template <typename Clone>
    Result<HashSet> TryUnion(const HashSet& other, Allocator& allocator, Hasher resultHasher,
                             KeyEqual resultEqual, Clone&& clone) const noexcept {
        static_assert(noexcept(clone(*static_cast<const K*>(nullptr))));
        static_assert(IsSameV<decltype(clone(*static_cast<const K*>(nullptr))), Result<K>>,
                      "HashSet TryUnion clone must return Result<K>");

        // 임시 결과와 복제 키의 소멸까지 입력 가드를 유지한다. 자기 자신은 중첩 가드다.
        InputGuard first(*this);
        InputGuard second(other);
        HashSet result(allocator, Move(resultHasher), Move(resultEqual));
        ErrorCode error = ErrorCode::None;
        auto append = [&](const K& key) noexcept {
            if (error != ErrorCode::None || result.Contains(key)) {
                return;
            }
            auto copied = clone(key);
            if (!copied) {
                error = copied.Error();
                return;
            }
            if (!result.map_.KeysEqual(copied.Value(), key) ||
                result.map_.HashKey(copied.Value()) != result.map_.HashKey(key)) {
                error = ErrorCode::InvalidArgument;
                return;
            }
            auto added = result.Add(Move(copied.Value()));
            if (!added) {
                error = added.Error();
            }
        };
        ForEach(append);
        other.ForEach(append);

        if (error != ErrorCode::None) {
            return Result<HashSet>::Failure(error);
        }
        return Result<HashSet>::Success(Move(result));
    }

    // 보관할 원소 수를 기준으로 공간을 확보한다. 실패 시 기존 내용과 주소를 유지한다.
    Result<void> Reserve(usize count) noexcept {
        return map_.Reserve(count);
    }

    // 삭제 슬롯을 정리하고 최소 용량으로 축소한다. 실패 시 기존 내용과 주소를 유지한다.
    Result<void> Compact() noexcept {
        return map_.Compact();
    }

    usize Count() const noexcept {
        return map_.Count();
    }

    usize Capacity() const noexcept {
        return map_.Capacity();
    }

    bool IsEmpty() const noexcept {
        return map_.IsEmpty();
    }

    // 원소만 비우고 용량은 유지한다.
    void Clear() noexcept {
        map_.Clear();
    }

    // 원소와 저장소를 모두 반환한다.
    void Reset() noexcept {
        map_.Reset();
    }

    // noexcept 콜백에 const 키를 전달한다. 순회 순서는 보장하지 않으며 구조 변경은 금지한다.
    template <typename Visitor>
    void ForEach(Visitor&& visitor) const noexcept {
        static_assert(noexcept(visitor(*static_cast<const K*>(nullptr))));
        map_.ForEach([&](const K& key, const Marker&) noexcept { visitor(key); });
    }

    // ---- STL 호환 반복자 ----
    // 원소를 const K&로 방문하는 전방 반복자다. 순서는 정해지지 않는다. 무효화와 구조 변경 탐지
    // 규약은 HashMap 반복자와 같다(구조 변경 후 역참조·증가는 Fatal).
    class ConstIterator {
    public:
        using value_type = K;
        using reference = const K&;
        using pointer = const K*;
        using difference_type = isize;

        ConstIterator() noexcept = default;

        const K& operator*() const noexcept {
            return (*inner_).key;
        }

        const K* operator->() const noexcept {
            return &(*inner_).key;
        }

        ConstIterator& operator++() noexcept {
            ++inner_;
            return *this;
        }

        ConstIterator operator++(int) noexcept {
            ConstIterator previous = *this;
            ++inner_;
            return previous;
        }

        friend bool operator==(const ConstIterator& left, const ConstIterator& right) noexcept {
            return left.inner_ == right.inner_;
        }

    private:
        friend class HashSet;
        using Inner = typename HashMap<K, Marker, Hasher, KeyEqual>::const_iterator;

        explicit ConstIterator(Inner inner) noexcept : inner_(inner) {
        }

        Inner inner_;
    };

    using key_type = K;
    using value_type = K;
    using size_type = usize;
    using difference_type = isize;
    using iterator = ConstIterator;
    using const_iterator = ConstIterator;

    ConstIterator begin() const noexcept {
        return ConstIterator(map_.begin());
    }

    ConstIterator end() const noexcept {
        return ConstIterator(map_.end());
    }

    ConstIterator cbegin() const noexcept {
        return begin();
    }

    ConstIterator cend() const noexcept {
        return end();
    }

private:
    HashMap<K, Marker, Hasher, KeyEqual> map_;
};

} // namespace rocks
