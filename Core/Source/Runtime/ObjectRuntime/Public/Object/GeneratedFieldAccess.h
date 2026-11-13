#pragma once

#include <Meta/GeneratedType.h>
#include <Object/AssetId.h>
#include <Object/ConstructionContext.h>
#include <Object/SoftObjectRef.h>
#include <Core/ScopeExit.h>

namespace rocks::detail {

template <class T>
struct ObjectReferenceField {
    static constexpr bool Supported = false;
};

template <class T>
struct ObjectReferenceField<ObjectRef<T>> {
    static constexpr bool Supported = true;
    using Target = T;
};

template <class T>
struct SoftReferenceField {
    static constexpr bool Supported = false;
};

template <class T>
struct SoftReferenceField<SoftObjectRef<T>> {
    static constexpr bool Supported = true;
};

// 리플렉션 enum(ROCKS_ENUM)이다: 생성 헤더가 GeneratedType<E>::Id·IsDeclared를 내는 enum class다.
template <class T>
struct ReflectedEnumField {
    static constexpr bool Supported = __is_enum(T) && requires {
        GeneratedType<T>::Id();
        GeneratedType<T>::IsDeclared(int64{});
    };
};

// 리플렉션 struct(ROCKS_STRUCT)다: 생성 헤더가 GeneratedType<S>::Id·ContainsStrongReference를 내는
// 비enum 클래스다(Docs/Design/ReflectionFoundations.ko.md 4.6절).
template <class T>
struct ReflectedStructField {
    static constexpr bool Supported = !__is_enum(T) && __is_class(T) && requires {
        GeneratedType<T>::Id();
        GeneratedType<T>::ContainsStrongReference;
    };
};

// 리플렉션 배열 필드의 컨테이너다(Docs/Design/ReflectionFoundations.ko.md 5.6절). 부분 특수화로만
// 고르며(requires 탐지 없음) Element는 원소의 C++ 형식이다: Array<E>는 E(스칼라·String·Guid·
// AssetId·Digest·enum·SoftObjectRef·참조 없는 struct), ObjectRefArray<T>는 ObjectRef<T>(원소 null
// 허용), StructRefArray<S>는 참조 struct S다. Registry는 원소를 Registry가 관리하는 컨테이너
// (ObjectRefArray·StructRefArray)인지다.
template <class T>
struct ArrayField {
    static constexpr bool Supported = false;
};

template <class E>
struct ArrayField<Array<E>> {
    static constexpr bool Supported = true;
    static constexpr bool Registry = false;
    using Element = E;
};

template <class T>
struct ArrayField<ObjectRefArray<T>> {
    static constexpr bool Supported = true;
    static constexpr bool Registry = true;
    using Element = ObjectRef<T>;
};

template <class S>
struct ArrayField<StructRefArray<S>> {
    static constexpr bool Supported = true;
    static constexpr bool Registry = true;
    using Element = S;
};

// struct 멤버 포인터의 소유 struct·멤버 형식이다(생성 codec·경로 thunk가 같이 쓴다 — 경로 헤더가
// Serialization에 의존하지 않도록 여기에 둔다).
template <class T>
struct StructMemberPointer;

template <class S, class M>
struct StructMemberPointer<M S::*> {
    using Owner = S;
    using Member = M;
};

template <class T>
constexpr ValueKind FieldValueKind() noexcept {
    if constexpr (ArrayField<T>::Supported) {
        return ValueKind::Array;
    } else if constexpr (IsSameV<T, bool>) {
        return ValueKind::Bool;
    } else if constexpr (IsSameV<T, uint8>) {
        return ValueKind::UInt8;
    } else if constexpr (IsSameV<T, uint16>) {
        return ValueKind::UInt16;
    } else if constexpr (IsSameV<T, uint32>) {
        return ValueKind::UInt32;
    } else if constexpr (IsSameV<T, uint64>) {
        return ValueKind::UInt64;
    } else if constexpr (IsSameV<T, int32>) {
        return ValueKind::Int32;
    } else if constexpr (IsSameV<T, int64>) {
        return ValueKind::Int64;
    } else if constexpr (IsSameV<T, float>) {
        return ValueKind::Float32;
    } else if constexpr (IsSameV<T, double>) {
        return ValueKind::Float64;
    } else if constexpr (IsSameV<T, Guid>) {
        return ValueKind::Guid;
    } else if constexpr (IsSameV<T, String>) {
        return ValueKind::Utf8String;
    } else if constexpr (ObjectReferenceField<T>::Supported) {
        return ValueKind::ObjectReference;
    } else if constexpr (IsSameV<T, AssetId>) {
        return ValueKind::AssetId;
    } else if constexpr (IsSameV<T, Sha256Digest>) {
        return ValueKind::Digest256;
    } else if constexpr (SoftReferenceField<T>::Supported) {
        return ValueKind::SoftObjectReference;
    } else if constexpr (ReflectedEnumField<T>::Supported) {
        return ValueKind::Enum;
    } else if constexpr (ReflectedStructField<T>::Supported) {
        return ValueKind::Struct;
    } else {
        return ValueKind::Opaque;
    }
}

// C++ 형식의 단일 값 종류다(enum·struct·컨테이너는 Opaque — 그 셋은 Target과 정적 대조로 가린다).
// requires 탐지 없이 IsSameV와 부분 특수화(ObjectReferenceField·SoftReferenceField)만 쓴다.
template <class T>
constexpr ValueKind ScalarValueKind() noexcept {
    if constexpr (IsSameV<T, bool>) {
        return ValueKind::Bool;
    } else if constexpr (IsSameV<T, uint8>) {
        return ValueKind::UInt8;
    } else if constexpr (IsSameV<T, uint16>) {
        return ValueKind::UInt16;
    } else if constexpr (IsSameV<T, uint32>) {
        return ValueKind::UInt32;
    } else if constexpr (IsSameV<T, uint64>) {
        return ValueKind::UInt64;
    } else if constexpr (IsSameV<T, int32>) {
        return ValueKind::Int32;
    } else if constexpr (IsSameV<T, int64>) {
        return ValueKind::Int64;
    } else if constexpr (IsSameV<T, float>) {
        return ValueKind::Float32;
    } else if constexpr (IsSameV<T, double>) {
        return ValueKind::Float64;
    } else if constexpr (IsSameV<T, Guid>) {
        return ValueKind::Guid;
    } else if constexpr (IsSameV<T, String>) {
        return ValueKind::Utf8String;
    } else if constexpr (ObjectReferenceField<T>::Supported) {
        return ValueKind::ObjectReference;
    } else if constexpr (IsSameV<T, AssetId>) {
        return ValueKind::AssetId;
    } else if constexpr (IsSameV<T, Sha256Digest>) {
        return ValueKind::Digest256;
    } else if constexpr (SoftReferenceField<T>::Supported) {
        return ValueKind::SoftObjectReference;
    } else {
        return ValueKind::Opaque;
    }
}

// 대상 종류 Target과 C++ 대상 형식 T(배열이면 원소)가 맞는지의 컴파일 시 대조다(경로 thunk는
// MetaCompiler가 명시한 TargetKind — 틀리면 컴파일 오류, 기존 생성 접근기는 FieldValueKind<T>()).
template <ValueKind Target, class T>
struct TargetValueCheck {
    static constexpr bool Valid = Target == ValueKind::Enum ? __is_enum(T)
                                  : Target == ValueKind::Struct
                                      ? __is_class(T) &&
                                            ScalarValueKind<T>() == ValueKind::Opaque &&
                                            !ArrayField<T>::Supported
                                      : Target != ValueKind::Opaque && Target != ValueKind::Array &&
                                            ScalarValueKind<T>() == Target;
    static_assert(Valid, "path thunk target kind does not match the C++ member type");
};

// 단일 값 하나를 Target 종류로 FieldValue에 복사한다(String·Soft 경로는 value의 allocator로 할당 —
// OutOfMemory). enum은 정수와 GeneratedType<E>::Id()를 싣는다. 참조는 handle을 싣는다.
//
// 사용 예:
//   FieldValue value(allocator);
//   auto copied = detail::CopyTargetValue<ValueKind::UInt32>(slot.index, value);
template <ValueKind Target, class T>
Result<void> CopyTargetValue(const T& source, FieldValue& value) noexcept {
    static_assert(TargetValueCheck<Target, T>::Valid);
    static_assert(Target != ValueKind::Struct, "struct values are not FieldValue");
    value.kind = Target;
    if constexpr (Target == ValueKind::Utf8String) {
        return value.string.TryAssign(source.View());
    } else if constexpr (Target == ValueKind::SoftObjectReference) {
        value.guid = source.asset.value;
        return value.string.TryAssign(source.objectPath.View());
    } else if constexpr (Target == ValueKind::AssetId) {
        value.guid = source.value;
    } else if constexpr (Target == ValueKind::Digest256) {
        value.digest = source;
    } else if constexpr (Target == ValueKind::Enum) {
        value.signedInteger = static_cast<int64>(source);
        value.type = GeneratedType<T>::Id();
    } else if constexpr (Target == ValueKind::ObjectReference) {
        value.reference = source.Handle();
    } else if constexpr (Target == ValueKind::Bool) {
        value.boolean = source;
    } else if constexpr (Target == ValueKind::Guid) {
        value.guid = source;
    } else if constexpr (Target == ValueKind::Float32) {
        value.float32 = source;
    } else if constexpr (Target == ValueKind::Float64) {
        value.float64 = source;
    } else if constexpr (Target == ValueKind::Int32 || Target == ValueKind::Int64) {
        value.signedInteger = source;
    } else {
        value.unsignedInteger = source;
    }

    return Result<void>::Success();
}

// FieldValue를 Target 종류의 단일 값 target에 쓴다. 정수 폭이 넘치면 Overflow, enum은 값의 enum
// ID가 GeneratedType<E>::Id()가 아니면 TypeMismatch·선언되지 않은 값이면 InvalidArgument, Soft는
// null 규칙·경로 상한 위반이 InvalidArgument다(실패하면 target은 그대로다). String·Soft는 target의
// allocator로 할당한다(OutOfMemory — 이때는 제자리 대입이므로 원자적 게시가 필요한 쓰기는 새 값에
// 쓴 뒤 Swap한다). 참조는 Registry로만 쓰므로 받지 않는다(컴파일 오류).
template <ValueKind Target, class T>
Result<void> WriteTargetValue(T& target, const FieldValue& value) noexcept {
    static_assert(TargetValueCheck<Target, T>::Valid);
    static_assert(Target != ValueKind::Struct && Target != ValueKind::ObjectReference,
                  "struct values and references are not written through FieldValue");
    if (value.kind != Target) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (Target == ValueKind::Utf8String) {
        return target.TryAssign(value.string.View());
    } else if constexpr (Target == ValueKind::SoftObjectReference) {
        return target.TryAssign(AssetId{value.guid}, value.string.View());
    } else if constexpr (Target == ValueKind::AssetId) {
        target = AssetId{value.guid};
    } else if constexpr (Target == ValueKind::Digest256) {
        target = value.digest;
    } else if constexpr (Target == ValueKind::Enum) {
        if (value.type != GeneratedType<T>::Id()) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (!GeneratedType<T>::IsDeclared(value.signedInteger)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        target = static_cast<T>(value.signedInteger);
    } else if constexpr (Target == ValueKind::Bool) {
        target = value.boolean;
    } else if constexpr (Target == ValueKind::Guid) {
        target = value.guid;
    } else if constexpr (Target == ValueKind::Float32) {
        target = value.float32;
    } else if constexpr (Target == ValueKind::Float64) {
        target = value.float64;
    } else if constexpr (Target == ValueKind::Int32) {
        if (value.signedInteger < -2147483648ll || value.signedInteger > 2147483647ll) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        target = static_cast<int32>(value.signedInteger);
    } else if constexpr (Target == ValueKind::Int64) {
        target = value.signedInteger;
    } else {
        const uint64 maximum = static_cast<uint64>(static_cast<T>(~T(0)));
        if (value.unsignedInteger > maximum) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        target = static_cast<T>(value.unsignedInteger);
    }

    return Result<void>::Success();
}

// 멤버 값을 FieldValue로 복사한다(변환 규칙은 CopyTargetValue 한 벌 — 값 종류는 FieldValueKind).
// String·Soft 경로는 value의 allocator로 새 버퍼를 할당한 뒤 원본을 읽으므로, owner 객체의 멤버를
// 복사하는 호출자는 owner를 encode 고정해 할당 콜백이 원본을 바꾸거나 해제하지 못하게
// 한다(ReadGeneratedField — 고정 중 재진입 쓰기는 Busy).
template <class Member>
Result<void> CopyFieldValue(const Member& source, FieldValue& value) noexcept {
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    return CopyTargetValue<FieldValueKind<Member>()>(source, value);
}

// 단일 값(String·Soft·참조 제외) 멤버에 FieldValue를 쓴다(변환 규칙은 WriteTargetValue 한 벌).
// enum은 이 필드의 enum ID와 선언된 값(Flags는 선언 값의 OR)만 받는다 — staged 쓰기·복원도 이
// 검사를 지난다(TypeRegistry를 거치지 않는다).
template <class Member>
Result<void> WriteScalarField(Member& target, const FieldValue& value) noexcept {
    return WriteTargetValue<FieldValueKind<Member>()>(target, value);
}

// Generated code가 typed member pointer를 전달한다. raw offset/레이아웃 추정은 사용하지 않는다.
// 별도 serializer/도구도 같은 context/handle 경계를 지나며 pointer/view를 반환받지 않는다.
// 복사는 owner의 encode 고정(PinForEncode) 아래에서 한다: String·Soft 경로 복사(TryAssign)는 새
// 버퍼를 할당한 뒤 원본을 읽으므로, 그 할당 콜백이 같은 필드를 쓰면(WriteGeneratedField —
// ResolveForWrite) 고정 때문에 Busy가 되어 원본 버퍼가 복사 전에 해제되지 않는다. 고정은
// RequestDestroy를 막지 않으므로 out을 게시하기 전에 owner의 논리 수명을 다시 확인하고, out의 옛
// 버퍼는 고정을 푼 뒤 반환한다.
template <class Owner, class MemberOwner, class Member>
Result<void> ReadGeneratedField(ObjectRegistry& registry, const ObjectAccessContext& access,
                                ObjectHandle handle, Member MemberOwner::* member,
                                FieldValue& output) noexcept {
    static_assert(__is_base_of(MemberOwner, Owner));
    auto borrowed = registry.Resolve<Owner>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto target = borrowed.Value().Get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    auto pin = registry.PinForEncode(access, handle);
    if (!pin) {
        return Result<void>::Failure(pin.Error());
    }

    FieldValue candidate(output.GetAllocator());
    auto copied = CopyFieldValue(target.Value()->*member, candidate);
    if (!copied) {
        return copied;
    }

    // String 복사 allocator callback이 owner를 논리 파괴했다면 out을 게시하지 않는다.
    auto stillValid = borrowed.Value().Get();
    if (!stillValid) {
        return Result<void>::Failure(stillValid.Error());
    }

    pin.Value().Reset();
    output = Move(candidate);
    return Result<void>::Success();
}

// 단일 값 멤버 쓰기의 공통 본체다(살아 있는 객체 WriteGeneratedField, staged 쓰기, struct 멤버
// 적용). String·Soft는 대상 멤버의 allocator로 새 값을 만든 뒤 get()으로 owner를 다시 확인하고
// 할당·반환 없는 Swap으로 게시한다. pin은 살아 있는 객체 쓰기가 할당 전에 쥔 encode
// 고정이다(staged·struct 적용은 nullptr): 할당 콜백의 재진입 쓰기(ResolveForWrite)를 Busy로 막아
// 갱신이 덮이지 않게 하고, 게시 또는 실패 직후 풀어 옛 버퍼·실패한 새 버퍼는 고정 밖에서 반환한다.
template <class Owner, class MemberOwner, class Member, class Getter>
Result<void> WriteGeneratedValue(Member MemberOwner::* member, const FieldValue& value,
                                 Getter&& get, EncodePin* pin = nullptr) noexcept {
    static_assert(__is_base_of(MemberOwner, Owner));
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    auto target = get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    if constexpr (IsSameV<Member, String>) {
        String replacement((target.Value()->*member).GetAllocator());
        // replacement보다 뒤에 선언하므로 모든 경로에서 replacement 반환보다 먼저 고정을 푼다.
        auto release = MakeScopeExit([pin]() noexcept {
            if (pin) {
                pin->Reset();
            }
        });
        auto copied = replacement.TryAssign(value.string.View());
        if (!copied) {
            return copied;
        }

        target = get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        // 할당/해제 없는 Swap이 commit이다. 이후 old storage 반환 callback의 RequestDestroy는
        // 이미 완료한 성공을 되돌리지 않는다. 호출 측 Borrow는 이 정리까지 물리 수명을 유지한다.
        (target.Value()->*member).Swap(replacement);
        return Result<void>::Success();
    } else if constexpr (SoftReferenceField<Member>::Supported) {
        // staged 쓰기·복원도 같은 null 규칙·경로 상한을 지킨다(등록된 write의 검증을 거치지
        // 않는다).
        const AssetId asset{value.guid};
        String replacement((target.Value()->*member).objectPath.GetAllocator());
        auto release = MakeScopeExit([pin]() noexcept {
            if (pin) {
                pin->Reset();
            }
        });
        if (auto valid = Member::Validate(asset, value.string.View()); !valid) {
            return valid;
        }

        auto copied = replacement.TryAssign(value.string.View());
        if (!copied) {
            return copied;
        }

        target = get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        // String과 같이 할당 없는 Swap이 commit이다.
        (target.Value()->*member).asset = asset;
        (target.Value()->*member).objectPath.Swap(replacement);
        return Result<void>::Success();
    } else {
        return WriteScalarField(target.Value()->*member, value);
    }
}

// 등록된 생성 write thunk다. 쓰기용 Borrow(ResolveForWrite — owner가 encode 고정 중이면 Busy)를
// 얻고, String·Soft는 새 값 할당부터 Swap까지 owner를 encode 고정해 할당 콜백의 재진입 쓰기를
// Busy로 막는다(고정은 RequestDestroy를 막지 않으므로 게시 직전 owner를 다시 확인한다). 수동
// 함수표의 write callback(FieldWriteOperation이 부른다)에는 이 고정이 없다 — 할당하는 수동
// callback은 같은 규칙을 스스로 지켜야 한다.
template <class Owner, class MemberOwner, class Member>
Result<void> WriteGeneratedField(ObjectRegistry& registry, const ObjectAccessContext& access,
                                 ObjectHandle handle, Member MemberOwner::* member,
                                 const FieldValue& value) noexcept {
    if (value.kind != FieldValueKind<Member>()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // 쓰기용 Borrow: owner가 encode 고정 중이면 Busy다(encode가 순회하는 String·Soft 경로 버퍼를
    // 이 쓰기가 해제하지 못하게 — ObjectRegistry::PinForEncode).
    auto borrowed = registry.ResolveForWrite<Owner>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    if constexpr (ObjectReferenceField<Member>::Supported) {
        auto target = borrowed.Value().Get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        return registry.SetReference(access, handle, target.Value()->*member, value.reference);
    } else if constexpr (IsSameV<Member, String> || SoftReferenceField<Member>::Supported) {
        auto pin = registry.PinForEncode(access, handle);
        if (!pin) {
            return Result<void>::Failure(pin.Error());
        }

        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return borrowed.Value().Get(); }, &pin.Value());
    } else {
        return WriteGeneratedValue<Owner>(member, value,
                                          [&]() noexcept { return borrowed.Value().Get(); });
    }
}

template <class Owner, class MemberOwner, class Member>
Result<void> WriteGeneratedStagedField(ConstructionContext& construction, ObjectHandle handle,
                                       Member MemberOwner::* member,
                                       const FieldValue& value) noexcept {
    if (value.kind != FieldValueKind<Member>()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (ObjectReferenceField<Member>::Supported) {
        auto target = construction.Get<Owner>(handle);
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        return construction.SetReference(handle, target.Value()->*member, value.reference);
    } else {
        return WriteGeneratedValue<Owner>(
            member, value, [&]() noexcept { return construction.Get<Owner>(handle); });
    }
}

} // namespace rocks::detail
