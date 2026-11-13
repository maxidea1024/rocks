#pragma once

#include <Base/Checked.h>
#include <Object/FieldCodec.h>
#include <Object/GeneratedFieldAccess.h>

// 단일 값 종류 필드의 codec이다(Docs/Design/ReflectionFoundations.ko.md 6.4절). 값을 wire로 쓰는
// EncodeFieldWire와 저장값을 해석·검증하는 ParseFieldValue가 한 곳에 있고, 생성 thunk
// (EncodeGeneratedField·DecodeGeneratedField)와 수동 함수표용 범용 codec(EncodeThroughFieldValue·
// DecodeThroughFieldValue)이 같이 쓴다. 생성 헤더와 직렬화기만 include한다.
namespace rocks::detail {

// 값 종류의 wire tag다. 강한 참조는 문맥의 referenceWire다. Struct·Array·Opaque는 Unsupported다.
inline Result<uint32> FieldWireType(ValueKind kind, uint32 referenceWire) noexcept {
    switch (kind) {
    case ValueKind::Bool:
        return Result<uint32>::Success(static_cast<uint32>(WireType::Bool));
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
        return Result<uint32>::Success(static_cast<uint32>(WireType::U32));
    case ValueKind::UInt64:
        return Result<uint32>::Success(static_cast<uint32>(WireType::U64));
    case ValueKind::Int32:
    case ValueKind::Int64:
    case ValueKind::Enum:
        return Result<uint32>::Success(static_cast<uint32>(WireType::I64));
    case ValueKind::Float32:
        return Result<uint32>::Success(static_cast<uint32>(WireType::F32));
    case ValueKind::Float64:
        return Result<uint32>::Success(static_cast<uint32>(WireType::F64));
    case ValueKind::Guid:
    case ValueKind::AssetId:
        return Result<uint32>::Success(static_cast<uint32>(WireType::Guid));
    case ValueKind::Utf8String:
        return Result<uint32>::Success(static_cast<uint32>(WireType::String));
    case ValueKind::Digest256:
        return Result<uint32>::Success(static_cast<uint32>(WireType::Bytes));
    case ValueKind::SoftObjectReference:
        return Result<uint32>::Success(static_cast<uint32>(WireType::SoftObjectReference));
    case ValueKind::ObjectReference:
        return Result<uint32>::Success(referenceWire);
    case ValueKind::Struct:
    case ValueKind::Array:
    case ValueKind::Opaque:
        break;
    }

    return Result<uint32>::Failure(ErrorCode::Unsupported);
}

// 좁은 정수 종류의 상한이다(그 밖은 0 — 검사하지 않는다).
constexpr uint64 UnsignedLimit(ValueKind kind) noexcept {
    return kind == ValueKind::UInt8    ? 0xffull
           : kind == ValueKind::UInt16 ? 0xffffull
           : kind == ValueKind::UInt32 ? 0xffffffffull
                                       : 0;
}

// 지금 위치(context.Field())의 값을 out에 쓰고 wire tag를 돌려준다. value.kind는 스키마의 값 종류와
// 같아야 한다(TypeMismatch). text는 Utf8String의 값·SoftObjectReference의 경로이고(value.string
// 대신 쓴다 — 생성 thunk가 멤버를 복사하지 않게), staticNative는 강한 참조의 정적 대상 타입이다.
// 다시 검증하는 것: 정수 폭(Overflow), enum ID(TypeMismatch)·선언 값, SoftObjectRef null 규칙·경로
// 상한, rejectMissingRequired일 때 Required null(InvalidArgument). non-null 소프트 참조는 쓴 뒤
// 수집한다.
inline Result<uint32> EncodeFieldWire(ArchiveWriter& out, FieldEncodeContext& context,
                                      const FieldValue& value, StringView text,
                                      const void* staticNative) noexcept {
    const FieldSchema& schema = context.Field();
    if (value.kind != schema.value.kind) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    auto wire = FieldWireType(value.kind, context.ReferenceWireType());
    if (!wire) {
        return wire;
    }

    Result<void> written = Result<void>::Success();
    switch (value.kind) {
    case ValueKind::Bool:
        written = out.WriteBool(value.boolean);
        break;
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
        if (value.unsignedInteger > UnsignedLimit(value.kind)) {
            return Result<uint32>::Failure(ErrorCode::Overflow);
        }

        written = out.WriteU32(static_cast<uint32>(value.unsignedInteger));
        break;
    case ValueKind::UInt64:
        written = out.WriteU64(value.unsignedInteger);
        break;
    case ValueKind::Int32:
        if (value.signedInteger < -2147483648ll || value.signedInteger > 2147483647ll) {
            return Result<uint32>::Failure(ErrorCode::Overflow);
        }

        written = out.WriteI64(value.signedInteger);
        break;
    case ValueKind::Int64:
        written = out.WriteI64(value.signedInteger);
        break;
    case ValueKind::Float32:
        written = out.WriteF32(value.float32);
        break;
    case ValueKind::Float64:
        written = out.WriteF64(value.float64);
        break;
    case ValueKind::Guid:
    case ValueKind::AssetId:
        written = out.WriteGuid(value.guid);
        break;
    case ValueKind::Utf8String:
        written = out.WriteString(text);
        break;
    case ValueKind::Digest256:
        written = out.WriteBytes(Span<const uint8>(value.digest.bytes, Sha256Digest::Bytes));
        break;
    case ValueKind::SoftObjectReference: {
        // C++에서 직접 쓴 값은 쓰기 검증을 거치지 않았으므로 저장 때 다시 검사한다.
        const AssetId asset{value.guid};
        if (auto valid = SoftObjectRef<>::Validate(asset, text); !valid) {
            return Result<uint32>::Failure(valid.Error());
        }

        const bool isNull = asset.IsNull() && text.ByteCount() == 0;
        if (auto required = context.CheckRequired(isNull); !required) {
            return Result<uint32>::Failure(required.Error());
        }

        written = out.WriteGuid(value.guid);
        if (written) {
            written = out.WriteString(text);
        }

        if (written && !isNull) {
            written = context.CollectSoftReference(asset, text, schema.value.type);
        }

        break;
    }
    case ValueKind::Enum: {
        const EnumSchema* enumeration = context.FieldEnum();
        if (!enumeration) {
            return Result<uint32>::Failure(ErrorCode::Unsupported);
        }

        if (value.type != schema.value.type) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }

        // 등록된 스키마로 하는 검증은 열거자 수에 비례하므로 그만큼 청구한다.
        if (auto spent = context.Spend(enumeration->values.Size()); !spent) {
            return Result<uint32>::Failure(spent.Error());
        }

        // C++에서 static_cast로 넣은 선언되지 않은 값은 저장하지 않는다.
        if (!IsDeclaredEnumValue(*enumeration, value.signedInteger)) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }

        written = out.WriteI64(value.signedInteger);
        break;
    }
    case ValueKind::ObjectReference:
        written = context.WriteReference(out, value.reference, {schema.value.type, staticNative});
        break;
    case ValueKind::Struct:
    case ValueKind::Array:
    case ValueKind::Opaque:
        return Result<uint32>::Failure(ErrorCode::Unsupported);
    }

    if (!written) {
        return Result<uint32>::Failure(written.Error());
    }

    return wire;
}

// 지금 위치(context.Field())의 저장값 field를 해석·검증한다. 1단계와 2단계가 같은 reader 호출
// 순서를 실행하며(문자열·경로는 view로 읽는다), 검증(wire 종류 TypeMismatch, 정수 폭 Overflow, enum
// 정책, digest 길이·SoftObjectRef 규칙·trailing byte InvalidArgument)이 끝난 뒤 진단·수집을
// 보고하고 2단계 몫(ReserveApply)을 청구한다. apply가 있으면(2단계) 게시할 값을 채운다(문자열
// 복사는 apply의 allocator). staticNative는 강한 참조의 정적 대상 타입이다.
inline Result<void> ParseFieldValue(const FieldView& field, FieldDecodeContext& context,
                                    const void* staticNative, FieldValue* apply) noexcept {
    const FieldSchema& schema = context.Field();
    const ValueKind kind = schema.value.kind;
    auto wire = FieldWireType(kind, context.ReferenceWireType());
    if (!wire) {
        return Result<void>::Failure(wire.Error());
    }

    if (field.wireType != wire.Value()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    auto opened = context.OpenValue(field);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    ArchiveReader& reader = opened.Value();
    bool boolean = false;
    uint64 unsignedInteger = 0;
    int64 signedInteger = 0;
    float float32 = 0;
    double float64 = 0;
    Guid guid{};
    StringView text;
    Sha256Digest digest{};
    DecodedReference reference;
    bool undeclared = false;
    int64 stored = 0;
    switch (kind) {
    case ValueKind::Bool: {
        auto decoded = reader.ReadBool();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        boolean = decoded.Value();
        break;
    }
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32: {
        auto decoded = reader.ReadU32();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        if (decoded.Value() > UnsignedLimit(kind)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        unsignedInteger = decoded.Value();
        break;
    }
    case ValueKind::UInt64: {
        auto decoded = reader.ReadU64();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        unsignedInteger = decoded.Value();
        break;
    }
    case ValueKind::Int32:
    case ValueKind::Int64: {
        auto decoded = reader.ReadI64();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        if (kind == ValueKind::Int32 &&
            (decoded.Value() < -2147483648ll || decoded.Value() > 2147483647ll)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        signedInteger = decoded.Value();
        break;
    }
    case ValueKind::Float32: {
        auto decoded = reader.ReadF32();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        float32 = decoded.Value();
        break;
    }
    case ValueKind::Float64: {
        auto decoded = reader.ReadF64();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        float64 = decoded.Value();
        break;
    }
    case ValueKind::Guid:
    case ValueKind::AssetId: {
        auto decoded = reader.ReadGuid();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        guid = decoded.Value();
        break;
    }
    case ValueKind::Utf8String: {
        auto decoded = reader.ReadStringView();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        text = decoded.Value();
        break;
    }
    case ValueKind::Digest256: {
        auto decoded = reader.ReadBytes();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        if (decoded.Value().Size() != Sha256Digest::Bytes) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
            digest.bytes[index] = decoded.Value()[index];
        }

        break;
    }
    case ValueKind::SoftObjectReference: {
        auto asset = reader.ReadGuid();
        if (!asset) {
            return Result<void>::Failure(asset.Error());
        }

        auto path = reader.ReadStringView();
        if (!path) {
            return Result<void>::Failure(path.Error());
        }

        if (!SoftObjectRef<>::Validate(AssetId{asset.Value()}, path.Value())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        guid = asset.Value();
        text = path.Value();
        break;
    }
    case ValueKind::Enum: {
        const EnumSchema* enumeration = context.FieldEnum();
        if (!enumeration) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        auto decoded = reader.ReadI64();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        // 등록된 스키마로 하는 검증·대체 값 계산은 열거자 수에 비례하므로 그만큼 청구한다(1단계가
        // 2단계의 같은 검사 몫까지 청구한다).
        signedInteger = decoded.Value();
        if (auto spent = context.Spend(enumeration->values.Size()); !spent) {
            return spent;
        }

        if (!IsDeclaredEnumValue(*enumeration, signedInteger)) {
            // 진단 없이 값을 바꾸지 않는다(직렬화기가 hook 없이 ReportAndDefault를 주면 거부).
            if (context.Policy().undeclaredEnum == UndeclaredEnumPolicy::Reject ||
                !context.CanReport()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            if (auto spent = context.Spend(enumeration->values.Size()); !spent) {
                return spent;
            }

            undeclared = true;
            stored = signedInteger;
            signedInteger = EnumReplacementValue(*enumeration, signedInteger);
        }

        break;
    }
    case ValueKind::ObjectReference: {
        auto decoded = context.ReadReference(reader, {schema.value.type, staticNative});
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        reference = decoded.Value();
        break;
    }
    case ValueKind::Struct:
    case ValueKind::Array:
    case ValueKind::Opaque:
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!reader.AtEnd()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 진단·수집은 필드 검증이 끝난 뒤 보고한다(복원 2단계에서는 문맥이 버린다).
    if (undeclared) {
        if (auto reported =
                context.Report(FieldDiagnosticKind::UndeclaredEnumValue, stored, schema.value.type);
            !reported) {
            return reported;
        }
    }

    if (kind == ValueKind::SoftObjectReference) {
        const bool isNull = guid == Guid{} && text.ByteCount() == 0;
        if (auto reported = context.ReportIfRequiredNull(isNull); !reported) {
            return reported;
        }

        if (!isNull) {
            if (auto collected =
                    context.CollectSoftReference(AssetId{guid}, text, schema.value.type);
                !collected) {
                return collected;
            }
        }
    }

    if (kind == ValueKind::ObjectReference) {
        if (auto reported = context.ReportIfRequiredNull(reference.isNull); !reported) {
            return reported;
        }
    }

    // 2단계 몫: staged 쓰기 1 + 문자열(값 복사와 staged 교체본) 2배 + 강한 참조 연결 1.
    usize applyWork = 0;
    if (!TryMultiply(text.ByteCount(), 2, applyWork) ||
        !TryAdd(applyWork, kind == ValueKind::ObjectReference ? 2 : 1, applyWork)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (auto reserved = context.ReserveApply(applyWork); !reserved) {
        return reserved;
    }

    if (!apply) {
        return Result<void>::Success();
    }

    apply->kind = kind;
    apply->boolean = boolean;
    apply->unsignedInteger = unsignedInteger;
    apply->signedInteger = signedInteger;
    apply->float32 = float32;
    apply->float64 = float64;
    apply->guid = guid;
    apply->digest = digest;
    apply->reference = reference.handle;
    apply->type = kind == ValueKind::Enum ? schema.value.type : TypeId{};
    if (kind == ValueKind::Utf8String || kind == ValueKind::SoftObjectReference) {
        return apply->string.TryAssign(text);
    }

    return Result<void>::Success();
}

// 생성 thunk의 멤버 형식이 지금 스키마의 값 형식과 같은지다(별명·수동 스키마와 정적 thunk의
// 어긋남이 조용한 오인코딩이 되지 않게).
template <class Member>
bool MatchesSchema(const FieldSchema& field) noexcept {
    if (field.value.kind != FieldValueKind<Member>()) {
        return false;
    }

    if constexpr (ReflectedEnumField<Member>::Supported) {
        return field.value.type == GeneratedType<Member>::Id();
    } else {
        return true;
    }
}

// 강한 참조 멤버의 정적 대상 타입이다(그 밖은 nullptr).
template <class Member>
const void* StaticReferenceNative() noexcept {
    if constexpr (ObjectReferenceField<Member>::Supported) {
        return NativeTypeKey<typename ObjectReferenceField<Member>::Target>();
    } else {
        return nullptr;
    }
}

// 생성 encode thunk다. owner를 Borrow하고 멤버를 직접 읽어 쓴다(String·경로를 복사하지 않는다 —
// ArchiveWriter가 성장 전에 크기 한도를 검사한다). 쓰기 뒤 owner의 논리 수명을 다시 확인한다.
template <class Owner, class MemberOwner, class Member>
Result<uint32> EncodeGeneratedField(ObjectRegistry& registry, const ObjectAccessContext& access,
                                    ObjectHandle handle, Member MemberOwner::* member,
                                    ArchiveWriter& out, FieldEncodeContext& context) noexcept {
    static_assert(__is_base_of(MemberOwner, Owner));
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    if (!MatchesSchema<Member>(context.Field())) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    auto borrowed = registry.Resolve<Owner>(access, handle);
    if (!borrowed) {
        return Result<uint32>::Failure(borrowed.Error());
    }

    auto target = borrowed.Value().Get();
    if (!target) {
        return Result<uint32>::Failure(target.Error());
    }

    const Member& source = target.Value()->*member;
    FieldValue value(context.Scratch());
    StringView text;
    value.kind = FieldValueKind<Member>();
    if constexpr (IsSameV<Member, String>) {
        text = source.View();
    } else if constexpr (SoftReferenceField<Member>::Supported) {
        value.guid = source.asset.value;
        text = source.objectPath.View();
    } else {
        if (auto copied = CopyFieldValue(source, value); !copied) {
            return Result<uint32>::Failure(copied.Error());
        }
    }

    auto wire = EncodeFieldWire(out, context, value, text, StaticReferenceNative<Member>());
    if (!wire) {
        return wire;
    }

    auto stillValid = borrowed.Value().Get();
    if (!stillValid) {
        return Result<uint32>::Failure(stillValid.Error());
    }

    return wire;
}

// 생성 decode thunk다. 1단계(construction == nullptr)는 해석·검증만, 2단계는 같은 해석 뒤 staged
// 쓰기로 게시한다(검사는 1단계 검증의 부분집합이다).
template <class Owner, class MemberOwner, class Member>
Result<void> DecodeGeneratedField(ConstructionContext* construction, ObjectHandle handle,
                                  Member MemberOwner::* member, const FieldView& field,
                                  FieldDecodeContext& context) noexcept {
    static_assert(__is_base_of(MemberOwner, Owner));
    if (!MatchesSchema<Member>(context.Field())) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (ReflectedEnumField<Member>::Supported) {
        // 1단계는 등록된 enum 스키마로 판정하므로, 정적 enum(staged 쓰기의 기준)으로도 확인해
        // 2단계가 입력 때문에 실패하지 않게 한다(enum 값은 할당 없이 해석된다).
        FieldValue value(context.Scratch());
        auto parsed = ParseFieldValue(field, context, nullptr, &value);
        if (!parsed) {
            return parsed;
        }

        if (!GeneratedType<Member>::IsDeclared(value.signedInteger)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (!construction) {
            return Result<void>::Success();
        }

        return WriteGeneratedStagedField<Owner>(*construction, handle, member, value);
    } else {
        if (!construction) {
            return ParseFieldValue(field, context, StaticReferenceNative<Member>(), nullptr);
        }

        FieldValue value(context.Scratch());
        auto parsed = ParseFieldValue(field, context, StaticReferenceNative<Member>(), &value);
        if (!parsed) {
            return parsed;
        }

        return WriteGeneratedStagedField<Owner>(*construction, handle, member, value);
    }
}

// 수동 함수표·실행 중 정의되는 필드용 범용 encode다(같은 표의 read로 값을 얻는다). 강한 참조의 정적
// 대상 타입을 모르므로 스키마 기대 타입만 검사한다.
//
// 사용 예:
//   RuntimeFieldOps ops{id, read, write, writeStaged, &detail::EncodeThroughFieldValue,
//                       &detail::DecodeThroughFieldValue};
inline Result<uint32> EncodeThroughFieldValue(const RuntimeFieldOps& self, ObjectRegistry& registry,
                                              const ObjectAccessContext& access,
                                              ObjectHandle handle, ArchiveWriter& out,
                                              FieldEncodeContext& context) noexcept {
    if (!self.read) {
        return Result<uint32>::Failure(ErrorCode::Unsupported);
    }

    FieldValue value(context.Scratch());
    if (auto read = self.read(registry, access, handle, value); !read) {
        return Result<uint32>::Failure(read.Error());
    }

    return EncodeFieldWire(out, context, value, value.string.View(), nullptr);
}

// 수동 함수표·실행 중 정의되는 필드용 범용 decode다(2단계는 같은 표의 writeStaged로 게시한다). 정적
// 대상 타입을 모르므로, writeStaged의 검사가 스키마보다 엄격하면(예: 스키마 기대 타입 Object인데
// 멤버가 ObjectRef<Derived>) 2단계에서 실패할 수 있다 — 그런 함수표는 계약 위반이며 rollback된다.
inline Result<void> DecodeThroughFieldValue(const RuntimeFieldOps& self,
                                            ConstructionContext* construction, ObjectHandle handle,
                                            const FieldView& field,
                                            FieldDecodeContext& context) noexcept {
    if (!construction) {
        return ParseFieldValue(field, context, nullptr, nullptr);
    }

    if (!self.writeStaged) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    FieldValue value(context.Scratch());
    if (auto parsed = ParseFieldValue(field, context, nullptr, &value); !parsed) {
        return parsed;
    }

    return self.writeStaged(*construction, handle, value);
}

} // namespace rocks::detail
