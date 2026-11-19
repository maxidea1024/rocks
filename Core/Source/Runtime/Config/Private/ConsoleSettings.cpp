#include "ConfigState.h"
#include "ConfigText.h"

#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Object/PropertyPath.h>
#include <Platform/Clock.h>
#include <ReflectionText/TextAssignments.h>

// 콘솔 설정·콘솔 명령·변경 콜백·설정 snapshot이다(Docs/Design/ConfigSystem.ko.md 5절 — 설정 CF2).
namespace rocks::config {

namespace {

using reflection_text::TextCodecContext;
using reflection_text::TextCodecOptions;
using reflection_text::TextDiagnostics;
using reflection_text::TextScalarType;
using reflection_text::TextSource;

StringView ToView(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
}

bool IsRegionName(StringView text) noexcept {
    bool first = true;
    for (const char c : text) {
        const bool lower = c >= 'a' && c <= 'z';
        if (first ? !lower : !(lower || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }

        first = false;
    }

    return !first;
}

} // namespace

ValueKind ConsoleValueKind(ConsoleValueType type) noexcept {
    switch (type) {
    case ConsoleValueType::Bool:
        return ValueKind::Bool;
    case ConsoleValueType::Int32:
        return ValueKind::Int32;
    case ConsoleValueType::Int64:
        return ValueKind::Int64;
    case ConsoleValueType::Float:
        return ValueKind::Float32;
    case ConsoleValueType::Double:
        return ValueKind::Float64;
    case ConsoleValueType::String:
        return ValueKind::Utf8String;
    case ConsoleValueType::Name:
        return ValueKind::Name;
    case ConsoleValueType::Enum:
        return ValueKind::Enum;
    }

    return ValueKind::Opaque;
}

namespace detail {

ConfigState::~ConfigState() = default;

usize ConfigState::FindSetting(StringView name) const noexcept {
    for (usize index = 0; index < settings.Size(); ++index) {
        if (settings[index]->name.View() == name) {
            return index;
        }
    }

    return IndexNone;
}

usize ConfigState::FindCommand(StringView name) const noexcept {
    for (usize index = 0; index < commands.Size(); ++index) {
        if (commands[index]->name.View() == name) {
            return index;
        }
    }

    return IndexNone;
}

uint32 ConsoleFlagsOfField(uint32 settingsFlags) noexcept {
    uint32 flags = 0;
    if (HasFlag(settingsFlags, SettingsFieldFlags::UserConfig)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::UserConfig);
    }

    if (HasFlag(settingsFlags, SettingsFieldFlags::RestartRequired)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::RestartRequired);
    }

    if (HasFlag(settingsFlags, SettingsFieldFlags::StartupOnly)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::StartupOnly);
    }

    if (HasFlag(settingsFlags, SettingsFieldFlags::RenderRead)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::RenderRead);
    }

    if (HasFlag(settingsFlags, SettingsFieldFlags::DevelopmentOnly)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::DevelopmentOnly);
    }

    return flags;
}

StringView CheckConsoleLayer(bool releaseRules, ConfigLayer layer, uint32 consoleFlags,
                             ConfigDiagnosticCode& code, bool sealedWrite) noexcept {
    // 출시 규칙의 개발 전용 판정이 먼저다(출시 빌드에서는 그 설정이 없는 것과 같다).
    code = ConfigDiagnosticCode::LayerNotAllowed;
    if (releaseRules && HasFlag(consoleFlags, ConsoleSettingFlags::DevelopmentOnly) &&
        layer != ConfigLayer::CodeDefault) {
        return "a DevelopmentOnly value keeps its code default under release rules";
    }

    // 봉인 뒤 부팅 변수에 바로 쓰는 층(콘솔·코드·범위 임시 변경)은 쓰기 때 거부한다(CVar 9절 — 부른
    // 쪽이 바로 효력을 기대하므로 결과로 알린다). 파일 층·파생 층·휘발 층은 받고 Sync가 지금 값을
    // 지킨다.
    if (sealedWrite && HasBootFlags(consoleFlags) && IsDirectWriteLayer(layer)) {
        code = ConfigDiagnosticCode::BootSealed;
        return BootSealedMessage;
    }

    if (layer == ConfigLayer::Console &&
        HasFlag(consoleFlags, ConsoleSettingFlags::ConsoleReadOnly)) {
        code = ConfigDiagnosticCode::ConsoleReadOnly;
        return "the console cannot change a ConsoleReadOnly setting";
    }

    if (layer == ConfigLayer::User && !HasFlag(consoleFlags, ConsoleSettingFlags::UserConfig)) {
        return "the user layer can write UserConfig settings only";
    }

    return {};
}

usize ConsoleFieldOf(const SectionState& section, StringView name) noexcept {
    for (const SettingsFieldSchema& field : section.ops.settings->fields) {
        if (field.consoleName.Size() != 0 && ToView(field.consoleName) == name) {
            return section.lease.FieldIndex(field.field);
        }
    }

    return IndexNone;
}

bool EntryTouchesSection(const ConfigState& state, usize sectionIndex,
                         const ConfigEntry& entry) noexcept {
    const SectionState& section = *state.sections[sectionIndex];
    switch (entry.kind) {
    case EntryKind::Assignment:
        return entry.section == sectionIndex;
    case EntryKind::Console:
        return ConsoleFieldOf(section, entry.path.View()) != IndexNone;
    case EntryKind::Document:
        break;
    }

    if (entry.branch == section.branch &&
        FindMemberIgnoringCase(entry.document->Root(), section.section.View()).IsValid()) {
        return true;
    }

    const JsonValue settings = SettingsSectionOf(entry);
    for (usize member = 0; settings.IsObject() && member < settings.Size(); ++member) {
        if (ConsoleFieldOf(section, settings.KeyAt(member)) != IndexNone) {
            return true;
        }
    }

    return false;
}

Result<bool> ConsoleTextOfJson(JsonValue value, ValueKind kind, String& out) noexcept {
    bool matches = false;
    switch (kind) {
    case ValueKind::Bool:
        matches = value.IsBool();
        break;
    case ValueKind::Int32:
    case ValueKind::Int64:
        matches = value.IsValid() &&
                  (value.Type() == JsonType::Integer || value.Type() == JsonType::Unsigned);
        break;
    case ValueKind::Float32:
    case ValueKind::Float64:
        matches = value.IsNumber();
        break;
    case ValueKind::Utf8String:
    case ValueKind::Name:
    case ValueKind::Enum:
        matches = value.IsString();
        break;
    default:
        break;
    }

    if (!matches) {
        return Result<bool>::Success(false);
    }

    Result<void> assigned = Result<void>::Success();
    if (value.IsBool()) {
        assigned = out.TryAssign(value.GetBool() ? "true" : "false");
    } else if (value.IsNumber()) {
        assigned = out.TryAssign(value.RawNumber());
    } else {
        assigned = out.TryAssign(value.GetString());
    }

    if (!assigned) {
        return Result<bool>::Failure(assigned.Error());
    }

    return Result<bool>::Success(true);
}

Result<void> CopyFieldValue(const FieldValue& value, FieldValue& out) noexcept {
    if (auto assigned = out.string.TryAssign(value.string.View()); !assigned) {
        return assigned;
    }

    // Text(리플렉션 B9l)는 out의 allocator로 새 노드를 만든다.
    auto text = value.text.DeepClone(out.GetAllocator());
    if (!text) {
        return Result<void>::Failure(text.Error());
    }

    out.text = Move(text.Value());

    out.kind = value.kind;
    out.boolean = value.boolean;
    out.unsignedInteger = value.unsignedInteger;
    out.signedInteger = value.signedInteger;
    out.float32 = value.float32;
    out.float64 = value.float64;
    out.guid = value.guid;
    out.reference = value.reference;
    out.digest = value.digest;
    out.name = value.name;
    out.type = value.type;
    out.present = value.present;
    return Result<void>::Success();
}

namespace {

TextScalarType ScalarTypeOf(const ConsoleSettingState& setting) noexcept {
    TextScalarType type;
    type.type.kind = setting.kind;
    type.enumSchema = setting.enumSchema;
    if (setting.enumSchema != nullptr) {
        type.type.type = setting.enumSchema->id;
    }

    return type;
}

Result<void> FormatScalar(ConfigState& state, const TextScalarType& type, const FieldValue& value,
                          String& out) noexcept {
    TextDiagnostics diagnostics(state.allocator);
    const TextCodecOptions options = state.CodecOptions();
    TextCodecContext codec(state.objects.Types(), options, diagnostics, state.allocator);
    StringBuilder<64> text(state.allocator);
    if (auto formatted = reflection_text::FormatTextScalar(codec, type, value, text); !formatted) {
        return formatted;
    }

    return out.TryAssign(text.View());
}

// 섹션 값 storage의 필드(스키마 번호 field) 값을 out에 읽는다.
Result<void> ReadField(ConfigState& state, const SectionState& section, void* storage, usize field,
                       FieldValue& out) noexcept {
    const PathStep step{section.lease.Schema().fields[field].id};
    const PropertyPath path{Span<const PathStep>(&step, 1)};
    return PathRead(
        state.objects, state.access,
        PathTarget::Value(section.lease, section.lease.Struct(), storage, state.allocator), path,
        out);
}

// 읽은 값(value — 콘솔 설정 종류)의 CVar 검사용 값이다. String·Name의 텍스트는 입력 텍스트다(값
// 표기가 원문 그대로다).
cvar::detail::ScalarValue ScalarOfField(const FieldValue& value, StringView text) noexcept {
    cvar::detail::ScalarValue scalar;
    switch (value.kind) {
    case ValueKind::Bool:
        scalar.boolean = value.boolean;
        break;
    case ValueKind::Float32:
        scalar.real = static_cast<double>(value.float32);
        break;
    case ValueKind::Float64:
        scalar.real = value.float64;
        break;
    case ValueKind::Utf8String:
        scalar.text = value.string.View();
        break;
    case ValueKind::Name:
        scalar.text = text;
        break;
    default:
        scalar.integer = value.signedInteger;
        break;
    }

    return scalar;
}

// CVar 검사가 보정한 수 값을 value에 되쓴다(정수·실수만 보정된다).
void StoreScalar(const cvar::detail::ScalarValue& scalar, FieldValue& value) noexcept {
    switch (value.kind) {
    case ValueKind::Float32:
        value.float32 = static_cast<float>(scalar.real);
        break;
    case ValueKind::Float64:
        value.float64 = scalar.real;
        break;
    case ValueKind::Int32:
    case ValueKind::Int64:
        value.signedInteger = scalar.integer;
        break;
    default:
        break;
    }
}

} // namespace

Result<void> FormatConsoleValue(ConfigState& state, const ConsoleSettingState& setting,
                                const FieldValue& value, String& out) noexcept {
    return FormatScalar(state, ScalarTypeOf(setting), value, out);
}

Result<StringView> CheckConsoleValue(ConfigState& state, const ConsoleSettingState& setting,
                                     ConfigLayer layer, StringView text, FieldValue& out,
                                     ConfigDiagnosticCode& code, bool writing) noexcept {
    const StringView problem = CheckConsoleLayer(state.releaseRules, layer, setting.flags, code,
                                                 writing && state.startupComplete);
    if (!problem.IsEmpty()) {
        return Result<StringView>::Success(problem);
    }

    // enum은 수도 받는다(CVar 4.4·15절): ReflectionText enum 표기는 이름만이므로 그 값의 열거자
    // 이름으로 바꿔 읽는다. 표에 없는 수는 거부한다.
    if (setting.kind == ValueKind::Enum && setting.enumSchema != nullptr) {
        if (auto number = TryParseInt64(text); number) {
            StringView named;
            for (const EnumeratorSchema& enumerator : setting.enumSchema->values) {
                if (enumerator.value == number.Value()) {
                    named = enumerator.toolName.Size() != 0 ? ToView(enumerator.toolName)
                                                            : ToView(enumerator.name);
                }
            }

            if (named.IsEmpty()) {
                code = ConfigDiagnosticCode::InvalidEnum;
                return Result<StringView>::Success("the value is not one of the enum values");
            }

            text = named;
        }
    }

    // 문자열 값의 경로 이름은 파일 값과 같은 규칙으로 펼치지 않는다(독립 설정은 경로 필드가
    // 아니다).
    TextDiagnostics diagnostics(state.allocator);
    const TextCodecOptions options = state.CodecOptions();
    TextCodecContext codec(state.objects.Types(), options, diagnostics, state.allocator);
    auto parsed = reflection_text::ParseTextScalar(
        codec, ScalarTypeOf(setting), text, TextSource::Assignment(0), setting.name.View(), out);
    if (!parsed) {
        return Result<StringView>::Failure(parsed.Error());
    }

    code = ConfigDiagnosticCode::InvalidSetting;
    if (!parsed.Value()) {
        return Result<StringView>::Success("the value does not parse as the setting type");
    }

    // NaN·무한대는 범위·Clamp와 무관하게 늘 거부한다(CVar 4.2절 — CVar 검사 함수와 같은 판정을
    // 독립 설정·필드·명령 인자에도 건다).
    if (setting.kind == ValueKind::Float32 || setting.kind == ValueKind::Float64) {
        constexpr double Largest = NumericLimits<double>::Max();
        const double number =
            setting.kind == ValueKind::Float32 ? static_cast<double>(out.float32) : out.float64;
        if (number != number || number > Largest || number < -Largest) {
            code = ConfigDiagnosticCode::ValueOutOfRange;
            return Result<StringView>::Success("the value is not a finite number");
        }
    }

    if (setting.hasRange) {
        double number = 0;
        switch (setting.kind) {
        case ValueKind::Int32:
        case ValueKind::Int64:
            number = static_cast<double>(out.signedInteger);
            break;
        case ValueKind::Float32:
            number = static_cast<double>(out.float32);
            break;
        default:
            number = out.float64;
            break;
        }

        // NaN은 어느 비교도 참이 아니므로 범위 밖이다.
        if (!(number >= setting.minimum && number <= setting.maximum)) {
            code = ConfigDiagnosticCode::ValueOutOfRange;
            return Result<StringView>::Success("the value is outside the setting range");
        }
    }

    if (setting.definition != nullptr && setting.definition->check != nullptr) {
        // CVar 핸들 설정: 정의의 범위·보정·Validate를 타입대로 적용한다(컴파일 때 기본값 검사와
        // 같은 함수 — CVar 4.2절). String·Name의 검사 인자는 값 텍스트다.
        cvar::detail::ScalarValue scalar = ScalarOfField(out, text);
        StringView reason;
        switch (setting.definition->check(*setting.definition, scalar, reason)) {
        case cvar::detail::ValueCheck::Accepted:
            break;
        case cvar::detail::ValueCheck::Clamped:
            StoreScalar(scalar, out);
            code = ConfigDiagnosticCode::Clamped;
            break;
        case cvar::detail::ValueCheck::OutOfRange:
            code = ConfigDiagnosticCode::ValueOutOfRange;
            return Result<StringView>::Success(reason);
        case cvar::detail::ValueCheck::ValidationFailed:
            code = ConfigDiagnosticCode::ValidationFailed;
            return Result<StringView>::Success(reason);
        case cvar::detail::ValueCheck::UndefinedLevel:
            code = ConfigDiagnosticCode::UndefinedLevel;
            return Result<StringView>::Success(reason);
        }
    }

    return Result<StringView>::Success(StringView{});
}

Result<void> AdoptPendingValues(ConfigState& state, ConsoleSettingState& setting) noexcept {
    Array<ConfigEntry*> order(state.allocator);
    for (UniquePtr<ConfigEntry>& entry : state.entries) {
        if (auto pushed = order.TryPushBack(entry.Get()); !pushed) {
            return pushed;
        }
    }

    order.Sort([](const ConfigEntry* left, const ConfigEntry* right) noexcept {
        return left->layer != right->layer ? left->layer < right->layer
                                           : left->sequence < right->sequence;
    });
    if (auto computed = RecomputeConsoleSetting(state, setting, order.View(), true, setting.value,
                                                setting.text, setting.sources);
        !computed) {
        return computed;
    }

    // 등록 전에 들어온 값이 적용됐으면 알린다(정보 LateBound — CVar 12절). 출처는 가장 강한
    // 입력이다.
    if (setting.sources.IsEmpty()) {
        return Result<void>::Success();
    }

    const ConfigEntry* top = state.FindEntry(setting.sources[setting.sources.Size() - 1]);
    return Report(state, ConfigDiagnosticCode::LateBound, ConfigSeverity::Note,
                  top != nullptr ? top->layer : ConfigLayer::CodeDefault,
                  top != nullptr ? top->sourceName.View() : StringView{}, nullptr,
                  setting.name.View(), "values set before registration were applied");
}

Result<void> RecomputeConsoleSetting(ConfigState& state, const ConsoleSettingState& setting,
                                     Span<ConfigEntry* const> order, bool report, FieldValue& value,
                                     String& text, Array<uint64>& sources) noexcept {
    sources.Clear();
    if (auto copied = CopyFieldValue(setting.defaultValue, value); !copied) {
        return copied;
    }

    FieldValue candidate(state.allocator);
    String converted(state.allocator);
    for (ConfigEntry* entry : order) {
        StringView contribution;
        JsonPosition at{0, 0, 0};
        const JsonPosition* position = nullptr;
        if (entry->kind == EntryKind::Console && entry->path.View() == setting.name.View()) {
            contribution = entry->value.View();
        } else if (entry->kind == EntryKind::Document) {
            const JsonValue settings = SettingsSectionOf(*entry);
            const JsonValue member =
                settings.IsObject() ? settings.Find(setting.name.View()) : JsonValue{};
            if (!member.IsValid()) {
                continue;
            }

            at = member.Position();
            position = &at;
            auto matched = ConsoleTextOfJson(member, setting.kind, converted);
            if (!matched) {
                return Result<void>::Failure(matched.Error());
            }

            if (!matched.Value()) {
                if (report) {
                    if (auto reported = Report(
                            state, ConfigDiagnosticCode::InvalidSetting, ConfigSeverity::Error,
                            entry->layer, entry->sourceName.View(), position, setting.name.View(),
                            "the JSON value type does not match the setting type");
                        !reported) {
                        return reported;
                    }
                }

                continue;
            }

            contribution = converted.View();
        } else {
            continue;
        }

        // 봉인 뒤 바로 쓰는 층으로 들어온 입력은 그 뒤에 등록한 부팅 값에 닿지 않는다(쓰기 때
        // 이름을 몰라 거부하지 못한 늦은 등록 — CVar 9절). 등록 때(report) 한 번 알린다.
        if (entry->sealedDirectWrite && HasBootFlags(setting.flags)) {
            if (report) {
                if (auto reported =
                        Report(state, ConfigDiagnosticCode::BootSealed, ConfigSeverity::Error,
                               entry->layer, entry->sourceName.View(), position,
                               setting.name.View(), BootSealedMessage);
                    !reported) {
                    return reported;
                }
            }

            continue;
        }

        ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
        auto checked =
            CheckConsoleValue(state, setting, entry->layer, contribution, candidate, code, false);
        if (!checked) {
            return Result<void>::Failure(checked.Error());
        }

        // 묶음 펼침 입력은 여기서 알리지 않는다(묶음 검사가 (묶음, 등급, 항목)마다 한 번 알린다 —
        // CVar 11절).
        if (!checked.Value().IsEmpty()) {
            if (report && !entry->groupExpansion) {
                if (auto reported = Report(state, code, ConfigSeverity::Error, entry->layer,
                                           entry->sourceName.View(), position, setting.name.View(),
                                           checked.Value());
                    !reported) {
                    return reported;
                }
            }

            continue;
        }

        if (report && code == ConfigDiagnosticCode::Clamped) {
            if (auto reported = Report(state, code, ConfigSeverity::Note, entry->layer,
                                       entry->sourceName.View(), position, setting.name.View(),
                                       "the value was clamped to the setting range");
                !reported) {
                return reported;
            }
        }

        if (auto copied = CopyFieldValue(candidate, value); !copied) {
            return copied;
        }

        if (auto pushed = sources.TryPushBack(entry->sequence); !pushed) {
            return pushed;
        }
    }

    return FormatConsoleValue(state, setting, value, text);
}

Result<void> RegisterFieldSettings(ConfigState& state, usize sectionIndex) noexcept {
    SectionState& section = *state.sections[sectionIndex];
    // 이름 검사를 먼저 모두 한다(중간에 실패해 일부만 등록되지 않게).
    for (const SettingsFieldSchema& field : section.ops.settings->fields) {
        if (field.consoleName.Size() == 0) {
            continue;
        }

        const StringView name = ToView(field.consoleName);
        StringView region;
        StringView rest;
        if (!IsConsoleSettingName(name) ||
            !name.Split(".", region, rest, StringComparison::CaseSensitive)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        bool known = false;
        for (const String& candidate : state.regions) {
            known = known || candidate.View() == region;
        }

        if (!known) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (state.FindSetting(name) != IndexNone || state.FindCommand(name) != IndexNone) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    const usize first = state.settings.Size();
    auto rollback = [&]() noexcept { static_cast<void>(state.settings.TryResize(first)); };
    for (const SettingsFieldSchema& field : section.ops.settings->fields) {
        if (field.consoleName.Size() == 0) {
            continue;
        }

        auto made = UniquePtr<ConsoleSettingState>::TryMake(state.allocator, state.allocator);
        if (!made) {
            rollback();
            return Result<void>::Failure(made.Error());
        }

        ConsoleSettingState& setting = *made.Value();
        setting.section = sectionIndex;
        setting.field = section.lease.FieldIndex(field.field);
        const FieldSchema& schema = section.lease.Schema().fields[setting.field];
        setting.kind = schema.value.kind;
        setting.enumSchema = section.lease.FieldEnum(setting.field);
        setting.flags = ConsoleFlagsOfField(field.flags);
        Result<void> filled = setting.name.TryAssign(ToView(field.consoleName));
        if (filled) {
            filled = setting.help.TryAssign(schema.displayName.Size() != 0
                                                ? ToView(schema.displayName)
                                                : ToView(EffectiveToolName(schema)));
        }

        if (filled) {
            filled = ReadField(state, section, section.value, setting.field, setting.defaultValue);
        }

        if (filled) {
            filled = CopyFieldValue(setting.defaultValue, setting.value);
        }

        if (filled) {
            filled = FormatConsoleValue(state, setting, setting.value, setting.text);
        }

        if (filled) {
            filled = state.settings.TryPushBack(Move(made.Value()));
        }

        if (!filled) {
            rollback();
            return filled;
        }
    }

    return Result<void>::Success();
}

Result<void> ComputeSettingResults(ConfigState& state, Span<ConfigEntry* const> order,
                                   Span<void* const> sectionValues,
                                   Array<UniquePtr<SettingResult>>& out) noexcept {
    for (const UniquePtr<ConsoleSettingState>& setting : state.settings) {
        auto made = UniquePtr<SettingResult>::TryMake(state.allocator, state.allocator);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        SettingResult& result = *made.Value();
        if (setting->section == IndexNone) {
            if (auto computed = RecomputeConsoleSetting(state, *setting, order, false, result.value,
                                                        result.text, result.sources);
                !computed) {
                return computed;
            }
        } else {
            const SectionState& section = *state.sections[setting->section];
            if (auto read = ReadField(state, section, sectionValues[setting->section],
                                      setting->field, result.value);
                !read) {
                return read;
            }

            if (auto formatted = FormatConsoleValue(state, *setting, result.value, result.text);
                !formatted) {
                return formatted;
            }
        }

        result.changed = result.text.View() != setting->text.View();
        if (auto pushed = out.TryPushBack(Move(made.Value())); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 이름 블록에 값 하나를 더한다.
Result<void> AddNamed(NamedBlock& block, StringView name, const FieldValue& value) noexcept {
    Allocator& allocator = block.entries.GetAllocator();
    auto made = UniquePtr<NamedBlock::Entry>::TryMake(allocator, allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    if (auto assigned = made.Value()->name.TryAssign(name); !assigned) {
        return assigned;
    }

    if (auto copied = CopyFieldValue(value, made.Value()->value); !copied) {
        return copied;
    }

    return block.entries.TryPushBack(Move(made.Value()));
}

Result<void> PrepareNamedBlock(ConfigState& state, Span<const UniquePtr<SettingResult>> settings,
                               Span<void* const> sectionValues, SharedPtr<const NamedBlock>& out,
                               String& signature) noexcept {
    auto made = SharedPtr<NamedBlock>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    NamedBlock& block = *made.Value();
    // 콘솔 설정(독립·필드)의 RenderRead 값(CVar 핸들은 CVar 칸에 있다).
    for (usize index = 0; index < settings.Size(); ++index) {
        const ConsoleSettingState& setting = *state.settings[index];
        if (!HasFlag(setting.flags, ConsoleSettingFlags::RenderRead) ||
            setting.definition != nullptr) {
            continue;
        }

        if (auto added = AddNamed(block, setting.name.View(), settings[index]->value); !added) {
            return added;
        }
    }

    // 콘솔 이름이 없는 RenderRead 설정 struct 필드(`<Branch>.<Section>.<필드>`).
    for (usize sectionIndex = 0; sectionIndex < state.sections.Size(); ++sectionIndex) {
        const SectionState& section = *state.sections[sectionIndex];
        for (const SettingsFieldSchema& field : section.ops.settings->fields) {
            if (!HasFlag(field.flags, SettingsFieldFlags::RenderRead) ||
                field.consoleName.Size() != 0) {
                continue;
            }

            const usize index = section.lease.FieldIndex(field.field);
            FieldValue value(state.allocator);
            if (auto read = ReadField(state, section, sectionValues[sectionIndex], index, value);
                !read) {
                return read;
            }

            StringBuilder<128> name(state.allocator);
            static_cast<void>(name.TryAppendFormat(
                "{}.{}.{}", state.branches[section.branch].View(), section.section.View(),
                ToView(EffectiveToolName(section.lease.Schema().fields[index]))));
            if (auto status = name.Status(); !status) {
                return status;
            }

            if (auto added = AddNamed(block, name.View(), value); !added) {
                return added;
            }
        }
    }

    block.entries.Sort([](const UniquePtr<NamedBlock::Entry>& left,
                          const UniquePtr<NamedBlock::Entry>& right) noexcept {
        return detail::LessBytes(left->name.View(), right->name.View());
    });
    // 서명은 이름 순서의 `이름=표기` 줄이다. 이전 서명과 같으면 이전 블록을 쓴다(out은 null).
    for (const UniquePtr<NamedBlock::Entry>& entry : block.entries) {
        String text(state.allocator);
        const FieldValue& value = entry->value;
        TextScalarType type;
        type.type.kind = value.kind;
        type.type.type = value.type;
        if (value.kind == ValueKind::Enum) {
            // enum 표기는 정수로 쓴다(서명은 비교에만 쓴다).
            type.type.kind = ValueKind::Int64;
        }

        FieldValue plain(state.allocator);
        if (auto copied = CopyFieldValue(value, plain); !copied) {
            return copied;
        }

        plain.kind = type.type.kind;
        if (auto formatted = FormatScalar(state, type, plain, text); !formatted) {
            return formatted;
        }

        Result<void> appended = signature.TryAppend(entry->name.View());
        if (appended) {
            appended = signature.TryAppend("=");
        }

        if (appended) {
            appended = signature.TryAppend(text.View());
        }

        if (appended) {
            appended = signature.TryAppend("\n");
        }

        if (!appended) {
            return appended;
        }
    }

    if (state.snapshot.Get() != nullptr && signature.View() == state.renderSignature.View()) {
        out = SharedPtr<const NamedBlock>();
        return Result<void>::Success();
    }

    out = Move(made.Value());
    return Result<void>::Success();
}

void RunPendingCommands(ConfigState& state) noexcept {
    state.syncing = true;
    // 명령은 큐 순서대로 실행한다(CVar 10절 5단계). 실행 중 콘솔 입력으로 새로 들어온 명령(뒤에
    // 붙는다)은 다음 Sync에 실행한다.
    const usize count = state.pendingCommands.Size();
    for (usize index = 0; index < count; ++index) {
        // 명령이 콘솔 입력을 더 넣을 수 있으므로(배열이 자랄 수 있다) 꺼내서 실행한다.
        PendingCommand pending = Move(state.pendingCommands[index]);
        RunPendingCommand(*state.self, state, pending);
    }

    static_cast<void>(state.pendingCommands.TryRemoveAt(0, count)); // 줄이기는 할당하지 않는다
    state.syncing = false;
}

} // namespace detail

using detail::ConfigEntry;
using detail::ConfigState;
using detail::ConsoleCommandState;
using detail::ConsoleSettingState;
using detail::EntryKind;
using detail::PendingCommand;
using detail::SectionState;

Result<void> ConfigContext::RegisterConsoleRegion(StringView region) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || !IsRegionName(region)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    for (const String& existing : state->regions) {
        if (existing.View() == region) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    String added(*allocator_);
    if (auto assigned = added.TryAssign(region); !assigned) {
        return assigned;
    }

    return state->regions.TryPushBack(Move(added));
}

Result<ConsoleSettingId> ConfigContext::FindConsoleSetting(StringView name) const noexcept {
    const ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<ConsoleSettingId>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<ConsoleSettingId>::Failure(ErrorCode::WrongThread);
    }

    const usize index = state->FindSetting(name);
    if (index == IndexNone ||
        (state->releaseRules &&
         HasFlag(state->settings[index]->flags, ConsoleSettingFlags::DevelopmentOnly))) {
        return Result<ConsoleSettingId>::Failure(ErrorCode::NotFound);
    }

    ConsoleSettingId id;
    id.index = static_cast<uint32>(index);
    return Result<ConsoleSettingId>::Success(id);
}

const FieldValue& ConfigContext::ConsoleValue(ConsoleSettingId id) const noexcept {
    const ConfigState* state = state_.Get();
    if (state == nullptr || !id.IsValid() || id.index >= state->settings.Size()) {
        platform::Fatal("ConfigContext::ConsoleValue: invalid console setting id");
    }

    if (platform::CurrentThreadId() != state->owner) {
        platform::Fatal("ConfigContext used from a thread other than its owner");
    }

    return state->settings[id.index]->value;
}

Result<void> ConfigContext::CompleteStartup() noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (state->startupComplete) {
        return Result<void>::Success();
    }

    // 큐에 남은 쓰기는 마지막 Sync 값에 들어 있지 않으므로 그대로 봉인하면 빠진다. 먼저 Sync한다.
    if (HasPendingChanges()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 봉인: 부팅 변수(BootCVar)의 지금 값을 불변 부팅 블록으로 굳혀 게시한다(CVar 9절 — 봉인 뒤
    // snapshot의 부팅 값은 모두 같다). 게시가 실패하면 봉인하지 않는다.
    Array<detail::SlotWrite> boot(*allocator_);
    for (const UniquePtr<ConsoleSettingState>& setting : state->settings) {
        if (setting->definition == nullptr || setting->definition->kind != cvar::HandleKind::Boot) {
            continue;
        }

        detail::SlotWrite write;
        write.index =
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&setting->definition->index);
        write.value = &setting->value;
        if (auto pushed = boot.TryPushBack(write); !pushed) {
            return pushed;
        }
    }

    state->startupComplete = true;
    if (auto published = detail::PublishSnapshot(*state, nullptr, {}, boot.View()); !published) {
        state->startupComplete = false;
        return published;
    }

    return Result<void>::Success();
}

} // namespace rocks::config
