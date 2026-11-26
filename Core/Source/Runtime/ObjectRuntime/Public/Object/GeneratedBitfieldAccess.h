#pragma once

#include <Object/GeneratedFieldCodec.h>
#include <Object/GeneratedPathAccess.h>

// bitfield bool PROPERTY(리플렉션 B9m — Docs/Design/ReflectionExtensions.ko.md 14절)의 생성 함수표
// 템플릿이다. bitfield는 멤버 포인터·참조를 만들 수 없으므로 멤버 포인터 기반 thunk 대신
// MetaCompiler가 GeneratedType<Owner>에 내는 정적 접근자 Get_<name>(const Owner&)·
// Set_<name>(Owner&, bool)을 템플릿 인자로 받는다. 값 종류는 Bool, wire는 Bool이다. 클래스 필드는
// BitfieldFieldOps(read·write·writeStaged·encode·decode·경로), struct 멤버는
// BitfieldStructFieldOps(encode·decode·경로)다. 비트 배치는 컴파일러가 처리하고 offset·mask는
// 공개하지 않는다. 생성 헤더만 include한다.
namespace rocks::detail {

// 스키마가 이 thunk의 값 형식(Bool 단일 값)과 같은지다.
inline bool MatchesBitfieldSchema(const FieldSchema& field) noexcept {
    return field.value.kind == ValueKind::Bool && field.value.type == TypeId{} &&
           field.element.kind == ValueKind::Opaque && field.maxElements == 0;
}

// Owner 값의 bitfield를 지금 위치의 wire Bool로 쓴다.
template <class Owner, auto Get>
Result<uint32> EncodeBitfieldValue(const Owner& owner, ArchiveWriter& out,
                                   FieldEncodeContext& context) noexcept {
    if (!MatchesBitfieldSchema(context.Field())) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    FieldValue value(context.Scratch());
    value.kind = ValueKind::Bool;
    value.boolean = Get(owner);
    return EncodeFieldWire(out, context, value, StringView{}, nullptr);
}

// 지금 위치의 wire Bool을 해석한다(apply가 있으면 값을 채운다).
inline Result<void> ParseBitfieldValue(const FieldView& field, FieldDecodeContext& context,
                                       FieldValue* apply) noexcept {
    if (!MatchesBitfieldSchema(context.Field())) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    return ParseFieldValue(field, context, nullptr, apply);
}

// bitfield 하나의 무할당 교환 연산이다(Attach 게시 — PreparedAttach가 자리 location과 상자 값을
// swap으로 맞바꾼다). 자리는 bitfield를 가진 Owner 값의 주소이고 상자는 bool 값이다(크기·native
// 키는 bool과 같아 bool 상자와 대조된다).
template <class Owner, auto Get, auto Set>
const RuntimeValueOps& BitfieldValueOps() noexcept {
    static const RuntimeValueOps ops = []() noexcept {
        RuntimeValueOps made = ValueOpsOf<bool>();
        made.swap = +[](void* location, void* box) noexcept {
            Owner& owner = *static_cast<Owner*>(location);
            bool& held = *static_cast<bool*>(box);
            const bool current = Get(owner);
            Set(owner, held);
            held = current;
        };
        return made;
    }();
    return ops;
}

// Owner 값 안 bitfield의 경로 단계다: 읽기·쓰기는 bool 사본으로 단일 값 끝 지점(PathValue)을 거친
// 뒤 바뀌었으면 Set으로 되쓰고, 끝 단계의 Detach는 bool 사본을 상자로 복사하며 Attach는 Owner 값
// 주소를 자리로 기록한다(게시 때 BitfieldValueOps의 swap이 접근자로 맞바꾼다).
template <class Owner, auto Get, auto Set>
Result<void> PathBitfield(PathCursor& cursor, Owner& owner) noexcept {
    if (auto checked = cursor.CheckTarget(ValueKind::Bool, ValueKind::Opaque); !checked) {
        return checked;
    }

    const PathOperation operation = cursor.Operation();
    if ((operation == PathOperation::Detach || operation == PathOperation::Attach) &&
        cursor.IsLast()) {
        if (operation == PathOperation::Detach) {
            const bool value = Get(owner);
            return cursor.DetachValue(ValueOpsOf<bool>(), &value,
                                      &PathEnterDetached<ValueKind::Bool, bool>);
        }

        return cursor.AttachValue(BitfieldValueOps<Owner, Get, Set>(), &owner);
    }

    bool value = Get(owner);
    const bool before = value;
    if (auto done = PathValue<ValueKind::Bool>(cursor, value); !done) {
        return done;
    }

    if (value != before) {
        Set(owner, value);
    }

    return Result<void>::Success();
}

template <class Owner, auto Get, auto Set>
Result<void> ReadBitfieldField(ObjectRegistry& registry, const ObjectAccessContext& access,
                               ObjectHandle handle, FieldValue& out) noexcept {
    auto borrowed = registry.Resolve<Owner>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto target = borrowed.Value().Get();
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    out.kind = ValueKind::Bool;
    out.present = true;
    out.boolean = Get(*target.Value());
    return Result<void>::Success();
}

// 살아 있는 객체 쓰기다(쓰기용 Borrow — owner가 encode 고정 중이면 Busy). ReadOnly면 owner를 확인한
// 뒤 Unsupported다.
template <class Owner, auto Get, auto Set, bool ReadOnly>
Result<void> WriteBitfieldField(ObjectRegistry& registry, const ObjectAccessContext& access,
                                ObjectHandle handle, const FieldValue& value) noexcept {
    if (value.kind != ValueKind::Bool) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (!value.present) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if constexpr (ReadOnly) {
        auto borrowed = registry.Resolve<Owner>(access, handle);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto valid = borrowed.Value().Get();
        if (!valid) {
            return Result<void>::Failure(valid.Error());
        }

        return Result<void>::Failure(ErrorCode::Unsupported);
    } else {
        auto borrowed = registry.ResolveForWrite<Owner>(access, handle);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto target = borrowed.Value().Get();
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        Set(*target.Value(), value.boolean);
        return Result<void>::Success();
    }
}

template <class Owner, auto Get, auto Set>
Result<void> WriteBitfieldStagedField(ConstructionContext& construction, ObjectHandle handle,
                                      const FieldValue& value) noexcept {
    if (value.kind != ValueKind::Bool) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (!value.present) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto target = construction.GetForWrite<Owner>(handle);
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    Set(*target.Value(), value.boolean);
    return Result<void>::Success();
}

template <class Owner, auto Get, auto Set>
Result<uint32> EncodeBitfieldField(const RuntimeFieldOps&, ObjectRegistry* registry,
                                   const ObjectAccessContext& access, ObjectHandle handle,
                                   ArchiveWriter& out, FieldEncodeContext& context) noexcept {
    // 원본 모드(클래스 기본값 레코드)는 그 객체에서 바로 읽는다.
    if (const Object* raw = context.RawSource()) {
        return EncodeBitfieldValue<Owner, Get>(*static_cast<const Owner*>(raw), out, context);
    }

    if (ConstructionContext* staged = context.StagedSource()) {
        auto owner = staged->Get<Owner>(handle);
        if (!owner) {
            return Result<uint32>::Failure(owner.Error());
        }

        return EncodeBitfieldValue<Owner, Get>(*owner.Value(), out, context);
    }

    if (registry == nullptr) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    auto borrowed = registry->Resolve<Owner>(access, handle);
    if (!borrowed) {
        return Result<uint32>::Failure(borrowed.Error());
    }

    auto target = borrowed.Value().Get();
    if (!target) {
        return Result<uint32>::Failure(target.Error());
    }

    return EncodeBitfieldValue<Owner, Get>(*target.Value(), out, context);
}

// 1단계(construction 없음)는 해석·검증만, 2단계는 같은 해석 뒤 staged 객체에 쓴다.
template <class Owner, auto Get, auto Set>
Result<void> DecodeBitfieldField(const RuntimeFieldOps&, ConstructionContext* construction,
                                 ObjectHandle handle, const FieldView& field,
                                 FieldDecodeContext& context) noexcept {
    if (!construction) {
        return ParseBitfieldValue(field, context, nullptr);
    }

    FieldValue value(context.Scratch());
    if (auto parsed = ParseBitfieldValue(field, context, &value); !parsed) {
        return parsed;
    }

    return WriteBitfieldStagedField<Owner, Get, Set>(*construction, handle, value);
}

template <class Owner, auto Get, auto Set>
Result<void> PathBitfieldClassField(const RuntimeFieldOps&, PathCursor& cursor) noexcept {
    static_assert(IsBaseOfV<Object, Owner>);
    auto owner = cursor.Owner(NativeTypeKey<Owner>());
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    return PathBitfield<Owner, Get, Set>(cursor, *static_cast<Owner*>(owner.Value()));
}

// 클래스 bitfield bool 필드 하나의 함수표다(생성 코드 — 리플렉션 B9m). Owner는 함수표를 내는
// 클래스(파생이면 base의 접근자를 그대로 받는다 — 접근자 인자가 base 참조라 파생 값이 그대로
// 넘어간다), Get·Set은 GeneratedType<선언 클래스>::Get_<name>·Set_<name>이다. ReadOnly 필드의
// write는 owner 확인 뒤 Unsupported다.
//
// 생성 코드:
//   detail::BitfieldFieldOps<::Settings, &GeneratedType<::Settings>::Get_visible,
//                            &GeneratedType<::Settings>::Set_visible, false>(VisibleId),
template <class Owner, auto Get, auto Set, bool ReadOnly>
RuntimeFieldOps BitfieldFieldOps(Guid fieldId) noexcept {
    RuntimeFieldOps ops;
    ops.fieldId = fieldId;
    ops.read = &ReadBitfieldField<Owner, Get, Set>;
    ops.write = &WriteBitfieldField<Owner, Get, Set, ReadOnly>;
    ops.writeStaged = &WriteBitfieldStagedField<Owner, Get, Set>;
    ops.encode = &EncodeBitfieldField<Owner, Get, Set>;
    ops.decode = &DecodeBitfieldField<Owner, Get, Set>;
    ops.path = &PathBitfieldClassField<Owner, Get, Set>;
    return ops;
}

template <class Owner, auto Get, auto Set>
Result<uint32> EncodeBitfieldMember(const RuntimeStructFieldOps&, const void* structValue,
                                    ArchiveWriter& out, FieldEncodeContext& context) noexcept {
    return EncodeBitfieldValue<Owner, Get>(*static_cast<const Owner*>(structValue), out, context);
}

// struct 멤버 decode다(검증 방식은 structValue null, 관찰·적용 방식은 struct 값에 쓴다 — 관찰
// 사본은 1단계 몫이라 ObservationScratch).
template <class Owner, auto Get, auto Set>
Result<void> DecodeBitfieldMember(const RuntimeStructFieldOps&, ConstructionContext* construction,
                                  ObjectHandle, void* structValue, const FieldView& field,
                                  FieldDecodeContext& context) noexcept {
    if (!structValue) {
        return ParseBitfieldValue(field, context, nullptr);
    }

    FieldValue value(construction ? context.Scratch() : context.ObservationScratch());
    if (auto parsed = ParseBitfieldValue(field, context, &value); !parsed) {
        return parsed;
    }

    Set(*static_cast<Owner*>(structValue), value.boolean);
    return Result<void>::Success();
}

template <class Owner, auto Get, auto Set>
Result<void> PathBitfieldStructMember(const RuntimeStructFieldOps&, PathCursor& cursor,
                                      void* structValue) noexcept {
    if (auto checked = cursor.CheckMemberOwner(NativeTypeKey<Owner>()); !checked) {
        return checked;
    }

    return PathBitfield<Owner, Get, Set>(cursor, *static_cast<Owner*>(structValue));
}

// struct bitfield bool 멤버 하나의 함수표다(생성 코드 — 리플렉션 B9m, Owner는 함수표를 내는
// struct).
//
// 생성 코드:
//   detail::BitfieldStructFieldOps<::Flags, &GeneratedType<::Flags>::Get_on,
//                                  &GeneratedType<::Flags>::Set_on>(OnId),
template <class Owner, auto Get, auto Set>
RuntimeStructFieldOps BitfieldStructFieldOps(Guid fieldId) noexcept {
    RuntimeStructFieldOps ops;
    ops.fieldId = fieldId;
    ops.encode = &EncodeBitfieldMember<Owner, Get, Set>;
    ops.decode = &DecodeBitfieldMember<Owner, Get, Set>;
    ops.path = &PathBitfieldStructMember<Owner, Get, Set>;
    return ops;
}

} // namespace rocks::detail
