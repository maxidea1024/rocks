#include <Core/Containers/HashMap.h>
#include <Core/Containers/HashSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <Tests/Support/HostFailure.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

struct CollisionHash {
    uint64 operator()(int32) const noexcept {
        return ~0ull;
    }
};

struct MoveKey {
    int32 value;

    explicit MoveKey(int32 value) noexcept : value(value) {
    }
    MoveKey(const MoveKey&) = delete;
    MoveKey& operator=(const MoveKey&) = delete;
    MoveKey(MoveKey&& other) noexcept : value(other.value) {
        other.value = -1;
    }
};

struct MoveKeyHash {
    uint64 operator()(const MoveKey& value) const noexcept {
        return MixHash(static_cast<uint64>(value.value));
    }
};

struct MoveKeyEqual {
    bool operator()(const MoveKey& left, const MoveKey& right) const noexcept {
        return left.value == right.value;
    }
};

struct alignas(64) MoveValue {
    static inline int32 live = 0;
    int32 value = 0;

    explicit MoveValue(int32 value = 0) noexcept : value(value) {
        ++live;
    }
    MoveValue(const MoveValue&) = delete;
    MoveValue& operator=(const MoveValue&) = delete;
    MoveValue(MoveValue&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
    }
    ~MoveValue() noexcept {
        --live;
    }
};

struct ModularHash {
    int32 modulus = 1;

    uint64 operator()(int32 value) const noexcept {
        return MixHash(static_cast<uint64>(value % modulus));
    }
};

struct ModularEqual {
    int32 modulus = 1;

    bool operator()(int32 left, int32 right) const noexcept {
        return left % modulus == right % modulus;
    }
};

struct OverloadedMapPredicate {
    int32* constCalls;
    int32* mutableCalls;

    bool operator()(const int32& key, const int32&) noexcept {
        ++*constCalls;
        return key == 1;
    }

    bool operator()(int32&, int32&) noexcept {
        ++*mutableCalls;
        return false;
    }
};

struct OverloadedSetPredicate {
    int32* constCalls;
    int32* mutableCalls;

    bool operator()(const int32& key) noexcept {
        ++*constCalls;
        return key == 1;
    }

    bool operator()(int32&) noexcept {
        ++*mutableCalls;
        return false;
    }
};

struct HashReentryState;

struct ReentrantHash {
    HashReentryState* state = nullptr;

    uint64 operator()(int32 key) const noexcept;
};

using ReentrantSet = HashSet<int32, ReentrantHash>;

struct HashReentryState {
    ReentrantSet* target = nullptr;
    int32 busyCount = 0;
    int32 unexpectedCount = 0;
    bool active = false;
};

uint64 ReentrantHash::operator()(int32 key) const noexcept {
    if (state != nullptr && state->target != nullptr && !state->active) {
        state->active = true;
        auto inserted = state->target->Add(999);
        if (!inserted && inserted.Error() == ErrorCode::Busy) {
            ++state->busyCount;
        } else {
            ++state->unexpectedCount;
        }
        state->active = false;
    }
    return MixHash(static_cast<uint64>(key));
}

struct CloneReentryState;

struct ReentrantCloneKey {
    int32 value = 0;
    CloneReentryState* state = nullptr;
    bool trigger = false;

    ReentrantCloneKey(int32 value, CloneReentryState* state = nullptr,
                      bool trigger = false) noexcept
        : value(value), state(state), trigger(trigger) {
    }

    ReentrantCloneKey(const ReentrantCloneKey&) = delete;
    ReentrantCloneKey& operator=(const ReentrantCloneKey&) = delete;

    ReentrantCloneKey(ReentrantCloneKey&& other) noexcept
        : value(other.value), state(other.state), trigger(other.trigger) {
        other.trigger = false;
    }

    ~ReentrantCloneKey() noexcept;
};

struct ReentrantCloneHash {
    uint64 operator()(const ReentrantCloneKey& key) const noexcept {
        return MixHash(static_cast<uint64>(key.value));
    }
};

struct ReentrantCloneEqual {
    bool operator()(const ReentrantCloneKey& left, const ReentrantCloneKey& right) const noexcept {
        return left.value == right.value;
    }
};

using ReentrantCloneSet = HashSet<ReentrantCloneKey, ReentrantCloneHash, ReentrantCloneEqual>;

struct CloneReentryState {
    ReentrantCloneSet* left = nullptr;
    ReentrantCloneSet* right = nullptr;
    int32 busyCount = 0;
    int32 unexpectedCount = 0;
};

ReentrantCloneKey::~ReentrantCloneKey() noexcept {
    if (!trigger || state == nullptr) {
        return;
    }
    auto leftAttempt = state->left->Add(ReentrantCloneKey(901));
    auto rightAttempt = state->right->Add(ReentrantCloneKey(902));
    if (!leftAttempt && leftAttempt.Error() == ErrorCode::Busy) {
        ++state->busyCount;
    } else {
        ++state->unexpectedCount;
    }
    if (!rightAttempt && rightAttempt.Error() == ErrorCode::Busy) {
        ++state->busyCount;
    } else {
        ++state->unexpectedCount;
    }
}

} // namespace

TEST(CoreHashMapFindOrAddUsesExistingOrDefaultValue) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32> map(allocator);
        auto first = map.FindOrAdd(1, 17);
        REQUIRE(first);
        CHECK(first.Value().outcome == InsertOutcome::Inserted);
        REQUIRE(first.Value().value != nullptr);
        CHECK(*first.Value().value == 17);

        int32* original = first.Value().value;
        auto duplicate = map.FindOrAdd(1, 200);
        REQUIRE(duplicate);
        CHECK(duplicate.Value().outcome == InsertOutcome::AlreadyPresent);
        CHECK(duplicate.Value().value == original);
        CHECK(*duplicate.Value().value == 17);

        auto empty = map.FindOrAdd(2);
        REQUIRE(empty);
        CHECK(empty.Value().outcome == InsertOutcome::Inserted);
        REQUIRE(empty.Value().value != nullptr);
        CHECK(*empty.Value().value == 0);
        CHECK(map.Count() == 2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(CoreHashMapFindOrAddFactoryAndAllocationFailuresPreserveState) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        HashMap<MoveKey, MoveValue, MoveKeyHash, MoveKeyEqual> map(allocator);
        REQUIRE(map.Reserve(4));
        const usize capacity = map.Capacity();
        for (usize index = 0; index != capacity; ++index) {
            REQUIRE(map.Insert(MoveKey(static_cast<int32>(index)),
                               MoveValue(static_cast<int32>(index * 10))));
        }
        MoveValue* old = map.Find(MoveKey(0));
        REQUIRE(old != nullptr);
        int32 calls = 0;

        // 기존 키는 준비 함수를 호출하지 않고 새 값 인자도 소비하지 않는다.
        MoveKey duplicateKey(0);
        allocator.FailOnAttempt(1);
        auto duplicate = map.FindOrAddWith(Move(duplicateKey), [&]() noexcept -> Result<MoveValue> {
            ++calls;
            return Result<MoveValue>::Success(MoveValue(999));
        });
        REQUIRE(duplicate);
        CHECK(duplicate.Value().outcome == InsertOutcome::AlreadyPresent);
        CHECK(duplicate.Value().value == old);
        CHECK(duplicateKey.value == 0);
        CHECK(calls == 0);
        CHECK(allocator.Attempts() == 0);

        MoveKey nextKey(99);
        auto factoryFailure = map.FindOrAddWith(Move(nextKey), [&]() noexcept -> Result<MoveValue> {
            ++calls;
            return Result<MoveValue>::Failure(ErrorCode::DependencyFailed);
        });
        CHECK(!factoryFailure);
        CHECK(factoryFailure.Error() == ErrorCode::DependencyFailed);
        CHECK(calls == 1);
        CHECK(nextKey.value == 99);
        CHECK(map.Capacity() == capacity);
        CHECK(map.Count() == capacity);
        CHECK(map.Find(MoveKey(0)) == old);
        CHECK(allocator.Attempts() == 0);

        // 준비가 끝난 뒤 확장 할당에 실패해도 임시 객체가 회수되고 기존 포인터가 살아 있다.
        const int32 liveBefore = MoveValue::live;
        allocator.FailOnAttempt(1);
        auto allocationFailure =
            map.FindOrAddWith(Move(nextKey), [&]() noexcept -> Result<MoveValue> {
                ++calls;
                return Result<MoveValue>::Success(MoveValue(990));
            });
        CHECK(!allocationFailure);
        CHECK(allocationFailure.Error() == ErrorCode::OutOfMemory);
        CHECK(calls == 2);
        CHECK(MoveValue::live == liveBefore);
        CHECK(nextKey.value == 99);
        CHECK(map.Capacity() == capacity);
        CHECK(map.Count() == capacity);
        CHECK(map.Find(MoveKey(0)) == old);
        CHECK(old->value == 0);

        allocator.DisableFailure();
        auto inserted = map.FindOrAddWith(Move(nextKey), [&]() noexcept -> Result<MoveValue> {
            ++calls;
            return Result<MoveValue>::Success(MoveValue(990));
        });
        REQUIRE(inserted);
        CHECK(calls == 3);
        CHECK(inserted.Value().outcome == InsertOutcome::Inserted);
        REQUIRE(inserted.Value().value != nullptr);
        CHECK(inserted.Value().value->value == 990);
        CHECK(nextKey.value == -1);
    }
    CHECK(MoveValue::live == 0);
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreHashMapFindOrAddDoesNotConsumeDuplicateInputs) {
    SystemAllocator allocator;
    {
        HashMap<MoveKey, MoveValue, MoveKeyHash, MoveKeyEqual> map(allocator);
        REQUIRE(map.Insert(MoveKey(5), MoveValue(50)));
        MoveKey key(5);
        MoveValue value(99);
        auto found = map.FindOrAdd(Move(key), Move(value));
        REQUIRE(found);
        CHECK(found.Value().outcome == InsertOutcome::AlreadyPresent);
        CHECK(found.Value().value == map.Find(MoveKey(5)));
        CHECK(found.Value().value->value == 50);
        CHECK(key.value == 5);
        CHECK(value.value == 99);
    }
    CHECK(MoveValue::live == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashMapFindOrAddFactoryCannotMutateSameMap) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32> map(allocator);
        REQUIRE(map.Insert(1, 10));
        int32 calls = 0;
        auto added = map.FindOrAddWith(2, [&]() noexcept -> Result<int32> {
            ++calls;
            CHECK(map.Find(1) != nullptr);
            CHECK(map.Insert(3, 30).Error() == ErrorCode::Busy);
            CHECK(map.Reserve(100).Error() == ErrorCode::Busy);
            CHECK(map.Compact().Error() == ErrorCode::Busy);
            return Result<int32>::Success(20);
        });
        REQUIRE(added);
        CHECK(calls == 1);
        CHECK(added.Value().outcome == InsertOutcome::Inserted);
        CHECK(*added.Value().value == 20);
        CHECK(map.Find(3) == nullptr);
        CHECK(map.Count() == 2);

        map.ForEach([&](const int32&, int32&) noexcept {
            auto nested = map.FindOrAddWith(4, [&]() noexcept -> Result<int32> {
                ++calls;
                return Result<int32>::Success(40);
            });
            CHECK(nested.Error() == ErrorCode::Busy);
        });
        CHECK(calls == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashMapEraseIfPreservesCollisionsAndValues) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32, CollisionHash> map(allocator);
        for (int32 key = 0; key != 100; ++key) {
            REQUIRE(map.Insert(key, key * 10));
        }
        int32 calls = 0;
        auto removed = map.EraseIf([&](const int32& key, const int32& value) noexcept {
            ++calls;
            CHECK(value == key * 10);
            CHECK(map.Find(key) != nullptr);
            CHECK(map.Insert(200, 1).Error() == ErrorCode::Busy);
            CHECK(map.EraseIf([](const int32&, const int32&) noexcept { return false; }).Error() ==
                  ErrorCode::Busy);
            return key % 2 == 0;
        });
        REQUIRE(removed);
        CHECK(removed.Value() == 50);
        CHECK(calls == 100);
        CHECK(map.Count() == 50);
        for (int32 key = 0; key != 100; ++key) {
            const int32* value = map.Find(key);
            CHECK((value == nullptr) == (key % 2 == 0));
            if (value) {
                CHECK(*value == key * 10);
            }
        }
        auto none = map.EraseIf([](const int32&, const int32&) noexcept { return false; });
        REQUIRE(none);
        CHECK(none.Value() == 0);
        auto all = map.EraseIf([](const int32&, const int32&) noexcept { return true; });
        REQUIRE(all);
        CHECK(all.Value() == 50);
        CHECK(map.IsEmpty());
        REQUIRE(map.Insert(9, 90));
        CHECK(*map.Find(9) == 90);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashSetEraseIfAndSelfSetOperations) {
    SystemAllocator allocator;
    {
        HashSet<int32, CollisionHash> set(allocator);
        for (int32 key = 0; key != 32; ++key) {
            REQUIRE(set.Add(key));
        }
        auto removed = set.EraseIf([&](const int32& key) noexcept {
            CHECK(set.Contains(key));
            CHECK(set.Add(50).Error() == ErrorCode::Busy);
            return key % 3 == 0;
        });
        REQUIRE(removed);
        CHECK(removed.Value() == 11);
        CHECK(set.Count() == 21);
        for (int32 key = 0; key != 32; ++key) {
            CHECK(set.Contains(key) == (key % 3 != 0));
        }

        auto selfIntersect = set.IntersectWith(set);
        REQUIRE(selfIntersect);
        CHECK(selfIntersect.Value() == 0);
        CHECK(set.Count() == 21);
        auto selfDifference = set.DifferenceWith(set);
        REQUIRE(selfDifference);
        CHECK(selfDifference.Value() == 21);
        CHECK(set.IsEmpty());
        REQUIRE(set.Add(91));
        CHECK(set.Contains(91));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashEraseIfAlwaysPassesConstObjectsToPredicates) {
    SystemAllocator allocator;
    {
        HashMap<int32, int32> map(allocator);
        HashSet<int32> set(allocator);
        REQUIRE(map.Insert(1, 10));
        REQUIRE(map.Insert(2, 20));
        REQUIRE(set.Add(1));
        REQUIRE(set.Add(2));
        int32 mapConst = 0;
        int32 mapMutable = 0;
        int32 setConst = 0;
        int32 setMutable = 0;

        // 오버로드가 모두 존재해도 키와 값의 읽기 전용 버전만 호출해야 한다.
        auto mapResult = map.EraseIf(OverloadedMapPredicate{&mapConst, &mapMutable});
        auto setResult = set.EraseIf(OverloadedSetPredicate{&setConst, &setMutable});
        REQUIRE(mapResult && setResult);
        CHECK(mapResult.Value() == 1);
        CHECK(setResult.Value() == 1);
        CHECK(mapConst == 2);
        CHECK(setConst == 2);
        CHECK(mapMutable == 0);
        CHECK(setMutable == 0);
        CHECK(!map.Contains(1));
        CHECK(map.Contains(2));
        CHECK(!set.Contains(1));
        CHECK(set.Contains(2));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashEraseIfDestroysMoveOnlyValuesExactlyOnce) {
    SystemAllocator allocator;
    {
        HashMap<MoveKey, MoveValue, MoveKeyHash, MoveKeyEqual> map(allocator);
        HashSet<MoveKey, MoveKeyHash, MoveKeyEqual> set(allocator);
        for (int32 key = 0; key != 10; ++key) {
            REQUIRE(map.Insert(MoveKey(key), MoveValue(key * 10)));
            REQUIRE(set.Add(MoveKey(key)));
        }
        CHECK(MoveValue::live == 10);
        auto removedMap = map.EraseIf(
            [](const MoveKey& key, const MoveValue&) noexcept { return key.value % 2 == 0; });
        auto removedSet =
            set.EraseIf([](const MoveKey& key) noexcept { return key.value % 2 == 0; });
        REQUIRE(removedMap && removedSet);
        CHECK(removedMap.Value() == 5);
        CHECK(removedSet.Value() == 5);
        CHECK(MoveValue::live == 5);
        for (int32 key = 0; key != 10; ++key) {
            CHECK(map.Contains(MoveKey(key)) == (key % 2 != 0));
            CHECK(set.Contains(MoveKey(key)) == (key % 2 != 0));
        }
        auto rest = map.EraseIf([](const MoveKey&, const MoveValue&) noexcept { return true; });
        REQUIRE(rest);
        CHECK(rest.Value() == 5);
        CHECK(MoveValue::live == 0);
    }
    CHECK(MoveValue::live == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(CoreHashSetOperationsUseOtherSetsStatefulPolicy) {
    SystemAllocator allocator;
    {
        HashSet<int32, ModularHash, ModularEqual> left(allocator, {100}, {100});
        HashSet<int32, ModularHash, ModularEqual> right(allocator, {10}, {10});
        REQUIRE(left.Add(1));
        REQUIRE(left.Add(11));
        REQUIRE(left.Add(2));
        REQUIRE(right.Add(11));

        // 두 집합의 정책 값이 달라도 상대 집합의 Contains 정책으로 소속을 판단한다.
        auto removed = left.IntersectWith(right);
        REQUIRE(removed);
        CHECK(removed.Value() == 1);
        CHECK(left.Count() == 2);
        CHECK(left.Contains(1));
        CHECK(left.Contains(11));
        CHECK(!left.Contains(2));
        auto difference = left.DifferenceWith(right);
        REQUIRE(difference);
        CHECK(difference.Value() == 2);
        CHECK(left.IsEmpty());
        CHECK(right.Count() == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashSetOperationsGuardOtherHasherReentry) {
    SystemAllocator allocator;
    {
        HashReentryState state;
        ReentrantSet left(allocator, ReentrantHash{});
        ReentrantSet right(allocator, ReentrantHash{&state});
        REQUIRE(left.Add(1));
        REQUIRE(left.Add(2));
        REQUIRE(right.Add(2));
        state.target = &right;

        // 상대 집합의 사용자 해시에서 같은 집합을 변경하려 해도 순회가 끝날 때까지 차단한다.
        auto intersection = left.IntersectWith(right);
        REQUIRE(intersection);
        CHECK(intersection.Value() == 1);
        CHECK(state.busyCount > 0);
        CHECK(state.unexpectedCount == 0);
        CHECK(right.Count() == 1);
        state.target = nullptr;
        CHECK(left.Contains(2));
        CHECK(right.Contains(2));
        CHECK(!right.Contains(999));

        REQUIRE(left.Add(1));
        state.busyCount = 0;
        state.target = &right;
        auto difference = left.DifferenceWith(right);
        REQUIRE(difference);
        CHECK(difference.Value() == 1);
        CHECK(state.busyCount > 0);
        CHECK(state.unexpectedCount == 0);
        state.target = nullptr;
        CHECK(left.Contains(1));
        CHECK(!left.Contains(2));
        CHECK(right.Contains(2));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreHashSetOperationsMatchIndependentRandomizedModel) {
    SystemAllocator allocator;
    {
        uint32 state = 0x2b4d6f81u;
        for (int32 round = 0; round != 300; ++round) {
            HashSet<int32, CollisionHash> left(allocator);
            HashSet<int32, CollisionHash> right(allocator);
            bool presentLeft[64]{};
            bool presentRight[64]{};
            for (int32 key = 0; key != 64; ++key) {
                state = state * 1664525u + 1013904223u;
                if ((state >> 30) & 1u) {
                    REQUIRE(left.Add(key));
                    presentLeft[key] = true;
                }
                state = state * 1664525u + 1013904223u;
                if ((state >> 30) & 1u) {
                    REQUIRE(right.Add(key));
                    presentRight[key] = true;
                }
            }
            usize expectedRemoved = 0;
            const bool intersection = (round % 2) == 0;
            for (int32 key = 0; key != 64; ++key) {
                const bool keep = intersection ? presentRight[key] : !presentRight[key];
                if (presentLeft[key] && !keep) {
                    presentLeft[key] = false;
                    ++expectedRemoved;
                }
            }
            auto result = intersection ? left.IntersectWith(right) : left.DifferenceWith(right);
            REQUIRE(result);
            CHECK(result.Value() == expectedRemoved);
            for (int32 key = 0; key != 64; ++key) {
                CHECK(left.Contains(key) == presentLeft[key]);
                CHECK(right.Contains(key) == presentRight[key]);
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(CoreHashSetTryUnionUsesResultPolicyAndGuardsInputs) {
    SystemAllocator allocator;
    {
        HashSet<int32, ModularHash, ModularEqual> left(allocator, {100}, {100});
        HashSet<int32, ModularHash, ModularEqual> right(allocator, {100}, {100});
        REQUIRE(left.Add(1));
        REQUIRE(left.Add(11));
        REQUIRE(left.Add(2));
        REQUIRE(right.Add(11));
        REQUIRE(right.Add(12));
        REQUIRE(right.Add(3));
        int32 clones = 0;

        // 결과 정책은 1과 11, 2와 12를 같은 키로 판정한다.
        auto combined = left.TryUnion(right, allocator, ModularHash{10}, ModularEqual{10},
                                      [&](const int32& key) noexcept -> Result<int32> {
                                          ++clones;
                                          CHECK(left.Add(90).Error() == ErrorCode::Busy);
                                          CHECK(right.Add(90).Error() == ErrorCode::Busy);
                                          CHECK(left.Contains(key) || right.Contains(key));
                                          return Result<int32>::Success(key);
                                      });
        REQUIRE(combined);
        CHECK(clones == 3);
        CHECK(combined.Value().Count() == 3);
        CHECK(combined.Value().Contains(1));
        CHECK(combined.Value().Contains(2));
        CHECK(combined.Value().Contains(3));
        CHECK(left.Count() == 3);
        CHECK(right.Count() == 3);
        CHECK(!left.Contains(90));
        CHECK(!right.Contains(90));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(CoreHashSetTryUnionSelfAndMoveOnlyKeys) {
    SystemAllocator allocator;
    {
        HashSet<MoveKey, MoveKeyHash, MoveKeyEqual> set(allocator);
        REQUIRE(set.Add(MoveKey(1)));
        REQUIRE(set.Add(MoveKey(2)));
        const MoveKey* original = set.Find(MoveKey(1));
        REQUIRE(original != nullptr);
        int32 clones = 0;

        // 자기 자신과의 합집합도 별도 소유 결과를 만들며 중복 키는 다시 복제하지 않는다.
        auto combined = set.TryUnion(set, allocator, MoveKeyHash{}, MoveKeyEqual{},
                                     [&](const MoveKey& key) noexcept -> Result<MoveKey> {
                                         ++clones;
                                         CHECK(set.Add(MoveKey(8)).Error() == ErrorCode::Busy);
                                         return Result<MoveKey>::Success(MoveKey(key.value));
                                     });
        REQUIRE(combined);
        CHECK(clones == 2);
        CHECK(combined.Value().Count() == 2);
        CHECK(combined.Value().Contains(MoveKey(1)));
        CHECK(combined.Value().Contains(MoveKey(2)));
        CHECK(set.Find(MoveKey(1)) == original);
        CHECK(original->value == 1);
        CHECK(set.Count() == 2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(CoreHashSetTryUnionFailuresReleaseTemporaryAndPreserveInputs) {
    SystemAllocator backing;
    test::NFailAllocator outputAllocator(backing);
    {
        HashSet<int32, CollisionHash> left(backing);
        HashSet<int32, CollisionHash> right(backing);
        REQUIRE(left.Add(1));
        REQUIRE(left.Add(2));
        REQUIRE(right.Add(3));
        const int32* leftAddress = left.Find(1);
        const int32* rightAddress = right.Find(3);
        REQUIRE(leftAddress != nullptr && rightAddress != nullptr);
        const auto initialStats = backing.Stats();
        int32 calls = 0;

        auto cloneFailure =
            left.TryUnion(right, outputAllocator, CollisionHash{}, Equal<int32>{},
                          [&](const int32& key) noexcept -> Result<int32> {
                              ++calls;
                              return calls == 2
                                         ? Result<int32>::Failure(ErrorCode::DependencyFailed)
                                         : Result<int32>::Success(key);
                          });
        CHECK(!cloneFailure);
        CHECK(cloneFailure.Error() == ErrorCode::DependencyFailed);
        CHECK(calls == 2);
        CHECK(left.Find(1) == leftAddress);
        CHECK(right.Find(3) == rightAddress);
        CHECK(left.Count() == 2);
        CHECK(right.Count() == 1);
        CHECK(backing.Stats().liveAllocations == initialStats.liveAllocations);
        CHECK(backing.Stats().liveBytes == initialStats.liveBytes);

        // clone이 동일하지 않은 키를 반환하면 성공한 결과로 취급하지 않는다.
        auto wrongClone = left.TryUnion(right, outputAllocator, CollisionHash{}, Equal<int32>{},
                                        [](const int32& key) noexcept -> Result<int32> {
                                            return Result<int32>::Success(key + 100);
                                        });
        CHECK(!wrongClone);
        CHECK(wrongClone.Error() == ErrorCode::InvalidArgument);
        CHECK(backing.Stats().liveAllocations == initialStats.liveAllocations);
        CHECK(backing.Stats().liveBytes == initialStats.liveBytes);

        outputAllocator.FailOnAttempt(1);
        auto allocationFailure = left.TryUnion(
            right, outputAllocator, CollisionHash{}, Equal<int32>{},
            [](const int32& key) noexcept -> Result<int32> { return Result<int32>::Success(key); });
        CHECK(!allocationFailure);
        CHECK(allocationFailure.Error() == ErrorCode::OutOfMemory);
        CHECK(left.Find(1) == leftAddress);
        CHECK(right.Find(3) == rightAddress);
        CHECK(left.Count() == 2);
        CHECK(right.Count() == 1);
        CHECK(backing.Stats().liveAllocations == initialStats.liveAllocations);
        CHECK(backing.Stats().liveBytes == initialStats.liveBytes);
        outputAllocator.DisableFailure();
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreHashSetTryUnionTemporaryCleanupKeepsInputsGuarded) {
    SystemAllocator allocator;
    {
        ReentrantCloneSet left(allocator);
        ReentrantCloneSet right(allocator);
        REQUIRE(left.Add(ReentrantCloneKey(1)));
        REQUIRE(right.Add(ReentrantCloneKey(2)));
        const ReentrantCloneKey* originalLeft = left.Find(ReentrantCloneKey(1));
        const ReentrantCloneKey* originalRight = right.Find(ReentrantCloneKey(2));
        REQUIRE(originalLeft != nullptr && originalRight != nullptr);
        CloneReentryState state{&left, &right};
        int32 calls = 0;

        // 복제 도중 실패하면 임시 결과를 소멸하는 동안에도 두 입력을 보호해야 한다.
        auto failure = left.TryUnion(
            right, allocator, ReentrantCloneHash{}, ReentrantCloneEqual{},
            [&](const ReentrantCloneKey& key) noexcept -> Result<ReentrantCloneKey> {
                ++calls;
                if (calls == 2) {
                    return Result<ReentrantCloneKey>::Failure(ErrorCode::DependencyFailed);
                }
                return Result<ReentrantCloneKey>::Success(
                    ReentrantCloneKey(key.value, &state, true));
            });
        CHECK(!failure);
        CHECK(failure.Error() == ErrorCode::DependencyFailed);
        CHECK(calls == 2);
        CHECK(state.busyCount == 2);
        CHECK(state.unexpectedCount == 0);
        CHECK(left.Count() == 1);
        CHECK(right.Count() == 1);
        CHECK(left.Find(ReentrantCloneKey(1)) == originalLeft);
        CHECK(right.Find(ReentrantCloneKey(2)) == originalRight);
        CHECK(!left.Contains(ReentrantCloneKey(901)));
        CHECK(!right.Contains(ReentrantCloneKey(902)));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}

TEST(CoreHashSetTryUnionEveryAllocationFailureRollsBack) {
    SystemAllocator backing;
    test::NFailAllocator outputAllocator(backing);
    {
        HashSet<int32, CollisionHash> left(backing);
        HashSet<int32, CollisionHash> right(backing);
        for (int32 key = 0; key != 20; ++key) {
            REQUIRE(left.Add(key));
            REQUIRE(right.Add(key + 10));
        }
        const int32* leftAddress = left.Find(1);
        const int32* rightAddress = right.Find(11);
        REQUIRE(leftAddress != nullptr && rightAddress != nullptr);
        const auto baseline = backing.Stats();
        int32 failures = 0;
        bool succeeded = false;

        // 결과 저장소 성장의 각 할당을 차례로 실패시켜 원본과 자원 회수를 확인한다.
        for (uint64 attempt = 1; attempt != 12; ++attempt) {
            outputAllocator.FailOnAttempt(attempt);
            {
                auto result = left.TryUnion(right, outputAllocator, CollisionHash{}, Equal<int32>{},
                                            [](const int32& key) noexcept -> Result<int32> {
                                                return Result<int32>::Success(key);
                                            });
                if (!result) {
                    CHECK(result.Error() == ErrorCode::OutOfMemory);
                    ++failures;
                } else {
                    succeeded = true;
                    CHECK(result.Value().Count() == 30);
                    for (int32 key = 0; key != 30; ++key) {
                        CHECK(result.Value().Contains(key));
                    }
                }
            }
            CHECK(left.Find(1) == leftAddress);
            CHECK(right.Find(11) == rightAddress);
            CHECK(left.Count() == 20);
            CHECK(right.Count() == 20);
            CHECK(backing.Stats().liveAllocations == baseline.liveAllocations);
            CHECK(backing.Stats().liveBytes == baseline.liveBytes);
            if (succeeded) {
                break;
            }
        }
        CHECK(succeeded);
        CHECK(failures >= 2);
        outputAllocator.DisableFailure();
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

namespace rocks::test {

// 자식 프로세스 전용이다. Fatal이 발생하지 않으면 0으로 돌아가 음성 대조를 실패시킨다.
int RunHashOperationsNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;

    if (operation == 0 || operation == 2) {
        HashMap<int32, int32> map(allocator);
        if (!map.Insert(1, 10)) {
            return 0;
        }
        if (operation == 0) {
            auto result = map.EraseIf([&](const int32&, const int32&) noexcept {
                map.Clear();
                return false;
            });
            (void)result;
        } else {
            auto result = map.FindOrAddWith(2, [&]() noexcept -> Result<int32> {
                map.Clear();
                return Result<int32>::Success(20);
            });
            (void)result;
        }
        return 0;
    }

    HashSet<int32> left(allocator);
    HashSet<int32> right(allocator);
    if (!left.Add(1) || !right.Add(2)) {
        return 0;
    }
    if (operation == 1) {
        auto result = left.EraseIf([&](const int32&) noexcept {
            left.Clear();
            return false;
        });
        (void)result;
    } else if (operation == 3) {
        auto result = left.TryUnion(right, allocator, Hash<int32>{}, Equal<int32>{},
                                    [&](const int32& key) noexcept -> Result<int32> {
                                        right.Erase(2);
                                        return Result<int32>::Success(key);
                                    });
        (void)result;
    }
    return 0;
}

} // namespace rocks::test

namespace {

struct FactoryReentryState;

struct FactoryReentryValue {
    int32 value = 0;
    FactoryReentryState* state = nullptr;

    explicit FactoryReentryValue(int32 value = 0, FactoryReentryState* state = nullptr) noexcept
        : value(value), state(state) {
    }
    FactoryReentryValue(const FactoryReentryValue&) = delete;
    FactoryReentryValue& operator=(const FactoryReentryValue&) = delete;
    FactoryReentryValue(FactoryReentryValue&& other) noexcept
        : value(other.value), state(other.state) {
    }
    ~FactoryReentryValue() noexcept;
};

using FactoryReentryMap = HashMap<int32, FactoryReentryValue>;

struct FactoryReentryState {
    FactoryReentryMap* map = nullptr;
    bool armed = false;
    int32 busyCount = 0;
    int32 unexpectedCount = 0;
};

FactoryReentryValue::~FactoryReentryValue() noexcept {
    if (state == nullptr || !state->armed) {
        return;
    }
    auto result = state->map->Insert(999, FactoryReentryValue(999));
    if (!result && result.Error() == ErrorCode::Busy) {
        ++state->busyCount;
    } else {
        ++state->unexpectedCount;
    }
}

class FactoryReentryAllocator final : public Allocator {
public:
    FactoryReentryAllocator(Allocator& backing, FactoryReentryState& state) noexcept
        : backing_(backing), state_(state) {
    }

    bool armNext = false;
    bool failNext = false;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (armNext) {
            armNext = false;
            state_.armed = true;
            if (failNext) {
                failNext = false;
                return Result<void*>::Failure(ErrorCode::OutOfMemory);
            }
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
    FactoryReentryState& state_;
};

} // namespace

TEST(CoreHashMapFactoryTemporaryDestructionKeepsMapGuarded) {
    SystemAllocator backing;
    FactoryReentryState state;
    FactoryReentryAllocator allocator(backing, state);
    {
        FactoryReentryMap map(allocator);
        state.map = &map;
        REQUIRE(map.Reserve(4));
        const usize capacity = map.Capacity();
        for (usize index = 0; index != capacity; ++index) {
            REQUIRE(map.Insert(static_cast<int32>(index),
                               FactoryReentryValue(static_cast<int32>(index))));
        }
        const FactoryReentryValue* old = map.Find(0);
        REQUIRE(old != nullptr);

        // factory 임시는 생성 이후 할당이 실패해도 가드가 살아 있는 동안 소멸한다.
        allocator.armNext = true;
        allocator.failNext = true;
        auto failure = map.FindOrAddWith(100, [&]() noexcept -> Result<FactoryReentryValue> {
            return Result<FactoryReentryValue>::Success(FactoryReentryValue(100, &state));
        });
        CHECK(!failure);
        CHECK(failure.Error() == ErrorCode::OutOfMemory);
        CHECK(state.busyCount >= 1);
        CHECK(state.unexpectedCount == 0);
        CHECK(map.Find(0) == old);
        CHECK(map.Count() == capacity);

        state.armed = false;
        state.busyCount = 0;
        allocator.armNext = true;
        auto success = map.FindOrAddWith(100, [&]() noexcept -> Result<FactoryReentryValue> {
            return Result<FactoryReentryValue>::Success(FactoryReentryValue(100, &state));
        });
        REQUIRE(success);
        CHECK(success.Value().outcome == InsertOutcome::Inserted);
        CHECK(state.busyCount >= 1);
        CHECK(state.unexpectedCount == 0);
        CHECK(!map.Contains(999));
        CHECK(map.Count() == capacity + 1);

        // 삽입된 값은 이후에는 일반적인 컨테이너 수명에 따라 파괴된다.
        state.armed = false;
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}
