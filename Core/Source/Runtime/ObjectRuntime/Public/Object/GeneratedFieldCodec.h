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
        return Result<uint32>::Success(static_cast<uint32>(WireType::Record));
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

    if constexpr (ReflectedEnumField<Member>::Supported ||
                  ReflectedStructField<Member>::Supported) {
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

// 스키마에 없는 저장 필드다: unknownField 정책이 Reject면 InvalidArgument, 아니면 진단(경로는 지금
// 위치 — struct 안이면 감싼 필드까지) 뒤 건너뛴다. 객체 Record와 struct Record가 같이 쓴다.
inline Result<void> SkipUnknownField(FieldDecodeContext& context, Guid fieldId) noexcept {
    if (context.Policy().unknownField == UnknownFieldPolicy::Reject) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return context.ReportUnknownField(fieldId);
}

// 누락된 struct 값 안의 Required 후손(중첩 포함)을 지금 위치 아래 경로로 진단한다. Required 후손이
// 없는 가지는 건너뛰고(등록이 계산한 ContainsRequired) 방문마다 작업량 1을 청구한다.
inline Result<void> ReportMissingRequired(StructView view, FieldDecodeContext& context) noexcept {
    const TypeSchema& schema = view.Schema();
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        if (auto spent = context.Spend(1); !spent) {
            return spent;
        }

        const FieldSchema& member = schema.fields[index];
        const StructView nested = view.FieldStruct(index);
        const bool required = HasFlag(member.flags, FieldFlags::Required);
        const bool descend = !nested.IsEmpty() && nested.ContainsRequired();
        if (!required && !descend) {
            continue;
        }

        FieldScope scope(context, member, schema.id, view.FieldEnum(index), nested);
        // Transient 멤버(유효 플래그 — 바깥에서 전파된 것 포함)는 저장·복원하지 않으므로 누락도
        // 아니다.
        if (HasFlag(context.EffectiveFlags(), FieldFlags::Transient)) {
            continue;
        }

        if (required) {
            if (auto reported = context.Report(FieldDiagnosticKind::RequiredMissing); !reported) {
                return reported;
            }
        }

        if (descend) {
            if (auto reported = ReportMissingRequired(nested, context); !reported) {
                return reported;
            }
        }
    }

    return Result<void>::Success();
}

// 지금 위치(context.Field())가 저장 데이터에 없다: 누락이 허용되지 않으면 NotFound, 허용되면
// Required 진단(struct 필드면 그 안의 Required 후손까지 — 2단계는 생략). 객체·struct가 같이 쓴다.
inline Result<void> CheckMissingField(FieldDecodeContext& context) noexcept {
    if (!context.IsMissingAllowed(context.EffectiveFlags())) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (auto reported = context.ReportIfRequiredNull(true); !reported) {
        return reported;
    }

    // 진단을 받을 곳이 있을 때만 Required 후손을 돈다(받을 곳이 없으면 작업만 쓴다).
    const FieldCodecFrame* frame = context.Current();
    if (context.IsRecording() && context.CanReport() && frame && !frame->structure.IsEmpty() &&
        frame->structure.ContainsRequired()) {
        return ReportMissingRequired(frame->structure, context);
    }

    return Result<void>::Success();
}

// struct 값(value)을 struct ID·버전 헤더의 중첩 Record로 out에 제자리로 쓴다(Docs/Design/
// ReflectionFoundations.ko.md 4.6절 — RecordWriter, 값 byte를 한 번만 쓰므로 중첩 깊이만큼 복사가
// 늘지 않는다). 참조를 담지 않는 struct는 먼저 SubstituteStruct hook을 거친다. 멤버는 스키마 순서로
// FieldScope 안에서 멤버 thunk가 out에 바로 쓰고, 유효 플래그가 제외(IsExcluded)인 멤버는 쓰지
// 않는다. 쓴 TLV 수를 AddFields로 그래프 누계에 더한다. 작업량은 out의 청구로 센다(호출자가 out의
// WorkUsed를 청구한다). 실패하면 out의 내용은 정해지지 않는다.
inline Result<uint32> EncodeStructRecord(StructView view, const void* value, ArchiveWriter& out,
                                         FieldEncodeContext& context) noexcept {
    if (view.IsEmpty() || !value) {
        return Result<uint32>::Failure(ErrorCode::Unsupported);
    }

    const TypeSchema& schema = view.Schema();
    if (!view.ContainsStrongReference()) {
        auto substituted = context.SubstituteStruct(schema.id, view.StructOps().nativeType, value);
        if (!substituted) {
            return Result<uint32>::Failure(substituted.Error());
        }

        value = substituted.Value();
    }

    auto record = RecordWriter::Begin(out, schema.id, schema.version, context.NestedRecordLimits());
    if (!record) {
        return Result<uint32>::Failure(record.Error());
    }

    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& member = schema.fields[index];
        FieldScope scope(context, member, schema.id, view.FieldEnum(index),
                         view.FieldStruct(index));
        if (context.IsExcluded(context.EffectiveFlags())) {
            continue;
        }

        const RuntimeStructFieldOps& ops = view.FieldOps(index);
        if (!ops.encode) {
            return Result<uint32>::Failure(ErrorCode::Unsupported);
        }

        if (auto begun = record.Value().BeginField(member.id); !begun) {
            return Result<uint32>::Failure(begun.Error());
        }

        auto wire = ops.encode(ops, value, out, context);
        if (!wire) {
            return wire;
        }

        // thunk가 스키마 값 종류의 wire를 썼는지 다시 확인한다.
        auto expected = FieldWireType(member.value.kind, context.ReferenceWireType());
        if (!expected || expected.Value() != wire.Value()) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }

        if (auto ended = record.Value().EndField(wire.Value()); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }
    }

    if (auto ended = record.Value().End(); !ended) {
        return Result<uint32>::Failure(ended.Error());
    }

    if (auto counted = context.AddFields(record.Value().FieldCount()); !counted) {
        return Result<uint32>::Failure(counted.Error());
    }

    return Result<uint32>::Success(static_cast<uint32>(WireType::Record));
}

// 맨 위 Record(context.CurrentRecord())를 struct 값 하나로 해석한다. 저장 필드마다 스키마 위치를
// 찾아(정렬 색인) 멤버 thunk를 부르고, 모르는 필드·저장된 Transient·누락은 객체 Record와 같은
// 규칙이다. construction·value는 멤버 thunk의 세 방식(검증·관찰·적용)을 정한다.
inline Result<void> DecodeStructRecord(StructView view, ConstructionContext* construction,
                                       ObjectHandle owner, void* value,
                                       FieldDecodeContext& context) noexcept {
    RecordReader* record = context.CurrentRecord();
    if (view.IsEmpty() || !record) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TypeSchema& schema = view.Schema();
    uint64 searchWork = 1;
    for (usize count = schema.fields.Size(); count != 0; count >>= 1) {
        ++searchWork;
    }

    uint64 indexWork = 0;
    if (!TryMultiply(uint64{record->FieldCount()}, searchWork, indexWork) ||
        !TryAdd(indexWork, uint64{schema.fields.Size()}, indexWork)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (auto spent = context.Spend(indexWork); !spent) {
        return spent;
    }

    // 스키마 필드는 4096개 이하다(등록 규칙).
    uint64 present[4096 / 64] = {};
    for (usize stored = 0; stored < record->FieldCount(); ++stored) {
        auto field = record->Next();
        if (!field) {
            return Result<void>::Failure(field.Error());
        }

        const usize index = view.FieldIndex(field.Value().id);
        if (index == StructView::NoField) {
            if (auto skipped = SkipUnknownField(context, field.Value().id); !skipped) {
                return skipped;
            }

            continue;
        }

        const FieldSchema& member = schema.fields[index];
        if (HasFlag(member.flags, FieldFlags::Transient)) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        FieldScope scope(context, member, schema.id, view.FieldEnum(index),
                         view.FieldStruct(index));
        const RuntimeStructFieldOps& ops = view.FieldOps(index);
        if (!ops.decode) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (auto decoded = ops.decode(ops, construction, owner, value, field.Value(), context);
            !decoded) {
            return decoded;
        }

        present[index / 64] |= uint64{1} << (index % 64);
    }

    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& member = schema.fields[index];
        if ((present[index / 64] & (uint64{1} << (index % 64))) != 0 ||
            HasFlag(member.flags, FieldFlags::Transient)) {
            continue;
        }

        FieldScope scope(context, member, schema.id, view.FieldEnum(index),
                         view.FieldStruct(index));
        if (auto missing = CheckMissingField(context); !missing) {
            return missing;
        }
    }

    return Result<void>::Success();
}

// struct 필드 하나(중첩 Record)를 해석한다: Record 스택에 열고 버전을 판정(같으면 그대로, 낮으면
// MigrateStruct 결과를 현재 버전으로 다시 열고, 높으면 newerVersion 정책)한 뒤 DecodeStructRecord.
// 1단계에서 관찰 대상 struct면 검증과 같은 한 번의 해석으로 값을 채워(관찰 중인 조상 아래면 그
// 자리, 아니면 scratch 임시 값) ObserveStruct를 부른다.
inline Result<void> DecodeStructField(StructView view, ConstructionContext* construction,
                                      ObjectHandle owner, void* value, const FieldView& field,
                                      FieldDecodeContext& context) noexcept {
    if (view.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const TypeSchema& schema = view.Schema();
    const RuntimeStructOps& structOps = view.StructOps();
    // 관찰용 임시 값의 수명이다(모든 경로에서 소멸·반환).
    struct Temporary {
        const RuntimeStructOps* ops = nullptr;
        Allocator* allocator = nullptr;
        void* memory = nullptr;

        ~Temporary() {
            if (memory) {
                ops->destroy(memory);
                if (!allocator->Deallocate(memory)) {
                    platform::Fatal("Struct observation scratch ownership mismatch");
                }
            }
        }
    } temporary;
    const bool observe = construction == nullptr && context.IsObservedStruct(schema.id);
    if (observe && value == nullptr) {
        auto memory = context.Scratch().Allocate(structOps.size, structOps.alignment, 0x4f425356u);
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        structOps.construct(memory.Value(), context.Scratch());
        temporary.ops = &structOps;
        temporary.allocator = &context.Scratch();
        temporary.memory = memory.Value();
        value = memory.Value();
    }

    if (auto spent = context.Spend(1); !spent) {
        return spent;
    }

    auto scope = context.OpenRecord(field, schema.id, 1, ~0u);
    if (!scope) {
        return Result<void>::Failure(scope.Error());
    }

    const uint32 stored = context.CurrentRecord()->SchemaVersion();
    Result<void> decoded = Result<void>::Success();
    if (stored < schema.version) {
        // 원본 Record 바이트로 올리고, 원본 scope를 닫은 뒤 결과를 현재 버전으로만 연다.
        auto migrated = context.MigrateStruct(schema.id, stored, schema.version, field.payload);
        if (!migrated) {
            return Result<void>::Failure(migrated.Error());
        }

        scope.Value().Close();
        auto reopened = context.OpenMigratedRecord(migrated.Value(), schema.id, schema.version);
        if (!reopened) {
            return Result<void>::Failure(reopened.Error());
        }

        decoded = DecodeStructRecord(view, construction, owner, value, context);
    } else {
        if (stored > schema.version) {
            if (context.Policy().newerVersion != NewerVersionPolicy::LoadNewerSkippingUnknown) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            // 진단 없이 모르는 멤버를 건너뛰지 않는다(직렬화기가 hook 없이 이 정책을 주면 거부).
            if (!context.CanReport()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            if (auto reported = context.Report(FieldDiagnosticKind::NewerVersion, stored);
                !reported) {
                return reported;
            }
        }

        decoded = DecodeStructRecord(view, construction, owner, value, context);
    }

    if (!decoded) {
        return decoded;
    }

    if (observe) {
        return context.ObserveStruct(schema.id, structOps.nativeType, value);
    }

    return Result<void>::Success();
}

// 지금 위치의 등록 struct가 정적 thunk의 C++ struct와 같은지다(ID 대조에 더해 native 키로 — 수동
// 등록의 어긋남이 타입 혼동이 되지 않게).
template <class Member>
bool MatchesStructView(const FieldCodecContext& context) noexcept {
    const FieldCodecFrame* frame = context.Current();
    return frame && !frame->structure.IsEmpty() &&
           frame->structure.StructOps().nativeType == NativeTypeKey<Member>();
}

// struct 멤버 포인터의 소유 struct·멤버 형식이다.
template <class T>
struct StructMemberPointer;

template <class S, class M>
struct StructMemberPointer<M S::*> {
    using Owner = S;
    using Member = M;
};

// 참조 멤버의 적용(2단계) 쓰기다. 지금은 소유 객체 저장소 안 주소의 SetReference이며, B5의 참조
// struct 컨테이너가 이 한 곳을 컨테이너 기준 검사로 넓힌다(4.3절).
template <class T>
Result<void> SetStructMemberReference(ConstructionContext& construction, ObjectHandle owner,
                                      ObjectRef<T>& field, ObjectHandle target) noexcept {
    return construction.SetReference(owner, field, target);
}

// 생성(또는 수동) struct 멤버 encode thunk다. 멤버를 복사하지 않고 직접 쓴다.
template <auto MemberPointer>
Result<uint32> EncodeStructMember(const RuntimeStructFieldOps&, const void* structValue,
                                  ArchiveWriter& out, FieldEncodeContext& context) noexcept {
    using Traits = StructMemberPointer<decltype(MemberPointer)>;
    using S = typename Traits::Owner;
    using Member = typename Traits::Member;
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    if (!MatchesSchema<Member>(context.Field())) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    const Member& source = static_cast<const S*>(structValue)->*MemberPointer;
    if constexpr (ReflectedStructField<Member>::Supported) {
        if (!MatchesStructView<Member>(context)) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }

        return EncodeStructRecord(context.Current()->structure, &source, out, context);
    } else {
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

        return EncodeFieldWire(out, context, value, text, StaticReferenceNative<Member>());
    }
}

// 생성(또는 수동) struct 멤버 decode thunk다. 검증 방식(structValue null)은 값에 손대지 않고, 관찰·
// 적용 방식은 struct 값에 직접 쓴다(참조 멤버의 적용은 SetStructMemberReference).
template <auto MemberPointer>
Result<void> DecodeStructMember(const RuntimeStructFieldOps&, ConstructionContext* construction,
                                ObjectHandle owner, void* structValue, const FieldView& field,
                                FieldDecodeContext& context) noexcept {
    using Traits = StructMemberPointer<decltype(MemberPointer)>;
    using S = typename Traits::Owner;
    using Member = typename Traits::Member;
    static_assert(FieldValueKind<Member>() != ValueKind::Opaque);
    if (!MatchesSchema<Member>(context.Field())) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    S* target = static_cast<S*>(structValue);
    if constexpr (ReflectedStructField<Member>::Supported) {
        if (!MatchesStructView<Member>(context)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        return DecodeStructField(context.Current()->structure, construction, owner,
                                 target ? &(target->*MemberPointer) : nullptr, field, context);
    } else if constexpr (ObjectReferenceField<Member>::Supported) {
        // 관찰 대상 struct는 참조를 담지 않으므로 construction이 없으면 검증만 한다.
        if (!construction || !target) {
            return ParseFieldValue(field, context, StaticReferenceNative<Member>(), nullptr);
        }

        FieldValue value(context.Scratch());
        if (auto parsed = ParseFieldValue(field, context, StaticReferenceNative<Member>(), &value);
            !parsed) {
            return parsed;
        }

        return SetStructMemberReference(*construction, owner, target->*MemberPointer,
                                        value.reference);
    } else {
        // 검증 방식은 값을 채우지 않는다(문자열을 scratch에 복사하지 않는다). enum은 정적 enum
        // 확인에 값이 필요하다.
        FieldValue value(context.Scratch());
        FieldValue* apply = target || ReflectedEnumField<Member>::Supported ? &value : nullptr;
        if (auto parsed = ParseFieldValue(field, context, nullptr, apply); !parsed) {
            return parsed;
        }

        // 1단계는 등록 enum 스키마로 판정하므로 정적 enum(직접 쓰기의 기준)으로도 확인한다.
        if constexpr (ReflectedEnumField<Member>::Supported) {
            if (!GeneratedType<Member>::IsDeclared(value.signedInteger)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (!target) {
            return Result<void>::Success();
        }

        return WriteGeneratedValue<S>(MemberPointer, value,
                                      [target]() noexcept { return Result<S*>::Success(target); });
    }
}

// struct 멤버 하나의 함수표다(생성 코드·수동 시험 공용).
//
// 사용 예:
//   const RuntimeStructFieldOps fields[] = {detail::StructFieldOps<&BulkDataRef::bulkId>(BulkId)};
template <auto MemberPointer>
RuntimeStructFieldOps StructFieldOps(Guid fieldId) noexcept {
    using Member = typename StructMemberPointer<decltype(MemberPointer)>::Member;
    RuntimeStructFieldOps ops;
    ops.fieldId = fieldId;
    ops.encode = &EncodeStructMember<MemberPointer>;
    ops.decode = &DecodeStructMember<MemberPointer>;
    if constexpr (ReflectedEnumField<Member>::Supported ||
                  ReflectedStructField<Member>::Supported) {
        ops.valueNativeType = NativeTypeKey<Member>();
    }

    return ops;
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
    Result<uint32> wire = Result<uint32>::Failure(ErrorCode::Unsupported);
    if constexpr (ReflectedStructField<Member>::Supported) {
        // struct 필드는 등록 struct(지금 위치의 structure)로 중첩 Record를 쓴다.
        if (!MatchesStructView<Member>(context)) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }

        wire = EncodeStructRecord(context.Current()->structure, &source, out, context);
    } else {
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

        wire = EncodeFieldWire(out, context, value, text, StaticReferenceNative<Member>());
    }

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

    if constexpr (ReflectedStructField<Member>::Supported) {
        // struct 필드: 1단계는 값 없이 검증(관찰은 scratch), 2단계는 새 객체 안의 멤버 주소에 쓴다.
        if (!MatchesStructView<Member>(context)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (!construction) {
            return DecodeStructField(context.Current()->structure, nullptr, handle, nullptr, field,
                                     context);
        }

        auto owner = construction->Get<Owner>(handle);
        if (!owner) {
            return Result<void>::Failure(owner.Error());
        }

        return DecodeStructField(context.Current()->structure, construction, handle,
                                 &(owner.Value()->*member), field, context);
    } else if constexpr (ReflectedEnumField<Member>::Supported) {
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
