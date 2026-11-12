#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/HashSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

// 모든 키를 한 슬롯에서 시작하게 해 충돌, 삭제 후 탐색, 재사용을 검증한다.
struct CollisionHash {
    usize operator()(int32) const noexcept {
        return 0;
    }
};

struct alignas(64) TrackedValue {
    static inline int32 live = 0;
    static inline int32 constructed = 0;
    static inline int32 destroyed = 0;
    int32 value;

    explicit TrackedValue(int32 number) noexcept : value(number) {
        ++live;
        ++constructed;
    }

    TrackedValue(const TrackedValue&) = delete;
    TrackedValue& operator=(const TrackedValue&) = delete;

    TrackedValue(TrackedValue&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
        ++constructed;
    }

    ~TrackedValue() noexcept {
        --live;
        ++destroyed;
    }
};

struct MoveOnlyKey {
    int32 value;

    explicit MoveOnlyKey(int32 number) noexcept : value(number) {
    }
    MoveOnlyKey(const MoveOnlyKey&) = delete;
    MoveOnlyKey& operator=(const MoveOnlyKey&) = delete;
    MoveOnlyKey(MoveOnlyKey&& other) noexcept : value(other.value) {
        other.value = -1;
    }
};

struct MoveKeyHash {
    usize operator()(const MoveOnlyKey& key) const noexcept {
        return static_cast<usize>(key.value);
    }
};

struct MoveKeyEqual {
    bool operator()(const MoveOnlyKey& left, const MoveOnlyKey& right) const noexcept {
        return left.value == right.value;
    }
};

} // namespace

TEST(CoreHashMapCollisionRemovalAndDuplicate) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32, CollisionHash> map(allocator);
        CHECK(map.Count() == 0);
        CHECK(map.Find(7) == nullptr);
        CHECK(!map.Erase(7));

        for (int32 key = 0; key != 200; ++key) {
            auto inserted = map.Insert(key, key * 3);
            REQUIRE(inserted);
            CHECK(inserted.Value() == InsertOutcome::Inserted);
        }
        CHECK(map.Count() == 200);
        auto duplicate = map.Insert(17, -100);
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        REQUIRE(map.Find(17) != nullptr);
        CHECK(*map.Find(17) == 51);
        CHECK(map.Count() == 200);

        // 충돌 사슬의 앞·중간을 지워도 그 뒤 키의 탐색이 끊기면 안 된다.
        for (int32 key = 0; key < 200; key += 2) {
            CHECK(map.Erase(key));
            CHECK(!map.Erase(key));
        }
        CHECK(map.Count() == 100);
        for (int32 key = 0; key != 200; ++key) {
            auto* value = map.Find(key);
            if (key % 2 == 0) {
                CHECK(value == nullptr);
            } else {
                REQUIRE(value != nullptr);
                CHECK(*value == key * 3);
            }
        }
        for (int32 key = 200; key != 300; ++key) {
            auto inserted = map.Insert(key, key * 3);
            REQUIRE(inserted);
            CHECK(inserted.Value() == InsertOutcome::Inserted);
        }
        for (int32 key = 1; key < 200; key += 2) {
            auto* value = map.Find(key);
            REQUIRE(value != nullptr);
            CHECK(*value == key * 3);
        }
        CHECK(map.Count() == 200);
        map.Clear();
        CHECK(map.Count() == 0);
        CHECK(map.Find(201) == nullptr);
        auto afterClear = map.Insert(9, 19);
        REQUIRE(afterClear);
        CHECK(afterClear.Value() == InsertOutcome::Inserted);
        REQUIRE(map.Find(9) != nullptr);
        CHECK(*map.Find(9) == 19);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(CoreHashSetCollisionRemovalAndDuplicate) {
    SystemAllocator allocator;
    {
        HashSet<int32, CollisionHash> set(allocator);
        CHECK(set.Count() == 0);
        CHECK(!set.Contains(10));
        for (int32 key = 0; key != 128; ++key) {
            auto added = set.Add(key);
            REQUIRE(added);
            CHECK(added.Value() == InsertOutcome::Inserted);
        }
        auto duplicate = set.Add(13);
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        CHECK(set.Count() == 128);
        for (int32 key = 0; key < 128; key += 3) {
            CHECK(set.Erase(key));
            CHECK(!set.Contains(key));
            CHECK(!set.Erase(key));
        }
        for (int32 key = 0; key != 128; ++key) {
            CHECK(set.Contains(key) == (key % 3 != 0));
        }
        for (int32 key = 0; key < 128; key += 3) {
            auto added = set.Add(key);
            REQUIRE(added);
            CHECK(added.Value() == InsertOutcome::Inserted);
        }
        CHECK(set.Count() == 128);
        set.Clear();
        CHECK(set.Count() == 0);
        CHECK(!set.Contains(13));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(CoreHashMapAllocationFailurePreservesExistingItems) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        HashMap<int32, int32> map(allocator);
        allocator.FailOnAttempt(1);
        auto first = map.Insert(1, 2);
        CHECK(!first);
        CHECK(first.Error() == ErrorCode::OutOfMemory);
        CHECK(map.Count() == 0);
        CHECK(map.Find(1) == nullptr);

        allocator.DisableFailure();
        auto inserted = map.Insert(1, 11);
        REQUIRE(inserted);
        CHECK(inserted.Value() == InsertOutcome::Inserted);
        int32* original = map.Find(1);
        REQUIRE(original != nullptr);
        // 확장 실패는 원소뿐 아니라 기존 포인터까지 보존해야 한다.
        allocator.FailOnAttempt(1);
        auto reserve = map.Reserve(1024);
        CHECK(!reserve);
        CHECK(reserve.Error() == ErrorCode::OutOfMemory);
        CHECK(map.Count() == 1);
        CHECK(map.Find(1) == original);
        CHECK(*map.Find(1) == 11);

        allocator.DisableFailure();
        auto overflow = map.Reserve(MaxSize);
        CHECK(!overflow);
        CHECK(overflow.Error() == ErrorCode::Overflow);
        CHECK(map.Count() == 1);
        REQUIRE(map.Find(1) != nullptr);
        CHECK(*map.Find(1) == 11);
        REQUIRE(map.Reserve(1024));
        REQUIRE(map.Find(1) != nullptr);
        CHECK(*map.Find(1) == 11);
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreHashSetAllocationFailurePreservesExistingItems) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        HashSet<int32> set(allocator);
        allocator.FailOnAttempt(1);
        auto first = set.Add(4);
        CHECK(!first);
        CHECK(first.Error() == ErrorCode::OutOfMemory);
        CHECK(set.Count() == 0);
        CHECK(!set.Contains(4));
        allocator.DisableFailure();
        auto inserted = set.Add(4);
        REQUIRE(inserted);
        CHECK(inserted.Value() == InsertOutcome::Inserted);
        allocator.FailOnAttempt(1);
        auto reserve = set.Reserve(1024);
        CHECK(!reserve);
        CHECK(reserve.Error() == ErrorCode::OutOfMemory);
        CHECK(set.Count() == 1);
        CHECK(set.Contains(4));
        allocator.DisableFailure();
        auto overflow = set.Reserve(MaxSize);
        CHECK(!overflow);
        CHECK(overflow.Error() == ErrorCode::Overflow);
        CHECK(set.Contains(4));
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreHashMapMoveOnlyAlignmentAndLifetime) {
    TrackedValue::live = 0;
    TrackedValue::constructed = 0;
    TrackedValue::destroyed = 0;
    SystemAllocator allocator;
    {
        HashMap<MoveOnlyKey, TrackedValue, MoveKeyHash, MoveKeyEqual> map(allocator);
        for (int32 key = 0; key != 80; ++key) {
            auto inserted = map.Insert(MoveOnlyKey(key), TrackedValue(key * 5));
            REQUIRE(inserted);
            CHECK(inserted.Value() == InsertOutcome::Inserted);
        }
        CHECK(TrackedValue::live == 80);
        // 재해시로 키가 이동한 뒤에도 새 키의 해시·동등 비교로 찾을 수 있어야 한다.
        for (int32 key = 0; key != 80; ++key) {
            auto* value = map.Find(MoveOnlyKey(key));
            REQUIRE(value != nullptr);
            CHECK(value->value == key * 5);
            CHECK(reinterpret_cast<uptr>(value) % alignof(TrackedValue) == 0);
        }
        auto duplicate = map.Insert(MoveOnlyKey(13), TrackedValue(-5));
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        CHECK(TrackedValue::live == 80);
        REQUIRE(map.Find(MoveOnlyKey(13)) != nullptr);
        CHECK(map.Find(MoveOnlyKey(13))->value == 65);
        for (int32 key = 0; key != 80; key += 2) {
            CHECK(map.Erase(MoveOnlyKey(key)));
        }
        CHECK(TrackedValue::live == 40);
        map.Clear();
        CHECK(TrackedValue::live == 0);
    }
    CHECK(TrackedValue::live == 0);
    CHECK(TrackedValue::constructed == TrackedValue::destroyed);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashSetSupportsMoveOnlyKeys) {
    SystemAllocator allocator;
    {
        HashSet<MoveOnlyKey, MoveKeyHash, MoveKeyEqual> set(allocator);
        for (int32 key = 0; key != 64; ++key) {
            auto added = set.Add(MoveOnlyKey(key));
            REQUIRE(added);
            CHECK(added.Value() == InsertOutcome::Inserted);
        }
        CHECK(set.Count() == 64);
        CHECK(set.Contains(MoveOnlyKey(7)));
        CHECK(set.Erase(MoveOnlyKey(7)));
        CHECK(!set.Contains(MoveOnlyKey(7)));
        auto restored = set.Add(MoveOnlyKey(7));
        REQUIRE(restored);
        CHECK(restored.Value() == InsertOutcome::Inserted);
        CHECK(set.Contains(MoveOnlyKey(7)));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashCollectionsRandomizedCollisionModel) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32, CollisionHash> map(allocator);
        HashSet<int32, CollisionHash> set(allocator);
        bool exists[64]{};
        int32 values[64]{};
        usize count = 0;
        uint32 state = 0x9e3779b9u;
        // 일부 키의 존재만 확인하면 삭제 뒤 충돌 사슬의 손상을 놓칠 수 있다.
        for (uint32 step = 0; step != 12000; ++step) {
            state = state * 1664525u + 1013904223u;
            const int32 key = static_cast<int32>((state >> 8) % 64);
            const uint32 operation = (state >> 16) % 8;
            const int32 value = static_cast<int32>(step);
            if (operation <= 2) {
                const bool old = exists[key];
                auto inserted = map.Insert(key, value);
                auto added = set.Add(key);
                REQUIRE(inserted && added);
                CHECK(inserted.Value() ==
                      (old ? InsertOutcome::AlreadyPresent : InsertOutcome::Inserted));
                CHECK(added.Value() ==
                      (old ? InsertOutcome::AlreadyPresent : InsertOutcome::Inserted));
                if (!old) {
                    exists[key] = true;
                    values[key] = value;
                    ++count;
                }
            } else if (operation == 3) {
                CHECK(map.Erase(key) == exists[key]);
                CHECK(set.Erase(key) == exists[key]);
                if (exists[key]) {
                    exists[key] = false;
                    --count;
                }
            } else if (operation == 4) {
                auto updated = map.InsertOrAssign(key, value);
                auto added = set.Add(key);
                REQUIRE(updated && added);
                CHECK(updated.Value() ==
                      (exists[key] ? InsertOutcome::AlreadyPresent : InsertOutcome::Inserted));
                CHECK(added.Value() ==
                      (exists[key] ? InsertOutcome::AlreadyPresent : InsertOutcome::Inserted));
                if (!exists[key]) {
                    exists[key] = true;
                    ++count;
                }
                values[key] = value;
            } else if (operation == 5) {
                REQUIRE(map.Reserve((state >> 24) % 128));
                REQUIRE(set.Reserve((state >> 24) % 128));
            } else if (operation == 6 && (state & 1023u) == 0) {
                map.Clear();
                set.Clear();
                for (usize index = 0; index != 64; ++index) {
                    exists[index] = false;
                }
                count = 0;
            }
            CHECK(map.Count() == count);
            CHECK(set.Count() == count);
            for (int32 index = 0; index != 64; ++index) {
                const int32* actual = map.Find(index);
                CHECK((actual != nullptr) == exists[index]);
                CHECK(set.Contains(index) == exists[index]);
                if (actual) {
                    CHECK(*actual == values[index]);
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(CoreHashCollectionsMoveAndAllocatorOwnership) {
    SystemAllocator first;
    SystemAllocator second;
    {
        HashMap<int32, int32> source(first);
        REQUIRE(source.Insert(1, 10));
        HashMap<int32, int32> moved(Move(source));
        CHECK(source.IsEmpty());
        REQUIRE(moved.Find(1) != nullptr);
        CHECK(*moved.Find(1) == 10);
        REQUIRE(source.Insert(2, 20));
        HashMap<int32, int32> target(second);
        REQUIRE(target.Insert(3, 30));
        target = Move(moved);
        CHECK(moved.IsEmpty());
        CHECK(target.Find(3) == nullptr);
        REQUIRE(target.Find(1) != nullptr);
        CHECK(*target.Find(1) == 10);
        REQUIRE(moved.Insert(4, 40));

        HashSet<int32> sourceSet(first);
        REQUIRE(sourceSet.Add(1));
        HashSet<int32> movedSet(Move(sourceSet));
        CHECK(sourceSet.IsEmpty());
        CHECK(movedSet.Contains(1));
        REQUIRE(sourceSet.Add(2));
        HashSet<int32> targetSet(second);
        REQUIRE(targetSet.Add(3));
        targetSet = Move(movedSet);
        CHECK(movedSet.IsEmpty());
        CHECK(targetSet.Contains(1));
        CHECK(!targetSet.Contains(3));
        REQUIRE(movedSet.Add(4));
    }
    CHECK(first.Stats().liveAllocations == 0);
    CHECK(first.Stats().liveBytes == 0);
    CHECK(second.Stats().liveAllocations == 0);
    CHECK(second.Stats().liveBytes == 0);
}

TEST(CoreHashCollectionsTraversalAndCapacity) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32> map(allocator);
        REQUIRE(map.Reserve(40));
        CHECK(map.Capacity() >= 40);
        for (int32 key = 0; key != 40; ++key) {
            REQUIRE(map.Insert(key, key));
        }
        int32 sum = 0;
        map.ForEach([&](const int32& key, int32& value) noexcept {
            sum += key;
            value += 100;
        });
        CHECK(sum == 780);
        const auto& constMap = map;
        int32 constSum = 0;
        constMap.ForEach([&](const int32& key, const int32& value) noexcept {
            CHECK(value == key + 100);
            constSum += value;
        });
        CHECK(constSum == 4780);
        const usize capacity = map.Capacity();
        map.Clear();
        CHECK(map.IsEmpty());
        CHECK(map.Capacity() == capacity);
        map.Reset();
        CHECK(map.IsEmpty());
        CHECK(map.Capacity() == 0);
        HashSet<int32> set(allocator);
        REQUIRE(set.Reserve(40));
        for (int32 key = 0; key != 40; ++key) {
            REQUIRE(set.Add(key));
        }
        int32 setSum = 0;
        set.ForEach([&](const int32& key) noexcept { setSum += key; });
        CHECK(setSum == 780);
        const usize setCapacity = set.Capacity();
        set.Clear();
        CHECK(set.IsEmpty());
        CHECK(set.Capacity() == setCapacity);
        set.Reset();
        CHECK(set.Capacity() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashCollectionsStringAndNameKeys) {
    SystemAllocator allocator;
    {
        // 내부 NUL 뒤의 바이트도 동등성과 해시에 포함해야 한다.
        const char bytes[] = {'a', '\0', 'b'};
        const char equivalent[] = {'a', '\0', 'b'};
        HashMap<StringView, int32> views(allocator);
        REQUIRE(views.Insert(StringView(bytes, 3), 41));
        auto duplicate = views.Insert(StringView(equivalent, 3), 42);
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        REQUIRE(views.Find(StringView(equivalent, 3)) != nullptr);
        CHECK(*views.Find(StringView(equivalent, 3)) == 41);
        CHECK(views.Find(StringView(bytes, 1)) == nullptr);

        String first(allocator);
        String second(allocator);
        REQUIRE(first.TryAssign(StringView(bytes, 3)));
        REQUIRE(second.TryAssign(StringView(equivalent, 3)));
        HashMap<String, int32> strings(allocator);
        REQUIRE(strings.Insert(Move(first), 91));
        REQUIRE(strings.Find(second) != nullptr);
        CHECK(*strings.Find(second) == 91);
        // 소유 문자열과 별도 버퍼의 view를 이종 조회·삭제한다.
        const StringView matching(equivalent, 3);
        const StringView missing(equivalent, 1);
        REQUIRE(strings.Find(matching) != nullptr);
        CHECK(*strings.Find(matching) == 91);
        CHECK(strings.Contains(matching));
        CHECK(!strings.Contains(missing));
        CHECK(!strings.Erase(missing));
        CHECK(strings.Erase(matching));
        CHECK(strings.Find(matching) == nullptr);
        CHECK(strings.IsEmpty());

        HashSet<String> stringSet(allocator);
        String owned(allocator);
        REQUIRE(owned.TryAssign(StringView(bytes, 3)));
        REQUIRE(stringSet.Add(Move(owned)));
        CHECK(stringSet.Contains(matching));
        CHECK(!stringSet.Contains(missing));
        CHECK(!stringSet.Erase(missing));
        CHECK(stringSet.Erase(matching));
        CHECK(!stringSet.Contains(matching));

        HashSet<Name> names(allocator);
        REQUIRE(names.Add(Name{1, 7}));
        CHECK(names.Contains(Name{1, 7}));
        CHECK(!names.Contains(Name{2, 7}));
        auto same = names.Add(Name{1, 7});
        REQUIRE(same);
        CHECK(same.Value() == InsertOutcome::AlreadyPresent);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashCollectionsDoNotConsumeInputsOnFailureOrDuplicate) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        HashMap<MoveOnlyKey, TrackedValue, MoveKeyHash, MoveKeyEqual> map(allocator);
        // 할당 실패 및 중복은 전달받은 rvalue를 이동하지 않는다.
        MoveOnlyKey key(5);
        TrackedValue value(15);
        allocator.FailOnAttempt(1);
        auto failed = map.Insert(Move(key), Move(value));
        CHECK(!failed);
        CHECK(failed.Error() == ErrorCode::OutOfMemory);
        CHECK(key.value == 5);
        CHECK(value.value == 15);
        CHECK(map.IsEmpty());
        allocator.DisableFailure();
        auto success = map.Insert(Move(key), Move(value));
        REQUIRE(success);
        CHECK(success.Value() == InsertOutcome::Inserted);
        MoveOnlyKey duplicateKey(5);
        TrackedValue duplicateValue(44);
        auto duplicate = map.Insert(Move(duplicateKey), Move(duplicateValue));
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        CHECK(duplicateKey.value == 5);
        CHECK(duplicateValue.value == 44);
        REQUIRE(map.Find(MoveOnlyKey(5)) != nullptr);
        CHECK(map.Find(MoveOnlyKey(5))->value == 15);
        // 저장된 값을 입력으로 다시 쓰는 경우 원본을 손상시키기 전에 거부한다.
        auto alias = map.Insert(MoveOnlyKey(6), Move(*map.Find(MoveOnlyKey(5))));
        CHECK(!alias);
        CHECK(alias.Error() == ErrorCode::InvalidArgument);
        CHECK(map.Count() == 1);
        CHECK(map.Find(MoveOnlyKey(5))->value == 15);

        HashSet<MoveOnlyKey, MoveKeyHash, MoveKeyEqual> set(allocator);
        MoveOnlyKey setKey(6);
        allocator.FailOnAttempt(1);
        auto setFailure = set.Add(Move(setKey));
        CHECK(!setFailure);
        CHECK(setFailure.Error() == ErrorCode::OutOfMemory);
        CHECK(setKey.value == 6);
        allocator.DisableFailure();
        REQUIRE(set.Add(Move(setKey)));
        MoveOnlyKey repeated(6);
        auto setDuplicate = set.Add(Move(repeated));
        REQUIRE(setDuplicate);
        CHECK(setDuplicate.Value() == InsertOutcome::AlreadyPresent);
        CHECK(repeated.value == 6);
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
    CHECK(TrackedValue::live == 0);
}

TEST(CoreHashMapRejectsAliasedKeyAndValueInputs) {
    SystemAllocator allocator;
    {
        HashMap<MoveOnlyKey, MoveOnlyKey, MoveKeyHash, MoveKeyEqual> map(allocator);
        MoveOnlyKey both(9);
        // 같은 객체를 키와 값으로 동시에 이동하면 두 결과를 모두 보존할 수 없다.
        auto inserted = map.Insert(Move(both), Move(both));
        CHECK(!inserted);
        CHECK(inserted.Error() == ErrorCode::InvalidArgument);
        CHECK(both.value == 9);
        CHECK(map.IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashCollectionsTraversalRejectsStructuralChanges) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32> map(allocator);
        REQUIRE(map.Insert(1, 2));
        int32 visits = 0;
        // 중첩 읽기와 값 갱신은 허용하지만 구조 변경은 Busy로 거부한다.
        map.ForEach([&](const int32& key, int32& value) noexcept {
            ++visits;
            CHECK(key == 1);
            CHECK(map.Find(1) != nullptr);
            CHECK(map.Insert(9, 9).Error() == ErrorCode::Busy);
            CHECK(map.InsertOrAssign(1, 9).Error() == ErrorCode::Busy);
            CHECK(map.Reserve(80).Error() == ErrorCode::Busy);
            int32 nested = 0;
            const auto& readOnly = map;
            readOnly.ForEach([&](const int32&, const int32&) noexcept { ++nested; });
            CHECK(nested == 1);
            value = 3;
        });
        CHECK(visits == 1);
        CHECK(map.Count() == 1);
        REQUIRE(map.Find(1) != nullptr);
        CHECK(*map.Find(1) == 3);
        HashSet<int32> set(allocator);
        REQUIRE(set.Add(4));
        set.ForEach([&](const int32& key) noexcept {
            CHECK(key == 4);
            CHECK(set.Contains(4));
            CHECK(set.Add(9).Error() == ErrorCode::Busy);
            CHECK(set.Reserve(80).Error() == ErrorCode::Busy);
            int32 nested = 0;
            set.ForEach([&](const int32&) noexcept { ++nested; });
            CHECK(nested == 1);
        });
        CHECK(set.Count() == 1);
        CHECK(set.Contains(4));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace rocks::test {

// 별도 child에서만 실행한다. 요청된 Fatal이 발생하지 않으면 0을 반환해 음성 대조를 실패시킨다.
int RunHashTraversalNegativeControl(bool useSet, uint32 operation) noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    if (useSet) {
        HashSet<int32> set(allocator);
        if (!set.Add(1)) {
            return 0;
        }
        set.ForEach([&](const int32&) noexcept {
            if (operation == 0) {
                (void)set.Erase(1);
            } else if (operation == 1) {
                set.Clear();
            } else {
                set.Reset();
            }
        });
    } else {
        HashMap<int32, int32> map(allocator);
        if (!map.Insert(1, 2)) {
            return 0;
        }
        map.ForEach([&](const int32&, int32&) noexcept {
            if (operation == 0) {
                (void)map.Erase(1);
            } else if (operation == 1) {
                map.Clear();
            } else {
                map.Reset();
            }
        });
    }
    return 0;
}

} // namespace rocks::test

namespace {

struct SaltedModuloHash {
    int32 salt;
    int32 modulus;
    usize operator()(int32 value) const noexcept {
        return static_cast<usize>((value % modulus) ^ salt);
    }
};

struct ModuloEqual {
    int32 modulus;
    bool operator()(int32 left, int32 right) const noexcept {
        return left % modulus == right % modulus;
    }
};

struct WraparoundHash {
    usize operator()(int32) const noexcept {
        return MaxSize;
    }
};

} // namespace

TEST(CoreHashCollectionsMovePreservesStatefulPolicies) {
    SystemAllocator first;
    SystemAllocator second;
    {
        // 비교 규칙과 해시의 상태를 함께 옮겨야 기존 키와 동등한 키를 찾을 수 있다.
        HashMap<int32, int32, SaltedModuloHash, ModuloEqual> source(
            first, SaltedModuloHash{119, 10}, ModuloEqual{10});
        REQUIRE(source.Insert(7, 50));
        HashMap<int32, int32, SaltedModuloHash, ModuloEqual> moved(Move(source));
        REQUIRE(moved.Find(17) != nullptr);
        CHECK(*moved.Find(17) == 50);
        auto duplicate = moved.Insert(27, 80);
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        REQUIRE(source.Insert(2, 20));
        CHECK(source.Contains(12));
        HashMap<int32, int32, SaltedModuloHash, ModuloEqual> target(second, SaltedModuloHash{1, 2},
                                                                    ModuloEqual{2});
        REQUIRE(target.Insert(3, 30));
        target = Move(moved);
        REQUIRE(target.Find(37) != nullptr);
        CHECK(*target.Find(37) == 50);
        CHECK(!target.Contains(3));
        REQUIRE(moved.Insert(4, 40));
        CHECK(moved.Contains(14));

        HashSet<int32, SaltedModuloHash, ModuloEqual> set(first, SaltedModuloHash{50, 10},
                                                          ModuloEqual{10});
        REQUIRE(set.Add(8));
        HashSet<int32, SaltedModuloHash, ModuloEqual> next(Move(set));
        CHECK(next.Contains(18));
        REQUIRE(set.Add(5));
        CHECK(set.Contains(15));
        HashSet<int32, SaltedModuloHash, ModuloEqual> destination(second, SaltedModuloHash{1, 2},
                                                                  ModuloEqual{2});
        REQUIRE(destination.Add(3));
        destination = Move(next);
        CHECK(destination.Contains(28));
        CHECK(!destination.Contains(3));
    }
    CHECK(first.Stats().liveAllocations == 0);
    CHECK(second.Stats().liveAllocations == 0);
}

TEST(CoreHashCollectionsCompactFailureAndRecovery) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        HashMap<int32, int32, CollisionHash> map(allocator);
        REQUIRE(map.Reserve(128));
        for (int32 key = 0; key != 4; ++key) {
            REQUIRE(map.Insert(key, key + 10));
        }
        CHECK(map.Erase(0));
        const usize oldCapacity = map.Capacity();
        int32* oldPointer = map.Find(2);
        REQUIRE(oldPointer != nullptr);
        allocator.FailOnAttempt(1);
        // 축소 할당에 실패하면 기존 버퍼, 포인터, 용량과 모든 원소를 보존한다.
        auto fail = map.Compact();
        CHECK(!fail);
        CHECK(fail.Error() == ErrorCode::OutOfMemory);
        CHECK(map.Capacity() == oldCapacity);
        CHECK(map.Find(2) == oldPointer);
        CHECK(*map.Find(2) == 12);
        CHECK(map.Count() == 3);
        allocator.DisableFailure();
        REQUIRE(map.Compact());
        CHECK(map.Capacity() < oldCapacity);
        CHECK(map.Count() == 3);
        for (int32 key = 1; key != 4; ++key) {
            REQUIRE(map.Find(key) != nullptr);
            CHECK(*map.Find(key) == key + 10);
        }
        map.ForEach([&](const int32&, int32&) noexcept {
            CHECK(map.Compact().Error() == ErrorCode::Busy);
        });

        HashSet<int32, CollisionHash> set(allocator);
        REQUIRE(set.Reserve(128));
        REQUIRE(set.Add(11));
        REQUIRE(set.Add(12));
        const int32* stored = set.Find(11);
        REQUIRE(stored != nullptr);
        const usize setCapacity = set.Capacity();
        allocator.FailOnAttempt(1);
        auto setFailure = set.Compact();
        CHECK(!setFailure);
        CHECK(setFailure.Error() == ErrorCode::OutOfMemory);
        CHECK(set.Find(11) == stored);
        CHECK(set.Capacity() == setCapacity);
        CHECK(set.Contains(12));
        allocator.DisableFailure();
        REQUIRE(set.Compact());
        CHECK(set.Capacity() < setCapacity);
        CHECK(set.Contains(11));
        set.ForEach(
            [&](const int32&) noexcept { CHECK(set.Compact().Error() == ErrorCode::Busy); });
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreHashCollectionsAutomaticTombstoneCleanupFailure) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        HashMap<int32, TrackedValue, CollisionHash> map(allocator);
        REQUIRE(map.Reserve(16));
        for (int32 key = 0; key != 12; ++key) {
            REQUIRE(map.Insert(key, TrackedValue(key * 10)));
        }
        for (int32 key = 0; key != 9; ++key) {
            CHECK(map.Erase(key));
        }
        auto* old = map.Find(10);
        REQUIRE(old != nullptr);
        const usize oldCapacity = map.Capacity();
        TrackedValue candidate(900);
        // 삭제 슬롯이 쌓인 상황에서 자동 재구성이 실패해도 후보와 기존 주소를 보존한다.
        allocator.FailOnAttempt(1);
        auto fail = map.Insert(99, Move(candidate));
        CHECK(!fail);
        CHECK(fail.Error() == ErrorCode::OutOfMemory);
        CHECK(candidate.value == 900);
        CHECK(map.Capacity() == oldCapacity);
        CHECK(map.Find(10) == old);
        CHECK(old->value == 100);
        CHECK(map.Count() == 3);
        CHECK(map.Find(99) == nullptr);
        allocator.DisableFailure();
        auto inserted = map.Insert(99, Move(candidate));
        REQUIRE(inserted);
        CHECK(inserted.Value() == InsertOutcome::Inserted);
        REQUIRE(map.Find(99) != nullptr);
        CHECK(map.Find(99)->value == 900);
        REQUIRE(map.Find(10) != nullptr);
        CHECK(map.Find(10)->value == 100);
    }
    CHECK(TrackedValue::live == 0);
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreHashCollisionWraparoundAndDuplicateBeyondDeletion) {
    SystemAllocator allocator;
    {
        // 충돌 탐색은 마지막 슬롯에서 시작해 배열 앞부분으로 이어질 수 있다.
        HashMap<int32, int32, WraparoundHash> map(allocator);
        REQUIRE(map.Reserve(8));
        for (int32 key = 0; key != 8; ++key) {
            REQUIRE(map.Insert(key, 10 + key));
        }
        CHECK(map.Erase(1));
        CHECK(map.Erase(3));
        auto duplicate = map.Insert(7, 99);
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        CHECK(map.Count() == 6);
        REQUIRE(map.Find(7) != nullptr);
        CHECK(*map.Find(7) == 17);
        REQUIRE(map.Insert(11, 21));
        REQUIRE(map.Insert(12, 22));
        for (int32 key = 0; key != 8; ++key) {
            if (key == 1 || key == 3) {
                CHECK(map.Find(key) == nullptr);
            } else {
                REQUIRE(map.Find(key) != nullptr);
                CHECK(*map.Find(key) == 10 + key);
            }
        }
        REQUIRE(map.Find(11) != nullptr);
        REQUIRE(map.Find(12) != nullptr);
        CHECK(*map.Find(11) == 21);
        CHECK(*map.Find(12) == 22);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashCollectionsGuidKeys) {
    SystemAllocator allocator;
    {
        // high와 low를 각각 바꾼 식별자를 별도 키로 보관한다.
        HashMap<Guid, int32> map(allocator);
        REQUIRE(map.Insert(Guid{1, 2}, 10));
        REQUIRE(map.Insert(Guid{2, 2}, 20));
        REQUIRE(map.Insert(Guid{1, 3}, 30));
        auto duplicate = map.Insert(Guid{1, 2}, 40);
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        CHECK(map.Count() == 3);
        REQUIRE(map.Find(Guid{1, 2}) != nullptr);
        REQUIRE(map.Find(Guid{2, 2}) != nullptr);
        REQUIRE(map.Find(Guid{1, 3}) != nullptr);
        CHECK(*map.Find(Guid{1, 2}) == 10);
        CHECK(*map.Find(Guid{2, 2}) == 20);
        CHECK(*map.Find(Guid{1, 3}) == 30);
        CHECK(map.Find(Guid{2, 3}) == nullptr);

        HashSet<Guid> set(allocator);
        REQUIRE(set.Add(Guid{1, 2}));
        REQUIRE(set.Add(Guid{2, 2}));
        REQUIRE(set.Add(Guid{1, 3}));
        CHECK(set.Contains(Guid{1, 2}));
        CHECK(set.Contains(Guid{2, 2}));
        CHECK(set.Contains(Guid{1, 3}));
        CHECK(!set.Contains(Guid{2, 3}));
        auto same = set.Add(Guid{1, 2});
        REQUIRE(same);
        CHECK(same.Value() == InsertOutcome::AlreadyPresent);
        CHECK(set.Count() == 3);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashMapGrowthInsertionPreservesInputsAndEntriesOnAllocationFailure) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        HashMap<MoveOnlyKey, TrackedValue, MoveKeyHash, MoveKeyEqual> map(allocator);
        REQUIRE(map.Reserve(4));
        const usize capacity = map.Capacity();
        REQUIRE(capacity >= 4 && capacity <= 16);
        TrackedValue* originalPointers[16]{};

        // 빈 슬롯을 채우면 다음 신규 삽입은 반드시 저장소를 확장해야 한다.
        for (usize index = 0; index != capacity; ++index) {
            auto inserted = map.Insert(MoveOnlyKey(static_cast<int32>(index)),
                                       TrackedValue(static_cast<int32>(index * 10)));
            REQUIRE(inserted);
            CHECK(inserted.Value() == InsertOutcome::Inserted);
        }
        CHECK(map.Count() == capacity);
        for (usize index = 0; index != capacity; ++index) {
            originalPointers[index] = map.Find(MoveOnlyKey(static_cast<int32>(index)));
            REQUIRE(originalPointers[index] != nullptr);
        }
        auto* existing = map.Find(MoveOnlyKey(2));
        REQUIRE(existing != nullptr);
        CHECK(existing->value == 20);

        MoveOnlyKey newKey(99);
        TrackedValue newValue(990);
        allocator.FailOnAttempt(1);
        auto failed = map.Insert(Move(newKey), Move(newValue));
        CHECK(!failed);
        CHECK(failed.Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Attempts() == 1);
        CHECK(newKey.value == 99);
        CHECK(newValue.value == 990);
        CHECK(map.Count() == capacity);
        CHECK(map.Capacity() == capacity);
        CHECK(map.Find(MoveOnlyKey(2)) == existing);
        CHECK(existing->value == 20);
        CHECK(map.Find(MoveOnlyKey(99)) == nullptr);
        for (usize index = 0; index != capacity; ++index) {
            auto* value = map.Find(MoveOnlyKey(static_cast<int32>(index)));
            REQUIRE(value != nullptr);
            CHECK(value == originalPointers[index]);
            CHECK(value->value == static_cast<int32>(index * 10));
        }

        // 확장은 단 한 번의 할당으로 완료되어야 두 번째 할당 실패 주입이 영향을 주지 않는다.
        allocator.FailOnAttempt(2);
        auto inserted = map.Insert(Move(newKey), Move(newValue));
        REQUIRE(inserted);
        CHECK(inserted.Value() == InsertOutcome::Inserted);
        CHECK(allocator.Attempts() == 1);
        CHECK(map.Count() == capacity + 1);
        CHECK(map.Capacity() > capacity);
        REQUIRE(map.Find(MoveOnlyKey(99)) != nullptr);
        CHECK(map.Find(MoveOnlyKey(99))->value == 990);
        for (usize index = 0; index != capacity; ++index) {
            auto* value = map.Find(MoveOnlyKey(static_cast<int32>(index)));
            REQUIRE(value != nullptr);
            CHECK(value->value == static_cast<int32>(index * 10));
        }
    }
    CHECK(TrackedValue::live == 0);
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreHashMapAllowsSameCopyableKeyAndValueLvalue) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32> map(allocator);
        int32 both = 17;

        // 같은 읽기 전용 원본에서 키와 값을 복사하는 사용은 안전하다.
        auto inserted = map.Insert(both, both);
        REQUIRE(inserted);
        CHECK(inserted.Value() == InsertOutcome::Inserted);
        CHECK(both == 17);
        REQUIRE(map.Find(17) != nullptr);
        CHECK(*map.Find(17) == 17);

        auto duplicate = map.Insert(both, both);
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        auto assigned = map.InsertOrAssign(both, both);
        REQUIRE(assigned);
        CHECK(assigned.Value() == InsertOutcome::AlreadyPresent);
        CHECK(both == 17);
        CHECK(map.Count() == 1);
        CHECK(*map.Find(17) == 17);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}
