#include "TextInternal.h"

#include <Object/NameField.h>
#include <Object/ValueText.h>

// 필드 경로 텍스트와 그 자리다(리플렉션 B9i3b — Docs/Design/ReflectionExtensions.ko.md 1.5·10.4절).
// 경로 문법(이름·index·key 따옴표 escape `\\`·`\"`·`\n`·`\r`·`\t`)은 ObjectRuntime의
// ParsePropertyPath·FormatPropertyPath 하나를 쓰고, Soft·ClassRef key는 문맥의 KeyHook이 값 표기
// 규칙으로 다룬다. 용도 판정(TextUse)은 경로 단계 플래그의 합으로 한다.
namespace rocks::reflection_text {

namespace {

// 경로 단계가 struct 값으로 끝나 다음 단계(`.`)를 받을 수 있는지다(ParsePropertyPath와 같은 규칙).
bool IsStructStep(const FieldSchema& field, PathSelector selector) noexcept {
    switch (selector) {
    case PathSelector::Index:
        return ValueKindTakesIndex(field.value.kind) && field.element.kind == ValueKind::Struct;
    case PathSelector::Key:
        return field.value.kind == ValueKind::Map && field.element.kind == ValueKind::Struct;
    case PathSelector::None:
        return field.value.kind == ValueKind::Struct ||
               (field.value.kind == ValueKind::Optional && field.element.kind == ValueKind::Struct);
    }

    return false;
}

// key 단계 하나의 입력 판정(Name key의 내용 규칙·숨김 열거자)이다. 통과하면 true다.
struct KeyCheck {
    TextDiagnosticCode code = TextDiagnosticCode::InvalidValue;
    StringView message;
};

Result<bool> CheckKey(TextCodecContext& context, const EnumSchema* keyEnum, const PathKey& key,
                      KeyCheck& failure) noexcept {
    auto value = ReadPathKey(key);
    if (!value) {
        return Result<bool>::Failure(value.Error());
    }

    if (key.kind == ValueKind::Name) {
        auto valid = ValidateNameFieldText(value.Value().text, context.GetAllocator());
        if (!valid) {
            if (valid.Error() != ErrorCode::InvalidArgument) {
                return Result<bool>::Failure(valid.Error());
            }

            failure.message = "Name key must be NFC and a valid Name";
            return Result<bool>::Success(false);
        }
    }

    if (key.kind == ValueKind::Enum && keyEnum &&
        detail::UsesHiddenEnumerator(*keyEnum, value.Value().signedInteger)) {
        failure.code = TextDiagnosticCode::HiddenEnumerator;
        failure.message = "hidden enumerator is not accepted as input";
        return Result<bool>::Success(false);
    }

    return Result<bool>::Success(true);
}

// 단계가 InstancedStruct 값으로 끝나 다음 단계가 지금 담긴 struct의 멤버인지다(리플렉션 B9j).
bool IsInstancedStep(const FieldSchema& field, PathSelector selector) noexcept {
    switch (selector) {
    case PathSelector::Index:
        return ValueKindTakesIndex(field.value.kind) &&
               field.element.kind == ValueKind::InstancedStruct;
    case PathSelector::Key:
        return field.value.kind == ValueKind::Map &&
               field.element.kind == ValueKind::InstancedStruct;
    case PathSelector::None:
        return field.value.kind == ValueKind::InstancedStruct;
    }

    return false;
}

// DescribeTextPath의 본체다. checkKeys가 있으면 key 단계마다 CheckKey를 부른다. instanced가 있으면
// InstancedStruct 뒤 멤버 단계를 그 자리의 지금 담긴 struct로 푼다(리플렉션 B9j).
Result<bool> Describe(const TypeLease& type, PropertyPath path, TextSlot& out,
                      TextCodecContext* checkKeys, KeyCheck* failure,
                      PathInstancedTypeResolver* instanced = nullptr) noexcept {
    if (!type.IsValid() || type.Kind() == TypeKind::Enum || path.steps.Size() == 0) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    detail::SchemaScope scope;
    if (type.Kind() == TypeKind::Struct) {
        scope.view = type.Struct();
    } else {
        scope.lease = &type;
    }

    TextSlot slot;
    // InstancedStruct 뒤 멤버 단계의 담긴 struct lease다(값이 같은 레코드를 붙잡으므로 slot의
    // view는 그 값이 사는 동안 유효하다).
    TypeLease content;
    for (usize index = 0; index < path.steps.Size(); ++index) {
        const PathStep& step = path.steps[index];
        const usize fieldIndex = scope.FieldIndex(step.fieldId);
        if (fieldIndex == IndexNone) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }

        const FieldSchema& field = scope.Schema().fields[fieldIndex];
        slot.field = &field;
        slot.valueEnum = scope.FieldEnum(fieldIndex);
        slot.keyEnum = scope.FieldKeyEnum(fieldIndex);
        slot.structView = scope.FieldStruct(fieldIndex);
        slot.flags |= field.flags;
        switch (step.selector) {
        case PathSelector::None:
            slot.part = TextSlotPart::Field;
            break;
        case PathSelector::Index:
            if (!ValueKindTakesIndex(field.value.kind)) {
                return Result<bool>::Failure(ErrorCode::InvalidArgument);
            }

            slot.part = TextSlotPart::Element;
            break;
        case PathSelector::Key: {
            if (field.value.kind != ValueKind::Map && field.value.kind != ValueKind::Set) {
                return Result<bool>::Failure(ErrorCode::InvalidArgument);
            }

            if (step.index >= path.keys.Size()) {
                return Result<bool>::Failure(ErrorCode::InvalidArgument);
            }

            slot.part = field.value.kind == ValueKind::Map ? TextSlotPart::MapValue
                                                           : TextSlotPart::SetElement;
            if (checkKeys) {
                const EnumSchema* keyEnum =
                    field.value.kind == ValueKind::Map ? slot.keyEnum : slot.valueEnum;
                auto checked = CheckKey(*checkKeys, keyEnum, path.keys[step.index], *failure);
                if (!checked || !checked.Value()) {
                    return checked;
                }
            }

            break;
        }
        default:
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }

        if (index + 1 < path.steps.Size() && instanced && IsInstancedStep(field, step.selector)) {
            TypeLease resolved;
            if (auto queried = instanced->InstancedType(
                    PropertyPath{Span<const PathStep>(path.steps.Data(), index + 1), path.keys},
                    resolved);
                !queried) {
                return Result<bool>::Failure(queried.Error());
            }

            if (!resolved.IsValid()) {
                return Result<bool>::Failure(ErrorCode::NotFound);
            }

            content = Move(resolved);
            scope.lease = nullptr;
            scope.view = content.Struct();
            continue;
        }

        if (index + 1 < path.steps.Size()) {
            if (!IsStructStep(field, step.selector) || slot.structView.IsEmpty()) {
                return Result<bool>::Failure(ErrorCode::InvalidArgument);
            }

            scope.lease = nullptr;
            scope.view = slot.structView;
        }
    }

    out = slot;
    return Result<bool>::Success(true);
}

StringView PathFailureMessage(PropertyPathError reason) noexcept {
    switch (reason) {
    case PropertyPathError::UnknownProperty:
        return "unknown property";
    case PropertyPathError::Syntax:
        return "path syntax error";
    case PropertyPathError::TooDeep:
        return "path has too many steps";
    case PropertyPathError::InvalidKey:
        return "invalid key";
    case PropertyPathError::NotArray:
        return "index or key on a field that has none";
    case PropertyPathError::NotStruct:
        return "member of a value that is not a struct";
    case PropertyPathError::InstancedEmpty:
        return "member of an empty instanced struct";
    default:
        return "invalid path";
    }
}

} // namespace

Result<void> DescribeTextPath(const TypeLease& type, PropertyPath path, TextSlot& out,
                              PathInstancedTypeResolver* instanced) noexcept {
    auto described = Describe(type, path, out, nullptr, nullptr, instanced);
    if (!described) {
        return Result<void>::Failure(described.Error());
    }

    return Result<void>::Success();
}

Result<bool> ResolveTextPath(TextCodecContext& context, const TypeLease& type, StringView text,
                             TextSource source, TextPath& out) noexcept {
    if (auto started = context.Start(); !started) {
        return Result<bool>::Failure(started.Error());
    }

    auto inputFlags = TextInputExcludedFlags(context.Options().use);
    if (!inputFlags) {
        return Result<bool>::Failure(inputFlags.Error());
    }

    if (!type.IsValid() || type.Kind() == TypeKind::Enum) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto charged = context.ChargeInput(text.ByteCount()); !charged) {
        return Result<bool>::Failure(charged.Error());
    }

    PropertyPathFailure failure;
    context.TakeKeyFailure();
    auto parsed = ParsePropertyPath(type, text, out.path, &failure, &context.KeyHook(),
                                    context.InstancedTypes());
    const TextDiagnosticCode keyFailure = context.TakeKeyFailure();
    if (!parsed) {
        if (parsed.Error() == ErrorCode::OutOfMemory) {
            return Result<bool>::Failure(parsed.Error());
        }

        TextDiagnosticCode code = TextDiagnosticCode::InvalidValue;
        if (failure.reason == PropertyPathError::UnknownProperty) {
            code = TextDiagnosticCode::UnknownProperty;
        } else if (failure.reason == PropertyPathError::InvalidKey) {
            code = parsed.Error() == ErrorCode::Unsupported ? TextDiagnosticCode::Unsupported
                                                            : keyFailure;
        }

        return detail::Report(context, code, source, text, PathFailureMessage(failure.reason));
    }

    KeyCheck keyCheck;
    auto described =
        Describe(type, out.path.View(), out.slot, &context, &keyCheck, context.InstancedTypes());
    if (!described) {
        return described;
    }

    if (!described.Value()) {
        return detail::Report(context, keyCheck.code, source, text, keyCheck.message);
    }

    if (ClassifyTextInput(context.Options().use, out.slot.flags) == TextFieldInput::Excluded) {
        return detail::Report(context, TextDiagnosticCode::ExcludedProperty, source, text,
                              "property is excluded from text input");
    }

    return Result<bool>::Success(true);
}

Result<bool> FormatTextPath(TextCodecContext& context, const TypeLease& type, PropertyPath path,
                            String& out) noexcept {
    if (auto started = context.Start(); !started) {
        return Result<bool>::Failure(started.Error());
    }

    TextSlot slot;
    PathInstancedTypeResolver* const instanced = context.InstancedTypes();
    if (auto described = DescribeTextPath(type, path, slot, instanced); !described) {
        return Result<bool>::Failure(described.Error());
    }

    if (IsTextOutputExcluded(context.Options().use, slot.flags)) {
        return Result<bool>::Success(false);
    }

    String text(out.GetAllocator());
    if (auto formatted = FormatPropertyPath(type, path, text, &context.KeyHook(), instanced);
        !formatted) {
        return Result<bool>::Failure(formatted.Error());
    }

    if (auto appended = out.TryAppend(text.View()); !appended) {
        return Result<bool>::Failure(appended.Error());
    }

    return Result<bool>::Success(true);
}

bool SplitTextAssignment(StringView text, StringView& path, StringView& value) noexcept {
    const char* data = text.Data();
    const usize size = text.ByteCount();
    usize depth = 0;
    bool quoted = false;
    for (usize index = 0; index < size; ++index) {
        const char c = data[index];
        if (quoted) {
            if (c == '\\') {
                ++index;
            } else if (c == '"') {
                quoted = false;
            }

            continue;
        }

        if (c == '"' && depth != 0) {
            quoted = true;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']' && depth != 0) {
            --depth;
        } else if (c == '=' && depth == 0) {
            path = StringView(data, index);
            value = StringView(data + index + 1, size - index - 1);
            return true;
        }
    }

    return false;
}

} // namespace rocks::reflection_text
