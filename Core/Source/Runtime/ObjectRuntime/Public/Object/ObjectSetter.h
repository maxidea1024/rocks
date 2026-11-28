#pragma once

#include <Object/Object.h>
#include <Object/ValueSame.h>

// 코드 통지(Object::NotifyChanged·SetAndNotify)와 생성 setter(ROCKS_SETTER)의 본체다
// (Docs/Design/ChangeNotification.ko.md CN-D9·CN-D13). 생성 헤더가 include하므로 사용자는 따로
// include하지 않는다.
namespace rocks::detail {

// 두 템플릿 인자가 같은 멤버를 가리키는지 템플릿 인자 동등성으로 본다. 멤버 포인터의 ==는 virtual
// 멤버 함수에서 상수식이 아니므로(값이 정해지지 않는다) 쓰지 않는다. 형식이 다르면 다른 형식이라
// 거짓이고, 파생 클래스 이름으로 만든 기반 멤버 포인터(&Derived::baseField)는 기반 멤버와 같다.
template <auto Member>
struct MemberTag {};

template <auto Left, auto Right>
inline constexpr bool SameMemberV = IsSameV<MemberTag<Left>, MemberTag<Right>>;

// 생성 NotifyId의 "대상 아님" static_assert가 템플릿 인자에 의존하게 한다.
template <auto>
inline constexpr bool NotNotifyTargetV = false;

template <class M, class C>
struct SetterAccess<M C::*> {
    // object(C 또는 파생)의 Member에 value를 넣는다(CN-D13 대입 규칙 — 같으면 false, 넣었으면
    // true). 할당하는 값은 사본을 먼저 만든 뒤 교환하므로 실패하면 필드가 그대로다. 옛 값은
    // 돌아가기 전에 반환한다.
    template <M C::* Member, class Value = M>
    static Result<bool> Assign(Object& object, SetterParam<Value> value) noexcept {
        static_assert(!IsFunctionV<M>, "SetAndNotify and ROCKS_SETTER need a data member");
        static_assert(IsBaseOfV<Object, C>,
                      "SetAndNotify and ROCKS_SETTER need a member of a reflected class");
        M& field = static_cast<C&>(object).*Member;
        if (ValueSame<M>::Same(field, value)) {
            return Result<bool>::Success(false);
        }

        // 리플렉션 쓰기(상자 경유)와 같게 값 트리 안의 struct 검증 hook을 먼저 본다(거부는
        // InvalidArgument — 필드 그대로).
        if constexpr (ValueValidate<M>::HasHook) {
            if (auto checked = ValidateValueTree(value); !checked) {
                return Result<bool>::Failure(checked.Error());
            }
        }

        if constexpr (IsPlainCopyV<M>) {
            field = value;
        } else if constexpr (IsArrayV<M>) {
            using Element = RemoveExtentT<M>;
            if constexpr (IsPlainCopyV<Element>) {
                for (usize index = 0; index < sizeof(M) / sizeof(Element); ++index) {
                    field[index] = value[index];
                }
            } else {
                static_assert(ValueCopy<M>::Supported,
                              "the static array field cannot be copied by value (ValueCopy)");
                // 원소 자신의 allocator(String[N] 등), 없으면 객체 Registry의 것이다.
                Allocator* allocator = ValueAllocatorOf(object, field[0]);
                if (!allocator) {
                    return Result<bool>::Failure(ErrorCode::Unsupported);
                }

                // 정적 배열의 복사는 원소를 모두 따로 만든 뒤 옮긴다(실패하면 그대로).
                if (auto copied = ValueCopy<M>::Copy(field, value, *allocator); !copied) {
                    return Result<bool>::Failure(copied.Error());
                }
            }
        } else {
            static_assert(ValueCopy<M>::Supported && ValueSwap<M>::Supported,
                          "the field cannot be copied by value (ValueCopy) - Registry reference "
                          "fields are written in place through ObjectRegistry (RMC2903)");
            Allocator* allocator = ValueAllocatorOf(object, field);
            if (!allocator) {
                return Result<bool>::Failure(ErrorCode::Unsupported);
            }

            M copy = MakeAllocatedValue<M>(*allocator);
            if (auto copied = ValueCopy<M>::Copy(copy, value, *allocator); !copied) {
                return Result<bool>::Failure(copied.Error());
            }

            // 할당 없는 교환이 commit이다. copy가 옛 값을 쥐고 이 함수 끝에서 반환한다.
            ValueSwap<M>::Swap(field, copy);
        }

        return Result<bool>::Success(true);
    }

    // value(리플렉션 쓰기의 상자 값 — 호출자가 버린다)를 교환으로 넣는다(생성 setter thunk —
    // CN-D14). 같으면 false다. 사본을 만들지 않으므로 할당 실패가 없다(값 검증은 상자 위 경로가
    // 이미 했다).
    template <M C::* Member, class Value = M>
    static Result<bool> AssignOwned(Object& object, Value& value) noexcept {
        static_assert(!IsFunctionV<M>, "the setter thunk needs a data member");
        M& field = static_cast<C&>(object).*Member;
        if (ValueSame<M>::Same(field, value)) {
            return Result<bool>::Success(false);
        }

        if constexpr (IsPlainCopyV<M>) {
            field = value;
        } else {
            static_assert(ValueSwap<M>::Supported,
                          "the field cannot be swapped without allocation");
            ValueSwap<M>::Swap(field, value);
        }

        return Result<bool>::Success(true);
    }

    // Member(Notify 필드·계산 값 함수)의 변경을 object의 구독자에게 알린다(CN-D9).
    template <M C::* Member>
    static Result<void> Notify(Object& object) noexcept {
        return object.NotifySelf(GeneratedType<C>::template NotifyId<Member>());
    }

    // 통지할 쓰기의 대입 전 검사다(접근 문맥·스레드·종료 — Registry가 없으면 Success). 생성
    // setter의 Notify 필드와 SetAndNotify가 대입 전에 부른다.
    static Result<void> Prepare(const Object& object) noexcept {
        return object.CheckNotifyAccess();
    }

    // 대입 전 검사 → Assign → 바뀌었으면 Notify다(Object::SetAndNotify). 생성 setter가 있는 필드는
    // setter를 쓴다(Apply를 건너뛰지 않게 — CN-D13).
    template <M C::* Member, class Value = M>
    static Result<bool> SetAndNotify(Object& object, SetterParam<Value> value) noexcept {
        static_assert(!GeneratedType<C>::template HasSetter<Member>,
                      "the field has a ROCKS_SETTER: call the generated setter so Apply runs "
                      "(ChangeNotification CN-D13)");
        if (auto ready = Prepare(object); !ready) {
            return Result<bool>::Failure(ready.Error());
        }

        auto assigned = Assign<Member>(object, value);
        if (!assigned || !assigned.Value()) {
            return assigned;
        }

        if (auto notified = Notify<Member>(object); !notified) {
            return Result<bool>::Failure(notified.Error());
        }

        return Result<bool>::Success(true);
    }

private:
    // 사본의 allocator다: 필드 자신의 것(String·Array·Map·Set·Soft의 경로), 없으면 객체 Registry의
    // 객체 allocator(Registry가 없으면 nullptr).
    template <class Value>
    static Allocator* ValueAllocatorOf(const Object& object, const Value& field) noexcept {
        if constexpr (requires { field.GetAllocator(); }) {
            return &field.GetAllocator();
        } else if constexpr (requires { field.objectPath.GetAllocator(); }) {
            return &field.objectPath.GetAllocator();
        } else {
            return object.ValueAllocator();
        }
    }
};

} // namespace rocks::detail
