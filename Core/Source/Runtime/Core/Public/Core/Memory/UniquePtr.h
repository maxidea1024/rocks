#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

// UniquePtr의 기본 deleter다. TryMake가 받은 allocator와 할당 시작 주소를 기억해 소멸 후 반환한다.
// 파생 → 기반 이동 뒤에도 원래 할당 주소로 반환하므로 기반이 첫 기반이 아니어도 된다.
struct AllocatorDeleter {
    void* allocation = nullptr;
    Allocator* allocator = nullptr;

    template <typename T>
    void operator()(T* object) noexcept {
        DestroyAt(object);
        if (!allocator->Deallocate(allocation)) {
            platform::Fatal("UniquePtr storage deallocation failed");
        }
    }
};

// 객체 하나의 단독 소유권이다(UE TUniquePtr<T, Deleter> 대응).
// 사용 예:
//   auto created = UniquePtr<Enemy>::TryMake(allocator, 100);
//   if (!created) { return created.Error(); }
//   UniquePtr<Enemy> enemy = Move(created.Value());
//
//   // 외부 라이브러리 자원: 해제 함수를 deleter로 맡긴다.
//   using FontHandle = UniquePtr<Font, void (*)(Font*) noexcept>;
//   FontHandle font(OpenFont(path), &CloseFont);
//
// 계약:
// - 기본 deleter(AllocatorDeleter): allocator는 이 포인터와 객체보다 오래 살아야 한다. 소멸·Reset은
//   객체 소멸자를 부른 뒤 같은 allocator에 저장소를 반환하며, 반환이 실패하면 누수를 숨기지 않고
//   Fatal이다. 생성자는 noexcept여야 한다.
// - 사용자 deleter: UniquePtr(object, deleter)로 소유를 넘긴다. deleter(object)는 noexcept 호출
//   가능해야 하고 deleter의 이동·소멸도 noexcept여야 한다. 함수 포인터 deleter가 nullptr인데 객체가
//   있으면 Fatal이다. Release로 소유를 돌려받을 수 있다(호출자가 직접 해제해야 한다).
// - 복사할 수 없고 이동하면 원본은 빈 포인터가 된다. 빈 포인터의 역참조는 Fatal이다.
// - UniquePtr<Derived>는 기본 deleter이고 Base에 가상 소멸자가 있을 때만 UniquePtr<Base>로 이동할
//   수 있다. 사용자 deleter는 타입 변환을 지원하지 않는다.
// - Reset은 deleter를 지역으로 옮긴 뒤 호출하므로 소멸자가 같은 UniquePtr에 재진입해 새 객체를
//   넣어도 안전하다.
// - 같은 UniquePtr을 여러 스레드가 동시에 변경하지 않는다. 객체 자체의 스레드 안전성과 무관하다.
// - GC 객체 참조(RootRef/WeakRef)가 아니다. Object 시스템 객체에는 사용하지 않는다.
template <typename T, typename Deleter = AllocatorDeleter>
class UniquePtr {
    static_assert(IsNothrowMoveConstructibleV<Deleter>, "UniquePtr deleter needs noexcept move");
    static_assert(IsNothrowDestructibleV<Deleter>, "UniquePtr deleter needs noexcept destruction");
    static_assert(noexcept(DeclVal<Deleter&>()(static_cast<T*>(nullptr))),
                  "UniquePtr deleter must be noexcept callable with T*");

    static constexpr bool UsesAllocator = IsSameV<Deleter, AllocatorDeleter>;

public:
    constexpr UniquePtr() noexcept
        requires(__is_nothrow_constructible(Deleter))
    = default;

    constexpr UniquePtr(decltype(nullptr)) noexcept
        requires(__is_nothrow_constructible(Deleter))
    {
    }

    // 사용자 deleter로 object의 소유를 넘겨받는다. object가 nullptr이면 빈 포인터다.
    UniquePtr(T* object, Deleter deleter) noexcept
        requires(!UsesAllocator)
        : object_(object), deleter_(Move(deleter)) {
        if constexpr (__is_pointer(Deleter)) {
            if (object_ != nullptr && deleter_ == nullptr) {
                platform::Fatal("UniquePtr adopted an object with a null deleter");
            }
        }
    }

    // 실패: allocator의 Allocate 오류(OutOfMemory 등)를 그대로 반환한다. 실패 시 아무것도 남기지
    // 않는다.
    template <typename... Args>
        requires(UsesAllocator)
    static Result<UniquePtr> TryMake(Allocator& allocator, Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...),
                      "UniquePtr::TryMake requires a noexcept constructor");
        auto storage = allocator.Allocate(sizeof(T), alignof(T));
        if (!storage) {
            return Result<UniquePtr>::Failure(storage.Error());
        }
        T* object = ConstructAt<T>(storage.Value(), Forward<Args>(args)...);
        return Result<UniquePtr>::Success(
            UniquePtr(object, AllocatorDeleter{storage.Value(), &allocator}, AdoptTag{}));
    }

    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    UniquePtr(UniquePtr&& other) noexcept : object_(other.object_), deleter_(Move(other.deleter_)) {
        other.object_ = nullptr;
    }

    template <typename U>
        requires(UsesAllocator && !IsSameV<U, T> && __is_convertible_to(U*, T*) &&
                 __has_virtual_destructor(T))
    UniquePtr(UniquePtr<U, AllocatorDeleter>&& other) noexcept
        : object_(other.object_), deleter_(other.deleter_) {
        other.object_ = nullptr;
    }

    UniquePtr& operator=(UniquePtr&& other) noexcept {
        if (this != &other) {
            // 기존 객체 소멸이 other에 접근해도 안전하도록 소유권을 먼저 옮겨 둔다.
            UniquePtr previous(Move(*this));
            object_ = other.object_;
            DestroyAt(&deleter_);
            ConstructAt<Deleter>(&deleter_, Move(other.deleter_));
            other.object_ = nullptr;
        }
        return *this;
    }

    UniquePtr& operator=(decltype(nullptr)) noexcept {
        Reset();
        return *this;
    }

    ~UniquePtr() noexcept {
        Reset();
    }

    // 객체를 deleter로 해제한다. 소멸자가 같은 UniquePtr에 재진입해도 이미 비어 있다.
    void Reset() noexcept {
        if (object_ == nullptr) {
            return;
        }
        T* object = object_;
        object_ = nullptr;
        Deleter deleter(Move(deleter_));
        deleter(object);
    }

    // 소유를 포기하고 포인터를 돌려준다. 사용자 deleter 전용이다(기본 deleter는 할당 주소·
    // allocator를 함께 알아야 해제할 수 있으므로 제공하지 않는다).
    T* Release() noexcept
        requires(!UsesAllocator)
    {
        T* object = object_;
        object_ = nullptr;
        return object;
    }

    T* Get() const noexcept {
        return object_;
    }

    Deleter& GetDeleter() noexcept {
        return deleter_;
    }

    const Deleter& GetDeleter() const noexcept {
        return deleter_;
    }

    Allocator* GetAllocator() const noexcept
        requires(UsesAllocator)
    {
        return object_ != nullptr ? deleter_.allocator : nullptr;
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

    friend bool operator==(const UniquePtr& pointer, decltype(nullptr)) noexcept {
        return pointer.object_ == nullptr;
    }

private:
    template <typename, typename>
    friend class UniquePtr;

    struct AdoptTag {};

    UniquePtr(T* object, Deleter deleter, AdoptTag) noexcept
        : object_(object), deleter_(Move(deleter)) {
    }

    void Check() const noexcept {
        if (object_ == nullptr) {
            platform::Fatal("UniquePtr dereferenced while empty");
        }
    }

    T* object_ = nullptr;
    Deleter deleter_{};
};

} // namespace rocks
