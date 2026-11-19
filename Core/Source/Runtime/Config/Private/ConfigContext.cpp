#include "ConfigState.h"
#include "ConfigText.h"

#include <Core/Algorithms.h>
#include <Core/ScopeExit.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>
#include <ReflectionText/TextJson.h>

namespace rocks::config {

namespace detail {

namespace {

Span<const char> ToSpan(StringView text) noexcept {
    return Span<const char>(text.Data(), text.ByteCount());
}

bool IsAsciiIdentifier(StringView text) noexcept {
    bool first = true;
    for (const char c : text) {
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (!letter && (first || c < '0' || c > '9')) {
            return false;
        }

        first = false;
    }

    return !first;
}

} // namespace

ConfigState::ConfigState(Allocator& allocator, ObjectRegistry& objects) noexcept
    : allocator(allocator), objects(objects), access(objects.OwnerAccess()), platform(allocator),
      platformParents(allocator),
      directories{String(allocator), String(allocator), String(allocator), String(allocator)},
      projectName(allocator), branches(allocator), sections(allocator), entries(allocator),
      queue(allocator), diagnostics(allocator), regions(allocator), settings(allocator),
      commands(allocator), pendingCommands(allocator), dynamicVars(allocator),
      renderSignature(allocator), pendingRestarts(allocator), ignoredBootWrites(allocator),
      reportedUnresolved(allocator), groups(allocator), unmetQualityBudgets(allocator),
      reportedQualityProblems(allocator), reportedPresetProblems(allocator),
      groupNoConverge(allocator), syncHooks(allocator), onSynced(allocator) {
}

reflection_text::TextCodecOptions ConfigState::CodecOptions() const noexcept {
    reflection_text::TextCodecOptions options;
    options.names = objects.Names();
    options.mapMerge = reflection_text::MapMergeMode::MergeByKey;
    return options;
}

usize ConfigState::FindBranch(StringView name) const noexcept {
    for (usize index = 0; index < branches.Size(); ++index) {
        if (branches[index].View() == name) {
            return index;
        }
    }

    return IndexNone;
}

usize ConfigState::FindSection(usize branch, StringView section) const noexcept {
    for (usize index = 0; index < sections.Size(); ++index) {
        if (sections[index]->branch == branch &&
            EqualsIgnoringAsciiCase(ToSpan(sections[index]->section.View()), ToSpan(section))) {
            return index;
        }
    }

    return IndexNone;
}

const ConfigEntry* ConfigState::FindEntry(uint64 sequence) const noexcept {
    // entries는 넣은 순서(sequence 오름차순)다.
    usize low = 0;
    usize high = entries.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (entries[middle]->sequence < sequence) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < entries.Size() && entries[low]->sequence == sequence ? entries[low].Get()
                                                                      : nullptr;
}

Result<void> Report(ConfigState& state, ConfigDiagnosticCode code, ConfigSeverity severity,
                    ConfigLayer layer, StringView source, const JsonPosition* position,
                    StringView path, StringView message) noexcept {
    ConfigDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.severity = severity;
    diagnostic.layer = layer;
    diagnostic.source = source;
    if (position != nullptr) {
        diagnostic.position = *position;
    }

    diagnostic.path = path;
    diagnostic.message = message;
    return state.diagnostics.Add(diagnostic);
}

StringView CheckLayerAllowed(const ConfigState& state, ConfigLayer layer, uint32 settingsFlags,
                             ConfigDiagnosticCode& code) noexcept {
    code = ConfigDiagnosticCode::LayerNotAllowed;
    if (state.releaseRules && HasFlag(settingsFlags, SettingsFieldFlags::DevelopmentOnly) &&
        layer != ConfigLayer::CodeDefault) {
        return "a DevelopmentOnly value keeps its code default under release rules";
    }

    // 봉인 뒤 부팅 필드에 바로 쓰는 층은 넣을 때 거부한다(독립 설정과 같다 — CVar 9절).
    if (state.validatingWrite && state.startupComplete && IsDirectWriteLayer(layer) &&
        (HasFlag(settingsFlags, SettingsFieldFlags::StartupOnly) ||
         HasFlag(settingsFlags, SettingsFieldFlags::RestartRequired))) {
        code = ConfigDiagnosticCode::BootSealed;
        return BootSealedMessage;
    }

    if (layer == ConfigLayer::User && !HasFlag(settingsFlags, SettingsFieldFlags::UserConfig)) {
        return "the user layer can write UserConfig fields only";
    }

    return {};
}

uint32 SettingsFlagsOf(const SectionState& section, Guid field) noexcept {
    for (const SettingsFieldSchema& entry : section.ops.settings->fields) {
        if (entry.field == field) {
            return entry.flags;
        }
    }

    return 0;
}

Result<bool> CheckSettingsSection(ConfigState& state, const ConfigEntry& entry,
                                  JsonValue settings) noexcept {
    const JsonPosition at = settings.Position();
    if (!settings.IsObject()) {
        if (auto reported =
                Report(state, ConfigDiagnosticCode::InvalidSection, ConfigSeverity::Error,
                       entry.layer, entry.sourceName.View(), &at, ConsoleSettingsSection,
                       "the Settings section maps console setting names to values");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    bool valid = true;
    for (usize member = 0; member < settings.Size(); ++member) {
        const StringView name = settings.KeyAt(member);
        const JsonValue value = settings.ValueAt(member);
        const JsonPosition where = value.Position();
        StringView problem;
        if (!IsConsoleSettingName(name)) {
            problem = "console setting names are lowercase dotted paths (render.graph.immediate)";
        } else if (!value.IsBool() && !value.IsNumber() && !value.IsString()) {
            problem = "a console setting value is a bool, number or string";
        }

        if (!problem.IsEmpty()) {
            valid = false;
            if (auto reported =
                    Report(state, ConfigDiagnosticCode::InvalidSetting, ConfigSeverity::Error,
                           entry.layer, entry.sourceName.View(), &where, name, problem);
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }
        }
    }

    return Result<bool>::Success(valid);
}

// 독립 콘솔 설정에 들어오는 값 하나를 검사한다(등록되지 않은 이름은 대기 값으로 받는다).
Result<bool> ValidateConsoleValue(ConfigState& state, const ConfigEntry& entry, StringView name,
                                  StringView text, JsonValue json) noexcept {
    const usize index = state.FindSetting(name);
    if (index == IndexNone || state.settings[index]->section != IndexNone) {
        return Result<bool>::Success(true);
    }

    const ConsoleSettingState& setting = *state.settings[index];
    const JsonPosition at = json.IsValid() ? json.Position() : JsonPosition{0, 0, 0};
    const JsonPosition* position = json.IsValid() ? &at : nullptr;
    String converted(state.allocator);
    if (json.IsValid()) {
        auto matched = ConsoleTextOfJson(json, setting.kind, converted);
        if (!matched) {
            return matched;
        }

        if (!matched.Value()) {
            if (auto reported =
                    Report(state, ConfigDiagnosticCode::InvalidSetting, ConfigSeverity::Error,
                           entry.layer, entry.sourceName.View(), position, name,
                           "the JSON value type does not match the setting type");
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }

            return Result<bool>::Success(false);
        }

        text = converted.View();
    }

    ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
    FieldValue parsed(state.allocator);
    auto checked = CheckConsoleValue(state, setting, entry.layer, text, parsed, code, true);
    if (!checked) {
        return Result<bool>::Failure(checked.Error());
    }

    if (checked.Value().IsEmpty()) {
        // `Clamp()` CVar는 범위 밖 값을 보정해 받는다(CVar 6.2절 — 입력은 받고 Note를 남긴다).
        if (code == ConfigDiagnosticCode::Clamped) {
            if (auto reported =
                    Report(state, code, ConfigSeverity::Note, entry.layer, entry.sourceName.View(),
                           position, name, "the value was clamped to the setting range");
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }
        }

        return Result<bool>::Success(true);
    }

    if (auto reported = Report(state, code, ConfigSeverity::Error, entry.layer,
                               entry.sourceName.View(), position, name, checked.Value());
        !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

// 입력 하나를 검사한다(큐에 넣기 전): Document의 모양(섹션 키·`Settings` 섹션), 닿는 섹션마다 코드
// 기본값 위 적용, 독립 콘솔 설정 값. 반환: true면 문제없음.
Result<bool> ValidateEntry(ConfigState& state, const ConfigEntry& entry) noexcept {
    // 넣기 전 검사 동안만 켠다(봉인 뒤 부팅 필드의 바로 쓰는 층 거부 — CheckLayerAllowed).
    const bool validating = state.validatingWrite;
    state.validatingWrite = true;
    auto restore = MakeScopeExit([&]() noexcept { state.validatingWrite = validating; });
    // `Quality.json`은 설정 struct 섹션이 아니라 묶음 → 등급 → 항목 값이다(CVar 11절).
    if (IsQualityDocument(state, entry)) {
        return ValidateQualityDocument(state, entry);
    }

    bool valid = true;
    if (entry.kind == EntryKind::Document) {
        const JsonValue root = entry.document->Root();
        for (usize member = 0; member < root.Size(); ++member) {
            const StringView key = root.KeyAt(member);
            const JsonValue value = root.ValueAt(member);
            const JsonPosition at = value.Position();
            // 섹션 키는 대소문자를 무시하므로 대소문자만 다른 두 키는 같은 섹션을 두 번 쓴다.
            bool duplicate = false;
            for (usize other = 0; other < member && !duplicate; ++other) {
                duplicate = EqualsIgnoringAsciiCase(ToSpan(root.KeyAt(other)), ToSpan(key));
            }

            StringView problem;
            ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSection;
            if (duplicate) {
                problem = "the section appears twice (section keys ignore ASCII case)";
            } else if (EqualsIgnoringAsciiCase(ToSpan(key), ToSpan(ConsoleSettingsSection))) {
                auto checked = CheckSettingsSection(state, entry, value);
                if (!checked) {
                    return checked;
                }

                valid = valid && checked.Value();
                continue;
            } else if (state.FindSection(entry.branch, key) == IndexNone) {
                code = ConfigDiagnosticCode::UnknownSection;
                problem = "no settings struct is registered for this section in this branch";
            } else if (!value.IsObject()) {
                problem = "a section value is a JSON object";
            }

            if (!problem.IsEmpty()) {
                valid = false;
                if (auto reported = Report(state, code, ConfigSeverity::Error, entry.layer,
                                           entry.sourceName.View(), &at, key, problem);
                    !reported) {
                    return Result<bool>::Failure(reported.Error());
                }
            }
        }

        // 모양이 틀린 문서는 적용해 보지 않는다(같은 문제를 두 번 보고하지 않게).
        if (!valid) {
            return Result<bool>::Success(false);
        }
    }

    for (usize index = 0; index < state.sections.Size(); ++index) {
        if (!EntryTouchesSection(state, index, entry)) {
            continue;
        }

        SectionState& section = *state.sections[index];
        void* scratch = section.ops.create(state.allocator);
        if (scratch == nullptr) {
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }

        auto release =
            MakeScopeExit([&]() noexcept { section.ops.destroy(state.allocator, scratch); });
        Array<String> paths(state.allocator);
        auto applied = ApplyEntryToSection(state, section, scratch, entry, paths);
        if (!applied) {
            return applied;
        }

        valid = valid && applied.Value();
    }

    if (entry.kind == EntryKind::Console) {
        auto checked =
            ValidateConsoleValue(state, entry, entry.path.View(), entry.value.View(), JsonValue{});
        if (!checked) {
            return checked;
        }

        valid = valid && checked.Value();
    }

    const JsonValue settings = SettingsSectionOf(entry);
    for (usize member = 0; settings.IsObject() && member < settings.Size(); ++member) {
        auto checked = ValidateConsoleValue(state, entry, settings.KeyAt(member), {},
                                            settings.ValueAt(member));
        if (!checked) {
            return checked;
        }

        valid = valid && checked.Value();
    }

    return Result<bool>::Success(valid);
}

// 섹션 값 storage의 정규 JSON(Export — 변경 감지용)을 out에 쓴다.
Result<void> ExportSection(ConfigState& state, const SectionState& section, void* storage,
                           String& out) noexcept {
    StringBuilder<512> text(state.allocator);
    JsonWriter writer(text);
    const reflection_text::TextCodecOptions options = state.CodecOptions();
    if (auto exported = reflection_text::ExportJson(
            reflection_text::AssignmentTarget::Value(state.objects, state.access, section.lease,
                                                     storage, state.allocator),
            writer, options);
        !exported) {
        return exported;
    }

    if (auto finished = writer.Finish(); !finished) {
        return finished;
    }

    return out.TryAssign(text.View());
}

} // namespace detail

using detail::ConfigEntry;
using detail::ConfigState;
using detail::ConsoleSettingState;
using detail::EntryKind;
using detail::QueuedChange;
using detail::SectionState;
using detail::SettingResult;
using detail::SourceStack;

namespace detail {

Result<void> RecomputeSection(ConfigState& state, SectionState& section, usize sectionIndex,
                              Span<ConfigEntry* const> order, SectionResult& out) noexcept {
    out.value = section.ops.create(state.allocator);
    if (out.value == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (ConfigEntry* entry : order) {
        if (!EntryTouchesSection(state, sectionIndex, *entry)) {
            continue;
        }

        // 거부되면 그 입력의 부분 적용(섹션 객체 뒤 콘솔 값 거부 등)이 남지 않도록 사본에 적용해
        // 보고 성공한 사본으로 바꾼다.
        void* trial = section.ops.create(state.allocator);
        if (trial == nullptr) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        auto release =
            MakeScopeExit([&]() noexcept { section.ops.destroy(state.allocator, trial); });
        if (auto copied = section.lease.StructOps().copy(trial, out.value, state.allocator);
            !copied) {
            return copied;
        }

        Array<String> paths(state.allocator);
        auto applied = ApplyEntryToSection(state, section, trial, *entry, paths);
        if (!applied) {
            return Result<void>::Failure(applied.Error());
        }

        if (!applied.Value()) {
            continue;
        }

        section.ops.moveAssign(out.value, trial);
        for (String& path : paths) {
            SourceStack* stack = nullptr;
            for (UniquePtr<SourceStack>& existing : out.sources) {
                if (existing->path.View() == path.View()) {
                    stack = existing.Get();
                    break;
                }
            }

            if (stack == nullptr) {
                auto made = UniquePtr<SourceStack>::TryMake(state.allocator, state.allocator);
                if (!made) {
                    return Result<void>::Failure(made.Error());
                }

                made.Value()->path = Move(path);
                stack = made.Value().Get();
                if (auto pushed = out.sources.TryPushBack(Move(made.Value())); !pushed) {
                    return pushed;
                }
            }

            if (auto pushed = stack->entries.TryPushBack(entry->sequence); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

Result<void> SortedEntries(ConfigState& state, bool (*keep)(const ConfigEntry& entry) noexcept,
                           Array<ConfigEntry*>& out) noexcept {
    out.Clear();
    for (UniquePtr<ConfigEntry>& entry : state.entries) {
        if (keep != nullptr && !keep(*entry)) {
            continue;
        }

        if (auto pushed = out.TryPushBack(entry.Get()); !pushed) {
            return pushed;
        }
    }

    out.Sort([](const ConfigEntry* left, const ConfigEntry* right) noexcept {
        return left->layer != right->layer ? left->layer < right->layer
                                           : left->sequence < right->sequence;
    });
    return Result<void>::Success();
}

} // namespace detail

using detail::SectionResult;

namespace {

StringView ToView(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
}

// OnSynced를 부른다(Sync마다 한 번 — 7.3절). 콜백 안의 Sync는 Busy다.
void NotifySynced(ConfigState& state, Span<const cvar::Index> variables,
                  Span<const TypeId> settings) noexcept {
    // 바뀐 것이 없는 Sync도 소유 스레드를 이 문맥에 다시 묶는다(한 스레드에 문맥이 여럿이면 가장
    // 최근에 Sync한 문맥이 묶인다 — 10.1절).
    detail::RebindGameLane(state);
    SyncChanges changes;
    changes.variables = variables;
    changes.settings = settings;
    state.syncing = true;
    state.onSynced.Broadcast(changes);
    state.syncing = false;
}

} // namespace

ConfigContext::ConfigContext(Allocator& allocator, ObjectRegistry& objects,
                             const ConfigContextOptions& options) noexcept
    : allocator_(&allocator), objects_(&objects), options_(options) {
}

ConfigContext::~ConfigContext() {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return;
    }

    // 묶음이 남은 채 문맥을 없애면 그 묶음의 Set이 닫힌 통로에 가고 종료 순서가 틀린 것이다(10.1절
    // — 조용히 넘기지 않는다). 게임 lane의 자동 묶음은 여기서 푼다.
    // 문맥은 소유 스레드에서만 없앤다(그 스레드의 자동 묶음을 여기서 풀어야 한다 — 10.1절).
    if (platform::CurrentThreadId() != state->owner) {
        platform::Fatal("ConfigContext destroyed on a thread other than its owner");
    }

    // 묶인 명령 대상이 남았으면 대상보다 문맥이 먼저 없어지는 것이다(대상이 소멸 때 풀어야 한다 —
    // CVar 20절 CVR6 리뷰 반영).
    for (const UniquePtr<detail::ConsoleCommandState>& command : state->commands) {
        if (command->target != nullptr) {
            platform::Fatal("ConfigContext destroyed while a console command is still bound to a "
                            "target; unbind it before the context goes away");
        }
    }

    detail::WriteChannel& channel = *state->channel;
    if (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&channel.bindings) != 0) {
        platform::Fatal(
            "ConfigContext destroyed while cvar::SnapshotBinding objects still bind its "
            "snapshots");
    }

    if (detail::CurrentBinding.gameLane == &channel) {
        detail::CurrentBinding = detail::ThreadBinding{};
    }

    {
        platform::LockGuard lock(channel.mutex);
        channel.open = false;
        channel.queue.Clear();
        channel.current = SharedPtr<const ConfigSnapshot>();
    }

    // 프로세스 기본 문맥이면 풀고(그 뒤 묶음 없는 쓰기는 NotFound), 기본 문맥을 거쳐 통로를 얻은
    // 쓰기(닫힌 통로라 ShuttingDown으로 끝난다)가 통로 참조를 놓을 때까지 기다린다(CVar 6.2절).
    detail::ReleaseProcessDefault(channel);

    for (UniquePtr<SectionState>& section : state->sections) {
        if (section->value != nullptr) {
            section->ops.destroy(state->allocator, section->value);
            section->value = nullptr;
        }
    }
}

Result<void> ConfigContext::Initialize() noexcept {
    if (state_.Get() != nullptr ||
        (options_.mode != ConfigMode::Sealed && options_.mode != ConfigMode::Standard) ||
        options_.maxDiagnostics == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto made = UniquePtr<ConfigState>::TryMake(*allocator_, *allocator_, *objects_);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    ConfigState& state = *made.Value();
    state.owner = platform::CurrentThreadId();
    state.mode = options_.mode;
    state.releaseRules = options_.releaseRules;
    state.diagnostics.SetLimit(options_.maxDiagnostics);
    const StringView directories[] = {options_.engineDir, options_.projectDir, options_.userDataDir,
                                      options_.savedDir};
    Result<void> copied = state.platform.TryAssign(options_.platform);
    for (usize index = 0; copied && index < 4; ++index) {
        copied = state.directories[index].TryAssign(directories[index]);
    }

    if (copied) {
        copied = state.projectName.TryAssign(options_.projectName);
    }

    for (usize index = 0; copied && index < options_.platformParents.Size(); ++index) {
        String parent(*allocator_);
        copied = parent.TryAssign(options_.platformParents[index]);
        if (copied) {
            copied = state.platformParents.TryPushBack(Move(parent));
        }
    }

    for (usize index = 0;
         copied && index < sizeof(DefaultConfigBranches) / sizeof(DefaultConfigBranches[0]);
         ++index) {
        String branch(*allocator_);
        copied = branch.TryAssign(DefaultConfigBranches[index]);
        if (copied) {
            copied = state.branches.TryPushBack(Move(branch));
        }
    }

    for (usize index = 0;
         copied && index < sizeof(DefaultConsoleRegions) / sizeof(DefaultConsoleRegions[0]);
         ++index) {
        String region(*allocator_);
        copied = region.TryAssign(DefaultConsoleRegions[index]);
        if (copied) {
            copied = state.regions.TryPushBack(Move(region));
        }
    }

    if (!copied) {
        return copied;
    }

    auto channel = SharedPtr<detail::WriteChannel>::TryMake(*allocator_, *allocator_);
    if (!channel) {
        return Result<void>::Failure(channel.Error());
    }

    channel.Value()->releaseRules = state.releaseRules;
    channel.Value()->owner = state.owner;
    state.channel = Move(channel.Value());
    // 빈 snapshot을 게시하고 소유 스레드(게임 lane)를 묶는다(CVar 칸·이름 블록 없음, 세대 1).
    if (auto published = detail::PublishSnapshot(state, nullptr, {}); !published) {
        return published;
    }

    // 옵션의 view는 여기까지만 빌린다.
    options_.platform = {};
    options_.platformParents = {};
    options_.engineDir = {};
    options_.projectDir = {};
    options_.projectName = {};
    options_.userDataDir = {};
    options_.savedDir = {};
    state_ = Move(made.Value());
    state_->self = this;
    // 문맥이 스스로 갖는 핸들(치트 허용 상태·`cvar.*` 콘솔 도구 — CVar 13·15절)이다.
    if (auto registered = detail::RegisterBuiltinConsoleHandles(*this); !registered) {
        state_ = nullptr;
        return registered;
    }

    return Result<void>::Success();
}

Result<void> ConfigContext::RegisterBranch(StringView name) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || !detail::IsAsciiIdentifier(name)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (state->FindBranch(name) != IndexNone) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    String branch(*allocator_);
    if (auto assigned = branch.TryAssign(name); !assigned) {
        return assigned;
    }

    return state->branches.TryPushBack(Move(branch));
}

Result<void> ConfigContext::RegisterSettingsType(const detail::SettingsTypeOps& ops) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || ops.settings == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    const usize branch = state->FindBranch(ToView(ops.settings->branch));
    if (branch == IndexNone) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const StringView sectionName = ToView(ops.settings->section);
    if (state->FindSection(branch, sectionName) != IndexNone) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    for (const UniquePtr<SectionState>& section : state->sections) {
        if (section->ops.id == ops.id) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    auto lease = objects_->Types().AcquireValueType(ops.id, TypeKind::Struct);
    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    // 생성 SettingsSchema의 필드는 그 struct의 필드여야 한다(다른 등록과 섞인 표를 거부한다).
    for (const SettingsFieldSchema& field : ops.settings->fields) {
        if (lease.Value().FieldIndex(field.field) == IndexNone ||
            (field.flags & ~KnownSettingsFieldFlags) != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    auto made = UniquePtr<SectionState>::TryMake(*allocator_, *allocator_);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    SectionState& section = *made.Value();
    section.branch = branch;
    section.lease = Move(lease.Value());
    section.ops = ops;
    if (auto assigned = section.section.TryAssign(sectionName); !assigned) {
        return assigned;
    }

    if (auto reserved = state->sections.TryReserve(state->sections.Size() + 1); !reserved) {
        return reserved;
    }

    section.value = ops.create(*allocator_);
    if (section.value == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    static_cast<void>(state->sections.TryPushBack(Move(made.Value()))); // 예약했다
    // 기본값의 정규 JSON(변경 감지 기준)과 콘솔 이름 필드를 등록한다. 실패하면 섹션을 뺀다.
    const usize index = state->sections.Size() - 1;
    Result<void> registered =
        detail::ExportSection(*state, *state->sections[index], state->sections[index]->value,
                              state->sections[index]->exported);
    if (registered) {
        registered = detail::RegisterFieldSettings(*state, index);
    }

    if (!registered) {
        ops.destroy(*allocator_, state->sections[index]->value);
        static_cast<void>(state->sections.TryPopBack());
        return registered;
    }

    state->dirty = true;
    state->namesChanged = true;
    return Result<void>::Success();
}

const void* ConfigContext::FindSettings(TypeId type) const noexcept {
    const ConfigState* state = state_.Get();
    if (state == nullptr) {
        return nullptr;
    }

    if (platform::CurrentThreadId() != state->owner) {
        platform::Fatal("ConfigContext used from a thread other than its owner");
    }

    for (const UniquePtr<SectionState>& section : state->sections) {
        if (section->ops.id == type) {
            return section->value;
        }
    }

    return nullptr;
}

namespace detail {

Result<void> ApplyQueuedChange(Array<ConfigEntry*>& plan, const QueuedChange& change) noexcept {
    // 빼기(Unset·Clear·ClearLayer)와 바꾸기(Set — 같은 자리의 이전 쓰기를 뺀다)다.
    if (change.entry.Get() == nullptr || change.replace) {
        usize kept = 0;
        for (usize index = 0; index < plan.Size(); ++index) {
            const ConfigEntry& entry = *plan[index];
            const bool removed =
                entry.layer == change.layer &&
                (change.allTags || entry.tag.View() == change.tag.View()) &&
                (change.path.IsEmpty() ||
                 (entry.kind == EntryKind::Console && entry.path.View() == change.path.View()));
            if (!removed) {
                plan[kept++] = plan[index];
            }
        }

        static_cast<void>(plan.TryResize(kept)); // 줄이기는 할당하지 않는다
    }

    if (change.entry.Get() != nullptr) {
        return plan.TryPushBack(change.entry.Get());
    }

    return Result<void>::Success();
}

Result<void> QueueLayerText(ConfigState& owner, ConfigLayer layer, StringView tag,
                            StringView branch, StringView sourceName, StringView text,
                            bool replace) noexcept {
    ConfigState* state = &owner;
    Allocator* allocator_ = &owner.allocator;
    if (auto rejected = RejectQualityLayerWrite(*state, layer, sourceName); !rejected) {
        return rejected;
    }

    const usize branchIndex = state->FindBranch(branch);
    if (branchIndex == IndexNone) {
        if (auto reported =
                detail::Report(*state, ConfigDiagnosticCode::UnknownBranch, ConfigSeverity::Error,
                               layer, sourceName, nullptr, branch, "the branch is not registered");
            !reported) {
            return reported;
        }

        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto queued = SharedPtr<QueuedChange>::TryMake(*allocator_, *allocator_);
    auto entry = UniquePtr<ConfigEntry>::TryMake(*allocator_, *allocator_);
    auto document = UniquePtr<JsonDocument>::TryMake(*allocator_, *allocator_);
    if (!queued || !entry || !document) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    ConfigEntry& added = *entry.Value();
    added.kind = EntryKind::Document;
    added.layer = layer;
    added.branch = branchIndex;
    if (auto assigned = added.tag.TryAssign(tag); !assigned) {
        return assigned;
    }

    if (auto assigned = added.sourceName.TryAssign(sourceName); !assigned) {
        return assigned;
    }

    JsonReadOptions read;
    read.allowLineComments = true;
    const JsonError error = document.Value()->Parse(text, read);
    if (!error.Succeeded()) {
        if (error.code == JsonErrorCode::OutOfMemory) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (auto reported = detail::Report(*state, ConfigDiagnosticCode::JsonSyntax,
                                           ConfigSeverity::Error, layer, sourceName,
                                           &error.position, {}, JsonErrorMessage(error.code));
            !reported) {
            return reported;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    added.document = Move(document.Value());
    const JsonValue root = added.document->Root();
    if (!root.IsObject()) {
        const JsonPosition at = root.Position();
        if (auto reported = detail::Report(*state, ConfigDiagnosticCode::InvalidSection,
                                           ConfigSeverity::Error, layer, sourceName, &at, {},
                                           "a config file is a JSON object of sections");
            !reported) {
            return reported;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto valid = detail::ValidateEntry(*state, added);
    if (!valid) {
        return Result<void>::Failure(valid.Error());
    }

    if (!valid.Value()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 콘솔 층 문서도 치트·복제 권한을 넣을 때 본다(적용 때 다시 본다 — CVar 13절).
    if (layer == ConfigLayer::Console) {
        auto allowed = CheckEntryPermission(*state, added, CheatsAllowed(*state), nullptr);
        if (!allowed) {
            return Result<void>::Failure(allowed.Error());
        }

        if (!allowed.Value()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    queued.Value()->entry = Move(entry.Value());
    // 바꾸기(hot reload — 8절)면 새 입력 앞에 (층, 태그) 빼기를 함께 넣는다. 한꺼번에 넣으므로 다른
    // 스레드의 쓰기가 그 사이에 끼지 않는다.
    if (replace) {
        auto removal = SharedPtr<QueuedChange>::TryMake(*allocator_, *allocator_);
        if (!removal) {
            return Result<void>::Failure(removal.Error());
        }

        removal.Value()->layer = layer;
        if (auto assigned = removal.Value()->tag.TryAssign(tag); !assigned) {
            return assigned;
        }

        SharedPtr<QueuedChange> pair[] = {Move(removal.Value()), Move(queued.Value())};
        return Enqueue(*state->channel, Span<SharedPtr<QueuedChange>>(pair), false);
    }

    SharedPtr<QueuedChange> one[] = {Move(queued.Value())};
    return Enqueue(*state->channel, Span<SharedPtr<QueuedChange>>(one), false);
}

} // namespace detail

Result<void> ConfigContext::AddLayerText(ConfigLayer layer, StringView tag, StringView branch,
                                         StringView sourceName, StringView text) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || !IsConfigLayer(layer) || layer == ConfigLayer::CodeDefault) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    return detail::QueueLayerText(*state, layer, tag, branch, sourceName, text, false);
}

namespace detail {

Result<void> PrepareAssignment(ConfigState& owner, ConfigLayer layer, StringView tag,
                               StringView target, StringView value,
                               SharedPtr<QueuedChange>& out) noexcept {
    ConfigState* state = &owner;
    Allocator* allocator_ = &owner.allocator;
    if (!IsConfigLayer(layer) || layer == ConfigLayer::CodeDefault) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 출처 이름은 대입 원문(`<target>=<value>`)이다.
    StringBuilder<128> source(*allocator_);
    static_cast<void>(source.TryAppendFormat("{}={}", target, value));
    if (auto status = source.Status(); !status) {
        return status;
    }

    if (auto rejected = RejectQualityLayerWrite(*state, layer, source.View()); !rejected) {
        return rejected;
    }

    // 콘솔 설정 이름(소문자 점 경로)이면 콘솔 이름 대입이다. 등록되지 않은 이름이면 대기 값으로
    // 받는다.
    if (IsConsoleSettingName(target)) {
        auto queued = SharedPtr<QueuedChange>::TryMake(*allocator_, *allocator_);
        auto entry = UniquePtr<ConfigEntry>::TryMake(*allocator_, *allocator_);
        if (!queued || !entry) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        ConfigEntry& added = *entry.Value();
        added.kind = EntryKind::Console;
        added.layer = layer;
        Result<void> assigned = added.tag.TryAssign(tag);
        if (assigned) {
            assigned = added.sourceName.TryAssign(source.View());
        }

        if (assigned) {
            assigned = added.path.TryAssign(target);
        }

        if (assigned) {
            assigned = added.value.TryAssign(value);
        }

        if (!assigned) {
            return assigned;
        }

        auto valid = detail::ValidateEntry(*state, added);
        if (!valid) {
            return Result<void>::Failure(valid.Error());
        }

        if (!valid.Value()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (layer == ConfigLayer::Console) {
            auto allowed = CheckEntryPermission(*state, added, CheatsAllowed(*state), nullptr);
            if (!allowed) {
                return Result<void>::Failure(allowed.Error());
            }

            if (!allowed.Value()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        queued.Value()->entry = Move(entry.Value());
        out = Move(queued.Value());
        return Result<void>::Success();
    }

    StringView branchName;
    StringView rest;
    StringView sectionName;
    StringView path;
    usize section = IndexNone;
    if (target.Split(".", branchName, rest, StringComparison::CaseSensitive) &&
        rest.Split(".", sectionName, path, StringComparison::CaseSensitive) && !path.IsEmpty()) {
        const usize branch = state->FindBranch(branchName);
        section = branch == IndexNone ? IndexNone : state->FindSection(branch, sectionName);
    }

    if (section == IndexNone) {
        if (auto reported =
                detail::Report(*state, ConfigDiagnosticCode::UnknownTarget, ConfigSeverity::Error,
                               layer, source.View(), nullptr, target,
                               "an assignment target is <Branch>.<Section>.<field path> of a "
                               "registered settings struct or a console setting name");
            !reported) {
            return reported;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto queued = SharedPtr<QueuedChange>::TryMake(*allocator_, *allocator_);
    auto entry = UniquePtr<ConfigEntry>::TryMake(*allocator_, *allocator_);
    if (!queued || !entry) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    ConfigEntry& added = *entry.Value();
    added.kind = EntryKind::Assignment;
    added.layer = layer;
    added.section = section;
    added.branch = state->sections[section]->branch;
    Result<void> assigned = added.tag.TryAssign(tag);
    if (assigned) {
        assigned = added.sourceName.TryAssign(source.View());
    }

    if (assigned) {
        assigned = added.path.TryAssign(path);
    }

    if (assigned) {
        assigned = added.value.TryAssign(value);
    }

    if (!assigned) {
        return assigned;
    }

    auto valid = detail::ValidateEntry(*state, added);
    if (!valid) {
        return Result<void>::Failure(valid.Error());
    }

    if (!valid.Value()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    queued.Value()->entry = Move(entry.Value());
    out = Move(queued.Value());
    return Result<void>::Success();
}

Result<void> PrepareUnset(ConfigState& state, ConfigLayer layer, StringView tag,
                          SharedPtr<QueuedChange>& out) noexcept {
    auto queued = SharedPtr<QueuedChange>::TryMake(state.allocator, state.allocator);
    if (!queued) {
        return Result<void>::Failure(queued.Error());
    }

    queued.Value()->layer = layer;
    if (auto assigned = queued.Value()->tag.TryAssign(tag); !assigned) {
        return assigned;
    }

    out = Move(queued.Value());
    return Result<void>::Success();
}

} // namespace detail

Result<void> ConfigContext::AddAssignment(ConfigLayer layer, StringView tag, StringView target,
                                          StringView value) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || !IsConfigLayer(layer) || layer == ConfigLayer::CodeDefault) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    SharedPtr<QueuedChange> one[1];
    if (auto prepared = detail::PrepareAssignment(*state, layer, tag, target, value, one[0]);
        !prepared) {
        return prepared;
    }

    return detail::Enqueue(*state->channel, Span<SharedPtr<QueuedChange>>(one), false);
}

Result<void> ConfigContext::Unset(ConfigLayer layer, StringView tag) noexcept {
    ConfigState* state = state_.Get();
    // 품질 묶음 층은 묶음 펼침만 쓰고 지운다(CVar 11절).
    if (state == nullptr || !IsConfigLayer(layer) || layer == ConfigLayer::CodeDefault ||
        layer == ConfigLayer::Quality) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    SharedPtr<QueuedChange> one[1];
    if (auto prepared = detail::PrepareUnset(*state, layer, tag, one[0]); !prepared) {
        return prepared;
    }

    return detail::Enqueue(*state->channel, Span<SharedPtr<QueuedChange>>(one), false);
}

bool ConfigContext::HasPendingChanges() const noexcept {
    const ConfigState* state = state_.Get();
    return state != nullptr &&
           (!state->queue.IsEmpty() || state->dirty || !state->pendingCommands.IsEmpty() ||
            detail::HasQueuedChanges(*state->channel));
}

Result<void> ConfigContext::Sync() noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    // 콜백 안에서 다시 부르면 값이 콜백 도중에 바뀌므로 거부한다.
    if (state->syncing) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 0. sync 지점 앞 hook(파일 감시 등)이 이번 sync에 넣을 입력을 큐에 넣는다. hook 안의 Sync는
    // Busy다. 등록 순서대로 부르고, hook 안에서 목록을 바꿀 수 없다(RemoveSyncHook은 Busy).
    if (!state->syncHooks.IsEmpty()) {
        state->syncing = true;
        for (usize index = 0; index < state->syncHooks.Size(); ++index) {
            const detail::SyncHook hook = state->syncHooks[index];
            hook.hook(hook.context, *this);
        }

        state->syncing = false;
    }

    // 1. 쓰기 큐(다른 스레드의 쓰기·hook이 넣은 입력 포함)를 넣은 순서대로 꺼낸다. 다른 스레드의
    // 쓰기는 여기서 다시 검사한다(6.2절 — 적용 때 판정).
    if (auto drained = detail::DrainQueue(*state); !drained) {
        return drained;
    }

    if (state->queue.IsEmpty() && !state->dirty && !state->pendingCommands.IsEmpty()) {
        // 값은 그대로이고 큐에 든 명령만 실행한다.
        detail::RunPendingCommands(*state);
        NotifySynced(*state, {}, {});
        return Result<void>::Success();
    }

    if (state->queue.IsEmpty() && !state->dirty) {
        NotifySynced(*state, {}, {});
        return Result<void>::Success();
    }

    // 1. 큐를 순서대로 적용한 입력 목록(소유권은 아직 옮기지 않는다)을 만든다.
    Array<ConfigEntry*> plan(*allocator_);
    for (UniquePtr<ConfigEntry>& entry : state->entries) {
        if (auto pushed = plan.TryPushBack(entry.Get()); !pushed) {
            return pushed;
        }
    }

    uint64 firstNew = state->nextSequence;
    for (SharedPtr<QueuedChange>& change : state->queue) {
        if (change->entry.Get() != nullptr) {
            firstNew = firstNew < change->entry->sequence ? firstNew : change->entry->sequence;
        }

        if (auto applied = detail::ApplyQueuedChange(plan, *change); !applied) {
            return applied;
        }
    }

    // 앞 Sync의 묶음 펼침은 빼고 2단계 묶음 해결이 다시 만든다(CVar 10·11절).
    {
        usize kept = 0;
        for (usize index = 0; index < plan.Size(); ++index) {
            if (!plan[index]->groupExpansion) {
                plan[kept++] = plan[index];
            }
        }

        static_cast<void>(plan.TryResize(kept)); // 줄이기는 할당하지 않는다
    }

    // 2. 적용 순서(층, 넣은 순서)로 정렬하고 섹션마다 다시 합친다. 실패하면 만든 값을 버린다.
    Array<ConfigEntry*> order(*allocator_);
    if (auto appended = order.TryAppend(plan.View()); !appended) {
        return appended;
    }

    order.Sort([](const ConfigEntry* left, const ConfigEntry* right) noexcept {
        return left->layer != right->layer ? left->layer < right->layer
                                           : left->sequence < right->sequence;
    });
    Array<UniquePtr<SectionResult>> results(*allocator_);
    auto discard = MakeScopeExit([&]() noexcept {
        for (usize index = 0; index < results.Size(); ++index) {
            if (results[index]->value != nullptr) {
                state->sections[index]->ops.destroy(*allocator_, results[index]->value);
                results[index]->value = nullptr;
            }
        }
    });
    for (usize index = 0; index < state->sections.Size(); ++index) {
        auto made = UniquePtr<SectionResult>::TryMake(*allocator_, *allocator_);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        if (auto pushed = results.TryPushBack(Move(made.Value())); !pushed) {
            return pushed;
        }

        if (auto computed = detail::RecomputeSection(*state, *state->sections[index], index,
                                                     order.View(), *results[index]);
            !computed) {
            return computed;
        }
    }

    // 3. 새 입력이 더 강한 출처에 가려진 경로를 알린다(약한 쓰기 — 값은 바뀌지 않았다). 진단 목록에
    // 남기고, 실패가 아니므로 로그는 Debug다.
    for (usize index = 0; index < results.Size(); ++index) {
        for (const UniquePtr<SourceStack>& stack : results[index]->sources) {
            for (usize position = 0; position < stack->entries.Size(); ++position) {
                const uint64 sequence = stack->entries[position];
                if (sequence < firstNew) {
                    continue;
                }

                const ConfigEntry* weak = nullptr;
                const ConfigEntry* top = nullptr;
                for (ConfigEntry* entry : plan) {
                    weak = entry->sequence == sequence ? entry : weak;
                    top =
                        entry->sequence == stack->entries[stack->entries.Size() - 1] ? entry : top;
                }

                if (weak == nullptr || top == nullptr || top->layer <= weak->layer) {
                    continue;
                }

                StringBuilder<128> message(*allocator_);
                static_cast<void>(message.TryAppendFormat(
                    "a stronger source ({} {}) already sets this value; the write is kept in the "
                    "source stack",
                    ConfigLayerName(top->layer), top->sourceName.View()));
                Log::Debug(detail::LogConfig,
                           "Config write to {Path} from {Layer} {Source} is shadowed by {TopLayer} "
                           "{TopSource}; the value is unchanged",
                           stack->path.View(), ConfigLayerName(weak->layer),
                           weak->sourceName.View(), ConfigLayerName(top->layer),
                           top->sourceName.View());
                if (auto reported = detail::Report(
                        *state, ConfigDiagnosticCode::WeakWrite, ConfigSeverity::Note, weak->layer,
                        weak->sourceName.View(), nullptr, stack->path.View(), message.View());
                    !reported) {
                    return reported;
                }
            }
        }
    }

    // 4. 시작 뒤 바뀐 RestartRequired 값은 지금 값을 두고 재시작 대기로 돌린다(섹션 값 먼저 —
    // 필드 콘솔 설정은 고친 섹션 값에서 읽는다). 그다음 콘솔 설정의 새 값, 섹션 값의 변경
    // 여부(정규 JSON 비교), 설정 snapshot을 만든다.
    Array<void*> values(*allocator_);
    Array<Array<UniquePtr<SourceStack>>*> sourceLists(*allocator_);
    for (const UniquePtr<SectionResult>& result : results) {
        if (auto pushed = values.TryPushBack(result->value); !pushed) {
            return pushed;
        }

        if (auto pushed = sourceLists.TryPushBack(&result->sources); !pushed) {
            return pushed;
        }
    }

    Array<UniquePtr<detail::PendingRestart>> pendingRestarts(*allocator_);
    if (auto held = detail::HoldBootSections(*state, order.View(), values.View(),
                                             sourceLists.View(), pendingRestarts);
        !held) {
        return held;
    }

    // 묶음 해결(CVar 10절 2단계): 묶음 등급을 품질 묶음 층에 펼치고 다시 계산하기를 펼친 등급이
    // 멈출 때까지 되풀이한다. 콘솔 설정의 새 값은 펼침을 합친 적용 순서(settingOrder)로 계산한다.
    Array<UniquePtr<ConfigEntry>> generated(*allocator_);
    Array<ConfigEntry*> settingOrder(*allocator_);
    Array<UniquePtr<SettingResult>> settingResults(*allocator_);
    QualityResolutionSet resolutions(*allocator_);
    Array<int32> expandedLevels(*allocator_);
    if (auto resolved =
            detail::ResolveGroups(*state, order.View(), values.View(), generated, settingOrder,
                                  settingResults, resolutions, expandedLevels);
        !resolved) {
        return resolved;
    }

    if (auto held = detail::HoldBootSettings(*state, settingOrder.View(), settingResults.View(),
                                             pendingRestarts);
        !held) {
        return held;
    }

    Array<String> exports(*allocator_);
    Array<bool> sectionChanged(*allocator_);
    for (usize index = 0; index < results.Size(); ++index) {
        String exported(*allocator_);
        if (auto written =
                detail::ExportSection(*state, *state->sections[index], values[index], exported);
            !written) {
            return written;
        }

        if (auto pushed = sectionChanged.TryPushBack(exported.View() !=
                                                     state->sections[index]->exported.View());
            !pushed) {
            return pushed;
        }

        if (auto pushed = exports.TryPushBack(Move(exported)); !pushed) {
            return pushed;
        }
    }

    SharedPtr<const detail::NamedBlock> named;
    String signature(*allocator_);
    if (auto prepared = detail::PrepareNamedBlock(*state, settingResults.View(), values.View(),
                                                  named, signature);
        !prepared) {
        return prepared;
    }

    // 유효값이 바뀐 CVar 칸(세대 +1)과 OnSynced의 번호 목록(이름 순서)이다.
    Array<detail::SlotWrite> writes(*allocator_);
    Array<const ConsoleSettingState*> changedVariables(*allocator_);
    for (usize index = 0; index < settingResults.Size(); ++index) {
        const ConsoleSettingState& setting = *state->settings[index];
        if (setting.definition == nullptr || !settingResults[index]->changed) {
            continue;
        }

        detail::SlotWrite write;
        write.index =
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&setting.definition->index);
        write.value = &settingResults[index]->value;
        if (auto pushed = writes.TryPushBack(write); !pushed) {
            return pushed;
        }

        if (auto pushed = changedVariables.TryPushBack(&setting); !pushed) {
            return pushed;
        }
    }

    changedVariables.Sort(
        [](const ConsoleSettingState* left, const ConsoleSettingState* right) noexcept {
            return detail::LessBytes(left->name.View(), right->name.View());
        });
    Array<cvar::Index> variableIndexes(*allocator_);
    Array<TypeId> changedSettings(*allocator_);
    for (const ConsoleSettingState* setting : changedVariables) {
        const cvar::Index index{
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&setting->definition->index)};
        if (auto pushed = variableIndexes.TryPushBack(index); !pushed) {
            return pushed;
        }
    }

    for (usize index = 0; index < sectionChanged.Size(); ++index) {
        if (sectionChanged[index]) {
            if (auto pushed = changedSettings.TryPushBack(state->sections[index]->ops.id);
                !pushed) {
                return pushed;
            }
        }
    }

    // 5. 입력 소유권을 새 목록으로 옮길 자리를 먼저 잡고, 바뀐 조각만 새로 만든 snapshot을 게시한다
    // (게시가 실패하면 값은 Sync 전 그대로다). 이 뒤로는 실패하지 않는다.
    Array<UniquePtr<ConfigEntry>> next(*allocator_);
    if (auto reserved = next.TryReserve(plan.Size() + generated.Size()); !reserved) {
        return reserved;
    }

    // 등록이 바뀌었으면 값이 그대로여도 게시한다(콘솔 이름 색인 — 다른 스레드의 콘솔 입력).
    if (named.Get() != nullptr || !writes.IsEmpty() || state->namesChanged) {
        if (auto published = detail::PublishSnapshot(
                *state, named.Get() != nullptr ? &named : nullptr, writes.View());
            !published) {
            return published;
        }

        if (named.Get() != nullptr) {
            state->renderSignature = Move(signature);
        }
    }

    // 6. 맞바꾼다: 섹션 값·출처 스택·정규 JSON, 콘솔 설정 값, snapshot, 입력 목록.
    for (usize index = 0; index < results.Size(); ++index) {
        SectionState& section = *state->sections[index];
        section.ops.moveAssign(section.value, results[index]->value);
        section.sources = Move(results[index]->sources);
        section.exported = Move(exports[index]);
    }

    for (usize index = 0; index < settingResults.Size(); ++index) {
        ConsoleSettingState& setting = *state->settings[index];
        setting.value = Move(settingResults[index]->value);
        setting.text = Move(settingResults[index]->text);
        setting.sources = Move(settingResults[index]->sources);
    }

    state->pendingRestarts = Move(pendingRestarts);
    detail::CommitGroups(*state, resolutions, expandedLevels.View());

    for (ConfigEntry* kept : plan) {
        for (UniquePtr<ConfigEntry>& owned : state->entries) {
            if (owned.Get() == kept) {
                static_cast<void>(next.TryPushBack(Move(owned))); // 예약했다
                break;
            }
        }

        for (SharedPtr<QueuedChange>& change : state->queue) {
            if (change->entry.Get() == kept) {
                static_cast<void>(next.TryPushBack(Move(change->entry))); // 예약했다
                break;
            }
        }
    }

    for (UniquePtr<ConfigEntry>& expansion : generated) {
        static_cast<void>(next.TryPushBack(Move(expansion))); // 예약했다
    }

    state->entries = Move(next);
    detail::PruneQualityProblems(*state);
    state->queue.Clear();
    state->dirty = false;
    // 7. 값이 다 바뀐 뒤 명령을 실행하고 OnSynced를 부른다(알림은 새 값을 본다).
    detail::RunPendingCommands(*state);
    NotifySynced(*state, variableIndexes.View(), changedSettings.View());
    return Result<void>::Success();
}

Result<void> ConfigContext::GetSources(StringView branch, StringView section, StringView path,
                                       Array<ConfigSourceRef>& out) const noexcept {
    const ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    const usize branchIndex = state->FindBranch(branch);
    const usize sectionIndex =
        branchIndex == IndexNone ? IndexNone : state->FindSection(branchIndex, section);
    if (sectionIndex == IndexNone) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    out.Clear();
    for (const UniquePtr<SourceStack>& stack : state->sections[sectionIndex]->sources) {
        if (stack->path.View() != path) {
            continue;
        }

        for (const uint64 sequence : stack->entries) {
            const ConfigEntry* entry = state->FindEntry(sequence);
            if (entry == nullptr) {
                continue;
            }

            if (auto pushed = out.TryPushBack(
                    ConfigSourceRef{entry->layer, entry->tag.View(), entry->sourceName.View()});
                !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

ConfigDiagnostics& ConfigContext::Diagnostics() noexcept {
    if (state_.Get() == nullptr) {
        platform::Fatal("ConfigContext::Diagnostics before Initialize");
    }

    return state_->diagnostics;
}

} // namespace rocks::config
