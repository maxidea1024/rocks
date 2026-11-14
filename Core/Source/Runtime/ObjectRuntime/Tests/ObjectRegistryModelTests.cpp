#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <ObjectRuntime/Private/ObjectRegistryAccess.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// ObjectRegistry 모델 대조 시험이다. 고정 seed의 무작위 연산(생성·트랜잭션·root 해제·파괴 요청·
// 전체·증분 GC·DrainRetired) 뒤마다 이름 색인·자식 고리·객체 경로·CollectInners·DescribeOuter·
// 통계를 독립 모델과 대조하고, 전체 Collect 뒤 생존 집합이 rooted 객체의 Outer 닫힘과 같은지, 끝에
// 할당 누수가 없는지 본다. 이어서 저장소 정비(chunk 포인터 표·빈 슬롯 목록·회수 대상 목록)의 성질을
// 시간이 아니라 잠금 안 탐색 계수로 본다 — 생성·회수 한 번이 보는 슬롯 수가 저장소 크기와 무관하다.
using namespace rocks;

namespace {

constexpr TypeId ModelNodeType{0x52474d4f44454c31ULL, 1};
constexpr TypeId LeaseHolderType{0x52474d4f44454c32ULL, 1};

struct ModelNode final : Object {};

// 소멸하면서 쥔 BackendLease를 놓는다(회수 도중 다른 Retired 슬롯의 보유 계수가 반환된다).
struct LeaseHolder final : Object {
    BackendLease lease;
};

// 이름 풀이다. 같은 행의 변형은 대소문자·정규화를 무시하면 같은 이름이다.
const char* const Variants[6][3] = {
    {"a", "A", "a"}, {"bb", "BB", "bB"}, {"c", "C", "c"}, {"d\xC3\xA9", "D\xC3\x89", "d\xC3\x89"},
    {"e", "E", "e"}, {"f", "F", "f"},
};

constexpr uint32 MaxModelObjects = 4096;

// 모델이 보는 객체 상태다. Reserved·Gone 밖은 InspectLifecycle로 읽는다.
enum class Phase : uint8 {
    Reserved,
    Alive,
    DestroyRequested,
    Cutoff,
    Retired,
    Gone,
    Other
};

struct ModelObject {
    ObjectHandle handle;
    int32 outer = -1;
    int32 key = -1; // -1이면 빈 이름(Outer가 있으면 대표 자식)
    int32 variant = 0;
    bool reclaimed = false;
    bool committed = false;
};

bool Occupies(Phase phase) noexcept {
    return phase == Phase::Reserved || phase == Phase::Alive || phase == Phase::DestroyRequested;
}

bool Published(Phase phase) noexcept {
    return phase == Phase::Alive || phase == Phase::DestroyRequested;
}

usize Length(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

// NFC UTF-8 byte 순(빈 이름이 먼저)이다 — CollectInners의 같은 Outer 안 순서.
bool NameBefore(const char* left, const char* right) noexcept {
    for (usize index = 0;; ++index) {
        const auto l = static_cast<unsigned char>(left[index]);
        const auto r = static_cast<unsigned char>(right[index]);
        if (l != r) {
            return l < r;
        }

        if (l == 0) {
            return false;
        }
    }
}

#define MODEL_EXPECT(condition, ...)                                                               \
    Expect((condition), #condition, __LINE__ __VA_OPT__(, ) __VA_ARGS__)

// 한 seed의 Registry와 독립 모델이다. 첫 불일치를 시험 실패로 기록하고 그 seed를 멈춘다.
class RegistryModel {
public:
    RegistryModel(Allocator& allocator, ObjectRegistry& registry, uint64 seed) noexcept
        : allocator_(allocator), registry_(registry), access_(registry.OwnerAccess()), seed_(seed),
          rng_(seed * 0x9e3779b97f4a7c15ULL + 1), objects_(allocator), roots_(allocator),
          phases_(allocator) {
    }

    bool Prepare() noexcept {
        if (!objects_.TryReserve(MaxModelObjects) || !roots_.TryReserve(MaxModelObjects) ||
            !phases_.TryReserve(MaxModelObjects)) {
            return false;
        }

        return true;
    }

    void Run(uint32 operations) noexcept {
        for (operation_ = 0; operation_ < operations && !failed_; ++operation_) {
            Refresh();
            Snapshot();
            const uint32 kind = Below(100);
            const GcPhase phase = registry_.CollectionPhase();
            if (kind < 35) {
                DoCreate();
            } else if (kind < 45) {
                DoTransaction(false);
            } else if (kind < 60) {
                if (Count() > 0) {
                    roots_[Below(Count())].Reset();
                }
            } else if (kind < 66) {
                const int32 picked = PickPublished();
                if (picked >= 0) {
                    (void)registry_.RequestDestroy(access_, objects_[picked].handle);
                }
            } else if (kind < 72) {
                if (phase == GcPhase::Idle) {
                    MODEL_EXPECT(registry_.Collect(access_).IsOk());
                    VerifyExactAfterCollect();
                }
            } else if (kind < 80) {
                if (phase == GcPhase::Idle) {
                    MODEL_EXPECT(registry_.BeginCollection(access_).IsOk());
                }
            } else if (kind < 95) {
                if (phase != GcPhase::Idle) {
                    const uint32 budget = 1 + Below(4);
                    MODEL_EXPECT(registry_.StepCollection(access_, {budget, 0}).IsOk());
                }
            } else {
                MODEL_EXPECT(registry_.DrainRetired(access_).IsOk());
            }

            Verify(false);
        }
    }

    // 진행 중 cycle을 마치고 전체 Collect로 정확한 생존을 본 뒤 root를 모두 놓는다(Shutdown 전).
    void Finish() noexcept {
        for (uint32 guard = 0; guard < 1000000 && registry_.CollectionPhase() != GcPhase::Idle;
             ++guard) {
            if (!registry_.StepCollection(access_, {0xffffffffu, 0})) {
                break;
            }
        }

        MODEL_EXPECT(registry_.CollectionPhase() == GcPhase::Idle);
        MODEL_EXPECT(registry_.Collect(access_).IsOk());
        VerifyExactAfterCollect();
        Verify(false);
        for (RootRef<Object>& root : roots_) {
            root.Reset();
        }
    }

    bool Failed() const noexcept {
        return failed_;
    }

private:
    bool Expect(bool ok, const char* what, int line, int64 first = -1, int64 second = -1) noexcept {
        if (ok || failed_) {
            return ok;
        }

        failed_ = true;
        snprintf(message_, sizeof(message_), "ObjectRegistryModel seed %llu op %u: %s [%lld, %lld]",
                 static_cast<unsigned long long>(seed_), operation_, what,
                 static_cast<long long>(first), static_cast<long long>(second));
        test::RecordFailure(message_, __FILE__, line);
        return false;
    }

    uint32 Next() noexcept {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return static_cast<uint32>(rng_ >> 11);
    }

    uint32 Below(uint32 bound) noexcept {
        return Next() % bound;
    }

    uint32 Count() const noexcept {
        return static_cast<uint32>(objects_.Size());
    }

    const char* NameOf(int32 index) const noexcept {
        const ModelObject& object = objects_[index];
        return object.key < 0 ? "" : Variants[object.key][object.variant];
    }

    ObjectHandle OuterHandle(int32 outer) const noexcept {
        return outer < 0 ? ObjectHandle{} : objects_[outer].handle;
    }

    Phase PhaseOf(int32 index) const noexcept {
        const ModelObject& object = objects_[index];
        if (object.reclaimed) {
            return Phase::Gone;
        }

        if (!object.committed) {
            return Phase::Reserved;
        }

        auto state = registry_.InspectLifecycle(access_, object.handle);
        if (!state) {
            return state.Error() == ErrorCode::StaleHandle ? Phase::Gone : Phase::Other;
        }

        switch (state.Value()) {
        case ObjectLifecycle::Alive:
            return Phase::Alive;
        case ObjectLifecycle::DestroyRequested:
            return Phase::DestroyRequested;
        case ObjectLifecycle::Unreachable:
            return Phase::Cutoff;
        case ObjectLifecycle::Retired:
            return Phase::Retired;
        default:
            return Phase::Other;
        }
    }

    // 회수된 객체를 모델에 반영한다. 회수된 객체는 root가 없어야 한다.
    void Refresh() noexcept {
        for (uint32 index = 0; index < Count(); ++index) {
            ModelObject& object = objects_[index];
            if (!object.reclaimed && object.committed &&
                PhaseOf(static_cast<int32>(index)) == Phase::Gone) {
                object.reclaimed = true;
                MODEL_EXPECT(!roots_[index].IsValid(), index);
            }
        }
    }

    void Snapshot() noexcept {
        for (uint32 index = 0; index < Count(); ++index) {
            phases_[index] = PhaseOf(static_cast<int32>(index));
        }
    }

    // index의 객체 경로를 buffer[0, length)에 쓴다(Outer 경로 + '.' + 이름, 빈 이름은 건너뛴다).
    usize PathOf(int32 index, char* buffer) const noexcept {
        if (objects_[index].outer < 0) {
            return 0;
        }

        usize length = PathOf(objects_[index].outer, buffer);
        const char* name = NameOf(index);
        const usize bytes = Length(name);
        if (length != 0 && bytes != 0) {
            buffer[length++] = '.';
        }

        for (usize at = 0; at < bytes; ++at) {
            buffer[length++] = name[at];
        }

        return length;
    }

    int32 Depth(int32 index) const noexcept {
        int32 depth = 0;
        for (int32 at = objects_[index].outer; at >= 0; at = objects_[at].outer) {
            ++depth;
        }

        return depth;
    }

    // (outer, key)를 점유(Reserved 포함)하거나 공개한 객체다. Outer 없는 빈 이름은 색인에 없다.
    int32 Occupant(int32 outer, int32 key, bool publishedOnly) const noexcept {
        if (outer < 0 && key < 0) {
            return -1;
        }

        for (uint32 index = 0; index < Count(); ++index) {
            const ModelObject& object = objects_[index];
            if (object.outer != outer || object.key != key) {
                continue;
            }

            const Phase phase = phases_[index];
            if (publishedOnly ? Published(phase) : Occupies(phase)) {
                return static_cast<int32>(index);
            }
        }

        return -1;
    }

    // outer의 CollectInners 대상(Alive, cutoff 아님) 직계를 이름 순으로 모은다.
    void AliveChildren(int32 outer, Array<int32>& out) const noexcept {
        out.Clear();
        for (uint32 index = 0; index < Count(); ++index) {
            if (objects_[index].outer == outer && phases_[index] == Phase::Alive) {
                if (!out.TryPushBack(static_cast<int32>(index))) {
                    return;
                }
            }
        }

        for (usize at = 1; at < out.Size(); ++at) {
            for (usize back = at; back > 0 && NameBefore(NameOf(out[back]), NameOf(out[back - 1]));
                 --back) {
                const int32 swapped = out[back];
                out[back] = out[back - 1];
                out[back - 1] = swapped;
            }
        }
    }

    // DepthFirst 기대값: 각 Inner 바로 뒤에 그 부분 트리(모으지 않는 객체의 부분 트리는 뺀다).
    void AppendDepthFirst(int32 outer, Array<ObjectHandle>& out) noexcept {
        Array<int32> children(allocator_);
        AliveChildren(outer, children);
        for (const int32 child : children) {
            if (!out.TryPushBack(objects_[child].handle)) {
                return;
            }

            AppendDepthFirst(child, out);
        }
    }

    ErrorCode ExpectedCreate(int32 outer, int32 key) const noexcept {
        if (outer >= 0) {
            const Phase phase = phases_[outer];
            if (!(phase == Phase::Alive || phase == Phase::Reserved)) {
                return ErrorCode::StaleHandle;
            }

            bool hasRepresentative = false;
            bool hasNamed = false;
            for (uint32 index = 0; index < Count(); ++index) {
                if (objects_[index].outer == outer && Occupies(phases_[index])) {
                    (objects_[index].key < 0 ? hasRepresentative : hasNamed) = true;
                }
            }

            if (key < 0) {
                if (objects_[outer].outer >= 0 || hasNamed) {
                    return ErrorCode::InvalidArgument;
                }
            } else if (hasRepresentative) {
                return ErrorCode::InvalidArgument;
            }
        }

        return Occupant(outer, key, false) >= 0 ? ErrorCode::AlreadyExists : ErrorCode::None;
    }

    int32 PickPublished() noexcept {
        for (uint32 tries = 0; tries < 16 && Count() > 0; ++tries) {
            const auto index = static_cast<int32>(Below(Count()));
            if (Published(phases_[index]) || phases_[index] == Phase::Cutoff) {
                return index;
            }
        }

        return -1;
    }

    int32 PickKey() noexcept {
        const auto key = static_cast<int32>(Below(8)) - 2; // -2·-1은 빈 이름
        return key < 0 ? -1 : key;
    }

    int32 Append(ObjectHandle handle, int32 outer, int32 key, int32 variant,
                 bool committed) noexcept {
        ModelObject object;
        object.handle = handle;
        object.outer = outer;
        object.key = key;
        object.variant = variant;
        object.committed = committed;
        if (!objects_.TryPushBack(object) || !roots_.TryEmplaceBack() ||
            !phases_.TryPushBack(committed ? Phase::Alive : Phase::Reserved)) {
            MODEL_EXPECT(false);
            return -1;
        }

        return static_cast<int32>(Count() - 1);
    }

    void DoCreate() noexcept {
        if (Count() >= MaxModelObjects) {
            return;
        }

        int32 outer = -1;
        if (Below(3) != 0) {
            outer = PickPublished();
            if (outer >= 0 && Depth(outer) > 4) {
                outer = -1;
            }
        }

        const int32 key = PickKey();
        const auto variant = static_cast<int32>(Below(3));
        const ErrorCode expected = ExpectedCreate(outer, key);
        auto made = registry_.NewObject<Object>(access_, ModelNodeType, OuterHandle(outer),
                                                key < 0 ? "" : Variants[key][variant]);
        const ErrorCode actual = made ? ErrorCode::None : made.Error();
        MODEL_EXPECT(actual == expected, static_cast<int64>(actual), static_cast<int64>(expected));
        if (made) {
            const int32 index = Append(made.Value().Handle(), outer, key, variant, true);
            if (index >= 0 && Below(3) != 0) {
                roots_[index] = Move(made.Value());
            }
        }
    }

    void DoTransaction(bool nested) noexcept {
        ConstructionContext transaction(registry_, access_);
        const auto first = static_cast<int32>(Count());
        ObjectHandle staged[8];
        int32 stagedCount = 0;
        bool dead = false;
        const auto planned = 1 + static_cast<int32>(Below(5));
        for (int32 step = 0; step < planned && Count() < MaxModelObjects; ++step) {
            int32 outer = -1;
            const uint32 choice = Below(4);
            if (choice == 0 && stagedCount > 0) {
                outer = first + static_cast<int32>(Below(static_cast<uint32>(stagedCount)));
            } else if (choice != 3) {
                outer = PickPublished();
            }

            if (outer >= 0 && Depth(outer) > 4) {
                outer = -1;
            }

            const int32 key = PickKey();
            const auto variant = static_cast<int32>(Below(3));
            const ErrorCode expected = ExpectedCreate(outer, key);
            const ObjectHandle outerHandle = OuterHandle(outer);
            auto made = transaction.Create<ModelNode>(ModelNodeType, outerHandle,
                                                      key < 0 ? "" : Variants[key][variant]);
            const ErrorCode actual = made ? ErrorCode::None : made.Error();
            MODEL_EXPECT(actual == expected, static_cast<int64>(actual),
                         static_cast<int64>(expected));
            if (!made) {
                dead = true;
                break;
            }

            if (Append(made.Value(), outer, key, variant, false) < 0) {
                return;
            }

            staged[stagedCount++] = made.Value();
            // 공개 전 객체는 FindChild에 보이지 않고 IsNameTaken에는 보인다.
            if (outer >= 0 || key >= 0) {
                auto found =
                    registry_.FindChild(access_, outerHandle, key < 0 ? "" : Variants[key][0]);
                const ErrorCode want = outer >= 0 && !objects_[outer].committed
                                           ? ErrorCode::StaleHandle
                                           : ErrorCode::NotFound;
                MODEL_EXPECT(!found && found.Error() == want);
                auto taken =
                    registry_.IsNameTaken(access_, outerHandle, key < 0 ? "" : Variants[key][1]);
                MODEL_EXPECT(taken && taken.Value());
            }
        }

        if (!nested && !dead && Below(4) == 0) {
            // 동시에 열린 두 번째 트랜잭션(점유 규칙)
            DoTransaction(true);
        }

        Verify(true);
        if (!dead && Below(3) != 0) {
            RootRef<Object> held[8];
            ObjectHandle chosen[8];
            int32 chosenIndex[8];
            usize selected = 0;
            for (int32 at = 0; at < stagedCount; ++at) {
                if (Below(2) == 0) {
                    chosen[selected] = staged[at];
                    chosenIndex[selected] = first + at;
                    ++selected;
                }
            }

            auto committed = transaction.Commit(Span<const ObjectHandle>(chosen, selected),
                                                Span<RootRef<Object>>(held, selected));
            MODEL_EXPECT(committed.IsOk());
            if (committed) {
                for (int32 at = 0; at < stagedCount; ++at) {
                    objects_[first + at].committed = true;
                }

                for (usize at = 0; at < selected; ++at) {
                    if (!roots_[chosenIndex[at]].IsValid()) {
                        roots_[chosenIndex[at]] = Move(held[at]);
                    }
                }

                return;
            }
        }

        transaction.Rollback();
        for (uint32 index = static_cast<uint32>(first); index < Count(); ++index) {
            ModelObject& object = objects_[index];
            if (!object.committed && !object.reclaimed) {
                auto state = registry_.InspectLifecycle(access_, object.handle);
                MODEL_EXPECT(!state && state.Error() == ErrorCode::StaleHandle, index);
                object.reclaimed = true;
            }
        }
    }

    void Verify(bool inTransaction) noexcept {
        Refresh();
        Snapshot();
        for (uint32 index = 0; index < Count() && !failed_; ++index) {
            const auto self = static_cast<int32>(index);
            const ModelObject& object = objects_[index];
            const Phase phase = phases_[index];
            if (phase == Phase::Gone || phase == Phase::Reserved) {
                continue;
            }

            MODEL_EXPECT(phase != Phase::Other, index);
            String name(allocator_);
            auto got = registry_.GetName(access_, object.handle, name);
            if (!Published(phase)) {
                MODEL_EXPECT(!got && got.Error() == ErrorCode::Destroyed, index,
                             static_cast<int64>(phase));
                continue;
            }

            MODEL_EXPECT(got && name.View() == StringView(NameOf(self)), index);
            char expected[256];
            const usize length = PathOf(self, expected);
            String path(allocator_);
            auto pathGot = registry_.GetObjectPath(access_, object.handle, path);
            MODEL_EXPECT(pathGot && path.View() == StringView(expected, length), index);
            auto outer = registry_.GetOuter(access_, object.handle);
            MODEL_EXPECT(outer && outer.Value() == OuterHandle(object.outer), index);
            if (object.outer >= 0 || object.key >= 0) {
                const char* query = object.key < 0 ? "" : Variants[object.key][Below(3)];
                auto found = registry_.FindChild(access_, OuterHandle(object.outer), query);
                MODEL_EXPECT(found && found.Value() == object.handle, index);
                auto taken = registry_.IsNameTaken(access_, OuterHandle(object.outer), query);
                MODEL_EXPECT(taken && taken.Value(), index);
            }
        }

        // 공개된 객체마다(그리고 Outer 없는 이름 공간) 이름 풀 전체를 조회한다.
        Array<int32> children(allocator_);
        Array<ObjectHandle> want(allocator_);
        for (int32 outer = -1; outer < static_cast<int32>(Count()) && !failed_; ++outer) {
            if (outer >= 0 && !Published(phases_[outer])) {
                continue;
            }

            const ObjectHandle outerHandle = OuterHandle(outer);
            for (int32 key = outer < 0 ? 0 : -1; key < 6; ++key) {
                const char* query = key < 0 ? "" : Variants[key][Below(3)];
                auto found = registry_.FindChild(access_, outerHandle, query);
                const int32 published = Occupant(outer, key, true);
                if (published >= 0) {
                    MODEL_EXPECT(found && found.Value() == objects_[published].handle, outer, key);
                } else {
                    MODEL_EXPECT(!found && found.Error() == ErrorCode::NotFound, outer, key);
                }

                auto taken = registry_.IsNameTaken(access_, outerHandle, query);
                MODEL_EXPECT(taken && taken.Value() == (Occupant(outer, key, false) >= 0), outer,
                             key);
            }

            if (outer < 0) {
                continue;
            }

            // CollectInners: Direct는 Alive 직계의 이름 순, DepthFirst는 전위(부분 트리째 이름 순).
            AliveChildren(outer, children);
            Array<ObjectHandle> inners(allocator_);
            auto direct = registry_.CollectInners(access_, outerHandle, InnerOrder::Direct, inners);
            MODEL_EXPECT(direct.IsOk(), outer);
            bool same = direct && inners.Size() == children.Size();
            for (usize at = 0; same && at < children.Size(); ++at) {
                same = inners[at] == objects_[children[at]].handle;
            }

            MODEL_EXPECT(same, outer, static_cast<int64>(inners.Size()));
            want.Clear();
            AppendDepthFirst(outer, want);
            Array<ObjectHandle> deep(allocator_);
            auto depthFirst =
                registry_.CollectInners(access_, outerHandle, InnerOrder::DepthFirst, deep);
            MODEL_EXPECT(depthFirst.IsOk(), outer);
            same = depthFirst && deep.Size() == want.Size();
            for (usize at = 0; same && at < want.Size(); ++at) {
                same = deep[at] == want[at];
            }

            MODEL_EXPECT(same, outer, static_cast<int64>(deep.Size()));
            if (phases_[outer] == Phase::Alive) {
                auto summary = registry_.DescribeOuter(access_, outerHandle);
                MODEL_EXPECT(summary.IsOk(), outer);
                if (summary) {
                    bool representative = false;
                    bool named = false;
                    for (uint32 index = 0; index < Count(); ++index) {
                        if (objects_[index].outer == outer && Occupies(phases_[index])) {
                            (objects_[index].key < 0 ? representative : named) = true;
                        }
                    }

                    char path[256];
                    const OuterSummary& value = summary.Value();
                    MODEL_EXPECT(value.hasRepresentative == representative, outer);
                    MODEL_EXPECT(value.hasNamedInners == named, outer);
                    MODEL_EXPECT(value.isOutermost == (objects_[outer].outer < 0), outer);
                    MODEL_EXPECT(value.pathBytes == PathOf(outer, path), outer);
                }
            }
        }

        if (inTransaction || registry_.CollectionPhase() != GcPhase::Idle || failed_) {
            return;
        }

        // Idle에서는 cutoff·Retired가 남지 않고(BackendLease가 없다) 통계가 모델과 같다.
        ObjectStats expected;
        for (uint32 index = 0; index < Count(); ++index) {
            const ModelObject& object = objects_[index];
            const Phase phase = phases_[index];
            MODEL_EXPECT(phase != Phase::Retired && phase != Phase::Cutoff, index,
                         static_cast<int64>(phase));
            if ((object.outer >= 0 || object.key >= 0) && Published(phase)) {
                ++expected.namedObjects;
            }

            expected.liveObjects += phase == Phase::Alive ? 1u : 0u;
            expected.destroyRequested += phase == Phase::DestroyRequested ? 1u : 0u;
            expected.roots += roots_[index].IsValid() ? 1u : 0u;
        }

        const ObjectStats stats = registry_.Stats();
        MODEL_EXPECT(stats.namedObjects == expected.namedObjects, stats.namedObjects,
                     expected.namedObjects);
        MODEL_EXPECT(stats.liveObjects == expected.liveObjects, stats.liveObjects,
                     expected.liveObjects);
        MODEL_EXPECT(stats.destroyRequested == expected.destroyRequested, stats.destroyRequested,
                     expected.destroyRequested);
        MODEL_EXPECT(stats.roots == expected.roots, stats.roots, expected.roots);
        MODEL_EXPECT(stats.reservedObjects == 0, stats.reservedObjects);
        MODEL_EXPECT(stats.retiredObjects == 0, stats.retiredObjects);
        MODEL_EXPECT(stats.borrows == 0 && stats.backendHolds == 0 && stats.encodePins == 0);
    }

    // 전체 Collect 뒤 생존 집합은 rooted 객체와 그 Outer 사슬(inner → outer 강한 간선)이다.
    void VerifyExactAfterCollect() noexcept {
        Array<bool> keep(allocator_);
        if (!keep.TryResize(Count())) {
            MODEL_EXPECT(false);
            return;
        }

        for (bool& kept : keep) {
            kept = false;
        }

        for (uint32 index = 0; index < Count(); ++index) {
            if (roots_[index].IsValid()) {
                for (auto at = static_cast<int32>(index); at >= 0 && !keep[at];
                     at = objects_[at].outer) {
                    keep[at] = true;
                }
            }
        }

        Refresh();
        for (uint32 index = 0; index < Count(); ++index) {
            if (!objects_[index].committed) {
                continue;
            }

            const Phase phase = PhaseOf(static_cast<int32>(index));
            if (keep[index]) {
                MODEL_EXPECT(Published(phase), index, static_cast<int64>(phase));
            } else {
                MODEL_EXPECT(phase == Phase::Gone, index, static_cast<int64>(phase));
            }
        }
    }

    Allocator& allocator_;
    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    uint64 seed_;
    uint64 rng_;
    Array<ModelObject> objects_;
    Array<RootRef<Object>> roots_;
    Array<Phase> phases_;
    uint32 operation_ = 0;
    bool failed_ = false;
    char message_[256] = {};
};

// seed 하나를 새 Registry로 돌리고 종료·누수까지 본다.
void RunModelSeed(uint64 seed, uint32 operations) noexcept {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(types.Register({ModelNodeType, 1, {}, {}}, MakeRuntimeTypeOps<ModelNode>()));
        ObjectRegistry registry(allocator, types, {0x52474d4f, seed});
        {
            RegistryModel model(allocator, registry, seed);
            REQUIRE(model.Prepare());
            model.Run(operations);
            model.Finish();
        }

        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

Result<TypeRegistrationToken> RegisterNode(TypeRegistry& types) noexcept {
    return types.Register({ModelNodeType, 1, {}, {}}, MakeRuntimeTypeOps<ModelNode>());
}

Result<TypeRegistrationToken> RegisterLeaseHolder(TypeRegistry& types) noexcept {
    return types.Register({LeaseHolderType, 1, {}, {}}, MakeRuntimeTypeOps<LeaseHolder>());
}

Result<ObjectHandle> NewNode(ObjectRegistry& registry, RootRef<Object>* root = nullptr) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), ModelNodeType);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const ObjectHandle handle = made.Value().Handle();
    if (root != nullptr) {
        *root = Move(made.Value());
    }

    return Result<ObjectHandle>::Success(handle);
}

bool IsReclaimed(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    return registry.InspectLifecycle(registry.OwnerAccess(), handle).Error() ==
           ErrorCode::StaleHandle;
}

detail::RegistryWorkCounters Counters(const ObjectRegistry& registry) noexcept {
    return detail::ObjectRegistryAccess::WorkCounters(registry);
}

} // namespace

// 고정 seed 6개마다 무작위 연산 500개를 모델과 매 연산 대조한다. runner seed(--seed)와 무관하게
// 늘 같은 연산열이라 실패가 그대로 재현된다.
TEST(ObjectRegistryModel_RandomOperationsMatchModel) {
    for (uint64 seed = 1; seed <= 6; ++seed) {
        RunModelSeed(seed, 500);
    }
}

// 생성은 빈 슬롯 하나만 보고(옛 chunk에 흩어진 빈 슬롯도), 회수는 회수 대상만 한 번씩 보며,
// rollback은 저널의 슬롯만 직접 회수한다(회수 대상 목록을 걷지 않는다).
TEST(ObjectRegistryModel_SlotWorkStaysConstantPerOperation) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterNode(types));
        REQUIRE(RegisterLeaseHolder(types));
        ObjectRegistry registry(allocator, types, {0x52474d50, 1});
        const auto access = registry.OwnerAccess();
        constexpr uint32 Count = 4096;
        Array<RootRef<Object>> kept(allocator);
        REQUIRE(kept.TryReserve(Count));
        detail::RegistryWorkCounters before = Counters(registry);
        for (uint32 index = 0; index < Count; ++index) {
            RootRef<Object> root;
            REQUIRE(NewNode(registry, &root));
            if (index % 2 == 0) {
                REQUIRE(kept.TryPushBack(Move(root)));
            }
        }

        detail::RegistryWorkCounters after = Counters(registry);
        CHECK(after.freeSlotProbes - before.freeSlotProbes == Count);
        CHECK(registry.Stats().chunks == Count / 64);

        // 쓰레기 2048개 회수: 회수 대상 목록의 슬롯을 한 번씩만 본다.
        before = after;
        auto collected = registry.Collect(access);
        REQUIRE(collected);
        CHECK(collected.Value().reclaimed == Count / 2);
        after = Counters(registry);
        CHECK(after.reclaimProbes - before.reclaimProbes == Count / 2);

        // 빈 슬롯이 옛 chunk에 흩어져 있어도 생성마다 하나만 보고 새 chunk를 만들지 않는다.
        before = after;
        for (uint32 index = 0; index < Count / 2; ++index) {
            REQUIRE(NewNode(registry));
        }

        after = Counters(registry);
        CHECK(after.freeSlotProbes - before.freeSlotProbes == Count / 2);
        CHECK(registry.Stats().chunks == Count / 64);

        // rollback 1000개: 저널의 슬롯만 직접 회수한다.
        Array<ObjectHandle> rolled(allocator);
        REQUIRE(rolled.TryReserve(1000));
        before = Counters(registry);
        {
            ConstructionContext transaction(registry, access);
            for (uint32 index = 0; index < 1000; ++index) {
                auto made = transaction.Create<ModelNode>(ModelNodeType);
                REQUIRE(made);
                REQUIRE(rolled.TryPushBack(made.Value()));
            }

            transaction.Rollback();
        }

        after = Counters(registry);
        CHECK(after.reclaimProbes == before.reclaimProbes);
        CHECK(registry.Stats().reservedObjects == 0 && registry.Stats().retiredObjects == 0);
        for (const ObjectHandle handle : rolled) {
            CHECK(IsReclaimed(registry, handle));
        }

        // BackendLease로 지연된 100개: Collect는 분류 전 목록을 한 번 걷고(회수 3996 + 지연 100),
        // 지연 슬롯은 다시 걷지 않는다. lease를 놓으면 풀린 목록에서 바로 꺼낸다.
        constexpr uint32 Deferred = 100;
        Array<BackendLease> leases(allocator);
        REQUIRE(leases.TryReserve(Deferred));
        for (uint32 index = 0; index < Deferred; ++index) {
            auto lease = registry.AcquireBackendLease(access, kept[index].Handle());
            REQUIRE(lease);
            REQUIRE(leases.TryPushBack(Move(lease.Value())));
        }

        for (RootRef<Object>& root : kept) {
            root.Reset();
        }

        before = Counters(registry);
        collected = registry.Collect(access);
        REQUIRE(collected);
        CHECK(collected.Value().reclaimed == Count - Deferred);
        after = Counters(registry);
        CHECK(after.reclaimProbes - before.reclaimProbes == Count);
        CHECK(registry.Stats().retiredObjects == Deferred);
        before = after;
        auto drained = registry.DrainRetired(access);
        REQUIRE(drained);
        CHECK(drained.Value() == 0);
        after = Counters(registry);
        CHECK(after.reclaimProbes == before.reclaimProbes);

        // 지연 H개가 남은 채, 이번 쓰레기 N쌍의 holder 소멸자가 짝 victim의 lease를 놓는다: 회수
        // 탐색은 N에 비례하고 H를 다시 걷지 않는다(쌍마다 분류 2번 + 풀린 victim 1번 이하).
        constexpr uint32 Pairs = 1000;
        for (uint32 index = 0; index < Pairs; ++index) {
            auto victim = NewNode(registry);
            REQUIRE(victim);
            auto holder = registry.NewObject<LeaseHolder>(access, LeaseHolderType);
            REQUIRE(holder);
            auto lease = registry.AcquireBackendLease(access, victim.Value());
            REQUIRE(lease);
            auto borrowed = registry.Resolve<LeaseHolder>(access, holder.Value().Handle());
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            pointer.Value()->lease = Move(lease.Value());
        }

        before = Counters(registry);
        collected = registry.Collect(access);
        REQUIRE(collected);
        CHECK(collected.Value().reclaimed == 2 * Pairs);
        after = Counters(registry);
        CHECK(after.reclaimProbes - before.reclaimProbes <= 3 * Pairs);
        CHECK(registry.Stats().retiredObjects == Deferred);
        for (BackendLease& lease : leases) {
            lease.Reset();
        }

        before = after;
        drained = registry.DrainRetired(access);
        REQUIRE(drained);
        CHECK(drained.Value() == Deferred);
        after = Counters(registry);
        CHECK(after.reclaimProbes - before.reclaimProbes == Deferred);
        CHECK(registry.Stats().retiredObjects == 0);

        // 증분 회수: slice마다 회수한 것과 완료 검사의 첫 칸만 본다.
        for (uint32 index = 0; index < Count; ++index) {
            REQUIRE(NewNode(registry));
        }

        before = Counters(registry);
        REQUIRE(registry.BeginCollection(access));
        uint32 slices = 0;
        while (registry.CollectionPhase() != GcPhase::Idle && slices < 100000) {
            REQUIRE(registry.StepCollection(access, {64, 0}));
            ++slices;
        }

        after = Counters(registry);
        CHECK(registry.LastGcReport().reclaimed == Count);
        CHECK(after.reclaimProbes - before.reclaimProbes <= uint64(Count) + slices);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 빈 슬롯은 가장 최근에 회수한 것부터 재사용하고(새 chunk는 낮은 번호부터), 세대가 오른다.
TEST(ObjectRegistryModel_ReusesMostRecentlyReclaimedSlotFirst) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterNode(types));
        ObjectRegistry registry(allocator, types, {0x52474d51, 1});
        const auto access = registry.OwnerAccess();
        ObjectHandle first[3];
        for (ObjectHandle& handle : first) {
            auto made = NewNode(registry);
            REQUIRE(made);
            handle = made.Value();
        }

        CHECK(first[0].slot == 0 && first[1].slot == 1 && first[2].slot == 2);
        // 회수 순서는 sweep 순서(슬롯 0, 1, 2)라 빈 슬롯 목록 맨 앞은 2다.
        REQUIRE(registry.Collect(access));
        const uint32 expected[] = {2, 1, 0, 3};
        RootRef<Object> roots[4];
        for (uint32 index = 0; index < 4; ++index) {
            auto made = NewNode(registry, &roots[index]);
            REQUIRE(made);
            CHECK(made.Value().slot == expected[index]);
            CHECK(made.Value().generation == (index < 3 ? 2u : 1u));
        }

        for (RootRef<Object>& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// rollback은 저널의 슬롯만 회수한다. 이미 회수 가능해진 다른 Retired 슬롯은 DrainRetired·GC가
// 회수한다.
TEST(ObjectRegistryModel_RollbackReclaimsOnlyJournalSlots) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterNode(types));
        ObjectRegistry registry(allocator, types, {0x52474d52, 1});
        const auto access = registry.OwnerAccess();
        auto lingering = NewNode(registry);
        REQUIRE(lingering);
        auto lease = registry.AcquireBackendLease(access, lingering.Value());
        REQUIRE(lease);
        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().retiredObjects == 1);
        lease.Value().Reset();

        ObjectHandle staged[3];
        const detail::RegistryWorkCounters before = Counters(registry);
        {
            ConstructionContext transaction(registry, access);
            auto outer = transaction.Create<ModelNode>(ModelNodeType);
            REQUIRE(outer);
            auto inner = transaction.Create<ModelNode>(ModelNodeType, outer.Value(), "Inner");
            REQUIRE(inner);
            auto plain = transaction.Create<ModelNode>(ModelNodeType);
            REQUIRE(plain);
            staged[0] = outer.Value();
            staged[1] = inner.Value();
            staged[2] = plain.Value();
            transaction.Rollback();
        }

        CHECK(Counters(registry).reclaimProbes == before.reclaimProbes);
        for (const ObjectHandle handle : staged) {
            CHECK(IsReclaimed(registry, handle));
        }

        CHECK(registry.Stats().retiredObjects == 1);
        CHECK(registry.Stats().reservedObjects == 0);
        CHECK(!IsReclaimed(registry, lingering.Value()));
        auto drained = registry.DrainRetired(access);
        REQUIRE(drained);
        CHECK(drained.Value() == 1);
        CHECK(IsReclaimed(registry, lingering.Value()));
        CHECK(registry.Stats().retiredObjects == 0);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 회수 중 소멸자가 다른 Retired 슬롯의 BackendLease를 놓으면, 지연 목록에 있던 그 슬롯이 풀린
// 목록으로 옮겨져 같은 회수에서 회수된다(목록 순서와 무관하게, 전체·증분 모두).
TEST(ObjectRegistryModel_ReleaseDuringReclaimRevisitsSkippedSlots) {
    SystemAllocator allocator;
    for (uint32 mode = 0; mode < 4; ++mode) {
        TypeRegistry types(allocator);
        REQUIRE(RegisterNode(types));
        REQUIRE(RegisterLeaseHolder(types));
        // 새 Registry라 만든 순서가 슬롯 순서(= sweep·회수 대상 목록 순서)다.
        ObjectRegistry registry(allocator, types, {0x52474d53, mode});
        const auto access = registry.OwnerAccess();
        {
            // mode 0·2: 잡힌 쪽이 먼저(목록 앞), 1·3: 쥔 쪽이 먼저. 2·3은 증분 GC(slice 예산 1).
            const bool heldFirst = mode % 2 == 0;
            ObjectHandle held;
            ObjectHandle holder;
            for (uint32 step = 0; step < 2; ++step) {
                if ((step == 0) == heldFirst) {
                    auto made = NewNode(registry);
                    REQUIRE(made);
                    held = made.Value();
                } else {
                    auto made = registry.NewObject<LeaseHolder>(access, LeaseHolderType);
                    REQUIRE(made);
                    holder = made.Value().Handle();
                    made.Value().Reset();
                }
            }

            {
                auto lease = registry.AcquireBackendLease(access, held);
                REQUIRE(lease);
                auto borrowed = registry.Resolve<LeaseHolder>(access, holder);
                REQUIRE(borrowed);
                auto pointer = borrowed.Value().Get();
                REQUIRE(pointer);
                pointer.Value()->lease = Move(lease.Value());
            }

            if (mode < 2) {
                auto collected = registry.Collect(access);
                REQUIRE(collected);
                CHECK(collected.Value().reclaimed == 2);
            } else {
                REQUIRE(registry.BeginCollection(access));
                for (uint32 guard = 0; guard < 1000 && registry.CollectionPhase() != GcPhase::Idle;
                     ++guard) {
                    REQUIRE(registry.StepCollection(access, {1, 0}));
                }

                CHECK(registry.CollectionPhase() == GcPhase::Idle);
                CHECK(registry.LastGcReport().reclaimed == 2);
            }

            CHECK(held.slot == (heldFirst ? 0u : 1u));
            CHECK(IsReclaimed(registry, held));
            CHECK(IsReclaimed(registry, holder));
            CHECK(registry.Stats().retiredObjects == 0);
            CHECK(registry.Stats().backendHolds == 0);
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
