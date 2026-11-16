#include "TextInternal.h"

#include <Base/NumericLimits.h>
#include <Core/Guid.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/Unicode.h>
#include <Object/AssetId.h>
#include <Object/ClassRef.h>
#include <Object/NameField.h>
#include <Object/ValueText.h>

// 단일 값의 명령행 표기다(리플렉션 B9i3b — Docs/Design/ReflectionExtensions.ko.md 10.4절 표).
// 문맥 없이 정해지는 종류(bool·정수·enum·Guid·AssetId·Digest)는 ObjectRuntime의 공용 표기
// (Object/ValueText.h — 경로 key 텍스트와 같은 규칙)를 쓰고, 실수·Name·참조·ClassRef는 여기서 문맥
// (Name pool·resolver·TypeRegistry)으로 다룬다.
namespace rocks::reflection_text {

namespace detail {

namespace {

Result<bool> Fail(ScalarFailure& failure, TextDiagnosticCode code, StringView message) noexcept {
    failure.code = code;
    failure.message = message;
    return Result<bool>::Success(false);
}

// 좁은 정수 폭의 범위 안인지 본다(UInt64·Int64는 해석이 이미 본다).
bool FitsWidth(ValueKind kind, uint64 unsignedValue, int64 signedValue) noexcept {
    switch (kind) {
    case ValueKind::UInt8:
        return unsignedValue <= NumericLimits<uint8>::Max();
    case ValueKind::UInt16:
        return unsignedValue <= NumericLimits<uint16>::Max();
    case ValueKind::UInt32:
        return unsignedValue <= NumericLimits<uint32>::Max();
    case ValueKind::Int8:
        return signedValue >= NumericLimits<int8>::Min() &&
               signedValue <= NumericLimits<int8>::Max();
    case ValueKind::Int16:
        return signedValue >= NumericLimits<int16>::Min() &&
               signedValue <= NumericLimits<int16>::Max();
    case ValueKind::Int32:
        return signedValue >= NumericLimits<int32>::Min() &&
               signedValue <= NumericLimits<int32>::Max();
    default:
        return true;
    }
}

// 실수 텍스트: 10진(Core TryParseFloat·TryParseDouble)과 비유한 `nan`·`inf`·`-inf`(소문자 그대로)만
// 받는다(Core 해석이 받는 `infinity`·`NaN`·`+inf`는 거부 — 표기가 하나가 되게).
Result<bool> ParseReal(ValueKind kind, StringView text, FieldValue& out,
                       ScalarFailure& failure) noexcept {
    const bool special = text == "nan" || text == "inf" || text == "-inf";
    if (!special) {
        const char* data = text.Data();
        const usize size = text.ByteCount();
        usize start = size != 0 && (data[0] == '-' || data[0] == '+') ? 1 : 0;
        if (start == size || !((data[start] >= '0' && data[start] <= '9') || data[start] == '.')) {
            return Fail(failure, TextDiagnosticCode::InvalidValue, "not a decimal number");
        }
    }

    if (kind == ValueKind::Float32) {
        auto parsed = TryParseFloat(text);
        if (!parsed) {
            return parsed.Error() == ErrorCode::Overflow
                       ? Fail(failure, TextDiagnosticCode::OutOfRange, "number out of range")
                       : Fail(failure, TextDiagnosticCode::InvalidValue, "not a decimal number");
        }

        out.float32 = parsed.Value();
        return Result<bool>::Success(true);
    }

    auto parsed = TryParseDouble(text);
    if (!parsed) {
        return parsed.Error() == ErrorCode::Overflow
                   ? Fail(failure, TextDiagnosticCode::OutOfRange, "number out of range")
                   : Fail(failure, TextDiagnosticCode::InvalidValue, "not a decimal number");
    }

    out.float64 = parsed.Value();
    return Result<bool>::Success(true);
}

// Name: NFC로 정규화하고 내용 규칙(길이·NUL — Object/NameField.h)을 본 뒤 pool에 intern한다.
Result<bool> ParseName(TextCodecContext& context, StringView text, FieldValue& out,
                       ScalarFailure& failure) noexcept {
    NamePool* pool = context.Options().names;
    if (!pool) {
        return Fail(failure, TextDiagnosticCode::Unsupported, "Name values need a NamePool");
    }

    StringBuilder<128> normalized(context.GetAllocator());
    auto made = TryNormalize(NormalizationForm::NFC, text, normalized);
    if (!made) {
        if (made.Error() == ErrorCode::InvalidArgument) {
            return Fail(failure, TextDiagnosticCode::InvalidValue, "not valid UTF-8");
        }

        return Result<bool>::Failure(made.Error());
    }

    auto valid = ValidateNameFieldText(normalized.View(), context.GetAllocator());
    if (!valid) {
        if (valid.Error() == ErrorCode::InvalidArgument) {
            return Fail(failure, TextDiagnosticCode::InvalidValue, "not a valid Name");
        }

        return Result<bool>::Failure(valid.Error());
    }

    auto interned = pool->TryIntern(normalized.View());
    if (!interned) {
        return Result<bool>::Failure(interned.Error());
    }

    out.name = interned.Value();
    return Result<bool>::Success(true);
}

TextReferenceKind ReferenceKindOf(ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::WeakObjectReference:
        return TextReferenceKind::Weak;
    case ValueKind::SoftObjectReference:
        return TextReferenceKind::Soft;
    default:
        return TextReferenceKind::Hard;
    }
}

// 참조: `none`(대소문자 무시)은 null, 그 밖은 resolver. resolver가 없으면 Soft는 36자 AssetId(빈
// 객체 경로)만 받고 Hard·Weak는 Unsupported다.
Result<bool> ParseReference(TextCodecContext& context, const TextScalarType& type, StringView text,
                            FieldValue& out, ScalarFailure& failure) noexcept {
    const ValueKind kind = type.type.kind;
    out.reference = ObjectHandle{};
    out.guid = Guid{};
    out.string.Clear();
    if (TextEqualsIgnoringCase(text, "none")) {
        return Result<bool>::Success(true);
    }

    TextReferenceResolver* resolver = context.Options().resolver;
    if (resolver) {
        auto resolved = resolver->Resolve(text, type.type.type, ReferenceKindOf(kind), out);
        if (!resolved) {
            if (resolved.Error() == ErrorCode::OutOfMemory) {
                return Result<bool>::Failure(resolved.Error());
            }

            out.reference = ObjectHandle{};
            out.guid = Guid{};
            out.string.Clear();
            return Fail(failure, TextDiagnosticCode::Unresolved, "reference not resolved");
        }

        out.kind = kind;
        return Result<bool>::Success(true);
    }

    if (kind == ValueKind::SoftObjectReference) {
        auto asset = AssetId::TryParse(text);
        if (asset) {
            out.guid = asset.Value().value;
            return Result<bool>::Success(true);
        }
    }

    return Fail(failure, TextDiagnosticCode::Unsupported, "references need a resolver");
}

// TypeId 텍스트(36자 소문자 하이픈 — Guid 표기, 또는 rocksids의 32자 소문자 16진)인지 보고 읽는다.
bool ParseTypeIdText(StringView text, TypeId& out) noexcept {
    const usize size = text.ByteCount();
    if (size != rocks::detail::GuidTextBytes && size != 32) {
        return false;
    }

    for (usize index = 0; index < size; ++index) {
        const char c = text.Data()[index];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex && !(c == '-' && size == rocks::detail::GuidTextBytes)) {
            return false;
        }
    }

    auto parsed = TryParseGuid(text);
    if (!parsed) {
        return false;
    }

    out = parsed.Value();
    return true;
}

// ClassRef: `none`(대소문자 무시)은 null, TypeId 텍스트, 그 밖은 타입 도구 이름. 등록·클래스·Base
// 파생을 본다(Deprecated 클래스는 여기서 거부하지 않는다 — 같은 값 재대입이 무동작이어야 하므로
// 쓰기 단계가 지금 값과 대조해 정한다, 9.2절).
Result<bool> ParseClass(TextCodecContext& context, const TextScalarType& type, StringView text,
                        FieldValue& out, ScalarFailure& failure) noexcept {
    out.guid = Guid{};
    if (TextEqualsIgnoringCase(text, "none")) {
        return Result<bool>::Success(true);
    }

    TypeRegistry& types = context.Types();
    TypeId id{};
    if (!ParseTypeIdText(text, id)) {
        const Span<const char> name(text.Data(), text.ByteCount());
        if (!IsTypeToolName(name)) {
            return Fail(failure, TextDiagnosticCode::InvalidValue, "not a type name or TypeId");
        }

        auto named = types.AcquireByToolName(name);
        if (!named) {
            if (named.Error() == ErrorCode::NotFound) {
                return Fail(failure, TextDiagnosticCode::UnknownType, "unknown type");
            }

            return Result<bool>::Failure(named.Error());
        }

        id = named.Value().Schema().id;
    }

    auto checked =
        rocks::detail::CheckClassReferenceLease(types.Acquire(id), type.type.type, nullptr);
    if (!checked) {
        switch (checked.Error()) {
        case ErrorCode::NotFound:
            return Fail(failure, TextDiagnosticCode::UnknownType, "unknown type");
        case ErrorCode::InvalidArgument:
            return Fail(failure, TextDiagnosticCode::InvalidValue,
                        "not a class derived from the base class");
        default:
            return Result<bool>::Failure(checked.Error());
        }
    }

    out.guid = id;
    return Result<bool>::Success(true);
}

} // namespace

// value를 숨김이 아닌 열거자로 나타낼 수 없는지 본다(숨김 열거자를 썼다). 보통 enum은 그 값의
// 열거자가 Hidden이면, Flags enum은 숨김이 아닌 열거자의 OR로 덮을 수 없는 비트가 남으면 참이다.
bool UsesHiddenEnumerator(const EnumSchema& schema, int64 value) noexcept {
    if (!HasFlag(schema.flags, EnumFlags::Flags)) {
        const EnumeratorSchema* found = rocks::detail::FindEnumeratorByValue(schema, value);
        return found && HasFlag(found->flags, EnumeratorFlags::Hidden);
    }

    int64 remaining = value;
    for (usize index = 0; index < schema.values.Size(); ++index) {
        const EnumeratorSchema& enumerator = schema.values[index];
        if (!HasFlag(enumerator.flags, EnumeratorFlags::Hidden) && enumerator.value != 0 &&
            (enumerator.value & ~value) == 0) {
            remaining &= ~enumerator.value;
        }
    }

    return remaining != 0;
}

bool TextEqualsIgnoringCase(StringView text, StringView word) noexcept {
    return EqualsIgnoringAsciiCase(Span<const char>(text.Data(), text.ByteCount()),
                                   Span<const char>(word.Data(), word.ByteCount()));
}

Result<bool> ParseScalarCore(TextCodecContext& context, const TextScalarType& type, StringView text,
                             FieldValue& out, ScalarFailure& failure) noexcept {
    const ValueKind kind = type.type.kind;
    out.kind = kind;
    out.present = true;
    out.type = TypeId{};
    switch (kind) {
    case ValueKind::Bool:
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
    case ValueKind::Enum:
    case ValueKind::Guid:
    case ValueKind::AssetId:
    case ValueKind::Digest256: {
        auto parsed = ParseScalarText(type.type, type.enumSchema, text);
        if (!parsed) {
            if (parsed.Error() == ErrorCode::Overflow) {
                return Fail(failure, TextDiagnosticCode::OutOfRange, "integer out of range");
            }

            return Fail(failure, TextDiagnosticCode::InvalidValue,
                        kind == ValueKind::Enum ? StringView("unknown enumerator")
                                                : StringView("invalid value text"));
        }

        const PathKeyValue& value = parsed.Value();
        if (!FitsWidth(kind, value.unsignedInteger, value.signedInteger)) {
            return Fail(failure, TextDiagnosticCode::OutOfRange, "integer out of range");
        }

        if (kind == ValueKind::Enum) {
            if (UsesHiddenEnumerator(*type.enumSchema, value.signedInteger)) {
                return Fail(failure, TextDiagnosticCode::HiddenEnumerator,
                            "hidden enumerator is not accepted as input");
            }

            out.type = type.type.type;
        }

        out.boolean = value.boolean;
        out.unsignedInteger = value.unsignedInteger;
        out.signedInteger = value.signedInteger;
        out.guid = value.guid;
        out.digest = value.digest;
        return Result<bool>::Success(true);
    }
    case ValueKind::Float32:
    case ValueKind::Float64:
        return ParseReal(kind, text, out, failure);
    case ValueKind::Utf8String: {
        auto assigned = out.string.TryAssign(text);
        if (!assigned) {
            if (assigned.Error() == ErrorCode::InvalidArgument) {
                return Fail(failure, TextDiagnosticCode::InvalidValue, "not valid UTF-8");
            }

            return Result<bool>::Failure(assigned.Error());
        }

        return Result<bool>::Success(true);
    }
    case ValueKind::Name:
        return ParseName(context, text, out, failure);
    case ValueKind::ObjectReference:
    case ValueKind::WeakObjectReference:
    case ValueKind::SoftObjectReference:
        return ParseReference(context, type, text, out, failure);
    case ValueKind::ClassReference:
        return ParseClass(context, type, text, out, failure);
    default:
        return Fail(failure, TextDiagnosticCode::Unsupported, "value kind has no text form");
    }
}

} // namespace detail

TextScalarType SlotValueType(const TextSlot& slot) noexcept {
    TextScalarType type;
    if (!slot.field) {
        return type;
    }

    const FieldSchema& field = *slot.field;
    type.enumSchema = slot.valueEnum;
    if (slot.part == TextSlotPart::Field && !ValueKindIsContainer(field.value.kind) &&
        field.value.kind != ValueKind::Optional) {
        type.type = field.value;
        return type;
    }

    type.type = field.element;
    return type;
}

TextScalarType SlotKeyType(const TextSlot& slot) noexcept {
    TextScalarType type;
    if (!slot.field) {
        return type;
    }

    if (slot.field->value.kind == ValueKind::Map) {
        type.type = slot.field->key;
        type.enumSchema = slot.keyEnum;
    } else if (slot.field->value.kind == ValueKind::Set) {
        type.type = slot.field->element;
        type.enumSchema = slot.valueEnum;
    }

    return type;
}

Result<bool> ParseTextScalar(TextCodecContext& context, const TextScalarType& type, StringView text,
                             TextSource source, StringView pathText, FieldValue& out) noexcept {
    if (auto started = context.Start(); !started) {
        return Result<bool>::Failure(started.Error());
    }

    if (auto charged = context.ChargeInput(text.ByteCount()); !charged) {
        return Result<bool>::Failure(charged.Error());
    }

    detail::ScalarFailure failure;
    auto parsed = detail::ParseScalarCore(context, type, text, out, failure);
    if (!parsed || parsed.Value()) {
        return parsed;
    }

    return detail::Report(context, failure.code, source, pathText, failure.message);
}

Result<void> FormatTextScalar(TextCodecContext& context, const TextScalarType& type,
                              const FieldValue& value, StringBuilderBase& out) noexcept {
    const ValueKind kind = type.type.kind;
    if (value.kind != kind) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    switch (kind) {
    case ValueKind::Bool:
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
    case ValueKind::Enum:
    case ValueKind::Guid:
    case ValueKind::AssetId:
    case ValueKind::Digest256: {
        PathKeyValue scalar;
        scalar.kind = kind;
        scalar.boolean = value.boolean;
        scalar.unsignedInteger = value.unsignedInteger;
        scalar.signedInteger = value.signedInteger;
        scalar.guid = value.guid;
        scalar.digest = value.digest;
        return AppendScalarText(type.type, type.enumSchema, scalar, out);
    }
    case ValueKind::Float32:
        // NaN은 부호·payload 없이 `nan` 하나로 쓴다(비교 값이 아니다). 무한대는 "{}"가 inf·-inf다.
        return value.float32 != value.float32 ? out.TryAppend("nan")
                                              : out.TryAppendFormat("{}", value.float32);
    case ValueKind::Float64:
        return value.float64 != value.float64 ? out.TryAppend("nan")
                                              : out.TryAppendFormat("{}", value.float64);
    case ValueKind::Utf8String:
        return out.TryAppend(value.string.View());
    case ValueKind::Name: {
        auto display =
            AcquireNameFieldText(context.Options().names, value.name, context.GetAllocator());
        if (!display) {
            return Result<void>::Failure(display.Error());
        }

        const StringView text = display.Value().View();
        return out.TryAppend(text.IsEmpty() ? StringView("None") : text);
    }
    case ValueKind::ObjectReference:
    case ValueKind::WeakObjectReference:
    case ValueKind::SoftObjectReference: {
        const bool soft = kind == ValueKind::SoftObjectReference;
        if (soft ? (value.guid == Guid{} && value.string.IsEmpty()) : value.reference.IsNull()) {
            return out.TryAppend("none");
        }

        if (TextReferenceResolver* resolver = context.Options().resolver) {
            StringBuilder<128> text(context.GetAllocator());
            auto formatted = resolver->Format(detail::ReferenceKindOf(kind), value, text);
            if (formatted && !text.HasFailed()) {
                return out.TryAppend(text.View());
            }

            if (!formatted && formatted.Error() == ErrorCode::OutOfMemory) {
                return formatted;
            }
        }

        if (soft && value.string.IsEmpty()) {
            return out.TryAppendFormat("{}", value.guid);
        }

        return Result<void>::Failure(ErrorCode::Unsupported);
    }
    case ValueKind::ClassReference: {
        if (value.guid == Guid{}) {
            return out.TryAppend("none");
        }

        // 등록된 클래스는 타입 도구 이름, 이름이 없거나 등록되지 않은 값은 36자 TypeId다.
        auto lease = context.Types().Acquire(value.guid);
        if (lease && lease.Value().Schema().toolName.Size() != 0) {
            const Span<const char> name = lease.Value().Schema().toolName;
            return out.TryAppend(StringView(name.Data(), name.Size()));
        }

        return out.TryAppendFormat("{}", value.guid);
    }
    default:
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
}

} // namespace rocks::reflection_text
