#pragma once

#include <Core/Delegate.h>
#include <Object/ConstructionContext.h>
#include <Object/Event.h>
#include <UiWidget/WidgetEventBinding.h>

namespace rocks::ui {

// 에셋 이벤트를 제공하는 모듈이 안정 ID와 타입이 있는 C++ 접근자를 연결한다.
// 한 카탈로그의 이벤트 ID와 메서드 ID는 각각 유일하다. 기반 클래스에 등록한 접근자는
// 파생 객체에도 적용된다. 게임 모듈과 저장소는 생성된 위젯보다 오래 살아야 한다.
// 카탈로그는 인스턴스 생성 후 해제해도 된다. 이벤트는 컨트롤러를 약한 handle로 보관한다.
class ROCKS_UIWIDGET_API WidgetEventCatalog {
public:
    explicit WidgetEventCatalog(Allocator& allocator) noexcept;

    // 기본 컨트롤의 이벤트를 등록한 카탈로그를 만든다. 게임 컨트롤러 메서드는 여기에 추가한다.
    static Result<WidgetEventCatalog> CreateStandard(Allocator& allocator) noexcept;

    // 식별자는 코드 이름을 바꾸더라도 유지한다. 시그니처 변경에는 새 시그니처 ID를 부여한다.
    // accessor는 Widget의 리플렉션 이벤트 접근자다(Docs/Design/UiToolkit 21.5a절 — 예:
    // &ButtonWidget::OnClicked). 연결은 그 이벤트에 native 구독을 더한다.
    template <class Owner, class... Args>
    Result<void> RegisterEvent(Guid id, Guid signature,
                               Event<Args...>& (Owner::*accessor)() noexcept) noexcept {
        if (!accessor) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto attach = Attach::CreateLambda(
            [accessor](ConstructionContext& stage, ObjectHandle sender, ObjectRegistry& objects,
                       ObjectHandle controller,
                       const SharedPtr<MethodList>& methods) noexcept -> Result<void> {
                auto source = stage.GetForWrite<Owner>(sender);
                if (!source) {
                    return Result<void>::Failure(source.Error());
                }

                auto callback = Delegate<void(Args...)>::CreateLambda(
                    [&objects, controller, methods](Args... args) noexcept {
                        for (const auto& erased : *methods) {
                            auto* method = static_cast<TypedMethod<Args...>*>(erased.Get());
                            method->invoke.Execute(objects, controller, args...);
                        }
                    });
                auto added = (source.Value()->*accessor)().TryAdd(Move(callback));
                return added ? Result<void>::Success() : Result<void>::Failure(added.Error());
            });
        return AddEvent({{id, signature, GeneratedType<Owner>::Id(), NativeSignature<Args...>()},
                         Move(attach)});
    }

    template <class Owner, class... Args>
    Result<void> RegisterMethod(Guid id, Guid signature,
                                void (Owner::*method)(Args...) noexcept) noexcept {
        if (!method) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto stored = SharedPtr<TypedMethod<Args...>>::TryMake(allocator_);
        if (!stored) {
            return Result<void>::Failure(stored.Error());
        }

        stored.Value()->invoke =
            Delegate<void(ObjectRegistry&, ObjectHandle, Args...)>::CreateLambda(
                [method](ObjectRegistry& objects, ObjectHandle target, Args... args) noexcept {
                    auto owner = objects.ResolveForWrite<Owner>(objects.OwnerAccess(), target);
                    if (owner) {
                        auto live = owner.Value().Get();
                        if (live) {
                            (live.Value()->*method)(Forward<Args>(args)...);
                        }
                    }
                });
        return AddMethod({{id, signature, GeneratedType<Owner>::Id(), NativeSignature<Args...>()},
                          Move(stored.Value())});
    }

    Result<void> Validate(const TypeLease& sender, const TypeLease& controller,
                          const WidgetEventBinding& binding) const noexcept;

private:
    friend class WidgetTemplate;
    Result<void> Connect(ConstructionContext& stage, ObjectRegistry& objects, ObjectHandle sender,
                         TypeId senderType, ObjectHandle controller, TypeId controllerType,
                         Span<const WidgetEventBinding> bindings) const noexcept;

private:
    // 같은 시그니처를 게임 DLL과 엔진 DLL에서 각각 등록해도 일치해야 한다.
    // 런타임 타입 확인에만 사용한다. 디스크에는 공개 GUID만 기록한다.
    template <class... Args>
    static constexpr StringView NativeSignature() noexcept {
        return __PRETTY_FUNCTION__;
    }

    struct MethodBase {};
    template <class... Args>
    struct TypedMethod : MethodBase {
        Delegate<void(ObjectRegistry&, ObjectHandle, Args...)> invoke;
    };
    using MethodList = Array<SharedPtr<MethodBase>>;
    using Attach = Delegate<Result<void>(ConstructionContext&, ObjectHandle, ObjectRegistry&,
                                         ObjectHandle, const SharedPtr<MethodList>&)>;
    struct Descriptor {
        Guid id;
        Guid signature;
        TypeId owner;
        StringView nativeSignature;
    };
    struct EventEntry {
        Descriptor desc;
        Attach attach;
    };
    struct MethodEntry {
        Descriptor desc;
        SharedPtr<MethodBase> method;
    };
    Result<void> CheckDescriptor(const Descriptor& value) const noexcept;
    Result<void> AddEvent(EventEntry entry) noexcept;
    Result<void> AddMethod(MethodEntry entry) noexcept;
    const EventEntry* FindEvent(Guid id) const noexcept;
    const MethodEntry* FindMethod(Guid id) const noexcept;

    Allocator& allocator_;
    Array<EventEntry> events_;
    Array<MethodEntry> methods_;
};

} // namespace rocks::ui
