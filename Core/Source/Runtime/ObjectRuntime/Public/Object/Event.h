#pragma once

#include <Base/PlacementNew.h>
#include <Base/TypeTraits.h>
#include <Core/Containers/Array.h>
#include <Core/Delegate.h>
#include <Core/Memory/Allocator.h>
#include <Object/FunctionBinding.h>
#include <Object/ObjectRegistry.h>

// 리플렉션된 이벤트다(Docs/Design/EventReflection.ko.md ED1·ED2·ED6–ED8·ED13 — UiToolkit CU3, UE
// dynamic multicast delegate 대응). 이벤트 필드 하나가 C++ 엔진 코드의 native 구독(Delegate —
// 저장하지 않는다)과 리플렉션 바인딩(대상 handle + FunctionId — FunctionBinding)을 함께 담는다.
// 안쪽 저장은 Core MulticastDelegate라 호출 순서는 섞인 등록 순서이고 발행 중 추가·제거 규칙도
// Core와 같다.
//
// 사용 예:
//   ROCKS_PROPERTY(Params = "killer") Event<WeakObjectRef<Actor>> died;   // EV2 — 리플렉션
//   died.TryAddLambda([](WeakObjectRef<Actor> killer) noexcept { ... });   // native 구독
//   died.TryBind(registry, access, listener, OnDiedId);                    // 리플렉션 바인딩
//   died.Broadcast({registry, access, Handle()}, WeakObjectRef<Actor>(killer));
//
// 계약:
// - 인자는 nothrow 복사 가능한 값 또는 const T&다(StringView는 받지 않는다 — const String&).
// - 구독이 생길 때 상태를 할당한다(이벤트 필드 자체는 16 byte — 구독 없는 객체에 비용이 없다).
// - 바인딩·발행은 발행자 소유 스레드·접근 문맥에서만 한다. 발행은 발행자를 root로 붙잡아 발행
//   중 회수되지 않게 한다. 파괴 요청된 발행자와 생성 중(Reserved) 발행자는 붙잡지 못해 발행하지
//   않는다 — 파괴를 알리는 이벤트(died 등)는 RequestDestroy 전에 발행하고, 논리 파괴가 Clear한다.
// - 리플렉션 바인딩은 발행 1회에 frame 하나(인자를 한 번 복사)를 함께 쓴다. 대상이 사라졌거나
//   파괴 요청됐으면 그 바인딩을 지운다(대상 쪽 역방향 목록은 없다 — Compact·TryBind가 정리한다).
// - 리플렉션 바인딩의 delegate 항목 코드는 TryBind를 부른 image에 있다(템플릿). 바인딩하는 모듈은
//   바인딩보다 오래 살아야 한다 — 모듈 사이 바인딩은 발행자 모듈의 RuntimeEventOps(EV2)로 한다.
// - 이동·복사 금지(바인딩 항목이 상태 주소를 쥔다). 발행 중 파괴는 Fatal이다(Core).
namespace rocks {

// 이벤트 발행 문맥이다: Registry·접근 문맥과 발행자(이벤트 필드를 가진 객체) handle.
struct EventContext {
    ObjectRegistry& registry;
    const ObjectAccessContext& access;
    ObjectHandle publisher;
};

namespace detail {

// 이벤트 인자 형식 규칙이다(ED1): const T&이거나 nothrow 복사 가능한 값, StringView 아님. frame 칸
// 복사 가능(ValueCopy — 리플렉션 struct는 생성 헤더가 필요하다)은 여기서 보지 않고 바인딩·발행이
// 쓰는 BindingFrame이 본다: 이벤트 필드 선언(클래스 정의)은 같은 헤더의 struct 생성 헤더보다 먼저
// 오므로 클래스 수준 검사에 넣으면 리플렉션 struct 인자를 쓸 수 없다.
template <class T>
constexpr bool IsEventArgument() noexcept {
    if constexpr (IsReferenceV<T>) {
        return IsConstV<RemoveReferenceT<T>> && !IsRvalueReferenceV<T> &&
               !IsSameV<BindingSlotType<T>, StringView>;
    } else {
        return IsNothrowCopyConstructibleV<T> && !IsSameV<RemoveCvT<T>, StringView>;
    }
}

} // namespace detail

template <class... Args>
class Event {
    static_assert((detail::IsEventArgument<Args>() && ...),
                  "Event arguments must be nothrow-copyable values or const references, not "
                  "StringView");

public:
    using DelegateType = Delegate<void(Args...)>;

    explicit Event(Allocator& allocator) noexcept : allocator_(&allocator) {
    }

    ~Event() noexcept {
        if (state_ != nullptr) {
            DestroyAt(state_);
            if (!allocator_->Deallocate(state_)) {
                platform::Fatal("Event state release failed");
            }
        }
    }

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    // native 구독을 더한다(저장하지 않는다). 실패: 바인딩 없는 delegate는 InvalidArgument, 할당
    // 실패는 그 오류.
    Result<DelegateHandle> TryAdd(DelegateType&& delegate) noexcept {
        if (auto ensured = EnsureState(); !ensured) {
            return Result<DelegateHandle>::Failure(ensured.Error());
        }

        return state_->delegate.TryAdd(Move(delegate));
    }

    template <class Callable>
    Result<DelegateHandle> TryAddLambda(Callable&& callable) noexcept {
        return TryAdd(DelegateType::CreateLambda(Forward<Callable>(callable)));
    }

    // 대상 객체의 리플렉션 함수 functionId를 바인딩한다(ED4). 같은 (대상, 함수)가 이미 있으면
    // AlreadyExists, 그 밖의 실패는 FunctionBinding::Create와 같다(NotFound·InvalidArgument·
    // TypeMismatch·Registry 오류)와 할당 실패다. 죽은 바인딩을 분할 상환으로 정리한다(ED7).
    Result<DelegateHandle> TryBind(ObjectRegistry& registry, const ObjectAccessContext& access,
                                   ObjectHandle target, Guid functionId) noexcept {
        if (auto ensured = EnsureState(); !ensured) {
            return Result<DelegateHandle>::Failure(ensured.Error());
        }

        State& state = *state_;
        if (state.records.Size() >= state.liveAtLastCompact * 2 + MulticastCompactSlack) {
            Compact(registry, access);
        }

        for (const Record& record : state.records) {
            if (!record.dead && record.binding.Target() == target &&
                record.binding.FunctionId() == functionId) {
                return Result<DelegateHandle>::Failure(ErrorCode::AlreadyExists);
            }
        }

        auto binding = FunctionBinding::Create<Args...>(registry, access, target, functionId);
        if (!binding) {
            return Result<DelegateHandle>::Failure(binding.Error());
        }

        if (auto reserved = state.records.TryEnsureCapacity(state.records.Size() + 1); !reserved) {
            return Result<DelegateHandle>::Failure(reserved.Error());
        }

        const uint64 id = state.nextId++;
        if (!state.records.TryPushBack(Record{id, binding.Value(), DelegateHandle{}, false})) {
            platform::Fatal("Event reserved record push failed");
        }

        // 실패하면 항목의 소멸이 레코드를 지운다(BoundTarget 소멸자).
        auto added = state.delegate.TryAdd(DelegateType::CreateLambda(BoundTarget(state_, id)));
        if (!added) {
            return added;
        }

        if (Record* record = state.Find(id)) {
            record->handle = added.Value();
        }

        return added;
    }

    // native 구독·리플렉션 바인딩 하나를 지운다. 없으면 false다. 리플렉션 바인딩은 레코드를 먼저
    // 죽음 표시한다(실행 중인 자기 항목을 지워 소멸이 미뤄져도 같은 쌍을 다시 바인딩할 수 있다).
    bool Remove(DelegateHandle handle) noexcept {
        if (state_ == nullptr || !handle.IsValid()) {
            return false;
        }

        for (Record& record : state_->records) {
            if (record.handle == handle) {
                record.dead = true;
                record.handle.Reset();
                break;
            }
        }

        return state_->delegate.Remove(handle);
    }

    // 리플렉션 바인딩 handle 하나를 지운다(native 구독 handle이면 지우지 않고 false — 연산표 경로가
    // 다른 이벤트의 handle로 C++ 구독을 지우지 않게 한다, EventReflection ED16).
    bool Unbind(DelegateHandle handle) noexcept {
        if (state_ == nullptr || !handle.IsValid()) {
            return false;
        }

        return RemoveMatching(
                   [&](const Record& record) noexcept { return record.handle == handle; }) != 0;
    }

    // target의 리플렉션 함수 functionId 바인딩을 지운다(UE Remove(Object, FunctionName)). 없으면
    // false다.
    bool Unbind(ObjectHandle target, Guid functionId) noexcept {
        if (state_ == nullptr) {
            return false;
        }

        return RemoveMatching([&](const Record& record) noexcept {
                   return record.binding.Target() == target &&
                          record.binding.FunctionId() == functionId;
               }) != 0;
    }

    // target을 대상으로 하는 리플렉션 바인딩을 모두 지우고 그 수를 돌려준다(UE RemoveAll(Object)).
    usize UnbindAll(ObjectHandle target) noexcept {
        if (state_ == nullptr) {
            return 0;
        }

        const usize removed = RemoveMatching(
            [&](const Record& record) noexcept { return record.binding.Target() == target; });
        state_->liveAtLastCompact = state_->LiveRecords();
        return removed;
    }

    // 모든 구독·바인딩을 지운다(발행자 논리 파괴 — ED8).
    void Clear() noexcept {
        if (state_ == nullptr) {
            return;
        }

        for (Record& record : state_->records) {
            record.dead = true;
            record.handle.Reset();
        }

        state_->delegate.Clear();
        state_->liveAtLastCompact = 0;
    }

    // 대상이 사라졌거나(StaleHandle) 살아 있지 않은(파괴 요청·도달 불가·은퇴) 리플렉션 바인딩을
    // 지우고 그 수를 돌려준다(ED7). 생존 조회의 다른 오류(다른 문맥 등)는 지우지 않는다.
    usize Compact(ObjectRegistry& registry, const ObjectAccessContext& access) noexcept {
        if (state_ == nullptr) {
            return 0;
        }

        const usize removed = RemoveMatching([&](const Record& record) noexcept {
            auto lifecycle = registry.InspectLifecycle(access, record.binding.Target());
            return lifecycle ? lifecycle.Value() != ObjectLifecycle::Alive
                             : lifecycle.Error() == ErrorCode::StaleHandle;
        });
        state_->liveAtLastCompact = state_->LiveRecords();
        return removed;
    }

    // 살아 있는 리플렉션 바인딩을 등록 순서로 visit(target, functionId)한다(native 구독은 빼고,
    // 죽음 표시는 건너뛴다 — EventReflection ED20 저장·EV5 목록). visit은 Result<void>를 돌려주고
    // 실패하면 그 오류로 멈춘다. visit 안에서 이 이벤트를 바꾸지 않는다.
    //
    // 사용 예:
    //   auto visited = died.ForEachBinding([&](ObjectHandle target, Guid function) noexcept {
    //       return list.TryPushBack({target, function});
    //   });
    template <class Visit>
    Result<void> ForEachBinding(Visit&& visit) const noexcept {
        if (state_ == nullptr) {
            return Result<void>::Success();
        }

        for (const Record& record : state_->records) {
            if (record.dead) {
                continue;
            }

            if (auto visited = visit(record.binding.Target(), record.binding.FunctionId());
                !visited) {
                return visited;
            }
        }

        return Result<void>::Success();
    }

    bool IsBound() const noexcept {
        return state_ != nullptr && state_->delegate.IsBound();
    }

    // 리플렉션 바인딩 레코드 수다(죽음 표시가 아직 정리되지 않은 것 포함 — 발행 중 지운 것은 가장
    // 바깥 발행이 끝날 때 빠진다).
    usize BindingCount() const noexcept {
        return state_ == nullptr ? 0 : state_->records.Size();
    }

    // 발행한다(ED6). 결과는 TryBroadcast와 같고 버린다(엔진 호출 지점마다 결과 처리를 강요하지
    // 않는다).
    void Broadcast(const EventContext& context, Args... args) noexcept {
        (void)TryBroadcast(context, args...);
    }

    // 발행한다. 실패: 발행자를 붙잡지 못함(문맥·스레드·파괴 요청·회수 — Registry 오류, 아무것도
    // 부르지 않는다), 리플렉션 바인딩의 첫 실패 코드(frame 인자 할당 실패 포함 — 다른 구독은 계속
    // 부른다). 구독이 없으면 아무것도 하지 않고 성공이다.
    Result<void> TryBroadcast(const EventContext& context, Args... args) noexcept {
        if (state_ == nullptr) {
            return Result<void>::Success();
        }

        auto pinned = context.registry.TryPin<Object>(context.access, context.publisher);
        if (!pinned) {
            return Result<void>::Failure(pinned.Error());
        }

        Scope scope{&context.registry, &context.access, nullptr, ErrorCode::None, state_->current};
        alignas(FrameType) unsigned char storage[sizeof(FrameType)];
        if (state_->records.Size() != 0) {
            FrameType* frame =
                ::new (static_cast<void*>(storage)) FrameType(context.registry.ObjectAllocator());
            if (auto assigned = frame->Assign(args...); assigned) {
                scope.frame = frame;
            } else {
                scope.firstError = assigned.Error();
                DestroyAt(frame);
            }
        }

        state_->current = &scope;
        state_->delegate.Broadcast(args...);
        state_->current = scope.outer;
        if (scope.frame != nullptr) {
            DestroyAt(scope.frame);
        }

        return scope.firstError == ErrorCode::None ? Result<void>::Success()
                                                   : Result<void>::Failure(scope.firstError);
    }

private:
    using FrameType = BindingFrame<Args...>;

    struct Record {
        uint64 id;
        FunctionBinding binding;
        DelegateHandle handle;
        bool dead;
    };

    // 발행 하나의 문맥이다(중첩 발행은 outer로 잇는다 — 지금 실행 중인 항목은 늘 꼭대기의 것이다).
    struct Scope {
        ObjectRegistry* registry;
        const ObjectAccessContext* access;
        FrameType* frame;
        ErrorCode firstError;
        Scope* outer;
    };

    struct State {
        explicit State(Allocator& allocator) noexcept : records(allocator), delegate(allocator) {
        }

        // 바인딩 id 순이다. delegate보다 먼저 선언해 나중에 소멸한다(delegate 항목의 소멸이
        // 레코드를 지운다).
        Array<Record> records;
        MulticastDelegate<void(Args...)> delegate;
        uint64 nextId = 1;
        usize liveAtLastCompact = 0;
        Scope* current = nullptr;

        Record* Find(uint64 id) noexcept {
            return const_cast<Record*>(static_cast<const State*>(this)->Find(id));
        }

        const Record* Find(uint64 id) const noexcept {
            usize low = 0;
            usize high = records.Size();
            while (low < high) {
                const usize middle = low + (high - low) / 2;
                if (records[middle].id < id) {
                    low = middle + 1;
                } else {
                    high = middle;
                }
            }

            return low < records.Size() && records[low].id == id ? &records[low] : nullptr;
        }

        usize LiveRecords() const noexcept {
            usize live = 0;
            for (const Record& record : records) {
                live += record.dead ? 0 : 1;
            }

            return live;
        }

        void Erase(uint64 id) noexcept {
            Record* record = Find(id);
            if (record == nullptr) {
                return;
            }

            if (!records.TryRemoveAt(static_cast<usize>(record - records.Data()))) {
                platform::Fatal("Event record removal failed");
            }
        }
    };

    // 리플렉션 바인딩의 delegate 항목이다(상태와 바인딩 id만 쥔다 — 레코드 배열이 커지거나 당겨져도
    // id로 찾는다). 생존 판정은 레코드의 죽음 표시다(Core C1 — 죽은 항목은 불리지 않고 Core 정리가
    // 한 pass로 해제한다). 소멸이 레코드를 지운다.
    struct BoundTarget {
        BoundTarget(State* owner, uint64 binding) noexcept : state(owner), id(binding) {
        }

        BoundTarget(BoundTarget&& source) noexcept : state(source.state), id(source.id) {
            source.state = nullptr;
        }

        BoundTarget(const BoundTarget&) = delete;
        BoundTarget& operator=(const BoundTarget&) = delete;
        BoundTarget& operator=(BoundTarget&&) = delete;

        ~BoundTarget() noexcept {
            if (state != nullptr) {
                state->Erase(id);
            }
        }

        bool IsAlive() const noexcept {
            const Record* record = state == nullptr ? nullptr : state->Find(id);
            return record != nullptr && !record->dead;
        }

        void operator()(Args...) noexcept {
            Record* record = state->Find(id);
            Scope* scope = state->current;
            if (record == nullptr || record->dead || scope == nullptr || scope->frame == nullptr) {
                return;
            }

            // binding을 복사해 부른다(호출된 함수가 같은 이벤트에 TryBind하면 레코드 배열이
            // 옮겨진다).
            const FunctionBinding binding = record->binding;
            auto called = binding.Invoke(*scope->registry, *scope->access, *scope->frame);
            if (called) {
                return;
            }

            if (scope->firstError == ErrorCode::None) {
                scope->firstError = called.Error();
            }

            // 대상이 사라졌거나 파괴 요청됐으면 바인딩을 죽음 표시한다(실행 중인 항목이므로 Core가
            // 발행 뒤 정리한다). 호출 중 레코드가 옮겨졌을 수 있어 다시 찾는다.
            if (called.Error() == ErrorCode::StaleHandle ||
                called.Error() == ErrorCode::Destroyed) {
                if (Record* again = state->Find(id)) {
                    again->dead = true;
                    again->handle.Reset();
                }
            }
        }

        State* state;
        uint64 id;
    };

    Result<void> EnsureState() noexcept {
        if (state_ != nullptr) {
            return Result<void>::Success();
        }

        auto memory = allocator_->Allocate(sizeof(State), alignof(State));
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        state_ = InitializeAt<State>(memory.Value(), *allocator_);
        return Result<void>::Success();
    }

    // matches가 참인 레코드를 죽음 표시하고 Core 정리를 한 번 부른다(C1 — 죽은 항목은 생존 판정이
    // false라 한 pass로 해제되고, 항목 소멸이 레코드를 지운다). 발행 중이면 Core가 정리를 가장 바깥
    // 발행 뒤로 미룬다(그때까지 죽은 항목은 불리지 않는다). 반환: 표시한 레코드 수.
    template <class Matches>
    usize RemoveMatching(Matches&& matches) noexcept {
        State& state = *state_;
        usize removed = 0;
        for (Record& record : state.records) {
            if (!record.dead && matches(record)) {
                record.dead = true;
                record.handle.Reset();
                ++removed;
            }
        }

        if (removed != 0) {
            state.delegate.CompactDead();
        }

        return removed;
    }

    Allocator* allocator_;
    State* state_ = nullptr;
};

} // namespace rocks
