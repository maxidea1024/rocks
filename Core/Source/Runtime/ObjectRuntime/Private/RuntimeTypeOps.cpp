#include <Object/RuntimeTypeOps.h>
#include <Object/ClassRef.h>
#include <Object/NameField.h>
#include <Object/ObjectRegistry.h>
#include <Object/SoftObjectRef.h>
#include <Object/TypeRegistry.h>
#include <Platform/Memory.h>

// RuntimeTypeOps.h의 필드 쓰기 경로다. FieldWriteOperation 호출(문맥·대상 타입·필드 검사 뒤
// callback, Notify 필드의 변경 통지)과 공용 검증(detail::ValidateFieldWrite·ValidateArrayLength·
// ValidateArrayEdit)을 둔다.
namespace rocks {

namespace {

// 최상위 쓰기의 같은 값 판정이다(Docs/Design/ChangeNotification.ko.md CN-D15): 스칼라·enum은 비트
// 같음(0.0과 -0.0은 다름, NaN payload 구별), 문자열은 byte, 참조는 대상 정체성(null끼리 같음),
// Name은 토큰과 표시 문자열. Text와 그 밖의 형식은 늘 다르다.
bool SameFieldValue(const FieldValue& current, const FieldValue& next) noexcept {
    if (current.kind != next.kind || current.present != next.present) {
        return false;
    }

    if (!current.present) {
        return true;
    }

    switch (current.kind) {
    case ValueKind::Bool:
        return current.boolean == next.boolean;
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
        return current.unsignedInteger == next.unsignedInteger;
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
        return current.signedInteger == next.signedInteger;
    case ValueKind::Enum:
        return current.type == next.type && current.signedInteger == next.signedInteger;
    case ValueKind::Float32:
        return platform::BitCast<uint32>(current.float32) ==
               platform::BitCast<uint32>(next.float32);
    case ValueKind::Float64:
        return platform::BitCast<uint64>(current.float64) ==
               platform::BitCast<uint64>(next.float64);
    case ValueKind::Guid:
    case ValueKind::AssetId:
    case ValueKind::ClassReference:
        return current.guid == next.guid;
    case ValueKind::Utf8String:
        return current.string.View() == next.string.View();
    case ValueKind::SoftObjectReference:
        return current.guid == next.guid && current.string.View() == next.string.View();
    case ValueKind::ObjectReference:
    case ValueKind::WeakObjectReference:
        return current.reference.IsNull() ? next.reference.IsNull()
                                          : current.reference == next.reference;
    case ValueKind::Digest256:
        return current.digest == next.digest;
    case ValueKind::Name:
        return current.name == next.name && current.name.displayId == next.name.displayId;
    default:
        return false;
    }
}

} // namespace

Result<void> FieldWriteOperation::operator()(ObjectRegistry& registry,
                                             const ObjectAccessContext& access, ObjectHandle handle,
                                             const FieldValue& value) const noexcept {
    // 묶인 aggregate 필드(struct·배열·Optional<값 struct>)는 callback이 없다(아래에서 문맥·대상
    // 검사 뒤 Unsupported).
    const bool aggregateField = bound_ && aggregate_;
    if (!callback_ && !aggregateField) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!bound_) {
        // 등록 전 원본 함수표는 기존 저수준 callback 계약을 유지한다.
        return callback_(registry, access, handle, value);
    }

    // 다른 TypeRegistry 또는 같은 native 타입의 별명 스키마도 대상 객체 기준으로 확인한다.
    // callback을 호출할 때는 registry 잠금을 보유하지 않으며 대상 타입 lease는 종료까지 유지한다.
    auto type = registry.AcquireType(access, handle);
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    // 필드를 선언한 타입 또는 그 파생 객체에 쓸 수 있다. 파생 스키마는 상속 필드를 같은 id·kind·
    // flags·참조 타입으로 포함함을 등록 시 강제한다.
    if (!type.Value().IsNativeA(nativeType_)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    const FieldSchema* actual = nullptr;
    usize actualIndex = 0;
    const auto fields = type.Value().Schema().fields;
    for (usize index = 0; index < fields.Size(); ++index) {
        if (fields[index].id == fieldId_) {
            actual = &fields[index];
            actualIndex = index;
            break;
        }
    }

    if (!actual) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // struct·배열 필드는 FieldValue로 통째 쓰지 않는다(멤버·원소 단위 쓰기는 B6 경로 접근 —
    // 3.6·5.6절, 배열 통째 쓰기는 B6 뒤에도 Unsupported). 문맥·수명·대상 타입·필드 검사 뒤의 정책
    // 오류다.
    if (aggregateField) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 검증에 쓴 lease(대상 타입·양쪽 기대 타입·참조 대상)는 callback 완료까지 유지한다. 그 전에
    // 해제하면 callback 재진입에서 같은 ID의 타입을 unregister/재등록해 검증과 실제 필드 게시의
    // 기준이 달라질 수 있다.
    TypeLease sourceExpected;
    TypeLease targetExpected;
    TypeLease referenced;
    detail::FieldWriteTarget target;
    target.actual = actual;
    target.effectiveFlags = actual->flags;
    target.actualEnum = type.Value().FieldEnum(actualIndex);
    // 강한 참조와 weak 참조(리플렉션 B9d1)는 같은 대상 검사를 쓴다(생존 객체·기대 타입의 파생).
    const bool referenceKind =
        valueKind_ == ValueKind::ObjectReference || valueKind_ == ValueKind::WeakObjectReference;
    if (referenceKind && value.kind == valueKind_ && actual->value.kind == valueKind_) {
        // 등록 시점에 자기 참조나 별칭이 아직 미등록일 수 있으므로 호출 시 각 레지스트리에서
        // 참조 타입을 확인한다. 서로 다른 GUID라도 같은 native 타입인 별칭은 허용한다.
        if (referencedType_ != TypeId{}) {
            auto expected = registry_->Acquire(referencedType_);
            if (!expected) {
                return Result<void>::Failure(expected.Error());
            }

            sourceExpected = Move(expected.Value());
        }

        if (actual->value.type != TypeId{}) {
            auto expected = registry.AcquireRegisteredType(access, actual->value.type);
            if (!expected) {
                return Result<void>::Failure(expected.Error());
            }

            targetExpected = Move(expected.Value());
        }

        target.sourceExpectedNative =
            sourceExpected.IsValid() ? sourceExpected.Ops().nativeType : nullptr;
        target.targetExpectedNative =
            targetExpected.IsValid() ? targetExpected.Ops().nativeType : nullptr;
        // 원본·대상 스키마의 기대 타입이 서로 다른 native면 참조 값을 조회하기 전에 거부한다(별칭은
        // 같은 native라 통과한다). ValidateFieldWrite도 같은 검사를 한다.
        if (target.sourceExpectedNative && target.targetExpectedNative &&
            target.sourceExpectedNative != target.targetExpectedNative) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        // 비정규 null(IsNull인 non-zero handle)은 null이다(B5 컨테이너·SetReference와 같은 정규화).
        if (!value.reference.IsNull()) {
            // weak 대상이 다른 Registry의 객체면 WrongContext다(조회는 그것을 회수된 handle과 같은
            // StaleHandle로 알린다 — 리플렉션 B9d1).
            if (valueKind_ == ValueKind::WeakObjectReference &&
                !registry.OwnsHandle(value.reference)) {
                return Result<void>::Failure(ErrorCode::WrongContext);
            }

            auto found = registry.AcquireType(access, value.reference);
            if (!found) {
                return Result<void>::Failure(found.Error());
            }

            referenced = Move(found.Value());
            target.referenced = &referenced;
        }
    }

    detail::FieldWriteBinding binding;
    binding.value = ValueType{valueKind_, referencedType_};
    binding.optional = optional_;
    binding.readOnly = readOnly_;
    if (auto valid = detail::ValidateFieldWrite(binding, target, value); !valid) {
        return valid;
    }

    // Name 토큰은 이 Registry의 pool 것이고 표시 문자열이 내용 규칙을 지켜야 한다(수동 callback도
    // 지키도록 여기서 검사한다 — pool 없음 Unsupported, 다른 pool WrongContext, 게시되지 않은 토큰
    // StaleHandle, 내용 InvalidArgument). 순서는 소프트 참조 규칙처럼 읽기 전용 정책 뒤다.
    // Optional<Name>을 unset으로 되돌리는 쓰기는 토큰을 보지 않는다.
    if (value.kind == ValueKind::Name && value.present) {
        if (auto valid = ValidateNameFieldToken(registry.Names(), value.name, value.GetAllocator());
            !valid) {
            return valid;
        }
    }

    // ClassRef 값은 이 Registry에 등록된 클래스이고 실제 대상 스키마의 Base 파생이어야 한다(수동
    // callback도 지키도록 여기서 검사한다 — 리플렉션 B9d2). 원본 스키마의 Base가 다르면(별칭
    // 스키마) 그것도 만족해야 한다. 순서는 Name 토큰처럼 읽기 전용 정책 뒤다. 판정한 클래스 lease는
    // 참조 lease처럼 callback 완료까지 쥔다(callback 재진입이 그 클래스를 해제·재등록하지 못하게).
    TypeLease classLease;
    if (value.kind == ValueKind::ClassReference && value.guid != TypeId{}) {
        auto found = detail::CheckClassReferenceLease(
            registry.AcquireRegisteredType(access, value.guid), actual->value.type, nullptr);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        // Deprecated 클래스는 새로 쓰지 못하지만 지금 값과 같은 값의 재기록(asset-info → asset-set
        // 왕복·undo·붙여넣기)은 받는다(생성 write는 같은 값이면 아무것도 하지 않는다 — 리플렉션
        // B9h2).
        if (HasFlag(found.Value().Schema().flags, TypeFlags::Deprecated)) {
            const RuntimeFieldOps* ops = type.Value().FieldOps(actualIndex);
            FieldValue current(value.GetAllocator());
            if (!ops || !ops->read || !ops->read(registry, access, handle, current) ||
                current.kind != ValueKind::ClassReference || current.guid != value.guid) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (referencedType_ != TypeId{} && !found.Value().IsA(referencedType_)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        classLease = Move(found.Value());
    }

    // 변경 통지(Docs/Design/ChangeNotification.ko.md CN-D10): Notify 필드의 쓰기가 성공하면 알린다.
    // 강한 참조는 생성 write가 부르는 SetReference가 알린다(handle이 바뀐 때만). 구독자가 있을 때만
    // 쓰기 전 값을 등록된 read로 읽어 같은 값이면 알리지 않는다(CN-D15 — 읽지 못하면 다르다로
    // 본다, Text는 늘 다르다로 보므로 읽지 않는다).
    const bool notify = HasFlag(actual->flags, FieldFlags::Notify) &&
                        actual->value.kind != ValueKind::ObjectReference;
    bool unchanged = false;
    if (notify && value.kind != ValueKind::Text && registry.NotifyWatched(handle)) {
        const RuntimeFieldOps* ops = type.Value().FieldOps(actualIndex);
        FieldValue current(value.GetAllocator());
        unchanged = ops && ops->read && ops->read(registry, access, handle, current) &&
                    SameFieldValue(current, value);
    }

    // 수동 함수표가 비정규 handle을 저장하지 않게 callback에는 정규 null 사본을 넘긴다(참조 값의
    // 문자열은 비어 있으므로 할당하지 않는다).
    Result<void> written = Result<void>::Success();
    if ((value.kind == ValueKind::ObjectReference ||
         value.kind == ValueKind::WeakObjectReference) &&
        value.reference.IsNull() && value.reference != ObjectHandle{}) {
        FieldValue normalized(value.GetAllocator());
        normalized.kind = value.kind;
        normalized.reference = ObjectHandle{};
        written = callback_(registry, access, handle, normalized);
    } else {
        written = callback_(registry, access, handle, value);
    }

    // 쓰기는 이미 성공했다 — 통지의 실패는 쓰기 결과를 바꾸지 않는다(접근·종료는 위에서 보았고,
    // callback이 owner를 파괴 요청했으면 통지는 Success로 아무도 부르지 않는다).
    if (written && notify && !unchanged) {
        (void)registry.NotifyChecked(access, handle, fieldId_, NotifyIndexNone);
    }

    return written;
}

namespace detail {

Result<void> ValidateFieldWrite(const FieldWriteBinding& binding, const FieldWriteTarget& target,
                                const FieldValue& value) noexcept {
    const ValueKind kind = binding.value.kind;
    // 원소 모드는 배열·정적 배열 필드의 원소·Map 필드의 값 형식과 대조한다(리플렉션 B9f3 — Map
    // key 단계 쓰기, B9g — 정적 배열 [i] 쓰기).
    if (!target.actual || (target.element && !ValueKindTakesIndex(target.actual->value.kind) &&
                           target.actual->value.kind != ValueKind::Map)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // Optional 필드(리플렉션 B9e)는 내부 형식과 대조한다. 원본·대상 스키마의 Optional 여부가 다르면
    // 같은 값이라도 다른 필드다.
    const bool optional = !target.element && target.actual->value.kind == ValueKind::Optional;
    if (optional != binding.optional) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    const ValueType& actual =
        target.element || optional ? target.actual->element : target.actual->value;
    if (actual.kind != kind || value.kind != kind) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // enum 값은 필드가 가리키는 enum의 ID를 싣는다(원본·대상 스키마가 같은 enum을 가리켜야 한다).
    // unset으로 되돌리는 Optional 쓰기는 값 칸을 보지 않으므로 값의 ID도 보지 않는다(Optional이
    // 아닌 자리의 present = false는 ID를 먼저 본 뒤 값 내용 검사에서 InvalidArgument — 오류 순서).
    if (kind == ValueKind::Enum &&
        (((!optional || value.present) && value.type != binding.value.type) ||
         actual.type != binding.value.type)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (kind == ValueKind::ObjectReference || kind == ValueKind::WeakObjectReference) {
        // 서로 다른 GUID라도 같은 native 타입인 별칭은 허용한다. weak 참조도 쓰는 순간에는 같은
        // Registry의 생존 객체여야 한다(죽으면 읽을 때 null이 되는 것은 그 뒤의 일이다).
        const void* source = target.sourceExpectedNative;
        const void* expected = target.targetExpectedNative;
        if (source && expected && source != expected) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        // null은 단일 참조·ObjectRefArray 원소 모두 받는다(B5 — 원소 null 허용). 비정규
        // null(IsNull인 non-zero handle)도 null이다(쓰는 쪽이 ObjectHandle{}로 정규화한다).
        if (!value.reference.IsNull()) {
            // 선언 타입 또는 그 파생 타입 객체를 허용한다(SetReference와 같은 상속 규칙).
            if (!target.referenced || (source && !target.referenced->IsNativeA(source)) ||
                (expected && !target.referenced->IsNativeA(expected))) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }
        }
    }

    // struct 값·배열(aggregate — 원소 모드의 struct 원소, Optional<값 struct> 포함)은 FieldValue로
    // 통째 쓰지 않는다 — 종류 대조 뒤의 정책 오류다(멤버·원소 단위 쓰기는 경로 접근이 단일 값으로
    // 내려가 이 함수를 다시 거친다).
    if (optional ? !ValueKindIsWholeFieldValue(kind) : ValueKindIsAggregate(kind)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (binding.readOnly || HasFlag(target.effectiveFlags, FieldFlags::ReadOnly)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // unset은 Optional 필드만의 값이다(값 내용 검사 없이 받는다).
    if (!value.present) {
        return optional ? Result<void>::Success()
                        : Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 값 내용 검사는 읽기 전용 정책 뒤다(정수 폭 Overflow를 callback이 검사하는 것과 같은 순서).
    // 소프트 참조의 null 규칙·경로 상한은 수동 callback도 지키도록 여기서 검사한다. 기대 타입은
    // 대상 에셋을 로드하지 않으므로 쓰기에서 검사하지 않는다.
    if (kind == ValueKind::SoftObjectReference &&
        !SoftObjectRef<>::Validate(AssetId{value.guid}, value.string.View())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 선언되지 않은 enum 값(Flags는 선언 값의 OR가 아닌 값)은 수동 callback에도 넘기지 않는다.
    // 실제 대상 객체 쪽 enum 스키마로 검사한다.
    if (kind == ValueKind::Enum &&
        (!target.actualEnum || !IsDeclaredEnumValue(*target.actualEnum, value.signedInteger))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> ValidateArrayLength(const FieldSchema& field, usize newLength) noexcept {
    if (field.value.kind != ValueKind::Array && field.value.kind != ValueKind::Map &&
        field.value.kind != ValueKind::Set) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (newLength > field.maxElements) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

Result<void> ValidateArrayEdit(const FieldWriteTarget& target, ArrayEdit edit) noexcept {
    if (static_cast<uint8>(edit) > static_cast<uint8>(ArrayEdit::Clear)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const ValueKind kind = target.actual ? target.actual->value.kind : ValueKind::Opaque;
    if (kind != ValueKind::Array && kind != ValueKind::Map && kind != ValueKind::Set) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // 바깥 struct·배열 필드의 ReadOnly가 전파되어 원소 삽입·삭제·비우기를 모두 막는다(Map 항목·Set
    // 원소도 같다 — 리플렉션 B9f3).
    if (HasFlag(target.effectiveFlags, FieldFlags::ReadOnly)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

Result<void> ValidateOptionalEdit(const FieldWriteTarget& target) noexcept {
    if (!target.actual || target.actual->value.kind != ValueKind::Optional ||
        target.actual->element.kind != ValueKind::Struct) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // 바깥 struct·배열 필드의 ReadOnly가 전파되어 Optional<struct>의 값 만들기·비우기를 막는다.
    if (HasFlag(target.effectiveFlags, FieldFlags::ReadOnly)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

Result<void> ValidateInstancedEdit(const FieldWriteTarget& target) noexcept {
    if (!target.actual) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // 원소 모드면 원소·Map 값, 아니면 필드 값이 InstancedStruct여야 한다.
    const ValueType& value = target.element ? target.actual->element : target.actual->value;
    if (value.kind != ValueKind::InstancedStruct) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // 바깥 struct·배열 필드의 ReadOnly가 전파되어 형 바꾸기·비우기를 막는다.
    if (HasFlag(target.effectiveFlags, FieldFlags::ReadOnly)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

Result<void> ValidateWeakTarget(ObjectRegistry& registry, const ObjectAccessContext& access,
                                ObjectHandle target, const void* staticNative) noexcept {
    if (target.IsNull()) {
        return Result<void>::Success();
    }

    if (!registry.OwnsHandle(target)) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    auto type = registry.AcquireType(access, target);
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    if (staticNative && !type.Value().IsNativeA(staticNative)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> ValidateClassReference(ObjectRegistry& registry, const ObjectAccessContext& access,
                                    TypeId value, TypeId base, const void* baseNative) noexcept {
    if (value == TypeId{}) {
        return Result<void>::Success();
    }

    auto checked = CheckWritableClassReferenceLease(registry.AcquireRegisteredType(access, value),
                                                    base, baseNative);
    if (!checked) {
        return Result<void>::Failure(checked.Error());
    }

    return Result<void>::Success();
}

} // namespace detail

} // namespace rocks
