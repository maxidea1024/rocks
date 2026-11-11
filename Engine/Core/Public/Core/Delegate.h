#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Core/Traits.h>
#include <Platform/Fatal.h>

namespace rocks {

// Delegate가 할당 없이 담는 호출 대상의 최대 크기다. 정적 함수·멤버 함수 포인터와 작은 캡처
// lambda는 이 안에 들어간다. 더 큰 lambda는 TryCreateLambda(allocator, ...)로 만든다.
inline constexpr usize DelegateInlineBytes = 48;

template <typename Signature>
class Delegate;

template <typename Signature>
class MulticastDelegate;

// MulticastDelegate 등록 식별자다(UE FDelegateHandle 대응). 기본값은 무효이며 같은
// MulticastDelegate 안에서만 의미가 있다.
class DelegateHandle {
public:
    constexpr DelegateHandle() noexcept = default;

    constexpr bool IsValid() const noexcept {
        return id_ != 0;
    }

    constexpr void Reset() noexcept {
        id_ = 0;
    }

    friend constexpr bool operator==(DelegateHandle, DelegateHandle) noexcept = default;

private:
    template <typename>
    friend class MulticastDelegate;

    explicit constexpr DelegateHandle(uint64 id) noexcept : id_(id) {
    }

    uint64 id_ = 0;
};

namespace detail {

template <typename Return, typename Callable, typename... Args>
Return InvokeDelegateTarget(Callable& callable, Args&&... args) noexcept {
    if constexpr (IsSameV<Return, void>) {
        callable(Forward<Args>(args)...);
    } else {
        return callable(Forward<Args>(args)...);
    }
}

} // namespace detail

// 호출 대상 하나를 담는 delegate다(UE TDelegate 대응). 이동만 가능하다.
// 사용 예:
//   Delegate<void(int)> onChanged = Delegate<void(int)>::CreateLambda([this](int v) noexcept {
//       Refresh(v);
//   });
//   onChanged.ExecuteIfBound(3);
//   auto weak = Delegate<int()>::CreateWeak(sharedWidget, &Widget::Count); // 대상 소멸 시 자동
//   해제
//
// 계약:
// - CreateStatic·CreateLambda·CreateRaw·CreateWeak·CreateWeakLambda는 할당하지 않는다(inline
//   저장, DelegateInlineBytes 초과 lambda는 컴파일 오류). TryCreateLambda는 allocator에 할당하며
//   allocator는 delegate보다 오래 살아야 한다.
// - CreateRaw는 대상 객체의 수명을 추적하지 않는다(UE BindRaw와 같다). 객체가 먼저 사라질 수
//   있으면 SharedPtr 대상의 CreateWeak/CreateWeakLambda를 쓴다. 이 둘은 호출 동안 대상을 강하게
//   붙잡아(Lock) 호출 중 소멸하지 않게 하고, 대상이 사라지면 IsBound가 false가 된다.
// - Execute는 바인딩이 없거나 약한 대상이 사라졌으면 Fatal이다. 반환값이 없는 delegate는
//   ExecuteIfBound로 실행 여부를 받는다(다른 스레드가 약한 대상을 놓을 수 있으면 이쪽을 쓴다).
// - 실행 중인 delegate를 그 호출 안에서 Unbind하면 호출이 끝난 뒤 해제한다(한 번만 실행하는
//   callback 패턴). 실행 중 재바인드·이동·파괴는 대상이 쓰는 저장소를 깨므로 Fatal이다.
// - 호출 대상의 이동 생성·소멸은 noexcept여야 한다. 인자는 호출 대상에 그대로 전달된다.
// - 내부 동기화가 없다. 같은 인스턴스를 여러 스레드에서 동시에 실행·변경하지 않는다. Execute는
//   const이지만 실행 상태와 호출 대상(mutable lambda 포함)은 내부에서 갱신한다.
template <typename Return, typename... Args>
class Delegate<Return(Args...)> {
public:
    constexpr Delegate() noexcept = default;

    Delegate(const Delegate&) = delete;
    Delegate& operator=(const Delegate&) = delete;

    Delegate(Delegate&& other) noexcept {
        other.CheckNotExecuting("Delegate moved during its own execution");
        TakeFrom(other);
    }

    Delegate& operator=(Delegate&& other) noexcept {
        if (this != &other) {
            CheckNotExecuting("Delegate rebound during its own execution");
            other.CheckNotExecuting("Delegate moved during its own execution");
            // 기존 대상 소멸자가 재진입하더라도 새 대상이 먼저 자리를 잡게 임시로 옮긴다.
            Delegate previous;
            previous.TakeFrom(*this);
            TakeFrom(other);
        }
        return *this;
    }

    ~Delegate() noexcept {
        CheckNotExecuting("Delegate destroyed during its own execution");
        DestroyBinding();
    }

    // ---- 생성 ----

    static Delegate CreateStatic(Return (*function)(Args...)) noexcept {
        Delegate result;
        if (function != nullptr) {
            result.template Emplace<StaticTarget>(function);
        }
        return result;
    }

    template <typename Callable>
    static Delegate CreateLambda(Callable&& callable) noexcept {
        using Stored = RemoveConstT<RemoveReferenceT<Callable>>;
        Delegate result;
        result.template Emplace<LambdaTarget<Stored>>(Forward<Callable>(callable));
        return result;
    }

    // inline 용량을 넘는 lambda를 allocator에 담는다. 실패하면 Allocate 오류를 반환하며 callable은
    // 이동하지 않는다.
    template <typename Callable>
    static Result<Delegate> TryCreateLambda(Allocator& allocator, Callable&& callable) noexcept {
        using Stored = RemoveConstT<RemoveReferenceT<Callable>>;
        static_assert(IsNothrowDestructibleV<Stored>, "Delegate target needs noexcept destruction");
        static_assert(__is_nothrow_constructible(Stored, Callable&&),
                      "Delegate target needs a noexcept constructor");
        auto memory = allocator.Allocate(sizeof(Stored), alignof(Stored));
        if (!memory) {
            return Result<Delegate>::Failure(memory.Error());
        }
        Stored* stored = ConstructAt<Stored>(memory.Value(), Forward<Callable>(callable));
        Delegate result;
        result.template Emplace<HeapLambdaTarget<Stored>>(stored, &allocator);
        return Result<Delegate>::Success(Move(result));
    }

    // object->*method를 호출한다. 수명은 추적하지 않는다(UE BindRaw). object가 nullptr면 빈
    // delegate다.
    template <typename Object, typename Method>
    static Delegate CreateRaw(Object* object, Method method) noexcept {
        Delegate result;
        if (object != nullptr && method != nullptr) {
            result.template Emplace<RawTarget<Object, Method>>(object, method);
        }
        return result;
    }

    // SharedPtr 대상의 멤버 함수를 약하게 묶는다(UE BindSP). 빈 SharedPtr면 빈 delegate다.
    template <typename Class, typename Method>
    static Delegate CreateWeak(const SharedPtr<Class>& object, Method method) noexcept {
        Delegate result;
        if (object && method != nullptr) {
            result.template Emplace<WeakTarget<Class, Method>>(object, method);
        }
        return result;
    }

    // owner가 살아 있는 동안만 lambda를 호출한다(UE CreateSPLambda). owner는 호출 동안 유지된다.
    template <typename Class, typename Callable>
    static Delegate CreateWeakLambda(const SharedPtr<Class>& owner, Callable&& callable) noexcept {
        using Stored = RemoveConstT<RemoveReferenceT<Callable>>;
        Delegate result;
        if (owner) {
            result.template Emplace<WeakLambdaTarget<Class, Stored>>(owner,
                                                                     Forward<Callable>(callable));
        }
        return result;
    }

    // ---- 상태 ----

    // 바인딩이 있고 약한 대상이면 아직 살아 있다. 다른 스레드가 대상을 놓으면 곧 바뀔 수 있다.
    bool IsBound() const noexcept {
        return ops_ != nullptr && !unbindPending_ && ops_->alive(storage_);
    }

    explicit operator bool() const noexcept {
        return IsBound();
    }

    // Raw·Weak 바인딩의 대상 주소다(비교용, 역참조하지 않는다). 그 밖은 nullptr다.
    const void* BoundObject() const noexcept {
        return ops_ != nullptr && !unbindPending_ ? ops_->object(storage_) : nullptr;
    }

    // 바인딩을 해제한다. 이 delegate가 실행 중이면 가장 바깥 실행이 끝난 뒤 해제한다.
    void Unbind() noexcept {
        if (executing_ != 0) {
            unbindPending_ = true;
            return;
        }
        DestroyBinding();
    }

    // ---- 실행 ----

    Return Execute(Args... args) const noexcept {
        if (!IsBound()) {
            platform::Fatal("Delegate::Execute called on an unbound delegate");
        }
        ExecutionScope scope(*this);
        return ops_->invoke(storage_, Forward<Args>(args)...);
    }

    // 실행했으면 true다. 약한 대상이 사라졌으면 호출하지 않고 false다.
    bool ExecuteIfBound(Args... args) const noexcept
        requires(IsSameV<Return, void>)
    {
        if (ops_ == nullptr || unbindPending_) {
            return false;
        }
        ExecutionScope scope(*this);
        return ops_->invokeIfAlive(storage_, Forward<Args>(args)...);
    }

private:
    template <typename>
    friend class MulticastDelegate;

    struct Ops {
        Return (*invoke)(void* storage, Args&&... args) noexcept;
        bool (*invokeIfAlive)(void* storage, Args&&... args) noexcept;
        bool (*alive)(const void* storage) noexcept;
        const void* (*object)(const void* storage) noexcept;
        void (*relocate)(void* from, void* to) noexcept;
        void (*destroy)(void* storage) noexcept;
    };

    // ---- 대상 종류 ----

    struct StaticTarget {
        Return (*function)(Args...);

        Return Call(Args&&... args) noexcept {
            return detail::InvokeDelegateTarget<Return>(function, Forward<Args>(args)...);
        }

        bool CallIfAlive(Args&&... args) noexcept {
            function(Forward<Args>(args)...);
            return true;
        }

        bool IsAlive() const noexcept {
            return true;
        }

        const void* Object() const noexcept {
            return nullptr;
        }
    };

    template <typename Stored>
    struct LambdaTarget {
        Stored callable;

        template <typename Callable>
        explicit LambdaTarget(Callable&& value) noexcept : callable(Forward<Callable>(value)) {
        }

        Return Call(Args&&... args) noexcept {
            return detail::InvokeDelegateTarget<Return>(callable, Forward<Args>(args)...);
        }

        bool CallIfAlive(Args&&... args) noexcept {
            callable(Forward<Args>(args)...);
            return true;
        }

        bool IsAlive() const noexcept {
            return true;
        }

        const void* Object() const noexcept {
            return nullptr;
        }
    };

    template <typename Stored>
    struct HeapLambdaTarget {
        Stored* callable;
        Allocator* allocator;

        HeapLambdaTarget(Stored* value, Allocator* owner) noexcept
            : callable(value), allocator(owner) {
        }

        HeapLambdaTarget(HeapLambdaTarget&& other) noexcept
            : callable(other.callable), allocator(other.allocator) {
            other.callable = nullptr;
        }

        ~HeapLambdaTarget() noexcept {
            if (callable != nullptr) {
                DestroyAt(callable);
                if (!allocator->Deallocate(callable)) {
                    platform::Fatal("Delegate lambda storage release failed");
                }
            }
        }

        Return Call(Args&&... args) noexcept {
            return detail::InvokeDelegateTarget<Return>(*callable, Forward<Args>(args)...);
        }

        bool CallIfAlive(Args&&... args) noexcept {
            (*callable)(Forward<Args>(args)...);
            return true;
        }

        bool IsAlive() const noexcept {
            return true;
        }

        const void* Object() const noexcept {
            return nullptr;
        }
    };

    template <typename Instance, typename Method>
    struct RawTarget {
        Instance* object;
        Method method;

        Return Call(Args&&... args) noexcept {
            if constexpr (IsSameV<Return, void>) {
                (object->*method)(Forward<Args>(args)...);
            } else {
                return (object->*method)(Forward<Args>(args)...);
            }
        }

        bool CallIfAlive(Args&&... args) noexcept {
            (object->*method)(Forward<Args>(args)...);
            return true;
        }

        bool IsAlive() const noexcept {
            return true;
        }

        const void* Object() const noexcept {
            return object;
        }
    };

    template <typename Class, typename Method>
    struct WeakTarget {
        WeakPtr<Class> object;
        const void* address;
        Method method;

        WeakTarget(const SharedPtr<Class>& shared, Method value) noexcept
            : object(shared), address(shared.Get()), method(value) {
        }

        Return Call(Args&&... args) noexcept {
            SharedPtr<Class> locked = object.Lock();
            if (!locked) {
                platform::Fatal("Delegate::Execute called after its weak target expired");
            }
            if constexpr (IsSameV<Return, void>) {
                (locked.Get()->*method)(Forward<Args>(args)...);
            } else {
                return (locked.Get()->*method)(Forward<Args>(args)...);
            }
        }

        bool CallIfAlive(Args&&... args) noexcept {
            SharedPtr<Class> locked = object.Lock();
            if (!locked) {
                return false;
            }
            (locked.Get()->*method)(Forward<Args>(args)...);
            return true;
        }

        bool IsAlive() const noexcept {
            return !object.IsExpired();
        }

        const void* Object() const noexcept {
            return address;
        }
    };

    template <typename Class, typename Stored>
    struct WeakLambdaTarget {
        WeakPtr<Class> owner;
        const void* address;
        Stored callable;

        template <typename Callable>
        WeakLambdaTarget(const SharedPtr<Class>& shared, Callable&& value) noexcept
            : owner(shared), address(shared.Get()), callable(Forward<Callable>(value)) {
        }

        Return Call(Args&&... args) noexcept {
            SharedPtr<Class> locked = owner.Lock();
            if (!locked) {
                platform::Fatal("Delegate::Execute called after its weak target expired");
            }
            return detail::InvokeDelegateTarget<Return>(callable, Forward<Args>(args)...);
        }

        bool CallIfAlive(Args&&... args) noexcept {
            SharedPtr<Class> locked = owner.Lock();
            if (!locked) {
                return false;
            }
            callable(Forward<Args>(args)...);
            return true;
        }

        bool IsAlive() const noexcept {
            return !owner.IsExpired();
        }

        const void* Object() const noexcept {
            return address;
        }
    };

    template <typename Target>
    static Target& TargetAt(void* storage) noexcept {
        return *__builtin_launder(static_cast<Target*>(storage));
    }

    template <typename Target>
    static const Target& TargetAt(const void* storage) noexcept {
        return *__builtin_launder(static_cast<const Target*>(storage));
    }

    template <typename Target>
    static constexpr Ops OpsFor = {
        [](void* storage, Args&&... args) noexcept -> Return {
            return TargetAt<Target>(storage).Call(Forward<Args>(args)...);
        },
        [](void* storage, Args&&... args) noexcept -> bool {
            return TargetAt<Target>(storage).CallIfAlive(Forward<Args>(args)...);
        },
        [](const void* storage) noexcept -> bool { return TargetAt<Target>(storage).IsAlive(); },
        [](const void* storage) noexcept -> const void* {
            return TargetAt<Target>(storage).Object();
        },
        [](void* from, void* to) noexcept {
            Target& source = TargetAt<Target>(from);
            ConstructAt<Target>(to, Move(source));
            DestroyAt(&source);
        },
        [](void* storage) noexcept { DestroyAt(&TargetAt<Target>(storage)); },
    };

    template <typename Target, typename... TargetArgs>
    void Emplace(TargetArgs&&... arguments) noexcept {
        static_assert(sizeof(Target) <= DelegateInlineBytes,
                      "Delegate target is too large for inline storage; use TryCreateLambda");
        static_assert(alignof(Target) <= 16, "Delegate target alignment exceeds inline storage");
        static_assert(IsNothrowMoveConstructibleV<Target>,
                      "Delegate target needs a noexcept move constructor");
        static_assert(IsNothrowDestructibleV<Target>, "Delegate target needs noexcept destruction");
        ConstructAt<Target>(storage_, Forward<TargetArgs>(arguments)...);
        ops_ = &OpsFor<Target>;
    }

    void TakeFrom(Delegate& other) noexcept {
        if (other.ops_ != nullptr) {
            other.ops_->relocate(other.storage_, storage_);
            ops_ = other.ops_;
            unbindPending_ = other.unbindPending_;
            other.ops_ = nullptr;
            other.unbindPending_ = false;
        }
    }

    // 대상 소멸자가 재진입해 이 delegate를 다시 바꾸더라도 이중 소멸하지 않게 먼저 비운다.
    void DestroyBinding() const noexcept {
        const Ops* ops = ops_;
        ops_ = nullptr;
        unbindPending_ = false;
        if (ops != nullptr) {
            ops->destroy(storage_);
        }
    }

    void CheckNotExecuting(const char* message) const noexcept {
        if (executing_ != 0) {
            platform::Fatal(message);
        }
    }

    struct ExecutionScope {
        const Delegate& owner;

        explicit ExecutionScope(const Delegate& delegate) noexcept : owner(delegate) {
            ++owner.executing_;
        }

        ExecutionScope(const ExecutionScope&) = delete;
        ExecutionScope& operator=(const ExecutionScope&) = delete;

        ~ExecutionScope() noexcept {
            if (--owner.executing_ == 0 && owner.unbindPending_) {
                owner.DestroyBinding();
            }
        }
    };

    alignas(16) mutable unsigned char storage_[DelegateInlineBytes];
    mutable const Ops* ops_ = nullptr;
    mutable uint32 executing_ = 0;
    mutable bool unbindPending_ = false;
};

// 여러 대상을 등록해 한 번에 호출하는 delegate다(UE TMulticastDelegate 대응). 반환값은 없다.
// 사용 예:
//   MulticastDelegate<void(int)> onDamaged(allocator);
//   auto handle = onDamaged.TryAddLambda([](int amount) noexcept { Log(amount); });
//   onDamaged.Broadcast(10);
//   onDamaged.Remove(handle.Value());
//
// 계약:
// - 등록 항목은 개별 할당되어 주소가 고정된다. Broadcast 중 추가·제거해도 실행 중인 대상을 옮기지
//   않는다. Broadcast 중 추가한 항목은 다음 Broadcast부터 호출되고, 제거한 항목은 이후 순서에서
//   호출되지 않는다. 실제 정리(소멸·반환)는 가장 바깥 Broadcast가 끝난 뒤에 한다.
// - 호출 순서는 등록 순서다. 약한 대상이 사라진 항목은 건너뛰고 다음 정리 때 제거된다.
// - 같은 MulticastDelegate를 대상 안에서 다시 Broadcast할 수 있다(중첩 깊이만큼 정리 지연).
//   Broadcast 중 이동·파괴는 Fatal이다.
// - 인자는 각 대상에 lvalue로 전달된다(값 인자는 대상마다 복사). rvalue 참조 인자는 지원하지
// 않는다.
// - allocator는 이 객체보다 오래 살아야 한다. 내부 동기화는 없다.
template <typename... Args>
class MulticastDelegate<void(Args...)> {
    static_assert((!IsRvalueReferenceV<Args> && ...),
                  "MulticastDelegate does not support rvalue reference parameters");

public:
    using DelegateType = Delegate<void(Args...)>;

    explicit MulticastDelegate(Allocator& allocator) noexcept
        : allocator_(&allocator), entries_(allocator) {
    }

    MulticastDelegate(const MulticastDelegate&) = delete;
    MulticastDelegate& operator=(const MulticastDelegate&) = delete;

    MulticastDelegate(MulticastDelegate&& other) noexcept
        : allocator_(other.allocator_), entries_(*other.allocator_), nextId_(other.nextId_) {
        other.CheckNotBroadcasting("MulticastDelegate moved during Broadcast");
        entries_ = Move(other.entries_);
    }

    MulticastDelegate& operator=(MulticastDelegate&&) = delete;

    ~MulticastDelegate() noexcept {
        CheckNotBroadcasting("MulticastDelegate destroyed during Broadcast");
        for (Entry* entry : entries_) {
            FreeEntry(entry);
        }
    }

    // delegate를 등록한다. 실패: 바인딩 없는 delegate는 InvalidArgument, 할당 실패는 그 오류
    // (delegate는 바뀌지 않는다).
    Result<DelegateHandle> TryAdd(DelegateType&& delegate) noexcept {
        if (!delegate.IsBound()) {
            return Result<DelegateHandle>::Failure(ErrorCode::InvalidArgument);
        }
        // Broadcast 중에는 배열이 커져도 항목 주소는 그대로이고 Broadcast는 index로 다시 읽는다.
        auto reserved = entries_.TryReserve(entries_.Size() + 1);
        if (!reserved) {
            return Result<DelegateHandle>::Failure(reserved.Error());
        }
        auto memory = allocator_->Allocate(sizeof(Entry), alignof(Entry));
        if (!memory) {
            return Result<DelegateHandle>::Failure(memory.Error());
        }
        Entry* entry = ConstructAt<Entry>(memory.Value(), nextId_++, Move(delegate));
        if (!entries_.TryPushBack(entry)) {
            platform::Fatal("MulticastDelegate reserved push failed");
        }
        return Result<DelegateHandle>::Success(DelegateHandle(entry->id));
    }

    template <typename Callable>
    Result<DelegateHandle> TryAddLambda(Callable&& callable) noexcept {
        return TryAdd(DelegateType::CreateLambda(Forward<Callable>(callable)));
    }

    Result<DelegateHandle> TryAddStatic(void (*function)(Args...)) noexcept {
        return TryAdd(DelegateType::CreateStatic(function));
    }

    template <typename Object, typename Method>
    Result<DelegateHandle> TryAddRaw(Object* object, Method method) noexcept {
        return TryAdd(DelegateType::CreateRaw(object, method));
    }

    template <typename Class, typename Method>
    Result<DelegateHandle> TryAddWeak(const SharedPtr<Class>& object, Method method) noexcept {
        return TryAdd(DelegateType::CreateWeak(object, method));
    }

    template <typename Class, typename Callable>
    Result<DelegateHandle> TryAddWeakLambda(const SharedPtr<Class>& owner,
                                            Callable&& callable) noexcept {
        return TryAdd(DelegateType::CreateWeakLambda(owner, Forward<Callable>(callable)));
    }

    // handle의 항목을 제거한다. 이미 없거나 무효면 false다.
    bool Remove(DelegateHandle handle) noexcept {
        if (!handle.IsValid()) {
            return false;
        }
        for (Entry* entry : entries_) {
            if (entry->id == handle.id_) {
                Retire(*entry);
                CompactIfIdle();
                return true;
            }
        }
        return false;
    }

    // Raw·Weak 바인딩 대상이 object인 항목을 모두 제거하고 제거 수를 반환한다(UE RemoveAll).
    usize RemoveAll(const void* object) noexcept {
        if (object == nullptr) {
            return 0;
        }
        usize removed = 0;
        for (Entry* entry : entries_) {
            if (entry->id != 0 && entry->delegate.BoundObject() == object) {
                Retire(*entry);
                ++removed;
            }
        }
        CompactIfIdle();
        return removed;
    }

    void Clear() noexcept {
        for (Entry* entry : entries_) {
            if (entry->id != 0) {
                Retire(*entry);
            }
        }
        CompactIfIdle();
    }

    // 호출될 항목(제거되지 않았고 약한 대상이 살아 있음)이 하나라도 있으면 true다.
    bool IsBound() const noexcept {
        for (const Entry* entry : entries_) {
            if (entry->id != 0 && entry->delegate.IsBound()) {
                return true;
            }
        }
        return false;
    }

    void Broadcast(Args... args) const noexcept {
        ++broadcasting_;
        // 시작 시점의 항목만 호출한다. 추가는 뒤에 붙고 배열이 커져도 index로 다시 읽는다.
        const usize count = entries_.Size();
        for (usize index = 0; index < count; ++index) {
            Entry* entry = entries_[index];
            if (entry->id != 0) {
                entry->delegate.ExecuteIfBound(args...);
            }
        }
        if (--broadcasting_ == 0) {
            Compact();
        }
    }

private:
    struct Entry {
        uint64 id; // 0이면 제거됨
        DelegateType delegate;

        Entry(uint64 value, DelegateType&& target) noexcept : id(value), delegate(Move(target)) {
        }
    };

    void Retire(Entry& entry) noexcept {
        entry.id = 0;
        entry.delegate.Unbind(); // 실행 중이면 그 실행이 끝난 뒤 해제된다.
    }

    void CompactIfIdle() const noexcept {
        if (broadcasting_ == 0) {
            Compact();
        }
    }

    // 제거된 항목과 약한 대상이 사라진 항목을 반환한다. 할당하지 않는다.
    void Compact() const noexcept {
        usize kept = 0;
        const usize size = entries_.Size();
        for (usize index = 0; index < size; ++index) {
            Entry* entry = entries_[index];
            if (entry->id != 0 && entry->delegate.IsBound()) {
                entries_[kept++] = entry;
            } else {
                FreeEntry(entry);
            }
        }
        if (!entries_.TryRemoveAt(kept, size - kept)) {
            platform::Fatal("MulticastDelegate compaction failed");
        }
    }

    void FreeEntry(Entry* entry) const noexcept {
        DestroyAt(entry);
        if (!allocator_->Deallocate(entry)) {
            platform::Fatal("MulticastDelegate entry release failed");
        }
    }

    void CheckNotBroadcasting(const char* message) const noexcept {
        if (broadcasting_ != 0) {
            platform::Fatal(message);
        }
    }

    Allocator* allocator_;
    // Broadcast(const)가 끝난 뒤 정리하므로 mutable이다.
    mutable Array<Entry*> entries_;
    uint64 nextId_ = 1;
    mutable uint32 broadcasting_ = 0;
};

} // namespace rocks
