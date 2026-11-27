#pragma once

#include <UiCore/ElementAttribute.h>

namespace rocks::ui {

// 화면을 조립하는 함수의 첫 줄에서 스코프를 연다. Build와 그 인자의 모든 Make가 같은
// 할당자를 쓴다. 스코프는 스레드별로 중첩할 수 있고, 반환된 위젯은 스코프 밖에서도 유효하다.
// 할당자 자체는 생성된 위젯보다 오래 살아야 한다.
//
//   ElementBuildScope scope(allocator);
//   const StackPanel::SlotArgs slots[] = {
//       {.size = Auto, .padding = 4, .content = Make<Button>({.onClicked = Bind(this,
//       &Panel::Open)})},
//       {.size = Fill(1), .content = body},
//   };
//   return Build<VerticalStack>({.slots = slots});
//
// 중첩 Make의 인자는 Build 호출 전에 평가된다. 트리를 만드는 동안 같은 할당자를 사용한다.
class ROCKS_UICORE_API ElementBuildScope {
public:
    explicit ElementBuildScope(Allocator& allocator) noexcept;
    ~ElementBuildScope() noexcept;
    ElementBuildScope(const ElementBuildScope&) = delete;
    ElementBuildScope& operator=(const ElementBuildScope&) = delete;
    static Allocator* Current() noexcept;

private:
    Allocator* previous_;
};

// 모든 위젯에 공통인 초기 상태. 매 프레임 자체 그리기가 필요한 위젯에만 volatilePaint를
// 켠다. updateAttributes를 켜면 Build가 반환하기 전에 바인딩의 첫 값을 읽을 수 있다.
struct ElementArgs {
    Visibility visibility = Visibility::Visible;
    bool enabled = true;
    bool clipToBounds = false;
    bool volatilePaint = false;
    bool updateAttributes = false;
};

// 슬롯 목록에 복사해도 자식의 수명과 최초 생성 오류가 함께 보존된다.
struct ROCKS_UICORE_API ElementChild {
    ElementChild() noexcept = default;
    template <typename T>
    ElementChild(const SharedPtr<T>& child) noexcept : element(child) {
    }

    template <typename T>
    ElementChild(const Result<SharedPtr<T>>& result) noexcept
        : element(result ? SharedPtr<Element>(result.Value()) : SharedPtr<Element>{}),
          error(result ? ErrorCode::None : result.Error()) {
    }

    template <typename T>
    ElementChild(const Result<SharedRef<T>>& result) noexcept
        : element(result ? SharedPtr<Element>(result.Value().AsSharedPtr()) : SharedPtr<Element>{}),
          error(result ? ErrorCode::None : result.Error()) {
    }

    SharedPtr<Element> element;
    ErrorCode error = ErrorCode::None;
    Result<void> AttachTo(Element& parent) const noexcept;
};

namespace detail {

template <typename Callback, typename Owner, typename Method>
Callback BindMethod(const Owner& owner, Method method) noexcept {
    if constexpr (IsPointerV<Owner>) {
        return Callback::CreateRaw(owner, method);
    } else {
        return Callback::CreateWeak(owner, method);
    }
}
} // namespace detail

// 일반 멤버 함수는 이벤트로, 인자가 없는 조회 함수는 속성 getter로 연결한다.
// Bind(this, &Panel::Open)은 Panel이 위젯보다 오래 살아야 한다. SharedPtr를 넘기면
// 대상을 약하게 참조하므로 패널을 닫아 소유자가 소멸하면 바인딩도 비활성화된다.
template <typename Owner, typename Class, typename R, typename... A>
Delegate<R(A...)> Bind(const Owner& owner, R (Class::*method)(A...)) noexcept {
    return detail::BindMethod<Delegate<R(A...)>>(owner, method);
}

template <typename Owner, typename Class, typename R, typename... A>
Delegate<R(A...)> Bind(const Owner& owner, R (Class::*method)(A...) const) noexcept {
    return detail::BindMethod<Delegate<R(A...)>>(owner, method);
}

// 데이터 멤버는 값을 읽는 단방향 속성으로 노출한다. 예를 들어 .color = Bind(model, &Model::tint)는
// 속성 갱신 시점에 tint를 읽고, 값이 달라진 경우에만 색을 다시 그린다.
template <typename Owner, typename Class, typename T>
    requires(!IsFunctionV<T>)
Delegate<T()> Bind(const Owner& owner, T Class::* member) noexcept {
    if (!owner || !member) {
        return {};
    }

    if constexpr (IsPointerV<Owner>) {
        return Delegate<T()>::CreateLambda([owner, member]() noexcept { return owner->*member; });
    } else {
        return Delegate<T()>::CreateWeakLambda(
            owner, [object = owner.Get(), member]() noexcept { return object->*member; });
    }
}

namespace detail {

template <typename T, typename Args>
Result<SharedRef<T>> BuildElement(Args&& args) noexcept {
    auto* allocator = ElementBuildScope::Current();
    if (!allocator) {
        return Result<SharedRef<T>>::Failure(ErrorCode::WrongContext);
    }

    auto value = SharedRef<T>::TryMake(*allocator, *allocator);
    if (!value) {
        return value;
    }

    auto& element = *value.Value();
    if constexpr (requires { args.common; }) {
        element.SetEnabled(args.common.enabled);
        element.SetVisibility(args.common.visibility);
        element.SetClipToBounds(args.common.clipToBounds);
    }

    auto result = element.Construct(Forward<Args>(args));
    if (!result) {
        return Result<SharedRef<T>>::Failure(result.Error());
    }

    if constexpr (requires { args.common; }) {
        if (args.common.updateAttributes) {
            element.UpdateBoundAttributes();
        }

        element.SetForceVolatile(args.common.volatilePaint);
    }

    return value;
}
} // namespace detail

// 완성된 루트 또는 첫 생성 오류를 돌려준다. 실패하면 이 호출에서 만든 부분 트리도 해제된다.
// 콜백을 소유한 Args는 중괄호로 전달하거나 Move(args)로 소유권을 넘긴다.
template <typename T>
    requires requires(T& element, const typename T::Args& args) { element.Construct(args); }
Result<SharedRef<T>> Build(const typename T::Args& args) noexcept {
    return detail::BuildElement<T>(args);
}

template <typename T>
Result<SharedRef<T>> Build(typename T::Args&& args) noexcept {
    return detail::BuildElement<T>(Move(args));
}

// 슬롯에 넣을 자식을 즉시 생성한다. 자식의 실패도 슬롯에 담겨 루트 Build까지 전달된다.
template <typename T>
    requires requires(T& element, const typename T::Args& args) { element.Construct(args); }
ElementChild Make(const typename T::Args& args) noexcept {
    return Build<T>(args);
}

template <typename T>
ElementChild Make(typename T::Args&& args) noexcept {
    return Build<T>(Move(args));
}

} // namespace rocks::ui
