#pragma once

#include <Core/Delegate.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/PropertyPath.h>

namespace rocks::ui {

// 화면/컨트롤러가 소유하는 변경 통지 바인딩 묶음이다. 등록 시 한 번, 이후에는 지정한
// 원천 필드가 바뀔 때만 읽는다. 경로는 등록 시 FieldId·배열 인덱스·Map 키로 해석한다.
// 객체를 GC 루트로 만들지 않는다. ObjectRegistry가 이 묶음보다 오래 살아야 하며
// 소유 스레드의 ObjectAccessScope 안에서 사용한다.
class ROCKS_UIWIDGET_API WidgetFieldBindings {
public:
    using Setter = Delegate<Result<void>(ObjectRegistry&, ObjectHandle, const FieldValue&)>;

    explicit WidgetFieldBindings(ObjectRegistry& objects) noexcept;
    WidgetFieldBindings(const WidgetFieldBindings&) = delete;
    WidgetFieldBindings& operator=(const WidgetFieldBindings&) = delete;
    WidgetFieldBindings(WidgetFieldBindings&&) = delete;
    WidgetFieldBindings& operator=(WidgetFieldBindings&&) = delete;

    // ex) bindings.Bind(model, "healthRatio", bar, WidgetSetter(&ProgressBarWidget::SetValue))
    // 컨트롤의 공개 setter를 거치므로 화면을 열기 전과 연 뒤에 같은 API를 사용한다.
    Result<DelegateHandle> Bind(ObjectHandle source, StringView path, ObjectHandle target,
                                Setter setter) noexcept;
    Result<void> ApplyOnce(ObjectHandle source, StringView path, ObjectHandle target,
                           Setter setter) noexcept;
    void Unbind(DelegateHandle handle) noexcept;

    // 모델의 네이티브 setter가 실제로 값을 바꾼 뒤 최상위 FieldId를 알린다.
    // 경로 쓰기를 사용하는 모델은 Write로 쓰기와 통지를 함께 처리할 수 있다.
    // 대상 setter 오류는 호출자에게 반환한다. 원천에 성공한 쓰기는 이미 반영된 상태다.
    Result<void> Notify(ObjectHandle source, Guid field) noexcept;
    Result<void> NotifyNamed(ObjectHandle source, StringView fieldName) noexcept;
    ObjectRegistry& Registry() const noexcept;
    // 초기값을 staged 객체에 적용한 컴파일러가 변경 구독만 설치할 때 사용한다.
    Result<DelegateHandle> SubscribeField(ObjectHandle source, Guid field, ObjectHandle target,
                                          Setter setter) noexcept;
    Result<void> Write(ObjectHandle source, PropertyPath path, const FieldValue& value) noexcept;

private:
    Result<DelegateHandle> Connect(ObjectHandle source, const PropertyPathBuffer& path,
                                   ObjectHandle target, Setter setter, bool initial) noexcept;
    ObjectRegistry& objects_;
    MulticastDelegate<void(ObjectHandle, Guid, ErrorCode&)> changed_;
};

// 생성 중인 위젯과 이미 공개된 위젯 모두 같은 타입 검사와 setter를 거친다.
template <class Owner, class Return, class Value>
Result<void> ApplyWidgetSetter(Owner& owner, Return (Owner::*method)(Value) noexcept,
                               const FieldValue& input, Allocator& allocator) noexcept {
    using Stored = RemoveCvRefT<Value>;
    Stored value = [&]() noexcept {
        if constexpr (IsSameV<Stored, String>) {
            return String(allocator);
        } else {
            return Stored{};
        }
    }();
    if constexpr (IsSameV<Stored, StringView>) {
        if (input.kind != ValueKind::Utf8String || !input.present) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
        value = input.string.View();
    } else {
        auto decoded = ::rocks::detail::WriteScalarField(value, input, &allocator);
        if (!decoded) {
            return decoded;
        }
    }
    if constexpr (IsSameV<Return, void>) {
        (owner.*method)(Forward<Value>(value));
        return Result<void>::Success();
    } else {
        return (owner.*method)(Forward<Value>(value));
    }
}

// 값 하나를 받는 C++ setter를 리플렉션 필드의 목적지로 지정한다. 형식이 일치해야 하며,
// 숫자·bool·enum 등은 생성 필드 접근자와 같은 검증을 거친다. StringView는 호출 동안 유효하다.
template <class Owner, class Return, class Value>
WidgetFieldBindings::Setter WidgetSetter(Return (Owner::*method)(Value) noexcept) noexcept {
    return WidgetFieldBindings::Setter::CreateLambda(
        [method](ObjectRegistry& objects, ObjectHandle handle,
                 const FieldValue& input) noexcept -> Result<void> {
            if (!method) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            auto borrowed = objects.ResolveForWrite<Owner>(objects.OwnerAccess(), handle);
            if (!borrowed) {
                return Result<void>::Failure(borrowed.Error());
            }
            auto owner = borrowed.Value().Get();
            if (!owner) {
                return Result<void>::Failure(owner.Error());
            }
            return ApplyWidgetSetter(*owner.Value(), method, input, objects.ObjectAllocator());
        });
}

// 네이티브 위젯 이벤트를 Object의 C++ 멤버 함수에 연결한다. 매 호출 시 handle의
// 세대와 타입을 검사한다. 대상이 먼저 파괴되면 호출을 건너뛴다. 저장소 수명 안에서 사용한다.
// ex) toggle.SetOnChanged(WidgetMethod(objects, label.Handle(), &Widget::SetEnabled))
template <class Owner, class... Args>
Delegate<void(Args...)> WidgetMethod(ObjectRegistry& objects, ObjectHandle target,
                                     void (Owner::*method)(Args...) noexcept) noexcept {
    return Delegate<void(Args...)>::CreateLambda([&objects, target, method](Args... args) noexcept {
        if (method) {
            auto borrowed = objects.ResolveForWrite<Owner>(objects.OwnerAccess(), target);
            if (borrowed) {
                auto owner = borrowed.Value().Get();
                if (owner) {
                    (owner.Value()->*method)(Forward<Args>(args)...);
                }
            }
        }
    });
}

} // namespace rocks::ui
