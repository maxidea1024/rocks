#include <GcSoak/GcSoak.h>

#include <Core/Containers/Array.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/StructRefArray.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>

// 클러스터 장기 smoke(GC-S7, Docs/Design/GarbageCollectionScale.ko.md 13.8절)다. 모델이 간선
// 다중집합· outer·소속(루트 번호)·파괴 요청·Discard·루트 보유를 따라가고, 매 slice 뒤 회수된 객체가
// oracle (루트에서 간선·inner → outer·패키지 고리·루트 → 소속)에서 닿지 않는지(안전성), 일정 간격의
// 정지형 Collect 뒤 생존 집합이 oracle과 같은지(정확성)를 본다. Registry는 Begin·Collect마다 장부를
// 다시 센다(RegistryOptions::verifyClusterLedger — Debug).
//
// - World형(packages = false): Level(루트 보유, 지정 클러스터 루트) 아래 Actor·Component가 가입하고
//   Level → Actor, Actor → Component 소유 간선을 둔다. 같은 Level Actor 사이 참조, Level 밖 일반
//   객체와 그 간선, Actor·Component·Level 파괴, World API 밖 Actor 파괴(Component 정리는 다음 Begin
//   전에 — World의 RepairBypassedActors와 같다)를 섞는다. 소속 불변식 검증을 켠다.
// - 패키지형(packages = true): 패키지 정책 Registry에서 패키지 생성(outer 사슬 포함), 패키지 사이
//   간선 편집, 부분 트리 Discard, 패키지 Discard와 루트 반환, Discard 없이 루트 반환(버그 경로)을
//   섞는다.
// 두 형 모두 소속 소유자의 단일 필드 쓰기·배열 원소 교체·StructRefArray 원소 추가·제거·비우기(등록
// struct — 장부가 원소 trace로 센다)를 섞는다. 패키지형은 패키지 객체의 간선과 기존 패키지 아래
// 새 객체도 만들어, 경계 비율 안전장치가 해체한 패키지를 모델이 Registry 조회로 따라간다.
// 모든 변경은 증분 cycle의 slice 사이에도 일어난다(장벽으로만 shade된 클러스터의 해체, slice
// 예산 중간에 끊긴 클러스터, 처리된 클러스터에 밖 outer를 가진 새 소속 가입이 무작위로 생긴다).
namespace rocks {

namespace {

constexpr uint32 NoIndex = NumericLimits<uint32>::Max();
// 모델 간선 칸 수다(Level이 Actor를 소유하는 칸). 그 밖의 소유자는 OtherEdges개까지 쓴다.
constexpr uint32 MaxEdges = 48;
constexpr uint32 OtherEdges = 6;
// 모델 StructRefArray 원소 칸 수다.
constexpr uint32 MaxSlots = 4;
constexpr TypeId ClusterSoakNodeType{0x4743534b4e4f4431ULL, 1};
constexpr TypeId ClusterSoakPackageType{0x4743534b504b4731ULL, 1};
constexpr TypeId ClusterSoakSlotType{0x4743534b534c5431ULL, 1};
constexpr Guid ClusterSoakSlotTargetId{0x4743534bULL, 1};

} // namespace

} // namespace rocks

namespace rocks_cluster_soak {

// StructRefArray 원소다(등록 struct).
struct ClusterSoakSlot {
    explicit ClusterSoakSlot(rocks::Allocator&) noexcept {
    }

    rocks::ObjectRef<rocks::Object> target;
};

} // namespace rocks_cluster_soak

namespace rocks {

template <>
struct GeneratedType<rocks_cluster_soak::ClusterSoakSlot> {
    static constexpr bool ContainsStrongReference = true;

    static void Trace(const rocks_cluster_soak::ClusterSoakSlot& value,
                      TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }
};

namespace {

using rocks_cluster_soak::ClusterSoakSlot;

class ClusterSoakNode final : public Object {
public:
    explicit ClusterSoakNode(Allocator& allocator) noexcept : items(allocator), slots(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(single);
        items.Trace(visitor);
        slots.Trace(visitor);
    }

    // 단일 참조 필드(SetReference — 장부의 단일 간선 지점)다.
    ObjectRef<Object> single;
    ObjectRefArray<Object> items;
    // 원소 제거·비우기는 등록 struct의 원소 trace로 장부를 고친다.
    StructRefArray<ClusterSoakSlot> slots;
};

class ClusterSoakPackage final : public Object {
public:
    explicit ClusterSoakPackage(Allocator& allocator) noexcept : items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        items.Trace(visitor);
    }

    ObjectRefArray<Object> items;
};

enum class Role : uint8 {
    Normal,
    Level,
    Actor,
    Component,
    Package,
    Inner
};

struct Rng {
    uint64 state;

    uint32 Below(uint32 bound) noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<uint32>((state * 0x2545f4914f6cdd1dULL) % bound);
    }
};

struct Entry {
    ObjectHandle handle;
    Role role = Role::Normal;
    uint32 outer = NoIndex;
    // 소속 클러스터의 루트 번호다(없으면 NoIndex). 루트는 자기 번호다.
    uint32 cluster = NoIndex;
    // World형: Actor의 Level, Component의 Actor다.
    uint32 owner = NoIndex;
    uint32 edges[MaxEdges];
    uint32 degree = 0;
    // 단일 참조 필드의 대상이다(없으면 NoIndex).
    uint32 single = NoIndex;
    // StructRefArray 원소의 대상이다(원소 순서, null이면 NoIndex).
    uint32 slots[MaxSlots];
    uint32 slotCount = 0;
    bool clusterRoot = false;
    bool destroyed = false;
    bool discarded = false;
    // World API 밖에서 파괴돼 다음 Begin 전에 Component를 정리할 Actor다.
    bool bypassed = false;
    bool dead = false;
    bool reachable = false;
    RootRef<Object> held;
};

class ClusterSoak {
public:
    ClusterSoak(Allocator& allocator, const GcClusterSoakOptions& options,
                GcClusterSoakReport& report) noexcept
        : options_(options), report_(report), rng_{options.seed | 1u}, entries_(allocator),
          stack_(allocator), memberHead_(allocator), memberNext_(allocator), childHead_(allocator),
          childNext_(allocator) {
    }

    bool Run(ObjectRegistry& objects) noexcept {
        objects_ = &objects;
        if (!entries_.TryReserve(options_.maxObjects) || !stack_.TryReserve(options_.maxObjects)) {
            return Fail("model allocation failed", NoIndex, ErrorCode::OutOfMemory);
        }

        for (uint32 count = 0; count < 200 && report_.failure == nullptr; ++count) {
            Mutate();
        }

        for (uint64 tick = 0; tick < options_.ticks && report_.failure == nullptr; ++tick) {
            report_.ticks = tick + 1;
            if (options_.progress != nullptr && options_.progressInterval != 0 &&
                tick % options_.progressInterval == 0) {
                options_.progress(report_, options_.progressContext);
            }

            if (tick % 7 == 6) {
                CheckExact();
                continue;
            }

            RepairBypassed();
            if (!objects.BeginCollection(Access())) {
                return Fail("BeginCollection failed", NoIndex, ErrorCode::Busy);
            }

            ++report_.cyclesBegun;
            while (objects.CollectionPhase() != GcPhase::Idle && report_.failure == nullptr) {
                const GcBudget budget{1 + rng_.Below(48), 0};
                auto step = objects.StepCollection(Access(), budget, options_.scheduler);
                if (!step) {
                    return Fail("StepCollection failed", NoIndex, step.Error());
                }

                ++report_.steps;
                CheckSafety();
                for (uint32 edits = rng_.Below(6); edits != 0; --edits) {
                    Mutate();
                }
            }

            ++report_.cyclesCompleted;
            CheckSafety();
        }

        if (report_.failure == nullptr) {
            RepairBypassed();
            CheckExact();
        }

        return report_.failure == nullptr;
    }

    void ReleaseAll() noexcept {
        for (Entry& entry : entries_) {
            entry.held.Reset();
        }
    }

private:
    ObjectAccessContext Access() const noexcept {
        return objects_->OwnerAccess();
    }

    bool Fail(const char* reason, uint32 object, ErrorCode error) noexcept {
        if (report_.failure == nullptr) {
            report_.failure = reason;
            report_.failureTick = report_.ticks;
            report_.failureObject = object;
            report_.failureError = error;
        }

        return false;
    }

    bool Usable(uint32 index) const noexcept {
        return index < entries_.Size() && !entries_[index].dead && !entries_[index].destroyed &&
               !entries_[index].discarded;
    }

    uint32 Pick() noexcept {
        return entries_.IsEmpty() ? NoIndex : rng_.Below(static_cast<uint32>(entries_.Size()));
    }

    uint32 PickRole(Role role) noexcept {
        for (uint32 attempt = 0; attempt < 8; ++attempt) {
            const uint32 index = Pick();
            if (index != NoIndex && Usable(index) && entries_[index].role == role) {
                return index;
            }
        }

        return NoIndex;
    }

    bool Exists(uint32 index) noexcept {
        return static_cast<bool>(objects_->InspectLifecycle(Access(), entries_[index].handle));
    }

    // items 편집이다(Borrow를 쥐고 한다).
    template <class Edit>
    Result<void> EditItems(uint32 index, Edit&& edit) noexcept {
        const ObjectHandle handle = entries_[index].handle;
        if (entries_[index].role == Role::Package) {
            auto borrowed = objects_->Resolve<ClusterSoakPackage>(Access(), handle);
            if (!borrowed) {
                return Result<void>::Failure(borrowed.Error());
            }

            auto pointer = borrowed.Value().Get();
            return pointer ? edit(pointer.Value()->items) : Result<void>::Failure(pointer.Error());
        }

        auto borrowed = objects_->Resolve<ClusterSoakNode>(Access(), handle);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto pointer = borrowed.Value().Get();
        return pointer ? edit(pointer.Value()->items) : Result<void>::Failure(pointer.Error());
    }

    bool AddEdge(uint32 owner, uint32 target) noexcept {
        const uint32 limit = entries_[owner].role == Role::Level ? MaxEdges : OtherEdges;
        if (entries_[owner].degree >= limit) {
            return false;
        }

        auto added = EditItems(owner, [&](ObjectRefArray<Object>& items) noexcept {
            return objects_->AddReference(Access(), entries_[owner].handle, items,
                                          entries_[target].handle);
        });
        if (!added) {
            // cutoff white 대상(Sweeping)만 거부될 수 있다.
            ++report_.rejectedWrites;
            return false;
        }

        entries_[owner].edges[entries_[owner].degree++] = target;
        ++report_.edgeWrites;
        return true;
    }

    void RemoveEdge(uint32 owner, uint32 target) noexcept {
        Entry& entry = entries_[owner];
        for (uint32 edge = 0; edge < entry.degree; ++edge) {
            if (entry.edges[edge] != target) {
                continue;
            }

            auto removed = EditItems(owner, [&](ObjectRefArray<Object>& items) noexcept {
                return objects_->RemoveReference(Access(), entry.handle, items,
                                                 entries_[target].handle);
            });
            if (!removed) {
                // cutoff white 소유자(Sweeping 중 도달하지 않은 객체)는 편집할 수 없다.
                if (removed.Error() != ErrorCode::Destroyed) {
                    Fail("RemoveReference failed", owner, removed.Error());
                }

                return;
            }

            for (uint32 next = edge + 1; next < entry.degree; ++next) {
                entry.edges[next - 1] = entry.edges[next];
            }

            --entry.degree;
            ++report_.edgeWrites;
            return;
        }
    }

    // 단일 참조 필드를 target(NoIndex면 null)으로 바꾼다(SetReference).
    void SetSingle(uint32 owner, uint32 target) noexcept {
        auto borrowed = objects_->Resolve<ClusterSoakNode>(Access(), entries_[owner].handle);
        if (!borrowed) {
            ++report_.rejectedWrites;
            return;
        }

        auto pointer = borrowed.Value().Get();
        const ObjectHandle handle = target == NoIndex ? ObjectHandle{} : entries_[target].handle;
        if (!pointer || !objects_->SetReference(Access(), entries_[owner].handle,
                                                pointer.Value()->single, handle)) {
            // cutoff white 대상(Sweeping)만 거부될 수 있다.
            ++report_.rejectedWrites;
            return;
        }

        entries_[owner].single = target;
        ++report_.edgeWrites;
    }

    // items의 at번 원소를 target으로 바꾼다(SetReferenceAt — 모델 간선 순서는 배열과 같다).
    void ReplaceEdge(uint32 owner, uint32 at, uint32 target) noexcept {
        auto replaced = EditItems(owner, [&](ObjectRefArray<Object>& items) noexcept {
            return objects_->SetReferenceAt(Access(), entries_[owner].handle, items, at,
                                            entries_[target].handle);
        });
        if (!replaced) {
            ++report_.rejectedWrites;
            return;
        }

        entries_[owner].edges[at] = target;
        ++report_.edgeWrites;
    }

    // 단일 필드 쓰기 또는 배열 원소 교체다(소유 간선 — Actor의 Component — 은 건드리지 않는다).
    void MutateSingle(Role role) noexcept {
        const uint32 owner = PickRole(role);
        const uint32 target = Pick();
        if (owner == NoIndex || target == NoIndex || !Usable(target)) {
            return;
        }

        if (rng_.Below(2) == 0) {
            SetSingle(owner, rng_.Below(4) == 0 ? NoIndex : target);
            return;
        }

        Entry& entry = entries_[owner];
        if (entry.degree == 0) {
            return;
        }

        const uint32 at = rng_.Below(entry.degree);
        if (entries_[entry.edges[at]].role != Role::Component) {
            ReplaceEdge(owner, at, target);
        }
    }

    void ClearEdges(uint32 owner) noexcept {
        auto cleared = EditItems(owner, [&](ObjectRefArray<Object>& items) noexcept {
            return objects_->ClearReferences(Access(), entries_[owner].handle, items);
        });
        if (!cleared) {
            if (cleared.Error() != ErrorCode::Destroyed) {
                Fail("ClearReferences failed", owner, cleared.Error());
            }

            return;
        }

        entries_[owner].degree = 0;
        ++report_.edgeWrites;
    }

    // StructRefArray 편집이다(Borrow를 쥐고 한다). 패키지 객체는 원소가 없다.
    template <class Edit>
    Result<void> EditSlots(uint32 index, Edit&& edit) noexcept {
        auto borrowed = objects_->Resolve<ClusterSoakNode>(Access(), entries_[index].handle);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto pointer = borrowed.Value().Get();
        return pointer ? edit(pointer.Value()->slots) : Result<void>::Failure(pointer.Error());
    }

    // 원소를 더하고 그 참조를 target으로 둔다. 참조 쓰기가 거부되면(cutoff white 대상) 원소는
    // null로 남는다.
    void AddSlot(uint32 owner, uint32 target) noexcept {
        Entry& entry = entries_[owner];
        if (entry.slotCount == MaxSlots) {
            return;
        }

        bool added = false;
        bool linked = false;
        auto edited = EditSlots(owner, [&](StructRefArray<ClusterSoakSlot>& slots) noexcept {
            auto index = objects_->AddElement(Access(), entry.handle, slots);
            if (!index) {
                return Result<void>::Failure(index.Error());
            }

            added = true;
            linked = static_cast<bool>(objects_->SetElementReference(
                Access(), entry.handle, slots, index.Value(), slots[index.Value()].target,
                entries_[target].handle));
            return Result<void>::Success();
        });
        if (!edited || !added) {
            // cutoff white 소유자(Sweeping 중 도달하지 않은 객체)는 편집할 수 없다.
            ++report_.rejectedWrites;
            return;
        }

        entry.slots[entry.slotCount++] = linked ? target : NoIndex;
        report_.edgeWrites += linked ? 2u : 1u;
        report_.rejectedWrites += linked ? 0u : 1u;
    }

    void RemoveSlot(uint32 owner, uint32 at) noexcept {
        Entry& entry = entries_[owner];
        auto removed = EditSlots(owner, [&](StructRefArray<ClusterSoakSlot>& slots) noexcept {
            return objects_->RemoveElementAt(Access(), entry.handle, slots, at);
        });
        if (!removed) {
            if (removed.Error() != ErrorCode::Destroyed) {
                Fail("RemoveElementAt failed", owner, removed.Error());
            }

            return;
        }

        for (uint32 next = at + 1; next < entry.slotCount; ++next) {
            entry.slots[next - 1] = entry.slots[next];
        }

        --entry.slotCount;
        ++report_.edgeWrites;
    }

    void ClearSlots(uint32 owner) noexcept {
        auto cleared = EditSlots(owner, [&](StructRefArray<ClusterSoakSlot>& slots) noexcept {
            return objects_->ClearElements(Access(), entries_[owner].handle, slots);
        });
        if (!cleared) {
            if (cleared.Error() != ErrorCode::Destroyed) {
                Fail("ClearElements failed", owner, cleared.Error());
            }

            return;
        }

        entries_[owner].slotCount = 0;
        ++report_.edgeWrites;
    }

    // 원소 추가·제거·비우기다(소유자 규칙은 간선 추가와 같다 — World형 Actor는 같은 Level Actor만
    // 가리킨다).
    void MutateSlots(Role role) noexcept {
        const uint32 owner = PickRole(role);
        if (owner == NoIndex) {
            return;
        }

        const uint32 roll = rng_.Below(6);
        if (roll < 3) {
            const uint32 target = Pick();
            if (target != NoIndex && Usable(target) &&
                (role != Role::Actor || (entries_[target].role == Role::Actor &&
                                         entries_[target].owner == entries_[owner].owner))) {
                AddSlot(owner, target);
            }
        } else if (roll < 5) {
            if (entries_[owner].slotCount != 0) {
                RemoveSlot(owner, rng_.Below(entries_[owner].slotCount));
            }
        } else {
            ClearSlots(owner);
        }
    }

    uint32 Append(ObjectHandle handle, Role role, uint32 outer, uint32 cluster,
                  uint32 owner) noexcept {
        Entry entry;
        entry.handle = handle;
        entry.role = role;
        entry.outer = outer;
        entry.cluster = cluster;
        entry.owner = owner;
        if (!entries_.TryPushBack(Move(entry))) {
            platform::Fatal("Reserved cluster soak model exhausted");
        }

        ++report_.created;
        return static_cast<uint32>(entries_.Size() - 1);
    }

    ObjectHandle Create(ConstructionContext& transaction, TypeId type,
                        ObjectHandle outer) noexcept {
        char name[16] = {'c'};
        uint32 length = 1;
        for (uint32 value = ++names_; value != 0; value /= 10) {
            name[length++] = static_cast<char>('0' + value % 10);
        }

        auto created =
            type == ClusterSoakPackageType
                ? transaction.Create<ClusterSoakPackage>(type, outer, StringView{})
                : transaction.Create<ClusterSoakNode>(
                      type, outer, outer.IsNull() ? StringView{} : StringView(name, length));
        return created ? created.Value() : ObjectHandle{};
    }

    // ---- World형 ----

    void CreateLevel() noexcept {
        // 모델 크기 상한을 지킨다(상한 없이 Level을 만들면 모델이 끝없이 커진다).
        if (entries_.Size() >= options_.maxObjects) {
            return;
        }

        ConstructionContext transaction(*objects_, Access());
        const ObjectHandle level = Create(transaction, ClusterSoakNodeType, {});
        if (level.IsNull() || !transaction.SetClusterRoot(level)) {
            Fail("Level creation failed", NoIndex, ErrorCode::InvalidArgument);
            return;
        }

        const ObjectHandle roots[] = {level};
        RootRef<Object> out[1];
        if (!transaction.Commit(roots, out)) {
            Fail("Level commit failed", NoIndex, ErrorCode::InvalidArgument);
            return;
        }

        const uint32 index = Append(level, Role::Level, NoIndex, NoIndex, NoIndex);
        ++levels_;
        entries_[index].cluster = index;
        entries_[index].clusterRoot = true;
        entries_[index].held = Move(out[0]);
    }

    // Actor와 Component 0–2개를 Level 클러스터에 넣는다(Actor의 outer는 가끔 Level 밖 객체 — 처리된
    // 클러스터에 밖 outer를 가진 소속 가입).
    void SpawnActor() noexcept {
        const uint32 level = PickRole(Role::Level);
        if (level == NoIndex || entries_.Size() + 3 > options_.maxObjects ||
            entries_[level].degree == MaxEdges) {
            return;
        }

        const uint32 outerPick = PickRole(Role::Normal);
        const uint32 outer = outerPick != NoIndex && rng_.Below(3) == 0 ? outerPick : NoIndex;
        ConstructionContext transaction(*objects_, Access());
        const ObjectHandle actor =
            Create(transaction, ClusterSoakNodeType,
                   outer == NoIndex ? ObjectHandle{} : entries_[outer].handle);
        const ObjectHandle levelHandle = entries_[level].handle;
        // 밖 outer가 cutoff white(Sweeping 중 도달하지 않은 객체)면 생성이 거부된다(StaleHandle).
        if (actor.IsNull() && outer != NoIndex) {
            ++report_.rejectedWrites;
            return;
        }

        if (actor.IsNull() || !transaction.JoinCluster(actor, levelHandle)) {
            Fail("Actor creation failed", level, ErrorCode::InvalidArgument);
            return;
        }

        ObjectHandle components[2];
        const uint32 count = rng_.Below(3);
        for (uint32 index = 0; index < count; ++index) {
            components[index] = Create(transaction, ClusterSoakNodeType, {});
            if (components[index].IsNull() ||
                !transaction.JoinCluster(components[index], levelHandle)) {
                Fail("Component creation failed", level, ErrorCode::InvalidArgument);
                return;
            }
        }

        auto pointer = transaction.Get<ClusterSoakNode>(actor);
        if (!pointer ||
            !transaction.AssignReferences(actor, pointer.Value()->items,
                                          Span<const ObjectHandle>(components, count)) ||
            !transaction.Commit(Span<const ObjectHandle>{}, Span<RootRef<Object>>{})) {
            Fail("Actor commit failed", level, ErrorCode::InvalidArgument);
            return;
        }

        const uint32 actorIndex = Append(actor, Role::Actor, outer, level, level);
        for (uint32 index = 0; index < count; ++index) {
            const uint32 component =
                Append(components[index], Role::Component, NoIndex, level, actorIndex);
            entries_[actorIndex].edges[entries_[actorIndex].degree++] = component;
        }

        // Level이 Actor를 소유한다(World의 Level 배열).
        if (!AddEdge(level, actorIndex)) {
            Fail("Level could not own a new actor", actorIndex, ErrorCode::InvalidArgument);
        }
    }

    // Actor의 파괴(World DestroyActorNow와 같은 순서): Component 파괴·간선 비우기·Level 간선 제거·
    // Actor 파괴.
    void DestroyActor(uint32 actor) noexcept {
        Entry& entry = entries_[actor];
        for (uint32 index = 0; index < entries_.Size(); ++index) {
            if (entries_[index].role == Role::Component && entries_[index].owner == actor &&
                !entries_[index].dead && !entries_[index].destroyed) {
                Destroy(index);
            }
        }

        if (!entry.destroyed) {
            ClearEdges(actor);
        }

        if (Usable(entry.owner)) {
            RemoveEdge(entry.owner, actor);
        }

        if (!entry.destroyed) {
            Destroy(actor);
        }
    }

    void Destroy(uint32 index) noexcept {
        auto requested = objects_->RequestDestroy(Access(), entries_[index].handle);
        if (!requested) {
            // cutoff white(이번 cycle에 회수될 객체 — 그 클러스터는 cutoff에 해체됐다)는 건너뛴다.
            if (requested.Error() != ErrorCode::Destroyed) {
                Fail("RequestDestroy failed", index, requested.Error());
            }

            return;
        }

        Entry& entry = entries_[index];
        entry.destroyed = true;
        ++report_.destroys;
        if (entry.clusterRoot) {
            Dissolve(index);
        } else {
            entry.cluster = NoIndex;
        }
    }

    void Dissolve(uint32 root) noexcept {
        entries_[root].clusterRoot = false;
        for (Entry& entry : entries_) {
            if (entry.cluster == root) {
                entry.cluster = NoIndex;
            }
        }
    }

    void DestroyLevel(uint32 level) noexcept {
        for (uint32 index = 0; index < entries_.Size(); ++index) {
            Entry& actor = entries_[index];
            if (actor.role == Role::Actor && actor.owner == level && !actor.dead &&
                (!actor.destroyed || actor.bypassed)) {
                actor.bypassed = false;
                DestroyActor(index);
            }
        }

        ClearEdges(level);
        Destroy(level);
        entries_[level].held.Reset();
        --levels_;
    }

    // World API 밖 Actor 파괴는 다음 Begin 전에 정리한다(World::RepairBypassedActors와 같다).
    void RepairBypassed() noexcept {
        if (options_.packages) {
            return;
        }

        for (uint32 index = 0; index < entries_.Size() && report_.failure == nullptr; ++index) {
            Entry& entry = entries_[index];
            if (entry.role == Role::Actor && entry.bypassed) {
                entry.bypassed = false;
                DestroyActor(index);
            }
        }
    }

    void MutateWorld(uint32 roll) noexcept {
        if (roll < 2 || levels_ == 0) {
            if (levels_ < 2 || rng_.Below(6) == 0) {
                CreateLevel();
            }
        } else if (roll < 6) {
            SpawnActor();
        } else if (roll < 8) {
            // 같은 Level Actor 사이 참조, 또는 일반 객체 → 아무 생존 객체.
            const uint32 owner = PickRole(rng_.Below(2) == 0 ? Role::Actor : Role::Normal);
            const uint32 target = Pick();
            if (owner != NoIndex && target != NoIndex && Usable(target) &&
                (entries_[owner].role == Role::Normal ||
                 (entries_[target].role == Role::Actor &&
                  entries_[target].owner == entries_[owner].owner))) {
                AddEdge(owner, target);
            }
        } else if (roll < 10) {
            const uint32 owner = PickRole(rng_.Below(2) == 0 ? Role::Actor : Role::Normal);
            if (owner != NoIndex && entries_[owner].degree != 0) {
                const uint32 target = entries_[owner].edges[rng_.Below(entries_[owner].degree)];
                // Actor의 Component 소유 간선은 Component 파괴로만 뗀다.
                if (entries_[target].role != Role::Component) {
                    RemoveEdge(owner, target);
                }
            }
        } else if (roll < 11) {
            const uint32 actor = PickRole(Role::Actor);
            if (actor != NoIndex) {
                DestroyActor(actor);
            }
        } else if (roll < 12) {
            const uint32 component = PickRole(Role::Component);
            if (component != NoIndex) {
                const uint32 actor = entries_[component].owner;
                if (Usable(actor)) {
                    RemoveEdge(actor, component);
                }

                Destroy(component);
            }
        } else if (roll < 13) {
            // World API 밖 파괴: Actor만 RequestDestroy한다(Component는 다음 Begin 전에 정리).
            const uint32 actor = PickRole(Role::Actor);
            if (actor != NoIndex) {
                Destroy(actor);
                entries_[actor].bypassed = entries_[actor].destroyed;
            }
        } else if (roll < 14) {
            const uint32 level = PickRole(Role::Level);
            if (level != NoIndex && rng_.Below(4) == 0) {
                DestroyLevel(level);
            }
        } else {
            MutateNormal();
        }
    }

    // ---- 패키지형 ----

    void CreatePackage() noexcept {
        if (entries_.Size() + 4 > options_.maxObjects) {
            return;
        }

        ConstructionContext transaction(*objects_, Access());
        const ObjectHandle package = Create(transaction, ClusterSoakPackageType, {});
        if (package.IsNull()) {
            Fail("Package creation failed", NoIndex, ErrorCode::InvalidArgument);
            return;
        }

        ObjectHandle inners[3];
        uint32 outers[3];
        const uint32 count = 1 + rng_.Below(3);
        for (uint32 index = 0; index < count; ++index) {
            // outer 사슬: 패키지 아래 또는 앞 inner 아래.
            outers[index] = index == 0 || rng_.Below(2) == 0 ? NoIndex : index - 1;
            const ObjectHandle outer = outers[index] == NoIndex ? package : inners[outers[index]];
            inners[index] = Create(transaction, ClusterSoakNodeType, outer);
            if (inners[index].IsNull()) {
                Fail("Package inner creation failed", NoIndex, ErrorCode::InvalidArgument);
                return;
            }
        }

        const ObjectHandle roots[] = {package};
        RootRef<Object> out[1];
        if (!transaction.Commit(roots, out)) {
            Fail("Package commit failed", NoIndex, ErrorCode::InvalidArgument);
            return;
        }

        const uint32 packageIndex = Append(package, Role::Package, NoIndex, NoIndex, NoIndex);
        entries_[packageIndex].cluster = packageIndex;
        entries_[packageIndex].clusterRoot = true;
        entries_[packageIndex].held = Move(out[0]);
        uint32 placed[3];
        for (uint32 index = 0; index < count; ++index) {
            const uint32 outer = outers[index] == NoIndex ? packageIndex : placed[outers[index]];
            placed[index] = Append(inners[index], Role::Inner, outer, packageIndex, NoIndex);
        }
    }

    // 기존 패키지 아래 새 객체를 만든다. 패키지가 클러스터면 가입하고, 안전장치가 해체한
    // 패키지면 일반 객체다(Registry의 결과를 모델 소속과 대조한다).
    void CreateLateInner() noexcept {
        const uint32 package = PickRole(Role::Package);
        if (package == NoIndex || entries_.Size() >= options_.maxObjects) {
            return;
        }

        ConstructionContext transaction(*objects_, Access());
        const ObjectHandle inner =
            Create(transaction, ClusterSoakNodeType, entries_[package].handle);
        if (inner.IsNull() ||
            !transaction.Commit(Span<const ObjectHandle>{}, Span<RootRef<Object>>{})) {
            // cutoff white 패키지(Sweeping 중 도달하지 않은 패키지) 아래에는 만들 수 없다.
            ++report_.rejectedWrites;
            return;
        }

        const uint32 cluster = entries_[package].clusterRoot ? package : NoIndex;
        const uint32 index = Append(inner, Role::Inner, package, cluster, NoIndex);
        auto member = objects_->InspectClusterMembership(Access(), inner);
        if (!member || member.Value() != (cluster != NoIndex)) {
            Fail("Late package object membership diverged from the model", index,
                 member ? ErrorCode::None : member.Error());
        }
    }

    // 안전장치가 해체한 패키지 클러스터를 모델에 옮긴다(cycle 끝 — cutoff·정지형 sweep 뒤).
    void SyncFlattened() noexcept {
        if (!options_.packages) {
            return;
        }

        for (uint32 index = 0; index < entries_.Size(); ++index) {
            const Entry& entry = entries_[index];
            if (!entry.clusterRoot || !Usable(index)) {
                continue;
            }

            auto member = objects_->InspectClusterMembership(Access(), entry.handle);
            if (member && !member.Value()) {
                Dissolve(index);
                ++report_.flattened;
            }
        }
    }

    // index의 부분 트리(outer 사슬이 index를 지나는 Discard되지 않은 객체)를 Discard로 표시한다.
    void MarkDiscarded(uint32 index) noexcept {
        entries_[index].discarded = true;
        entries_[index].destroyed = true;
        if (entries_[index].clusterRoot) {
            Dissolve(index);
        } else {
            entries_[index].cluster = NoIndex;
        }

        for (uint32 child = 0; child < entries_.Size(); ++child) {
            if (entries_[child].outer == index && !entries_[child].discarded &&
                !entries_[child].dead) {
                MarkDiscarded(child);
            }
        }
    }

    void MutatePackages(uint32 roll) noexcept {
        if (roll < 3) {
            CreatePackage();
        } else if (roll < 8) {
            // 패키지 객체의 간선도 둔다(경계가 거의 전부인 패키지 — 안전장치 경로).
            const uint32 owner = PickRole(rng_.Below(3) == 0 ? Role::Package : Role::Inner);
            const uint32 target = Pick();
            if (owner != NoIndex && target != NoIndex && Usable(target)) {
                AddEdge(owner, target);
            }
        } else if (roll < 10) {
            const uint32 owner = PickRole(Role::Inner);
            if (owner != NoIndex && entries_[owner].degree != 0) {
                RemoveEdge(owner, entries_[owner].edges[rng_.Below(entries_[owner].degree)]);
            }
        } else if (roll < 11) {
            const uint32 owner = PickRole(Role::Inner);
            if (owner != NoIndex) {
                ClearEdges(owner);
            }
        } else if (roll < 13) {
            // 부분 트리 Discard.
            const uint32 inner = PickRole(Role::Inner);
            if (inner != NoIndex) {
                auto discarded = objects_->Discard(Access(), entries_[inner].handle);
                if (!discarded) {
                    if (discarded.Error() != ErrorCode::Destroyed) {
                        Fail("Discard failed", inner, discarded.Error());
                    }

                    return;
                }

                ++report_.discards;
                MarkDiscarded(inner);
            }
        } else if (roll < 14) {
            // 패키지 Discard와 루트 반환.
            const uint32 package = PickRole(Role::Package);
            if (package != NoIndex) {
                auto discarded = objects_->Discard(Access(), entries_[package].handle);
                if (!discarded) {
                    if (discarded.Error() != ErrorCode::Destroyed) {
                        Fail("Package Discard failed", package, discarded.Error());
                    }

                    return;
                }

                ++report_.discards;
                MarkDiscarded(package);
                entries_[package].held.Reset();
            }
        } else if (roll < 15 && rng_.Below(2) == 0) {
            CreateLateInner();
        } else if (roll < 15) {
            // 버그 경로: Discard 없이 루트를 놓는다(표식되지 않은 클러스터를 cutoff에 해체한다).
            const uint32 package = PickRole(Role::Package);
            if (package != NoIndex && rng_.Below(4) == 0) {
                entries_[package].held.Reset();
            }
        } else {
            // 패키지 루트를 다시 쥐거나 놓는다.
            const uint32 package = PickRole(Role::Package);
            if (package != NoIndex && entries_[package].held.Handle().IsNull()) {
                auto pinned = objects_->TryPin<Object>(Access(), entries_[package].handle);
                if (pinned) {
                    entries_[package].held = Move(pinned.Value());
                }
            }
        }
    }

    // ---- 공통 ----

    void MutateNormal() noexcept {
        if (options_.packages) {
            return;
        }

        const uint32 roll = rng_.Below(4);
        if (roll == 0 && entries_.Size() < options_.maxObjects) {
            ConstructionContext transaction(*objects_, Access());
            const ObjectHandle created = Create(transaction, ClusterSoakNodeType, {});
            const bool hold = rng_.Below(2) == 0;
            const ObjectHandle roots[] = {created};
            RootRef<Object> out[1];
            if (created.IsNull() ||
                !transaction.Commit(Span<const ObjectHandle>(roots, hold ? 1u : 0u),
                                    Span<RootRef<Object>>(out, hold ? 1u : 0u))) {
                Fail("Normal object creation failed", NoIndex, ErrorCode::InvalidArgument);
                return;
            }

            const uint32 index = Append(created, Role::Normal, NoIndex, NoIndex, NoIndex);
            if (hold) {
                entries_[index].held = Move(out[0]);
            }
        } else if (roll == 1) {
            const uint32 index = PickRole(Role::Normal);
            if (index != NoIndex) {
                if (entries_[index].held.Handle().IsNull()) {
                    auto pinned = objects_->TryPin<Object>(Access(), entries_[index].handle);
                    if (pinned) {
                        entries_[index].held = Move(pinned.Value());
                    }
                } else {
                    entries_[index].held.Reset();
                }
            }
        } else if (roll == 2) {
            const uint32 index = PickRole(Role::Normal);
            if (index != NoIndex) {
                Destroy(index);
            }
        } else {
            const uint32 index = PickRole(Role::Normal);
            if (index != NoIndex) {
                ClearEdges(index);
            }
        }
    }

    void Mutate() noexcept {
        const uint32 roll = rng_.Below(22);
        if (roll >= 20) {
            MutateSlots(options_.packages ? Role::Inner
                                          : (rng_.Below(2) == 0 ? Role::Actor : Role::Normal));
        } else if (roll >= 18) {
            MutateSingle(options_.packages ? Role::Inner
                                           : (rng_.Below(2) == 0 ? Role::Actor : Role::Normal));
        } else if (options_.packages) {
            MutatePackages(roll);
        } else {
            MutateWorld(roll);
        }
    }

    // oracle: 루트에서 간선·inner → outer·패키지 고리(Discard되지 않은 자식)·루트 → 소속으로 닿는
    // 객체.
    void Reach() noexcept {
        // 루트 → 소속, 패키지 고리(outer → Discard되지 않은 inner) 인접 목록을 먼저 만든다(O(객체
        // 수) — 도달한 루트마다 전체를 훑으면 slice마다 O(루트 × 객체)라 긴 실행이 끝나지 않는다).
        const uint32 count = static_cast<uint32>(entries_.Size());
        if (!memberHead_.TryResize(count) || !memberNext_.TryResize(count) ||
            !childHead_.TryResize(count) || !childNext_.TryResize(count)) {
            platform::Fatal("Cluster soak adjacency allocation failed");
        }

        for (uint32 index = 0; index < count; ++index) {
            memberHead_[index] = NoIndex;
            childHead_[index] = NoIndex;
            entries_[index].reachable = false;
        }

        for (uint32 index = 0; index < count; ++index) {
            const Entry& entry = entries_[index];
            if (entry.dead) {
                continue;
            }

            if (entry.cluster != NoIndex && entry.cluster != index) {
                memberNext_[index] = memberHead_[entry.cluster];
                memberHead_[entry.cluster] = index;
            }

            if (options_.packages && entry.outer != NoIndex && !entry.discarded) {
                childNext_[index] = childHead_[entry.outer];
                childHead_[entry.outer] = index;
            }
        }

        stack_.Clear();
        auto visit = [&](uint32 next) noexcept {
            if (next != NoIndex && !entries_[next].reachable) {
                entries_[next].reachable = true;
                if (!stack_.TryPushBack(next)) {
                    platform::Fatal("Cluster soak stack allocation failed");
                }
            }
        };
        for (uint32 index = 0; index < count; ++index) {
            if (!entries_[index].dead && !entries_[index].held.Handle().IsNull()) {
                visit(index);
            }
        }

        while (!stack_.IsEmpty()) {
            const uint32 index = stack_[stack_.Size() - 1];
            (void)stack_.TryPopBack();
            const Entry& entry = entries_[index];
            visit(entry.outer);
            visit(entry.single);
            for (uint32 edge = 0; edge < entry.degree; ++edge) {
                visit(entry.edges[edge]);
            }

            for (uint32 slot = 0; slot < entry.slotCount; ++slot) {
                visit(entry.slots[slot]);
            }

            if (entry.clusterRoot) {
                for (uint32 at = memberHead_[index]; at != NoIndex; at = memberNext_[at]) {
                    visit(at);
                }
            }

            for (uint32 at = childHead_[index]; at != NoIndex; at = childNext_[at]) {
                visit(at);
            }
        }
    }

    // 회수된 객체를 dead로 옮기고 oracle에서 닿지 않았는지 본다.
    void CheckSafety() noexcept {
        SyncFlattened();
        Reach();
        for (uint32 index = 0; index < entries_.Size(); ++index) {
            Entry& entry = entries_[index];
            if (entry.dead || Exists(index)) {
                continue;
            }

            if (entry.reachable) {
                Fail("Reachable object was reclaimed", index, ErrorCode::None);
                return;
            }

            entry.dead = true;
            entry.degree = 0;
            entry.single = NoIndex;
            entry.slotCount = 0;
            entry.held.Reset();
            ++report_.reclaimed;
            if (entry.clusterRoot) {
                Dissolve(index);
            }
        }
    }

    // 정지형 수집 뒤 생존 집합이 oracle과 같다.
    void CheckExact() noexcept {
        RepairBypassed();
        auto collected = objects_->Collect(Access(), options_.scheduler);
        if (!collected) {
            Fail("Collect failed", NoIndex, collected.Error());
            return;
        }

        ++report_.collects;
        CheckSafety();
        Reach();
        uint32 clusters = 0;
        for (uint32 index = 0; index < entries_.Size(); ++index) {
            const Entry& entry = entries_[index];
            if (entry.dead) {
                continue;
            }

            if (!entry.reachable) {
                Fail("Unreachable object survived a stop-the-world collection", index,
                     ErrorCode::None);
                return;
            }

            clusters += entry.clusterRoot ? 1u : 0u;
        }

        if (objects_->Stats().clusters != clusters) {
            Fail("Cluster count diverged from the model", NoIndex, ErrorCode::None);
            return;
        }

        ++report_.verifications;
    }

    const GcClusterSoakOptions& options_;
    GcClusterSoakReport& report_;
    Rng rng_;
    ObjectRegistry* objects_ = nullptr;
    Array<Entry> entries_;
    Array<uint32> stack_;
    // oracle 인접 목록(Reach가 매번 다시 만든다): 클러스터 루트 → 소속, outer → 고리 자식.
    Array<uint32> memberHead_;
    Array<uint32> memberNext_;
    Array<uint32> childHead_;
    Array<uint32> childNext_;
    uint32 names_ = 0;
    // 살아 있는(파괴하지 않은) Level 수다.
    uint32 levels_ = 0;
};

} // namespace

bool RunGcClusterSoak(Allocator& allocator, const GcClusterSoakOptions& options,
                      GcClusterSoakReport& report) noexcept {
    report = {};
    bool passed = false;
    {
        TypeRegistry types(allocator);
        RuntimeTypeOps node = MakeRuntimeTypeOps<ClusterSoakNode>();
        node.parallelTraceSafe = true;
        node.completeFieldLayout = true;
        RuntimeTypeOps package = MakeRuntimeTypeOps<ClusterSoakPackage>();
        package.parallelTraceSafe = true;
        package.completeFieldLayout = true;
        static FieldSchema fields[1];
        fields[0].id = ClusterSoakSlotTargetId;
        fields[0].kind = FieldKind::ObjectReference;
        fields[0].flags = static_cast<uint32>(FieldFlags::Transient);
        fields[0].name = Span<const char>("target", 6);
        fields[0].value = ValueType{ValueKind::ObjectReference, {}};
        static const RuntimeStructFieldOps fieldOps[] = {{ClusterSoakSlotTargetId}};
        RuntimeStructOps slot = MakeRuntimeStructOps<ClusterSoakSlot>();
        slot.trace = [](const void* value, TraceVisitor& visitor) noexcept {
            GeneratedType<ClusterSoakSlot>::Trace(*static_cast<const ClusterSoakSlot*>(value),
                                                  visitor);
        };
        slot.fields = Span<const RuntimeStructFieldOps>(fieldOps, 1);
        if (!types.RegisterStruct({ClusterSoakSlotType,
                                   1,
                                   Span<const FieldSchema>(fields, 1),
                                   {"ClusterSoakSlot", 15}},
                                  Move(slot)) ||
            !types.Register(TypeSchema{ClusterSoakNodeType, 1, {}, {}}, Move(node)) ||
            !types.Register(TypeSchema{ClusterSoakPackageType, 1, {}, {}}, Move(package))) {
            report.failure = "type registration failed";
            return false;
        }

        {
            RegistryOptions registryOptions;
            registryOptions.verifyClusterLedger = true;
            registryOptions.assertRetiredWithoutDiscard = false;
            if (options.packages) {
                registryOptions.policy.packageNativeType = NativeTypeKey<ClusterSoakPackage>();
            } else {
                registryOptions.clusters = true;
                registryOptions.verifyClusterOwnership = true;
            }

            ObjectRegistry objects(allocator, types, Guid{0x4743534b, options.seed},
                                   registryOptions);
            ClusterSoak soak(allocator, options, report);
            passed = soak.Run(objects);
            soak.ReleaseAll();
            if (!objects.Shutdown(objects.OwnerAccess()) && report.failure == nullptr) {
                report.failure = "Registry shutdown failed";
                passed = false;
            }
        }

        if (!types.Shutdown() && report.failure == nullptr) {
            report.failure = "type registry shutdown failed";
            passed = false;
        }
    }

    return passed;
}

} // namespace rocks
