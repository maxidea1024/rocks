#pragma once

// OrderedSet: 삽입 순서를 보존한다 — 키 정렬이 아니다.
//
// 원소를 처음 넣은 순서대로 순회하는 해시 Set이다. OrderedMap 위에 만들었고 순서·압축·
// 실패 원자성·포인터와 반복자 무효화·동시성 규약이 모두 OrderedMap과 같다
// (Core/Containers/OrderedMap.h 머리 주석).
//   - 새 원소는 끝에 붙고, 이미 있는 원소의 Add는 순서를 바꾸지 않는다(AlreadyPresent).
//   - 지운 뒤 다시 넣은 원소는 끝에 붙는다.
//   - 같음(operator==)은 순서까지 비교한다.
// 기본 해시 정책은 SeededHash<K>다(hash flooding 방어 — OrderedMap.h 머리 주석).
// 원소를 바꿀 수 없도록 읽기 순회만 제공한다. 리플렉션 Set 필드와 Sparks Set·TagSet의 C++ 대응이다
// (Docs/Design/ReflectionExtensions.ko.md 7.1절).
//
// 사용 예:
//   OrderedSet<Name> tags(allocator);
//   if (auto added = tags.Add(tag); !added) { return added.Error(); }
//   for (const Name& tag : tags) { ... }  // 넣은 순서대로

#include <Core/Containers/OrderedMap.h>

namespace rocks {

template <typename K, typename Hasher = SeededHash<K>, typename KeyEqual = Equal<K>>
class OrderedSet {
    struct Marker {};

public:
    explicit OrderedSet(Allocator& allocator, Hasher hasher = {}, KeyEqual equal = {}) noexcept
        : map_(allocator, Move(hasher), Move(equal)) {
    }

    OrderedSet(const OrderedSet&) = delete;
    OrderedSet& operator=(const OrderedSet&) = delete;
    OrderedSet(OrderedSet&&) noexcept = default;
    OrderedSet& operator=(OrderedSet&&) noexcept = default;

    // 새 원소를 끝에 붙인다. 중복이면 순서를 바꾸지 않고 AlreadyPresent이며 입력을 소비하지 않는다.
    // 실패(OutOfMemory·Overflow·Busy·InvalidArgument)하면 Set이 그대로다.
    template <typename KeyArg>
    Result<InsertOutcome> Add(KeyArg&& key) noexcept {
        return map_.Insert(Forward<KeyArg>(key), Marker{});
    }

    // 정책이 지원하는 이종 키(String 원소를 StringView로)로도 조회할 수 있다.
    template <typename Q>
    bool Contains(const Q& key) const noexcept {
        return map_.Contains(key);
    }

    // 저장된 원소를 읽기 전용으로 빌린다. 없으면 nullptr다. 수명은 OrderedMap 포인터 규칙과 같다.
    template <typename Q>
    const K* Find(const Q& key) const noexcept {
        return map_.FindStoredKey(key);
    }

    // 있으면 지우고 true다. 남은 원소의 순서는 그대로다. 순회 중 호출은 Fatal이다.
    template <typename Q>
    bool Erase(const Q& key) noexcept {
        return map_.Erase(key);
    }

    // predicate가 true인 원소를 지우고 지운 수를 돌려준다. predicate는 noexcept bool(const K&)이며
    // 삽입 순서로 불린다. 순회 중 호출은 Busy다.
    template <typename Predicate>
    Result<usize> EraseIf(Predicate&& predicate) noexcept {
        static_assert(noexcept(predicate(*static_cast<const K*>(nullptr))),
                      "OrderedSet EraseIf predicate must be noexcept");
        static_assert(IsSameV<decltype(predicate(*static_cast<const K*>(nullptr))), bool>,
                      "OrderedSet EraseIf predicate must return bool");
        return map_.EraseIf([&](const K& key, const Marker&) noexcept { return predicate(key); });
    }

    // count개 원소를 저장소 성장 없이 담게 한다. 실패하면 그대로다(OrderedMap::Reserve).
    Result<void> Reserve(usize count) noexcept {
        return map_.Reserve(count);
    }

    // 죽은 항목을 정리하고 최소 용량으로 줄인다. 실패하면 그대로다(OrderedMap::Compact).
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

    // noexcept 콜백에 const 원소를 삽입 순서로 전달한다. 구조 변경은 금지한다.
    template <typename Visitor>
    void ForEach(Visitor&& visitor) const noexcept {
        static_assert(noexcept(visitor(*static_cast<const K*>(nullptr))),
                      "OrderedSet ForEach visitor must be noexcept");
        map_.ForEach([&](const K& key, const Marker&) noexcept { visitor(key); });
    }

    // 같은 원소를 같은 순서로 담았는지 본다(left의 KeyEqual). 순서가 다르면 거짓이다.
    friend bool operator==(const OrderedSet& left, const OrderedSet& right) noexcept {
        return left.map_.SequenceEquals(right.map_,
                                        [](const Marker&, const Marker&) noexcept { return true; });
    }

    // ---- STL 호환 반복자 ----
    // 원소를 const K&로 삽입 순서대로 방문하는 전방 반복자다. 무효화와 구조 변경 탐지 규약은
    // OrderedMap 반복자와 같다(구조 변경 후 역참조·증가는 Fatal).
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
        friend class OrderedSet;
        using Inner = typename OrderedMap<K, Marker, Hasher, KeyEqual>::const_iterator;

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
    OrderedMap<K, Marker, Hasher, KeyEqual> map_;
};

} // namespace rocks
