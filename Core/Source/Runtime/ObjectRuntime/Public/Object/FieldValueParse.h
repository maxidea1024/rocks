#pragma once

#include <Object/FieldValueWire.h>

// 필드 codec 사슬의 둘째 조각으로 단일 값의 wire 읽기 쪽과 codec 공용 판정이다(Docs/Design/
// ReflectionFoundations.ko.md 5.6·6.4절). 읽은 값(ParsedValue), 값 읽기(ReadValueWire)·미선언
// enum 판정(ResolveUndeclaredEnum)·보고(ReportParsedValue)·Name 2단계(ResolveParsedName), 단일 값
// 하나의 해석(ParseValuePayload — 단일 값 필드·Optional 내부 공용)과 단일 값 필드 읽기
// (ParseFieldValue — Optional<단일 값> 포함), 멤버 형식 대조(MatchesSchema), 정적 참조 형식
// (StaticReferenceNative), 모르는 필드 건너뛰기(SkipUnknownField), Required 위치 판정
// (CarriesRequired)이 있다. Object/GeneratedFieldCodec.h로 받는다.
namespace rocks::detail {

// ReadValueWire가 읽은 값 하나다. 할당하지 않는 view이며(text는 입력을 빌린다) kind에 맞는 칸만
// 뜻이 있다. undeclared는 선언되지 않은 enum 값을 읽었다는 표시(stored = 저장 값)이고, 정책
// 판정·대체 값은 ResolveUndeclaredEnum이 한다. Name은 text가 표시 문자열이고 name은 2단계의
// 토큰이다(ResolveParsedName — 1단계는 기본 None 그대로). ClassRef는 guid가 판정 뒤 값이고,
// unknownClass는 미등록 클래스를 decode 정책 ReportAndNull로 null로 바꿨다는 표시(storedClass =
// 저장 값 — 진단은 ReportParsedValue)다.
struct ParsedValue {
    bool boolean = false;
    uint64 unsignedInteger = 0;
    int64 signedInteger = 0;
    float float32 = 0;
    double float64 = 0;
    Guid guid{};
    StringView text;
    Name name{};
    Sha256Digest digest{};
    DecodedReference reference;
    bool undeclared = false;
    int64 stored = 0;
    bool unknownClass = false;
    TypeId storedClass{};
    // ClassRef 값의 클래스가 Deprecated다(리플렉션 B9h2 — 값은 그대로 읽고 ReportParsedValue가
    // DeprecatedType 진단을 낸다).
    bool deprecatedClass = false;
};

// 읽은 ClassRef 값 하나(out.guid — 0이면 null)를 판정한다(리플렉션 B9d2 — Docs/Design/
// ReflectionExtensions.ko.md 5.2절 표의 "decode 1단계"). base는 스키마의 Base 클래스 ID(type.type),
// staticNative는 정적 Base의 NativeTypeKey(nullptr이면 보지 않는다)다.
// - 기록하는 단계(복원 1단계): 조회 1을 Spend한 뒤 문맥의 AcquireClassReference(TypeRegistry 조회 —
//   ObjectRegistry 접근 아님)로 등록·클래스·Base 파생을 본다. 미등록(NotFound)이면 decode 정책
//   unknownClass가 Reject이거나 진단 받을 곳이 없을 때(CanReport) NotFound, ReportAndNull이면
//   out.unknownClass·storedClass를 남기고 null로 바꾼다(진단은 ReportParsedValue). 클래스가 아닌
//   레코드·Base 파생 위반·정적 Base 위반은 정책과 무관하게 InvalidArgument, 그 밖의 조회 오류는 그
//   오류다. 받은 값과 null로 바꾼 값은 CollectClassReference로 방문 순서대로 기록한다.
// - 2단계(입력 때문에 실패하지 않는다): TypeRegistry를 다시 조회하지 않고 TakeClassReference로
//   1단계 판정을 받는다 — 두 단계 사이에 등록이 바뀌어도 결과는 1단계 판정·진단과 같다(어긋난 방문
//   순서는 계약 위반 InvalidArgument).
// Deprecated 클래스 값은 받고 out.deprecatedClass를 표시한다(진단 DeprecatedType은
// ReportParsedValue — 리플렉션 B9h2). keyed(Map 키·Set
// 원소 자리 — 리플렉션 B9f2)면 ReportAndNull을 쓰지 않고 미등록은 늘 NotFound다(1.7절 — 대체 값이
// 서로 다른 두 키를 같게 만들어 2단계에서 중복이 생기지 않게).
inline Result<void> ResolveClassReference(FieldDecodeContext& context, TypeId base,
                                          const void* staticNative, ParsedValue& out,
                                          bool keyed = false) noexcept {
    if (out.guid == Guid{}) {
        return Result<void>::Success();
    }

    if (!context.IsRecording()) {
        auto decided = context.TakeClassReference(out.guid);
        if (!decided) {
            return Result<void>::Failure(decided.Error());
        }

        out.guid = decided.Value();
        return Result<void>::Success();
    }

    if (auto spent = context.Spend(1); !spent) {
        return spent;
    }

    auto found = context.AcquireClassReference(out.guid, base);
    if (found) {
        if (!found.Value().IsNativeA(staticNative)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        out.deprecatedClass = HasFlag(found.Value().Schema().flags, TypeFlags::Deprecated);
        return context.CollectClassReference(out.guid, false);
    }

    if (found.Error() != ErrorCode::NotFound) {
        return Result<void>::Failure(found.Error());
    }

    // 진단 없이 값을 바꾸지 않는다(직렬화기가 hook 없이 ReportAndNull을 주면 거부). 키 자리는
    // 정책과 무관하게 거부다.
    if (keyed || context.Policy().unknownClass != UnknownClassPolicy::ReportAndNull ||
        !context.CanReport()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (auto recorded = context.CollectClassReference(out.guid, true); !recorded) {
        return recorded;
    }

    out.unknownClass = true;
    out.storedClass = out.guid;
    out.guid = Guid{};
    return Result<void>::Success();
}

// reader에서 값 하나(type 종류)를 읽어 검증한다(단일 값 필드와 배열 원소 공용, 5.6절). 진단·청구는
// 하지 않는다(강한 참조는 문맥의 ReadReference, weak 참조는 ReadWeakReference — 1단계는 token을
// 남긴다, ClassRef는 ResolveClassReference가 조회 몫을 Spend하고 판정한다). 검증: 잘림·bool·UTF-8은
// reader 규칙, 정수 폭 Overflow, digest 길이·SoftObjectRef 규칙 InvalidArgument, enum은
// enumeration이 없으면 Unsupported이고 선언 여부를 set(있으면) 또는 스키마로 판정해
// out.undeclared에 표시만 한다. Name은 표시 문자열을 view로 읽고, 기록하는 단계(복원 1단계)면 그
// 몫(byte 수)을 Spend한 뒤 내용 규칙(ValidateNameFieldText — 길이·UTF-8·NUL·NFC, InvalidArgument)을
// 본다 — 토큰은 ResolveParsedName이 단계에 맞게 정한다. Struct·Array·Opaque는 Unsupported다.
// keyed는 Map 키·Set 원소 자리다(리플렉션 B9f2 — ClassRef 미등록은 정책과 무관하게 NotFound,
// ResolveClassReference). 미선언 enum은 여기서 표시만 하므로 키 자리의 거부는 호출자가 한다.
inline Result<void> ReadValueWire(ArchiveReader& reader, FieldDecodeContext& context,
                                  const ValueType& type, const EnumSchema* enumeration,
                                  const EnumValueSet* set, const void* staticNative,
                                  ParsedValue& out, bool keyed = false) noexcept {
    switch (type.kind) {
    case ValueKind::Bool: {
        auto decoded = reader.ReadBool();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.boolean = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32: {
        auto decoded = reader.ReadU32();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        if (decoded.Value() > UnsignedLimit(type.kind)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        out.unsignedInteger = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::UInt64: {
        auto decoded = reader.ReadU64();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.unsignedInteger = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64: {
        auto decoded = reader.ReadI64();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        if (!FitsSignedLimit(type.kind, decoded.Value())) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        out.signedInteger = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::Float32: {
        auto decoded = reader.ReadF32();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.float32 = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::Float64: {
        auto decoded = reader.ReadF64();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.float64 = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::Guid:
    case ValueKind::AssetId: {
        auto decoded = reader.ReadGuid();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.guid = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::Utf8String: {
        auto decoded = reader.ReadStringView();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.text = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::ClassReference: {
        auto decoded = reader.ReadGuid();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.guid = decoded.Value();
        return ResolveClassReference(context, type.type, staticNative, out, keyed);
    }
    case ValueKind::Name: {
        auto decoded = reader.ReadStringView();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        // 내용 검사는 기록하는 단계(복원 1단계)만 한다 — 2단계는 TakeName이 1단계 기록과 byte가
        // 같음을 확인하므로 다시 보지 않는다(NFC 판정 할당이 2단계의 입력 실패 지점이 되지 않게).
        // 청구는 검사 전이다(소진된 예산으로 검사하지 않는다 — 저장은 표시 문자열을 얻어야 길이를
        // 알므로 검사 뒤다). Spend 규약대로 1단계는 두 배를 청구한다(2단계가 하지 않는 몫까지 —
        // 보수적이다).
        if (context.IsRecording()) {
            if (auto spent = context.Spend(decoded.Value().ByteCount()); !spent) {
                return spent;
            }

            if (auto valid = ValidateNameFieldText(decoded.Value(), context.Scratch()); !valid) {
                return valid;
            }
        }

        out.text = decoded.Value();
        return Result<void>::Success();
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
            out.digest.bytes[index] = decoded.Value()[index];
        }

        return Result<void>::Success();
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

        out.guid = asset.Value();
        out.text = path.Value();
        return Result<void>::Success();
    }
    case ValueKind::Enum: {
        if (!enumeration) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        auto decoded = reader.ReadI64();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.signedInteger = decoded.Value();
        const bool declared = set ? set->Contains(out.signedInteger)
                                  : IsDeclaredEnumValue(*enumeration, out.signedInteger);
        if (!declared) {
            out.undeclared = true;
            out.stored = out.signedInteger;
        }

        return Result<void>::Success();
    }
    case ValueKind::ObjectReference: {
        auto decoded = context.ReadReference(reader, {type.type, staticNative});
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.reference = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::WeakObjectReference: {
        auto decoded = context.ReadWeakReference(reader, {type.type, staticNative});
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        out.reference = decoded.Value();
        return Result<void>::Success();
    }
    case ValueKind::Struct:
    case ValueKind::Array:
    case ValueKind::Optional:
    case ValueKind::Map:
    case ValueKind::Set:
    case ValueKind::StaticArray:
    case ValueKind::Opaque:
        break;
    }

    return Result<void>::Failure(ErrorCode::Unsupported);
}

// 선언되지 않은 enum 값(parsed.undeclared)에 decode 정책을 적용한다: Reject이거나 진단 받을 곳이
// 없으면(CanReport) InvalidArgument, ReportAndDefault면 대체 값 계산 몫(set이 있으면 조회 하나,
// 없으면 열거자 수)을 Spend하고 parsed.signedInteger를 대체 값으로 바꾼다(parsed.stored는 저장 값
// 그대로 — 진단은 ReportParsedValue).
inline Result<void> ResolveUndeclaredEnum(FieldDecodeContext& context, const EnumSchema& schema,
                                          const EnumValueSet* set, ParsedValue& parsed) noexcept {
    if (!parsed.undeclared) {
        return Result<void>::Success();
    }

    // 진단 없이 값을 바꾸지 않는다(직렬화기가 hook 없이 ReportAndDefault를 주면 거부).
    if (context.Policy().undeclaredEnum == UndeclaredEnumPolicy::Reject || !context.CanReport()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto spent = context.Spend(set ? set->LookupWork() : schema.values.Size()); !spent) {
        return spent;
    }

    parsed.signedInteger =
        set ? set->Replacement(parsed.stored) : EnumReplacementValue(schema, parsed.stored);
    return Result<void>::Success();
}

// 검증을 마친 값 하나의 진단·수집을 지금 위치로 보고한다(복원 2단계에서는 문맥이 버린다): 선언되지
// 않은 enum(UndeclaredEnumValue — value = 저장 값), null로 읽은 미등록 클래스(UnknownClassReference
// — valueType = 저장된 클래스 TypeId), non-null 소프트 참조 수집. 배열은 원소마다 원소
// 검증 직후 부른다(경로 [f[i]]). Required null은 여기서 보고하지 않는다 — 복원은 적용 뒤 staged
// 객체의 실효 값으로 한 번 진단한다(Docs/Design/ObjectFoundations.ko.md 4절).
inline Result<void> ReportParsedValue(FieldDecodeContext& context, const ValueType& type,
                                      const ParsedValue& parsed) noexcept {
    if (parsed.undeclared) {
        if (auto reported =
                context.Report(FieldDiagnosticKind::UndeclaredEnumValue, parsed.stored, type.type);
            !reported) {
            return reported;
        }
    }

    if (parsed.unknownClass) {
        if (auto reported =
                context.Report(FieldDiagnosticKind::UnknownClassReference, 0, parsed.storedClass);
            !reported) {
            return reported;
        }
    }

    if (parsed.deprecatedClass) {
        if (auto reported = context.Report(FieldDiagnosticKind::DeprecatedType, 0, parsed.guid);
            !reported) {
            return reported;
        }
    }

    if (type.kind == ValueKind::SoftObjectReference &&
        (parsed.guid != Guid{} || parsed.text.ByteCount() != 0)) {
        return context.CollectSoftReference(AssetId{parsed.guid}, parsed.text, type.type);
    }

    return Result<void>::Success();
}

// 검증을 마친 Name 값 하나(parsed.text)를 단계에 맞게 처리한다(ReflectionExtensions.ko.md 4.1절):
// 기록하는 단계(복원 1단계)는 CollectName으로 방문 순서대로 기록하고(2a intern 몫 청구 —
// parsed.name은 기본 None 그대로), 2단계는 TakeName이 준 토큰(2a가 대상 Registry의 pool로 intern한
// 것)을 parsed.name에 둔다. 단일 값 필드·struct 멤버와 배열 원소가 원소마다 같은 순서로 부른다.
inline Result<void> ResolveParsedName(FieldDecodeContext& context, ParsedValue& parsed) noexcept {
    if (context.IsRecording()) {
        return context.CollectName(parsed.text);
    }

    auto token = context.TakeName(parsed.text);
    if (!token) {
        return Result<void>::Failure(token.Error());
    }

    parsed.name = token.Value();
    return Result<void>::Success();
}

// 지금 위치의 단일 값 하나(type 종류 — 단일 값 필드의 값 형식 또는 Optional 필드의 내부 형식)를
// reader의 남은 byte 전부로 해석·검증한다(ParseFieldValue의 본문). 검증(정수 폭 Overflow, enum
// 정책, digest 길이·SoftObjectRef 규칙·trailing byte InvalidArgument)이 끝난 뒤 진단·수집을
// 보고하고 2단계 몫(ReserveApply)을 청구한다. apply가 있으면 게시할 값을 채운다(kind = type.kind,
// present = true — 문자열 복사는 apply의 allocator).
inline Result<void> ParseValuePayload(ArchiveReader& reader, FieldDecodeContext& context,
                                      const ValueType& type, const void* staticNative,
                                      FieldValue* apply) noexcept {
    const ValueKind kind = type.kind;
    ParsedValue parsed;
    if (auto read = ReadValueWire(reader, context, type, context.FieldEnum(), nullptr, staticNative,
                                  parsed);
        !read) {
        return read;
    }

    if (kind == ValueKind::Enum) {
        // 등록된 스키마로 하는 검증·대체 값 계산은 열거자 수에 비례하므로 그만큼 청구한다(1단계가
        // 2단계의 같은 검사 몫까지 청구한다).
        const EnumSchema& enumeration = *context.FieldEnum();
        if (auto spent = context.Spend(enumeration.values.Size()); !spent) {
            return spent;
        }

        if (auto resolved = ResolveUndeclaredEnum(context, enumeration, nullptr, parsed);
            !resolved) {
            return resolved;
        }
    }

    if (!reader.AtEnd()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 진단·수집은 필드 검증이 끝난 뒤 보고한다(복원 2단계에서는 문맥이 버린다).
    if (auto reported = ReportParsedValue(context, type, parsed); !reported) {
        return reported;
    }

    // Name: 1단계는 표시 문자열을 기록하고 2단계는 2a가 intern한 토큰을 받는다.
    if (kind == ValueKind::Name) {
        if (auto resolved = ResolveParsedName(context, parsed); !resolved) {
            return resolved;
        }
    }

    // 2단계 몫: staged 쓰기 1 + 문자열(값 복사와 staged 교체본) 2배 + 참조(강한·weak) 연결 1.
    // Name은 문자열을 복사하지 않는다(토큰을 쓴다 — intern 몫은 CollectName이 청구했다).
    const usize copiedText = kind == ValueKind::Name ? 0 : parsed.text.ByteCount();
    const bool reference =
        kind == ValueKind::ObjectReference || kind == ValueKind::WeakObjectReference;
    usize applyWork = 0;
    if (!TryMultiply(copiedText, 2, applyWork) ||
        !TryAdd(applyWork, reference ? 2 : 1, applyWork)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (auto reserved = context.ReserveApply(applyWork); !reserved) {
        return reserved;
    }

    if (!apply) {
        return Result<void>::Success();
    }

    apply->kind = kind;
    apply->present = true;
    apply->boolean = parsed.boolean;
    apply->unsignedInteger = parsed.unsignedInteger;
    apply->signedInteger = parsed.signedInteger;
    apply->float32 = parsed.float32;
    apply->float64 = parsed.float64;
    apply->guid = parsed.guid;
    apply->name = parsed.name;
    apply->digest = parsed.digest;
    apply->reference = parsed.reference.handle;
    apply->type = kind == ValueKind::Enum ? type.type : TypeId{};
    if (kind == ValueKind::Utf8String || kind == ValueKind::SoftObjectReference) {
        return apply->string.TryAssign(parsed.text);
    }

    return Result<void>::Success();
}

// 지금 위치(context.Field())의 단일 값 저장값 field를 해석·검증한다. 1단계와 2단계가 같은 reader
// 호출 순서를 실행하며(문자열·경로는 view로 읽는다), wire 종류(TypeMismatch)를 본 뒤 본문은
// ParseValuePayload다. apply가 있으면(2단계) 게시할 값을 채운다(문자열 복사는 apply의 allocator).
// staticNative는 강한 참조의 정적 대상 타입이다.
//
// Optional 필드(내부가 단일 값 — 리플렉션 B9e)는 payload를 OptionalWireReader로 열어(present byte
// 0·1 밖·unset 뒤 byte·내부 tag가 Optional 내부 자리의 것이 아님 InvalidArgument, 내부 tag ≠ 내부
// 형식의 wire TypeMismatch) set이면 내부 값을 ParseValuePayload(내부 형식 element)로 해석한다.
// unset이면 2단계 몫 1을 청구한다. apply의 kind는 내부 종류, present는 set 여부다(unset이면 값 칸은
// 기본값, enum이면 type만 그 enum ID).
inline Result<void> ParseFieldValue(const FieldView& field, FieldDecodeContext& context,
                                    const void* staticNative, FieldValue* apply) noexcept {
    const FieldSchema& schema = context.Field();
    const bool optional = schema.value.kind == ValueKind::Optional;
    auto wire = optional ? FieldWireType(schema, context.ReferenceWireType())
                         : FieldWireType(schema.value.kind, context.ReferenceWireType());
    if (!wire) {
        return Result<void>::Failure(wire.Error());
    }

    if (field.wireType != wire.Value()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // Optional<값 struct>(aggregate)는 이 단일 값 경로로 읽지 않는다(DecodeOptionalStruct).
    if (optional && FieldIsAggregate(schema)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto opened = context.OpenValue(field);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    if (!optional) {
        return ParseValuePayload(opened.Value(), context, schema.value, staticNative, apply);
    }

    auto inner = FieldWireType(schema.element.kind, context.ReferenceWireType());
    if (!inner) {
        return Result<void>::Failure(inner.Error());
    }

    auto reader = OptionalWireReader::Open(Move(opened.Value()), inner.Value());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    if (!reader.Value().IsSet()) {
        if (auto reserved = context.ReserveApply(1); !reserved) {
            return reserved;
        }

        if (apply) {
            apply->kind = schema.element.kind;
            apply->present = false;
            apply->type = schema.element.kind == ValueKind::Enum ? schema.element.type : TypeId{};
        }

        return Result<void>::Success();
    }

    if (apply) {
        apply->present = true;
    }

    return ParseValuePayload(reader.Value().Inner(), context, schema.element, staticNative, apply);
}

// 생성 thunk의 멤버 형식이 지금 스키마의 값 형식과 같은지다(별명·수동 스키마와 정적 thunk의
// 어긋남이 조용한 오인코딩이 되지 않게). 배열은 value = {Array, 0}이고 원소 종류가 같으며 enum·
// struct 원소는 element.type이 그 생성 ID여야 한다(struct 원소는 MatchesStructView도 — 호출자).
// 컨테이너 구별(Array<S>와 StructRefArray<S>)은 정적 thunk와 생성 static_assert가 맡는다.
template <class Member>
bool MatchesSchema(const FieldSchema& field) noexcept {
    if (field.value.kind != FieldValueKind<Member>()) {
        return false;
    }

    // Map·Set이 아닌 필드는 키 칸이 비어 있어야 한다(리플렉션 B9f2).
    if constexpr (!MapField<Member>::Supported) {
        if (field.key.kind != ValueKind::Opaque || field.key.type != TypeId{}) {
            return false;
        }
    }

    // Optional<T>(리플렉션 B9e)는 value = {Optional, 0}이고 내부 종류가 같으며 enum·struct 내부는
    // element.type이 그 생성 ID여야 한다(struct 내부는 MatchesStructView도 — 호출자).
    if constexpr (OptionalField<Member>::Supported) {
        using Inner = typename OptionalField<Member>::Inner;
        if (field.value.type != TypeId{} || field.element.kind != FieldValueKind<Inner>()) {
            return false;
        }

        if constexpr (ReflectedEnumField<Inner>::Supported ||
                      ReflectedStructField<Inner>::Supported) {
            return field.element.type == GeneratedType<Inner>::Id();
        } else {
            return true;
        }
    } else if constexpr (MapField<Member>::Supported) {
        // OrderedMap<K, V>·OrderedSet<T>(리플렉션 B9f2)는 value = {Map·Set, 0}이고 키·값(원소)
        // 종류가 같으며 enum·struct 대상은 그 생성 ID여야 한다(struct 값은 MatchesStructView도 —
        // 호출자). Set의 키 칸은 비어 있다.
        using Traits = MapField<Member>;
        if (field.value.type != TypeId{} ||
            field.element.kind != FieldValueKind<typename Traits::Value>()) {
            return false;
        }

        if constexpr (Traits::IsSet) {
            if (field.key.kind != ValueKind::Opaque || field.key.type != TypeId{}) {
                return false;
            }
        } else {
            if (field.key.kind != FieldValueKind<typename Traits::Key>()) {
                return false;
            }

            if constexpr (ReflectedEnumField<typename Traits::Key>::Supported) {
                if (field.key.type != GeneratedType<typename Traits::Key>::Id()) {
                    return false;
                }
            }
        }

        if constexpr (ReflectedEnumField<typename Traits::Value>::Supported ||
                      ReflectedStructField<typename Traits::Value>::Supported) {
            return field.element.type == GeneratedType<typename Traits::Value>::Id();
        } else {
            return true;
        }
    } else if constexpr (StaticArrayField<Member>::Supported) {
        // 정적 배열(리플렉션 B9g)은 value = {StaticArray, 0}이고 원소 종류가 같으며 maxElements가
        // 정확히 N이다(enum·struct 원소는 element.type이 그 생성 ID — struct 원소는
        // MatchesStructView도 호출자가 본다).
        using Element = typename StaticArrayField<Member>::Element;
        if (field.value.type != TypeId{} || field.element.kind != FieldValueKind<Element>() ||
            field.maxElements != StaticArrayField<Member>::Count) {
            return false;
        }

        if constexpr (ReflectedEnumField<Element>::Supported ||
                      ReflectedStructField<Element>::Supported) {
            return field.element.type == GeneratedType<Element>::Id();
        } else {
            return true;
        }
    } else if constexpr (ArrayField<Member>::Supported) {
        using Element = typename ArrayField<Member>::Element;
        if (field.value.type != TypeId{} || field.element.kind != FieldValueKind<Element>()) {
            return false;
        }

        if constexpr (ReflectedEnumField<Element>::Supported ||
                      ReflectedStructField<Element>::Supported) {
            return field.element.type == GeneratedType<Element>::Id();
        } else {
            return true;
        }
    } else if constexpr (ReflectedEnumField<Member>::Supported ||
                         ReflectedStructField<Member>::Supported) {
        return field.value.type == GeneratedType<Member>::Id();
    } else {
        return true;
    }
}

// 참조 멤버(강한·weak — 배열이면 원소)의 정적 대상 타입, ClassRef 멤버의 정적 Base다(그 밖은
// nullptr). 원소·키 형식을 Member로 넘겨 컨테이너 원소에도 쓴다.
template <class Member>
const void* StaticReferenceNative() noexcept {
    if constexpr (ObjectReferenceField<Member>::Supported) {
        return NativeTypeKey<typename ObjectReferenceField<Member>::Target>();
    } else if constexpr (WeakReferenceField<Member>::Supported) {
        return NativeTypeKey<typename WeakReferenceField<Member>::Target>();
    } else if constexpr (ClassReferenceField<Member>::Supported) {
        return NativeTypeKey<typename ClassReferenceField<Member>::Target>();
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

// 필드(또는 struct 멤버) 값이 Required 위치를 품는지다: 필드 자신이 Required이거나(단일 참조·
// 참조 배열), struct 필드·struct 원소 배열의 struct가 Required 후손을 담는다(StructView::
// ContainsRequired — 중첩 struct·배열 멤버·원소 struct 포함). 그래프 로드의 Required 실효 값
// 검사(Docs/Design/ObjectFoundations.ko.md 4절)가 검사할 최상위 필드와 struct 멤버를 이것으로
// 고른다. structure는 그 필드의 등록 struct(TypeLease·StructView::FieldStruct — 아니면 빈 view)다.
inline bool CarriesRequired(const FieldSchema& field, StructView structure) noexcept {
    return HasFlag(field.flags, FieldFlags::Required) ||
           (!structure.IsEmpty() && structure.ContainsRequired());
}

} // namespace rocks::detail
