#pragma once

#include <Core/Assert.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Core/Traits.h>

namespace rocks {

template <typename Signature>
class Function;

// 호출 대상을 한 번 할당하여 단독 소유한다. 이동·호출·Reset 자체는 추가 할당하지 않는다.
// allocator는 마지막 호출 대상의 파괴와 메모리 반환까지 살아 있어야 한다.
// 내부 동기화는 없으므로 동일 인스턴스의 호출과 이동·Reset을 동시에 수행하지 않는다.
// 현재는 작은 캡처도 할당하며 small-buffer 최적화는 후속 범위다.
template <typename Return, typename... Args>
class Function<Return(Args...)> {
public:
    Function() noexcept = default;
    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    Function(Function&& other) noexcept {
        Transfer(other);
    }

    Function& operator=(Function&& other) noexcept {
        if (this != &other) {
            Function previous;
            previous.Transfer(*this);
            Transfer(other);
            // 이전 캡처의 소멸자가 재진입할 수 있으므로 새 소유권부터 설치한다.
            // previous는 이 블록 끝에서 해제되며 이후 새 상태를 덮어쓰지 않는다.
        }
        return *this;
    }

    ~Function() noexcept {
        Reset();
    }

    // 할당 실패 시 callable을 이동하거나 변경하지 않는다. 저장 생성·파괴는 noexcept여야 한다.
    template <typename Callable>
    static Result<Function> TryCreate(Allocator& allocator, Callable&& callable) noexcept {
        using Stored = RemoveConstT<RemoveReferenceT<Callable>>;
        static_assert(IsNothrowDestructibleV<Stored>);
        static_assert(noexcept(Stored(Forward<Callable>(callable))));
        auto memory = allocator.Allocate(sizeof(Stored), alignof(Stored));
        if (!memory.IsOk()) {
            return Result<Function>::Failure(memory.Error());
        }
        Stored* stored = ConstructAt<Stored>(memory.Value(), Forward<Callable>(callable));
        Function result;
        result.object_ = stored;
        result.allocator_ = &allocator;
        result.invoke_ = [](void* object, Args&&... args) -> Return {
            return (*static_cast<Stored*>(object))(Forward<Args>(args)...);
        };
        result.destroy_ = [](void* object) noexcept { DestroyAt(static_cast<Stored*>(object)); };
        return Result<Function>::Success(Move(result));
    }

    // 빈 함수 호출은 복구 가능한 오류가 아니라 호출자 계약 위반이다.
    Return operator()(Args... args) {
        ROCKS_ASSERT(object_ != nullptr);
        return invoke_(object_, Forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return object_ != nullptr;
    }

    void Reset() noexcept {
        void* object = object_;
        Allocator* allocator = allocator_;
        auto destroy = destroy_;
        // 사용자 소멸자보다 먼저 비운다. 재진입 Reset의 이중 해제를 막고,
        // 소멸자가 새 함수를 설치하더라도 이 호출은 이전 캡처만 정리한다.
        object_ = nullptr;
        allocator_ = nullptr;
        invoke_ = nullptr;
        destroy_ = nullptr;
        if (object != nullptr) {
            destroy(object);
            ROCKS_ASSERT(allocator->Deallocate(object).IsOk());
        }
    }

private:
    void Transfer(Function& other) noexcept {
        object_ = other.object_;
        allocator_ = other.allocator_;
        invoke_ = other.invoke_;
        destroy_ = other.destroy_;
        other.object_ = nullptr;
        other.allocator_ = nullptr;
        other.invoke_ = nullptr;
        other.destroy_ = nullptr;
    }

    void* object_ = nullptr;
    Allocator* allocator_ = nullptr;
    Return (*invoke_)(void*, Args&&...) = nullptr;
    void (*destroy_)(void*) noexcept = nullptr;
};
} // namespace rocks
