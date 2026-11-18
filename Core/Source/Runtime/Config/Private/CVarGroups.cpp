#include "ConfigState.h"

#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>

// 프리셋 묶음이다(Docs/Design/CVarSystem.ko.md 10·11절 — CVR5): 등록 상태, `Quality.json` 검사와 값
// 덮어쓰기, Sync의 묶음 해결 반복(펼침 → 다시 계산 → 조합 검증), 조합 검증 입력과 결과 조회.
namespace rocks::config {

namespace detail {

namespace {

// `Quality.json`이 들어올 수 있는 파일 층이다(엔진·엔진 플랫폼·프로젝트·프로젝트 플랫폼 — Config
// 6.1).
bool IsQualityFileLayer(ConfigLayer layer) noexcept {
    return layer == ConfigLayer::Engine || layer == ConfigLayer::EnginePlatform ||
           layer == ConfigLayer::Project || layer == ConfigLayer::ProjectPlatform;
}

// 등급 키(10진 정수)를 읽는다.
bool ParseLevelKey(StringView key, int32& out) noexcept {
    auto parsed = TryParseInt32(key);
    if (!parsed) {
        return false;
    }

    out = parsed.Value();
    return true;
}

// JSON 값 하나(bool·수·문자열)인지다.
bool IsScalarJson(JsonValue value) noexcept {
    return value.IsBool() || value.IsNumber() || value.IsString();
}

// 묶음 등급 표에서 level을 찾는다(없으면 null).
const GroupLevelState* FindLevel(const GroupState& group, int32 level) noexcept {
    for (const UniquePtr<GroupLevelState>& candidate : group.levels) {
        if (candidate->level == level) {
            return candidate.Get();
        }
    }

    return nullptr;
}

// 이름이 같은 묶음이다(없으면 null).
const GroupState* FindGroup(const ConfigState& state, StringView name) noexcept {
    for (const UniquePtr<GroupState>& group : state.groups) {
        if (group->name.View() == name) {
            return group.Get();
        }
    }

    return nullptr;
}

// key로 한 번만 진단을 남긴다(같은 키를 이미 남겼으면 무동작). 키를 먼저 저장해 두므로 진단 뒤
// 기록이 실패하지 않는다. 반환: 이번에 남겼는지.
Result<bool> ReportOnce(ConfigState& state, Array<String>& seen, StringView key,
                        ConfigDiagnosticCode code, ConfigSeverity severity, ConfigLayer layer,
                        StringView source, StringView path, StringView message) noexcept {
    for (const String& existing : seen) {
        if (existing.View() == key) {
            return Result<bool>::Success(false);
        }
    }

    String stored(state.allocator);
    if (auto assigned = stored.TryAssign(key); !assigned) {
        return Result<bool>::Failure(assigned.Error());
    }

    if (auto reserved = seen.TryReserve(seen.Size() + 1); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    if (auto reported = Report(state, code, severity, layer, source, nullptr, path, message);
        !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    static_cast<void>(seen.TryPushBack(Move(stored))); // 예약했다
    return Result<bool>::Success(true);
}

// 항목 값의 콘솔 표기다(String·Name은 텍스트 그대로, 그 밖은 그 타입의 콘솔 표기 — enum은 이름).
Result<void> FormatItemValue(ConfigState& state, const cvar::detail::VarDefinition& target,
                             const cvar::detail::ScalarValue& value, String& out) noexcept {
    if (target.type == ConsoleValueType::String || target.type == ConsoleValueType::Name) {
        return out.TryAssign(value.text);
    }

    ConsoleSettingState shape(state.allocator);
    shape.type = target.type;
    shape.kind = ConsoleValueKind(target.type);
    if (target.type == ConsoleValueType::Enum) {
        shape.enumSchema = &target.enumSchema();
    }

    FieldValue scalar(state.allocator);
    scalar.kind = shape.kind;
    switch (target.type) {
    case ConsoleValueType::Bool:
        scalar.boolean = value.boolean;
        break;
    case ConsoleValueType::Int32:
    case ConsoleValueType::Int64:
        scalar.signedInteger = value.integer;
        break;
    case ConsoleValueType::Float:
        scalar.float32 = static_cast<float>(value.real);
        break;
    case ConsoleValueType::Double:
        scalar.float64 = value.real;
        break;
    case ConsoleValueType::Enum:
        scalar.signedInteger = value.integer;
        scalar.type = shape.enumSchema->id;
        break;
    case ConsoleValueType::String:
    case ConsoleValueType::Name:
        break;
    }

    return FormatConsoleValue(state, shape, scalar, out);
}

// 묶음 펼침에 쓸 항목 값 표기다: 코드 값, `Quality.json`(order에서 가장 강한 것)이 그 항목을 바꾸면
// 그 값. 덮어쓴 값이 항목 변수에 맞지 않으면 진단을 한 번 남기고 코드 값을 쓴다(항목 변수가 아직
// 등록되지 않았으면 타입만 보고, 나머지 검사는 등록 때 대기 값 이관이 한다).
Result<void> ItemText(ConfigState& state, Span<ConfigEntry* const> order, const GroupState& group,
                      StringView levelKey, const GroupItemState& item, String& out) noexcept {
    const ConfigEntry* source = nullptr;
    JsonValue found{};
    for (const ConfigEntry* entry : order) {
        if (!IsQualityDocument(state, *entry)) {
            continue;
        }

        const JsonValue level = entry->document->Root().Find(group.name.View()).Find(levelKey);
        const JsonValue value = level.Find(item.name.View());
        if (value.IsValid()) {
            source = entry;
            found = value;
        }
    }

    if (source == nullptr) {
        return out.TryAssign(item.text.View());
    }

    StringBuilder<192> key(state.allocator);
    static_cast<void>(key.TryAppendFormat("{}\n{}\n{}\n{}", source->sequence, group.name.View(),
                                          levelKey, item.name.View()));
    if (auto status = key.Status(); !status) {
        return status;
    }

    String converted(state.allocator);
    auto matched = ConsoleTextOfJson(found, ConsoleValueKind(item.target->type), converted);
    if (!matched) {
        return Result<void>::Failure(matched.Error());
    }

    StringView problem;
    ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
    if (!matched.Value()) {
        problem = "the JSON value type does not match the preset entry type";
    } else {
        const usize index = state.FindSetting(item.name.View());
        if (index != IndexNone && state.settings[index]->definition == item.target) {
            FieldValue scratch(state.allocator);
            auto checked = CheckConsoleValue(state, *state.settings[index], ConfigLayer::Quality,
                                             converted.View(), scratch, code, false);
            if (!checked) {
                return Result<void>::Failure(checked.Error());
            }

            problem = checked.Value();
        }
    }

    if (problem.IsEmpty()) {
        return out.TryAssign(converted.View());
    }

    if (auto reported = ReportOnce(state, state.reportedQualityProblems, key.View(), code,
                                   ConfigSeverity::Error, source->layer, source->sourceName.View(),
                                   item.name.View(), problem);
        !reported) {
        return Result<void>::Failure(reported.Error());
    }

    return out.TryAssign(item.text.View());
}

// levels(state.groups 순서)의 등급으로 묶음마다 펼침 입력을 만든다(품질 묶음 층, 태그 = 묶음 이름,
// 출처 `quality.shadow@2`). 항목 변수가 아직 등록되지 않았으면 그 이름의 대기 값이 된다(등록될 때
// 받는다 — 11절 보류).
Result<void> BuildExpansion(ConfigState& state, Span<ConfigEntry* const> order,
                            Span<const int32> levels,
                            Array<UniquePtr<ConfigEntry>>& generated) noexcept {
    generated.Clear();
    for (usize index = 0; index < state.groups.Size(); ++index) {
        const GroupState& group = *state.groups[index];
        const GroupLevelState* level = FindLevel(group, levels[index]);
        if (level == nullptr) {
            continue;
        }

        StringBuilder<16> levelKey(state.allocator);
        StringBuilder<160> source(state.allocator);
        static_cast<void>(levelKey.TryAppendFormat("{}", level->level));
        static_cast<void>(source.TryAppendFormat("{}@{}", group.name.View(), level->level));
        if (auto status = levelKey.Status(); !status) {
            return status;
        }

        if (auto status = source.Status(); !status) {
            return status;
        }

        for (const UniquePtr<GroupItemState>& item : level->items) {
            auto made = UniquePtr<ConfigEntry>::TryMake(state.allocator, state.allocator);
            if (!made) {
                return Result<void>::Failure(made.Error());
            }

            ConfigEntry& entry = *made.Value();
            entry.kind = EntryKind::Console;
            entry.layer = ConfigLayer::Quality;
            entry.groupExpansion = true;
            Result<void> filled = entry.tag.TryAssign(group.name.View());
            if (filled) {
                filled = entry.sourceName.TryAssign(source.View());
            }

            if (filled) {
                filled = entry.path.TryAssign(item->name.View());
            }

            if (filled) {
                filled = ItemText(state, order, group, levelKey.View(), *item, entry.value);
            }

            if (!filled) {
                return filled;
            }

            entry.sequence = state.nextSequence++;
            if (auto pushed = generated.TryPushBack(Move(made.Value())); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

// 묶음 변수의 유효 등급(settings — state.settings 순서의 새 값)을 조합 검증으로 펼칠 등급으로
// 바꾼다(규칙이 없는 묶음은 그대로). 규칙의 levels는 묶음 정의의 등급으로 바꿔 쓴다.
Result<void> ResolveLevels(ConfigState& state, Span<const UniquePtr<SettingResult>> settings,
                           QualityResolutionSet& out) noexcept {
    Array<QualityRequest> requests(state.allocator);
    for (const UniquePtr<GroupState>& group : state.groups) {
        QualityRequest request;
        request.group = group->name.View();
        const usize index = state.FindSetting(group->name.View());
        request.level = index == IndexNone
                            ? static_cast<int32>(group->definition->defaultValue.integer)
                            : static_cast<int32>(settings[index]->value.signedInteger);
        if (auto pushed = requests.TryPushBack(request); !pushed) {
            return pushed;
        }
    }

    Array<QualityGroupRule> rules(state.allocator);
    for (const QualityGroupRule& rule : state.qualityRules) {
        const GroupState* group = FindGroup(state, rule.group);
        if (group == nullptr) {
            continue;
        }

        QualityGroupRule copy = rule;
        copy.levels = group->levelValues.View();
        if (auto pushed = rules.TryPushBack(copy); !pushed) {
            return pushed;
        }
    }

    return ResolveQualityLevels(rules.View(), state.qualityBudgets, requests.View(),
                                state.qualityTraits, out);
}

} // namespace

Result<void> RejectQualityLayerWrite(ConfigState& state, ConfigLayer layer,
                                     StringView sourceName) noexcept {
    if (layer != ConfigLayer::Quality) {
        return Result<void>::Success();
    }

    if (auto reported = Report(state, ConfigDiagnosticCode::LayerNotAllowed, ConfigSeverity::Error,
                               layer, sourceName, nullptr, {},
                               "the quality layer is written by CVarGroup presets only");
        !reported) {
        return reported;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsQualityDocument(const ConfigState& state, const ConfigEntry& entry) noexcept {
    return entry.kind == EntryKind::Document && entry.document.Get() != nullptr &&
           entry.branch < state.branches.Size() &&
           state.branches[entry.branch].View() == QualityBranch;
}

Result<bool> ValidateQualityDocument(ConfigState& state, const ConfigEntry& entry) noexcept {
    bool valid = true;
    auto problem = [&](JsonValue at, StringView path, StringView why) noexcept {
        valid = false;
        const JsonPosition position = at.Position();
        return Report(state, ConfigDiagnosticCode::InvalidSetting, ConfigSeverity::Error,
                      entry.layer, entry.sourceName.View(), &position, path, why);
    };

    const JsonValue root = entry.document->Root();
    if (!IsQualityFileLayer(entry.layer)) {
        valid = false;
        const JsonPosition position = root.Position();
        if (auto reported =
                Report(state, ConfigDiagnosticCode::LayerNotAllowed, ConfigSeverity::Error,
                       entry.layer, entry.sourceName.View(), &position, {},
                       "Quality.json belongs to the engine and project file layers");
            !reported) {
            return Result<bool>::Failure(reported.Error());
        }

        return Result<bool>::Success(false);
    }

    for (usize groupIndex = 0; groupIndex < root.Size(); ++groupIndex) {
        const StringView groupName = root.KeyAt(groupIndex);
        const JsonValue levels = root.ValueAt(groupIndex);
        Result<void> reported = Result<void>::Success();
        if (!IsConsoleSettingName(groupName)) {
            reported = problem(levels, groupName, "a preset group name is a console name");
        } else if (!levels.IsObject()) {
            reported =
                problem(levels, groupName, "a preset group value is a JSON object of levels");
        }

        if (!reported) {
            return Result<bool>::Failure(reported.Error());
        }

        for (usize levelIndex = 0; levels.IsObject() && levelIndex < levels.Size(); ++levelIndex) {
            const JsonValue items = levels.ValueAt(levelIndex);
            int32 level = 0;
            if (!ParseLevelKey(levels.KeyAt(levelIndex), level)) {
                reported = problem(items, groupName, "a level key is a decimal integer");
            } else if (!items.IsObject()) {
                reported =
                    problem(items, groupName, "a level value is a JSON object of preset entries");
            }

            if (!reported) {
                return Result<bool>::Failure(reported.Error());
            }

            for (usize itemIndex = 0; items.IsObject() && itemIndex < items.Size(); ++itemIndex) {
                const StringView itemName = items.KeyAt(itemIndex);
                const JsonValue value = items.ValueAt(itemIndex);
                if (!IsConsoleSettingName(itemName)) {
                    reported = problem(value, itemName, "a preset entry name is a console name");
                } else if (!IsScalarJson(value)) {
                    reported = problem(value, itemName,
                                       "a preset entry value is a JSON bool, number or string");
                }

                if (!reported) {
                    return Result<bool>::Failure(reported.Error());
                }
            }
        }
    }

    return Result<bool>::Success(valid);
}

Result<void> MakeGroupState(ConfigState& state, const cvar::detail::GroupDefinition& definition,
                            UniquePtr<GroupState>& out) noexcept {
    auto made = UniquePtr<GroupState>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    GroupState& group = *made.Value();
    group.definition = &definition;
    if (auto assigned = group.name.TryAssign(definition.name); !assigned) {
        return assigned;
    }

    // 등급 오름차순으로 옮긴다(정의 순서는 상관없다 — 같은 등급은 컴파일 때 막혔다).
    for (uint32 index = 0; index < definition.levelCount; ++index) {
        const cvar::PresetLevel& source = definition.levels[index];
        auto level = UniquePtr<GroupLevelState>::TryMake(state.allocator, state.allocator);
        if (!level) {
            return Result<void>::Failure(level.Error());
        }

        level.Value()->level = source.level;
        for (uint32 entry = 0; entry < source.count; ++entry) {
            const cvar::PresetEntry& preset = source.entries[entry];
            // 출시 빌드의 치트 변수는 정의에 이름이 없다(등록되지 않는다 — 13.1절).
            if (preset.Target() == nullptr || preset.Target()->name.IsEmpty()) {
                continue;
            }

            auto item = UniquePtr<GroupItemState>::TryMake(state.allocator, state.allocator);
            if (!item) {
                return Result<void>::Failure(item.Error());
            }

            item.Value()->target = preset.Target();
            Result<void> filled = item.Value()->name.TryAssign(preset.Target()->name);
            if (filled) {
                filled =
                    FormatItemValue(state, *preset.Target(), preset.Value(), item.Value()->text);
            }

            if (filled) {
                filled = level.Value()->items.TryPushBack(Move(item.Value()));
            }

            if (!filled) {
                return filled;
            }
        }

        usize position = 0;
        while (position < group.levels.Size() && group.levels[position]->level < source.level) {
            ++position;
        }

        if (auto inserted = group.levels.TryInsert(Move(level.Value()), position); !inserted) {
            return inserted;
        }

        if (auto inserted = group.levelValues.TryInsert(source.level, position); !inserted) {
            return inserted;
        }
    }

    group.applied = static_cast<int32>(definition.defaultValue.integer);
    group.requested = group.applied;
    out = Move(made.Value());
    return Result<void>::Success();
}

Result<void> ResolveGroups(ConfigState& state, Span<ConfigEntry* const> order,
                           Span<void* const> sectionValues,
                           Array<UniquePtr<ConfigEntry>>& generated,
                           Array<ConfigEntry*>& settingOrder,
                           Array<UniquePtr<SettingResult>>& settingResults,
                           QualityResolutionSet& resolutions, Array<int32>& expanded) noexcept {
    // 첫 펼침 등급: 앞 Sync가 펼친 등급, 처음이면 묶음 변수의 지금 값이다.
    expanded.Clear();
    for (const UniquePtr<GroupState>& group : state.groups) {
        int32 level = group->applied;
        if (!group->expanded) {
            const usize index = state.FindSetting(group->name.View());
            level = index == IndexNone
                        ? level
                        : static_cast<int32>(state.settings[index]->value.signedInteger);
        }

        if (auto pushed = expanded.TryPushBack(level); !pushed) {
            return pushed;
        }
    }

    bool settled = false;
    for (usize pass = 0; pass < cvar::MaxGroupPasses; ++pass) {
        if (auto built = BuildExpansion(state, order, expanded.View(), generated); !built) {
            return built;
        }

        settingOrder.Clear();
        if (auto appended = settingOrder.TryAppend(order); !appended) {
            return appended;
        }

        for (UniquePtr<ConfigEntry>& entry : generated) {
            if (auto pushed = settingOrder.TryPushBack(entry.Get()); !pushed) {
                return pushed;
            }
        }

        settingOrder.Sort([](const ConfigEntry* left, const ConfigEntry* right) noexcept {
            return left->layer != right->layer ? left->layer < right->layer
                                               : left->sequence < right->sequence;
        });
        settingResults.Clear();
        if (auto computed =
                ComputeSettingResults(state, settingOrder.View(), sectionValues, settingResults);
            !computed) {
            return computed;
        }

        if (auto resolved = ResolveLevels(state, settingResults.View(), resolutions); !resolved) {
            return resolved;
        }

        settled = true;
        for (usize index = 0; index < expanded.Size(); ++index) {
            settled = settled && resolutions.groups[index]->applied == expanded[index];
        }

        // 펼친 등급이 다시 계산한 등급과 같으면 멈춘다. 마지막 차례면 바꾸지 않는다(마지막 결과 —
        // 지금 펼친 등급의 값 — 를 쓴다).
        if (settled || pass + 1 == cvar::MaxGroupPasses) {
            break;
        }

        for (usize index = 0; index < expanded.Size(); ++index) {
            expanded[index] = resolutions.groups[index]->applied;
        }
    }

    if (!settled) {
        StringView first;
        for (usize index = 0; index < expanded.Size() && first.IsEmpty(); ++index) {
            if (resolutions.groups[index]->applied != expanded[index]) {
                first = state.groups[index]->name.View();
            }
        }

        StringBuilder<160> message(state.allocator);
        static_cast<void>(message.TryAppendFormat(
            "preset groups did not settle within {} passes; the last result is used",
            cvar::MaxGroupPasses));
        if (auto status = message.Status(); !status) {
            return status;
        }

        if (auto reported =
                Report(state, ConfigDiagnosticCode::GroupNoConverge, ConfigSeverity::Warning,
                       ConfigLayer::Quality, {}, nullptr, first, message.View());
            !reported) {
            return reported;
        }
    }

    // 낮춘 결과가 바뀌었으면 알린다(정보 — 같은 결과는 다시 내지 않는다).
    for (usize index = 0; index < state.groups.Size(); ++index) {
        const GroupState& group = *state.groups[index];
        const QualityResolution& resolution = *resolutions.groups[index];
        const bool lowered = resolution.applied != resolution.requested || resolution.unmet;
        const bool same = group.expanded && group.requested == resolution.requested &&
                          group.applied == resolution.applied &&
                          group.reason.View() == resolution.reason.View() &&
                          group.unmet == resolution.unmet;
        if (!lowered || same) {
            continue;
        }

        String text(state.allocator);
        if (auto formatted = FormatQualityResolution(resolution, text); !formatted) {
            return formatted;
        }

        if (auto reported =
                Report(state, ConfigDiagnosticCode::QualityLowered, ConfigSeverity::Note,
                       ConfigLayer::Quality, {}, nullptr, group.name.View(), text.View());
            !reported) {
            return reported;
        }
    }

    return Result<void>::Success();
}

void CommitGroups(ConfigState& state, QualityResolutionSet& resolutions,
                  Span<const int32> expanded) noexcept {
    for (usize index = 0; index < state.groups.Size(); ++index) {
        GroupState& group = *state.groups[index];
        QualityResolution& resolution = *resolutions.groups[index];
        group.expanded = true;
        group.requested = resolution.requested;
        group.applied = expanded[index];
        group.reason = Move(resolution.reason);
        group.unmet = resolution.unmet;
    }

    state.unmetQualityBudgets = Move(resolutions.unmetBudgets);
}

Result<void> ReportQualityDocuments(ConfigState& state, usize& reported) noexcept {
    for (const UniquePtr<ConfigEntry>& owned : state.entries) {
        const ConfigEntry& entry = *owned;
        if (!IsQualityDocument(state, entry)) {
            continue;
        }

        const JsonValue root = entry.document->Root();
        for (usize groupIndex = 0; groupIndex < root.Size(); ++groupIndex) {
            const StringView groupName = root.KeyAt(groupIndex);
            const JsonValue levels = root.ValueAt(groupIndex);
            const GroupState* group = FindGroup(state, groupName);
            if (group == nullptr) {
                auto done = ReportOnce(
                    state, state.reportedUnresolved, groupName,
                    ConfigDiagnosticCode::UnresolvedName, ConfigSeverity::Warning, entry.layer,
                    entry.sourceName.View(), groupName, "no preset group has this name");
                if (!done) {
                    return Result<void>::Failure(done.Error());
                }

                reported += done.Value() ? 1 : 0;
                continue;
            }

            for (usize levelIndex = 0; levelIndex < levels.Size(); ++levelIndex) {
                const StringView levelKey = levels.KeyAt(levelIndex);
                const JsonValue items = levels.ValueAt(levelIndex);
                int32 levelValue = 0;
                const GroupLevelState* level =
                    ParseLevelKey(levelKey, levelValue) ? FindLevel(*group, levelValue) : nullptr;
                StringBuilder<192> key(state.allocator);
                static_cast<void>(key.TryAppendFormat("{}@{}", groupName, levelKey));
                if (auto status = key.Status(); !status) {
                    return status;
                }

                if (level == nullptr) {
                    auto done = ReportOnce(
                        state, state.reportedUnresolved, key.View(),
                        ConfigDiagnosticCode::UndefinedLevel, ConfigSeverity::Warning, entry.layer,
                        entry.sourceName.View(), key.View(),
                        "the preset group does not define this level; its values are unused");
                    if (!done) {
                        return Result<void>::Failure(done.Error());
                    }

                    reported += done.Value() ? 1 : 0;
                    continue;
                }

                for (usize itemIndex = 0; itemIndex < items.Size(); ++itemIndex) {
                    const StringView itemName = items.KeyAt(itemIndex);
                    bool defined = false;
                    for (const UniquePtr<GroupItemState>& item : level->items) {
                        defined = defined || item->name.View() == itemName;
                    }

                    if (defined) {
                        continue;
                    }

                    StringBuilder<256> itemKey(state.allocator);
                    static_cast<void>(itemKey.TryAppendFormat("{} {}", key.View(), itemName));
                    if (auto status = itemKey.Status(); !status) {
                        return status;
                    }

                    auto done =
                        ReportOnce(state, state.reportedUnresolved, itemKey.View(),
                                   ConfigDiagnosticCode::InvalidSetting, ConfigSeverity::Warning,
                                   entry.layer, entry.sourceName.View(), itemKey.View(),
                                   "Quality.json changes values of code-defined preset "
                                   "entries only; this entry is not defined for the level");
                    if (!done) {
                        return Result<void>::Failure(done.Error());
                    }

                    reported += done.Value() ? 1 : 0;
                }
            }
        }
    }

    return Result<void>::Success();
}

} // namespace detail

Result<void> ConfigContext::SetQualityValidation(Span<const QualityGroupRule> groups,
                                                 Span<const QualityBudgetRule> budgets,
                                                 const DeviceTraits* traits) noexcept {
    detail::ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    state->qualityRules = groups;
    state->qualityBudgets = budgets;
    state->qualityTraits = traits;
    state->dirty = true;
    return Result<void>::Success();
}

Result<void> ConfigContext::GetQualityLevels(QualityResolutionSet& out) const noexcept {
    const detail::ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    out.groups.Clear();
    out.unmetBudgets.Clear();
    auto fail = [&](ErrorCode error) noexcept {
        out.groups.Clear();
        out.unmetBudgets.Clear();
        return Result<void>::Failure(error);
    };

    Allocator& allocator = out.groups.GetAllocator();
    for (const UniquePtr<detail::GroupState>& group : state->groups) {
        if (!group->expanded) {
            continue;
        }

        auto made = UniquePtr<QualityResolution>::TryMake(allocator, allocator);
        if (!made || !made.Value()->group.TryAssign(group->name.View()) ||
            !made.Value()->reason.TryAssign(group->reason.View())) {
            return fail(ErrorCode::OutOfMemory);
        }

        made.Value()->requested = group->requested;
        made.Value()->applied = group->applied;
        made.Value()->unmet = group->unmet;
        if (!out.groups.TryPushBack(Move(made.Value()))) {
            return fail(ErrorCode::OutOfMemory);
        }
    }

    for (const String& budget : state->unmetQualityBudgets) {
        String copy(allocator);
        if (!copy.TryAssign(budget.View()) || !out.unmetBudgets.TryPushBack(Move(copy))) {
            return fail(ErrorCode::OutOfMemory);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::config
