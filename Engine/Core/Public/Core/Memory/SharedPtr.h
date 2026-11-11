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
class SharedRef;

template <typename T>
class EnableSharedFromThis;

namespace detail {

// SharedPtr/WeakPtr가 공유하는 제어 블록이다. TryMake는 객체 저장소와 같은 할당에, TryAdopt는
// deleter와 같은 할당에 둔다. destroyObject는 제어 블록을 받아 객체(와 deleter)를 정리한다.
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
    void (*destroyObject)(SharedControlBlock* control) noexcept;

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
            destroyObject(this);
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

    static void Destroy(SharedControlBlock* control) noexcept {
        DestroyAt(static_cast<T*>(control->object));
    }
};

// TryAdopt의 제어 블록이다. 제어 블록이 기반이므로 할당 시작 주소와 같다(TryAdopt가 확인).
// 마지막 strong 해제 때 deleter로 객체를 해제한 뒤 deleter 자신도 소멸한다. 저장소 반환은 마지막
// weak 해제 때 SharedControlBlock::ReleaseWeak가 한다.
template <typename T, typename Deleter>
struct SharedAdoptedStorage : SharedControlBlock {
    Deleter deleter;

    explicit SharedAdoptedStorage(Deleter&& value) noexcept
        : SharedControlBlock{}, deleter(Move(value)) {
    }

    static void Destroy(SharedControlBlock* control) noexcept;
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

    template <typename, typename>
    friend struct SharedAdoptedStorage;

    void AttachSharedControl(SharedControlBlock* control) noexcept {
        if (control_ == nullptr) {
            control_ = control;
            control->AddWeak();
        }
    }

    // TryAdopt 객체의 마지막 strong 해제 때 deleter보다 먼저 호출한다. deleter가 객체를 파괴하지
    // 않아도(외부 소유 객체) 객체가 쥔 weak 토큰 때문에 제어 블록이 남지 않게 한다. strong 그룹의
    // weak 토큰이 아직 있으므로 여기서 저장소가 반환되지는 않는다.
    void DetachSharedControl(SharedControlBlock* control) noexcept {
        if (control_ == control) {
            control_ = nullptr;
            control->ReleaseWeak();
        }
    }
};

template <typename T, typename Deleter>
void SharedAdoptedStorage<T, Deleter>::Destroy(SharedControlBlock* control) noexcept {
    auto* storage = static_cast<SharedAdoptedStorage*>(control);
    T* object = static_cast<T*>(control->object);
    if constexpr (__is_base_of(SharedFromThisAnchor, T)) {
        object->DetachSharedControl(control);
    }
    storage->deleter(object);
    DestroyAt(&storage->deleter);
}

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

    // 외부에서 만든 object의 소유를 넘겨받아 마지막 SharedPtr가 사라질 때 deleter(object)로
    // 해제한다(UE MakeShareable(object, deleter) 대응). 제어 블록과 deleter를 allocator에 한 번
    // 할당한다. object가 nullptr이면 빈 SharedPtr이며 deleter는 호출하지 않는다.
    // 실패: Allocate 오류. 이때 소유권은 호출자에게 남고 deleter는 호출되지 않는다.
    // deleter(T*)는 noexcept 호출 가능해야 하며 deleter의 이동·소멸도 noexcept여야 한다.
    template <typename Deleter>
    static Result<SharedPtr> TryAdopt(Allocator& allocator, T* object, Deleter deleter) noexcept {
        static_assert(IsNothrowMoveConstructibleV<Deleter>,
                      "SharedPtr deleter needs noexcept move");
        static_assert(IsNothrowDestructibleV<Deleter>,
                      "SharedPtr deleter needs noexcept destruction");
        static_assert(noexcept(DeclVal<Deleter&>()(static_cast<T*>(nullptr))),
                      "SharedPtr deleter must be noexcept callable with T*");
        if (object == nullptr) {
            return Result<SharedPtr>::Success(SharedPtr());
        }
        if constexpr (__is_pointer(Deleter)) {
            if (deleter == nullptr) {
                platform::Fatal("SharedPtr adopted an object with a null deleter");
            }
        }
        using Storage = detail::SharedAdoptedStorage<T, Deleter>;
        auto memory = allocator.Allocate(sizeof(Storage), alignof(Storage));
        if (!memory) {
            return Result<SharedPtr>::Failure(memory.Error());
        }
        Storage* storage = ConstructAt<Storage>(memory.Value(), Move(deleter));
        detail::SharedControlBlock* control = storage;
        if (static_cast<void*>(control) != memory.Value()) {
            platform::Fatal("SharedPtr adopted control block is not at the allocation start");
        }
        control->strong = 1;
        control->weak = 1;
        control->allocator = &allocator;
        control->object = object;
        control->destroyObject = &Storage::Destroy;
        if constexpr (__is_base_of(detail::SharedFromThisAnchor, T)) {
            object->AttachSharedControl(control);
        }
        return Result<SharedPtr>::Success(SharedPtr(object, control));
    }

    // aliasing 생성이다(UE TSharedPtr 대응). owner와 소유(계수)를 공유하면서 object를 가리킨다.
    // object는 보통 owner 객체의 멤버이며 owner 객체가 사는 동안 유효해야 한다. owner가 비었거나
    // object가 nullptr이면 소유하지 않는 빈 포인터다.
    template <typename U>
    SharedPtr(const SharedPtr<U>& owner, T* object) noexcept {
        if (owner.control_ != nullptr && object != nullptr) {
            object_ = object;
            control_ = owner.control_;
            control_->AddStrong();
        }
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

    // null이 될 수 없는 SharedRef로 바꾼다(UE TSharedPtr::ToSharedRef 대응). 빈 포인터면 Fatal이다.
    // 비어 있을 수 있는 값은 SharedRef<T>::TryFrom으로 실패를 값으로 받는다.
    SharedRef<T> ToSharedRef() const noexcept;

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

    template <typename U>
        requires(IsSameV<U, T> || __is_convertible_to(U*, T*))
    WeakPtr(const SharedRef<U>& shared) noexcept : WeakPtr(shared.AsSharedPtr()) {
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

// null이 될 수 없는 공유 소유권이다(UE TSharedRef<T, ESPMode::ThreadSafe> 대응). 계수·스레드·
// 수명 계약은 SharedPtr와 같다.
// 사용 예:
//   auto created = SharedRef<Texture>::TryMake(allocator, width, height);
//   if (!created) { return created.Error(); }
//   SharedRef<Texture> texture = created.Value();
//   Upload(*texture);                        // 빈 검사 없이 역참조
//   SharedPtr<Texture> maybe = texture;      // SharedPtr로는 암묵 변환
//   auto again = SharedRef<Texture>::TryFrom(maybe);  // 빈 SharedPtr면 InvalidArgument
//
// 계약:
// - 기본 생성이 없다. 만드는 방법은 TryMake, TryFrom, SharedPtr::ToSharedRef(빈 포인터는 Fatal)다.
// - 이동도 복사와 같다(UE와 같다). 이동 후에도 원본은 같은 객체를 가리키므로 "이동한 뒤 비어
//   있음"을 가정하지 않는다. 소유를 놓으려면 SharedRef를 담은 범위를 끝내거나 SharedPtr로 바꾼다.
// - null이 아니라는 것은 소유권만 보장한다. 같은 인스턴스를 여러 스레드에서 동시에 대입하는 것과
//   객체 자체의 동기화는 SharedPtr와 같이 호출자 책임이다.
template <typename T>
class SharedRef {
public:
    SharedRef() = delete;

    // 객체와 제어 블록을 한 번에 할당한다(SharedPtr::TryMake와 같다).
    template <typename... Args>
    static Result<SharedRef> TryMake(Allocator& allocator, Args&&... args) noexcept {
        auto created = SharedPtr<T>::TryMake(allocator, Forward<Args>(args)...);
        if (!created) {
            return Result<SharedRef>::Failure(created.Error());
        }
        return Result<SharedRef>::Success(SharedRef(Move(created.Value())));
    }

    // 빈 SharedPtr는 InvalidArgument다.
    template <typename U>
        requires(IsSameV<U, T> || __is_convertible_to(U*, T*))
    static Result<SharedRef> TryFrom(const SharedPtr<U>& pointer) noexcept {
        if (!pointer) {
            return Result<SharedRef>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<SharedRef>::Success(SharedRef(SharedPtr<T>(pointer)));
    }

    SharedRef(const SharedRef& other) noexcept : pointer_(other.pointer_) {
    }

    // 이동은 복사다. 원본을 비우면 null 불가 불변식이 깨진다.
    SharedRef(SharedRef&& other) noexcept : pointer_(other.pointer_) {
    }

    template <typename U>
        requires(!IsSameV<U, T> && __is_convertible_to(U*, T*))
    SharedRef(const SharedRef<U>& other) noexcept : pointer_(other.AsSharedPtr()) {
    }

    // SharedPtr의 복사 후 교환 대입을 쓰므로 자기 대입과 기존 객체 소멸 중 재진입에도 안전하다.
    SharedRef& operator=(const SharedRef& other) noexcept {
        pointer_ = other.pointer_;
        return *this;
    }

    SharedRef& operator=(SharedRef&& other) noexcept {
        pointer_ = other.pointer_;
        return *this;
    }

    ~SharedRef() noexcept = default;

    T& Get() const noexcept {
        return *pointer_.Get();
    }

    T& operator*() const noexcept {
        return *pointer_.Get();
    }

    T* operator->() const noexcept {
        return pointer_.Get();
    }

    // 내부 SharedPtr(항상 비어 있지 않음)를 빌린다. WeakPtr 생성 등에 복사 없이 쓴다.
    const SharedPtr<T>& AsSharedPtr() const noexcept {
        return pointer_;
    }

    operator SharedPtr<T>() const noexcept {
        return pointer_;
    }

    // 기반 타입 SharedPtr로의 변환이다(SharedPtr<Base> p = SharedRef<Derived>).
    template <typename U>
        requires(!IsSameV<U, T> && __is_convertible_to(T*, U*))
    operator SharedPtr<U>() const noexcept {
        return SharedPtr<U>(pointer_);
    }

    // 진단용 순간 값이다. 항상 1 이상이다.
    uint64 UseCount() const noexcept {
        return pointer_.UseCount();
    }

    friend bool operator==(const SharedRef& left, const SharedRef& right) noexcept {
        return left.pointer_ == right.pointer_;
    }

    friend bool operator==(const SharedRef& left, const SharedPtr<T>& right) noexcept {
        return left.pointer_ == right;
    }

private:
    template <typename>
    friend class SharedPtr;

    // pointer는 비어 있지 않아야 한다(호출자가 확인).
    explicit SharedRef(SharedPtr<T>&& pointer) noexcept : pointer_(Move(pointer)) {
    }

    SharedPtr<T> pointer_;
};

template <typename T>
SharedRef<T> SharedPtr<T>::ToSharedRef() const noexcept {
    if (object_ == nullptr) {
        platform::Fatal("SharedPtr::ToSharedRef called on an empty pointer");
    }
    return SharedRef<T>(SharedPtr(*this));
}

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
