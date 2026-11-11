#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

template <typename T>
class SharedPtr;

template <typename T>
class WeakPtr;

template <typename T>
class EnableSharedFromThis;

namespace detail {

// SharedPtr/WeakPtr가 공유하는 제어 블록이다. 객체 저장소와 같은 할당에 있다.
// - strong: 살아 있는 SharedPtr 수. 0이 되면 객체를 소멸한다.
// - weak: 살아 있는 WeakPtr 수 + (strong > 0이면 1). 0이 되면 할당 전체를 allocator에 반환한다.
// 계수는 compiler 원자 builtin으로 갱신한다. 일반 증가는 relaxed, WeakPtr::Lock의 조건부
// 증가(CAS)와 감소는 acq_rel이며 마지막 감소가 이전 사용자의 모든 쓰기를 관찰한 뒤 소멸·반환을
// 수행한다. 계수는 64-bit라 실사용에서 넘치지 않는다.
struct SharedControlBlock {
    uint64 strong;
    uint64 weak;
    Allocator* allocator;
    void* object;
    void (*destroyObject)(void* object) noexcept;

    void AddStrong() noexcept {
        __atomic_fetch_add(&strong, 1, __ATOMIC_RELAXED);
    }

    void AddWeak() noexcept {
        __atomic_fetch_add(&weak, 1, __ATOMIC_RELAXED);
    }

    // strong이 0이 아닐 때만 1 증가시킨다. WeakPtr::Lock이 이미 소멸 중인 객체를 되살리지 않게
    // 한다.
    bool TryAddStrong() noexcept {
        uint64 expected = __atomic_load_n(&strong, __ATOMIC_RELAXED);
        while (expected != 0) {
            if (__atomic_compare_exchange_n(&strong, &expected, expected + 1, true,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                return true;
            }
        }
        return false;
    }

    void ReleaseStrong() noexcept {
        if (__atomic_fetch_sub(&strong, 1, __ATOMIC_ACQ_REL) == 1) {
            // 객체 소멸자가 같은 객체의 WeakPtr를 해제해도 strong 그룹의 weak 토큰이 남아 있어
            // 저장소는 아직 유효하다. 소멸이 끝난 뒤 그 토큰을 반환한다.
            destroyObject(object);
            ReleaseWeak();
        }
    }

    void ReleaseWeak() noexcept {
        if (__atomic_fetch_sub(&weak, 1, __ATOMIC_ACQ_REL) == 1) {
            Allocator* owner = allocator;
            if (!owner->Deallocate(this)) {
                platform::Fatal("SharedPtr storage deallocation failed");
            }
        }
    }

    uint64 StrongCount() const noexcept {
        return __atomic_load_n(&strong, __ATOMIC_ACQUIRE);
    }
};

template <typename T>
struct SharedStorage {
    SharedControlBlock control;
    alignas(T) unsigned char object[sizeof(T)];

    static void Destroy(void* object) noexcept {
        DestroyAt(static_cast<T*>(object));
    }
};

// EnableSharedFromThis의 비템플릿 기반이다. SharedPtr::TryMake가 생성 직후 제어 블록을 연결한다.
// 객체는 weak 토큰 하나만 보유하므로 자기 자신을 강하게 소유하는 순환을 만들지 않는다.
class SharedFromThisAnchor {
protected:
    SharedFromThisAnchor() noexcept = default;

    // 복사·대입된 객체는 원본과 다른 소유권을 가지므로 연결을 복사하지 않는다.
    SharedFromThisAnchor(const SharedFromThisAnchor&) noexcept {
    }

    SharedFromThisAnchor& operator=(const SharedFromThisAnchor&) noexcept {
        return *this;
    }

    ~SharedFromThisAnchor() noexcept {
        if (control_ != nullptr) {
            control_->ReleaseWeak();
        }
    }

    SharedControlBlock* control_ = nullptr;

private:
    template <typename>
    friend class rocks::SharedPtr;

    void AttachSharedControl(SharedControlBlock* control) noexcept {
        if (control_ == nullptr) {
            control_ = control;
            control->AddWeak();
        }
    }
};

} // namespace detail

// 참조 계수 공유 소유권이다(UE TSharedPtr<T, ESPMode::ThreadSafe> 대응).
// 사용 예:
//   auto created = SharedPtr<Texture>::TryMake(allocator, width, height);
//   if (!created) { return created.Error(); }
//   SharedPtr<Texture> texture = created.Value();   // 복사는 참조 계수 증가
//   WeakPtr<Texture> observer = texture;            // 소유하지 않는 관찰
//   if (SharedPtr<Texture> locked = observer.Lock()) { Use(*locked); }
//
// 계약:
// - allocator는 해당 객체를 가리키는 모든 SharedPtr·WeakPtr보다 오래 살아야 한다.
// - 서로 다른 SharedPtr/WeakPtr 인스턴스는 같은 객체를 가리켜도 여러 스레드에서 동시에 복사·
//   소멸·Lock할 수 있다. 같은 인스턴스를 동시에 변경하는 것과 객체 자체의 동기화는 호출자 책임이다.
// - 마지막 SharedPtr를 놓는 스레드에서 객체 소멸자가 실행된다. 강한 순환 참조는 회수되지 않으므로
//   역참조에는 WeakPtr를 쓴다. GC 객체 참조(RootRef/WeakRef)가 아니며 Object 시스템에 쓰지 않는다.
// - 파생 → 기반 변환은 원래 타입의 소멸 함수를 유지하므로 기반 타입에 가상 소멸자가 없어도 된다.
// - 빈 포인터 역참조는 Fatal이다. UseCount는 진단용 순간 값이며 동기화 판단에 쓰지 않는다.
template <typename T>
class SharedPtr {
public:
    constexpr SharedPtr() noexcept = default;

    constexpr SharedPtr(decltype(nullptr)) noexcept {
    }

    // 객체와 제어 블록을 한 번에 할당한다. 실패: Allocate 오류를 그대로 반환하며 아무것도 남기지
    // 않는다.
    template <typename... Args>
    static Result<SharedPtr> TryMake(Allocator& allocator, Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...),
                      "SharedPtr::TryMake requires a noexcept constructor");
        using Storage = detail::SharedStorage<T>;
        auto memory = allocator.Allocate(sizeof(Storage), alignof(Storage));
        if (!memory) {
            return Result<SharedPtr>::Failure(memory.Error());
        }
        // 저장소 객체의 수명을 먼저 시작한 뒤 그 안에 T를 생성한다. 제어 블록은 첫 멤버라 할당
        // 주소와 같으며, 마지막 ReleaseWeak가 그 주소로 저장소를 반환한다.
        static_assert(__builtin_offsetof(Storage, control) == 0);
        Storage* storage = ConstructAt<Storage>(memory.Value());
        T* object = ConstructAt<T>(storage->object, Forward<Args>(args)...);
        storage->control = detail::SharedControlBlock{1, 1, &allocator, object, &Storage::Destroy};
        if constexpr (__is_base_of(detail::SharedFromThisAnchor, T)) {
            object->AttachSharedControl(&storage->control);
        }
        return Result<SharedPtr>::Success(SharedPtr(object, &storage->control));
    }

    SharedPtr(const SharedPtr& other) noexcept : object_(other.object_), control_(other.control_) {
        if (control_ != nullptr) {
            control_->AddStrong();
        }
    }

    SharedPtr(SharedPtr&& other) noexcept : object_(other.object_), control_(other.control_) {
        other.Clear();
    }

    template <typename U>
        requires(!IsSameV<U, T> && __is_convertible_to(U*, T*))
    SharedPtr(const SharedPtr<U>& other) noexcept
        : object_(other.object_), control_(other.control_) {
        if (control_ != nullptr) {
            control_->AddStrong();
        }
    }

    template <typename U>
        requires(!IsSameV<U, T> && __is_convertible_to(U*, T*))
    SharedPtr(SharedPtr<U>&& other) noexcept : object_(other.object_), control_(other.control_) {
        other.Clear();
    }

    // 복사 후 교환 방식이라 자기 대입과 대입 중 기존 객체 소멸의 재진입에도 안전하다.
    SharedPtr& operator=(const SharedPtr& other) noexcept {
        SharedPtr copy(other);
        Swap(copy);
        return *this;
    }

    SharedPtr& operator=(SharedPtr&& other) noexcept {
        SharedPtr moved(Move(other));
        Swap(moved);
        return *this;
    }

    SharedPtr& operator=(decltype(nullptr)) noexcept {
        Reset();
        return *this;
    }

    ~SharedPtr() noexcept {
        Reset();
    }

    void Reset() noexcept {
        detail::SharedControlBlock* control = control_;
        Clear();
        if (control != nullptr) {
            control->ReleaseStrong();
        }
    }

    T* Get() const noexcept {
        return object_;
    }

    explicit operator bool() const noexcept {
        return object_ != nullptr;
    }

    T& operator*() const noexcept {
        Check();
        return *object_;
    }

    T* operator->() const noexcept {
        Check();
        return object_;
    }

    // 진단용 순간 값이다. 빈 포인터는 0이다.
    uint64 UseCount() const noexcept {
        return control_ != nullptr ? control_->StrongCount() : 0;
    }

    friend bool operator==(const SharedPtr& left, const SharedPtr& right) noexcept {
        return left.object_ == right.object_;
    }

    friend bool operator==(const SharedPtr& pointer, decltype(nullptr)) noexcept {
        return pointer.object_ == nullptr;
    }

private:
    template <typename>
    friend class SharedPtr;

    template <typename>
    friend class WeakPtr;

    template <typename>
    friend class EnableSharedFromThis;

    SharedPtr(T* object, detail::SharedControlBlock* control) noexcept
        : object_(object), control_(control) {
    }

    void Swap(SharedPtr& other) noexcept {
        T* object = object_;
        detail::SharedControlBlock* control = control_;
        object_ = other.object_;
        control_ = other.control_;
        other.object_ = object;
        other.control_ = control;
    }

    void Clear() noexcept {
        object_ = nullptr;
        control_ = nullptr;
    }

    void Check() const noexcept {
        if (object_ == nullptr) {
            platform::Fatal("SharedPtr dereferenced while empty");
        }
    }

    T* object_ = nullptr;
    detail::SharedControlBlock* control_ = nullptr;
};

// 객체를 소유하지 않는 관찰자다(UE TWeakPtr 대응). 객체 접근은 Lock으로 SharedPtr를 얻어 수행한다.
// 객체가 소멸되면 Lock은 빈 SharedPtr를 반환하며, 저장소는 마지막 WeakPtr가 사라질 때 반환된다.
template <typename T>
class WeakPtr {
public:
    constexpr WeakPtr() noexcept = default;

    template <typename U>
        requires(IsSameV<U, T> || __is_convertible_to(U*, T*))
    WeakPtr(const SharedPtr<U>& shared) noexcept
        : object_(shared.object_), control_(shared.control_) {
        if (control_ != nullptr) {
            control_->AddWeak();
        }
    }

    WeakPtr(const WeakPtr& other) noexcept : object_(other.object_), control_(other.control_) {
        if (control_ != nullptr) {
            control_->AddWeak();
        }
    }

    WeakPtr(WeakPtr&& other) noexcept : object_(other.object_), control_(other.control_) {
        other.object_ = nullptr;
        other.control_ = nullptr;
    }

    WeakPtr& operator=(const WeakPtr& other) noexcept {
        WeakPtr copy(other);
        Swap(copy);
        return *this;
    }

    WeakPtr& operator=(WeakPtr&& other) noexcept {
        WeakPtr moved(Move(other));
        Swap(moved);
        return *this;
    }

    ~WeakPtr() noexcept {
        Reset();
    }

    void Reset() noexcept {
        detail::SharedControlBlock* control = control_;
        object_ = nullptr;
        control_ = nullptr;
        if (control != nullptr) {
            control->ReleaseWeak();
        }
    }

    // 객체가 아직 살아 있으면 새 소유권을, 아니면 빈 SharedPtr를 반환한다. 다른 스레드의 마지막
    // 해제와 경합해도 소멸 중인 객체를 반환하지 않는다.
    SharedPtr<T> Lock() const noexcept {
        if (control_ == nullptr || !control_->TryAddStrong()) {
            return SharedPtr<T>();
        }
        return SharedPtr<T>(object_, control_);
    }

    // 순간 값이다. false를 받아도 곧바로 만료될 수 있으므로 접근 전에는 Lock 결과를 검사한다.
    bool IsExpired() const noexcept {
        return control_ == nullptr || control_->StrongCount() == 0;
    }

private:
    template <typename>
    friend class EnableSharedFromThis;

    // 이미 살아 있는 제어 블록에 weak 토큰을 하나 더한다.
    WeakPtr(T* object, detail::SharedControlBlock* control) noexcept
        : object_(object), control_(control) {
        control_->AddWeak();
    }

    void Swap(WeakPtr& other) noexcept {
        T* object = object_;
        detail::SharedControlBlock* control = control_;
        object_ = other.object_;
        control_ = other.control_;
        other.object_ = object;
        other.control_ = control;
    }

    T* object_ = nullptr;
    detail::SharedControlBlock* control_ = nullptr;
};

// 객체가 자신을 가리키는 SharedPtr/WeakPtr를 얻게 한다(UE TSharedFromThis 대응).
// 사용 예: class Actor : public EnableSharedFromThis<Actor> { ... SharedFromThis() ... };
// - SharedPtr::TryMake로 만든 객체에서만 연결된다. 스택·UniquePtr·직접 생성한 객체는 빈 포인터를
//   반환한다(UE는 check 실패). 소멸자 실행 중에도 strong 계수가 0이므로 빈 포인터를 반환한다.
// - 생성자 안에서는 아직 연결 전이므로 빈 포인터다. T는 이 타입을 public 기반으로 가져야 한다.
template <typename T>
class EnableSharedFromThis : public detail::SharedFromThisAnchor {
public:
    SharedPtr<T> SharedFromThis() noexcept {
        if (control_ == nullptr || !control_->TryAddStrong()) {
            return SharedPtr<T>();
        }
        return SharedPtr<T>(static_cast<T*>(this), control_);
    }

    WeakPtr<T> WeakFromThis() noexcept {
        if (control_ == nullptr) {
            return WeakPtr<T>();
        }
        return WeakPtr<T>(static_cast<T*>(this), control_);
    }

protected:
    EnableSharedFromThis() noexcept = default;
};

} // namespace rocks
