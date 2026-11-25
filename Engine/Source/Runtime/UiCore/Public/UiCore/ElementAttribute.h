#pragma once

#include <Core/Delegate.h>
#include <UiCore/Element.h>

namespace rocks::ui {

enum class InvalidateReason : uint8 {
    Paint,
    Layout,
    Arrangement
};

// 소유 Element의 멤버로 선언한다. getter는 UI 트리를 수정하지 않는 Game lane 조회 함수다.
class ROCKS_UICORE_API AttributeBinding {
public:
    AttributeBinding(const AttributeBinding&) = delete;
    AttributeBinding& operator=(const AttributeBinding&) = delete;
    Result<void> SetPrerequisite(AttributeBinding* prerequisite) noexcept;

protected:
    explicit AttributeBinding(Element& owner) noexcept;
    ~AttributeBinding() noexcept;
    void SetRegistered(bool registered) noexcept;
    void Changed(InvalidateReason reason) noexcept;
    virtual void EvaluateValue() noexcept = 0;

private:
    friend class Element;
    void Evaluate(uint64 pass) noexcept;
    Element& owner_;
    AttributeBinding* next_ = nullptr;
    AttributeBinding* prerequisite_ = nullptr;
    uint64 evaluated_ = 0;
    bool registered_ = false;
};

// 속성 인자에는 상수와 getter를 같은 자리에 전달한다. 상수는 등록 비용이 없고,
// getter는 보이는 트리의 속성 갱신 단계에서 평가된다. getter에서 UI 트리를 수정하면 안 된다.
template <typename T>
struct Bindable {
    Bindable() noexcept = default;
    Bindable(T initial, Delegate<T()> binding = {}) noexcept
        : value(Move(initial)), getter(Move(binding)) {
    }

    Bindable(Delegate<T()> binding) noexcept : getter(Move(binding)) {
    }

    T value{};
    Delegate<T()> getter;
};

template <typename T, InvalidateReason Reason>
class ElementAttribute final : public AttributeBinding {
public:
    ElementAttribute(Element& owner, T initial) noexcept
        : AttributeBinding(owner), value_(Move(initial)) {
    }

    const T& Get() const noexcept {
        return value_;
    }

    void Set(T value) noexcept {
        getter_.Unbind();
        SetRegistered(false);
        Store(Move(value));
    }

    void Assign(Bindable<T> source) noexcept {
        if (!source.getter) {
            Set(Move(source.value));
            return;
        }

        getter_ = Move(source.getter);
        SetRegistered(true);
    }

private:
    void EvaluateValue() noexcept override {
        if (getter_) {
            Store(getter_.Execute());
        } else {
            SetRegistered(false);
        }
    }

    void Store(T value) noexcept {
        if (value_ != value) {
            value_ = Move(value);
            Changed(Reason);
        }
    }

    T value_;
    Delegate<T()> getter_;
};

} // namespace rocks::ui
