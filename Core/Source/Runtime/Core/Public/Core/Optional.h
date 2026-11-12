#pragma once

#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

// 빈 Optional을 명시하는 표지다. 예: Optional<int> value = NullOpt;
struct NullOptTag {
    explicit constexpr NullOptTag(int) noexcept {
    }
};

inline constexpr NullOptTag NullOpt{0};

// 값이 있거나 없는 상태를 표현하는 값 타입이다(UE TOptional 대응). 별도 heap 할당이 없다.
// 실패 원인이 필요하면 Result를 쓰고, 단순히 "없음"이 정상 상태일 때 Optional을 쓴다.
//
// 계약:
// - T의 이동 생성·소멸은 noexcept여야 한다. 복사는 T의 noexcept 복사 생성이 있을 때만 허용한다.
// - 빈 상태에서 Value()·operator*·operator->는 Fatal이다. 기본값이 필요하면 ValueOr를 쓴다.
// - 이동 후 원본은 값 보유 여부를 유지하고 값은 T가 정의한 이동 후 상태가 된다(UE와 같다).
// - Emplace는 새 값을 먼저 임시로 만든 뒤 기존 값을 파괴하므로 인자가 현재 값을 참조해도 안전하다.
// - 같은 Optional의 동시 변경과, T 소멸자에서 같은 Optional을 다시 변경하는 재진입은 지원하지
// 않는다.
template <typename T>
class Optional {
    static_assert(IsNothrowMoveConstructibleV<T>, "Optional<T> requires noexcept move");
    static_assert(IsNothrowDestructibleV<T>, "Optional<T> requires noexcept destruction");

public:
    constexpr Optional() noexcept = default;

    constexpr Optional(NullOptTag) noexcept {
    }

    Optional(const T& value) noexcept
        requires(__is_nothrow_constructible(T, const T&))
    {
        ConstructAt<T>(storage_, value);
        hasValue_ = true;
    }

    Optional(T&& value) noexcept {
        ConstructAt<T>(storage_, Move(value));
        hasValue_ = true;
    }

    Optional(const Optional& other) noexcept
        requires(__is_nothrow_constructible(T, const T&))
    {
        if (other.hasValue_) {
            ConstructAt<T>(storage_, other.Get());
            hasValue_ = true;
        }
    }

    Optional(Optional&& other) noexcept {
        if (other.hasValue_) {
            ConstructAt<T>(storage_, Move(other.Get()));
            hasValue_ = true;
        }
    }

    ~Optional() noexcept {
        Reset();
    }

    Optional& operator=(const Optional& other) noexcept
        requires(__is_nothrow_constructible(T, const T&))
    {
        if (this != &other) {
            if (other.hasValue_) {
                Emplace(other.Get());
            } else {
                Reset();
            }
        }
        return *this;
    }

    Optional& operator=(Optional&& other) noexcept {
        if (this != &other) {
            if (other.hasValue_) {
                Emplace(Move(other.Get()));
            } else {
                Reset();
            }
        }
        return *this;
    }

    Optional& operator=(NullOptTag) noexcept {
        Reset();
        return *this;
    }

    // UE IsSet에 해당한다.
    bool HasValue() const noexcept {
        return hasValue_;
    }

    explicit operator bool() const noexcept {
        return hasValue_;
    }

    // 값이 있어야 한다(없으면 Fatal). 반환 참조는 Reset·Emplace·대입·소멸 전까지만 유효하다.
    T& Value() & noexcept {
        Check();
        return Get();
    }

    const T& Value() const& noexcept {
        Check();
        return Get();
    }

    T& operator*() & noexcept {
        return Value();
    }

    const T& operator*() const& noexcept {
        return Value();
    }

    T* operator->() noexcept {
        return &Value();
    }

    const T* operator->() const noexcept {
        return &Value();
    }

    // 값이 없으면 nullptr이다(UE GetPtrOrNull).
    T* TryGet() noexcept {
        return hasValue_ ? &Get() : nullptr;
    }

    const T* TryGet() const noexcept {
        return hasValue_ ? &Get() : nullptr;
    }

    // 값의 복사본 또는 fallback을 반환한다(UE Get(DefaultValue)). T는 noexcept 복사가 필요하다.
    T ValueOr(const T& fallback) const noexcept
        requires(__is_nothrow_constructible(T, const T&))
    {
        return hasValue_ ? Get() : fallback;
    }

    void Reset() noexcept {
        if (hasValue_) {
            hasValue_ = false;
            DestroyAt(&Get());
        }
    }

    // 새 값을 만든다. T의 생성자는 noexcept여야 한다. 인자가 기존 값을 참조해도 안전하도록
    // 새 값을 먼저 만든 뒤 기존 값을 파괴하고 이동한다.
    template <typename... Args>
    T& Emplace(Args&&... args) noexcept {
        static_assert(__is_nothrow_constructible(T, Args && ...),
                      "Optional::Emplace requires a noexcept constructor");
        T replacement(Forward<Args>(args)...);
        Reset();
        ConstructAt<T>(storage_, Move(replacement));
        hasValue_ = true;
        return Get();
    }

    // 둘 다 비었으면 같고, 하나만 비었으면 다르며, 둘 다 있으면 T의 ==를 사용한다.
    friend bool operator==(const Optional& left, const Optional& right) noexcept {
        if (left.hasValue_ != right.hasValue_) {
            return false;
        }
        return !left.hasValue_ || left.Get() == right.Get();
    }

    friend bool operator==(const Optional& optional, NullOptTag) noexcept {
        return !optional.hasValue_;
    }

private:
    void Check() const noexcept {
        if (!hasValue_) {
            platform::Fatal("Optional::Value called on an empty Optional");
        }
    }

    // 저장소에 placement로 생성한 객체에 접근한다. 같은 저장소에 객체를 다시 만들 수 있으므로
    // 포인터 최적화 가정을 끊기 위해 launder를 거친다.
    T& Get() noexcept {
        return *__builtin_launder(reinterpret_cast<T*>(storage_));
    }

    const T& Get() const noexcept {
        return *__builtin_launder(reinterpret_cast<const T*>(storage_));
    }

    alignas(T) unsigned char storage_[sizeof(T)];
    bool hasValue_ = false;
};

} // namespace rocks
