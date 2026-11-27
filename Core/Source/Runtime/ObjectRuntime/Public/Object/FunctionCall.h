#pragma once

#include <Base/PlacementNew.h>
#include <Core/Memory/Allocator.h>
#include <Meta/GeneratedType.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/NameField.h>
#include <Object/ObjectRegistry.h>
#include <Object/RuntimeTypeOps.h>
#include <Object/TypeLease.h>
#include <Object/ValueCopy.h>
#include <Platform/Memory.h>

// 리플렉션 함수의 범용 호출이다(ROCKS_FUNCTION — Docs/Design/FunctionReflection.ko.md FD7–FD9, UE
// UObject::ProcessEvent·CallFunctionByNameWithArguments 대응). TypeRegistry::FindFunction이 돌려준
// FunctionHandle(함수 서술·생성 연산과 선언 타입의 lease)로 FunctionCall을 만들고, 인자를 쓰고,
// 대상 handle로 부른 뒤 결과를 읽는다. 호출 frame은 FunctionCall이 할당·생성·소멸한다.
//
// 사용 예:
//   auto function = types.FindFunctionByName(TypeIdOf<SceneComponent>(), "SetMobility");
//   if (!function) { return Result<void>::Failure(function.Error()); }
//   FunctionCall call(allocator, Move(function.Value()));
//   if (auto prepared = call.Prepare(); !prepared) { return prepared; }
//   FieldValue mobility(allocator);
//   mobility.kind = ValueKind::Enum;
//   mobility.type = TypeIdOf<ComponentMobility>();
//   mobility.signedInteger = static_cast<int64>(ComponentMobility::Movable);
//   if (auto set = call.SetArgument(registry, access, 0, mobility); !set) { return set; }
//   if (auto called = call.Invoke(registry, access, component); !called) { return called; }
//   if (call.FunctionError() != ErrorCode::None) { /* 함수 자신의 실패(Result) */ }
namespace rocks {

// 찾은 리플렉션 함수 하나다(이동 전용). 선언 타입의 TypeLease를 쥐어 호출하는 동안 Unregister·모듈
// 회수가 서술·thunk를 무효로 만들지 않는다(RuntimeTypeOps의 module 수명 — FD9).
class FunctionHandle {
public:
    FunctionHandle(TypeLease owner, const FunctionSchema& schema,
                   const RuntimeFunctionOps& ops) noexcept
        : owner_(Move(owner)), schema_(&schema), ops_(&ops) {
    }

    FunctionHandle(FunctionHandle&&) noexcept = default;
    FunctionHandle& operator=(FunctionHandle&&) noexcept = default;
    FunctionHandle(const FunctionHandle&) = delete;
    FunctionHandle& operator=(const FunctionHandle&) = delete;

    const FunctionSchema& Schema() const noexcept {
        return *schema_;
    }

    const RuntimeFunctionOps& Ops() const noexcept {
        return *ops_;
    }

    // 함수를 선언한 클래스의 lease다(호출 대상은 그 클래스이거나 파생이다).
    const TypeLease& Owner() const noexcept {
        return owner_;
    }

private:
    TypeLease owner_;
    const FunctionSchema* schema_;
    const RuntimeFunctionOps* ops_;
};

// 호출 frame 칸 하나의 서술이다: 생성 ParameterSchema와 같은 값 형식(value·element·key — 대상
// TypeId 포함, rocks::Object 대상은 Guid{}), ParameterFlags 중 형식을 가르는 것(ObjectHandle), 칸
// C++ 형식의 크기·정렬, frame 안 offset. 실행 이미지마다 다른 NativeTypeKey를 쓰지 않으므로
// Modular 빌드의 모듈 사이 바인딩에서도 같다.
struct FrameSlotLayout {
    ValueType value{};
    ValueType element{};
    ValueType key{};
    uint32 flags = 0;
    uint32 size = 0;
    uint32 alignment = 0;
    uint32 offset = 0;
};

namespace detail {

// 바인딩 인자 형식의 frame 칸 형식이다(const T&는 T — 생성 frame과 같다).
template <class T>
using BindingSlotType = RemoveCvT<RemoveReferenceT<T>>;

// 참조 대상의 schema TypeId다(rocks::Object 대상은 Guid{} — 생성 schema와 같다).
template <class Target>
constexpr TypeId BindingTargetIdOf() noexcept {
    if constexpr (IsSameV<Target, Object>) {
        return TypeId{};
    } else {
        return TypeIdOf<Target>();
    }
}

// 단일 값 형식 T(컨테이너 아님)의 ValueType이다.
// enum 칸은 생성 헤더가 보이는 리플렉션 enum이어야 한다. enum에는 선언 표지가 없어
// (GeneratedHeaderVisible이 잡지 못한다) 생성 헤더가 빠진 TU는 칸을 Opaque로 보고, 이 판정을
// 담은 inline 배치(FrameLayoutOf·EventOps의 frameLayout)가 TU마다 달라져 링커가 고른 판이 생성
// schema와 어긋난다(ODR — Docs/Design/EventReflection.ko.md ED18).
template <class T>
constexpr ValueType BindingValueTypeOf() noexcept {
    static_assert(!IsEnumV<T> || ReflectedEnumField<T>::Supported,
                  "an enum argument of a reflected function or event must be a reflected enum "
                  "whose generated header is included in this translation unit; include "
                  "<Module/Header.generated.h> of the enum (or of the class that declares the "
                  "event) before this use");
    if constexpr (IsSameV<T, ObjectHandle>) {
        return ValueType{ValueKind::WeakObjectReference, TypeId{}};
    } else if constexpr (WeakReferenceField<T>::Supported) {
        return ValueType{ValueKind::WeakObjectReference,
                         BindingTargetIdOf<typename WeakReferenceField<T>::Target>()};
    } else if constexpr (SoftReferenceField<T>::Supported) {
        return ValueType{ValueKind::SoftObjectReference,
                         BindingTargetIdOf<typename SoftReferenceField<T>::Target>()};
    } else if constexpr (ClassReferenceField<T>::Supported) {
        return ValueType{ValueKind::ClassReference,
                         BindingTargetIdOf<typename ClassReferenceField<T>::Target>()};
    } else if constexpr (FieldValueKind<T>() == ValueKind::Enum ||
                         FieldValueKind<T>() == ValueKind::Struct) {
        return ValueType{FieldValueKind<T>(), TypeIdOf<T>()};
    } else {
        return ValueType{FieldValueKind<T>(), TypeId{}};
    }
}

// frame 칸 형식 T의 서술이다(offset 제외 — 생성 SlotSchema와 같은 value·element·key 규칙).
template <class T>
constexpr FrameSlotLayout BindingSlotLayoutOf() noexcept {
    FrameSlotLayout slot;
    if constexpr (ArrayField<T>::Supported) {
        slot.value = ValueType{ValueKind::Array, TypeId{}};
        slot.element = BindingValueTypeOf<typename ArrayField<T>::Element>();
    } else if constexpr (OptionalField<T>::Supported) {
        slot.value = ValueType{ValueKind::Optional, TypeId{}};
        slot.element = BindingValueTypeOf<typename OptionalField<T>::Inner>();
    } else if constexpr (MapField<T>::Supported) {
        if constexpr (MapField<T>::IsSet) {
            slot.value = ValueType{ValueKind::Set, TypeId{}};
            slot.element = BindingValueTypeOf<typename MapField<T>::Key>();
        } else {
            slot.value = ValueType{ValueKind::Map, TypeId{}};
            slot.element = BindingValueTypeOf<typename MapField<T>::Value>();
            slot.key = BindingValueTypeOf<typename MapField<T>::Key>();
        }
    } else {
        slot.value = BindingValueTypeOf<T>();
    }

    slot.flags = IsSameV<T, ObjectHandle> ? static_cast<uint32>(ParameterFlags::ObjectHandle) : 0u;
    // 칸 FrameSlot<T>의 크기·정렬은 T와 같다(정렬된 byte 저장소).
    slot.size = static_cast<uint32>(sizeof(T));
    slot.alignment = static_cast<uint32>(alignof(T));
    return slot;
}

// 칸 서술이 같은지다(offset 제외 — 값 형식 value·element·key, 형식 flag, 값 ops 크기·정렬). 실행
// 이미지마다 다른 NativeTypeKey를 쓰지 않는다(Docs/Design/EventReflection.ko.md ED5).
ROCKS_OBJECTRUNTIME_API bool SlotDescriptionMatches(const ParameterSchema& parameter,
                                                    const RuntimeValueOps* value,
                                                    const FrameSlotLayout& expected) noexcept;

} // namespace detail

// 호출 하나다(이동·복사 금지 — frame 주소를 쥔다). Prepare가 frame을 할당하고 모든 칸을 기본값으로
// 만든다. 쓰지 않은 인자는 그 형식의 기본값이다. Invoke는 여러 번 부를 수 있다(같은 frame — 인자는
// 남고 결과는 덮어쓴다). 소유 스레드 전용이다.
class ROCKS_OBJECTRUNTIME_API FunctionCall {
public:
    FunctionCall(Allocator& allocator, FunctionHandle function) noexcept
        : allocator_(&allocator), function_(Move(function)) {
    }

    ~FunctionCall() noexcept;
    FunctionCall(const FunctionCall&) = delete;
    FunctionCall& operator=(const FunctionCall&) = delete;
    FunctionCall(FunctionCall&&) = delete;
    FunctionCall& operator=(FunctionCall&&) = delete;

    const FunctionHandle& Function() const noexcept {
        return function_;
    }

    // frame을 할당하고 생성한다(OutOfMemory면 아무것도 남기지 않는다). 두 번 부르면
    // InvalidArgument다.
    Result<void> Prepare() noexcept;

    // 단일 값 매개변수 index에 value를 쓴다(필드 쓰기와 같은 변환·검사 — FD9). struct·컨테이너
    // 매개변수는 InvalidArgument(SetArgumentValue를 쓴다), 범위 밖·준비 전은 InvalidArgument다.
    // 실패하면 칸은 그대로다.
    Result<void> SetArgument(ObjectRegistry& registry, const ObjectAccessContext& access,
                             usize index, const FieldValue& value) noexcept;

    // 매개변수 index에 같은 형식의 값 source를 복사한다(struct·컨테이너 — 값 ops의 copy, 단일 값
    // 칸도 된다). 형식은 칸 서술(값 형식·형식 flag·크기·정렬 — 실행 이미지와 무관하다,
    // EventReflection ED5)로 대조하며 다르면 TypeMismatch다.
    template <class T>
    Result<void> SetArgumentValue(usize index, const T& source) noexcept {
        return SetArgumentDescribed(index, &source,
                                    detail::BindingSlotLayoutOf<detail::BindingSlotType<T>>());
    }

    // 대상 self(static 함수는 null)로 함수를 부른다. 대상 해석 실패(WrongThread·WrongContext·
    // StaleHandle·Destroyed·TypeMismatch·Busy·ShuttingDown)는 그 오류이고, static에 handle을 주거나
    // 인스턴스 함수에 null을 주면 InvalidArgument, 준비 전이면 InvalidArgument다. 함수 자신의
    // 실패(Result 반환)는 성공으로 돌아오고 FunctionError()에 남는다.
    Result<void> Invoke(ObjectRegistry& registry, const ObjectAccessContext& access,
                        ObjectHandle self) noexcept;

    // 단일 값 반환을 value에 읽는다(반환이 없거나 struct·컨테이너면 InvalidArgument — ResultValue).
    Result<void> ReadResult(FieldValue& value) const noexcept;

    // 반환 칸이다(T가 반환 형식과 같을 때 — 칸 서술 대조, 아니면·반환 없음·준비 전 nullptr).
    template <class T>
    const T* ResultValue() const noexcept {
        return static_cast<const T*>(ResultDescribed(detail::BindingSlotLayoutOf<T>()));
    }

    // 마지막 Invoke에서 함수가 돌려준 실패 코드다(Result 함수가 아니거나 성공이면 None).
    ErrorCode FunctionError() const noexcept;

private:
    Result<void> SetArgumentDescribed(usize index, const void* source,
                                      const FrameSlotLayout& expected) noexcept;
    const void* ResultDescribed(const FrameSlotLayout& expected) const noexcept;
    void Release() noexcept;

    Allocator* allocator_;
    FunctionHandle function_;
    void* frame_ = nullptr;
};

namespace detail {

// 매개변수 칸 형식 T에 FieldValue를 쓴다(RuntimeParameterOps::write — 생성 코드가 형식마다 고른다).
// 참조(ObjectHandle·WeakObjectRef)는 kind ObjectReference·WeakObjectReference를 받고 이 Registry의
// 생존 객체·기대 타입이어야 한다(null 허용). ClassRef는 등록된 클래스·Base 파생, Name은 이
// Registry pool의 토큰이다. 그 밖은 필드 쓰기의 WriteTargetValue 규칙이다.
template <class T>
Result<void> WriteParameter(ObjectRegistry& registry, const ObjectAccessContext& access, void* slot,
                            const FieldValue& value, Allocator& valueAllocator) noexcept {
    T& target = *static_cast<T*>(slot);
    if constexpr (IsSameV<T, ObjectHandle>) {
        if (value.kind != ValueKind::ObjectReference &&
            value.kind != ValueKind::WeakObjectReference) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (!value.reference.IsNull()) {
            if (auto valid =
                    ValidateWeakTarget(registry, access, value.reference, NativeTypeKey<Object>());
                !valid) {
                return valid;
            }
        }

        target = value.reference;
        return Result<void>::Success();
    } else if constexpr (WeakReferenceField<T>::Supported) {
        if (value.kind != ValueKind::ObjectReference &&
            value.kind != ValueKind::WeakObjectReference) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (!value.reference.IsNull()) {
            if (auto valid =
                    ValidateWeakTarget(registry, access, value.reference,
                                       NativeTypeKey<typename WeakReferenceField<T>::Target>());
                !valid) {
                return valid;
            }
        }

        target = T(value.reference);
        return Result<void>::Success();
    } else if constexpr (ClassReferenceField<T>::Supported) {
        if (value.kind != ValueKind::ClassReference) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (auto valid =
                ValidateClassReference(registry, access, value.guid, TypeId{},
                                       NativeTypeKey<typename ClassReferenceField<T>::Target>());
            !valid) {
            return valid;
        }

        return WriteScalarField(target, value, &valueAllocator);
    } else if constexpr (OptionalField<T>::Supported) {
        return AssignOptionalValue(target, value, valueAllocator);
    } else {
        if constexpr (IsSameV<T, Name>) {
            if (value.kind != ValueKind::Name) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }

            if (auto valid = ValidateNameFieldToken(registry.Names(), value.name, valueAllocator);
                !valid) {
                return valid;
            }
        }

        return WriteScalarField(target, value, &valueAllocator);
    }
}

// 매개변수·반환 칸 형식 T를 FieldValue로 읽는다(RuntimeParameterOps::read). ObjectHandle은
// ObjectReference 값이다.
template <class T>
Result<void> ReadParameter(const void* slot, FieldValue& value) noexcept {
    const T& source = *static_cast<const T*>(slot);
    if constexpr (IsSameV<T, ObjectHandle>) {
        value.kind = ValueKind::ObjectReference;
        value.reference = source;
        value.present = true;
        return Result<void>::Success();
    } else {
        return CopyFieldValue(source, value);
    }
}

// 생성 호출 frame의 칸 하나다. 값을 정렬된 byte 저장소 맨 앞에 두므로 값 형식이 표준 배치가
// 아니어도(base가 있는 struct, 접근 수준이 섞인 struct) frame은 표준 배치라 offsetof로 칸 위치를
// 정한다. Construct·Destroy는 frame의 생성자·소멸자만 부른다(한 번씩).
template <class T>
struct alignas(T) FrameSlot {
    void Construct(Allocator& allocator) noexcept {
        ::new (static_cast<void*>(bytes)) T(MakeAllocatedValue<T>(allocator));
    }

    void Destroy() noexcept {
        Get().~T();
    }

    T& Get() noexcept {
        return *platform::Launder(reinterpret_cast<T*>(bytes));
    }

    unsigned char bytes[sizeof(T)];
};

// 생성 호출 frame Frame(allocator를 받는 생성자 — 칸마다 FrameSlot::Construct)을 빈 저장소에
// 만든다(RuntimeFunctionOps::construct).
template <class Frame>
Result<void> ConstructFunctionFrame(void* frame, Allocator& allocator) noexcept {
    ::new (frame) Frame(allocator);
    return Result<void>::Success();
}

// 생성 호출 frame을 소멸한다(RuntimeFunctionOps::destroy).
template <class Frame>
void DestroyFunctionFrame(void* frame) noexcept {
    static_cast<Frame*>(frame)->~Frame();
}

// 칸 형식 T가 FieldValue로 통째 읽고 쓰지 않는 형식(struct·컨테이너)인지다.
template <class T>
constexpr bool IsAggregateParameter() noexcept {
    if constexpr (IsSameV<T, ObjectHandle>) {
        return false;
    } else if constexpr (OptionalField<T>::Supported) {
        return FieldValueKind<typename OptionalField<T>::Inner>() == ValueKind::Struct;
    } else {
        return FieldValueKind<T>() == ValueKind::Struct || ArrayField<T>::Supported ||
               MapField<T>::Supported;
    }
}

// 칸 형식 T의 연산표다(생성 코드의 RuntimeFunctionOps::parameters·result 원소).
template <class T>
RuntimeParameterOps ParameterOpsOf() noexcept {
    RuntimeParameterOps ops;
    ops.value = &ValueOpsOf<T>();
    if constexpr (!IsAggregateParameter<T>()) {
        ops.write = &WriteParameter<T>;
        ops.read = &ReadParameter<T>;
    }

    return ops;
}

} // namespace detail

} // namespace rocks
