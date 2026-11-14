#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// A0-Oa Outer·이름의 GC·색인·할당 실패·재진입
// 시험이다(Docs/Design/ObjectFoundations.ko.md 1.4·1.5절, 9절 Oa). inner → outer 간선, Retire의
// 색인·고리 해제, 회수 조건(firstChild), 이름 색인 rebuild·성장, 잠금 밖 할당 지점의 실패 원자성을
// 본다.
using namespace rocks;

namespace {

constexpr TypeId NodeType{0x4f4947434e4f4445ULL, 1};
uint32 nodesDestroyed = 0;

struct GcNode final : Object {
    ObjectRef<GcNode> link;

    ~GcNode() noexcept {
        ++nodesDestroyed;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(link);
    }
};

Result<TypeRegistrationToken> Register(TypeRegistry& types, bool parallel = false) noexcept {
    auto ops = MakeRuntimeTypeOps<GcNode>();
    ops.parallelTraceSafe = parallel;
    return types.Register({NodeType, 1, {}, {}}, Move(ops));
}

Result<ObjectHandle> NewNode(ObjectRegistry& registry, ObjectHandle outer, StringView name,
                             RootRef<Object>* root = nullptr) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), NodeType, outer, name);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const ObjectHandle handle = made.Value().Handle();
    if (root != nullptr) {
        *root = Move(made.Value());
    }

    return Result<ObjectHandle>::Success(handle);
}

bool Alive(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto state = registry.InspectLifecycle(registry.OwnerAccess(), handle);
    return state && state.Value() == ObjectLifecycle::Alive;
}

bool Reclaimed(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    return registry.InspectLifecycle(registry.OwnerAccess(), handle).Error() ==
           ErrorCode::StaleHandle;
}

bool Finish(ObjectRegistry& registry, uint32 budget = 1,
            WorkerScheduler* workers = nullptr) noexcept {
    for (uint32 iteration = 0; iteration < 100000; ++iteration) {
        if (registry.CollectionPhase() == GcPhase::Idle) {
            return true;
        }

        if (!registry.StepCollection(registry.OwnerAccess(), {budget, 0}, workers)) {
            return false;
        }
    }

    return false;
}

// cutoff를 게시한 첫 slice까지 진행한다(budget 1이면 그 slice는 슬롯 하나만 분류한다).
bool StepToSweeping(ObjectRegistry& registry) noexcept {
    for (uint32 iteration = 0; iteration < 100000; ++iteration) {
        if (registry.CollectionPhase() == GcPhase::Sweeping) {
            return true;
        }

        if (!registry.StepCollection(registry.OwnerAccess(), {1, 0})) {
            return false;
        }
    }

    return false;
}

// 지정한 tag의 할당을 세고, 켜 두면 그 tag의 다음 할당을 실패시킨다(이름 색인 표는 'NAMT').
class TagAllocator final : public Allocator {
public:
    TagAllocator(Allocator& base, uint32 tag) noexcept : base_(base), tag_(tag) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (tag == tag_) {
            ++count_;
            lastBytes_ = bytes;
            if (failNext_) {
                failNext_ = false;
                return Result<void*>::Failure(ErrorCode::OutOfMemory);
            }
        }

        return base_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return base_.Deallocate(address);
    }

    void FailNext() noexcept {
        failNext_ = true;
    }

    uint32 Count() const noexcept {
        return count_;
    }

    usize LastBytes() const noexcept {
        return lastBytes_;
    }

private:
    Allocator& base_;
    uint32 tag_;
    uint32 count_ = 0;
    usize lastBytes_ = 0;
    bool failNext_ = false;
};

constexpr uint32 NameTableTag = 0x4e414d54u;

} // namespace

TEST(ObjectIdentityGc_InnerRootKeepsOuterAliveAcrossCollectors) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types, true));
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({4, 256, 64}));
        ObjectRegistry registry(allocator, types, {0x4f4701, 1});
        const auto access = registry.OwnerAccess();
        // Collect·증분 Step·병렬 trace 모두 inner → outer를 강한 간선으로 본다.
        for (uint32 mode = 0; mode < 3; ++mode) {
            auto outer = NewNode(registry, {}, {});
            REQUIRE(outer);
            RootRef<Object> innerRoot;
            auto inner = NewNode(registry, outer.Value(), "Inner", &innerRoot);
            REQUIRE(inner);
            if (mode == 0) {
                REQUIRE(registry.Collect(access));
            } else {
                REQUIRE(registry.BeginCollection(access));
                REQUIRE(Finish(registry, 1, mode == 2 ? &scheduler : nullptr));
            }

            CHECK(Alive(registry, outer.Value()));
            CHECK(Alive(registry, inner.Value()));
            auto found = registry.FindChild(access, outer.Value(), "inner");
            CHECK(found && found.Value() == inner.Value());
            innerRoot.Reset();
            REQUIRE(registry.Collect(access));
            CHECK(Reclaimed(registry, outer.Value()));
            CHECK(Reclaimed(registry, inner.Value()));
        }

        // 정책 없는 Registry에서 outer만 root면 inner는 회수되고 이름도 빠진다.
        RootRef<Object> outerRoot;
        auto outer = NewNode(registry, {}, {}, &outerRoot);
        REQUIRE(outer);
        auto inner = NewNode(registry, outer.Value(), "Inner");
        REQUIRE(inner);
        CHECK(registry.Stats().namedObjects == 1);
        REQUIRE(registry.Collect(access));
        CHECK(Alive(registry, outer.Value()));
        CHECK(Reclaimed(registry, inner.Value()));
        CHECK(registry.Stats().namedObjects == 0);
        CHECK(registry.FindChild(access, outer.Value(), "Inner").Error() == ErrorCode::NotFound);
        outerRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectIdentityGc_MarkingBirthsKeepOuterAndSweepingRejectsCutoffOuter) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {0x4f4702, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> anchor;
        REQUIRE(NewNode(registry, {}, {}, &anchor));
        // Marking 중: 아무도 붙잡지 않는 white outer 아래에 새 객체를 공개하면 그 trace가 outer를
        // 표식한다.
        auto white = NewNode(registry, {}, {});
        REQUIRE(white);
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(registry.CollectionPhase() == GcPhase::Marking);
        RootRef<Object> bornRoot;
        auto born = NewNode(registry, white.Value(), "Born", &bornRoot);
        REQUIRE(born);
        REQUIRE(Finish(registry));
        CHECK(Alive(registry, white.Value()));
        CHECK(Alive(registry, born.Value()));
        bornRoot.Reset();
        REQUIRE(registry.Collect(access));
        CHECK(Reclaimed(registry, white.Value()));

        // Sweeping 중: cutoff white Outer는 새 Outer가 될 수 없고 조회도 Destroyed다.
        auto doomed = NewNode(registry, {}, {});
        REQUIRE(doomed);
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(StepToSweeping(registry));
        auto rejected = registry.NewObject<Object>(access, NodeType, doomed.Value(), "Late");
        CHECK(!rejected && rejected.Error() == ErrorCode::StaleHandle);
        CHECK(registry.IsNameTaken(access, doomed.Value(), "Late").Error() == ErrorCode::Destroyed);
        CHECK(registry.FindChild(access, doomed.Value(), "Late").Error() == ErrorCode::Destroyed);
        CHECK(registry.DescribeOuter(access, doomed.Value()).Error() == ErrorCode::StaleHandle);
        REQUIRE(Finish(registry));
        CHECK(Reclaimed(registry, doomed.Value()));
        anchor.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// cutoff white 객체는 이름을 점유하지 않는다. Sweeping 중 같은 이름의 새 객체를 바로 만들 수 있다.
TEST(ObjectIdentityGc_CutoffWhiteNamesAreFreedDuringSweeping) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {0x4f4703, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> outerRoot;
        auto outer = NewNode(registry, {}, {}, &outerRoot);
        REQUIRE(outer);
        // white는 outer 바로 뒤 슬롯이라 cutoff slice(슬롯 하나 분류)에서 아직 분류되지 않는다.
        auto white = NewNode(registry, outer.Value(), "Name");
        REQUIRE(white);
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(StepToSweeping(registry));
        auto state = registry.InspectLifecycle(access, white.Value());
        REQUIRE(state && state.Value() == ObjectLifecycle::Unreachable);
        CHECK(registry.FindChild(access, outer.Value(), "Name").Error() == ErrorCode::NotFound);
        auto taken = registry.IsNameTaken(access, outer.Value(), "NAME");
        CHECK(taken && !taken.Value());
        auto summary = registry.DescribeOuter(access, outer.Value());
        CHECK(summary && !summary.Value().hasNamedInners);
        RootRef<Object> replacementRoot;
        auto replacement = NewNode(registry, outer.Value(), "name", &replacementRoot);
        REQUIRE(replacement);
        CHECK(registry.Stats().namedObjects == 1);
        auto found = registry.FindChild(access, outer.Value(), "Name");
        CHECK(found && found.Value() == replacement.Value());
        REQUIRE(Finish(registry));
        CHECK(Reclaimed(registry, white.Value()));
        CHECK(Alive(registry, replacement.Value()));
        found = registry.FindChild(access, outer.Value(), "NAME");
        CHECK(found && found.Value() == replacement.Value());
        CHECK(registry.Stats().namedObjects == 1);
        replacementRoot.Reset();
        outerRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// white 부모가 자식보다 먼저 Retire되면 자식이 고리에서 스스로 떨어질 때까지 부모 슬롯을 회수하지
// 않는다(sweep 중 DrainRetired·rollback 회수가 끼어도).
TEST(ObjectIdentityGc_RetiredParentWaitsForChildrenBeforeReclaim) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {0x4f4704, 1});
        const auto access = registry.OwnerAccess();
        auto parent = NewNode(registry, {}, "Parent");
        REQUIRE(parent);
        auto child = NewNode(registry, parent.Value(), "Child");
        REQUIRE(child);
        auto grandchild = NewNode(registry, child.Value(), "Grandchild");
        REQUIRE(grandchild);
        nodesDestroyed = 0;
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(StepToSweeping(registry));
        // cutoff slice가 첫 슬롯(parent)만 Retire했다.
        auto parentState = registry.InspectLifecycle(access, parent.Value());
        REQUIRE(parentState && parentState.Value() == ObjectLifecycle::Retired);
        auto childState = registry.InspectLifecycle(access, child.Value());
        REQUIRE(childState && childState.Value() == ObjectLifecycle::Unreachable);
        CHECK(registry.FindChild(access, {}, "Parent").Error() == ErrorCode::NotFound);
        // 이름·경로 조회는 Retired(parent)와 cutoff white(child) 모두 Destroyed다.
        String scratch(allocator);
        CHECK(registry.GetName(access, parent.Value(), scratch).Error() == ErrorCode::Destroyed);
        CHECK(registry.GetObjectPath(access, parent.Value(), scratch).Error() ==
              ErrorCode::Destroyed);
        CHECK(registry.GetName(access, child.Value(), scratch).Error() == ErrorCode::Destroyed);
        CHECK(registry.GetObjectPath(access, child.Value(), scratch).Error() ==
              ErrorCode::Destroyed);
        CHECK(registry.GetOuter(access, child.Value()).Error() == ErrorCode::Destroyed);
        CHECK(scratch.ByteCount() == 0);
        auto drained = registry.DrainRetired(access);
        REQUIRE(drained);
        CHECK(drained.Value() == 0);
        parentState = registry.InspectLifecycle(access, parent.Value());
        CHECK(parentState && parentState.Value() == ObjectLifecycle::Retired);
        {
            // rollback도 회수를 시도한다. 롤백한 객체만 회수되고 부모는 남는다.
            ConstructionContext transaction(registry, access);
            REQUIRE(transaction.Create<GcNode>(NodeType, {}, "Temporary"));
        }

        CHECK(nodesDestroyed == 1);
        parentState = registry.InspectLifecycle(access, parent.Value());
        CHECK(parentState && parentState.Value() == ObjectLifecycle::Retired);
        // 한 슬롯씩 더 분류해 child만 Retire된 상태(grandchild가 아직 child에 붙어 있다)를 본다.
        REQUIRE(registry.StepCollection(access, {1, 0}));
        childState = registry.InspectLifecycle(access, child.Value());
        REQUIRE(childState && childState.Value() == ObjectLifecycle::Retired);
        drained = registry.DrainRetired(access);
        REQUIRE(drained);
        CHECK(drained.Value() == 1);
        CHECK(Reclaimed(registry, parent.Value()));
        childState = registry.InspectLifecycle(access, child.Value());
        CHECK(childState && childState.Value() == ObjectLifecycle::Retired);
        REQUIRE(Finish(registry));
        CHECK(Reclaimed(registry, child.Value()));
        CHECK(Reclaimed(registry, grandchild.Value()));
        CHECK(nodesDestroyed == 4);
        CHECK(registry.Stats().namedObjects == 0);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 회수가 이름 기록을 돌려준다(같은 작업을 반복해도 살아 있는 할당이 늘지 않는다).
TEST(ObjectIdentityGc_NameRecordsAreReturnedOnReclaim) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {0x4f4705, 1});
        const auto access = registry.OwnerAccess();
        uint64 live[3]{};
        for (uint32 round = 0; round < 3; ++round) {
            auto outer = NewNode(registry, {}, "\xED\x95\x9C\xEA\xB8\x80");
            REQUIRE(outer);
            auto representative = NewNode(registry, outer.Value(), {});
            REQUIRE(representative);
            for (uint32 index = 0; index < 40; ++index) {
                char name[16];
                snprintf(name, sizeof(name), "N\xC3\xA9%u", index);
                REQUIRE(NewNode(registry, representative.Value(), name));
            }

            CHECK(registry.Stats().namedObjects == 42);
            REQUIRE(registry.Collect(access));
            CHECK(registry.Stats().namedObjects == 0);
            CHECK(registry.Stats().liveObjects == 0);
            live[round] = allocator.Stats().liveAllocations;
        }

        CHECK(live[1] == live[0]);
        CHECK(live[2] == live[0]);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// tombstone이 쌓이면 같은 크기로 다시 만들고(live가 1/4 이하), 표가 자라지 않는다.
TEST(ObjectIdentityGc_NameIndexRebuildsTombstonesWithoutGrowing) {
    SystemAllocator system;
    {
        TagAllocator allocator(system, NameTableTag);
        TypeRegistry types(system);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {0x4f4706, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> keepRoots[2];
        auto kept = NewNode(registry, {}, "Kept", &keepRoots[0]);
        auto other = NewNode(registry, {}, "Other", &keepRoots[1]);
        REQUIRE(kept && other);
        REQUIRE(allocator.Count() == 1);
        const usize tableBytes = allocator.LastBytes();
        uint64 baselineBytes = 0;
        for (uint32 round = 0; round < 100; ++round) {
            char name[16];
            snprintf(name, sizeof(name), "Churn%u", round);
            REQUIRE(NewNode(registry, {}, name));
            REQUIRE(registry.Collect(access));
            if (round == 0) {
                baselineBytes = system.Stats().liveBytes;
            }
        }

        // 표는 처음 크기 그대로이고(같은 크기 rebuild가 여러 번 있었다) 살아 있는 byte도 같다.
        CHECK(allocator.Count() > 2);
        CHECK(allocator.LastBytes() == tableBytes);
        CHECK(system.Stats().liveBytes == baselineBytes);
        auto found = registry.FindChild(access, {}, "KEPT");
        CHECK(found && found.Value() == kept.Value());
        found = registry.FindChild(access, {}, "other");
        CHECK(found && found.Value() == other.Value());
        CHECK(registry.Stats().namedObjects == 2);
        keepRoots[0].Reset();
        keepRoots[1].Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 색인 성장 할당이 실패하면 OutOfMemory이고 색인·슬롯·통계는 그대로다.
TEST(ObjectIdentityGc_NameIndexGrowthFailureLeavesIndexUnchanged) {
    SystemAllocator system;
    {
        TagAllocator allocator(system, NameTableTag);
        TypeRegistry types(system);
        REQUIRE(Register(types));
        ObjectRegistry registry(allocator, types, {0x4f4707, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[9];
        // 첫 표는 16칸이고 적재율 1/2이라 9번째 이름에서 자란다.
        for (uint32 index = 0; index < 8; ++index) {
            char name[16];
            snprintf(name, sizeof(name), "Fill%u", index);
            REQUIRE(NewNode(registry, {}, name, &roots[index]));
        }

        const ObjectStats before = registry.Stats();
        const uint32 tables = allocator.Count();
        allocator.FailNext();
        auto failed = registry.NewObject<Object>(access, NodeType, {}, "Ninth");
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Count() == tables + 1);
        const ObjectStats after = registry.Stats();
        CHECK(after.namedObjects == before.namedObjects);
        CHECK(after.liveObjects == before.liveObjects);
        CHECK(after.reservedObjects == 0);
        for (uint32 index = 0; index < 8; ++index) {
            char name[16];
            snprintf(name, sizeof(name), "fill%u", index);
            auto found = registry.FindChild(access, {}, name);
            CHECK(found && found.Value() == roots[index].Handle());
        }

        auto free = registry.IsNameTaken(access, {}, "Ninth");
        CHECK(free && !free.Value());
        REQUIRE(NewNode(registry, {}, "Ninth", &roots[8]));
        CHECK(registry.Stats().namedObjects == 9);
        for (auto& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// Create의 모든 할당 지점(저널·NFC 판정·비교 키·이름 기록·색인 성장·객체)이 실패해도 슬롯·색인·
// 통계·살아 있는 할당이 그대로다.
TEST(ObjectIdentityGc_CreateAllocationFailureSweepLeavesNoTrace) {
    SystemAllocator system;
    {
        rocks::test::NFailAllocator fault(system);
        TypeRegistry types(system);
        REQUIRE(Register(types));
        ObjectRegistry registry(fault, types, {0x4f4708, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> outerRoot;
        auto outer = NewNode(registry, {}, {}, &outerRoot);
        REQUIRE(outer);
        // 첫 색인 표는 16칸이고 적재율 1/2이라 9번째 이름이 표를 키운다.
        RootRef<Object> fill[9];
        for (uint32 index = 0; index < 8; ++index) {
            char name[16];
            snprintf(name, sizeof(name), "Fill%u", index);
            REQUIRE(NewNode(registry, outer.Value(), name, &fill[index]));
        }

        const ObjectStats before = registry.Stats();
        const uint64 liveBefore = system.Stats().liveAllocations;
        // q + U+0307은 NFC 빠른 검사가 애매해 정규화로 판정하고, 비ASCII라 비교 키도 할당한다.
        const StringView name("q\xCC\x87x");
        uint32 failures = 0;
        bool created = false;
        for (uint64 attempt = 1; attempt < 128 && !created; ++attempt) {
            {
                ConstructionContext transaction(registry, access);
                fault.FailOnAttempt(attempt);
                auto made = transaction.Create<GcNode>(NodeType, outer.Value(), name);
                fault.DisableFailure();
                if (made) {
                    const ObjectHandle roots[] = {made.Value()};
                    REQUIRE(transaction.Commit(roots, Span<RootRef<Object>>(&fill[8], 1)));
                    created = true;
                } else {
                    CHECK(made.Error() == ErrorCode::OutOfMemory);
                    ++failures;
                }
            }

            if (!created) {
                const ObjectStats after = registry.Stats();
                CHECK(after.namedObjects == before.namedObjects);
                CHECK(after.liveObjects == before.liveObjects);
                CHECK(after.reservedObjects == 0);
                CHECK(system.Stats().liveAllocations == liveBefore);
                auto free = registry.IsNameTaken(access, outer.Value(), name);
                CHECK(free && !free.Value());
            }
        }

        REQUIRE(created);
        // 저널·NFC 판정·비교 키(NFD·NFC 조각)·이름 기록·색인 표·객체 저장소.
        CHECK(failures >= 6);
        CHECK(registry.Stats().namedObjects == before.namedObjects + 1);
        auto found = registry.FindChild(access, outer.Value(), "Q\xCC\x87X");
        CHECK(found && found.Value() == fill[8].Handle());
        for (uint32 index = 0; index < 8; ++index) {
            char lower[16];
            snprintf(lower, sizeof(lower), "fill%u", index);
            auto kept = registry.FindChild(access, outer.Value(), lower);
            CHECK(kept && kept.Value() == fill[index].Handle());
        }

        for (auto& root : fill) {
            root.Reset();
        }

        outerRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 생성 저널은 가득 찼을 때만 두 배로 늘린다(생성마다 전체를 다시 할당하지 않는다). 성장 할당이
// 실패하면 그 Create는 OutOfMemory이고 객체를 만들지 않으며 이미 만든 객체는 롤백된다.
TEST(ObjectIdentityGc_ConstructionJournalGrowsGeometricallyAndAtomically) {
    SystemAllocator system;
    {
        rocks::test::NFailAllocator fault(system);
        TypeRegistry types(system);
        REQUIRE(Register(types));
        ObjectRegistry registry(fault, types, {0x4f4709, 1});
        const auto access = registry.OwnerAccess();
        // chunk를 먼저 만들어 이후 생성의 할당이 저널 성장과 객체 저장소뿐이게 한다.
        RootRef<Object> warm;
        REQUIRE(NewNode(registry, {}, {}, &warm));
        {
            // 저널 용량 1 → 2 → 4 → 8 → 16: 1·2·3·5·9번째 생성만 저널을 늘린다.
            ConstructionContext transaction(registry, access);
            const uint64 expected[] = {2, 2, 2, 1, 2, 1, 1, 1, 2};
            for (uint64 allocations : expected) {
                fault.DisableFailure();
                REQUIRE(transaction.Create<GcNode>(NodeType));
                CHECK(fault.Attempts() == allocations);
            }
        }

        const ObjectStats before = registry.Stats();
        const uint64 liveBefore = system.Stats().liveAllocations;
        {
            ConstructionContext transaction(registry, access);
            for (uint32 index = 0; index < 4; ++index) {
                REQUIRE(transaction.Create<GcNode>(NodeType));
            }

            // 5번째 생성의 첫 할당이 저널 성장(4 → 8)이다.
            fault.FailOnAttempt(1);
            auto failed = transaction.Create<GcNode>(NodeType);
            fault.DisableFailure();
            CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
            CHECK(registry.Stats().reservedObjects == 4);
            CHECK(transaction.Create<GcNode>(NodeType).Error() == ErrorCode::OutOfMemory);
        }

        CHECK(registry.Stats().reservedObjects == 0);
        CHECK(registry.Stats().liveObjects == before.liveObjects);
        CHECK(system.Stats().liveAllocations == liveBefore);
        warm.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(system.Stats().liveAllocations == 0);
}
