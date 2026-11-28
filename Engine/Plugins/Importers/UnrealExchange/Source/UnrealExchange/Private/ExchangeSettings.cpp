#include <Base/NumericLimits.h>
#include <Core/Guid.h>
#include <Core/Text/StringBuilder.h>
#include <UnrealExchange/ExchangeSettings.h>

// UE 설정 덤프 번역이다(Docs/Design/UnrealExchange 10.6절 8번).

namespace rocks {

namespace exchange_settings {

// bool 접두 b를 떼는 키의 최소 길이다(b + 대문자 하나).
constexpr usize BoolPrefixMinBytes = 2;
// UE FGuid 칸 수와 칸 이름이다.
constexpr usize GuidParts = 4;
constexpr StringView GuidKeys[GuidParts] = {"A", "B", "C", "D"};
// Guid 문자열 길이(36자)와 uint32 칸 하나의 비트 수다.
constexpr usize GuidTextBytes = 36;
constexpr uint32 WordBits = 32;
// PerPlatformFloat의 UE 멤버다.
constexpr StringView PerPlatformKeys[] = {"Default", "PerPlatform"};

struct TranslateState {
    const ExchangeSettingInput& input;
    JsonWriter& writer;
    Array<ExchangeSettingWarning>& warnings;
};

Result<void> Invalid() noexcept {
    return Result<void>::Failure(ErrorCode::CorruptData);
}

bool IsUpper(char value) noexcept {
    return value >= 'A' && value <= 'Z';
}

bool IsDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

// UE bool 프로퍼티 이름의 접두 b를 뗀 키다(bFlipGreenChannel → FlipGreenChannel).
StringView RocksKey(StringView key) noexcept {
    if (key.ByteCount() >= BoolPrefixMinBytes && key.Data()[0] == 'b' && IsUpper(key.Data()[1])) {
        return key.SliceBytes(1, key.ByteCount() - 1);
    }

    return key;
}

// UE enum 값 이름의 접두(첫 `_`까지, 대문자·숫자만, 뒤가 비지 않음)를 뗀 이름이다.
StringView EnumName(StringView value) noexcept {
    for (usize index = 0; index < value.ByteCount(); ++index) {
        const char at = value.Data()[index];
        if (at == '_') {
            if (index == 0 || index + 1 == value.ByteCount()) {
                return value;
            }

            return value.SliceBytes(index + 1, value.ByteCount() - index - 1);
        }

        if (!IsUpper(at) && !IsDigit(at)) {
            return value;
        }
    }

    return value;
}

Result<void> Warn(TranslateState& state, uint64 detail, StringView key) noexcept {
    ExchangeSettingWarning warning;
    warning.detail = detail;
    warning.key = key;
    return state.warnings.TryPushBack(warning);
}

// 값을 통째로 옮긴다(컨테이너는 재귀 — 깊이는 문서 깊이 상한 안이다).
Result<void> CopyValue(JsonWriter& writer, JsonValue value) noexcept {
    if (value.IsArray()) {
        if (auto begun = writer.BeginArray(); !begun) {
            return begun;
        }

        for (usize index = 0; index < value.Size(); ++index) {
            if (auto copied = CopyValue(writer, value.At(index)); !copied) {
                return copied;
            }
        }

        return writer.EndArray();
    }

    if (value.IsObject()) {
        if (auto begun = writer.BeginObject(); !begun) {
            return begun;
        }

        for (usize index = 0; index < value.Size(); ++index) {
            if (auto keyed = writer.Key(value.KeyAt(index)); !keyed) {
                return keyed;
            }

            if (auto copied = CopyValue(writer, value.ValueAt(index)); !copied) {
                return copied;
            }
        }

        return writer.EndObject();
    }

    return writer.Value(value);
}

Result<void> WriteEnum(JsonWriter& writer, JsonValue value) noexcept {
    if (value.IsString()) {
        return writer.String(EnumName(value.GetString()));
    }

    // enum에 없는 값은 정수로 온다(11.7절 13번) — ImportJson이 거부한다.
    if (value.IsNumber()) {
        return writer.Value(value);
    }

    return Invalid();
}

// UE FGuid DigitsWithHyphens(A - B 상위 - B 하위 - C 상위 - C 하위 D)는 Rocks Guid{A·B, C·D}의 36자
// 표준 표기(8-4-4-4-12)와 같은 순서다.
Result<void> WriteGuid(JsonWriter& writer, JsonValue value, Allocator& allocator) noexcept {
    if (!value.IsObject() || value.Size() != GuidParts) {
        return Invalid();
    }

    uint32 parts[GuidParts] = {};
    for (usize index = 0; index < GuidParts; ++index) {
        const JsonValue part = value.Find(GuidKeys[index]);
        if (!part.IsValid()) {
            return Invalid();
        }

        auto number = part.TryGetInt64();
        if (!number || number.Value() < NumericLimits<int32>::Min() ||
            number.Value() > NumericLimits<int32>::Max()) {
            return Invalid();
        }

        parts[index] = static_cast<uint32>(static_cast<int32>(number.Value()));
    }

    const Guid guid{(static_cast<uint64>(parts[0]) << WordBits) | parts[1],
                    (static_cast<uint64>(parts[2]) << WordBits) | parts[3]};
    StringBuilder<GuidTextBytes + 1> text(allocator);
    if (auto formatted = text.TryAppendFormat("{}", guid); !formatted) {
        return formatted;
    }

    return writer.String(text.View());
}

// 참조 하나의 resolver 텍스트(objectPath)다. null·references에 없는 대상이면 빈 view이고 found는
// 거짓이다.
Result<void> ReadReference(const TranslateState& state, JsonValue value, StringView& out,
                           bool& found) noexcept {
    found = false;
    if (value.IsNull()) {
        return Result<void>::Success();
    }

    const JsonValue path = value.Find("objectPath");
    if (!value.IsObject() || value.Size() != 1 || !path.IsValid() || !path.IsString()) {
        return Invalid();
    }

    out = path.GetString();
    found = state.input.references != nullptr && state.input.references->Contains(out);
    return Result<void>::Success();
}

Result<void> WriteReference(TranslateState& state, JsonValue value) noexcept {
    StringView path;
    bool found = false;
    if (auto read = ReadReference(state, value, path, found); !read) {
        return read;
    }

    return found ? state.writer.String(path) : state.writer.Null();
}

Result<void> WriteReferenceArray(TranslateState& state, JsonValue value) noexcept {
    if (!value.IsArray()) {
        return Invalid();
    }

    if (auto begun = state.writer.BeginArray(); !begun) {
        return begun;
    }

    for (usize index = 0; index < value.Size(); ++index) {
        StringView path;
        bool found = false;
        if (auto read = ReadReference(state, value.At(index), path, found); !read) {
            return read;
        }

        if (found) {
            if (auto written = state.writer.String(path); !written) {
                return written;
            }
        }
    }

    return state.writer.EndArray();
}

Result<void> WritePerPlatform(TranslateState& state, JsonValue value, uint64 detail,
                              StringView key) noexcept {
    const JsonValue fallback = value.Find("Default");
    const JsonValue platforms = value.Find("PerPlatform");
    if (!value.IsObject() || !fallback.IsValid() || !fallback.IsNumber()) {
        return Invalid();
    }

    if (platforms.IsValid() && !platforms.IsArray()) {
        return Invalid();
    }

    // UE 플랫폼 이름과 TargetProfile 이름의 대응은 후속이다(10.6절 8번). 모르는 멤버도 경고다.
    bool unknown = false;
    for (usize index = 0; index < value.Size(); ++index) {
        unknown = unknown || (value.KeyAt(index) != PerPlatformKeys[0] &&
                              value.KeyAt(index) != PerPlatformKeys[1]);
    }

    if ((platforms.IsValid() && platforms.Size() != 0) || unknown) {
        if (auto warned = Warn(state, detail, key); !warned) {
            return warned;
        }
    }

    if (auto begun = state.writer.BeginObject(); !begun) {
        return begun;
    }

    if (auto keyed = state.writer.Key("defaultValue"); !keyed) {
        return keyed;
    }

    if (auto written = state.writer.Value(fallback); !written) {
        return written;
    }

    return state.writer.EndObject();
}

Result<bool> IsDefault(TranslateState& state, JsonValue value, StringView text) noexcept {
    JsonDocument document(state.warnings.GetAllocator());
    const JsonError parsed = document.Parse(text);
    if (parsed.code == JsonErrorCode::OutOfMemory) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    if (!parsed.Succeeded()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<bool>::Success(JsonEquals(value, document.Root()));
}

const ExchangeSettingRow* FindRow(Span<const ExchangeSettingRow> rows, StringView key,
                                  usize& index) noexcept {
    for (index = 0; index < rows.Size(); ++index) {
        if (rows[index].ueKey == key) {
            return &rows[index];
        }
    }

    return nullptr;
}

Result<void> TranslateObject(TranslateState& state, JsonValue object,
                             Span<const ExchangeSettingRow> rows, bool top,
                             uint64 parentDetail) noexcept;

// EnumMap 행에서 value(UE 값 이름)의 멤버 행이다(문자열이 아니거나 표에 없으면 null).
const ExchangeSettingRow* FindEnumValue(const ExchangeSettingRow& row, JsonValue value) noexcept {
    if (!value.IsString()) {
        return nullptr;
    }

    const Span<const ExchangeSettingRow> members(row.members, row.memberCount);
    for (const ExchangeSettingRow& mapped : members) {
        if (mapped.ueKey == value.GetString()) {
            return &mapped;
        }
    }

    return nullptr;
}

// EnumMap 행의 값이 Rocks에 없는 값(빈 rocksKey 멤버)인가.
bool UnmappedEnum(const ExchangeSettingRow& row, JsonValue value) noexcept {
    if (row.kind != ExchangeSettingKind::EnumMap) {
        return false;
    }

    const ExchangeSettingRow* mapped = FindEnumValue(row, value);
    return mapped != nullptr && mapped->rocksKey.IsEmpty();
}

// 행 하나의 값을 쓴다(키는 이미 썼다).
Result<void> WriteRowValue(TranslateState& state, const ExchangeSettingRow& row, JsonValue value,
                           uint64 detail, StringView key) noexcept {
    const Span<const ExchangeSettingRow> members(row.members, row.memberCount);
    switch (row.kind) {
    case ExchangeSettingKind::Same:
        return CopyValue(state.writer, value);
    case ExchangeSettingKind::Enum:
        return WriteEnum(state.writer, value);
    case ExchangeSettingKind::Guid:
        return WriteGuid(state.writer, value, state.warnings.GetAllocator());
    case ExchangeSettingKind::PerPlatformFloat:
        return WritePerPlatform(state, value, detail, key);
    case ExchangeSettingKind::Reference:
        return WriteReference(state, value);
    case ExchangeSettingKind::ReferenceArray:
        return WriteReferenceArray(state, value);
    case ExchangeSettingKind::Struct:
        return TranslateObject(state, value, members, false, detail);
    case ExchangeSettingKind::StructArray: {
        if (!value.IsArray()) {
            return Invalid();
        }

        if (auto begun = state.writer.BeginArray(); !begun) {
            return begun;
        }

        for (usize index = 0; index < value.Size(); ++index) {
            if (auto element = TranslateObject(state, value.At(index), members, false, detail);
                !element) {
                return element;
            }
        }

        return state.writer.EndArray();
    }
    case ExchangeSettingKind::EnumMap: {
        // Rocks에 없는 값(빈 rocksKey)은 키를 쓰기 전에 걸렀다(UnmappedEnum).
        const ExchangeSettingRow* mapped = FindEnumValue(row, value);
        if (mapped == nullptr) {
            return Invalid();
        }

        if (mapped->rocksKey.IsEmpty()) {
            break;
        }

        return state.writer.String(mapped->rocksKey);
    }
    case ExchangeSettingKind::Flatten:
    case ExchangeSettingKind::Ignore:
    case ExchangeSettingKind::Unsupported:
        break;
    }

    // 여기 오면 행 종류 표의 결함이다(덤프 내용이 아니다).
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// value(값 struct)의 멤버를 row의 멤버 표로 형제 키에 쓴다(Flatten — 출력 키는 멤버 행의
// rocksKey, 비면 UE 멤버 이름에서 b 접두만 뗀 이름).
Result<void> WriteFlatten(TranslateState& state, const ExchangeSettingRow& row, JsonValue value,
                          uint64 detail) noexcept {
    if (!value.IsObject()) {
        return Invalid();
    }

    const Span<const ExchangeSettingRow> members(row.members, row.memberCount);
    for (usize member = 0; member < value.Size(); ++member) {
        const StringView key = value.KeyAt(member);
        usize index = 0;
        const ExchangeSettingRow* field = FindRow(members, key, index);
        if (field == nullptr) {
            if (auto warned = Warn(state, detail, key); !warned) {
                return warned;
            }

            continue;
        }

        // Flatten 멤버는 값 하나를 쓰는 종류(Same·Enum·EnumMap)이고 묶음이 없어야 한다(행 표의
        // 결함).
        if (field->group != 0 ||
            (field->kind != ExchangeSettingKind::Same && field->kind != ExchangeSettingKind::Enum &&
             field->kind != ExchangeSettingKind::EnumMap)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (UnmappedEnum(*field, value.ValueAt(member))) {
            if (auto warned = Warn(state, detail, key); !warned) {
                return warned;
            }

            continue;
        }

        const StringView outputKey = field->rocksKey.IsEmpty() ? RocksKey(key) : field->rocksKey;
        if (auto keyed = state.writer.Key(outputKey); !keyed) {
            return keyed;
        }

        if (auto written = WriteRowValue(state, *field, value.ValueAt(member), detail, key);
            !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// object의 멤버 가운데 묶음 group의 행만 rows로 번역해 쓴다(객체 시작·끝은 호출자). top이면 최상위
// 표(경고 detail = 행 번호)이고, 아니면 struct 멤버 표(경고 detail = parentDetail)다. 표에 없는
// 키의 경고는 묶음 0 순회에서만 낸다(묶음 순회마다 다시 내지 않는다).
Result<void> TranslateMembers(TranslateState& state, JsonValue object,
                              Span<const ExchangeSettingRow> rows, bool top, uint64 parentDetail,
                              uint8 group) noexcept {
    const uint32 outputBit = ExchangeClassBit(state.input.output);
    for (usize member = 0; member < object.Size(); ++member) {
        const StringView key = object.KeyAt(member);
        const JsonValue value = object.ValueAt(member);
        usize index = 0;
        const ExchangeSettingRow* row = FindRow(rows, key, index);
        const uint64 detail = top ? static_cast<uint64>(index) : parentDetail;
        if (row == nullptr) {
            if (group == 0) {
                if (auto warned =
                        Warn(state, top ? ExchangeUnknownSettingDetail : parentDetail, key);
                    !warned) {
                    return warned;
                }
            }

            continue;
        }

        if ((row->classes != 0 && (row->classes & outputBit) == 0) || row->group != group) {
            continue;
        }

        if (row->kind == ExchangeSettingKind::Ignore) {
            continue;
        }

        if (row->kind == ExchangeSettingKind::Unsupported) {
            auto same = IsDefault(state, value, row->defaultValue);
            if (!same) {
                return Result<void>::Failure(same.Error());
            }

            if (!same.Value()) {
                if (auto warned = Warn(state, detail, key); !warned) {
                    return warned;
                }
            }

            continue;
        }

        // Rocks에 없는 enum 값은 키를 쓰지 않고 경고로 버린다(Rocks 기본값이 남는다).
        if (UnmappedEnum(*row, value)) {
            if (auto warned = Warn(state, detail, key); !warned) {
                return warned;
            }

            continue;
        }

        if (row->kind == ExchangeSettingKind::Flatten) {
            if (auto flattened = WriteFlatten(state, *row, value, detail); !flattened) {
                return flattened;
            }

            continue;
        }

        const StringView outputKey = row->rocksKey.IsEmpty() ? RocksKey(key) : row->rocksKey;
        if (auto keyed = state.writer.Key(outputKey); !keyed) {
            return keyed;
        }

        if (auto written = WriteRowValue(state, *row, value, detail, key); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// object를 rows로 번역해 객체 하나를 쓴다. top이면 최상위 표(경고 detail = 행 번호 — 묶음 객체를
// 최상위 키 뒤에 쓴다)이고, 아니면 struct 멤버 표(경고 detail = parentDetail)다.
Result<void> TranslateObject(TranslateState& state, JsonValue object,
                             Span<const ExchangeSettingRow> rows, bool top,
                             uint64 parentDetail) noexcept {
    if (!object.IsObject()) {
        return Invalid();
    }

    const Span<const StringView> groups = top ? state.input.groups : Span<const StringView>();
    for (const ExchangeSettingRow& row : rows) {
        // 묶음 번호가 묶음 이름 수를 넘거나 멤버 표에 묶음이 있으면 행 표의 결함이다.
        if (row.group > groups.Size()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (auto begun = state.writer.BeginObject(); !begun) {
        return begun;
    }

    if (auto members = TranslateMembers(state, object, rows, top, parentDetail, 0); !members) {
        return members;
    }

    for (usize group = 0; group < groups.Size(); ++group) {
        if (auto keyed = state.writer.Key(groups[group]); !keyed) {
            return keyed;
        }

        if (auto begun = state.writer.BeginObject(); !begun) {
            return begun;
        }

        if (auto members = TranslateMembers(state, object, rows, top, parentDetail,
                                            static_cast<uint8>(group + 1));
            !members) {
            return members;
        }

        if (auto ended = state.writer.EndObject(); !ended) {
            return ended;
        }
    }

    return state.writer.EndObject();
}

} // namespace exchange_settings

Result<void> TranslateExchangeSettings(const ExchangeSettingInput& input, JsonWriter& writer,
                                       Array<ExchangeSettingWarning>& warnings) noexcept {
    using namespace exchange_settings;
    TranslateState state{input, writer, warnings};
    return TranslateObject(state, input.settings, input.rows, true, 0);
}

} // namespace rocks
