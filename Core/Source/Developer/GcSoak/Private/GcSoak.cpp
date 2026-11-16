#include <GcSoak/GcSoak.h>

#include <Core/Containers/Array.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>

// GcSoak.h의 시뮬레이터 구현이다. 모델은 객체 번호(표 칸) 단위로 간선(first·second·items)·Outer·
// 루트·보유 계수를 들고, 실제 Registry 결과를 그 모델로 예측한 결과와 대조한다.
namespace rocks {

namespace {

constexpr uint32 None = NumericLimits<uint32>::Max();
constexpr uint64 NoCycle = NumericLimits<uint64>::Max();
constexpr uint32 MaxItems = 8;
constexpr uint32 ProbeCount = 8;
constexpr TypeId SoakNodeType{0x47436e6f64655331ULL, 1};
constexpr TypeId SoakLeafType{0x47436c6561665331ULL, 1};
constexpr TypeId SoakProbeType{0x474370726f626531ULL, 1};

// 소멸자 수다. 회수(소멸자 실행)는 소유 스레드의 수집·DrainRetired에서만 일어난다.
uint64 destroyedObjects = 0;

// 다른 스레드에서 실행할 반환 하나다(소유 스레드가 채우고 helper가 비운다).
enum class ReleaseKind : uint8 {
    None,
    Backend,
    Root,
    EncodePin,
    Quit
};

// helper 스레드와의 명시적 hand-off 통로다. 소유 스레드가 반환할 보유를 옮겨 두고 request를 올린 뒤
// done을 기다린다. 표식 slice 안의 반환은 탐침 객체의 Trace가 같은 hand-off를 한다(armed — 소유
// 스레드에서만 읽고 쓴다: 탐침 타입은 parallelTraceSafe가 아니라 소유 스레드에서 trace된다).
struct ReleaseChannel {
    platform::Semaphore request{0, 1};
    platform::Semaphore done{0, 1};
    platform::Thread thread;
    ReleaseKind kind = ReleaseKind::None;
    BackendLease lease;
    RootRef<Object> root;
    EncodePin pin;
    bool armed = false;
    uint64 inSlice = 0;
    uint64 outside = 0;

    static void Run(void* raw) noexcept {
        auto& channel = *static_cast<ReleaseChannel*>(raw);
        for (;;) {
            if (channel.request.Wait() != platform::WaitResult::Signaled) {
                platform::Fatal("GcSoak release channel wait failed");
            }

            if (channel.kind == ReleaseKind::Quit) {
                channel.kind = ReleaseKind::None;
                channel.done.Release();
                return;
            }

            channel.lease.Reset();
            channel.root.Reset();
            channel.pin.Reset();
            channel.kind = ReleaseKind::None;
            channel.done.Release();
        }
    }

    void Handoff() noexcept {
        if (!request.Release() || done.Wait() != platform::WaitResult::Signaled) {
            platform::Fatal("GcSoak release channel hand-off failed");
        }
    }

    // 탐침 Trace에서 부른다: 준비된 반환이 있으면 slice 안에서 helper가 실행하게 한다.
    void FromTrace() noexcept {
        if (armed) {
            armed = false;
            ++inSlice;
            Handoff();
        }
    }
};

ReleaseChannel* activeChannel = nullptr;

class SoakNode final : public Object {
public:
    explicit SoakNode(Allocator& allocator) noexcept : items(allocator) {
    }

    ~SoakNode() noexcept {
        ++destroyedObjects;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(first);
        visitor.Visit(second);
        items.Trace(visitor);
    }

    ObjectRef<Object> first;
    ObjectRef<Object> second;
    ObjectRefArray<Object> items;
};

class SoakLeaf final : public Object {
public:
    SoakLeaf() noexcept = default;

    ~SoakLeaf() noexcept {
        ++destroyedObjects;
    }
};

// 강한 참조가 없는 수동 Trace 타입이다. trace될 때 준비된 cross-thread 반환을 실행한다.
class SoakProbe final : public Object {
public:
    SoakProbe() noexcept = default;

    ~SoakProbe() noexcept {
        ++destroyedObjects;
    }

    void Trace(TraceVisitor&) const noexcept {
        if (activeChannel != nullptr) {
            activeChannel->FromTrace();
        }
    }
};

enum class Kind : uint8 {
    Node,
    Leaf,
    Probe
};

// Registry가 알려 준 객체 상태다(InspectLifecycle). Stale은 회수(또는 슬롯 재사용)됐다는 뜻이다.
enum class Status : uint8 {
    Alive,
    DestroyRequested,
    Unreachable,
    Retired,
    Stale
};

struct Entry {
    ObjectHandle handle;
    uint32 outer = None;
    uint32 first = None;
    uint32 second = None;
    uint32 items[MaxItems] = {};
    uint32 itemCount = 0;
    uint32 roots = 0;
    uint32 holds = 0;
    uint32 pins = 0;
    // 모델에서 도달하지 않고 보유가 없다고 처음 관찰했을 때의 cyclesBegun이다(NoCycle이면 아니다).
    uint64 deadSince = NoCycle;
    // live 목록에서의 위치다.
    uint32 position = None;
    Kind kind = Kind::Node;
    bool used = false;
    bool destroyRequested = false;
    // Discard로 고리에서 떨어졌다(Registry의 discarded — 다시 Discard하면 무동작 성공).
    bool discarded = false;
};

struct RootHold {
    uint32 entry;
    RootRef<Object> root;
};

struct LeaseHold {
    uint32 entry;
    BackendLease lease;
};

struct PinHold {
    uint32 entry;
    EncodePin pin;
};

struct Rng {
    uint64 state;

    uint64 Next() noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545f4914f6cdd1dULL;
    }

    uint32 Below(uint32 bound) noexcept {
        return bound == 0 ? 0 : static_cast<uint32>(Next() % bound);
    }

    bool Chance(uint32 percent) noexcept {
        return Below(100) < percent;
    }
};

class Soak {
public:
    Soak(Allocator& allocator, const GcSoakOptions& options, GcSoakReport& report,
         ObjectRegistry& registry) noexcept
        : options_(options), report_(report), registry_(registry), access_(registry.OwnerAccess()),
          entries_(allocator), live_(allocator), roots_(allocator), leases_(allocator),
          pins_(allocator), reach_(allocator), queue_(allocator), status_(allocator),
          rng_{options.seed * 0x9e3779b97f4a7c15ULL + 1} {
    }

    bool Initialize() noexcept {
        const uint32 capacity = options_.maxObjects + ProbeCount;
        if (!entries_.TryResize(capacity) || !live_.TryReserve(capacity) ||
            !reach_.TryResize(capacity) || !queue_.TryReserve(capacity) ||
            !status_.TryResize(capacity)) {
            return Fail("model allocation failed", None, ErrorCode::OutOfMemory);
        }

        if (channel_.thread.Start(&ReleaseChannel::Run, &channel_) !=
            platform::ThreadStatus::Success) {
            return Fail("release helper thread did not start", None, ErrorCode::None);
        }

        activeChannel = &channel_;
        threadStarted_ = true;
        // 탐침은 끝까지 루트로 둔다(cycle마다 trace되어 slice 안 반환 기회를 만든다).
        for (uint32 index = 0; index < ProbeCount; ++index) {
            if (!CreateObject(Kind::Probe, None, true)) {
                return false;
            }
        }

        return true;
    }

    bool Failed() const noexcept {
        return report_.failure != nullptr;
    }

    void Tick() noexcept {
        const uint32 operations = 2 + rng_.Below(8);
        for (uint32 index = 0; index < operations && !Failed(); ++index) {
            Mutate();
        }

        if (!Failed()) {
            CollectStep();
        }

        if (!Failed() && (tick_ % options_.verifyInterval == 0 || verifyNow_)) {
            verifyNow_ = false;
            Verify();
        }

        ++tick_;
        report_.ticks = tick_;
    }

    // 모든 보유를 놓고 수집해 모두 회수됐는지 본 뒤 Shutdown한다. 실패 뒤에도 정리를 시도한다.
    void Finish() noexcept {
        const bool failedBefore = Failed();
        roots_.Clear();
        leases_.Clear();
        pins_.Clear();
        for (uint32 index = 0; index < entries_.Size(); ++index) {
            entries_[index].roots = 0;
            entries_[index].holds = 0;
            entries_[index].pins = 0;
        }

        StopChannel();
        if (registry_.CollectionPhase() == GcPhase::Marking) {
            (void)registry_.CancelCollection(access_);
        }

        GcBudget unlimited{NumericLimits<uint32>::Max(), 0};
        for (uint32 guard = 0; guard < 4 && registry_.CollectionPhase() != GcPhase::Idle; ++guard) {
            if (!registry_.StepCollection(access_, unlimited, options_.scheduler) &&
                !failedBefore) {
                Fail("final StepCollection failed", None, ErrorCode::None);
            }
        }

        for (uint32 round = 0; round < 2; ++round) {
            auto collected = registry_.Collect(access_);
            if (!collected && !failedBefore) {
                Fail("final Collect failed", None, collected.Error());
            }
        }

        if (!failedBefore) {
            // 모든 객체가 회수되어야 한다(남은 보유가 없다).
            for (uint32 position = 0; position < live_.Size(); ++position) {
                const uint32 index = live_[position];
                if (StatusOf(index) != Status::Stale) {
                    Fail("object survived after all holds were released", index, ErrorCode::None);
                    break;
                }
            }

            const ObjectStats stats = registry_.Stats();
            if (stats.liveObjects != 0 || stats.destroyRequested != 0 ||
                stats.retiredObjects != 0 || stats.reservedObjects != 0 || stats.roots != 0 ||
                stats.borrows != 0 || stats.backendHolds != 0 || stats.encodePins != 0) {
                Fail("registry statistics are not zero after the final collection", None,
                     ErrorCode::None);
            }

            if (destroyedObjects != report_.created) {
                Fail("destructor count differs from the created object count", None,
                     ErrorCode::None);
            }
        }

        auto stopped = registry_.Shutdown(access_);
        if (!stopped) {
            if (failedBefore) {
                // 망가진 상태라 Registry 소멸자가 진단 없이 멈춘다. 첫 위반을 먼저 알린다.
                platform::Fatal(report_.failure);
            }

            Fail("registry shutdown failed", None, stopped.Error());
        }
    }

private:
    bool Fail(const char* message, uint32 object, ErrorCode error) noexcept {
        if (report_.failure == nullptr) {
            report_.failure = message;
            report_.failureTick = tick_;
            report_.failureObject = object;
            report_.failureError = error;
        }

        return false;
    }

    static ErrorCode ErrorOf(const Result<void>& result) noexcept {
        return result ? ErrorCode::None : result.Error();
    }

    void StopChannel() noexcept {
        if (!threadStarted_) {
            return;
        }

        if (channel_.armed) {
            channel_.armed = false;
            channel_.Handoff();
        }

        channel_.kind = ReleaseKind::Quit;
        channel_.Handoff();
        if (channel_.thread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("GcSoak release helper did not join");
        }

        activeChannel = nullptr;
        threadStarted_ = false;
    }

    Status StatusOf(uint32 index) noexcept {
        auto lifecycle = registry_.InspectLifecycle(access_, entries_[index].handle);
        if (!lifecycle) {
            if (lifecycle.Error() != ErrorCode::StaleHandle) {
                Fail("InspectLifecycle returned an unexpected error", index, lifecycle.Error());
            }

            return Status::Stale;
        }

        switch (lifecycle.Value()) {
        case ObjectLifecycle::Alive:
            return Status::Alive;
        case ObjectLifecycle::DestroyRequested:
            return Status::DestroyRequested;
        case ObjectLifecycle::Unreachable:
            return Status::Unreachable;
        default:
            return Status::Retired;
        }
    }

    uint32 PickEntry() noexcept {
        return live_.IsEmpty() ? None : live_[rng_.Below(static_cast<uint32>(live_.Size()))];
    }

    // 모델 번호의 handle이다(None이면 null).
    ObjectHandle HandleOf(uint32 index) const noexcept {
        return index == None ? ObjectHandle{} : entries_[index].handle;
    }

    uint32 AllocateEntry() noexcept {
        for (uint32 index = 0; index < entries_.Size(); ++index) {
            const uint32 probe = (nextFree_ + index) % static_cast<uint32>(entries_.Size());
            if (!entries_[probe].used) {
                nextFree_ = probe + 1;
                return probe;
            }
        }

        return None;
    }

    void FreeEntry(uint32 index) noexcept {
        Entry& entry = entries_[index];
        const uint32 position = entry.position;
        const uint32 last = live_[live_.Size() - 1];
        live_[position] = last;
        entries_[last].position = position;
        (void)live_.TryPopBack();
        entry = Entry{};
    }

    // target에서 모델 간선으로 닿는 객체들의 쓰레기 관찰을 지운다. 장벽 shade·루트·새 Outer 자식이
    // 이번 cycle에 그 객체들을 표식할 수 있으므로(floating garbage) 회수 기한을 다시 센다.
    void ResetDeadFrom(uint32 target) noexcept {
        if (target == None) {
            return;
        }

        ++visitMark_;
        queue_.Clear();
        (void)queue_.TryPushBack(target);
        reach_[target] = visitMark_;
        for (usize cursor = 0; cursor < queue_.Size(); ++cursor) {
            const uint32 index = queue_[cursor];
            entries_[index].deadSince = NoCycle;
            VisitEdges(index, [&](uint32 next) noexcept {
                if (reach_[next] != visitMark_) {
                    reach_[next] = visitMark_;
                    (void)queue_.TryPushBack(next);
                }
            });
        }
    }

    template <class Visitor>
    void VisitEdges(uint32 index, Visitor&& visit) const noexcept {
        const Entry& entry = entries_[index];
        if (entry.outer != None) {
            visit(entry.outer);
        }

        if (entry.first != None) {
            visit(entry.first);
        }

        if (entry.second != None) {
            visit(entry.second);
        }

        for (uint32 item = 0; item < entry.itemCount; ++item) {
            if (entry.items[item] != None) {
                visit(entry.items[item]);
            }
        }
    }

    // ---- 변경 ----

    bool CreateObject(Kind kind, uint32 outer, bool keepRoot) noexcept {
        const uint32 index = AllocateEntry();
        if (index == None) {
            return true;
        }

        Status outerStatus = Status::Alive;
        if (outer != None) {
            outerStatus = StatusOf(outer);
        }

        char buffer[24];
        StringView name;
        if (outer != None || rng_.Chance(20)) {
            // 이름은 실행 전체에서 유일하다(같은 Outer 안 충돌이 없다).
            uint64 number = ++nameSerial_;
            uint32 length = 0;
            char digits[20];
            do {
                digits[length++] = static_cast<char>('0' + number % 10);
                number /= 10;
            } while (number != 0);

            buffer[0] = 'c';
            for (uint32 at = 0; at < length; ++at) {
                buffer[1 + at] = digits[length - 1 - at];
            }

            name = StringView(buffer, length + 1);
        }

        ConstructionContext construction(registry_, access_);
        const ObjectHandle outerHandle = HandleOf(outer);
        Result<ObjectHandle> created =
            kind == Kind::Node   ? construction.Create<SoakNode>(SoakNodeType, outerHandle, name)
            : kind == Kind::Leaf ? construction.Create<SoakLeaf>(SoakLeafType, outerHandle, name)
                                 : construction.Create<SoakProbe>(SoakProbeType, outerHandle, name);
        if (outerStatus != Status::Alive) {
            if (created) {
                return Fail("create under a dead outer succeeded", outer, ErrorCode::None);
            }

            if (created.Error() != ErrorCode::StaleHandle) {
                return Fail("create under a dead outer returned a wrong error", outer,
                            created.Error());
            }

            ++report_.rejectedWrites;
            return true;
        }

        if (!created) {
            return Fail("create failed", outer, created.Error());
        }

        auto root = construction.Commit();
        if (!root) {
            return Fail("commit failed", None, root.Error());
        }

        Entry& entry = entries_[index];
        entry = Entry{};
        entry.used = true;
        entry.handle = created.Value();
        entry.kind = kind;
        entry.outer = outer;
        entry.position = static_cast<uint32>(live_.Size());
        if (!live_.TryPushBack(index)) {
            return Fail("model allocation failed", index, ErrorCode::OutOfMemory);
        }

        ++report_.created;
        report_.peakObjects =
            report_.peakObjects > live_.Size() ? report_.peakObjects : uint32(live_.Size());
        // 새 inner가 shade·trace되면 Outer 사슬도 이번 cycle에 표식된다.
        ResetDeadFrom(outer);
        if (keepRoot) {
            entry.roots = 1;
            if (!roots_.TryPushBack(RootHold{index, Move(root.Value())})) {
                return Fail("model allocation failed", index, ErrorCode::OutOfMemory);
            }
        }

        return true;
    }

    void Mutate() noexcept {
        const uint32 roll = rng_.Below(100);
        if (roll < 18) {
            if (live_.Size() < options_.maxObjects + ProbeCount) {
                const Kind kind = rng_.Chance(75) ? Kind::Node : Kind::Leaf;
                const uint32 outer = rng_.Chance(40) ? PickEntry() : None;
                (void)CreateObject(kind, outer, rng_.Chance(45));
            }
        } else if (roll < 36) {
            SetField(rng_.Chance(50));
        } else if (roll < 58) {
            EditArray();
        } else if (roll < 63) {
            PinRoot();
        } else if (roll < 70) {
            ReleaseRoot();
        } else if (roll < 74) {
            RequestDestroy();
        } else if (roll < 75) {
            Discard();
        } else if (roll < 79) {
            AcquireLease();
        } else if (roll < 84) {
            ReleaseLease();
        } else if (roll < 86) {
            AcquirePin();
        } else if (roll < 89) {
            ReleasePin();
        } else if (roll < 90) {
            BorrowBlocksCollection();
        }
    }

    // 대상 하나를 고른다(15%는 null). 상태는 statusOut으로 돌려준다(null이면 Alive).
    uint32 PickTarget(Status& statusOut) noexcept {
        if (rng_.Chance(15)) {
            statusOut = Status::Alive;
            return None;
        }

        const uint32 target = PickEntry();
        statusOut = target == None ? Status::Alive : StatusOf(target);
        return target;
    }

    // owner를 Borrow한다. 상태가 Alive가 아니면 거절을 기대하고 node가 nullptr다.
    bool BorrowNode(uint32 owner, Borrow<SoakNode>& borrow, SoakNode*& node) noexcept {
        node = nullptr;
        const Status status = StatusOf(owner);
        auto resolved = registry_.Resolve<SoakNode>(access_, entries_[owner].handle);
        if (status != Status::Alive) {
            if (resolved) {
                return Fail("Resolve of a non-alive object succeeded", owner, ErrorCode::None);
            }

            const ErrorCode expected =
                status == Status::Stale ? ErrorCode::StaleHandle : ErrorCode::Destroyed;
            if (resolved.Error() != expected) {
                return Fail("Resolve of a non-alive object returned a wrong error", owner,
                            resolved.Error());
            }

            return true;
        }

        if (!resolved) {
            return Fail("Resolve of an alive object failed", owner, resolved.Error());
        }

        borrow = Move(resolved.Value());
        auto pointer = borrow.Get();
        if (!pointer) {
            return Fail("Borrow::Get failed", owner, pointer.Error());
        }

        node = pointer.Value();
        return true;
    }

    // 간선 쓰기 결과를 대조한다. 성공해야 하면 true를 돌려준다(모델을 바꿔야 한다).
    bool CheckWrite(const Result<void>& result, uint32 owner, uint32 target, Status targetStatus,
                    const char* what) noexcept {
        const bool pinned = entries_[owner].pins != 0;
        const bool targetDead = target != None && targetStatus != Status::Alive;
        if (pinned) {
            if (result || result.Error() != ErrorCode::Busy) {
                Fail(what, owner, ErrorOf(result));
            }

            ++report_.rejectedWrites;
            return false;
        }

        if (targetDead) {
            if (result || result.Error() != ErrorCode::StaleHandle) {
                Fail(what, target, ErrorOf(result));
            }

            ++report_.rejectedWrites;
            return false;
        }

        if (!result) {
            Fail(what, owner, result.Error());
            return false;
        }

        ++report_.edgeWrites;
        ResetDeadFrom(target);
        return true;
    }

    void SetField(bool firstField) noexcept {
        const uint32 owner = PickEntry();
        if (owner == None || entries_[owner].kind != Kind::Node) {
            return;
        }

        Borrow<SoakNode> borrow;
        SoakNode* node = nullptr;
        if (!BorrowNode(owner, borrow, node) || node == nullptr) {
            return;
        }

        Status targetStatus;
        const uint32 target = PickTarget(targetStatus);
        ObjectRef<Object>& field = firstField ? node->first : node->second;
        auto written =
            registry_.SetReference(access_, entries_[owner].handle, field, HandleOf(target));
        if (CheckWrite(written, owner, target, targetStatus, "SetReference result mismatch")) {
            (firstField ? entries_[owner].first : entries_[owner].second) = target;
        }
    }

    void EditArray() noexcept {
        const uint32 owner = PickEntry();
        if (owner == None || entries_[owner].kind != Kind::Node) {
            return;
        }

        Borrow<SoakNode> borrow;
        SoakNode* node = nullptr;
        if (!BorrowNode(owner, borrow, node) || node == nullptr) {
            return;
        }

        Entry& entry = entries_[owner];
        if (node->items.Size() != entry.itemCount) {
            Fail("ObjectRefArray size differs from the model", owner, ErrorCode::None);
            return;
        }

        const ObjectHandle handle = entry.handle;
        const uint32 roll = rng_.Below(100);
        if (roll < 40 || entry.itemCount == 0) {
            if (entry.itemCount == MaxItems) {
                return;
            }

            Status targetStatus;
            const uint32 target = PickTarget(targetStatus);
            const bool append = rng_.Chance(60);
            const uint32 at = append ? entry.itemCount : rng_.Below(entry.itemCount + 1);
            auto written =
                append ? registry_.AddReference(access_, handle, node->items, HandleOf(target))
                       : registry_.InsertReferenceAt(access_, handle, node->items, at,
                                                     HandleOf(target));
            if (CheckWrite(written, owner, target, targetStatus, "array insert result mismatch")) {
                for (uint32 move = entry.itemCount; move > at; --move) {
                    entry.items[move] = entry.items[move - 1];
                }

                entry.items[at] = target;
                ++entry.itemCount;
            }
        } else if (roll < 65) {
            Status targetStatus;
            const uint32 target = PickTarget(targetStatus);
            const uint32 at = rng_.Below(entry.itemCount);
            auto written =
                registry_.SetReferenceAt(access_, handle, node->items, at, HandleOf(target));
            if (CheckWrite(written, owner, target, targetStatus, "array set result mismatch")) {
                entry.items[at] = target;
            }
        } else if (roll < 85) {
            const uint32 at = rng_.Below(entry.itemCount);
            const bool byHandle = rng_.Chance(30);
            // 핸들로 지우면 같은 handle의 첫 원소가 지워진다.
            uint32 removed = at;
            if (byHandle) {
                for (uint32 item = 0; item < entry.itemCount; ++item) {
                    if (entry.items[item] == entry.items[at]) {
                        removed = item;
                        break;
                    }
                }
            }

            auto written = byHandle ? registry_.RemoveReference(access_, handle, node->items,
                                                                HandleOf(entry.items[at]))
                                    : registry_.RemoveReferenceAt(access_, handle, node->items, at);
            if (CheckWrite(written, owner, None, Status::Alive, "array remove result mismatch")) {
                for (uint32 move = removed; move + 1 < entry.itemCount; ++move) {
                    entry.items[move] = entry.items[move + 1];
                }

                --entry.itemCount;
            }
        } else {
            auto written = registry_.ClearReferences(access_, handle, node->items);
            if (CheckWrite(written, owner, None, Status::Alive, "array clear result mismatch")) {
                entry.itemCount = 0;
            }
        }
    }

    void PinRoot() noexcept {
        const uint32 target = PickEntry();
        if (target == None) {
            return;
        }

        const Status status = StatusOf(target);
        auto pinned = registry_.TryPin<Object>(access_, entries_[target].handle);
        if (status != Status::Alive) {
            if (pinned) {
                Fail("TryPin of a non-alive object succeeded", target, ErrorCode::None);
            }

            return;
        }

        if (!pinned) {
            Fail("TryPin of an alive object failed", target, pinned.Error());
            return;
        }

        ++entries_[target].roots;
        ++report_.rootChanges;
        ResetDeadFrom(target);
        if (!roots_.TryPushBack(RootHold{target, Move(pinned.Value())})) {
            Fail("model allocation failed", target, ErrorCode::OutOfMemory);
        }
    }

    // 탐침이 아닌 루트 하나를 놓는다. 일부는 helper 스레드로 넘긴다(다음 수집 slice 안에서 실행될
    // 수 있다).
    void ReleaseRoot() noexcept {
        if (roots_.Size() <= ProbeCount) {
            return;
        }

        const uint32 at = ProbeCount + rng_.Below(static_cast<uint32>(roots_.Size() - ProbeCount));
        RootHold hold = Move(roots_[at]);
        roots_[at] = Move(roots_[roots_.Size() - 1]);
        (void)roots_.TryPopBack();
        ++report_.rootChanges;
        if (!channel_.armed && rng_.Chance(30)) {
            // 모델 계수는 helper가 실제로 반환한 뒤(FlushChannel) 줄인다.
            channel_.kind = ReleaseKind::Root;
            pendingKind_ = ReleaseKind::Root;
            channel_.root = Move(hold.root);
            channel_.armed = true;
            pendingEntry_ = hold.entry;
        } else {
            --entries_[hold.entry].roots;
            hold.root.Reset();
        }
    }

    void RequestDestroy() noexcept {
        const uint32 target = PickEntry();
        if (target == None || entries_[target].kind == Kind::Probe) {
            return;
        }

        const Status status = StatusOf(target);
        auto destroyed = registry_.RequestDestroy(access_, entries_[target].handle);
        bool ok = false;
        switch (status) {
        case Status::Alive:
        case Status::DestroyRequested:
        case Status::Retired:
            ok = static_cast<bool>(destroyed);
            break;
        case Status::Unreachable:
            ok = destroyed || destroyed.Error() == ErrorCode::Destroyed;
            break;
        case Status::Stale:
            ok = !destroyed && destroyed.Error() == ErrorCode::StaleHandle;
            break;
        }

        if (!ok) {
            Fail("RequestDestroy result mismatch", target, ErrorOf(destroyed));
            return;
        }

        if (destroyed && status != Status::Stale) {
            entries_[target].destroyRequested = true;
            ++report_.destroys;
        }
    }

    void Discard() noexcept {
        const uint32 target = PickEntry();
        if (target == None || entries_[target].kind == Kind::Probe) {
            return;
        }

        // 부분 트리 판정에 쓰도록 Discard 전 상태를 모두 본다.
        for (uint32 position = 0; position < live_.Size(); ++position) {
            status_[live_[position]] = static_cast<uint8>(StatusOf(live_[position]));
        }

        const Status status = static_cast<Status>(status_[target]);
        auto discarded = registry_.Discard(access_, entries_[target].handle);
        if (entries_[target].discarded && status != Status::Stale) {
            // 이미 Discard된 객체는 무동작 성공이다(cutoff·Retire 뒤에도).
            if (!discarded) {
                Fail("repeated Discard failed", target, discarded.Error());
            }

            return;
        }

        if (status == Status::Alive || status == Status::DestroyRequested) {
            if (!discarded) {
                Fail("Discard of a live object failed", target, discarded.Error());
                return;
            }

            // 자식 고리로 닿는 부분 트리가 논리 파괴된다. 고리에 남은 객체는 Retire·Discard되지
            // 않은 객체다(Retire와 Discard가 고리에서 뗀다) — Outer 사슬의 모든 중간 객체가 그래야
            // 한다.
            for (uint32 position = 0; position < live_.Size(); ++position) {
                const uint32 index = live_[position];
                bool inSubtree = false;
                for (uint32 chain = index; chain != None; chain = entries_[chain].outer) {
                    if (chain == target) {
                        inSubtree = true;
                        break;
                    }

                    const Status link = static_cast<Status>(status_[chain]);
                    if (entries_[chain].discarded || link == Status::Retired ||
                        link == Status::Stale) {
                        break;
                    }
                }

                // 표시는 판정을 모두 끝낸 뒤 한다(판정이 Discard 전 discarded를 봐야 한다).
                reach_[index] = inSubtree ? visitMark_ + 1 : 0;
            }

            ++visitMark_;
            for (uint32 position = 0; position < live_.Size(); ++position) {
                const uint32 index = live_[position];
                if (reach_[index] == visitMark_) {
                    entries_[index].destroyRequested = true;
                    entries_[index].discarded = true;
                }
            }

            ++report_.discards;
            return;
        }

        const ErrorCode expected =
            status == Status::Stale ? ErrorCode::StaleHandle : ErrorCode::Destroyed;
        if (discarded || discarded.Error() != expected) {
            Fail("Discard of a dead object result mismatch", target, ErrorOf(discarded));
        }
    }

    void AcquireLease() noexcept {
        const uint32 target = PickEntry();
        if (target == None) {
            return;
        }

        const Status status = StatusOf(target);
        auto lease = registry_.AcquireBackendLease(access_, entries_[target].handle);
        if (status != Status::Alive) {
            if (lease) {
                Fail("AcquireBackendLease of a non-alive object succeeded", target,
                     ErrorCode::None);
            }

            return;
        }

        if (!lease) {
            Fail("AcquireBackendLease of an alive object failed", target, lease.Error());
            return;
        }

        ++entries_[target].holds;
        entries_[target].deadSince = NoCycle;
        ++report_.holds;
        if (!leases_.TryPushBack(LeaseHold{target, Move(lease.Value())})) {
            Fail("model allocation failed", target, ErrorCode::OutOfMemory);
        }
    }

    void ReleaseLease() noexcept {
        if (leases_.IsEmpty()) {
            return;
        }

        const uint32 at = rng_.Below(static_cast<uint32>(leases_.Size()));
        LeaseHold hold = Move(leases_[at]);
        leases_[at] = Move(leases_[leases_.Size() - 1]);
        (void)leases_.TryPopBack();
        if (!channel_.armed && rng_.Chance(50)) {
            channel_.kind = ReleaseKind::Backend;
            pendingKind_ = ReleaseKind::Backend;
            channel_.lease = Move(hold.lease);
            channel_.armed = true;
            pendingEntry_ = hold.entry;
        } else {
            --entries_[hold.entry].holds;
            hold.lease.Reset();
        }
    }

    void AcquirePin() noexcept {
        const uint32 target = PickEntry();
        if (target == None) {
            return;
        }

        const Status status = StatusOf(target);
        auto pin = registry_.PinForEncode(access_, entries_[target].handle);
        if (status != Status::Alive) {
            if (pin) {
                Fail("PinForEncode of a non-alive object succeeded", target, ErrorCode::None);
            }

            return;
        }

        if (!pin) {
            Fail("PinForEncode of an alive object failed", target, pin.Error());
            return;
        }

        ++entries_[target].pins;
        entries_[target].deadSince = NoCycle;
        ++report_.holds;
        if (!pins_.TryPushBack(PinHold{target, Move(pin.Value())})) {
            Fail("model allocation failed", target, ErrorCode::OutOfMemory);
        }
    }

    void ReleasePin() noexcept {
        if (pins_.IsEmpty()) {
            return;
        }

        const uint32 at = rng_.Below(static_cast<uint32>(pins_.Size()));
        PinHold hold = Move(pins_[at]);
        pins_[at] = Move(pins_[pins_.Size() - 1]);
        (void)pins_.TryPopBack();
        if (!channel_.armed && rng_.Chance(40)) {
            channel_.kind = ReleaseKind::EncodePin;
            pendingKind_ = ReleaseKind::EncodePin;
            channel_.pin = Move(hold.pin);
            channel_.armed = true;
            pendingEntry_ = hold.entry;
        } else {
            --entries_[hold.entry].pins;
            hold.pin.Reset();
        }
    }

    // Borrow를 쥔 동안 수집 진입이 Busy인지 본다.
    void BorrowBlocksCollection() noexcept {
        const uint32 target = PickEntry();
        if (target == None || StatusOf(target) != Status::Alive) {
            return;
        }

        auto borrowed = registry_.Resolve<Object>(access_, entries_[target].handle);
        if (!borrowed) {
            Fail("Resolve of an alive object failed", target, borrowed.Error());
            return;
        }

        ErrorCode result = ErrorCode::None;
        if (registry_.CollectionPhase() == GcPhase::Idle) {
            result = ErrorOf(registry_.BeginCollection(access_));
        } else {
            auto stepped = registry_.StepCollection(access_, GcBudget{}, options_.scheduler);
            result = stepped ? ErrorCode::None : stepped.Error();
        }

        if (result != ErrorCode::Busy) {
            Fail("collection did not report Busy while a Borrow was held", target, result);
            return;
        }

        auto collected = registry_.Collect(access_);
        if (collected || collected.Error() != ErrorCode::Busy) {
            Fail("Collect did not report Busy while a Borrow was held", target,
                 collected ? ErrorCode::None : collected.Error());
            return;
        }

        ++report_.busyChecks;
    }

    // ---- 수집 ----

    void CycleCompleted() noexcept {
        lastCompletedBegin_ = currentBegin_;
        ++report_.cyclesCompleted;
        verifyNow_ = true;
    }

    void CollectStep() noexcept {
        const GcPhase phase = registry_.CollectionPhase();
        if (phase == GcPhase::Idle) {
            const uint32 roll = rng_.Below(100);
            if (roll < 12) {
                auto begun = registry_.BeginCollection(access_);
                if (!begun) {
                    Fail("BeginCollection failed", None, begun.Error());
                    return;
                }

                currentBegin_ = ++cyclesBegun_;
                report_.cyclesBegun = cyclesBegun_;
            } else if (roll < 14) {
                currentBegin_ = ++cyclesBegun_;
                report_.cyclesBegun = cyclesBegun_;
                auto collected = registry_.Collect(access_);
                if (!collected) {
                    Fail("Collect failed", None, collected.Error());
                    return;
                }

                ++report_.collects;
                CycleCompleted();
            } else if (roll < 17) {
                auto drained = registry_.DrainRetired(access_);
                if (!drained) {
                    Fail("DrainRetired failed", None, drained.Error());
                    return;
                }

                ++report_.drains;
            }
        } else {
            if (rng_.Below(400) == 0) {
                auto cancelled = registry_.CancelCollection(access_);
                if (phase == GcPhase::Marking) {
                    if (!cancelled) {
                        Fail("CancelCollection during Marking failed", None, cancelled.Error());
                    }

                    ++report_.cancels;
                    FlushChannel();
                    return;
                }

                if (cancelled || cancelled.Error() != ErrorCode::Busy) {
                    Fail("CancelCollection after cutoff did not report Busy", None,
                         ErrorOf(cancelled));
                    return;
                }
            }

            GcBudget budget;
            if (options_.timeBudget) {
                budget = GcBudget{NumericLimits<uint32>::Max(), 100000};
            } else {
                budget = GcBudget{1 + rng_.Below(64), 0};
            }

            auto progress = registry_.StepCollection(access_, budget, options_.scheduler);
            if (!progress) {
                Fail("StepCollection failed", None, progress.Error());
                return;
            }

            ++report_.steps;
            if (progress.Value().IsComplete()) {
                CycleCompleted();
            }
        }

        FlushChannel();
    }

    // slice 밖에서 남은 반환을 실행한다(탐침이 trace되지 않은 tick).
    void FlushChannel() noexcept {
        const ReleaseKind kind = pendingKind_;
        if (channel_.armed) {
            channel_.armed = false;
            ++channel_.outside;
            channel_.Handoff();
        }

        // helper가 반환을 마쳤다(slice 안 또는 방금). 이제 모델 계수를 줄인다.
        if (pendingEntry_ != None) {
            Entry& entry = entries_[pendingEntry_];
            if (kind == ReleaseKind::Root) {
                --entry.roots;
            } else if (kind == ReleaseKind::Backend) {
                --entry.holds;
            } else if (kind == ReleaseKind::EncodePin) {
                --entry.pins;
            }

            pendingEntry_ = None;
        }

        report_.crossThreadInSlice = channel_.inSlice;
        report_.crossThreadOutside = channel_.outside;
    }

    // ---- 대조 ----

    void Verify() noexcept {
        ++report_.verifications;
        const ObjectStats stats = registry_.Stats();
        uint64 roots = 0;
        uint64 holds = 0;
        uint64 pins = 0;
        for (uint32 position = 0; position < live_.Size(); ++position) {
            const Entry& entry = entries_[live_[position]];
            roots += entry.roots;
            holds += entry.holds;
            pins += entry.pins;
        }

        if (stats.borrows != 0 || stats.reservedObjects != 0 || stats.roots != roots ||
            stats.backendHolds != holds || stats.encodePins != pins) {
            Fail("registry hold counters differ from the model", None, ErrorCode::None);
            return;
        }

        // 모델 도달성: 루트에서 간선(Outer 포함)으로 닿는 객체.
        ++visitMark_;
        const uint32 reachable = visitMark_;
        queue_.Clear();
        for (uint32 position = 0; position < live_.Size(); ++position) {
            const uint32 index = live_[position];
            if (entries_[index].roots != 0) {
                reach_[index] = reachable;
                (void)queue_.TryPushBack(index);
            }
        }

        for (usize cursor = 0; cursor < queue_.Size(); ++cursor) {
            VisitEdges(queue_[cursor], [&](uint32 next) noexcept {
                if (reach_[next] != reachable) {
                    reach_[next] = reachable;
                    (void)queue_.TryPushBack(next);
                }
            });
        }

        uint64 present = 0;
        for (uint32 position = 0; position < live_.Size() && !Failed(); ++position) {
            const uint32 index = live_[position];
            Entry& entry = entries_[index];
            const Status status = StatusOf(index);
            status_[index] = static_cast<uint8>(status);
            present += status == Status::Stale ? 0u : 1u;
            if (reach_[index] == reachable) {
                entry.deadSince = NoCycle;
                if (status != Status::Alive && status != Status::DestroyRequested) {
                    Fail("reachable object was cut off, retired or reclaimed", index,
                         ErrorCode::None);
                    return;
                }

                if ((status == Status::DestroyRequested) != entry.destroyRequested) {
                    Fail(entry.destroyRequested
                             ? "model expects destroy-requested but the object is alive"
                             : "object is destroy-requested but the model expects alive",
                         index, ErrorCode::None);
                    return;
                }
            } else {
                if (status == Status::Stale && (entry.holds != 0 || entry.pins != 0)) {
                    Fail("object reclaimed while a backend lease or encode pin was held", index,
                         ErrorCode::None);
                    return;
                }

                if (entry.holds != 0 || entry.pins != 0) {
                    entry.deadSince = NoCycle;
                } else if (status != Status::Stale) {
                    if (entry.deadSince == NoCycle) {
                        entry.deadSince = cyclesBegun_;
                    } else if (lastCompletedBegin_ != NoCycle &&
                               lastCompletedBegin_ > entry.deadSince) {
                        Fail("garbage survived a complete cycle that began after it died", index,
                             ErrorCode::None);
                        return;
                    }
                }
            }

            if (status == Status::Alive && entry.kind == Kind::Node) {
                VerifyFields(index);
            }
        }

        if (Failed()) {
            return;
        }

        if (stats.liveObjects + stats.destroyRequested + stats.retiredObjects != present) {
            Fail("registry object counts differ from the model", None, ErrorCode::None);
            return;
        }

        // 살아 있는(cutoff 아님) 객체의 간선은 회수된 객체를 가리키지 않는다.
        for (uint32 position = 0; position < live_.Size(); ++position) {
            const uint32 index = live_[position];
            const Status status = static_cast<Status>(status_[index]);
            if (status != Status::Alive && status != Status::DestroyRequested) {
                continue;
            }

            bool dangling = false;
            VisitEdges(index, [&](uint32 next) noexcept {
                dangling = dangling || static_cast<Status>(status_[next]) == Status::Stale;
            });
            if (dangling) {
                Fail("live object points to a reclaimed object", index, ErrorCode::None);
                return;
            }
        }

        // 회수된 객체를 모델에서 지우고 죽은 객체의 간선에서 뗀다.
        for (uint32 position = 0; position < live_.Size();) {
            const uint32 index = live_[position];
            if (static_cast<Status>(status_[index]) != Status::Stale) {
                ++position;
                continue;
            }

            ++report_.reclaimed;
            FreeEntry(index);
            status_[index] = static_cast<uint8>(Status::Stale);
        }

        for (uint32 position = 0; position < live_.Size(); ++position) {
            Entry& entry = entries_[live_[position]];
            auto clear = [&](uint32& edge) noexcept {
                if (edge != None && !entries_[edge].used) {
                    edge = None;
                }
            };

            clear(entry.outer);
            clear(entry.first);
            clear(entry.second);
            for (uint32 item = 0; item < entry.itemCount; ++item) {
                clear(entry.items[item]);
            }
        }

        if (destroyedObjects != report_.reclaimed) {
            Fail("destructor count differs from the observed reclaim count", None, ErrorCode::None);
        }
    }

    void VerifyFields(uint32 index) noexcept {
        const Entry& entry = entries_[index];
        auto borrowed = registry_.Resolve<SoakNode>(access_, entry.handle);
        if (!borrowed) {
            Fail("Resolve of an alive node failed during verification", index, borrowed.Error());
            return;
        }

        auto pointer = borrowed.Value().Get();
        if (!pointer) {
            Fail("Borrow::Get failed during verification", index, pointer.Error());
            return;
        }

        const SoakNode& node = *pointer.Value();
        bool same = node.first.Handle() == HandleOf(entry.first) &&
                    node.second.Handle() == HandleOf(entry.second) &&
                    node.items.Size() == entry.itemCount;
        for (uint32 item = 0; same && item < entry.itemCount; ++item) {
            same = node.items[item] == HandleOf(entry.items[item]);
        }

        if (!same) {
            Fail("object fields differ from the model", index, ErrorCode::None);
        }
    }

    const GcSoakOptions& options_;
    GcSoakReport& report_;
    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    Array<Entry> entries_;
    Array<uint32> live_;
    Array<RootHold> roots_;
    Array<LeaseHold> leases_;
    Array<PinHold> pins_;
    Array<uint32> reach_;
    Array<uint32> queue_;
    Array<uint8> status_;
    ReleaseChannel channel_;
    Rng rng_;
    uint64 tick_ = 0;
    uint64 nameSerial_ = 0;
    uint64 cyclesBegun_ = 0;
    uint64 currentBegin_ = 0;
    uint64 lastCompletedBegin_ = NoCycle;
    uint32 nextFree_ = 0;
    // helper에 넘겼지만 아직 모델 계수에 반영하지 않은 반환의 객체 번호다(None이면 없다).
    uint32 pendingEntry_ = None;
    ReleaseKind pendingKind_ = ReleaseKind::None;
    uint32 visitMark_ = 0;
    bool verifyNow_ = false;
    bool threadStarted_ = false;
};

} // namespace

bool RunGcSoak(Allocator& allocator, const GcSoakOptions& options, GcSoakReport& report) noexcept {
    report = {};
    destroyedObjects = 0;
    if (options.maxObjects == 0 || options.verifyInterval == 0) {
        report.failure = "invalid GcSoak options";
        return false;
    }

    {
        TypeRegistry types(allocator);
        RuntimeTypeOps node = MakeRuntimeTypeOps<SoakNode>();
        node.parallelTraceSafe = true;
        RuntimeTypeOps leaf = MakeRuntimeTypeOps<SoakLeaf>();
        leaf.parallelTraceSafe = true;
        auto nodeToken = types.Register(TypeSchema{SoakNodeType, 1, {}, {}}, Move(node));
        auto leafToken = types.Register(TypeSchema{SoakLeafType, 1, {}, {}}, Move(leaf));
        auto probeToken =
            types.Register(TypeSchema{SoakProbeType, 1, {}, {}}, MakeRuntimeTypeOps<SoakProbe>());
        if (!nodeToken || !leafToken || !probeToken) {
            report.failure = "GcSoak type registration failed";
            return false;
        }

        {
            RegistryOptions registryOptions;
            registryOptions.maxSlots = options.maxObjects * 4 + 4096;
            ObjectRegistry registry(allocator, types, Guid{0x47436f616b, options.seed},
                                    registryOptions);
            Soak soak(allocator, options, report, registry);
            if (soak.Initialize()) {
                for (uint64 tick = 0; tick < options.ticks && !soak.Failed(); ++tick) {
                    soak.Tick();
                }
            }

            soak.Finish();
        }

        if (!types.Unregister(probeToken.Value()) || !types.Unregister(leafToken.Value()) ||
            !types.Unregister(nodeToken.Value()) || !types.Shutdown()) {
            if (report.failure == nullptr) {
                report.failure = "GcSoak type registry shutdown failed";
            }
        }
    }

    return report.failure == nullptr;
}

} // namespace rocks
