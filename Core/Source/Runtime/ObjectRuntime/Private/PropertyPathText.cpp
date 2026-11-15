#include <Object/PropertyPath.h>

// 문자열 경로(ParsePropertyPath·FormatPropertyPath)다. 도구 이름 조회는 대소문자 무시이고 원소
// index는 정규 10진이다(Docs/Design/ReflectionFoundations.ko.md 7.1절).
namespace rocks {

namespace {

bool IsIdentifierStart(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool IsIdentifierPart(char c) noexcept {
    return IsIdentifierStart(c) || (c >= '0' && c <= '9');
}

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// 경로가 struct 값으로 끝나는 단계인지다(다음 단계 `.`를 받을 수 있다 — index 없는 배열은 아니다).
// Optional<값 struct> 필드는 그 내부 struct로 들어간다(리플렉션 B9e — set 여부는 경로 연산이 본다).
bool IsStructStep(const FieldSchema& field, bool indexed) noexcept {
    if (indexed) {
        return field.value.kind == ValueKind::Array && field.element.kind == ValueKind::Struct;
    }

    return field.value.kind == ValueKind::Struct ||
           (field.value.kind == ValueKind::Optional && field.element.kind == ValueKind::Struct);
}

usize DecimalDigits(uint32 value) noexcept {
    usize digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }

    return digits;
}

} // namespace

Result<void> ParsePropertyPath(const TypeLease& type, StringView text, PropertyPathBuffer& out,
                               PropertyPathFailure* failure) noexcept {
    const auto fail = [failure](ErrorCode code, PropertyPathError reason, usize step,
                                usize offset) noexcept {
        if (failure) {
            *failure = {reason, step, offset};
        }

        return Result<void>::Failure(code);
    };

    if (failure) {
        *failure = {};
    }

    if (!type.IsValid() || type.Kind() == TypeKind::Enum) {
        return fail(ErrorCode::InvalidArgument, PropertyPathError::None, 0, 0);
    }

    const char* data = text.Data();
    const usize size = text.ByteCount();
    PropertyPathBuffer parsed;
    StructView scope;
    usize position = 0;
    while (true) {
        // 이름(ASCII 식별자)을 읽어 지금 범위(클래스·struct lease 스키마 또는 struct view)에서
        // 찾는다.
        const usize nameStart = position;
        if (position >= size || !IsIdentifierStart(data[position])) {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, parsed.count,
                        position);
        }

        while (position < size && IsIdentifierPart(data[position])) {
            ++position;
        }

        const Span<const char> name(data + nameStart, position - nameStart);
        const Span<const FieldSchema> fields =
            parsed.count == 0 ? type.Schema().fields : scope.Schema().fields;
        usize found = TypeLease::NoField;
        for (usize index = 0; index < fields.Size(); ++index) {
            // Opaque 필드는 경로로 닿을 수 없으므로 찾지 않는다.
            if (fields[index].value.kind != ValueKind::Opaque &&
                EqualsIgnoringAsciiCase(EffectiveToolName(fields[index]), name)) {
                found = index;
                break;
            }
        }

        if (found == TypeLease::NoField) {
            return fail(ErrorCode::NotFound, PropertyPathError::UnknownProperty, parsed.count,
                        nameStart);
        }

        const FieldSchema& field = fields[found];
        const StructView target =
            parsed.count == 0 ? type.FieldStruct(found) : scope.FieldStruct(found);
        PathStep& step = parsed.steps[parsed.count];
        step = PathStep{field.id, 0, false};
        const usize stepIndex = parsed.count++;
        if (position < size && data[position] == '[') {
            if (field.value.kind != ValueKind::Array) {
                return fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray, stepIndex,
                            position);
            }

            // 정규 10진 index: `0` 하나 또는 0이 아닌 숫자로 시작, uint32 범위.
            ++position;
            const usize digitsStart = position;
            uint64 value = 0;
            while (position < size && IsDigit(data[position])) {
                value = value * 10 + static_cast<uint64>(data[position] - '0');
                if (value > NumericLimits<uint32>::Max()) {
                    return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, stepIndex,
                                digitsStart);
                }

                ++position;
            }

            if (position == digitsStart ||
                (data[digitsStart] == '0' && position - digitsStart > 1)) {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, stepIndex,
                            digitsStart);
            }

            if (position >= size || data[position] != ']') {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, stepIndex,
                            position);
            }

            ++position;
            step.index = static_cast<uint32>(value);
            step.indexed = true;
            // 배열 원소는 배열일 수 없다(중첩 배열 없음).
            if (position < size && data[position] == '[') {
                return fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray, stepIndex,
                            position);
            }
        }

        if (position == size) {
            break;
        }

        if (data[position] != '.') {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, parsed.count,
                        position);
        }

        if (parsed.count == MaxPropertyPathSteps) {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::TooDeep, parsed.count,
                        position);
        }

        if (!IsStructStep(field, step.indexed) || target.IsEmpty()) {
            return fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct, stepIndex, position);
        }

        ++position;
        scope = target;
    }

    out = parsed;
    return Result<void>::Success();
}

Result<void> FormatPropertyPath(const TypeLease& type, PropertyPath path, String& out) noexcept {
    const Span<const PathStep> steps = path.steps;
    if (!type.IsValid() || type.Kind() == TypeKind::Enum || steps.Size() == 0 ||
        steps.Size() > MaxPropertyPathSteps) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Span<const char> names[MaxPropertyPathSteps];
    usize bytes = 0;
    StructView scope;
    for (usize index = 0; index < steps.Size(); ++index) {
        const PathStep& step = steps[index];
        if (!IsCanonicalPathStep(step)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const usize position =
            index == 0 ? type.FieldIndex(step.fieldId) : scope.FieldIndex(step.fieldId);
        if (position == TypeLease::NoField) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        const FieldSchema& field =
            index == 0 ? type.Schema().fields[position] : scope.Schema().fields[position];
        if (field.value.kind == ValueKind::Opaque) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (step.indexed && field.value.kind != ValueKind::Array) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        const StructView target =
            index == 0 ? type.FieldStruct(position) : scope.FieldStruct(position);
        if (index + 1 < steps.Size() && (!IsStructStep(field, step.indexed) || target.IsEmpty())) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        names[index] = EffectiveToolName(field);
        bytes += names[index].Size() + (index != 0 ? 1 : 0) +
                 (step.indexed ? 2 + DecimalDigits(step.index) : 0);
        scope = target;
    }

    String result(out.GetAllocator());
    if (auto reserved = result.TryReserve(bytes); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < steps.Size(); ++index) {
        if (index != 0 && !result.TryAppend(".")) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (!result.TryAppend(StringView(names[index].Data(), names[index].Size()))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (steps[index].indexed) {
            char digits[12];
            usize count = 0;
            uint32 value = steps[index].index;
            do {
                digits[sizeof(digits) - 1 - count++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);

            if (!result.TryAppend("[") ||
                !result.TryAppend(StringView(digits + sizeof(digits) - count, count)) ||
                !result.TryAppend("]")) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    out.Swap(result);
    return Result<void>::Success();
}

} // namespace rocks
