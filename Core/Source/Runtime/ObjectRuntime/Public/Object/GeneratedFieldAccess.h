#pragma once

#include <Object/AssetId.h>
#include <Object/ConstructionContext.h>
#include <Object/SoftObjectRef.h>

namespace rocks::detail {

template <class T>
struct ObjectReferenceField {
    static constexpr bool Supported = false;
};

template <class T>
struct ObjectReferenceField<ObjectRef<T>> {
    static constexpr bool Supported = true;
};

template <class T>
struct SoftReferenceField {
    static constexpr bool Supported = false;
};

template <class T>
struct SoftReferenceField<SoftObjectRef<T>> {
    static constexpr bool Supported = true;
};

template <class T>
constexpr ValueKind FieldValueKind() noexcept {
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

template <class Member>
Result<void> CopyFieldValue(const Member& source, FieldValue& value) noexcept {
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    value.kind = FieldValueKind<Member>();
    if constexpr (IsSameV<Member, String>) {
        return value.string.TryAssign(source.View());
    } else if constexpr (SoftReferenceField<Member>::Supported) {
        value.guid = source.asset.value;
        return value.string.TryAssign(source.objectPath.View());
    } else if constexpr (IsSameV<Member, AssetId>) {
        value.guid = source.value;
    } else if constexpr (IsSameV<Member, Sha256Digest>) {
        value.digest = source;
    } else if constexpr (ObjectReferenceField<Member>::Supported) {
        value.reference = source.Handle();
    } else if constexpr (IsSameV<Member, bool>) {
        value.boolean = source;
    } else if constexpr (IsSameV<Member, Guid>) {
        value.guid = source;
    } else if constexpr (IsSameV<Member, float>) {
        value.float32 = source;
    } else if constexpr (IsSameV<Member, double>) {
        value.float64 = source;
    } else if constexpr (IsSameV<Member, int32> || IsSameV<Member, int64>) {
        value.signedInteger = source;
    } else {
        value.unsignedInteger = source;
    }
    return Result<void>::Success();
}

template <class Member>
Result<void> WriteScalarField(Member& target, const FieldValue& value) noexcept {
    if constexpr (IsSameV<Member, bool>) {
        target = value.boolean;
    } else if constexpr (IsSameV<Member, Guid>) {
        target = value.guid;
    } else if constexpr (IsSameV<Member, AssetId>) {
        target = AssetId{value.guid};
    } else if constexpr (IsSameV<Member, Sha256Digest>) {
        target = value.digest;
    } else if constexpr (IsSameV<Member, float>) {
        target = value.float32;
    } else if constexpr (IsSameV<Member, double>) {
        target = value.float64;
    } else if constexpr (IsSameV<Member, int32>) {
        if (value.signedInteger < -2147483648ll || value.signedInteger > 2147483647ll) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        target = static_cast<int32>(value.signedInteger);
    } else if constexpr (IsSameV<Member, int64>) {
        target = value.signedInteger;
    } else {
        const uint64 maximum = static_cast<uint64>(static_cast<Member>(~Member(0)));
        if (value.unsignedInteger > maximum) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }
        target = static_cast<Member>(value.unsignedInteger);
    }
    return Result<void>::Success();
}

// Generated code가 typed member pointer를 전달한다. raw offset/레이아웃 추정은 사용하지 않는다.
// 별도 serializer/도구도 같은 context/handle 경계를 지나며 pointer/view를 반환받지 않는다.
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
    output = Move(candidate);
    return Result<void>::Success();
}

template <class Owner, class MemberOwner, class Member, class Getter>
Result<void> WriteGeneratedValue(Member MemberOwner::* member, const FieldValue& value,
                                 Getter&& get) noexcept {
    static_assert(__is_base_of(MemberOwner, Owner));
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    auto target = get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }
    if constexpr (IsSameV<Member, String>) {
        String replacement((target.Value()->*member).GetAllocator());
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
        if (auto valid = Member::Validate(asset, value.string.View()); !valid) {
            return valid;
        }

        String replacement((target.Value()->*member).objectPath.GetAllocator());
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

template <class Owner, class MemberOwner, class Member>
Result<void> WriteGeneratedField(ObjectRegistry& registry, const ObjectAccessContext& access,
                                 ObjectHandle handle, Member MemberOwner::* member,
                                 const FieldValue& value) noexcept {
    if (value.kind != FieldValueKind<Member>()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }
    auto borrowed = registry.Resolve<Owner>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }
    if constexpr (ObjectReferenceField<Member>::Supported) {
        auto target = borrowed.Value().Get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }
        return registry.SetReference(access, handle, target.Value()->*member, value.reference);
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
