#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <ObjectRuntime/Private/ObjectRegistryAccess.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// ObjectRegistry 모델 대조 시험이다. 고정 seed의 무작위 연산(생성·트랜잭션·root 해제·파괴 요청·
// Discard·전체·증분 GC·DrainRetired) 뒤마다 이름 색인·자식 고리·객체 경로·CollectInners·
// DescribeOuter·통계를 독립 모델과 대조하고, 전체 Collect 뒤 생존 집합이 rooted 객체의 Outer 닫힘과
// 같은지, 끝에 할당 누수가 없는지 본다. 이어서 저장소 정비(chunk 포인터 표·빈 슬롯 목록·회수 대상
// 목록)의 성질을 시간이 아니라 잠금 안 탐색 계수로 본다 — 생성·회수 한 번이 보는 슬롯 수가 저장소
// 크기와 무관하다. 끝으로 패키지 정책 Registry에서 임의의 편집·Commit·Discard·GC 뒤 관찰자 누계가
// 객체 내용을 다시 센 패키지 쌍 다중도와 같은지 본다(Docs/Design/ObjectFoundations.ko.md 9절 Ob).
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
    bool discarded = false; // Discard됨(이름·고리에서 빠졌다 — 상태는 DestroyRequested 이후)
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
            if (kind < 33) {
                DoCreate();
            } else if (kind < 43) {
                DoTransaction(false);
            } else if (kind < 56) {
                if (Count() > 0) {
                    roots_[Below(Count())].Reset();
                }
            } else if (kind < 61) {
                const int32 picked = PickPublished();
                if (picked >= 0) {
                    (void)registry_.RequestDestroy(access_, objects_[picked].handle);
                }
            } else if (kind < 67) {
                DoDiscard();
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
            if (!object.discarded && (publishedOnly ? Published(phase) : Occupies(phase))) {
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
                if (objects_[index].outer == outer && Occupies(phases_[index]) &&
                    !objects_[index].discarded) {
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

    // index가 picked의 부분 트리(고리로 닿는 객체)인지다: picked까지의 Outer 사슬(picked 제외)이
    // 모두 Discard되지 않고 고리에 있는(Alive·DestroyRequested·cutoff white) 객체여야 한다.
    bool InSubtree(int32 index, int32 picked) const noexcept {
        for (int32 at = index; at >= 0; at = objects_[at].outer) {
            if (at == picked) {
                return true;
            }

            const Phase phase = phases_[at];
            if (objects_[at].discarded ||
                !(phase == Phase::Alive || phase == Phase::DestroyRequested ||
                  phase == Phase::Cutoff)) {
                return false;
            }
        }

        return false;
    }

    void DoDiscard() noexcept {
        if (Count() == 0) {
            return;
        }

        const auto picked = static_cast<int32>(Below(Count()));
        const Phase phase = phases_[picked];
        ErrorCode expected = ErrorCode::None;
        if (phase == Phase::Gone) {
            expected = ErrorCode::StaleHandle;
        } else if (objects_[picked].discarded) {
            expected = ErrorCode::None; // 무동작
        } else if (phase == Phase::Cutoff || phase == Phase::Retired) {
            expected = ErrorCode::Destroyed;
        }

        // 부분 트리는 Discard 전 상태로 정한다(표시하면서 판정하면 앞서 표시한 Outer가 사슬을
        // 끊는다).
        Array<uint32> subtree(allocator_);
        for (uint32 index = 0; index < Count(); ++index) {
            if (InSubtree(static_cast<int32>(index), picked) && !subtree.TryPushBack(index)) {
                MODEL_EXPECT(false);
                return;
            }
        }

        auto discarded = registry_.Discard(access_, objects_[picked].handle);
        const ErrorCode actual = discarded ? ErrorCode::None : discarded.Error();
        MODEL_EXPECT(actual == expected, static_cast<int64>(actual), static_cast<int64>(expected));
        if (!discarded || objects_[picked].discarded) {
            return;
        }

        for (const uint32 index : subtree) {
            objects_[index].discarded = true;
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

        // 부분 트리에 공개 전 객체가 있는 Outer의 Discard는 Busy, 공개 전 객체 자체는
        // StaleHandle이다.
        for (int32 at = 0; at < stagedCount && !dead; ++at) {
            const int32 outer = objects_[first + at].outer;
            if (outer >= 0 && outer < first && objects_[outer].committed && Below(2) == 0) {
                auto busy = registry_.Discard(access_, objects_[outer].handle);
                MODEL_EXPECT(!busy && busy.Error() == ErrorCode::Busy, outer);
                auto stale = registry_.Discard(access_, staged[at]);
                MODEL_EXPECT(!stale && stale.Error() == ErrorCode::StaleHandle, first + at);
                break;
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
            if (!object.discarded && (object.outer >= 0 || object.key >= 0)) {
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
                        if (objects_[index].outer == outer && Occupies(phases_[index]) &&
                            !objects_[index].discarded) {
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
            if ((object.outer >= 0 || object.key >= 0) && Published(phase) && !object.discarded) {
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

namespace model_fixture {

// 관찰자 대조 모델의 StructRefArray 원소다(등록 struct — Transient 참조 하나).
struct ModelSlot {
    explicit ModelSlot(Allocator&) noexcept {
    }

    ObjectRef<Object> target;
};

struct EdgeNode final : Object {
    explicit EdgeNode(Allocator& allocator) noexcept : items(allocator), slots(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    ObjectRef<Object> link;
    ObjectRefArray<Object> items;
    StructRefArray<ModelSlot> slots;
};

struct ModelPackage final : Object {};

} // namespace model_fixture

namespace rocks {

template <>
struct GeneratedType<model_fixture::ModelSlot> {
    static constexpr bool ContainsStrongReference = true;

    static void Trace(const model_fixture::ModelSlot& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }
};

} // namespace rocks

void model_fixture::EdgeNode::Trace(TraceVisitor& visitor) const noexcept {
    visitor.Visit(link);
    items.Trace(visitor);
    slots.Trace(visitor);
}

namespace {

using model_fixture::EdgeNode;
using model_fixture::ModelPackage;
using model_fixture::ModelSlot;

constexpr TypeId EdgePackageType{0x52474d4f44454c41ULL, 1};
constexpr TypeId EdgeNodeType{0x52474d4f44454c42ULL, 1};
constexpr TypeId EdgeSlotType{0x52474d4f44454c43ULL, 1};
constexpr Guid EdgeSlotTargetId{0x52474d53ULL, 1};

// 완전한 필드 배치를 선언한 함수표다(Trace가 모든 참조 멤버를 방문한다 — 패키지 정책 Registry의
// 생성 조건).
template <class T>
RuntimeTypeOps CompleteOps() noexcept {
    RuntimeTypeOps ops = MakeRuntimeTypeOps<T>();
    ops.completeFieldLayout = true;
    return ops;
}

bool RegisterEdgeTypes(TypeRegistry& types) noexcept {
    FieldSchema field;
    field.id = EdgeSlotTargetId;
    field.kind = FieldKind::ObjectReference;
    field.flags = static_cast<uint32>(FieldFlags::Transient);
    field.name = Span<const char>("target", 6);
    field.value = ValueType{ValueKind::ObjectReference, {}};
    const RuntimeStructFieldOps fieldOps[] = {{EdgeSlotTargetId}};
    RuntimeStructOps slot = MakeRuntimeStructOps<ModelSlot>();
    slot.trace = [](const void* value, TraceVisitor& visitor) noexcept {
        GeneratedType<ModelSlot>::Trace(*static_cast<const ModelSlot*>(value), visitor);
    };
    slot.fields = Span<const RuntimeStructFieldOps>(fieldOps, 1);
    return types
               .RegisterStruct({EdgeSlotType, 1, Span<const FieldSchema>(&field, 1), {"Slot", 4}},
                               Move(slot))
               .IsOk() &&
           types.Register({EdgePackageType, 1, {}, {}}, CompleteOps<ModelPackage>()).IsOk() &&
           types.Register({EdgeNodeType, 1, {}, {}}, CompleteOps<EdgeNode>()).IsOk();
}

// (소유 패키지, 대상 패키지) 다중도 표다(선형 — 모델 크기가 작다).
struct PairCount {
    ObjectHandle owner;
    ObjectHandle target;
    int64 count = 0;
};

void AddPair(Array<PairCount>& pairs, ObjectHandle owner, ObjectHandle target,
             int64 delta) noexcept {
    for (PairCount& pair : pairs) {
        if (pair.owner == owner && pair.target == target) {
            pair.count += delta;
            return;
        }
    }

    PairCount pair;
    pair.owner = owner;
    pair.target = target;
    pair.count = delta;
    if (!pairs.TryPushBack(pair)) {
        platform::Fatal("observer model pair allocation failed");
    }
}

int64 CountOf(const Array<PairCount>& pairs, ObjectHandle owner, ObjectHandle target) noexcept {
    for (const PairCount& pair : pairs) {
        if (pair.owner == owner && pair.target == target) {
            return pair.count;
        }
    }

    return 0;
}

constexpr uint32 MaxEdgeObjects = 600;

// 관찰자 대조 모델이다. 패키지 정책 Registry에서 임의의 live 편집(단일·다중 간선 지점)·staged
// 쓰기와 Commit·Rollback·Discard·root 해제(Discard 뒤에만)·전체·증분 GC를 섞고, 관찰자에서 꺼낸
// 변화의 누계가 Alive 소유자(Outer 있음)의 실제 필드를 다시 센 패키지 쌍 다중도와 같은지 본다.
// 간선 자체는 모델링하지 않는다(객체에서 읽는다) — 모델은 객체의 소속 패키지·Discard·회수만 안다.
class ObserverModel {
public:
    ObserverModel(Allocator& allocator, ObjectRegistry& registry, ReferenceObserver& observer,
                  uint64 seed) noexcept
        : allocator_(allocator), registry_(registry), observer_(observer),
          access_(registry.OwnerAccess()), seed_(seed), rng_(seed * 0x2545f4914f6cdd1dULL + 7),
          objects_(allocator), roots_(allocator), totals_(allocator) {
    }

    bool Prepare() noexcept {
        return objects_.TryReserve(MaxEdgeObjects).IsOk() &&
               roots_.TryReserve(MaxEdgeObjects).IsOk();
    }

    void Run(uint32 operations) noexcept {
        for (operation_ = 0; operation_ < operations && !failed_; ++operation_) {
            Refresh();
            const uint32 kind = Below(100);
            if (kind < 8) {
                NewPackage();
            } else if (kind < 20) {
                NewMember();
            } else if (kind < 50) {
                Edit();
            } else if (kind < 60) {
                Transaction();
            } else if (kind < 67) {
                DiscardSome();
            } else if (kind < 72) {
                ReleaseDiscardedRoot();
            } else if (kind < 78) {
                if (registry_.CollectionPhase() == GcPhase::Idle) {
                    Expect(registry_.Collect(access_).IsOk(), "Collect", __LINE__);
                }
            } else if (kind < 90) {
                StepGc();
            } else {
                Take();
            }

            if (operation_ % 25 == 24) {
                Verify();
            }
        }
    }

    // 남은 cycle을 마치고 대조한 뒤 모든 패키지를 Discard하고 root를 놓는다(Shutdown 전).
    void Finish() noexcept {
        for (uint32 guard = 0; guard < 1000000 && registry_.CollectionPhase() != GcPhase::Idle;
             ++guard) {
            if (!registry_.StepCollection(access_, {0xffffffffu, 0})) {
                break;
            }
        }

        Verify();
        for (uint32 index = 0; index < Count(); ++index) {
            if (objects_[index].isPackage && Live(index)) {
                Expect(registry_.Discard(access_, objects_[index].handle).IsOk(), "final Discard",
                       __LINE__);
            }

            roots_[index].Reset();
        }

        Expect(registry_.Collect(access_).IsOk(), "final Collect", __LINE__);
        const ObjectStats stats = registry_.Stats();
        Expect(stats.liveObjects == 0 && stats.destroyRequested == 0, "all reclaimed", __LINE__,
               stats.liveObjects, stats.destroyRequested);
        Expect(stats.retiredWithoutDiscard == 0, "retiredWithoutDiscard", __LINE__,
               stats.retiredWithoutDiscard);
    }

    bool Failed() const noexcept {
        return failed_;
    }

    usize Verified() const noexcept {
        return verified_;
    }

private:
    struct Tracked {
        ObjectHandle handle;
        int32 package = -1; // 소속 패키지 위치(패키지는 자기)
        bool isPackage = false;
        bool discarded = false;
        bool gone = false;
    };

    bool Expect(bool ok, const char* what, int line, int64 first = -1, int64 second = -1) noexcept {
        if (ok || failed_) {
            return ok;
        }

        failed_ = true;
        snprintf(message_, sizeof(message_), "ObserverModel seed %llu op %u: %s [%lld, %lld]",
                 static_cast<unsigned long long>(seed_), operation_, what,
                 static_cast<long long>(first), static_cast<long long>(second));
        test::RecordFailure(message_, __FILE__, line);
        return false;
    }

    uint32 Below(uint32 bound) noexcept {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return static_cast<uint32>(rng_ >> 11) % bound;
    }

    uint32 Count() const noexcept {
        return static_cast<uint32>(objects_.Size());
    }

    bool Live(uint32 index) const noexcept {
        return !objects_[index].discarded && !objects_[index].gone;
    }

    // live 객체(members면 패키지 제외)를 임의로 고른다(없으면 -1).
    int32 Pick(bool members) noexcept {
        if (Count() == 0) {
            return -1;
        }

        const uint32 start = Below(Count());
        for (uint32 step = 0; step < Count(); ++step) {
            const uint32 index = (start + step) % Count();
            if (Live(index) && (!members || !objects_[index].isPackage)) {
                return static_cast<int32>(index);
            }
        }

        return -1;
    }

    // 연결 대상: null 또는 Discard되지 않은 객체(패키지 포함).
    ObjectHandle PickTarget() noexcept {
        if (Below(5) == 0) {
            return {};
        }

        const int32 picked = Pick(false);
        return picked < 0 ? ObjectHandle{} : objects_[picked].handle;
    }

    int32 Append(ObjectHandle handle, int32 package, bool isPackage) noexcept {
        Tracked tracked;
        tracked.handle = handle;
        tracked.isPackage = isPackage;
        tracked.package = isPackage ? static_cast<int32>(Count()) : package;
        if (!objects_.TryPushBack(tracked) || !roots_.TryEmplaceBack()) {
            Expect(false, "model allocation", __LINE__);
            return -1;
        }

        return static_cast<int32>(Count() - 1);
    }

    void Refresh() noexcept {
        for (uint32 index = 0; index < Count(); ++index) {
            Tracked& object = objects_[index];
            if (object.gone) {
                continue;
            }

            auto state = registry_.InspectLifecycle(access_, object.handle);
            if (!state && state.Error() == ErrorCode::StaleHandle) {
                object.gone = true;
                // 패키지 정책: Discard되지 않은 객체는 회수되지 않는다.
                Expect(object.discarded, "reclaimed without Discard", __LINE__, index);
            }
        }
    }

    void NewPackage() noexcept {
        if (Count() + 2 > MaxEdgeObjects) {
            return;
        }

        auto package = registry_.NewObject<Object>(access_, EdgePackageType);
        if (!Expect(package.IsOk(), "NewObject package", __LINE__)) {
            return;
        }

        const int32 index = Append(package.Value().Handle(), -1, true);
        if (index < 0) {
            return;
        }

        roots_[index] = Move(package.Value());
        auto asset = registry_.NewObject<Object>(access_, EdgeNodeType, objects_[index].handle, {});
        if (Expect(asset.IsOk(), "NewObject asset", __LINE__)) {
            Append(asset.Value().Handle(), index, false);
        }
    }

    // 대표 에셋이나 그 아래 객체 밑에 이름 있는 객체를 만든다(패키지 바로 아래는 대표 자식뿐).
    void NewMember() noexcept {
        if (Count() + 1 > MaxEdgeObjects) {
            return;
        }

        const int32 outer = Pick(true);
        if (outer < 0) {
            return;
        }

        char name[16];
        snprintf(name, sizeof(name), "n%u", nameCounter_++);
        auto made = registry_.NewObject<Object>(access_, EdgeNodeType, objects_[outer].handle,
                                                StringView(name));
        if (Expect(made.IsOk(), "NewObject member", __LINE__)) {
            Append(made.Value().Handle(), objects_[outer].package, false);
        }
    }

    void Edit() noexcept {
        const int32 owner = Pick(true);
        if (owner < 0) {
            return;
        }

        const ObjectHandle handle = objects_[owner].handle;
        auto borrowed = registry_.Resolve<EdgeNode>(access_, handle);
        if (!Expect(borrowed.IsOk(), "Resolve owner", __LINE__, owner)) {
            return;
        }

        auto pointer = borrowed.Value().Get();
        if (!Expect(pointer.IsOk(), "Borrow owner", __LINE__, owner)) {
            return;
        }

        EdgeNode& node = *pointer.Value();
        Result<void> result = Result<void>::Success();
        const uint32 action = Below(11);
        const ObjectHandle target = PickTarget();
        const auto items = static_cast<uint32>(node.items.Size());
        const auto slots = static_cast<uint32>(node.slots.Size());
        switch (action) {
        case 0:
        case 1:
            result = registry_.SetReference(access_, handle, node.link, target);
            break;
        case 2:
            result = registry_.AddReference(access_, handle, node.items, target);
            break;
        case 3:
            result =
                registry_.InsertReferenceAt(access_, handle, node.items, Below(items + 1), target);
            break;
        case 4:
            if (items != 0) {
                result =
                    registry_.SetReferenceAt(access_, handle, node.items, Below(items), target);
            }

            break;
        case 5:
            if (items != 0) {
                result = registry_.RemoveReferenceAt(access_, handle, node.items, Below(items));
            }

            break;
        case 6:
            if (Below(3) == 0) {
                result = registry_.ClearReferences(access_, handle, node.items);
            }

            break;
        case 7: {
            auto added = registry_.AddElement(access_, handle, node.slots);
            result = added ? Result<void>::Success() : Result<void>::Failure(added.Error());
            break;
        }
        case 8:
            if (slots != 0) {
                const usize at = Below(slots);
                result = registry_.SetElementReference(access_, handle, node.slots, at,
                                                       node.slots[at].target, target);
            }

            break;
        case 9:
            if (slots != 0) {
                result = registry_.RemoveElementAt(access_, handle, node.slots, Below(slots));
            }

            break;
        default:
            if (Below(3) == 0) {
                result = registry_.ClearElements(access_, handle, node.slots);
            }

            break;
        }

        Expect(result.IsOk(), "edit", __LINE__, action,
               result ? 0 : static_cast<int64>(result.Error()));
    }

    // staged 객체(새 패키지와 대표 에셋, 또는 live 객체 아래 이름 있는 객체 1–3개)와 staged 쓰기,
    // 그 뒤 Commit 또는 Rollback.
    void Transaction() noexcept {
        if (Count() + 5 > MaxEdgeObjects) {
            return;
        }

        ConstructionContext transaction(registry_, access_);
        ObjectHandle staged[5];
        bool newPackage[5] = {}; // 새 패키지 소속(아래에서 위치를 얻는다)
        bool stagedIsPackage[5] = {};
        uint32 count = 0;
        int32 hostPackage = -1;
        ObjectHandle host;
        if (Below(3) == 0) {
            auto package = transaction.Create<ModelPackage>(EdgePackageType);
            if (!Expect(package.IsOk(), "staged package", __LINE__)) {
                return;
            }

            stagedIsPackage[count] = true;
            newPackage[count] = true;
            staged[count++] = package.Value();
            auto asset = transaction.Create<EdgeNode>(EdgeNodeType, package.Value());
            if (!Expect(asset.IsOk(), "staged asset", __LINE__)) {
                return;
            }

            newPackage[count] = true;
            staged[count++] = asset.Value();
            host = asset.Value();
        } else {
            const int32 outer = Pick(true);
            if (outer < 0) {
                return;
            }

            hostPackage = objects_[outer].package;
            host = objects_[outer].handle;
        }

        const uint32 extra = 1 + Below(3);
        for (uint32 step = 0; step < extra; ++step) {
            char name[16];
            snprintf(name, sizeof(name), "t%u", nameCounter_++);
            auto made = transaction.Create<EdgeNode>(EdgeNodeType, host, StringView(name));
            if (!Expect(made.IsOk(), "staged member", __LINE__)) {
                return;
            }

            newPackage[count] = hostPackage < 0;
            staged[count++] = made.Value();
        }

        // staged 쓰기: 대상은 live 객체 또는 같은 트랜잭션 객체.
        auto stagedTarget = [&]() noexcept {
            return Below(3) == 0 ? staged[Below(count)] : PickTarget();
        };
        for (uint32 at = 0; at < count && !failed_; ++at) {
            if (stagedIsPackage[at]) {
                continue;
            }

            auto pointer = transaction.Get<EdgeNode>(staged[at]);
            if (!Expect(pointer.IsOk(), "staged Get", __LINE__)) {
                return;
            }

            EdgeNode& node = *pointer.Value();
            Expect(transaction.SetReference(staged[at], node.link, stagedTarget()).IsOk(),
                   "staged SetReference", __LINE__);
            const ObjectHandle targets[] = {stagedTarget(), stagedTarget(), stagedTarget()};
            Expect(transaction
                       .AssignReferences(staged[at], node.items,
                                         Span<const ObjectHandle>(targets, Below(4)))
                       .IsOk(),
                   "staged AssignReferences", __LINE__);
            if (Below(2) == 0) {
                Expect(transaction.AssignElements(staged[at], node.slots, 2).IsOk(),
                       "staged AssignElements", __LINE__);
                Expect(transaction
                           .SetElementReference(staged[at], node.slots, 1, node.slots[1].target,
                                                stagedTarget())
                           .IsOk(),
                       "staged SetElementReference", __LINE__);
            }
        }

        if (failed_ || Below(5) == 0) {
            transaction.Rollback();
            return;
        }

        // 새 패키지는 root로 붙잡는다(패키지 정책 — Discard 전에는 놓지 않는다).
        const ObjectHandle roots[] = {staged[0]};
        RootRef<Object> held[1];
        const usize rootCount = stagedIsPackage[0] ? 1 : 0;
        auto committed = transaction.Commit(Span<const ObjectHandle>(roots, rootCount),
                                            Span<RootRef<Object>>(held, rootCount));
        if (!Expect(committed.IsOk(), "Commit", __LINE__)) {
            return;
        }

        int32 packageIndex = hostPackage;
        for (uint32 at = 0; at < count; ++at) {
            const int32 index = Append(staged[at], newPackage[at] ? packageIndex : hostPackage,
                                       stagedIsPackage[at]);
            if (index < 0) {
                return;
            }

            if (stagedIsPackage[at]) {
                packageIndex = index;
                roots_[index] = Move(held[0]);
            }
        }
    }

    // live 객체(패키지 포함)를 Discard하고 부분 트리(Discard되지 않은 자손)를 모델에 표시한다.
    void DiscardSome() noexcept {
        const int32 picked = Pick(Below(3) != 0);
        if (picked < 0) {
            return;
        }

        // 표시는 Discard 전에 정한다(Outer 조회는 Discard 뒤에도 되지만 부분 트리는 지금 기준이다).
        Array<uint32> subtree(allocator_);
        for (uint32 index = 0; index < Count(); ++index) {
            if (Live(index) && Descends(index, picked) && !subtree.TryPushBack(index)) {
                Expect(false, "model allocation", __LINE__);
                return;
            }
        }

        if (!Expect(registry_.Discard(access_, objects_[picked].handle).IsOk(), "Discard", __LINE__,
                    picked)) {
            return;
        }

        for (const uint32 index : subtree) {
            objects_[index].discarded = true;
        }
    }

    // index의 Outer 사슬에 picked가 있는지다(Registry의 GetOuter로 — 모델은 Outer 위치를 두지
    // 않는다).
    bool Descends(uint32 index, int32 picked) noexcept {
        ObjectHandle current = objects_[index].handle;
        for (uint32 guard = 0; guard < 64 && !current.IsNull(); ++guard) {
            if (current == objects_[picked].handle) {
                return true;
            }

            auto outer = registry_.GetOuter(access_, current);
            if (!outer) {
                return false;
            }

            current = outer.Value();
        }

        return false;
    }

    void ReleaseDiscardedRoot() noexcept {
        for (uint32 index = 0; index < Count(); ++index) {
            if (objects_[index].isPackage && objects_[index].discarded && roots_[index].IsValid()) {
                roots_[index].Reset();
                return;
            }
        }
    }

    void StepGc() noexcept {
        if (registry_.CollectionPhase() == GcPhase::Idle) {
            Expect(registry_.BeginCollection(access_).IsOk(), "BeginCollection", __LINE__);
            return;
        }

        Expect(registry_.StepCollection(access_, {1 + Below(6), 0}).IsOk(), "StepCollection",
               __LINE__);
    }

    void Take() noexcept {
        Array<ReferenceEdgeDelta> changes(allocator_);
        if (!Expect(observer_.TakeChanges(changes).IsOk(), "TakeChanges", __LINE__)) {
            return;
        }

        for (const ReferenceEdgeDelta& change : changes) {
            Expect(change.delta != 0, "zero delta taken", __LINE__);
            Expect((change.cause == ReferenceChangeCause::Commit) == (change.transaction != 0),
                   "transaction only on Commit", __LINE__, static_cast<int64>(change.cause));
            Expect(!(change.ownerPackage == change.targetPackage), "same-package change", __LINE__);
            AddPair(totals_, change.ownerPackage, change.targetPackage, change.delta);
        }
    }

    int32 IndexOf(ObjectHandle handle) const noexcept {
        for (uint32 index = 0; index < Count(); ++index) {
            if (objects_[index].handle == handle) {
                return static_cast<int32>(index);
            }
        }

        return -1;
    }

    // Alive 소유자(패키지가 아닌, Discard되지 않은 객체)의 실제 간선을 다시 세어 관찰자 누계와
    // 비교한다.
    void Verify() noexcept {
        Refresh();
        Take();
        Array<PairCount> recount(allocator_);
        for (uint32 index = 0; index < Count() && !failed_; ++index) {
            const Tracked& owner = objects_[index];
            if (owner.isPackage || !Live(index)) {
                continue;
            }

            auto borrowed = registry_.Resolve<EdgeNode>(access_, owner.handle);
            if (!Expect(borrowed.IsOk(), "Resolve for recount", __LINE__, index)) {
                return;
            }

            auto pointer = borrowed.Value().Get();
            if (!Expect(pointer.IsOk(), "Borrow for recount", __LINE__, index)) {
                return;
            }

            const ObjectHandle ownerPackage = objects_[owner.package].handle;
            auto count = [&](ObjectHandle target) noexcept {
                if (target.IsNull()) {
                    return;
                }

                const int32 at = IndexOf(target);
                if (!Expect(at >= 0, "unknown target", __LINE__, index)) {
                    return;
                }

                const ObjectHandle targetPackage = objects_[objects_[at].package].handle;
                if (!(targetPackage == ownerPackage)) {
                    AddPair(recount, ownerPackage, targetPackage, 1);
                }
            };
            const EdgeNode& node = *pointer.Value();
            count(node.link.Handle());
            for (const ObjectHandle item : node.items.View()) {
                count(item);
            }

            for (usize at = 0; at < node.slots.Size(); ++at) {
                count(node.slots[at].target.Handle());
            }
        }

        for (const PairCount& pair : recount) {
            Expect(CountOf(totals_, pair.owner, pair.target) == pair.count, "recount mismatch",
                   __LINE__, pair.count, CountOf(totals_, pair.owner, pair.target));
        }

        for (const PairCount& pair : totals_) {
            Expect(CountOf(recount, pair.owner, pair.target) == pair.count, "observer total extra",
                   __LINE__, pair.count, CountOf(recount, pair.owner, pair.target));
        }

        verified_ += recount.Size();
    }

    Allocator& allocator_;
    ObjectRegistry& registry_;
    ReferenceObserver& observer_;
    ObjectAccessContext access_;
    uint64 seed_;
    uint64 rng_;
    Array<Tracked> objects_;
    Array<RootRef<Object>> roots_;
    Array<PairCount> totals_;
    usize verified_ = 0;
    uint32 operation_ = 0;
    uint32 nameCounter_ = 0;
    bool failed_ = false;
    char message_[256] = {};
};

} // namespace

// 패키지 정책 Registry의 무작위 편집·Commit·Discard·GC 뒤 관찰자 누계 = 객체 내용을 다시 센
// 패키지 쌍 다중도다(속성 시험, 고정 seed 4개 × 연산 600개). 소속 객체는 Discard 없이 회수되지
// 않는다(assertRetiredWithoutDiscard 켬).
TEST(ObjectRegistryModel_ObserverTotalsMatchRecountedEdges) {
    usize verified = 0;
    for (uint64 seed = 1; seed <= 4; ++seed) {
        SystemAllocator allocator;
        {
            TypeRegistry types(allocator);
            REQUIRE(RegisterEdgeTypes(types));
            ReferenceObserver observer(allocator);
            RegistryOptions options;
            options.policy.packageNativeType = NativeTypeKey<ModelPackage>();
            options.referenceObserver = &observer;
            ObjectRegistry registry(allocator, types, {0x52474d54, seed}, options);
            {
                ObserverModel model(allocator, registry, observer, seed);
                REQUIRE(model.Prepare());
                model.Run(600);
                model.Finish();
                verified += model.Verified();
                CHECK(!model.Failed());
            }

            REQUIRE(registry.Shutdown(registry.OwnerAccess()));
            REQUIRE(types.Shutdown());
        }

        CHECK(allocator.Stats().liveAllocations == 0);
    }

    // 대조가 실제 간선을 보았다(빈 표끼리의 비교가 아니다).
    CHECK(verified > 100);
}
