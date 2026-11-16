#pragma once

#include <Base/Checked.h>
#include <Object/FieldCodec.h>
#include <Object/GeneratedFieldAccess.h>
#include <Serialization/MapWire.h>
#include <Serialization/OptionalWire.h>
#include <Serialization/SetWire.h>

// 필드 codec 사슬의 첫 조각으로 단일 값의 wire 쓰기 쪽이다(Docs/Design/ReflectionFoundations.ko.md
// 5.6·6.4절). 값 종류·필드의 wire tag(FieldWireType), 정수 폭 한도(UnsignedLimit·SignedLimit·
// FitsSignedLimit), enum 선언 값 집합(EnumValueSet), 값 쓰기(WriteValueWire)와 단일 값 필드
// 쓰기(EncodeFieldWire — Optional<단일 값> 포함)가 있다. 사슬(FieldValueWire → FieldValueParse →
// StructFieldCodec → ArrayFieldCodec → OptionalFieldCodec → MapFieldCodec → GeneratedFieldCodec)은
// 각 조각이 앞 조각을 include하므로 직접 include하지 말고 Object/GeneratedFieldCodec.h로 받는다.
namespace rocks::detail {

// 값 종류의 wire tag다. 참조(강한·weak)는 문맥의 referenceWire다. Array·Opaque는 Unsupported다(배열
// 필드는 아래 필드 단위 FieldWireType). 종류의 다른 성질은 MetaSchema 성질
// 표(ValueKindProperties)에 있고, wire 대응만 MetaSchema가 Serialization에 의존하지 않도록 이
// switch 하나에 둔다. Serialization에는 리플렉션 B9a3이 wire Optional 14·Set 15·Map 16·
// InstancedStruct 17·Text 18(Serialization/OptionalWire.h·SetWire.h·MapWire.h·
// InstancedStructWire.h·TextWire.h, 자리별 tag는 WireSlot.h)을 더했다. 그 값 종류를 더하는
// 단계(Name B9c2 = String, Optional B9e, Map·Set B9f2, InstancedStruct B9j, Text B9l —
// Docs/Design/ReflectionExtensions.ko.md 1.1·1.2절)가 이 switch와 아래 필드 단위 FieldWireType에
// 대응을 넣는다.
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
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
    case ValueKind::Enum:
        // 부호 있는 정수는 폭과 무관하게 I64다(폭을 넓히는 schema 변경이 wire 호환으로 남는다 —
        // Docs/Design/ReflectionExtensions.ko.md 2절).
        return Result<uint32>::Success(static_cast<uint32>(WireType::I64));
    case ValueKind::Float32:
        return Result<uint32>::Success(static_cast<uint32>(WireType::F32));
    case ValueKind::Float64:
        return Result<uint32>::Success(static_cast<uint32>(WireType::F64));
    case ValueKind::Guid:
    case ValueKind::AssetId:
    case ValueKind::ClassReference:
        // ClassRef는 클래스 TypeId다(null은 0 — Docs/Design/ReflectionExtensions.ko.md 5.2절).
        return Result<uint32>::Success(static_cast<uint32>(WireType::Guid));
    case ValueKind::Utf8String:
    case ValueKind::Name:
        // Name은 표시 문자열이다(None은 빈 문자열 — Docs/Design/ReflectionExtensions.ko.md 4절).
        return Result<uint32>::Success(static_cast<uint32>(WireType::String));
    case ValueKind::Digest256:
        return Result<uint32>::Success(static_cast<uint32>(WireType::Bytes));
    case ValueKind::SoftObjectReference:
        return Result<uint32>::Success(static_cast<uint32>(WireType::SoftObjectReference));
    case ValueKind::ObjectReference:
    case ValueKind::WeakObjectReference:
        // weak는 강한 참조와 같은 문맥 참조 wire다(graph v2 U32 — 같은 그래프 안 노드만,
        // Docs/Design/ReflectionExtensions.ko.md 5.1절).
        return Result<uint32>::Success(referenceWire);
    case ValueKind::Struct:
        return Result<uint32>::Success(static_cast<uint32>(WireType::Record));
    case ValueKind::Optional:
        // present byte + 내부 wire(Docs/Design/ReflectionExtensions.ko.md 1.2·6절).
        return Result<uint32>::Success(static_cast<uint32>(WireType::Optional));
    case ValueKind::Map:
        // 키 wire + 값 wire + count + 항목(Docs/Design/ReflectionExtensions.ko.md 1.2·7.2절).
        return Result<uint32>::Success(static_cast<uint32>(WireType::Map));
    case ValueKind::Set:
        // 원소 wire + count + 원소(같은 문서 1.2·7.2절).
        return Result<uint32>::Success(static_cast<uint32>(WireType::Set));
    case ValueKind::Array:
    case ValueKind::Opaque:
        break;
    }

    return Result<uint32>::Failure(ErrorCode::Unsupported);
}

// 필드 하나의 wire tag다(5.6절). 배열은 원소가 Struct면 RecordList, 그 밖의 단일 값 원소면
// ValueArray다(원소가 Opaque·Array면 Unsupported). Optional은 내부 종류가 Optional 내부 종류(성질
// 표)일 때 Optional이다(아니면 Unsupported — 내부 wire는 FieldWireType(element.kind)). Map은 키가
// Map 키 종류이고 값이 Map 값 종류(ObjectReference 제외 — ObjectRefMap은 리플렉션 B9f4)일 때 Map,
// Set은 원소가 Set 원소 종류일 때 Set이다(아니면 Unsupported — 리플렉션 B9f2). 그 밖은 값 종류의
// wire다. 직렬화기의 wire 재확인·ValidateLayout·struct encode의 멤버 wire 재확인이 쓴다.
inline Result<uint32> FieldWireType(const FieldSchema& field, uint32 referenceWire) noexcept {
    if (field.value.kind == ValueKind::Optional) {
        if (!ValueKindCanBeOptionalInner(field.element.kind)) {
            return Result<uint32>::Failure(ErrorCode::Unsupported);
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::Optional));
    }

    if (field.value.kind == ValueKind::Map) {
        if (!ValueKindCanBeMapKey(field.key.kind) || !ValueKindCanBeMapValue(field.element.kind) ||
            field.element.kind == ValueKind::ObjectReference) {
            return Result<uint32>::Failure(ErrorCode::Unsupported);
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::Map));
    }

    if (field.value.kind == ValueKind::Set) {
        if (!ValueKindCanBeSetElement(field.element.kind)) {
            return Result<uint32>::Failure(ErrorCode::Unsupported);
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::Set));
    }

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
    return kind == ValueKind::UInt8    ? uint64{NumericLimits<uint8>::Max()}
           : kind == ValueKind::UInt16 ? uint64{NumericLimits<uint16>::Max()}
           : kind == ValueKind::UInt32 ? uint64{NumericLimits<uint32>::Max()}
                                       : 0;
}

// 좁은 부호 있는 정수 종류(Int8·Int16·Int32)의 상한이다. 하한은 -상한 - 1이다(그 밖은 0 — 검사하지
// 않는다).
constexpr int64 SignedLimit(ValueKind kind) noexcept {
    return kind == ValueKind::Int8    ? int64{NumericLimits<int8>::Max()}
           : kind == ValueKind::Int16 ? int64{NumericLimits<int16>::Max()}
           : kind == ValueKind::Int32 ? int64{NumericLimits<int32>::Max()}
                                      : 0;
}

// value가 부호 있는 정수 종류 kind의 폭 안인지다(SignedLimit가 0인 Int64는 늘 참). 저장·로드가 같은
// 판정으로 Overflow를 낸다.
constexpr bool FitsSignedLimit(ValueKind kind, int64 value) noexcept {
    const int64 limit = SignedLimit(kind);
    return limit == 0 || (value >= -limit - 1 && value <= limit);
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
// 위치가 Required일 때 null 소프트·강한 참조(FieldEncodeContext::CheckRequired —
// rejectMissingRequired면 RequiredMissing 진단 + InvalidArgument, Required 검사 모드면 진단만).
// non-null 소프트 참조는 쓴 뒤 수집한다(검사 모드는 수집하지 않는다 — Soft 수집은 복원 1단계의 파일
// 값만). set이 있으면 enum 판정을 그 집합으로 하고 청구하지 않으며(배열 codec이 원소 수만큼 미리
// 청구한다), 없으면 열거자 수만큼 Spend한다. Name은 토큰을 문맥의 pool(Names())로 검사해 표시
// 문자열을 쓴다(AcquireNameFieldText — pool 없음 Unsupported, 다른 pool WrongContext, 게시되지 않은
// 토큰 StaleHandle, 내용 규칙 InvalidArgument, None은 빈 문자열) — 내용 검사 몫(byte 수)을
// Spend한다. weak 참조는 문맥의 WriteWeakReference(Required 검사 없음 — 대상이 죽었으면 null,
// 저장 집합 밖이면 정책대로 null + 진단)다. ClassRef는 Required null 검사 뒤 non-null 값을 문맥의
// AcquireClassReference(스키마 Base type.type)와 정적 Base(staticNative)로 다시 판정한다(조회 1을
// Spend — 미등록 NotFound, 그 밖 InvalidArgument, Required 검사 모드는 조회하지 않는다). Deprecated
// 클래스 값은 encode가 허용한다(로드한 값을 다시 저장할 수 있게 — 리플렉션 B9h2, 5.2절). key가
// 참이면 Map 키·Set 원소 자리다(리플렉션 B9f2): 키에는 Required가 없으므로 CheckRequired를 하지
// 않는다(필드의 Required는 Map 값·Set 원소마다 non-null이다 — Set 원소는 key = 거짓으로 쓴다).
inline Result<void> WriteValueWire(ArchiveWriter& out, FieldEncodeContext& context,
                                   const ValueType& type, const EnumSchema* enumeration,
                                   const FieldValue& value, StringView text,
                                   const void* staticNative, const EnumValueSet* set = nullptr,
                                   bool key = false) noexcept {
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
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
        if (!FitsSignedLimit(type.kind, value.signedInteger)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        return out.WriteI64(value.signedInteger);
    case ValueKind::Float32:
        return out.WriteF32(value.float32);
    case ValueKind::Float64:
        return out.WriteF64(value.float64);
    case ValueKind::Guid:
    case ValueKind::AssetId:
        return out.WriteGuid(value.guid);
    case ValueKind::ClassReference: {
        const bool isNull = value.guid == Guid{};
        if (auto required = key ? Result<void>::Success() : context.CheckRequired(isNull);
            !required) {
            return required;
        }

        // C++가 직접 넣은 값은 쓰기 검증을 거치지 않았으므로 저장 때 다시 검사한다. Required 검사
        // 모드(staged 실효 값)는 Required만 본다.
        if (!isNull && !context.IsRequiredCheck()) {
            if (auto spent = context.Spend(1); !spent) {
                return spent;
            }

            auto found = context.AcquireClassReference(value.guid, type.type);
            if (!found) {
                return Result<void>::Failure(found.Error());
            }

            if (!found.Value().IsNativeA(staticNative)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        return out.WriteGuid(value.guid);
    }
    case ValueKind::Utf8String:
        return out.WriteString(text);
    case ValueKind::Name: {
        // C++에서 직접 넣은 토큰은 쓰기 검증을 거치지 않았으므로 저장 때 다시 검사한다. lease가
        // 표시 문자열을 쓰는 동안 붙잡는다.
        // 청구는 검사 뒤다 — 표시 문자열을 얻어야 길이를 안다(검사 비용은 그 byte에 비례한다).
        auto display = AcquireNameFieldText(context.Names(), value.name, context.Scratch());
        if (!display) {
            return Result<void>::Failure(display.Error());
        }

        if (auto spent = context.Spend(display.Value().View().ByteCount()); !spent) {
            return spent;
        }

        return out.WriteString(display.Value().View());
    }
    case ValueKind::Digest256:
        return out.WriteBytes(Span<const uint8>(value.digest.bytes, Sha256Digest::Bytes));
    case ValueKind::SoftObjectReference: {
        // C++에서 직접 쓴 값은 쓰기 검증을 거치지 않았으므로 저장 때 다시 검사한다.
        const AssetId asset{value.guid};
        if (auto valid = SoftObjectRef<>::Validate(asset, text); !valid) {
            return valid;
        }

        const bool isNull = asset.IsNull() && text.ByteCount() == 0;
        if (auto required = key ? Result<void>::Success() : context.CheckRequired(isNull);
            !required) {
            return required;
        }

        if (auto written = out.WriteGuid(value.guid); !written) {
            return written;
        }

        if (auto written = out.WriteString(text); !written) {
            return written;
        }

        if (!isNull && !context.IsRequiredCheck()) {
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
    case ValueKind::WeakObjectReference:
        return context.WriteWeakReference(out, value.reference, {type.type, staticNative});
    case ValueKind::Struct:
    case ValueKind::Array:
    case ValueKind::Optional:
    case ValueKind::Map:
    case ValueKind::Set:
    case ValueKind::Opaque:
        break;
    }

    return Result<void>::Failure(ErrorCode::Unsupported);
}

// 지금 위치(context.Field())의 단일 값을 out에 쓰고 wire tag를 돌려준다. value.kind는 스키마의 값
// 종류와 같아야 한다(TypeMismatch). 검증·수집 규칙은 WriteValueWire다. Optional 필드(내부가 단일
// 값 — 리플렉션 B9e)는 value.kind가 내부 종류(element.kind)여야 하고, present가 거짓이면
// unset(present byte 0), 참이면 present 1 + 내부 wire + 내부 값(WriteValueWire — 내부 형식 element,
// enum은 FieldEnum)을 쓰고 wire Optional을 돌려준다. Optional이 아닌 필드에 present = false는
// InvalidArgument다.
inline Result<uint32> EncodeFieldWire(ArchiveWriter& out, FieldEncodeContext& context,
                                      const FieldValue& value, StringView text,
                                      const void* staticNative) noexcept {
    const FieldSchema& schema = context.Field();
    if (schema.value.kind == ValueKind::Optional) {
        if (value.kind != schema.element.kind || !ValueKindIsWholeFieldValue(value.kind)) {
            return Result<uint32>::Failure(ErrorCode::TypeMismatch);
        }

        auto inner = FieldWireType(value.kind, context.ReferenceWireType());
        if (!inner) {
            return inner;
        }

        if (!value.present) {
            if (auto written = OptionalWireWriter::WriteUnset(out); !written) {
                return Result<uint32>::Failure(written.Error());
            }

            return Result<uint32>::Success(static_cast<uint32>(WireType::Optional));
        }

        auto set = OptionalWireWriter::BeginSet(out, inner.Value());
        if (!set) {
            return Result<uint32>::Failure(set.Error());
        }

        if (auto written = WriteValueWire(out, context, schema.element, context.FieldEnum(), value,
                                          text, staticNative);
            !written) {
            return Result<uint32>::Failure(written.Error());
        }

        if (auto ended = set.Value().End(); !ended) {
            return Result<uint32>::Failure(ended.Error());
        }

        return Result<uint32>::Success(static_cast<uint32>(WireType::Optional));
    }

    if (value.kind != schema.value.kind) {
        return Result<uint32>::Failure(ErrorCode::TypeMismatch);
    }

    if (!value.present) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
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

} // namespace rocks::detail
