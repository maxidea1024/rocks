#include "ConfigState.h"
#include "ConfigText.h"

#include <Config/CVar.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>

// 콘솔 입력과 명령이다(Docs/Design/CVarSystem.ko.md 13·15절 — CVR6): 콘솔 입력 문법(`<이름>`·`?`·
// `<값>`·`reset`·명령), Explain·출처 스택 표시, 내장 `cvar.find`·`cvar.explain`·`cvar.dump`, 명령
// 인자 읽기와 실행(Sync 5단계), 다른 스레드 입력의 이름 색인, 치트 허용 상태·네트워크 역할 권한,
// 복제 목록 Export/Import, 콘솔 값 덤프.
namespace rocks::cvar {

constinit CVar<bool> CheatsEnabled = cvar::Var("cheat.enabled", true)
                                         .Help("Allow cheat console writes and cheat commands")
                                         .Replicated()
                                         .ConsoleReadOnly();

} // namespace rocks::cvar

namespace rocks::config {

namespace detail {

namespace {

StringView ConsoleInputToView(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
}

// 콘솔 층 쓰기의 태그다(콘솔 입력 하나의 이름마다 앞 쓰기를 바꾼다).
constexpr StringView ConsoleTag = "console";

// 서버 층 복제 목록의 태그다.
constexpr StringView ServerTag = "server";

Result<void> Write(ConsoleSink* output, StringView text) noexcept {
    return output == nullptr ? Result<void>::Success() : output->Write(text);
}

// text의 첫 토큰(공백·탭으로 나눈다)과 나머지(앞뒤 공백을 뗀다)다. 따옴표 묶음은 없다(15절).
void SplitToken(StringView text, StringView& first, StringView& rest) noexcept {
    const StringView trimmed = text.TrimAscii();
    usize at = 0;
    while (at < trimmed.ByteCount() && trimmed.Data()[at] != ' ' && trimmed.Data()[at] != '\t') {
        ++at;
    }

    first = StringView(trimmed.Data(), at);
    rest = StringView(trimmed.Data() + at, trimmed.ByteCount() - at).TrimAscii();
}

// 서식 한 줄을 output에 쓴다(끝에 줄바꿈).
template <class... Args>
Result<void> WriteLine(ConfigState& state, ConsoleSink* output, const char* format,
                       const Args&... args) noexcept {
    StringBuilder<192> line(state.allocator);
    static_cast<void>(line.TryAppendFormat(format, args...));
    static_cast<void>(line.TryAppend("\n"));
    if (auto status = line.Status(); !status) {
        return status;
    }

    return Write(output, line.View());
}

uint32 VarFlagsOf(const ConsoleSettingState& setting) noexcept {
    return setting.definition != nullptr ? setting.definition->flags : 0;
}

bool IsHidden(const ConfigState& state, uint32 consoleFlags) noexcept {
    return state.releaseRules && HasFlag(consoleFlags, ConsoleSettingFlags::DevelopmentOnly);
}

// 값 종류의 표시 이름이다.
StringView KindName(const ConsoleSettingState& setting) noexcept {
    switch (setting.kind) {
    case ValueKind::Bool:
        return "bool";
    case ValueKind::Int32:
        return "int32";
    case ValueKind::Int64:
        return "int64";
    case ValueKind::Float32:
        return "float";
    case ValueKind::Float64:
        return "double";
    case ValueKind::Utf8String:
        return "string";
    case ValueKind::Name:
        return "name";
    case ValueKind::Enum:
        // 이름 없는 schema(CVar enum)는 "enum"으로 쓴다.
        if (setting.enumSchema != nullptr &&
            !ConsoleInputToView(setting.enumSchema->name).IsEmpty()) {
            return ConsoleInputToView(setting.enumSchema->name);
        }

        return "enum";
    default:
        return "value";
    }
}

// 값 표기다(enum은 `3 (High)` — 15절).
Result<void> AppendValue(const ConsoleSettingState& setting, const FieldValue& value,
                         StringView text, StringBuilderBase& out) noexcept {
    if (setting.kind == ValueKind::Enum) {
        static_cast<void>(out.TryAppendFormat("{} ({})", value.signedInteger, text));
    } else {
        static_cast<void>(out.TryAppend(text));
    }

    return out.Status();
}

// 입력 하나가 이 설정에 준 값의 표기다(콘솔 대입은 그 텍스트, 파일 `Settings`는 JSON 값).
Result<void> ContributionOf(const ConsoleSettingState& setting, const ConfigEntry& entry,
                            String& out) noexcept {
    out.Clear();
    if (entry.kind == EntryKind::Console || entry.kind == EntryKind::Assignment) {
        return out.TryAssign(entry.value.View());
    }

    const JsonValue settings = SettingsSectionOf(entry);
    const JsonValue member = settings.IsObject() ? settings.Find(setting.name.View()) : JsonValue{};
    if (member.IsValid()) {
        auto matched = ConsoleTextOfJson(member, setting.kind, out);
        if (!matched) {
            return Result<void>::Failure(matched.Error());
        }

        if (matched.Value()) {
            return Result<void>::Success();
        }
    }

    return out.TryAssign("(file)");
}

// 설정의 출처 스택(입력 순번, 약한 것부터)이다.
Span<const uint64> SourcesOf(const ConfigState& state,
                             const ConsoleSettingState& setting) noexcept {
    if (setting.section == IndexNone) {
        return setting.sources.View();
    }

    const SectionState& section = *state.sections[setting.section];
    const StringView field =
        ConsoleInputToView(EffectiveToolName(section.lease.Schema().fields[setting.field]));
    for (const UniquePtr<SourceStack>& stack : section.sources) {
        if (stack->path.View() == field) {
            return stack->entries.View();
        }
    }

    return {};
}

// 값·출처 스택을 쓴다(`<이름>`, explain이면 종류·범위·도움말도 — 15절).
Result<void> ShowSetting(ConfigState& state, const ConsoleSettingState& setting, bool explain,
                         ConsoleSink* output) noexcept {
    StringBuilder<256> head(state.allocator);
    static_cast<void>(head.TryAppendFormat("{} = ", setting.name.View()));
    if (auto appended = AppendValue(setting, setting.value, setting.text.View(), head); !appended) {
        return appended;
    }

    if (explain) {
        static_cast<void>(head.TryAppendFormat(" ({}", KindName(setting)));
        const cvar::detail::VarDefinition* definition = setting.definition;
        if (definition != nullptr && cvar::HasFlag(definition->flags, cvar::VarFlags::HasRange)) {
            if (setting.kind == ValueKind::Float32 || setting.kind == ValueKind::Float64) {
                static_cast<void>(head.TryAppendFormat(", range {}..{}", definition->minimum.real,
                                                       definition->maximum.real));
            } else {
                static_cast<void>(head.TryAppendFormat(
                    ", range {}..{}", definition->minimum.integer, definition->maximum.integer));
            }

            if (cvar::HasFlag(definition->flags, cvar::VarFlags::Clamp)) {
                static_cast<void>(head.TryAppend(", clamp"));
            }
        } else if (setting.hasRange) {
            static_cast<void>(
                head.TryAppendFormat(", range {}..{}", setting.minimum, setting.maximum));
        }

        static_cast<void>(head.TryAppend(")"));
    }

    if (HasBootFlags(setting.flags)) {
        static_cast<void>(head.TryAppend(" [boot]"));
    }

    static_cast<void>(head.TryAppend("\n"));
    if (explain && !setting.help.IsEmpty()) {
        static_cast<void>(head.TryAppendFormat("  {}\n", setting.help.View()));
    }

    for (const UniquePtr<PendingRestart>& pending : state.pendingRestarts) {
        if (pending->name.View() == setting.name.View()) {
            static_cast<void>(
                head.TryAppendFormat("  pending restart: {}\n", pending->pending.View()));
        }
    }

    // 품질 묶음 변수면 펼친 등급(11절 — `quality.shadow@1 (requested 3, lowered: <사유>)`).
    for (const UniquePtr<GroupState>& group : state.groups) {
        if (group->expanded && group->name.View() == setting.name.View()) {
            QualityResolution resolution(state.allocator);
            static_cast<void>(resolution.group.TryAssign(group->name.View()));
            static_cast<void>(resolution.reason.TryAssign(group->reason.View()));
            resolution.requested = group->requested;
            resolution.applied = group->applied;
            resolution.unmet = group->unmet;
            String text(state.allocator);
            if (auto formatted = FormatQualityResolution(resolution, text); !formatted) {
                return formatted;
            }

            static_cast<void>(head.TryAppendFormat("  expands {}\n", text.View()));
        }
    }

    if (auto status = head.Status(); !status) {
        return status;
    }

    if (auto written = Write(output, head.View()); !written) {
        return written;
    }

    // 범위 값(8절 — Global 유효값보다 강하다): `scope <id> <출처 층> <값> <출처>`.
    if (setting.definition != nullptr) {
        const uint32 index =
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&setting.definition->index);
        for (const SharedPtr<ScopeState>& scope : state.scopes) {
            for (const SharedPtr<const ScopeValue>& value : scope->values) {
                if (value->index != index) {
                    continue;
                }

                if (auto written = WriteLine(state, output, "  scope {} {} {} {}", scope->id,
                                             ConfigLayerName(value->layer), value->text.View(),
                                             value->source.View());
                    !written) {
                    return written;
                }
            }
        }
    }

    // 출처 스택은 강한 것부터, 마지막이 코드 기본값이다.
    const Span<const uint64> sources = SourcesOf(state, setting);
    String contribution(state.allocator);
    for (usize position = sources.Size(); position-- > 0;) {
        const ConfigEntry* entry = state.FindEntry(sources[position]);
        if (entry == nullptr) {
            continue;
        }

        if (auto described = ContributionOf(setting, *entry, contribution); !described) {
            return described;
        }

        if (auto written = WriteLine(state, output, "  {} {} {}{}", ConfigLayerName(entry->layer),
                                     contribution.View(), entry->sourceName.View(),
                                     position + 1 == sources.Size() ? "  <- applied" : "");
            !written) {
            return written;
        }
    }

    String defaultText(state.allocator);
    if (auto formatted = FormatConsoleValue(state, setting, setting.defaultValue, defaultText);
        !formatted) {
        return formatted;
    }

    return WriteLine(state, output, "  Default {} code{}", defaultText.View(),
                     sources.IsEmpty() ? "  <- applied" : "");
}

// 명령의 도움말과 매개변수다(`<명령> ?`).
Result<void> ShowCommand(ConfigState& state, const ConsoleCommandState& command,
                         ConsoleSink* output) noexcept {
    StringBuilder<256> line(state.allocator);
    static_cast<void>(line.TryAppendFormat("{} (command", command.name.View()));
    const cvar::detail::CommandDefinition& definition = *command.definition;
    for (uint32 index = 0; index < definition.argumentCount; ++index) {
        ConsoleSettingState shape(state.allocator);
        shape.kind = ConsoleValueKind(definition.arguments[index].type);
        shape.enumSchema = definition.arguments[index].enumSchema != nullptr
                               ? &definition.arguments[index].enumSchema()
                               : nullptr;
        static_cast<void>(line.TryAppendFormat(" <{}>", KindName(shape)));
    }

    static_cast<void>(line.TryAppendFormat(")\n  {}\n", command.help.View()));
    if (auto status = line.Status(); !status) {
        return status;
    }

    return Write(output, line.View());
}

// 명령 인자를 매개변수마다 나눈다(공백 — 마지막 StringView 매개변수는 줄의 나머지). 수가 맞지
// 않으면 사유를 돌려준다.
Result<StringView> SplitArguments(const cvar::detail::CommandDefinition& definition,
                                  StringView text, Array<StringView>& out) noexcept {
    out.Clear();
    StringView remaining = text.TrimAscii();
    for (uint32 index = 0; index < definition.argumentCount; ++index) {
        const bool last = index + 1 == definition.argumentCount;
        if (last && definition.takesRest) {
            if (auto pushed = out.TryPushBack(remaining); !pushed) {
                return Result<StringView>::Failure(pushed.Error());
            }

            remaining = {};
            break;
        }

        if (remaining.IsEmpty()) {
            return Result<StringView>::Success("needs more arguments");
        }

        StringView token;
        StringView after;
        SplitToken(remaining, token, after);
        remaining = after;
        if (auto pushed = out.TryPushBack(token); !pushed) {
            return Result<StringView>::Failure(pushed.Error());
        }
    }

    if (!remaining.IsEmpty()) {
        return Result<StringView>::Success("takes fewer arguments");
    }

    return Result<StringView>::Success(StringView{});
}

// 명령 인자를 읽는다(매개변수 타입의 ReflectionText 단일 값 표기 — enum은 이름 또는 수). 맞지
// 않으면 사유를 돌려준다.
Result<StringView> ParseArguments(ConfigState& state,
                                  const cvar::detail::CommandDefinition& definition,
                                  StringView text, Array<FieldValue>& out) noexcept {
    Array<StringView> tokens(state.allocator);
    auto split = SplitArguments(definition, text, tokens);
    if (!split || !split.Value().IsEmpty()) {
        return split;
    }

    out.Clear();
    for (uint32 index = 0; index < definition.argumentCount; ++index) {
        ConsoleSettingState shape(state.allocator);
        shape.type = definition.arguments[index].type;
        shape.kind = ConsoleValueKind(shape.type);
        shape.enumSchema = definition.arguments[index].enumSchema != nullptr
                               ? &definition.arguments[index].enumSchema()
                               : nullptr;
        FieldValue value(state.allocator);
        ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
        auto checked = CheckConsoleValue(state, shape, ConfigLayer::Console, tokens[index], value,
                                         code, false);
        if (!checked) {
            return checked;
        }

        if (!checked.Value().IsEmpty()) {
            return checked;
        }

        if (auto pushed = out.TryPushBack(Move(value)); !pushed) {
            return Result<StringView>::Failure(pushed.Error());
        }
    }

    return Result<StringView>::Success(StringView{});
}

// 콘솔 층 쓰기(`<이름> <값>`)를 만든다(같은 이름의 앞 콘솔 쓰기를 바꾼다).
Result<void> MakeConsoleWrite(ConfigState& state, StringView name, StringView value,
                              SharedPtr<QueuedChange>& out) noexcept {
    if (auto prepared =
            PrepareAssignment(state, ConfigLayer::Console, ConsoleTag, name, value, out);
        !prepared) {
        return prepared;
    }

    out->layer = ConfigLayer::Console;
    out->replace = true;
    if (auto assigned = out->tag.TryAssign(ConsoleTag); !assigned) {
        return assigned;
    }

    return out->path.TryAssign(name);
}

// 콘솔 층 지우기(`<이름> reset`)를 만든다.
Result<void> MakeConsoleReset(ConfigState& state, StringView name,
                              SharedPtr<QueuedChange>& out) noexcept {
    auto change = SharedPtr<QueuedChange>::TryMake(state.allocator, state.allocator);
    if (!change) {
        return Result<void>::Failure(change.Error());
    }

    change.Value()->layer = ConfigLayer::Console;
    Result<void> assigned = change.Value()->tag.TryAssign(ConsoleTag);
    if (assigned) {
        assigned = change.Value()->path.TryAssign(name);
    }

    if (!assigned) {
        return assigned;
    }

    out = Move(change.Value());
    return Result<void>::Success();
}

// 문맥의 이름 색인 항목을 만든다.
Result<void> AddNameEntry(ConfigState& state, ConsoleNameIndex& index, StringView name,
                          bool command, uint32 consoleFlags, uint32 varFlags, bool field) noexcept {
    auto made = UniquePtr<ConsoleNameEntry>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    made.Value()->command = command;
    made.Value()->consoleFlags = consoleFlags;
    made.Value()->varFlags = varFlags;
    made.Value()->field = field;
    if (auto assigned = made.Value()->name.TryAssign(name); !assigned) {
        return assigned;
    }

    return index.entries.TryPushBack(Move(made.Value()));
}

// 게임 lane의 콘솔 입력 한 줄이다(runCommands면 명령을 큐에 넣지 않고 바로 실행한다 — 다른 스레드가
// 넣은 줄을 Sync 5단계에 처리할 때).
Result<ConsoleLineResult> ExecuteOnOwner(ConfigContext& context, ConfigState& state,
                                         StringView line, const SharedPtr<ConsoleSink>& sink,
                                         bool runCommands) noexcept;

// 명령 문맥을 sink로 쓰는 어댑터다(내장 명령이 표시 함수에 넘긴다).
class CommandSink final : public ConsoleSink {
public:
    explicit CommandSink(const cvar::CommandContext& context) noexcept : context_(context) {
    }

    Result<void> Write(StringView text) noexcept override {
        return context_.Write(text);
    }

private:
    const cvar::CommandContext& context_;
};

// 내장 명령 함수다(cvar.find·cvar.explain·cvar.dump — 15절).
Result<void> RunFind(cvar::CommandContext& context, StringView text) noexcept {
    ConfigState& state = *context.Config().State();
    Array<StringView> names(state.allocator);
    for (const UniquePtr<ConsoleSettingState>& setting : state.settings) {
        const StringView category =
            setting->definition != nullptr ? setting->definition->category : StringView{};
        if (!IsHidden(state, setting->flags) &&
            (setting->name.View().Contains(text) || setting->help.View().Contains(text) ||
             category.Contains(text))) {
            if (auto pushed = names.TryPushBack(setting->name.View()); !pushed) {
                return pushed;
            }
        }
    }

    for (const UniquePtr<ConsoleCommandState>& command : state.commands) {
        if (command->name.View().Contains(text) || command->help.View().Contains(text) ||
            command->definition->category.Contains(text)) {
            if (auto pushed = names.TryPushBack(command->name.View()); !pushed) {
                return pushed;
            }
        }
    }

    names.Sort([](StringView left, StringView right) noexcept { return LessBytes(left, right); });
    for (const StringView name : names) {
        const usize setting = state.FindSetting(name);
        StringBuilder<256> line(state.allocator);
        if (setting != IndexNone) {
            const ConsoleSettingState& found = *state.settings[setting];
            static_cast<void>(line.TryAppendFormat("{} = ", name));
            if (auto appended = AppendValue(found, found.value, found.text.View(), line);
                !appended) {
                return appended;
            }

            static_cast<void>(line.TryAppendFormat("  {}\n", found.help.View()));
        } else {
            const ConsoleCommandState& command = *state.commands[state.FindCommand(name)];
            static_cast<void>(
                line.TryAppendFormat("{} (command)  {}\n", name, command.help.View()));
        }

        if (auto status = line.Status(); !status) {
            return status;
        }

        if (auto written = context.Write(line.View()); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

Result<void> RunExplain(cvar::CommandContext& context, StringView name) noexcept {
    ConfigState& state = *context.Config().State();
    StringView first;
    StringView rest;
    SplitToken(name, first, rest);
    CommandSink output(context);
    if (first.IsEmpty() || !rest.IsEmpty()) {
        return Write(&output, "cvar.explain takes one name\n");
    }

    // 이름 하나의 설명만 쓴다(값 쓰기·명령 실행으로 넘기지 않는다).
    const usize setting = state.FindSetting(first);
    if (setting != IndexNone && !IsHidden(state, state.settings[setting]->flags)) {
        return ShowSetting(state, *state.settings[setting], true, &output);
    }

    const usize command = state.FindCommand(first);
    if (command != IndexNone) {
        return ShowCommand(state, *state.commands[command], &output);
    }

    return WriteLine(state, &output, "{} is not a console variable or command", first);
}

Result<void> RunDump(cvar::CommandContext& context, StringView mode) noexcept {
    const StringView trimmed = mode.TrimAscii();
    if (!trimmed.IsEmpty() && trimmed != "overrides") {
        return context.Write("cvar.dump takes no argument or 'overrides'\n");
    }

    String text(context.GetAllocator());
    if (auto dumped = context.Config().DumpConsoleValues(text, !trimmed.IsEmpty()); !dumped) {
        return dumped;
    }

    return context.Write(text.View());
}

} // namespace

// 문맥이 스스로 등록하는 콘솔 도구 명령이다(15절).
constinit cvar::ConsoleCommand FindConsoleCommand =
    cvar::Command<&RunFind>("cvar.find")
        .Help("List console variables and commands whose name, help or category contains the text");

constinit cvar::ConsoleCommand ExplainConsoleCommand =
    cvar::Command<&RunExplain>("cvar.explain")
        .Help("Show the value, type, range and source stack of a console variable");

constinit cvar::ConsoleCommand DumpConsoleCommand =
    cvar::Command<&RunDump>("cvar.dump")
        .Help("Write console values as a config file; 'overrides' writes only non-default values");

Result<void> RegisterBuiltinConsoleHandles(ConfigContext& context) noexcept {
    return context.Register(cvar::CheatsEnabled, FindConsoleCommand, ExplainConsoleCommand,
                            DumpConsoleCommand);
}

StringView CheckConsolePermission(uint32 varFlags, bool cheats, NetworkRole role,
                                  ConfigDiagnosticCode& code) noexcept {
    if (cvar::HasFlag(varFlags, cvar::VarFlags::Cheat) && !cheats) {
        code = ConfigDiagnosticCode::CheatsDisabled;
        return "is a cheat and cheats are disabled (cheat.enabled is false)";
    }

    if (cvar::HasFlag(varFlags, cvar::VarFlags::Replicated) && role == NetworkRole::Client) {
        code = ConfigDiagnosticCode::ReplicatedOnClient;
        return "is replicated from the server; a client cannot change it";
    }

    return {};
}

bool CheatsAllowed(const ConfigState& state) noexcept {
    const usize index = state.FindSetting(cvar::CheatsEnabled.Name());
    return index == IndexNone || state.settings[index]->value.boolean;
}

bool SnapshotCheatsAllowed(const ConfigSnapshot* snapshot, uint64 scope) noexcept {
    const cvar::Index index = cvar::CheatsEnabled.Index();
    if (snapshot == nullptr || !index.IsValid()) {
        return true;
    }

    // Global 실효값 AND 범위 사슬 값이다(범위는 치트를 좁히기만 한다 — 13.1절).
    const CVarSlot* slot = snapshot->FindSlot(index.value);
    const bool global = slot == nullptr || slot->bits != 0;
    const CVarText* text = nullptr;
    const CVarSlot* scoped = snapshot->FindScopedSlot(scope, index.value, text);
    return global && (scoped == nullptr || scoped->bits != 0);
}

Result<SharedPtr<const ConsoleNameIndex>> BuildConsoleNameIndex(ConfigState& state) noexcept {
    auto made = SharedPtr<ConsoleNameIndex>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<SharedPtr<const ConsoleNameIndex>>::Failure(made.Error());
    }

    ConsoleNameIndex& index = *made.Value();
    const Span<const UniquePtr<ConsoleSettingState>> settingLists[] = {state.settings.View(),
                                                                       state.registeringSettings};
    for (const Span<const UniquePtr<ConsoleSettingState>> settings : settingLists) {
        for (const UniquePtr<ConsoleSettingState>& setting : settings) {
            if (setting.Get() == nullptr || IsHidden(state, setting->flags)) {
                continue;
            }

            if (auto added = AddNameEntry(state, index, setting->name.View(), false, setting->flags,
                                          VarFlagsOf(*setting), setting->section != IndexNone);
                !added) {
                return Result<SharedPtr<const ConsoleNameIndex>>::Failure(added.Error());
            }
        }
    }

    const Span<const UniquePtr<ConsoleCommandState>> commandLists[] = {state.commands.View(),
                                                                       state.registeringCommands};
    for (const Span<const UniquePtr<ConsoleCommandState>> commands : commandLists) {
        for (const UniquePtr<ConsoleCommandState>& command : commands) {
            if (command.Get() == nullptr) {
                continue;
            }

            if (auto added = AddNameEntry(state, index, command->name.View(), true, 0,
                                          command->definition->flags, false);
                !added) {
                return Result<SharedPtr<const ConsoleNameIndex>>::Failure(added.Error());
            }
        }
    }

    index.entries.Sort([](const UniquePtr<ConsoleNameEntry>& left,
                          const UniquePtr<ConsoleNameEntry>& right) noexcept {
        return LessBytes(left->name.View(), right->name.View());
    });
    return Result<SharedPtr<const ConsoleNameIndex>>::Success(Move(made.Value()));
}

Result<bool> CheckEntryPermission(ConfigState& state, const ConfigEntry& entry, bool cheats,
                                  ConsoleSink* sink) noexcept {
    auto check = [&](StringView name) noexcept -> Result<bool> {
        const usize index = state.FindSetting(name);
        if (index == IndexNone) {
            return Result<bool>::Success(true);
        }

        ConfigDiagnosticCode code = ConfigDiagnosticCode::LayerNotAllowed;
        const StringView problem = CheckConsolePermission(VarFlagsOf(*state.settings[index]),
                                                          cheats, state.networkRole, code);
        if (problem.IsEmpty()) {
            return Result<bool>::Success(true);
        }

        if (auto reported = Report(state, code, ConfigSeverity::Error, entry.layer,
                                   entry.sourceName.View(), nullptr, name, problem);
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        static_cast<void>(WriteLine(state, sink, "{} {}", name, problem));
        return Result<bool>::Success(false);
    };

    if (entry.kind == EntryKind::Console) {
        return check(entry.path.View());
    }

    // 콘솔 층 `Settings` 문서는 멤버 하나라도 거부되면 문서 전체를 거부한다(9절 문서 입력과 같다).
    bool allowed = true;
    const JsonValue settings = SettingsSectionOf(entry);
    for (usize member = 0; settings.IsObject() && member < settings.Size(); ++member) {
        auto checked = check(settings.KeyAt(member));
        if (!checked) {
            return checked;
        }

        allowed = allowed && checked.Value();
    }

    return Result<bool>::Success(allowed);
}

Result<bool> RecheckConsolePermission(ConfigState& state, const QueuedChange& change,
                                      bool cheats) noexcept {
    return CheckEntryPermission(state, *change.entry, cheats, change.sink.Get());
}

bool ChangeTouchesCheats(const QueuedChange& change) noexcept {
    // 빼기는 그 자리의 값을 지우므로 늘 다시 계산한다.
    if (change.entry.Get() == nullptr || change.replace) {
        return true;
    }

    const ConfigEntry& entry = *change.entry;
    if (entry.kind == EntryKind::Console) {
        return entry.path.View() == cvar::CheatsEnabled.Name();
    }

    const JsonValue settings = SettingsSectionOf(entry);
    return settings.IsObject() && settings.Find(cvar::CheatsEnabled.Name()).IsValid();
}

Result<bool> CheatsOfPlan(ConfigState& state, Span<ConfigEntry* const> plan) noexcept {
    const usize index = state.FindSetting(cvar::CheatsEnabled.Name());
    if (index == IndexNone) {
        return Result<bool>::Success(true);
    }

    Array<ConfigEntry*> order(state.allocator);
    if (auto appended = order.TryAppend(plan); !appended) {
        return Result<bool>::Failure(appended.Error());
    }

    order.Sort([](const ConfigEntry* left, const ConfigEntry* right) noexcept {
        return left->layer != right->layer ? left->layer < right->layer
                                           : left->sequence < right->sequence;
    });
    FieldValue value(state.allocator);
    String text(state.allocator);
    Array<uint64> sources(state.allocator);
    if (auto computed = RecomputeConsoleSetting(state, *state.settings[index], order.View(), false,
                                                value, text, sources);
        !computed) {
        return Result<bool>::Failure(computed.Error());
    }

    return Result<bool>::Success(value.boolean);
}

Result<void> QueueConsoleLine(ConfigState& state, QueuedChange& change) noexcept {
    PendingCommand pending(state.allocator);
    pending.sink = change.sink;
    pending.arguments = Move(change.line);
    return state.pendingCommands.TryPushBack(Move(pending));
}

void RunPendingCommand(ConfigContext& context, ConfigState& state,
                       PendingCommand& pending) noexcept {
    // 다른 스레드가 낸 콘솔 입력 한 줄이다(게임 lane에서 처리하고 명령이면 바로 실행한다).
    if (pending.command == IndexNone) {
        static_cast<void>(
            ExecuteOnOwner(context, state, pending.arguments.View(), pending.sink, true));
        return;
    }

    const ConsoleCommandState& command = *state.commands[pending.command];
    ConfigDiagnosticCode code = ConfigDiagnosticCode::LayerNotAllowed;
    const StringView problem = CheckConsolePermission(
        command.definition->flags, CheatsAllowed(state), state.networkRole, code);
    if (!problem.IsEmpty()) {
        static_cast<void>(
            WriteLine(state, pending.sink.Get(), "{} {}", command.name.View(), problem));
        return;
    }

    Array<FieldValue> arguments(state.allocator);
    auto parsed = ParseArguments(state, *command.definition, pending.arguments.View(), arguments);
    if (!parsed || !parsed.Value().IsEmpty()) {
        static_cast<void>(WriteLine(state, pending.sink.Get(), "{} {}", command.name.View(),
                                    parsed ? parsed.Value() : ErrorCodeName(parsed.Error())));
        return;
    }

    cvar::CommandContext invocation(context, pending.sink.Get(), command.target, command.targetType,
                                    command.name.View(), state.allocator);
    if (auto executed = command.definition->invoke(invocation, arguments.Data()); !executed) {
        // 명령 실패는 그 명령의 콘솔 출력과 로그 양쪽에 남긴다.
        Log::Warning(LogConfig, "Console command {Command} failed ({Error})", command.name.View(),
                     ErrorCodeName(executed.Error()));
        static_cast<void>(WriteLine(state, pending.sink.Get(), "{}: command failed ({})",
                                    command.name.View(), ErrorCodeName(executed.Error())));
    }
}

namespace {

Result<ConsoleLineResult> ExecuteOnOwner(ConfigContext& context, ConfigState& state,
                                         StringView line, const SharedPtr<ConsoleSink>& sink,
                                         bool runCommands) noexcept {
    ConsoleSink* output = sink.Get();
    auto succeed = [](ConsoleLineResult result) noexcept {
        return Result<ConsoleLineResult>::Success(result);
    };
    auto fail = [](ErrorCode error) noexcept { return Result<ConsoleLineResult>::Failure(error); };
    auto reject = [&](StringView name, StringView problem) noexcept {
        auto written = WriteLine(state, output, "{} {}", name, problem);
        return written ? succeed(ConsoleLineResult::Rejected) : fail(written.Error());
    };

    StringView name;
    StringView rest;
    SplitToken(line, name, rest);
    const usize setting = state.FindSetting(name);
    if (setting != IndexNone && !IsHidden(state, state.settings[setting]->flags)) {
        const ConsoleSettingState& found = *state.settings[setting];
        if (rest.IsEmpty() || rest == "?") {
            const bool explain = !rest.IsEmpty();
            auto shown = ShowSetting(state, found, explain, output);
            return shown
                       ? succeed(explain ? ConsoleLineResult::Explained : ConsoleLineResult::Shown)
                       : fail(shown.Error());
        }

        SharedPtr<QueuedChange> change;
        if (rest == "reset") {
            if (auto made = MakeConsoleReset(state, found.name.View(), change); !made) {
                return fail(made.Error());
            }
        } else {
            ConfigDiagnosticCode code = ConfigDiagnosticCode::LayerNotAllowed;
            const StringView problem = CheckConsolePermission(
                VarFlagsOf(found), CheatsAllowed(state), state.networkRole, code);
            if (!problem.IsEmpty()) {
                if (auto reported = Report(state, code, ConfigSeverity::Error, ConfigLayer::Console,
                                           ConsoleTag, nullptr, name, problem);
                    !reported) {
                    return fail(reported.Error());
                }

                return reject(name, problem);
            }

            const usize before = state.diagnostics.Count();
            if (auto made = MakeConsoleWrite(state, found.name.View(), rest, change); made) {
                change->sink = sink;
            } else {
                if (made.Error() != ErrorCode::InvalidArgument) {
                    return fail(made.Error());
                }

                // 거부 사유(진단)를 `<이름> <사유>`로 쓴다.
                for (usize index = before; index < state.diagnostics.Count(); ++index) {
                    const ConfigDiagnostic item = state.diagnostics.Get(index);
                    if (auto written = WriteLine(state, output, "{} {}", name, item.message);
                        !written) {
                        return fail(written.Error());
                    }
                }

                return succeed(ConsoleLineResult::Rejected);
            }
        }

        SharedPtr<QueuedChange> one[] = {Move(change)};
        if (auto queued = Enqueue(*state.channel, Span<SharedPtr<QueuedChange>>(one), false);
            !queued) {
            return fail(queued.Error());
        }

        return succeed(ConsoleLineResult::Queued);
    }

    const usize command = state.FindCommand(name);
    if (command == IndexNone) {
        auto written = WriteLine(state, output, "{} is not a console variable or command", name);
        return written ? succeed(ConsoleLineResult::Unknown) : fail(written.Error());
    }

    const ConsoleCommandState& found = *state.commands[command];
    if (rest == "?") {
        auto shown = ShowCommand(state, found, output);
        return shown ? succeed(ConsoleLineResult::Explained) : fail(shown.Error());
    }

    ConfigDiagnosticCode code = ConfigDiagnosticCode::LayerNotAllowed;
    const StringView problem = CheckConsolePermission(found.definition->flags, CheatsAllowed(state),
                                                      state.networkRole, code);
    if (!problem.IsEmpty()) {
        return reject(name, problem);
    }

    // 인자를 지금 검사한다(실행 때 다시 읽는다).
    Array<FieldValue> arguments(state.allocator);
    auto parsed = ParseArguments(state, *found.definition, rest, arguments);
    if (!parsed) {
        return fail(parsed.Error());
    }

    if (!parsed.Value().IsEmpty()) {
        return reject(name, parsed.Value());
    }

    PendingCommand pending(state.allocator);
    pending.command = command;
    pending.sink = sink;
    if (auto assigned = pending.arguments.TryAssign(rest); !assigned) {
        return fail(assigned.Error());
    }

    if (runCommands) {
        RunPendingCommand(context, state, pending);
        return succeed(ConsoleLineResult::CommandQueued);
    }

    if (auto pushed = state.pendingCommands.TryPushBack(Move(pending)); !pushed) {
        return fail(pushed.Error());
    }

    return succeed(ConsoleLineResult::CommandQueued);
}

// 다른 스레드의 콘솔 입력이다(snapshot 이름 색인으로 해석하고 쓰기 큐에 넣는다 — 10.1·15절).
Result<ConsoleLineResult> ExecuteOffOwner(WriteChannel& channel, StringView line,
                                          const SharedPtr<ConsoleSink>& sink) noexcept {
    ConsoleSink* output = sink.Get();
    Allocator& allocator = channel.allocator;
    auto succeed = [](ConsoleLineResult result) noexcept {
        return Result<ConsoleLineResult>::Success(result);
    };
    auto fail = [](ErrorCode error) noexcept { return Result<ConsoleLineResult>::Failure(error); };
    auto say = [&](ConsoleLineResult result, StringView name,
                   StringView problem) noexcept -> Result<ConsoleLineResult> {
        StringBuilder<192> text(allocator);
        static_cast<void>(text.TryAppendFormat("{} {}\n", name, problem));
        if (auto status = text.Status(); !status) {
            return fail(status.Error());
        }

        auto written = Write(output, text.View());
        return written ? succeed(result) : fail(written.Error());
    };

    // 문맥이 없어졌으면 정의된 오류다(통로는 이 입구가 쥐고 있어 살아 있다).
    SharedPtr<const ConfigSnapshot> snapshot;
    {
        platform::LockGuard lock(channel.mutex);
        if (!channel.open) {
            return fail(ErrorCode::ShuttingDown);
        }

        snapshot = channel.current;
    }

    const StringView trimmed = line.TrimAscii();
    StringView name;
    StringView rest;
    SplitToken(trimmed, name, rest);

    const ConsoleNameIndex* names = snapshot.Get() != nullptr ? snapshot->ConsoleNames() : nullptr;
    const ConsoleNameEntry* entry = names != nullptr ? names->Find(name) : nullptr;
    if (entry == nullptr) {
        return say(ConsoleLineResult::Unknown, name, "is not a console variable or command");
    }

    auto change = SharedPtr<QueuedChange>::TryMake(allocator, allocator);
    if (!change) {
        return fail(change.Error());
    }

    QueuedChange& queued = *change.Value();
    queued.sink = sink;
    ConsoleLineResult result = ConsoleLineResult::CommandQueued;
    const auto role = static_cast<NetworkRole>(
        platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&channel.networkRole));
    if (entry->command || rest.IsEmpty() || rest == "?") {
        // 표시·Explain·명령은 게임 lane이 다음 Sync 5단계에 처리한다.
        // 명령 도움말(`<명령> ?`)은 치트가 꺼져 있어도 받는다(게임 lane과 같다).
        if (entry->command && rest != "?") {
            ConfigDiagnosticCode code = ConfigDiagnosticCode::LayerNotAllowed;
            const StringView problem = CheckConsolePermission(
                entry->varFlags, SnapshotCheatsAllowed(snapshot.Get()), role, code);
            if (!problem.IsEmpty()) {
                return say(ConsoleLineResult::Rejected, name, problem);
            }
        }

        if (auto assigned = queued.line.TryAssign(trimmed); !assigned) {
            return fail(assigned.Error());
        }
    } else if (rest == "reset") {
        queued.layer = ConfigLayer::Console;
        Result<void> assigned = queued.tag.TryAssign(ConsoleTag);
        if (assigned) {
            assigned = queued.path.TryAssign(entry->name.View());
        }

        if (!assigned) {
            return fail(assigned.Error());
        }

        result = ConsoleLineResult::Queued;
    } else {
        // 권한은 이 스레드의 snapshot으로 지금 보고, 값 형식은 Sync가 꺼낼 때 검사한다.
        ConfigDiagnosticCode code = ConfigDiagnosticCode::LayerNotAllowed;
        StringView problem = CheckConsolePermission(
            entry->varFlags, SnapshotCheatsAllowed(snapshot.Get()), role, code);
        if (problem.IsEmpty() &&
            HasFlag(entry->consoleFlags, ConsoleSettingFlags::ConsoleReadOnly)) {
            problem = "is read-only on the console";
        }

        if (problem.IsEmpty() && snapshot->IsSealed() && HasBootFlags(entry->consoleFlags)) {
            problem = BootSealedMessage;
        }

        if (!problem.IsEmpty()) {
            return say(ConsoleLineResult::Rejected, name, problem);
        }

        auto made = UniquePtr<ConfigEntry>::TryMake(allocator, allocator);
        if (!made) {
            return fail(made.Error());
        }

        ConfigEntry& added = *made.Value();
        added.kind = EntryKind::Console;
        added.layer = ConfigLayer::Console;
        StringBuilder<192> source(allocator);
        static_cast<void>(source.TryAppendFormat("{}={}", entry->name.View(), rest));
        Result<void> filled = source.Status();
        if (filled) {
            filled = added.tag.TryAssign(ConsoleTag);
        }

        if (filled) {
            filled = added.sourceName.TryAssign(source.View());
        }

        if (filled) {
            filled = added.path.TryAssign(entry->name.View());
        }

        if (filled) {
            filled = added.value.TryAssign(rest);
        }

        if (filled) {
            filled = queued.tag.TryAssign(ConsoleTag);
        }

        if (filled) {
            filled = queued.path.TryAssign(entry->name.View());
        }

        if (!filled) {
            return fail(filled.Error());
        }

        queued.entry = Move(made.Value());
        queued.layer = ConfigLayer::Console;
        queued.replace = true;
        queued.validateAtApply = true;
        result = ConsoleLineResult::Queued;
    }

    SharedPtr<QueuedChange> one[] = {Move(change.Value())};
    if (auto pushed = Enqueue(channel, Span<SharedPtr<QueuedChange>>(one), true); !pushed) {
        return fail(pushed.Error());
    }

    return succeed(result);
}

// JSON 문자열 하나를 붙인다(따옴표·escape).
void AppendJsonString(StringBuilderBase& out, StringView text) noexcept {
    static_cast<void>(out.TryAppend("\""));
    for (const char c : text) {
        switch (c) {
        case '"':
            static_cast<void>(out.TryAppend("\\\""));
            break;
        case '\\':
            static_cast<void>(out.TryAppend("\\\\"));
            break;
        case '\n':
            static_cast<void>(out.TryAppend("\\n"));
            break;
        case '\r':
            static_cast<void>(out.TryAppend("\\r"));
            break;
        case '\t':
            static_cast<void>(out.TryAppend("\\t"));
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                constexpr char Digits[] = "0123456789abcdef";
                const char escaped[] = {'\\',           'u', '0', '0', Digits[(c >> 4) & 0xF],
                                        Digits[c & 0xF]};
                static_cast<void>(out.TryAppend(StringView(escaped, 6)));
            } else {
                const char single[] = {c};
                static_cast<void>(out.TryAppend(StringView(single, 1)));
            }

            break;
        }
    }

    static_cast<void>(out.TryAppend("\""));
}

// 줄 주석에 넣을 텍스트를 붙인다(제어 문자는 escape — 줄바꿈이 주석을 끝내지 않게).
void AppendCommentText(StringBuilderBase& out, StringView text) noexcept {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            constexpr char Digits[] = "0123456789abcdef";
            const char escaped[] = {'\\', 'x', Digits[(c >> 4) & 0xF], Digits[c & 0xF]};
            static_cast<void>(out.TryAppend(StringView(escaped, 4)));
        } else {
            const char single[] = {c};
            static_cast<void>(out.TryAppend(StringView(single, 1)));
        }
    }
}

} // namespace

const ConsoleNameEntry* ConsoleNameIndex::Find(StringView name) const noexcept {
    usize low = 0;
    usize high = entries.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const StringView candidate = entries[middle]->name.View();
        if (candidate == name) {
            return entries[middle].Get();
        }

        if (LessBytes(candidate, name)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

} // namespace detail

using detail::ConfigState;

Result<ConsoleLineResult> ConfigContext::ExecuteConsoleLine(StringView line,
                                                            SharedPtr<ConsoleSink> sink) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || line.TrimAscii().IsEmpty()) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::InvalidArgument);
    }

    // 출시 빌드에는 콘솔 입력이 없다(UE ALLOW_CONSOLE_IN_SHIPPING 기본 0).
    if (state->releaseRules) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::Unsupported);
    }

    // 다른 스레드는 ConsoleInputPort로 들어온다(10.1절 — 문맥 상태를 건드리지 않는다).
    if (platform::CurrentThreadId() != state->owner) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::WrongThread);
    }

    return detail::ExecuteOnOwner(*this, *state, line, sink, false);
}

ConsoleInputPort ConfigContext::MakeConsoleInputPort() const noexcept {
    const ConfigState* state = state_.Get();
    return state != nullptr ? ConsoleInputPort(state->channel) : ConsoleInputPort();
}

Result<ConsoleLineResult> ConsoleInputPort::Execute(StringView line,
                                                    SharedPtr<ConsoleSink> sink) const noexcept {
    if (channel_.Get() == nullptr) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::ShuttingDown);
    }

    if (line.TrimAscii().IsEmpty()) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::InvalidArgument);
    }

    if (channel_->releaseRules) {
        return Result<ConsoleLineResult>::Failure(ErrorCode::Unsupported);
    }

    return detail::ExecuteOffOwner(*channel_, line, sink);
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
    for (const UniquePtr<detail::ConsoleSettingState>& setting : state->settings) {
        if (!detail::IsHidden(*state, setting->flags) &&
            setting->name.View().StartsWith(prefix, StringComparison::CaseSensitive)) {
            if (auto pushed = out.TryPushBack(setting->name.View()); !pushed) {
                return pushed;
            }
        }
    }

    for (const UniquePtr<detail::ConsoleCommandState>& command : state->commands) {
        if (command->name.View().StartsWith(prefix, StringComparison::CaseSensitive)) {
            if (auto pushed = out.TryPushBack(command->name.View()); !pushed) {
                return pushed;
            }
        }
    }

    out.Sort(
        [](StringView left, StringView right) noexcept { return detail::LessBytes(left, right); });
    return Result<void>::Success();
}

Result<void> ConfigContext::BindCommandTarget(const cvar::detail::VarDefinition& command,
                                              void* target, const void* targetType,
                                              const void* expected) noexcept {
    // 출시 빌드의 치트 명령은 등록되지 않는다(성공하는 무동작 — 호출하는 쪽이 빌드마다 갈리지
    // 않게).
    if constexpr (IsReleaseBuild) {
        if (cvar::HasFlag(command.flags, cvar::VarFlags::Cheat)) {
            return Result<void>::Success();
        }
    }

    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 묶기·풀기는 게임 lane 전용이다(대상의 파괴도 게임 lane에서 한다 — 명령은 게임 lane에서 대상을
    // 쓴다).
    if (platform::CurrentThreadId() != state->owner) {
        platform::Fatal(
            "ConfigContext::BindCommand/UnbindCommand called from a thread other than the "
            "context owner");
    }

    for (const UniquePtr<detail::ConsoleCommandState>& registered : state->commands) {
        if (registered->definition != &command) {
            continue;
        }

        if (target == nullptr && registered->target != expected) {
            return Result<void>::Success(); // 다른 대상이 묶여 있다(그대로 둔다).
        }

        registered->target = target;
        registered->targetType = target != nullptr ? targetType : nullptr;
        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ConfigContext::SetNetworkRole(NetworkRole role) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || static_cast<uint8>(role) > static_cast<uint8>(NetworkRole::Client)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    state->networkRole = role;
    platform::AtomicStore<platform::MemoryOrder::Relaxed>(&state->channel->networkRole,
                                                          static_cast<uint8>(role));
    return Result<void>::Success();
}

NetworkRole ConfigContext::GetNetworkRole() const noexcept {
    // 원자 사본을 읽는다(어느 스레드에서나 — 13.2절).
    const ConfigState* state = state_.Get();
    return state != nullptr
               ? static_cast<NetworkRole>(platform::AtomicLoad<platform::MemoryOrder::Relaxed>(
                     &state->channel->networkRole))
               : NetworkRole::Standalone;
}

Result<void> ConfigContext::ExportReplicated(Array<ReplicatedValue>& out) const noexcept {
    ConfigState* state = State();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    Array<ReplicatedValue> values(*allocator_);
    for (const UniquePtr<detail::ConsoleSettingState>& setting : state->settings) {
        if (!cvar::HasFlag(detail::VarFlagsOf(*setting), cvar::VarFlags::Replicated)) {
            continue;
        }

        if (auto pushed = values.TryPushBack({setting->name.View(), setting->text.View()});
            !pushed) {
            return pushed;
        }
    }

    values.Sort([](const ReplicatedValue& left, const ReplicatedValue& right) noexcept {
        return detail::LessBytes(left.name, right.name);
    });
    return out.TryAppend(values.View());
}

Result<usize> ConfigContext::ImportReplicated(Span<const ReplicatedValue> values) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<usize>::Failure(ErrorCode::WrongThread);
    }

    // 앞 목록을 통째로 바꾼다(서버 층 태그 하나).
    Array<SharedPtr<detail::QueuedChange>> changes(*allocator_);
    SharedPtr<detail::QueuedChange> removal;
    if (auto prepared =
            detail::PrepareUnset(*state, ConfigLayer::Server, detail::ServerTag, removal);
        !prepared) {
        return Result<usize>::Failure(prepared.Error());
    }

    if (auto pushed = changes.TryPushBack(Move(removal)); !pushed) {
        return Result<usize>::Failure(pushed.Error());
    }

    for (const ReplicatedValue& value : values) {
        const usize index = state->FindSetting(value.name);
        if (index == IndexNone || !cvar::HasFlag(detail::VarFlagsOf(*state->settings[index]),
                                                 cvar::VarFlags::Replicated)) {
            const ConfigDiagnosticCode code = index == IndexNone
                                                  ? ConfigDiagnosticCode::UnresolvedName
                                                  : ConfigDiagnosticCode::LayerNotAllowed;
            if (auto reported = detail::Report(*state, code, ConfigSeverity::Warning,
                                               ConfigLayer::Server, detail::ServerTag, nullptr,
                                               value.name, "is not a replicated console variable");
                !reported) {
                return Result<usize>::Failure(reported.Error());
            }

            continue;
        }

        SharedPtr<detail::QueuedChange> change;
        auto prepared = detail::PrepareAssignment(*state, ConfigLayer::Server, detail::ServerTag,
                                                  value.name, value.value, change);
        if (!prepared) {
            if (prepared.Error() != ErrorCode::InvalidArgument) {
                return Result<usize>::Failure(prepared.Error());
            }

            continue; // 값 진단을 남겼다.
        }

        if (auto pushed = changes.TryPushBack(Move(change)); !pushed) {
            return Result<usize>::Failure(pushed.Error());
        }
    }

    const usize added = changes.Size() - 1;
    if (auto queued = detail::Enqueue(*state->channel, changes.View(), false); !queued) {
        return Result<usize>::Failure(queued.Error());
    }

    return Result<usize>::Success(added);
}

Result<void> ConfigContext::DumpConsoleValues(String& out, bool overridesOnly) const noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    Array<const detail::ConsoleSettingState*> settings(*allocator_);
    String defaultText(*allocator_);
    for (const UniquePtr<detail::ConsoleSettingState>& setting : state->settings) {
        if (detail::IsHidden(*state, setting->flags)) {
            continue;
        }

        if (overridesOnly) {
            if (auto formatted = detail::FormatConsoleValue(*state, *setting, setting->defaultValue,
                                                            defaultText);
                !formatted) {
                return formatted;
            }

            if (defaultText.View() == setting->text.View()) {
                continue;
            }
        }

        if (auto pushed = settings.TryPushBack(setting.Get()); !pushed) {
            return pushed;
        }
    }

    settings.Sort([](const detail::ConsoleSettingState* left,
                     const detail::ConsoleSettingState* right) noexcept {
        return detail::LessBytes(left->name.View(), right->name.View());
    });
    StringBuilder<1024> text(*allocator_);
    static_cast<void>(text.TryAppend("{\n  \"Settings\": {\n"));
    for (usize index = 0; index < settings.Size(); ++index) {
        const detail::ConsoleSettingState& setting = *settings[index];
        const Span<const uint64> sources = detail::SourcesOf(*state, setting);
        const detail::ConfigEntry* top =
            sources.IsEmpty() ? nullptr : state->FindEntry(sources[sources.Size() - 1]);
        static_cast<void>(text.TryAppendFormat(
            "    // {}{} ", detail::HasBootFlags(setting.flags) ? "[boot] " : "",
            top != nullptr ? ConfigLayerName(top->layer) : StringView("Default")));
        detail::AppendCommentText(text,
                                  top != nullptr ? top->sourceName.View() : StringView("code"));
        static_cast<void>(text.TryAppend("\n"));
        static_cast<void>(text.TryAppend("    "));
        detail::AppendJsonString(text, setting.name.View());
        static_cast<void>(text.TryAppend(": "));
        switch (setting.kind) {
        case ValueKind::Bool:
        case ValueKind::Int32:
        case ValueKind::Int64:
        case ValueKind::Float32:
        case ValueKind::Float64:
            static_cast<void>(text.TryAppend(setting.text.View()));
            break;
        default:
            detail::AppendJsonString(text, setting.text.View());
            break;
        }

        static_cast<void>(text.TryAppend(index + 1 < settings.Size() ? ",\n" : "\n"));
    }

    static_cast<void>(text.TryAppend("  }\n}\n"));
    if (auto status = text.Status(); !status) {
        return status;
    }

    return out.TryAssign(text.View());
}

} // namespace rocks::config

namespace rocks::cvar::detail {

void FatalTargetType(StringView command) noexcept {
    Log::Fatal(config::detail::LogConfig,
               "Console command '{Command}' read its bound target as a different type", command);
}

} // namespace rocks::cvar::detail
