#pragma once

#include <Base/Checked.h>
#include <Object/FieldCodec.h>
#include <Object/GeneratedFieldAccess.h>

// 필드 codec이다(Docs/Design/ReflectionFoundations.ko.md 4.6·5.6·6.4절). 단일 값의 wire 쓰기
// (WriteValueWire)·읽기(ReadValueWire)·보고(ReportParsedValue)가 한 곳에 있고, 단일 값 필드
// (EncodeFieldWire·ParseFieldValue), struct 값(EncodeStructRecord·DecodeStructValue), 배열
// (EncodeArrayField·DecodeArrayField)이 이것을 쓴다. 생성 thunk(EncodeGeneratedField·
// DecodeGeneratedField·StructFieldOps)와 수동 함수표용 범용 codec(EncodeThroughFieldValue·
// DecodeThroughFieldValue)이 같이 쓴다. 생성 헤더와 직렬화기만 include한다.
namespace rocks::detail {

// 값 종류의 wire tag다. 강한 참조는 문맥의 referenceWire다. Array·Opaque는 Unsupported다(배열
// 필드는 아래 필드 단위 FieldWireType).
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

// 필드 하나의 wire tag다(5.6절). 배열은 원소가 Struct면 RecordList, 그 밖의 단일 값 원소면
// ValueArray다(원소가 Opaque·Array면 Unsupported). 그 밖은 값 종류의 wire다. 직렬화기의 wire
// 재확인·ValidateLayout·struct encode의 멤버 wire 재확인이 쓴다.
inline Result<uint32> FieldWireType(const FieldSchema& field, uint32 referenceWire) noexcept {
    if (field.value.kind != ValueKind::Array) {
        return FieldWireType(field.value.kind, referenceWire);
    }

    if (field.element.kind == ValueKind::Struct) {
        return Result<uint32>::Success(static_cast<uint32>(WireType::RecordList));
    }

    if (auto element = FieldWireType(field.element.kind, referenceWire); !element) {
        return element;
    }

    return Result<uint32>::Success(static_cast<uint32>(WireType::ValueArray));
}

// 좁은 정수 종류의 상한이다(그 밖은 0 — 검사하지 않는다).
constexpr uint64 UnsignedLimit(ValueKind kind) noexcept {
    return kind == ValueKind::UInt8    ? 0xffull
           : kind == ValueKind::UInt16 ? 0xffffull
           : kind == ValueKind::UInt32 ? 0xffffffffull
                                       : 0;
}

// 열거자 count개 집합의 원소 조회 하나의 작업량이다: BitWidth(count) + 1(이진 탐색 비교 수의 상한).
constexpr uint64 EnumLookupWork(usize count) noexcept {
    uint64 width = 1;
    for (usize remaining = count; remaining != 0; remaining >>= 1) {
        ++width;
    }

    return width;
}

// enum 원소 배열의 선언 값 판정 집합이다(5.6절 enum 원소 비용). 배열마다 Build로 한 번 만들고
// 원소마다 Contains(O(log v))·Replacement로 판정한다: Flags enum은 선언 mask, 일반 enum은 열거자
// 16개 이하면 스키마를 선형으로, 넘으면 Scratch에 정렬한 사본을 이진 탐색한다. 판정 결과는
// IsDeclaredEnumValue·EnumReplacementValue와 같다. schema는 집합보다 오래 살아야 한다(등록 enum
// 스키마 — 필드 lease 수명).
//
// 사용 예(배열 codec):
//   auto set = EnumValueSet::Build(*context.FieldEnum(), context);
//   if (!set) { return Result<void>::Failure(set.Error()); }
//   bool declared = set.Value().Contains(stored);
class EnumValueSet {
public:
    // schema의 판정 집합을 만든다. 작업량은 Spend로 청구한다: 선언 mask·0 선언 여부·사본 한 번 훑기
    // v, 정렬 사본이면 v × BitWidth(v)를 더한다. 사본 할당은 context.Scratch()(실패하면 그 오류)다.
    // underlying이 잘못된 스키마(등록이 막는다)는 InvalidArgument다.
    static Result<EnumValueSet> Build(const EnumSchema& schema,
                                      FieldCodecContext& context) noexcept {
        EnumValueSet set(schema, context.Scratch());
        if (!EnumUnderlyingRange(schema.underlying, set.minimum_, set.maximum_)) {
            return Result<EnumValueSet>::Failure(ErrorCode::InvalidArgument);
        }

        const usize count = schema.values.Size();
        if (auto spent = context.Spend(count); !spent) {
            return Result<EnumValueSet>::Failure(spent.Error());
        }

        set.flags_ = HasFlag(schema.flags, EnumFlags::Flags);
        set.mask_ = EnumDeclaredMask(schema);
        for (usize index = 0; index < count; ++index) {
            set.zeroDeclared_ = set.zeroDeclared_ || schema.values[index].value == 0;
        }

        if (set.flags_ || count <= LinearLimit) {
            return Result<EnumValueSet>::Success(Move(set));
        }

        uint64 sortWork = 0;
        if (!TryMultiply(uint64{count}, EnumLookupWork(count), sortWork)) {
            return Result<EnumValueSet>::Failure(ErrorCode::Overflow);
        }

        if (auto spent = context.Spend(sortWork); !spent) {
            return Result<EnumValueSet>::Failure(spent.Error());
        }

        if (auto reserved = set.sorted_.TryReserve(count); !reserved) {
            return Result<EnumValueSet>::Failure(reserved.Error());
        }

        for (usize index = 0; index < count; ++index) {
            if (auto pushed = set.sorted_.TryPushBack(schema.values[index].value); !pushed) {
                return Result<EnumValueSet>::Failure(pushed.Error());
            }
        }

        set.sorted_.Sort();
        return Result<EnumValueSet>::Success(Move(set));
    }

    EnumValueSet(EnumValueSet&&) noexcept = default;
    EnumValueSet& operator=(EnumValueSet&&) noexcept = default;
    EnumValueSet(const EnumValueSet&) = delete;
    EnumValueSet& operator=(const EnumValueSet&) = delete;

    // IsDeclaredEnumValue(schema, value)와 같다.
    bool Contains(int64 value) const noexcept {
        if (value < minimum_ || value > maximum_) {
            return false;
        }

        if (flags_) {
            return value >= 0 && (static_cast<uint64>(value) & ~mask_) == 0;
        }

        if (sorted_.IsEmpty()) {
            for (const EnumeratorSchema& declared : schema_->values) {
                if (declared.value == value) {
                    return true;
                }
            }

            return false;
        }

        usize low = 0;
        usize high = sorted_.Size();
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            if (sorted_[middle] == value) {
                return true;
            }

            if (sorted_[middle] < value) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        return false;
    }

    // EnumReplacementValue(schema, stored)와 같다(ReportAndDefault의 대체 값).
    int64 Replacement(int64 stored) const noexcept {
        if (flags_) {
            return stored < 0 || stored > maximum_
                       ? 0
                       : static_cast<int64>(static_cast<uint64>(stored) & mask_);
        }

        if (zeroDeclared_) {
            return 0;
        }

        return schema_->values.Size() != 0 ? schema_->values[0].value : 0;
    }

    // 원소 조회 하나의 작업량이다(EnumLookupWork(열거자 수)).
    uint64 LookupWork() const noexcept {
        return EnumLookupWork(schema_->values.Size());
    }

private:
    static constexpr usize LinearLimit = 16;

    EnumValueSet(const EnumSchema& schema, Allocator& allocator) noexcept
        : schema_(&schema), sorted_(allocator) {
    }

    const EnumSchema* schema_;
    Array<int64> sorted_;
    uint64 mask_ = 0;
    int64 minimum_ = 0;
    int64 maximum_ = 0;
    bool flags_ = false;
    bool zeroDeclared_ = false;
};

// 값 하나(type 종류)를 out에 쓴다(단일 값 필드와 배열 원소 공용, 5.6절). value.kind는 type.kind와
// 같아야 한다(TypeMismatch). text는 Utf8String의 값·SoftObjectReference의 경로이고(value.string
// 대신 쓴다 — 생성 thunk가 멤버를 복사하지 않게), staticNative는 강한 참조의 정적 대상 타입이다.
// 다시 검증하는 것: 정수 폭(Overflow), enum ID(TypeMismatch)·선언 값(InvalidArgument —
// enumeration이 없으면 Unsupported), SoftObjectRef null 규칙·경로 상한(InvalidArgument), 지금
// 위치가 Required이고 rejectMissingRequired일 때 null 소프트·강한 참조(RequiredMissing 진단 +
// InvalidArgument). non-null 소프트 참조는 쓴 뒤 수집한다. set이 있으면 enum 판정을 그 집합으로
// 하고 청구하지 않으며(배열 codec이 원소 수만큼 미리 청구한다), 없으면 열거자 수만큼 Spend한다.
inline Result<void> WriteValueWire(ArchiveWriter& out, FieldEncodeContext& context,
                                   const ValueType& type, const EnumSchema* enumeration,
                                   const FieldValue& value, StringView text,
                                   const void* staticNative,
                                   const EnumValueSet* set = nullptr) noexcept {
    if (value.kind != type.kind) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    switch (type.kind) {
    case ValueKind::Bool:
        return out.WriteBool(value.boolean);
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
        if (value.unsignedInteger > UnsignedLimit(type.kind)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        return out.WriteU32(static_cast<uint32>(value.unsignedInteger));
    case ValueKind::UInt64:
        return out.WriteU64(value.unsignedInteger);
    case ValueKind::Int32:
        if (value.signedInteger < -2147483648ll || value.signedInteger > 2147483647ll) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        return out.WriteI64(value.signedInteger);
    case ValueKind::Int64:
        return out.WriteI64(value.signedInteger);
    case ValueKind::Float32:
        return out.WriteF32(value.float32);
    case ValueKind::Float64:
        return out.WriteF64(value.float64);
    case ValueKind::Guid:
    case ValueKind::AssetId:
        return out.WriteGuid(value.guid);
    case ValueKind::Utf8String:
        return out.WriteString(text);
    case ValueKind::Digest256:
        return out.WriteBytes(Span<const uint8>(value.digest.bytes, Sha256Digest::Bytes));
    case ValueKind::SoftObjectReference: {
        // C++에서 직접 쓴 값은 쓰기 검증을 거치지 않았으므로 저장 때 다시 검사한다.
        const AssetId asset{value.guid};
        if (auto valid = SoftObjectRef<>::Validate(asset, text); !valid) {
            return valid;
        }

        const bool isNull = asset.IsNull() && text.ByteCount() == 0;
        if (auto required = context.CheckRequired(isNull); !required) {
            return required;
        }

        if (auto written = out.WriteGuid(value.guid); !written) {
            return written;
        }

        if (auto written = out.WriteString(text); !written) {
            return written;
        }

        if (!isNull) {
            return context.CollectSoftReference(asset, text, type.type);
        }

        return Result<void>::Success();
    }
    case ValueKind::Enum: {
        if (!enumeration) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (value.type != type.type) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        bool declared = false;
        if (set) {
            declared = set->Contains(value.signedInteger);
        } else {
            // 등록된 스키마로 하는 검증은 열거자 수에 비례하므로 그만큼 청구한다.
            if (auto spent = context.Spend(enumeration->values.Size()); !spent) {
                return spent;
            }

            declared = IsDeclaredEnumValue(*enumeration, value.signedInteger);
        }

        // C++에서 static_cast로 넣은 선언되지 않은 값은 저장하지 않는다.
        if (!declared) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return out.WriteI64(value.signedInteger);
    }
    case ValueKind::ObjectReference:
        return context.WriteReference(out, value.reference, {type.type, staticNative});
    case ValueKind::Struct:
    case ValueKind::Array:
    case ValueKind::Opaque:
        break;
    }

    return Result<void>::Failure(ErrorCode::Unsupported);
}

// 지금 위치(context.Field())의 단일 값을 out에 쓰고 wire tag를 돌려준다. value.kind는 스키마의 값
// 종류와 같아야 한다(TypeMismatch). 검증·수집 규칙은 WriteValueWire다.
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

    if (auto written = WriteValueWire(out, context, schema.value, context.FieldEnum(), value, text,
                                      staticNative);
        !written) {
        return Result<uint32>::Failure(written.Error());
    }

    return wire;
}

// ReadValueWire가 읽은 값 하나다. 할당하지 않는 view이며(text는 입력을 빌린다) kind에 맞는 칸만
// 뜻이 있다. undeclared는 선언되지 않은 enum 값을 읽었다는 표시(stored = 저장 값)이고, 정책
// 판정·대체 값은 ResolveUndeclaredEnum이 한다.
struct ParsedValue {
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
};

// reader에서 값 하나(type 종류)를 읽어 검증한다(단일 값 필드와 배열 원소 공용, 5.6절). 진단·청구는
// 하지 않는다(강한 참조는 문맥의 ReadReference — 1단계는 token을 남긴다). 검증: 잘림·bool·UTF-8은
// reader 규칙, 정수 폭 Overflow, digest 길이·SoftObjectRef 규칙 InvalidArgument, enum은
// enumeration이 없으면 Unsupported이고 선언 여부를 set(있으면) 또는 스키마로 판정해
// out.undeclared에 표시만 한다. Struct·Array·Opaque는 Unsupported다.
inline Result<void> ReadValueWire(ArchiveReader& reader, FieldDecodeContext& context,
                                  const ValueType& type, const EnumSchema* enumeration,
                                  const EnumValueSet* set, const void* staticNative,
                                  ParsedValue& out) noexcept {
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
    case ValueKind::Int32:
    case ValueKind::Int64: {
        auto decoded = reader.ReadI64();
        if (!decoded) {
            return Result<void>::Failure(decoded.Error());
        }

        if (type.kind == ValueKind::Int32 &&
            (decoded.Value() < -2147483648ll || decoded.Value() > 2147483647ll)) {
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
    case ValueKind::Struct:
    case ValueKind::Array:
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
// 않은 enum(UndeclaredEnumValue — value = 저장 값), Required인 위치의 null 소프트·강한
// 참조(RequiredMissing), non-null 소프트 참조 수집. 배열은 원소마다 원소 검증 직후 부른다(경로
// [f[i]]).
inline Result<void> ReportParsedValue(FieldDecodeContext& context, const ValueType& type,
                                      const ParsedValue& parsed) noexcept {
    if (parsed.undeclared) {
        if (auto reported =
                context.Report(FieldDiagnosticKind::UndeclaredEnumValue, parsed.stored, type.type);
            !reported) {
            return reported;
        }
    }

    if (type.kind == ValueKind::SoftObjectReference) {
        const bool isNull = parsed.guid == Guid{} && parsed.text.ByteCount() == 0;
        if (auto reported = context.ReportIfRequiredNull(isNull); !reported) {
            return reported;
        }

        if (!isNull) {
            return context.CollectSoftReference(AssetId{parsed.guid}, parsed.text, type.type);
        }
    }

    if (type.kind == ValueKind::ObjectReference) {
        return context.ReportIfRequiredNull(parsed.reference.isNull);
    }

    return Result<void>::Success();
}

// 지금 위치(context.Field())의 단일 값 저장값 field를 해석·검증한다. 1단계와 2단계가 같은 reader
// 호출 순서를 실행하며(문자열·경로는 view로 읽는다), 검증(wire 종류 TypeMismatch, 정수 폭 Overflow,
// enum 정책, digest 길이·SoftObjectRef 규칙·trailing byte InvalidArgument)이 끝난 뒤 진단·수집을
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
    ParsedValue parsed;
    if (auto read = ReadValueWire(reader, context, schema.value, context.FieldEnum(), nullptr,
                                  staticNative, parsed);
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
    if (auto reported = ReportParsedValue(context, schema.value, parsed); !reported) {
        return reported;
    }

    // 2단계 몫: staged 쓰기 1 + 문자열(값 복사와 staged 교체본) 2배 + 강한 참조 연결 1.
    usize applyWork = 0;
    if (!TryMultiply(parsed.text.ByteCount(), 2, applyWork) ||
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
    apply->boolean = parsed.boolean;
    apply->unsignedInteger = parsed.unsignedInteger;
    apply->signedInteger = parsed.signedInteger;
    apply->float32 = parsed.float32;
    apply->float64 = parsed.float64;
    apply->guid = parsed.guid;
    apply->digest = parsed.digest;
    apply->reference = parsed.reference.handle;
    apply->type = kind == ValueKind::Enum ? schema.value.type : TypeId{};
    if (kind == ValueKind::Utf8String || kind == ValueKind::SoftObjectReference) {
        return apply->string.TryAssign(parsed.text);
    }

    return Result<void>::Success();
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

    if constexpr (ArrayField<Member>::Supported) {
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

// 강한 참조 멤버(ObjectRefArray면 원소)의 정적 대상 타입이다(그 밖은 nullptr).
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
// 없는 가지는 건너뛰고(등록이 계산한 ContainsRequired) 방문마다 작업량 1을 청구한다. struct
// 멤버로만 내려가고(배열 원소 struct는 아님) 배열 멤버의 Required는 진단하지 않는다 — 누락 struct
// 안의 배열은 생성자 값이다(5.6절).
inline Result<void> ReportMissingRequired(StructView view, FieldDecodeContext& context) noexcept {
    const TypeSchema& schema = view.Schema();
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        if (auto spent = context.Spend(1); !spent) {
            return spent;
        }

        const FieldSchema& member = schema.fields[index];
        const StructView nested = view.FieldStruct(index);
        const bool required =
            HasFlag(member.flags, FieldFlags::Required) && member.value.kind != ValueKind::Array;
        const bool descend = member.value.kind == ValueKind::Struct && !nested.IsEmpty() &&
                             nested.ContainsRequired();
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
// Required 진단(struct 필드면 그 안의 Required 후손까지 — 2단계는 생략). 누락된 배열 필드는 생성자
// 값을 유지하며 Required 진단도 원소 struct 순회도 하지 않는다(Required 배열의 뜻은 원소마다
// non-null이다). 객체·struct가 같이 쓴다.
inline Result<void> CheckMissingField(FieldDecodeContext& context) noexcept {
    if (!context.IsMissingAllowed(context.EffectiveFlags())) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (context.Field().value.kind == ValueKind::Array) {
        return Result<void>::Success();
    }

    if (auto reported = context.ReportIfRequiredNull(true); !reported) {
        return reported;
    }

    // 진단을 받을 곳이 있을 때만 Required 후손을 돈다(받을 곳이 없으면 작업만 쓴다).
    const FieldCodecFrame* frame = context.Current();
    if (context.IsRecording() && context.CanReport() && frame &&
        frame->field->value.kind == ValueKind::Struct && !frame->structure.IsEmpty() &&
        frame->structure.ContainsRequired()) {
        return ReportMissingRequired(frame->structure, context);
    }

    return Result<void>::Success();
}

// struct 값(value)을 struct ID·버전 헤더의 중첩 Record로 out에 제자리로 쓴다(Docs/Design/
// ReflectionFoundations.ko.md 4.6절 — RecordWriter, 값 byte를 한 번만 쓰므로 중첩 깊이만큼 복사가
// 늘지 않는다). 참조를 담지 않는 struct는 먼저 SubstituteStruct hook을 거친다(배열 원소면 지금
// 위치의 index가 원소다). 멤버는 스키마 순서로 FieldScope 안에서 멤버 thunk가 out에 바로 쓰고, 유효
// 플래그가 제외(IsExcluded)인 멤버는 쓰지 않는다. thunk가 쓴 wire는 필드 단위
// FieldWireType으로 다시 확인한다(TypeMismatch). 쓴 TLV 수를 AddFields로 그래프 누계에 더한다.
// 작업량은 out의 청구로 센다(호출자가 out의 WorkUsed를 청구한다). 실패하면 out의 내용은 정해지지
// 않는다.
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

        // thunk가 스키마 필드의 wire를 썼는지 다시 확인한다(배열은 원소에 따라 ValueArray·
        // RecordList).
        auto expected = FieldWireType(member, context.ReferenceWireType());
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

// struct 값 하나의 저장 출처다: 필드 값(field — 중첩 Record TLV) 또는 struct 원소 배열의 목록
// 항목(list·index). 둘 중 하나만 쓴다.
struct StructRecordSource {
    const FieldView* field = nullptr;
    RecordListReader* list = nullptr;
    usize index = 0;
};

// struct 값 하나(중첩 Record — 필드 값 또는 목록 항목)를 해석한다(4.6·5.6절): Record 스택에
// 열고(필드는 OpenRecord, 항목은 OpenListItem — 둘 다 버전 범위 (1, UINT32_MAX)) 버전을 판정해
// 같으면 그대로, 낮으면 원본 Record byte(필드 payload·항목 ItemBytes)로 MigrateStruct한 결과를 원본
// scope를 닫은 뒤 현재 버전으로만 다시 열고, 높으면 newerVersion 정책(LoadNewerSkippingUnknown이면
// 지금 위치 경로의 NewerVersion 진단 — 배열 원소면 [f[i]])을 따른 뒤 DecodeStructRecord. 1단계에서
// 관찰 대상 struct면 검증과 같은 한 번의 해석으로 값을 채워(value가 있으면 그 자리 — 관찰 중인
// 조상이나 관찰 배열의 원소, 없으면 ObservationScratch 임시 값, 모든 경로에서 소멸) ObserveStruct를
// 부른다. 마이그레이션 보관과 2단계 커서는 방문 순서다.
inline Result<void> DecodeStructValue(StructView view, ConstructionContext* construction,
                                      ObjectHandle owner, void* value, StructRecordSource source,
                                      FieldDecodeContext& context) noexcept {
    if (view.IsEmpty() || (source.field == nullptr) == (source.list == nullptr)) {
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
        Allocator& scratch = context.ObservationScratch();
        auto memory = scratch.Allocate(structOps.size, structOps.alignment, 0x4f425356u);
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        structOps.construct(memory.Value(), scratch);
        temporary.ops = &structOps;
        temporary.allocator = &scratch;
        temporary.memory = memory.Value();
        value = memory.Value();
    }

    if (auto spent = context.Spend(1); !spent) {
        return spent;
    }

    auto scope = source.list ? context.OpenListItem(*source.list, source.index)
                             : context.OpenRecord(*source.field, schema.id, 1, ~0u);
    if (!scope) {
        return Result<void>::Failure(scope.Error());
    }

    const uint32 stored = context.CurrentRecord()->SchemaVersion();
    Result<void> decoded = Result<void>::Success();
    if (stored < schema.version) {
        // 원본 Record 바이트로 올리고, 원본 scope를 닫은 뒤 결과를 현재 버전으로만 연다.
        Span<const uint8> original;
        if (source.list) {
            auto bytes = source.list->ItemBytes(source.index);
            if (!bytes) {
                return Result<void>::Failure(bytes.Error());
            }

            original = bytes.Value();
        } else {
            original = source.field->payload;
        }

        auto migrated = context.MigrateStruct(schema.id, stored, schema.version, original);
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

// struct 필드 하나(중첩 Record TLV)를 해석한다 — 출처가 필드 값인 DecodeStructValue다.
inline Result<void> DecodeStructField(StructView view, ConstructionContext* construction,
                                      ObjectHandle owner, void* value, const FieldView& field,
                                      FieldDecodeContext& context) noexcept {
    StructRecordSource source;
    source.field = &field;
    return DecodeStructValue(view, construction, owner, value, source, context);
}

// 지금 위치의 등록 struct(배열이면 원소 struct)가 정적 thunk의 C++ struct와 같은지다(ID 대조에
// 더해 native 키로 — 수동 등록의 어긋남이 타입 혼동이 되지 않게).
template <class Member>
bool MatchesStructView(const FieldCodecContext& context) noexcept {
    const FieldCodecFrame* frame = context.Current();
    return frame && !frame->structure.IsEmpty() &&
           frame->structure.StructOps().nativeType == NativeTypeKey<Member>();
}

// 참조 멤버의 적용(2단계) 쓰기의 단일 지점이다(4.3·5.6절). 문맥에 StructRefArray 원소 위치가
// 있으면(StructElementScope 안) 컨테이너 기준 범위 검사의 SetElementReference, 없으면 소유 객체
// 저장소 안 주소의 SetReference다.
template <class T>
Result<void> SetStructMemberReference(ConstructionContext& construction,
                                      FieldDecodeContext& context, ObjectHandle owner,
                                      ObjectRef<T>& field, ObjectHandle target) noexcept {
    if (const detail::StructElementLocation* element = context.StructElement()) {
        return construction.SetElementReference(owner, *element, field, target);
    }

    return construction.SetReference(owner, field, target);
}

// 배열 원소 하나를 WriteValueWire 입력(value·text)으로 옮긴다. String·경로는 복사하지 않고 view로
// 둔다(원소 버퍼는 encode 고정이 지킨다).
template <class Element>
Result<void> ElementWireValue(const Element& element, FieldValue& value,
                              StringView& text) noexcept {
    value.kind = FieldValueKind<Element>();
    if constexpr (IsSameV<Element, String>) {
        text = element.View();
        return Result<void>::Success();
    } else if constexpr (SoftReferenceField<Element>::Supported) {
        value.guid = element.asset.value;
        text = element.objectPath.View();
        return Result<void>::Success();
    } else {
        return CopyFieldValue(element, value);
    }
}

// 단일 값 원소의 schema 최소 크기 shape다(Digest는 길이 prefix 8 + 32 byte exact, 그 밖은 wire
// 크기).
inline ValueArrayShape ElementArrayShape(ValueKind kind) noexcept {
    if (kind == ValueKind::Digest256) {
        return {8 + Sha256Digest::Bytes, true};
    }

    return {};
}

// enum 원소 배열의 판정 집합을 준비한다: 원소가 있으면 EnumValueSet을 만들어 set에 두고 count ×
// 조회 작업을 원소를 다루기 전에 Spend한다. 빈 배열은 만들지도 청구하지도 않는다(저장·로드 두
// 단계가 같은 조건 — set은 실패 상태로 남는다). enumeration이 없으면 Unsupported다.
inline Result<void> PrepareEnumElements(FieldCodecContext& context, const EnumSchema* enumeration,
                                        usize count, Result<EnumValueSet>& set) noexcept {
    if (!enumeration) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (count == 0) {
        return Result<void>::Success();
    }

    set = EnumValueSet::Build(*enumeration, context);
    if (!set) {
        return Result<void>::Failure(set.Error());
    }

    uint64 lookups = 0;
    if (!TryMultiply(uint64{count}, set.Value().LookupWork(), lookups)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    return context.Spend(lookups);
}

// struct 원소 배열(RecordList)의 목록 한도다(5.6절): 항목 수 MaxArrayElements, payload는 필드 값
// 한도 valueBytes, 항목 하나는 그 − 12(count 4 + 길이 8). 저장(EncodeArrayField — valueBytes =
// NestedRecordLimits의 전체 크기)과 직렬화기의 로드(graph_detail::ArrayLoadLimits —
// maxValueBytes)가 이 함수 하나로 같은 값을 만든다.
inline RecordListLimits ArrayRecordListLimits(usize valueBytes) noexcept {
    RecordListLimits limits;
    limits.maxItems = MaxArrayElements;
    limits.maxPayloadBytes = valueBytes;
    limits.maxItemBytes = valueBytes > 12 ? valueBytes - 12 : 0;
    return limits;
}

// 지금 위치(배열 필드 — context.Field())의 컨테이너 array를 out에 제자리로 쓰고 wire tag를
// 돌려준다(5.6절). 순서: MatchesSchema(struct 원소는 MatchesStructView도 — TypeMismatch) →
// Size ≤ maxElements(넘으면 배열 필드 경로의 ArrayTooLong 진단 뒤 CapacityExceeded) →
// AddElements(Size) → writer Begin(ValueArray — maxBytes·maxCount = 값 한도·MaxArrayElements,
// struct 원소는 RecordList — ArrayRecordListLimits(값 한도)) → 원소마다 ElementIndexScope::Set(i)
// 뒤 원소 → End. 원소마다 다시 검증한다: enum 선언 값(비어 있지 않은 배열마다 EnumValueSet 한 번 +
// 원소 수 × 조회 작업 선청구), Soft null 규칙·경로,
// rejectMissingRequired면 Required null(진단 경로 [f[i]]), 참조 원소는
// WriteReference({element.type, NativeTypeKey<T>()}, null은 0). struct 원소는 BeginItem →
// EncodeStructRecord → EndItem이다(치환 hook은 index 프레임에서). 가변 원소마다 Check()로 일찍
// 끊는다. 작업량은 out의 청구다. 호출자가 owner를 encode 고정한 상태여야 한다(원소 버퍼가 owner
// 밖이다).
template <class Container>
Result<uint32> EncodeArrayField(const Container& array, ArchiveWriter& out,
                                FieldEncodeContext& context) noexcept {
    using Traits = ArrayField<Container>;
    using Element = typename Traits::Element;
    static_assert(Traits::Supported);
    static_assert(FieldValueKind<Element>() != ValueKind::Opaque &&
                      FieldValueKind<Element>() != ValueKind::Array,
                  "Reflected array elements must be single values or reflected structs");
    const FieldSchema& field = context.Field();
    if (!MatchesSchema<Container>(field)) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (ReflectedStructField<Element>::Supported) {
        if (!MatchesStructView<Element>(context)) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }
    }

    // 필드별 상한은 저장(과 경로 쓰기)에서만 건다 — 넘친 배열은 그 경로로 진단한 뒤 거부한다.
    const usize count = array.Size();
    if (count > field.maxElements) {
        if (auto reported =
                context.Report(FieldDiagnosticKind::ArrayTooLong, static_cast<int64>(count));
            !reported) {
            return Result<uint32>::Failure(reported.Error());
        }

        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto counted = context.AddElements(count); !counted) {
        return Result<uint32>::Failure(counted.Error());
    }

    const usize maxBytes = context.NestedRecordLimits().archive.maxTotalBytes;
    if constexpr (ReflectedStructField<Element>::Supported) {
        const StructView view = context.Current()->structure;
        auto list = RecordListWriter::Begin(out, static_cast<uint32>(count),
                                            ArrayRecordListLimits(maxBytes));
        if (!list) {
            return Result<uint32>::Failure(list.Error());
        }

        ElementIndexScope element(context);
        for (usize index = 0; index < count; ++index) {
            element.Set(static_cast<uint32>(index));
            if (auto begun = list.Value().BeginItem(); !begun) {
                return Result<uint32>::Failure(begun.Error());
            }

            if (auto written = EncodeStructRecord(view, &array[index], out, context); !written) {
                return written;
            }

            if (auto ended = list.Value().EndItem(); !ended) {
                return Result<uint32>::Failure(ended.Error());
            }
        }

        if (auto ended = list.Value().End(); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::RecordList));
    } else {
        constexpr bool variable =
            IsSameV<Element, String> || SoftReferenceField<Element>::Supported;
        auto elementWire = FieldWireType(field.element.kind, context.ReferenceWireType());
        if (!elementWire) {
            return elementWire;
        }

        auto writer =
            ValueArrayWriter::Begin(out, elementWire.Value(), static_cast<uint32>(count), maxBytes,
                                    MaxArrayElements, ElementArrayShape(field.element.kind));
        if (!writer) {
            return Result<uint32>::Failure(writer.Error());
        }

        // enum 원소는 비어 있지 않은 배열마다 판정 집합을 한 번 만들고 원소 수만큼의 조회 작업을
        // 먼저 청구한다(로드 두 단계와 같은 조건).
        Result<EnumValueSet> set = Result<EnumValueSet>::Failure(ErrorCode::Unsupported);
        if constexpr (ReflectedEnumField<Element>::Supported) {
            if (auto prepared = PrepareEnumElements(context, context.FieldEnum(), count, set);
                !prepared) {
                return Result<uint32>::Failure(prepared.Error());
            }
        }

        const EnumValueSet* lookup = set ? &set.Value() : nullptr;
        FieldValue value(context.Scratch());
        ElementIndexScope element(context);
        for (usize index = 0; index < count; ++index) {
            element.Set(static_cast<uint32>(index));
            StringView text;
            if constexpr (ObjectReferenceField<Element>::Supported) {
                value.kind = ValueKind::ObjectReference;
                value.reference = array[index];
            } else if (auto copied = ElementWireValue(array[index], value, text); !copied) {
                return Result<uint32>::Failure(copied.Error());
            }

            if (auto written =
                    WriteValueWire(out, context, field.element, context.FieldEnum(), value, text,
                                   StaticReferenceNative<Element>(), lookup);
                !written) {
                return Result<uint32>::Failure(written.Error());
            }

            if constexpr (variable) {
                if (auto checked = writer.Value().Check(); !checked) {
                    return Result<uint32>::Failure(checked.Error());
                }
            }
        }

        if (auto ended = writer.Value().End(); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::ValueArray));
    }
}

// 해석한 단일 값 원소 하나를 target 끝에 넣는다(예약된 용량 안 — 주소가 옮겨지지 않는다).
// String·SoftObjectRef는 target의 allocator로 만든 뒤 TryAssign한다(실패하면 빈 원소가 남고 그 오류
// — 2단계면 그래프 rollback).
template <class Element>
Result<void> AppendParsedElement(Array<Element>& target, const ParsedValue& parsed) noexcept {
    if constexpr (IsSameV<Element, String>) {
        auto added = target.TryEmplaceBack(target.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        return added.Value()->TryAssign(parsed.text);
    } else if constexpr (SoftReferenceField<Element>::Supported) {
        auto added = target.TryEmplaceBack(target.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        return added.Value()->TryAssign(AssetId{parsed.guid}, parsed.text);
    } else if constexpr (ReflectedEnumField<Element>::Supported) {
        return target.TryPushBack(static_cast<Element>(parsed.signedInteger));
    } else if constexpr (IsSameV<Element, bool>) {
        return target.TryPushBack(parsed.boolean);
    } else if constexpr (IsSameV<Element, Guid>) {
        return target.TryPushBack(parsed.guid);
    } else if constexpr (IsSameV<Element, AssetId>) {
        return target.TryPushBack(AssetId{parsed.guid});
    } else if constexpr (IsSameV<Element, Sha256Digest>) {
        return target.TryPushBack(parsed.digest);
    } else if constexpr (IsSameV<Element, float>) {
        return target.TryPushBack(parsed.float32);
    } else if constexpr (IsSameV<Element, double>) {
        return target.TryPushBack(parsed.float64);
    } else if constexpr (IsSameV<Element, int32> || IsSameV<Element, int64>) {
        return target.TryPushBack(static_cast<Element>(parsed.signedInteger));
    } else {
        static_assert(IsSameV<Element, uint8> || IsSameV<Element, uint16> ||
                      IsSameV<Element, uint32> || IsSameV<Element, uint64>);
        return target.TryPushBack(static_cast<Element>(parsed.unsignedInteger));
    }
}

// 2단계 몫 선청구 값: 원소 메모리 count × elementBytes + 원소 수 + 1 + extra.
inline Result<void> ReserveArrayApply(FieldDecodeContext& context, usize count, usize elementBytes,
                                      uint64 extra) noexcept {
    uint64 work = 0;
    if (!TryMultiply(uint64{count}, uint64{elementBytes}, work) || !TryAdd(work, count, work) ||
        !TryAdd(work, uint64{1}, work) || !TryAdd(work, extra, work)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    return context.ReserveApply(work);
}

// 단일 값 원소 배열(Array<E>)의 decode다(DecodeArrayField의 ValueArray 가지). target이 없으면
// 검증만, 있으면(관찰·적용) Clear → TryReserve(count) → 원소마다 채운다.
template <class Element>
Result<void> DecodeValueArray(Array<Element>* target, const FieldView& field,
                              FieldDecodeContext& context) noexcept {
    const FieldSchema& schema = context.Field();
    const ValueType& type = schema.element;
    auto elementWire = FieldWireType(type.kind, context.ReferenceWireType());
    if (!elementWire) {
        return Result<void>::Failure(elementWire.Error());
    }

    auto opened = context.OpenValueArray(field, elementWire.Value(), MaxArrayElements,
                                         ElementArrayShape(type.kind));
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    ValueArrayReader& reader = opened.Value();
    const usize count = reader.Count();
    if (auto counted = context.AddElements(count); !counted) {
        return counted;
    }

    // enum 원소는 비어 있지 않은 배열마다 판정 집합을 한 번 만들고 원소 수만큼의 조회 작업을 원소
    // 읽기 전에 청구한다(1단계는 등록 enum과 정적 IsDeclared를 원소마다 모두 확인한다).
    Result<EnumValueSet> set = Result<EnumValueSet>::Failure(ErrorCode::Unsupported);
    if constexpr (ReflectedEnumField<Element>::Supported) {
        if (auto prepared = PrepareEnumElements(context, context.FieldEnum(), count, set);
            !prepared) {
            return prepared;
        }
    }

    if (target) {
        target->Clear();
        if (auto reserved = target->TryReserve(count); !reserved) {
            return reserved;
        }
    }

    const EnumValueSet* lookup = set ? &set.Value() : nullptr;
    usize textBytes = 0;
    ElementIndexScope element(context);
    for (usize index = 0; index < count; ++index) {
        element.Set(static_cast<uint32>(index));
        ParsedValue parsed;
        if (auto read = ReadValueWire(reader.Elements(), context, type, context.FieldEnum(), lookup,
                                      nullptr, parsed);
            !read) {
            return read;
        }

        if constexpr (ReflectedEnumField<Element>::Supported) {
            if (auto resolved =
                    ResolveUndeclaredEnum(context, *context.FieldEnum(), lookup, parsed);
                !resolved) {
                return resolved;
            }

            // 등록 enum이 정적 enum보다 넓어도 2단계가 입력 때문에 실패하지 않게 한다.
            if (!GeneratedType<Element>::IsDeclared(parsed.signedInteger)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        // 원소 진단은 원소 검증 직후 [f[i]]로 낸다.
        if (auto reported = ReportParsedValue(context, type, parsed); !reported) {
            return reported;
        }

        if (!TryAdd(textBytes, parsed.text.ByteCount(), textBytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        if (target) {
            if (auto appended = AppendParsedElement(*target, parsed); !appended) {
                return appended;
            }
        }
    }

    if (auto finished = reader.Finish(); !finished) {
        return finished;
    }

    uint64 textWork = 0;
    if (!TryMultiply(uint64{textBytes}, uint64{2}, textWork)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    return ReserveArrayApply(context, count, sizeof(Element), textWork);
}

// ObjectRefArray decode다(원소 tag = ReferenceWireType(), null 원소 허용). 1단계는 원소마다
// ReadReference — null은 token을 남기지 않고 Required면 [f[i]] 진단, non-null은 (token, 필드 기대
// 형식 {element.type, NativeTypeKey<T>()})을 남긴다. 2단계는 handle(null 포함)을 Scratch
// Array<ObjectHandle>(count 예약)에 모아 AssignReferences를 한 번 부른다.
template <class T>
Result<void> DecodeReferenceArray(ConstructionContext* construction, ObjectHandle owner,
                                  ObjectRefArray<T>* target, const FieldView& field,
                                  FieldDecodeContext& context) noexcept {
    const FieldSchema& schema = context.Field();
    auto opened = context.OpenValueArray(field, context.ReferenceWireType(), MaxArrayElements);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    ValueArrayReader& reader = opened.Value();
    const usize count = reader.Count();
    if (auto counted = context.AddElements(count); !counted) {
        return counted;
    }

    const bool apply = construction != nullptr && target != nullptr;
    Array<ObjectHandle> handles(context.Scratch());
    if (apply) {
        if (auto reserved = handles.TryReserve(count); !reserved) {
            return reserved;
        }
    }

    usize references = 0;
    ElementIndexScope element(context);
    for (usize index = 0; index < count; ++index) {
        element.Set(static_cast<uint32>(index));
        ParsedValue parsed;
        if (auto read = ReadValueWire(reader.Elements(), context, schema.element, nullptr, nullptr,
                                      NativeTypeKey<T>(), parsed);
            !read) {
            return read;
        }

        if (auto reported = ReportParsedValue(context, schema.element, parsed); !reported) {
            return reported;
        }

        references += parsed.reference.isNull ? 0 : 1;
        if (apply) {
            if (auto pushed = handles.TryPushBack(parsed.reference.handle); !pushed) {
                return pushed;
            }
        }
    }

    if (auto finished = reader.Finish(); !finished) {
        return finished;
    }

    // 원소 메모리는 staged 배열 버퍼와 Scratch 사본(각 count × sizeof(ObjectHandle))이고, 참조 연결
    // 수를 더한다.
    if (auto reserved =
            ReserveArrayApply(context, count, 2 * sizeof(ObjectHandle), uint64{references});
        !reserved) {
        return reserved;
    }

    if (!apply) {
        return Result<void>::Success();
    }

    return construction->AssignReferences(owner, *target, handles.View());
}

// struct 원소 배열(Array<S>·StructRefArray<S>) decode다(목록 항목 Record — 버전 범위 (1,
// UINT32_MAX)). 원소마다 ElementIndexScope::Set(i) 뒤 DecodeStructValue(목록 항목 출처)다.
// Array<S>는 검증(target 없음)이면 값 없이, 관찰·적용이면 Clear → TryReserve(count) → 원소를
// TryEmplaceBack(allocator 생성자가 있으면 target의 allocator)한 직후 그 주소에
// 해석한다(예약했으므로 주소가 고정된다). StructRefArray<S>는 2단계에서 AssignElements(owner,
// count) 뒤 원소마다 StructElementScope 안에서 원소 주소에 적용한다(참조 멤버는
// SetElementReference로 간다).
template <class Container>
Result<void> DecodeStructArray(ConstructionContext* construction, ObjectHandle owner,
                               Container* target, const FieldView& field,
                               FieldDecodeContext& context) noexcept {
    using Element = typename ArrayField<Container>::Element;
    const StructView view = context.Current()->structure;
    auto opened = context.OpenRecordList(field, view.Schema().id);
    if (!opened) {
        return Result<void>::Failure(opened.Error());
    }

    RecordListReader& list = opened.Value();
    const usize count = list.ItemCount();
    if (auto counted = context.AddElements(count); !counted) {
        return counted;
    }

    usize elementBytes = sizeof(Element);
    if constexpr (ArrayField<Container>::Registry) {
        // StructRefArray: 원소 블록 + 색인 포인터.
        elementBytes += sizeof(void*);
        const bool apply = construction != nullptr && target != nullptr;
        if (apply) {
            if (auto assigned = construction->AssignElements(owner, *target, count); !assigned) {
                return assigned;
            }
        }

        ElementIndexScope element(context);
        for (usize index = 0; index < count; ++index) {
            element.Set(static_cast<uint32>(index));
            StructRecordSource source;
            source.list = &list;
            source.index = index;
            if (apply) {
                StructElementScope located(context, StructRefArrayAccess::Location(*target, index));
                if (auto decoded = DecodeStructValue(view, construction, owner, &(*target)[index],
                                                     source, context);
                    !decoded) {
                    return decoded;
                }
            } else if (auto decoded =
                           DecodeStructValue(view, nullptr, owner, nullptr, source, context);
                       !decoded) {
                return decoded;
            }
        }
    } else {
        if (target) {
            target->Clear();
            if (auto reserved = target->TryReserve(count); !reserved) {
                return reserved;
            }
        }

        ElementIndexScope element(context);
        for (usize index = 0; index < count; ++index) {
            element.Set(static_cast<uint32>(index));
            Element* value = nullptr;
            if (target) {
                Result<Element*> added = Result<Element*>::Failure(ErrorCode::Unsupported);
                if constexpr (__is_constructible(Element, Allocator&)) {
                    added = target->TryEmplaceBack(target->GetAllocator());
                } else {
                    added = target->TryEmplaceBack();
                }

                if (!added) {
                    return Result<void>::Failure(added.Error());
                }

                value = added.Value();
            }

            StructRecordSource source;
            source.list = &list;
            source.index = index;
            if (auto decoded = DecodeStructValue(view, construction, owner, value, source, context);
                !decoded) {
                return decoded;
            }
        }
    }

    return ReserveArrayApply(context, count, elementBytes, 0);
}

// 지금 위치(배열 필드)의 저장값 field를 해석한다(5.6절). 세 방식은 struct와 같다: 검증
// (construction·target null — 원소를 할당하지 않는다), 관찰(construction null, target = 관찰 임시
// 값 안의 컨테이너 — ObservationScratch 원소로 채운다, 참조 없는 struct라 Array<E>만 온다), 적용(둘
// 다 non-null — staged 객체 안). 1·2단계가 같은 reader 호출 순서를 실행한다: wire
// 확인(TypeMismatch) → MatchesSchema(TypeMismatch) → OpenValueArray(maxCount =
// MaxArrayElements)·OpenRecordList → AddElements(count) → 원소 → Finish(trailing InvalidArgument) →
// ReserveApply 한 번(원소 메모리 count × sizeof(E) — StructRefArray는 + 색인 포인터,
// ObjectRefArray는 Scratch 사본 포함 — + 원소 수 + 1 + 2 × 문자열·경로 byte, ObjectRefArray는 +
// 참조 수). 로드는 필드별 maxElements를 걸지 않는다. 2단계 실패는 할당과 Registry 상태뿐이다(입력
// 검증은 모두 1단계).
template <class Container>
Result<void> DecodeArrayField(ConstructionContext* construction, ObjectHandle owner,
                              Container* target, const FieldView& field,
                              FieldDecodeContext& context) noexcept {
    using Traits = ArrayField<Container>;
    using Element = typename Traits::Element;
    static_assert(Traits::Supported);
    static_assert(FieldValueKind<Element>() != ValueKind::Opaque &&
                      FieldValueKind<Element>() != ValueKind::Array,
                  "Reflected array elements must be single values or reflected structs");
    const FieldSchema& schema = context.Field();
    auto wire = FieldWireType(schema, context.ReferenceWireType());
    if (!wire) {
        return Result<void>::Failure(wire.Error());
    }

    if (field.wireType != wire.Value()) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (!MatchesSchema<Container>(schema)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if constexpr (ReflectedStructField<Element>::Supported) {
        if (!MatchesStructView<Element>(context)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        return DecodeStructArray(construction, owner, target, field, context);
    } else if constexpr (ObjectReferenceField<Element>::Supported) {
        return DecodeReferenceArray(construction, owner, target, field, context);
    } else {
        return DecodeValueArray(target, field, context);
    }
}

// 생성 Trace가 모든 배열 필드·멤버에 부르는 오버로드 집합이다(5.6절 Trace). 실제 C++ 타입이 호출을
// 고르므로 descriptor에서 컨테이너를 잘못 추론해도 Trace가 빠지지 않는다: Array<E>는 강한 참조가
// 없어 아무것도 하지 않고, ObjectRefArray·StructRefArray는 모든 원소를 방문한다.
//
// 사용 예(생성 Trace):
//   detail::TraceArrayField(value.*(&::Mesh::materials), visitor);
template <class E>
void TraceArrayField(const Array<E>&, TraceVisitor&) noexcept {
}

template <class T>
void TraceArrayField(const ObjectRefArray<T>& array, TraceVisitor& visitor) noexcept {
    array.Trace(visitor);
}

template <class S>
void TraceArrayField(const StructRefArray<S>& array, TraceVisitor& visitor) noexcept {
    array.Trace(visitor);
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
    if constexpr (ArrayField<Member>::Supported) {
        return EncodeArrayField(source, out, context);
    } else if constexpr (ReflectedStructField<Member>::Supported) {
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
// 적용 방식은 struct 값에 직접 쓴다(참조 멤버의 적용은 SetStructMemberReference, 배열 멤버는
// DecodeArrayField).
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
    if constexpr (ArrayField<Member>::Supported) {
        return DecodeArrayField(construction, owner, target ? &(target->*MemberPointer) : nullptr,
                                field, context);
    } else if constexpr (ReflectedStructField<Member>::Supported) {
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

        return SetStructMemberReference(*construction, context, owner, target->*MemberPointer,
                                        value.reference);
    } else {
        // 검증 방식은 값을 채우지 않는다(문자열을 scratch에 복사하지 않는다). enum은 정적 enum
        // 확인에 값이 필요하다. 관찰 방식(construction 없음·target 있음)의 문자열 사본은 1단계에만
        // 있는 일이라 ObservationScratch로 한 번만 청구한다(Scratch는 2단계 몫까지 두 번 센다).
        FieldValue value(!construction && target ? context.ObservationScratch()
                                                 : context.Scratch());
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

// struct 멤버 하나의 함수표다(생성 코드·수동 시험 공용). valueNativeType은 Enum·Struct 멤버와
// Enum·Struct 원소 배열 멤버의 원소 native 키다. path는 경로 접근 thunk(B6 — 생성 코드는
// &detail::PathStructMember<&S::m, ValueKind::Target>, Object/GeneratedPathAccess.h)이며 주지
// 않으면 이 멤버로 들어가는 경로 연산은 Unsupported다.
//
// 사용 예:
//   const RuntimeStructFieldOps fields[] = {detail::StructFieldOps<&BulkDataRef::bulkId>(
//       BulkId, &detail::PathStructMember<&BulkDataRef::bulkId, ValueKind::UInt32>)};
template <auto MemberPointer>
RuntimeStructFieldOps StructFieldOps(Guid fieldId,
                                     StructFieldPathFunction path = nullptr) noexcept {
    using Member = typename StructMemberPointer<decltype(MemberPointer)>::Member;
    RuntimeStructFieldOps ops;
    ops.fieldId = fieldId;
    ops.encode = &EncodeStructMember<MemberPointer>;
    ops.decode = &DecodeStructMember<MemberPointer>;
    ops.path = path;
    if constexpr (ArrayField<Member>::Supported) {
        using Element = typename ArrayField<Member>::Element;
        if constexpr (ReflectedEnumField<Element>::Supported ||
                      ReflectedStructField<Element>::Supported) {
            ops.valueNativeType = NativeTypeKey<Element>();
        }
    } else if constexpr (ReflectedEnumField<Member>::Supported ||
                         ReflectedStructField<Member>::Supported) {
        ops.valueNativeType = NativeTypeKey<Member>();
    }

    return ops;
}

// 생성 encode thunk다. owner를 Borrow하고 멤버를 직접 읽어 쓴다(String·경로·배열 원소를 복사하지
// 않는다 — ArchiveWriter가 성장 전에 크기 한도를 검사하고, owner 밖 버퍼는 호출자가 쥔 encode
// 고정이 지킨다). 쓰기 뒤 owner의 논리 수명을 다시 확인한다.
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
    if constexpr (ArrayField<Member>::Supported) {
        // 배열 필드는 컨테이너를 제자리로 쓴다(ValueArray·RecordList).
        wire = EncodeArrayField(source, out, context);
    } else if constexpr (ReflectedStructField<Member>::Supported) {
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

    if constexpr (ArrayField<Member>::Supported) {
        // 배열 필드: 1단계는 원소 없이 검증, 2단계는 새 객체 안의 컨테이너에 쓴다.
        if (!construction) {
            return DecodeArrayField<Member>(nullptr, handle, nullptr, field, context);
        }

        auto owner = construction->Get<Owner>(handle);
        if (!owner) {
            return Result<void>::Failure(owner.Error());
        }

        return DecodeArrayField(construction, handle, &(owner.Value()->*member), field, context);
    } else if constexpr (ReflectedStructField<Member>::Supported) {
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
// 대상 타입을 모르므로 스키마 기대 타입만 검사한다. 단일 값 필드만 지원한다(struct·배열은
// Unsupported).
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
