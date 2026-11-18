#include "ConfigState.h"

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

Span<const char> ToSpan(StringView text) noexcept {
    return Span<const char>(text.Data(), text.ByteCount());
}

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

// byte 순서 비교(사전식)다.
bool LessBytes(StringView left, StringView right) noexcept {
    const usize count = left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    for (usize index = 0; index < count; ++index) {
        const auto a = static_cast<uint8>(left.Data()[index]);
        const auto b = static_cast<uint8>(right.Data()[index]);
        if (a != b) {
            return a < b;
        }
    }

    return left.ByteCount() < right.ByteCount();
}

Result<void> Write(ConsoleOutput& output, StringView text) noexcept {
    if (output.write == nullptr) {
        return Result<void>::Success();
    }

    return output.write(output.context, text);
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

ConfigSnapshot::ConfigSnapshot(Allocator& allocator) noexcept : entries_(allocator) {
}

StringView ConfigSnapshot::NameAt(usize index) const noexcept {
    if (index >= entries_.Size()) {
        platform::Fatal("ConfigSnapshot::NameAt index out of range");
    }

    return entries_[index]->name.View();
}

const FieldValue& ConfigSnapshot::ValueAt(usize index) const noexcept {
    if (index >= entries_.Size()) {
        platform::Fatal("ConfigSnapshot::ValueAt index out of range");
    }

    return entries_[index]->value;
}

const FieldValue* ConfigSnapshot::Find(StringView name) const noexcept {
    usize low = 0;
    usize high = entries_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (LessBytes(entries_[middle]->name.View(), name)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < entries_.Size() && entries_[low]->name.View() == name ? &entries_[low]->value
                                                                       : nullptr;
}

namespace detail {

// 설정 snapshot을 만드는 쪽의 접근이다(ConfigSnapshot의 friend).
struct SnapshotAccess {
    static Result<void> Add(ConfigSnapshot& snapshot, StringView name,
                            const FieldValue& value) noexcept {
        Allocator& allocator = snapshot.entries_.GetAllocator();
        auto made = UniquePtr<ConfigSnapshot::Entry>::TryMake(allocator, allocator);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        if (auto assigned = made.Value()->name.TryAssign(name); !assigned) {
            return assigned;
        }

        if (auto copied = CopyFieldValue(value, made.Value()->value); !copied) {
            return copied;
        }

        return snapshot.entries_.TryPushBack(Move(made.Value()));
    }

    static void Finish(ConfigSnapshot& snapshot, uint64 generation) noexcept {
        snapshot.generation_ = generation;
        snapshot.entries_.Sort([](const UniquePtr<ConfigSnapshot::Entry>& left,
                                  const UniquePtr<ConfigSnapshot::Entry>& right) noexcept {
            return LessBytes(left->name.View(), right->name.View());
        });
    }
};

ConfigState::~ConfigState() {
    for (PendingCommand& pending : pendingCommands) {
        if (pending.arguments != nullptr) {
            commands[pending.command]->args.destroy(allocator, pending.arguments);
        }
    }
}

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

StringView CheckConsoleLayer(const ConfigState& state, ConfigLayer layer, uint32 consoleFlags,
                             ConfigDiagnosticCode& code) noexcept {
    code = ConfigDiagnosticCode::LayerNotAllowed;
    if (state.releaseRules && HasFlag(consoleFlags, ConsoleSettingFlags::DevelopmentOnly) &&
        layer != ConfigLayer::CodeDefault) {
        return "a DevelopmentOnly value keeps its code default under release rules";
    }

    if (layer == ConfigLayer::Console &&
        HasFlag(consoleFlags, ConsoleSettingFlags::ConsoleReadOnly)) {
        code = ConfigDiagnosticCode::ConsoleReadOnly;
        return "the console cannot change a ConsoleReadOnly setting";
    }

    if (layer == ConfigLayer::Quality &&
        !HasFlag(consoleFlags, ConsoleSettingFlags::QualityMember)) {
        code = ConfigDiagnosticCode::NotQualityMember;
        return "the quality layer can change QualityMember settings only";
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

// 출처 스택의 층 이름 목록을 output에 쓴다.
Result<void> WriteStack(ConfigState& state, Span<const uint64> stack,
                        ConsoleOutput& output) noexcept {
    for (const uint64 sequence : stack) {
        const ConfigEntry* entry = state.FindEntry(sequence);
        if (entry == nullptr) {
            continue;
        }

        StringBuilder<128> line(state.allocator);
        static_cast<void>(line.TryAppendFormat("  {} {} ({})\n", ConfigLayerName(entry->layer),
                                               entry->tag.View(), entry->sourceName.View()));
        if (auto status = line.Status(); !status) {
            return status;
        }

        if (auto written = Write(output, line.View()); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> FormatConsoleValue(ConfigState& state, const ConsoleSettingState& setting,
                                const FieldValue& value, String& out) noexcept {
    return FormatScalar(state, ScalarTypeOf(setting), value, out);
}

Result<StringView> CheckConsoleValue(ConfigState& state, const ConsoleSettingState& setting,
                                     ConfigLayer layer, StringView text, FieldValue& out,
                                     ConfigDiagnosticCode& code) noexcept {
    const StringView problem = CheckConsoleLayer(state, layer, setting.flags, code);
    if (!problem.IsEmpty()) {
        return Result<StringView>::Success(problem);
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

    if (!setting.allowed.IsEmpty()) {
        String canonical(state.allocator);
        if (auto formatted = FormatConsoleValue(state, setting, out, canonical); !formatted) {
            return Result<StringView>::Failure(formatted.Error());
        }

        bool allowed = false;
        for (const String& candidate : setting.allowed) {
            allowed = allowed || (setting.kind == ValueKind::Enum || setting.allowedIgnoreCase
                                      ? EqualsIgnoringAsciiCase(ToSpan(candidate.View()),
                                                                ToSpan(canonical.View()))
                                      : candidate.View() == canonical.View());
        }

        if (!allowed) {
            code = ConfigDiagnosticCode::ValueOutOfRange;
            return Result<StringView>::Success("the value is not one of the allowed values");
        }
    }

    return Result<StringView>::Success(StringView{});
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

        ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
        auto checked =
            CheckConsoleValue(state, setting, entry->layer, contribution, candidate, code);
        if (!checked) {
            return Result<void>::Failure(checked.Error());
        }

        if (!checked.Value().IsEmpty()) {
            if (report) {
                if (auto reported = Report(state, code, ConfigSeverity::Error, entry->layer,
                                           entry->sourceName.View(), position, setting.name.View(),
                                           checked.Value());
                    !reported) {
                    return reported;
                }
            }

            continue;
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

Result<void> PrepareSnapshot(ConfigState& state, Span<const UniquePtr<SettingResult>> settings,
                             Span<void* const> sectionValues, SharedPtr<ConfigSnapshot>& out,
                             String& signature) noexcept {
    auto made = SharedPtr<ConfigSnapshot>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    ConfigSnapshot& snapshot = *made.Value();
    // 콘솔 설정(독립·필드)의 RenderRead 값.
    for (usize index = 0; index < settings.Size(); ++index) {
        const ConsoleSettingState& setting = *state.settings[index];
        if (!HasFlag(setting.flags, ConsoleSettingFlags::RenderRead)) {
            continue;
        }

        if (auto added = SnapshotAccess::Add(snapshot, setting.name.View(), settings[index]->value);
            !added) {
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

            if (auto added = SnapshotAccess::Add(snapshot, name.View(), value); !added) {
                return added;
            }
        }
    }

    SnapshotAccess::Finish(snapshot, 0);
    // 서명은 이름 순서의 `이름=표기` 줄이다. 이전 서명과 같으면 이전 snapshot을 쓴다.
    for (usize index = 0; index < snapshot.Count(); ++index) {
        const StringView name = snapshot.NameAt(index);
        String text(state.allocator);
        const FieldValue& value = snapshot.ValueAt(index);
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

        Result<void> appended = signature.TryAppend(name);
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
        out = SharedPtr<ConfigSnapshot>();
        return Result<void>::Success();
    }

    SnapshotAccess::Finish(snapshot, state.snapshotGeneration + 1);
    out = Move(made.Value());
    return Result<void>::Success();
}

void RunCommandsAndCallbacks(ConfigState& state, Span<const bool> sectionChanged,
                             Span<const bool> settingChanged) noexcept {
    state.syncing = true;
    // 명령은 큐 순서대로 실행한다. 실행 중 콘솔 입력으로 새로 들어온 명령(뒤에 붙는다)은 다음
    // Sync에 실행한다.
    const usize count = state.pendingCommands.Size();
    for (usize index = 0; index < count; ++index) {
        PendingCommand command = state.pendingCommands[index];
        const ConsoleCommandState& definition = *state.commands[command.command];
        if (auto executed =
                definition.execute(definition.context, command.arguments, command.output);
            !executed) {
            // 명령 실패는 그 명령의 콘솔 출력과 로그 양쪽에 남긴다.
            Log::Warning(detail::LogConfig, "Console command {Command} failed ({Error})",
                         definition.name.View(), ErrorCodeName(executed.Error()));
            StringBuilder<128> line(state.allocator);
            static_cast<void>(line.TryAppendFormat("{}: command failed ({})\n",
                                                   definition.name.View(),
                                                   ErrorCodeName(executed.Error())));
            static_cast<void>(Write(command.output, line.View()));
        }

        if (command.arguments != nullptr) {
            definition.args.destroy(state.allocator, command.arguments);
            state.pendingCommands[index].arguments = nullptr;
        }
    }

    static_cast<void>(state.pendingCommands.TryRemoveAt(0, count)); // 줄이기는 할당하지 않는다

    // 콜백은 구독 순서대로 부른다. 콜백 안의 Unsubscribe는 callback을 지워 둔다(아래에서 정리한다).
    for (usize index = 0; index < state.subscriptions.Size(); ++index) {
        const Subscription subscription = state.subscriptions[index];
        if (subscription.callback == nullptr) {
            continue;
        }

        const bool changed = subscription.setting != IndexNone
                                 ? settingChanged[subscription.setting]
                                 : sectionChanged[subscription.section];
        if (!changed) {
            continue;
        }

        ConfigChange change;
        StringBuilder<128> name(state.allocator);
        if (subscription.setting != IndexNone) {
            change.name = state.settings[subscription.setting]->name.View();
        } else {
            const SectionState& section = *state.sections[subscription.section];
            static_cast<void>(name.TryAppendFormat("{}.{}", state.branches[section.branch].View(),
                                                   section.section.View()));
            change.name = name.View();
            change.settings = section.ops.id;
        }

        subscription.callback(subscription.context, change);
    }

    usize kept = 0;
    for (usize index = 0; index < state.subscriptions.Size(); ++index) {
        if (state.subscriptions[index].callback != nullptr) {
            state.subscriptions[kept++] = state.subscriptions[index];
        }
    }

    static_cast<void>(state.subscriptions.TryResize(kept)); // 줄이기는 할당하지 않는다
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
using detail::Subscription;

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

Result<ConsoleSettingId>
ConfigContext::RegisterConsoleSetting(const ConsoleSettingDefinition& definition) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<ConsoleSettingId>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<ConsoleSettingId>::Failure(ErrorCode::WrongThread);
    }

    StringView region;
    StringView rest;
    bool known = false;
    if (IsConsoleSettingName(definition.name) &&
        definition.name.Split(".", region, rest, StringComparison::CaseSensitive)) {
        for (const String& candidate : state->regions) {
            known = known || candidate.View() == region;
        }
    }

    const ValueKind kind = ConsoleValueKind(definition.type);
    const bool numeric = kind == ValueKind::Int32 || kind == ValueKind::Int64 ||
                         kind == ValueKind::Float32 || kind == ValueKind::Float64;
    const bool textual =
        kind == ValueKind::Utf8String || kind == ValueKind::Name || kind == ValueKind::Enum;
    const bool enumType = definition.enumType != TypeId{};
    if (!known || definition.help.IsEmpty() || kind == ValueKind::Opaque ||
        (definition.flags & ~KnownConsoleSettingFlags) != 0 ||
        (definition.hasRange && (!numeric || !(definition.minimum <= definition.maximum))) ||
        (!definition.allowedValues.IsEmpty() && !textual) ||
        (enumType != (kind == ValueKind::Enum))) {
        return Result<ConsoleSettingId>::Failure(ErrorCode::InvalidArgument);
    }

    if (state->FindSetting(definition.name) != IndexNone ||
        state->FindCommand(definition.name) != IndexNone) {
        return Result<ConsoleSettingId>::Failure(ErrorCode::AlreadyExists);
    }

    auto made = UniquePtr<ConsoleSettingState>::TryMake(*allocator_, *allocator_);
    if (!made) {
        return Result<ConsoleSettingId>::Failure(made.Error());
    }

    ConsoleSettingState& setting = *made.Value();
    setting.type = definition.type;
    setting.kind = kind;
    setting.flags = definition.flags;
    setting.hasRange = definition.hasRange;
    setting.minimum = definition.minimum;
    setting.maximum = definition.maximum;
    setting.allowedIgnoreCase = definition.allowedValuesIgnoreCase;
    if (kind == ValueKind::Enum) {
        auto lease = objects_->Types().AcquireValueType(definition.enumType, TypeKind::Enum);
        if (!lease) {
            return Result<ConsoleSettingId>::Failure(lease.Error());
        }

        setting.enumLease = Move(lease.Value());
        setting.enumSchema = &setting.enumLease.Enum();
    }

    Result<void> filled = setting.name.TryAssign(definition.name);
    if (filled) {
        filled = setting.help.TryAssign(definition.help);
    }

    for (usize index = 0; filled && index < definition.allowedValues.Size(); ++index) {
        String allowed(*allocator_);
        filled = allowed.TryAssign(definition.allowedValues[index]);
        if (filled) {
            filled = setting.allowed.TryPushBack(Move(allowed));
        }
    }

    if (!filled) {
        return Result<ConsoleSettingId>::Failure(filled.Error());
    }

    // 기본값: 표기가 있으면 그 값(범위·허용 값도 맞아야 한다), 없으면 종류의 0 값.
    setting.defaultValue.kind = kind;
    if (setting.enumSchema != nullptr) {
        setting.defaultValue.type = setting.enumSchema->id;
        setting.defaultValue.signedInteger = EnumReplacementValue(*setting.enumSchema, 0);
    }

    if (!definition.defaultValue.IsEmpty()) {
        ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
        FieldValue parsed(*allocator_);
        auto checked = detail::CheckConsoleValue(*state, setting, ConfigLayer::CodeDefault,
                                                 definition.defaultValue, parsed, code);
        if (!checked) {
            return Result<ConsoleSettingId>::Failure(checked.Error());
        }

        if (!checked.Value().IsEmpty()) {
            return Result<ConsoleSettingId>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto copied = detail::CopyFieldValue(parsed, setting.defaultValue); !copied) {
            return Result<ConsoleSettingId>::Failure(copied.Error());
        }
    }

    // 이미 들어온 층 입력(대기 값)을 바로 넘긴다. 거부한 값은 진단을 남긴다.
    Array<ConfigEntry*> order(*allocator_);
    for (UniquePtr<ConfigEntry>& entry : state->entries) {
        if (auto pushed = order.TryPushBack(entry.Get()); !pushed) {
            return Result<ConsoleSettingId>::Failure(pushed.Error());
        }
    }

    order.Sort([](const ConfigEntry* left, const ConfigEntry* right) noexcept {
        return left->layer != right->layer ? left->layer < right->layer
                                           : left->sequence < right->sequence;
    });
    if (auto computed = detail::RecomputeConsoleSetting(
            *state, setting, order.View(), true, setting.value, setting.text, setting.sources);
        !computed) {
        return Result<ConsoleSettingId>::Failure(computed.Error());
    }

    if (auto pushed = state->settings.TryPushBack(Move(made.Value())); !pushed) {
        return Result<ConsoleSettingId>::Failure(pushed.Error());
    }

    // snapshot·콜백 판정이 이 설정을 보도록 다음 Sync가 다시 합친다.
    state->dirty = true;
    ConsoleSettingId id;
    id.index = static_cast<uint32>(state->settings.Size() - 1);
    return Result<ConsoleSettingId>::Success(id);
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

Result<void>
ConfigContext::RegisterConsoleCommand(const ConsoleCommandDefinition& definition) noexcept {
    return RegisterConsoleCommandType(definition, nullptr);
}

Result<void>
ConfigContext::RegisterConsoleCommandType(const ConsoleCommandDefinition& definition,
                                          const detail::SettingsTypeOps* arguments) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    StringView region;
    StringView rest;
    bool known = false;
    if (IsConsoleSettingName(definition.name) &&
        definition.name.Split(".", region, rest, StringComparison::CaseSensitive)) {
        for (const String& candidate : state->regions) {
            known = known || candidate.View() == region;
        }
    }

    if (!known || definition.help.IsEmpty() || definition.execute == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (state->FindSetting(definition.name) != IndexNone ||
        state->FindCommand(definition.name) != IndexNone) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto made = UniquePtr<ConsoleCommandState>::TryMake(*allocator_, *allocator_);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    ConsoleCommandState& command = *made.Value();
    command.execute = definition.execute;
    command.context = definition.context;
    if (arguments != nullptr) {
        auto lease = objects_->Types().AcquireValueType(arguments->id, TypeKind::Struct);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        command.lease = Move(lease.Value());
        command.args = *arguments;
    }

    Result<void> filled = command.name.TryAssign(definition.name);
    if (filled) {
        filled = command.help.TryAssign(definition.help);
    }

    if (filled) {
        filled = state->commands.TryPushBack(Move(made.Value()));
    }

    return filled;
}

namespace {

// 콘솔 설정 하나의 값·출처 스택을 출력한다(`<이름>`).
Result<void> ShowSetting(ConfigState& state, const ConsoleSettingState& setting,
                         ConsoleOutput& output) noexcept {
    StringBuilder<128> line(state.allocator);
    static_cast<void>(line.TryAppendFormat("{} = {}\n", setting.name.View(), setting.text.View()));
    if (auto status = line.Status(); !status) {
        return status;
    }

    if (auto written = Write(output, line.View()); !written) {
        return written;
    }

    if (setting.section == IndexNone) {
        return detail::WriteStack(state, setting.sources.View(), output);
    }

    const SectionState& section = *state.sections[setting.section];
    const StringView field =
        ToView(EffectiveToolName(section.lease.Schema().fields[setting.field]));
    for (const UniquePtr<detail::SourceStack>& stack : section.sources) {
        if (stack->path.View() == field) {
            return detail::WriteStack(state, stack->entries.View(), output);
        }
    }

    return Result<void>::Success();
}

// 도움말을 출력한다(`<이름> ?`).
Result<void> ShowHelp(ConfigState& state, StringView name, StringView help,
                      ConsoleOutput& output) noexcept {
    StringBuilder<128> line(state.allocator);
    static_cast<void>(line.TryAppendFormat("{}: {}\n", name, help));
    if (auto status = line.Status(); !status) {
        return status;
    }

    return Write(output, line.View());
}

// 방금 더해진 진단(from부터)을 출력한다.
Result<void> ShowDiagnostics(ConfigState& state, usize from, ConsoleOutput& output) noexcept {
    for (usize index = from; index < state.diagnostics.Count(); ++index) {
        const ConfigDiagnostic item = state.diagnostics.Get(index);
        StringBuilder<128> line(state.allocator);
        static_cast<void>(line.TryAppendFormat(
            "error {} {}: {}\n", ConfigDiagnosticCodeName(item.code), item.path, item.message));
        if (auto status = line.Status(); !status) {
            return status;
        }

        if (auto written = Write(output, line.View()); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<ConsoleLineResult> ConfigContext::ExecuteConsoleLine(StringView line,
                                                            ConsoleOutput output) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::WrongThread);
    }

    // 출시 빌드에는 콘솔 입력이 없다(UE ALLOW_CONSOLE_IN_SHIPPING 기본 0).
    if (state->releaseRules) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::Unsupported);
    }

    const StringView trimmed = line.TrimAscii();
    if (trimmed.IsEmpty()) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::InvalidArgument);
    }

    StringView name = trimmed;
    StringView rest;
    if (StringView left; trimmed.Split(" ", left, rest, StringComparison::CaseSensitive)) {
        name = left;
        rest = rest.TrimAscii();
    }

    auto succeed = [](ConsoleLineResult result) noexcept {
        return Result<ConsoleLineResult>::Success(result);
    };
    auto fail = [](Result<void> result) noexcept {
        return Result<ConsoleLineResult>::Failure(result.Error());
    };

    const usize setting = state->FindSetting(name);
    const bool hidden =
        setting != IndexNone && state->releaseRules &&
        HasFlag(state->settings[setting]->flags, ConsoleSettingFlags::DevelopmentOnly);
    if (setting != IndexNone && !hidden) {
        const ConsoleSettingState& found = *state->settings[setting];
        if (rest.IsEmpty()) {
            auto shown = ShowSetting(*state, found, output);
            return shown ? succeed(ConsoleLineResult::Shown) : fail(Move(shown));
        }

        if (rest == "?") {
            auto shown = ShowHelp(*state, found.name.View(), found.help.View(), output);
            return shown ? succeed(ConsoleLineResult::Help) : fail(Move(shown));
        }

        const usize before = state->diagnostics.Count();
        auto added = AddAssignment(ConfigLayer::Console, "console", name, rest);
        if (added) {
            return succeed(ConsoleLineResult::Queued);
        }

        if (added.Error() != ErrorCode::InvalidArgument) {
            return fail(Move(added));
        }

        auto shown = ShowDiagnostics(*state, before, output);
        return shown ? succeed(ConsoleLineResult::Rejected) : fail(Move(shown));
    }

    const usize command = state->FindCommand(name);
    if (command == IndexNone) {
        StringBuilder<128> message(*allocator_);
        static_cast<void>(message.TryAppendFormat("unknown console name '{}'\n", name));
        if (auto status = message.Status(); !status) {
            return fail(Move(status));
        }

        auto written = Write(output, message.View());
        return written ? succeed(ConsoleLineResult::Unknown) : fail(Move(written));
    }

    const ConsoleCommandState& definition = *state->commands[command];
    if (rest == "?") {
        auto shown = ShowHelp(*state, definition.name.View(), definition.help.View(), output);
        return shown ? succeed(ConsoleLineResult::Help) : fail(Move(shown));
    }

    // 인자: 공백으로 나눈 `<경로>=<값>`을 인자 struct 기본값 위에 대입한다.
    void* arguments = nullptr;
    if (definition.args.create != nullptr) {
        arguments = definition.args.create(*allocator_);
        if (arguments == nullptr) {
            return Result<ConsoleLineResult>::Failure(ErrorCode::OutOfMemory);
        }
    }

    auto release = [&]() noexcept {
        if (arguments != nullptr) {
            definition.args.destroy(*allocator_, arguments);
            arguments = nullptr;
        }
    };
    Array<reflection_text::TextAssignment> assignments(*allocator_);
    StringView remaining = rest;
    bool malformed = false;
    while (!remaining.IsEmpty()) {
        StringView token = remaining;
        StringView after;
        if (StringView left; remaining.Split(" ", left, after, StringComparison::CaseSensitive)) {
            token = left;
        }

        remaining = after.TrimAscii();
        reflection_text::TextAssignment assignment;
        if (!reflection_text::MakeTextAssignment(token, static_cast<uint32>(assignments.Size()),
                                                 assignment)) {
            malformed = true;
            break;
        }

        if (auto pushed = assignments.TryPushBack(assignment); !pushed) {
            release();
            return fail(Move(pushed));
        }
    }

    if (malformed || (arguments == nullptr && !assignments.IsEmpty())) {
        release();
        auto written = Write(output, arguments == nullptr && !malformed
                                         ? StringView("this command takes no arguments\n")
                                         : StringView("arguments are <path>=<value> separated by "
                                                      "spaces\n"));
        return written ? succeed(ConsoleLineResult::Rejected) : fail(Move(written));
    }

    if (!assignments.IsEmpty()) {
        reflection_text::TextDiagnostics diagnostics(*allocator_);
        const reflection_text::TextCodecOptions options = state->CodecOptions();
        auto prepared = reflection_text::PrepareAssignments(
            reflection_text::AssignmentTarget::Value(*objects_, state->access, definition.lease,
                                                     arguments, *allocator_),
            assignments.View(), options, diagnostics);
        Result<void> published = Result<void>::Success();
        if (prepared) {
            auto done = prepared.Value().Publish();
            if (!done) {
                published = Result<void>::Failure(done.Error());
            }
        }

        if (!prepared || !published) {
            release();
            if (!diagnostics.HasErrors()) {
                return Result<ConsoleLineResult>::Failure(prepared ? published.Error()
                                                                   : prepared.Error());
            }

            for (usize index = 0; index < diagnostics.Count(); ++index) {
                const reflection_text::TextDiagnostic item = diagnostics.Get(index);
                StringBuilder<128> message(*allocator_);
                static_cast<void>(message.TryAppendFormat(
                    "error {} {}: {}\n", reflection_text::TextDiagnosticCodeName(item.code),
                    item.path, item.message));
                if (auto written = Write(output, message.View()); !written) {
                    return fail(Move(written));
                }
            }

            return succeed(ConsoleLineResult::Rejected);
        }
    }

    PendingCommand pending;
    pending.command = command;
    pending.arguments = arguments;
    pending.output = output;
    if (auto pushed = state->pendingCommands.TryPushBack(pending); !pushed) {
        release();
        return fail(Move(pushed));
    }

    return succeed(ConsoleLineResult::CommandQueued);
}

Result<void> ConfigContext::CompleteConsoleName(StringView prefix,
                                                Array<StringView>& out) const noexcept {
    const ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    out.Clear();
    for (const UniquePtr<ConsoleSettingState>& setting : state->settings) {
        const bool hidden =
            state->releaseRules && HasFlag(setting->flags, ConsoleSettingFlags::DevelopmentOnly);
        if (!hidden && setting->name.View().StartsWith(prefix, StringComparison::CaseSensitive)) {
            if (auto pushed = out.TryPushBack(setting->name.View()); !pushed) {
                return pushed;
            }
        }
    }

    for (const UniquePtr<ConsoleCommandState>& command : state->commands) {
        if (command->name.View().StartsWith(prefix, StringComparison::CaseSensitive)) {
            if (auto pushed = out.TryPushBack(command->name.View()); !pushed) {
                return pushed;
            }
        }
    }

    out.Sort([](StringView left, StringView right) noexcept { return LessBytes(left, right); });
    return Result<void>::Success();
}

Result<ConfigSubscriptionId>
ConfigContext::Subscribe(StringView name, ConfigChangeCallback callback, void* context) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || callback == nullptr) {
        return Result<ConfigSubscriptionId>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<ConfigSubscriptionId>::Failure(ErrorCode::WrongThread);
    }

    const usize setting = state->FindSetting(name);
    if (setting == IndexNone) {
        return Result<ConfigSubscriptionId>::Failure(ErrorCode::NotFound);
    }

    Subscription subscription;
    subscription.id = state->nextSubscription++;
    subscription.setting = setting;
    subscription.callback = callback;
    subscription.context = context;
    if (auto pushed = state->subscriptions.TryPushBack(subscription); !pushed) {
        return Result<ConfigSubscriptionId>::Failure(pushed.Error());
    }

    return Result<ConfigSubscriptionId>::Success(ConfigSubscriptionId{subscription.id});
}

Result<ConfigSubscriptionId> ConfigContext::SubscribeSection(TypeId settings,
                                                             ConfigChangeCallback callback,
                                                             void* context) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || callback == nullptr) {
        return Result<ConfigSubscriptionId>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<ConfigSubscriptionId>::Failure(ErrorCode::WrongThread);
    }

    usize section = IndexNone;
    for (usize index = 0; index < state->sections.Size(); ++index) {
        if (state->sections[index]->ops.id == settings) {
            section = index;
        }
    }

    if (section == IndexNone) {
        return Result<ConfigSubscriptionId>::Failure(ErrorCode::NotFound);
    }

    Subscription subscription;
    subscription.id = state->nextSubscription++;
    subscription.section = section;
    subscription.callback = callback;
    subscription.context = context;
    if (auto pushed = state->subscriptions.TryPushBack(subscription); !pushed) {
        return Result<ConfigSubscriptionId>::Failure(pushed.Error());
    }

    return Result<ConfigSubscriptionId>::Success(ConfigSubscriptionId{subscription.id});
}

Result<void> ConfigContext::Unsubscribe(ConfigSubscriptionId id) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    for (usize index = 0; index < state->subscriptions.Size(); ++index) {
        Subscription& subscription = state->subscriptions[index];
        if (subscription.id != id.value || subscription.callback == nullptr) {
            continue;
        }

        // 콜백 도중이면 자리를 지우지 않고 표시만 한다(Sync가 끝에 정리한다).
        if (state->syncing) {
            subscription.callback = nullptr;
            return Result<void>::Success();
        }

        return state->subscriptions.TryRemoveAt(index);
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ConfigContext::CompleteStartup() noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    state->startupComplete = true;
    return Result<void>::Success();
}

SharedPtr<ConfigSnapshot> ConfigContext::Snapshot() const noexcept {
    const ConfigState* state = state_.Get();
    if (state == nullptr) {
        return SharedPtr<ConfigSnapshot>();
    }

    if (platform::CurrentThreadId() != state->owner) {
        platform::Fatal("ConfigContext used from a thread other than its owner");
    }

    return state->snapshot;
}

} // namespace rocks::config
