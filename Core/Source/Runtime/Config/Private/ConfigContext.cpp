#include "ConfigState.h"

#include <Core/Algorithms.h>
#include <Core/ScopeExit.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>

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
      queue(allocator), diagnostics(allocator) {
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

StringView CheckLayerAllowed(const ConfigState& state, ConfigLayer layer,
                             uint32 settingsFlags) noexcept {
    if (state.releaseRules && HasFlag(settingsFlags, SettingsFieldFlags::DevelopmentOnly) &&
        layer != ConfigLayer::CodeDefault) {
        return "a DevelopmentOnly value keeps its code default under release rules";
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

// 입력 하나를 섹션마다 기본값 위에 적용해 보아 검사한다(큐에 넣기 전). 반환: true면 문제없음.
Result<bool> ValidateEntry(ConfigState& state, const ConfigEntry& entry) noexcept {
    bool valid = true;
    auto apply = [&](SectionState& section) noexcept -> Result<void> {
        void* scratch = section.ops.create(state.allocator);
        if (scratch == nullptr) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        auto release =
            MakeScopeExit([&]() noexcept { section.ops.destroy(state.allocator, scratch); });
        Array<String> paths(state.allocator);
        auto applied = ApplyEntryToSection(state, section, scratch, entry, paths);
        if (!applied) {
            return Result<void>::Failure(applied.Error());
        }

        valid = valid && applied.Value();
        return Result<void>::Success();
    };

    if (entry.kind == EntryKind::Assignment) {
        if (auto applied = apply(*state.sections[entry.section]); !applied) {
            return Result<bool>::Failure(applied.Error());
        }

        return Result<bool>::Success(valid);
    }

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
        usize section = IndexNone;
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
        } else {
            section = state.FindSection(entry.branch, key);
            if (section == IndexNone) {
                code = ConfigDiagnosticCode::UnknownSection;
                problem = "no settings struct is registered for this section in this branch";
            } else if (!value.IsObject()) {
                problem = "a section value is a JSON object";
            }
        }

        if (!problem.IsEmpty()) {
            valid = false;
            if (auto reported = Report(state, code, ConfigSeverity::Error, entry.layer,
                                       entry.sourceName.View(), &at, key, problem);
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }

            continue;
        }

        if (auto applied = apply(*state.sections[section]); !applied) {
            return Result<bool>::Failure(applied.Error());
        }
    }

    return Result<bool>::Success(valid);
}

} // namespace detail

using detail::ConfigEntry;
using detail::ConfigState;
using detail::EntryKind;
using detail::QueuedChange;
using detail::SectionState;
using detail::SourceStack;

namespace {

StringView ToView(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
}

// 섹션 하나를 다시 합친 결과다(Sync가 모두 만든 뒤 한 번에 맞바꾼다).
struct SectionResult {
    explicit SectionResult(Allocator& allocator) noexcept : sources(allocator) {
    }

    void* value = nullptr;
    Array<UniquePtr<SourceStack>> sources;
};

// 섹션 하나를 코드 기본값에서 order 순서의 입력으로 다시 합친다.
Result<void> RecomputeSection(ConfigState& state, SectionState& section, usize sectionIndex,
                              Span<ConfigEntry* const> order, SectionResult& out) noexcept {
    out.value = section.ops.create(state.allocator);
    if (out.value == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (ConfigEntry* entry : order) {
        const bool applies = entry->kind == EntryKind::Assignment
                                 ? entry->section == sectionIndex
                                 : entry->branch == section.branch &&
                                       detail::FindMemberIgnoringCase(entry->document->Root(),
                                                                      section.section.View())
                                           .IsValid();
        if (!applies) {
            continue;
        }

        Array<String> paths(state.allocator);
        auto applied = detail::ApplyEntryToSection(state, section, out.value, *entry, paths);
        if (!applied) {
            return Result<void>::Failure(applied.Error());
        }

        if (!applied.Value()) {
            continue;
        }

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

    if (!copied) {
        return copied;
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
    state->dirty = true;
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

Result<void> ConfigContext::AddLayerText(ConfigLayer layer, StringView tag, StringView branch,
                                         StringView sourceName, StringView text) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || !IsConfigLayer(layer) || layer == ConfigLayer::CodeDefault) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
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

    auto queued = UniquePtr<QueuedChange>::TryMake(*allocator_, *allocator_);
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

    added.sequence = state->nextSequence++;
    queued.Value()->entry = Move(entry.Value());
    return state->queue.TryPushBack(Move(queued.Value()));
}

Result<void> ConfigContext::AddAssignment(ConfigLayer layer, StringView tag, StringView target,
                                          StringView value) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || !IsConfigLayer(layer) || layer == ConfigLayer::CodeDefault) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    // 출처 이름은 대입 원문(`<target>=<value>`)이다.
    StringBuilder<128> source(*allocator_);
    static_cast<void>(source.TryAppendFormat("{}={}", target, value));
    if (auto status = source.Status(); !status) {
        return status;
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
                               "registered settings struct");
            !reported) {
            return reported;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto queued = UniquePtr<QueuedChange>::TryMake(*allocator_, *allocator_);
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

    added.sequence = state->nextSequence++;
    queued.Value()->entry = Move(entry.Value());
    return state->queue.TryPushBack(Move(queued.Value()));
}

Result<void> ConfigContext::Unset(ConfigLayer layer, StringView tag) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || !IsConfigLayer(layer) || layer == ConfigLayer::CodeDefault) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    auto queued = UniquePtr<QueuedChange>::TryMake(*allocator_, *allocator_);
    if (!queued) {
        return Result<void>::Failure(queued.Error());
    }

    queued.Value()->layer = layer;
    if (auto assigned = queued.Value()->tag.TryAssign(tag); !assigned) {
        return assigned;
    }

    return state->queue.TryPushBack(Move(queued.Value()));
}

bool ConfigContext::HasPendingChanges() const noexcept {
    const ConfigState* state = state_.Get();
    return state != nullptr && (!state->queue.IsEmpty() || state->dirty);
}

Result<void> ConfigContext::Sync() noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (state->queue.IsEmpty() && !state->dirty) {
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
    for (UniquePtr<QueuedChange>& change : state->queue) {
        if (change->entry.Get() != nullptr) {
            firstNew = firstNew < change->entry->sequence ? firstNew : change->entry->sequence;
            if (auto pushed = plan.TryPushBack(change->entry.Get()); !pushed) {
                return pushed;
            }

            continue;
        }

        usize kept = 0;
        for (usize index = 0; index < plan.Size(); ++index) {
            const bool removed = plan[index]->layer == change->layer &&
                                 plan[index]->tag.View() == change->tag.View();
            if (!removed) {
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

        if (auto computed = RecomputeSection(*state, *state->sections[index], index, order.View(),
                                             *results[index]);
            !computed) {
            return computed;
        }
    }

    // 3. 새 입력이 더 강한 출처에 가려진 경로를 알린다(약한 쓰기 — 값은 바뀌지 않았다).
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
                if (auto reported = detail::Report(
                        *state, ConfigDiagnosticCode::WeakWrite, ConfigSeverity::Note, weak->layer,
                        weak->sourceName.View(), nullptr, stack->path.View(), message.View());
                    !reported) {
                    return reported;
                }
            }
        }
    }

    // 4. 입력 소유권을 새 목록으로 옮길 자리를 먼저 잡는다(이 뒤로는 실패하지 않는다).
    Array<UniquePtr<ConfigEntry>> next(*allocator_);
    if (auto reserved = next.TryReserve(plan.Size()); !reserved) {
        return reserved;
    }

    // 5. 맞바꾼다: 값, 출처 스택, 입력 목록.
    for (usize index = 0; index < results.Size(); ++index) {
        SectionState& section = *state->sections[index];
        section.ops.moveAssign(section.value, results[index]->value);
        section.sources = Move(results[index]->sources);
    }

    for (ConfigEntry* kept : plan) {
        for (UniquePtr<ConfigEntry>& owned : state->entries) {
            if (owned.Get() == kept) {
                static_cast<void>(next.TryPushBack(Move(owned))); // 예약했다
                break;
            }
        }

        for (UniquePtr<QueuedChange>& change : state->queue) {
            if (change->entry.Get() == kept) {
                static_cast<void>(next.TryPushBack(Move(change->entry))); // 예약했다
                break;
            }
        }
    }

    state->entries = Move(next);
    state->queue.Clear();
    state->dirty = false;
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
