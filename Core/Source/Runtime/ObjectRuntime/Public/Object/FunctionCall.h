#pragma once

#include <Core/Memory/Allocator.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/NameField.h>
#include <Object/ObjectRegistry.h>
#include <Object/RuntimeTypeOps.h>
#include <Object/TypeLease.h>
#include <Object/ValueCopy.h>

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

    // 매개변수 index에 같은 C++ 형식(nativeType = NativeTypeKey<T>())의 값 source를
    // 복사한다(struct· 컨테이너 — 값 ops의 copy, 단일 값 칸도 된다). 형식이 다르면 TypeMismatch다.
    Result<void> SetArgumentValue(usize index, const void* source, const void* nativeType) noexcept;

    // 대상 self(static 함수는 null)로 함수를 부른다. 대상 해석 실패(WrongThread·WrongContext·
    // StaleHandle·Destroyed·TypeMismatch·Busy·ShuttingDown)는 그 오류이고, static에 handle을 주거나
    // 인스턴스 함수에 null을 주면 InvalidArgument, 준비 전이면 InvalidArgument다. 함수 자신의
    // 실패(Result 반환)는 성공으로 돌아오고 FunctionError()에 남는다.
    Result<void> Invoke(ObjectRegistry& registry, const ObjectAccessContext& access,
                        ObjectHandle self) noexcept;

    // 단일 값 반환을 value에 읽는다(반환이 없거나 struct·컨테이너면 InvalidArgument — ResultValue).
    Result<void> ReadResult(FieldValue& value) const noexcept;

    // 반환 칸의 주소다(nativeType이 반환 형식과 같을 때 — 아니면·반환 없음·준비 전 nullptr).
    const void* ResultValue(const void* nativeType) const noexcept;

    // 마지막 Invoke에서 함수가 돌려준 실패 코드다(Result 함수가 아니거나 성공이면 None).
    ErrorCode FunctionError() const noexcept;

private:
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
