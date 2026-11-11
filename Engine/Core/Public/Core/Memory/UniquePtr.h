#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

// 할당기에서 만든 객체 하나의 단독 소유권이다(UE TUniquePtr 대응).
// 사용 예:
//   auto created = UniquePtr<Enemy>::TryMake(allocator, 100);
//   if (!created) { return created.Error(); }
//   UniquePtr<Enemy> enemy = Move(created.Value());
//
// 계약:
// - allocator는 이 포인터와 객체보다 오래 살아야 한다. 소멸·Reset은 객체 소멸자를 부른 뒤 같은
//   allocator에 저장소를 반환하며, 반환이 실패하면 누수를 숨기지 않고 Fatal이다.
// - 생성자는 noexcept여야 한다. 생성 실패를 표현하려면 TryMake 이후 별도의 초기화 Result를 쓴다.
// - 복사할 수 없고 이동하면 원본은 빈 포인터가 된다. 빈 포인터의 역참조는 Fatal이다.
// - UniquePtr<Derived>는 Base에 가상 소멸자가 있을 때만 UniquePtr<Base>로 이동할 수 있다.
//   원래 할당 주소를 함께 보관하므로 Base가 첫 기반이 아니어도 올바른 주소를 반환한다.
// - 같은 UniquePtr을 여러 스레드가 동시에 변경하지 않는다. 객체 자체의 스레드 안전성과 무관하다.
// - GC 객체 참조(RootRef/WeakRef)가 아니다. Object 시스템 객체에는 사용하지 않는다.
template <typename T>
class UniquePtr {
public:
    constexpr UniquePtr() noexcept = default;

    constexpr UniquePtr(decltype(nullptr)) noexcept {
    }

    // 실패: allocator의 Allocate 오류(OutOfMemory 등)를 그대로 반환한다. 실패 시 아무것도 남기지
    // 않는다.
    template <typename... Args>
    static Result<UniquePtr> TryMake(Allocator& allocator, Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...),
                      "UniquePtr::TryMake requires a noexcept constructor");
        auto storage = allocator.Allocate(sizeof(T), alignof(T));
        if (!storage) {
            return Result<UniquePtr>::Failure(storage.Error());
        }
        T* object = ConstructAt<T>(storage.Value(), Forward<Args>(args)...);
        return Result<UniquePtr>::Success(UniquePtr(object, storage.Value(), &allocator));
    }

    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    UniquePtr(UniquePtr&& other) noexcept
        : object_(other.object_), allocation_(other.allocation_), allocator_(other.allocator_) {
        other.Clear();
    }

    template <typename U>
        requires(!IsSameV<U, T> && __is_convertible_to(U*, T*) && __has_virtual_destructor(T))
    UniquePtr(UniquePtr<U>&& other) noexcept
        : object_(other.object_), allocation_(other.allocation_), allocator_(other.allocator_) {
        other.Clear();
    }

    UniquePtr& operator=(UniquePtr&& other) noexcept {
        if (this != &other) {
            // 기존 객체 소멸이 other에 접근해도 안전하도록 소유권을 먼저 옮겨 둔다.
            UniquePtr previous(Move(*this));
            object_ = other.object_;
            allocation_ = other.allocation_;
            allocator_ = other.allocator_;
            other.Clear();
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

    // 객체를 소멸하고 저장소를 반환한다. 소멸자가 같은 UniquePtr에 재진입해도 이미 비어 있다.
    void Reset() noexcept {
        if (object_ == nullptr) {
            return;
        }
        T* object = object_;
        void* allocation = allocation_;
        Allocator* allocator = allocator_;
        Clear();
        DestroyAt(object);
        if (!allocator->Deallocate(allocation)) {
            platform::Fatal("UniquePtr storage deallocation failed");
        }
    }

    T* Get() const noexcept {
        return object_;
    }

    Allocator* GetAllocator() const noexcept {
        return allocator_;
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
    template <typename>
    friend class UniquePtr;

    UniquePtr(T* object, void* allocation, Allocator* allocator) noexcept
        : object_(object), allocation_(allocation), allocator_(allocator) {
    }

    void Clear() noexcept {
        object_ = nullptr;
        allocation_ = nullptr;
        allocator_ = nullptr;
    }

    void Check() const noexcept {
        if (object_ == nullptr) {
            platform::Fatal("UniquePtr dereferenced while empty");
        }
    }

    T* object_ = nullptr;
    void* allocation_ = nullptr;
    Allocator* allocator_ = nullptr;
};

} // namespace rocks
