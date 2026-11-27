#pragma once

#include <Base/PlacementNew.h>
#include <Object/Event.h>
#include <Object/FunctionBinding.h>
#include <Object/FunctionCall.h>
#include <Object/ObjectRegistry.h>
#include <Object/RuntimeTypeOps.h>

// 리플렉션 이벤트 필드의 연산표 템플릿이다(Docs/Design/EventReflection.ko.md ED16·ED17 — 헤더
// 전용). 생성 코드와 손으로 만든 함수표가 이벤트 멤버 포인터 하나로 RuntimeEventOps를 만든다.
// thunk는 이 템플릿을 부른 image(발행자 클래스 모듈)에 생기고 RuntimeTypeOps::module이 그 코드
// 수명을 지킨다.
//
// 사용 예(손으로 만든 함수표 — 생성 코드도 같은 한 줄이다):
//   class Door : public Object {
//   public:
//       Event<int32> opened;
//   };
//   static const RuntimeEventOps events[] = {detail::MakeEventOps<&Door::opened>(OpenedId)};
//   ops.events = Span<const RuntimeEventOps>(events, 1);
//   // schema 필드: {OpenedId, FieldKind::Event, 0, "opened", ..., eventParameters}
//   // 인자 offset: detail::EventFrameShape<decltype(Door::opened)>::Value.offsets[i]
namespace rocks {

namespace detail {

// 이벤트 필드 형식의 발행 frame 배치다(BindingFrame<Args...>와 같다 — ED17).
template <class EventType>
struct EventFrameShapeOf;

template <class... Args>
struct EventFrameShapeOf<Event<Args...>> {
    using Type = BindingFrameShape<Args...>;
};

template <class EventType>
using EventFrameShape = typename EventFrameShapeOf<RemoveCvT<EventType>>::Type;

template <class Member>
struct EventMemberTraits;

template <class Class, class... Args>
struct EventMemberTraits<Event<Args...> Class::*> {
    using Owner = Class;
    using Shape = BindingFrameShape<Args...>;

    template <Event<Args...> Class::* Member>
    struct Thunks {
        // 발행자를 빌려(Write면 쓰기 — 바인딩 레코드를 바꾼다, 아니면 읽기 — 발행) callable(이벤트
        // 필드)을 부른다. Borrow는 callable이 끝날 때까지 쥔다.
        template <class Out, bool Write, class Callable>
        static Out WithEvent(ObjectRegistry& registry, const ObjectAccessContext& access,
                             ObjectHandle publisher, Callable&& callable) noexcept {
            auto borrow = Write ? registry.ResolveForWrite<Class>(access, publisher)
                                : registry.Resolve<Class>(access, publisher);
            if (!borrow) {
                return Out::Failure(borrow.Error());
            }

            auto object = borrow.Value().Get();
            if (!object) {
                return Out::Failure(object.Error());
            }

            return callable(object.Value()->*Member);
        }

        static Result<DelegateHandle> Bind(ObjectRegistry& registry,
                                           const ObjectAccessContext& access,
                                           ObjectHandle publisher, ObjectHandle target,
                                           Guid functionId) noexcept {
            return WithEvent<Result<DelegateHandle>, true>(
                registry, access, publisher, [&](Event<Args...>& event) noexcept {
                    return event.TryBind(registry, access, target, functionId);
                });
        }

        static Result<void> Unbind(ObjectRegistry& registry, const ObjectAccessContext& access,
                                   ObjectHandle publisher, DelegateHandle handle) noexcept {
            return WithEvent<Result<void>, true>(
                registry, access, publisher, [&](Event<Args...>& event) noexcept {
                    return event.Unbind(handle) ? Result<void>::Success()
                                                : Result<void>::Failure(ErrorCode::NotFound);
                });
        }

        static Result<void> UnbindFunction(ObjectRegistry& registry,
                                           const ObjectAccessContext& access,
                                           ObjectHandle publisher, ObjectHandle target,
                                           Guid functionId) noexcept {
            return WithEvent<Result<void>, true>(
                registry, access, publisher, [&](Event<Args...>& event) noexcept {
                    return event.Unbind(target, functionId)
                               ? Result<void>::Success()
                               : Result<void>::Failure(ErrorCode::NotFound);
                });
        }

        static Result<usize> UnbindAll(ObjectRegistry& registry, const ObjectAccessContext& access,
                                       ObjectHandle publisher, ObjectHandle target) noexcept {
            return WithEvent<Result<usize>, true>(
                registry, access, publisher, [&](Event<Args...>& event) noexcept {
                    return Result<usize>::Success(event.UnbindAll(target));
                });
        }

        static Result<void> Clear(ObjectRegistry& registry, const ObjectAccessContext& access,
                                  ObjectHandle publisher) noexcept {
            return WithEvent<Result<void>, true>(registry, access, publisher,
                                                 [&](Event<Args...>& event) noexcept {
                                                     event.Clear();
                                                     return Result<void>::Success();
                                                 });
        }

        static Result<usize> Compact(ObjectRegistry& registry, const ObjectAccessContext& access,
                                     ObjectHandle publisher) noexcept {
            return WithEvent<Result<usize>, true>(
                registry, access, publisher, [&](Event<Args...>& event) noexcept {
                    return Result<usize>::Success(event.Compact(registry, access));
                });
        }

        static Result<void> Bindings(ObjectRegistry& registry, const ObjectAccessContext& access,
                                     ObjectHandle publisher, EventBindingVisitor visitor,
                                     void* user) noexcept {
            if (visitor == nullptr) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            return WithEvent<Result<void>, false>(
                registry, access, publisher, [&](Event<Args...>& event) noexcept {
                    return event.ForEachBinding(
                        [&](ObjectHandle target, Guid functionId, bool resolved) noexcept {
                            return visitor(user, target, functionId, resolved);
                        });
                });
        }

        static Result<void> KeepUnresolved(ObjectRegistry& registry,
                                           const ObjectAccessContext& access,
                                           ObjectHandle publisher, ObjectHandle target,
                                           Guid functionId) noexcept {
            return WithEvent<Result<void>, true>(
                registry, access, publisher, [&](Event<Args...>& event) noexcept {
                    return event.TryKeepUnresolved(target, functionId);
                });
        }

        static Result<void> BroadcastFromFrame(ObjectRegistry& registry,
                                               const ObjectAccessContext& access,
                                               ObjectHandle publisher, void* frame) noexcept {
            if (frame == nullptr) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            return WithEvent<Result<void>, false>(
                registry, access, publisher, [&](Event<Args...>& event) noexcept {
                    return BroadcastSlots(event, EventContext{registry, access, publisher},
                                          static_cast<unsigned char*>(frame),
                                          MakeIndexSequence<sizeof...(Args)>{});
                });
        }

        template <usize... Index>
        static Result<void> BroadcastSlots(Event<Args...>& event, const EventContext& context,
                                           unsigned char* frame, IndexSequence<Index...>) noexcept {
            return event.TryBroadcast(context, SlotAt<Args>(frame, Shape::Value.offsets[Index])...);
        }

        template <class Arg>
        static BindingSlotType<Arg>& SlotAt(unsigned char* frame, uint32 offset) noexcept {
            return *platform::Launder(reinterpret_cast<BindingSlotType<Arg>*>(frame + offset));
        }

        static Result<void> Construct(void* frame, Allocator& allocator) noexcept {
            auto* bytes = static_cast<unsigned char*>(frame);
            uint32 index = 0;
            ((ValueOpsOf<BindingSlotType<Args>>().construct(bytes + Shape::Value.offsets[index],
                                                            allocator),
              ++index),
             ...);
            ::new (static_cast<void*>(bytes + Shape::Value.errorOffset)) ErrorCode(ErrorCode::None);
            return Result<void>::Success();
        }

        static void Destroy(void* frame) noexcept {
            auto* bytes = static_cast<unsigned char*>(frame);
            uint32 index = 0;
            ((ValueOpsOf<BindingSlotType<Args>>().destroy(bytes + Shape::Value.offsets[index]),
              ++index),
             ...);
        }

        static const FrameLayout& Layout() noexcept {
            static const FrameLayout layout = FrameLayoutOf<Args...>();
            return layout;
        }

        static Span<const RuntimeParameterOps> Parameters() noexcept {
            if constexpr (sizeof...(Args) == 0) {
                return {};
            } else {
                static const RuntimeParameterOps parameters[] = {
                    ParameterOpsOf<BindingSlotType<Args>>()...};
                return Span<const RuntimeParameterOps>(parameters, sizeof...(Args));
            }
        }
    };
};

// 이벤트 멤버 Member(Event<Args...> Class::*)의 연산표다(ED16). fieldId는 schema 필드 ID다.
template <auto Member>
RuntimeEventOps MakeEventOps(Guid fieldId) noexcept {
    using Traits = EventMemberTraits<decltype(Member)>;
    using Thunks = typename Traits::template Thunks<Member>;
    RuntimeEventOps ops;
    ops.fieldId = fieldId;
    ops.bind = &Thunks::Bind;
    ops.unbind = &Thunks::Unbind;
    ops.unbindFunction = &Thunks::UnbindFunction;
    ops.unbindAll = &Thunks::UnbindAll;
    ops.clear = &Thunks::Clear;
    ops.compact = &Thunks::Compact;
    ops.bindings = &Thunks::Bindings;
    ops.keepUnresolved = &Thunks::KeepUnresolved;
    ops.broadcastFromFrame = &Thunks::BroadcastFromFrame;
    ops.construct = &Thunks::Construct;
    ops.destroy = &Thunks::Destroy;
    ops.parameters = Thunks::Parameters();
    ops.frameSize = Traits::Shape::Value.size;
    ops.frameAlignment = Traits::Shape::Value.alignment;
    ops.frameLayout = &Thunks::Layout();
    return ops;
}

} // namespace detail

} // namespace rocks
