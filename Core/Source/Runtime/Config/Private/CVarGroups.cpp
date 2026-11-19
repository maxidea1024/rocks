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

// 등급 키를 읽는다. 정규 10진 표기만 받는다(`"+0"`·`"00"`은 거부 — 펼칠 때 등급을 다시 표기해
// 찾으므로 같은 표기여야 한다).
bool ParseLevelKey(StringView key, int32& out) noexcept {
    auto parsed = TryParseInt32(key);
    if (!parsed) {
        return false;
    }

    char buffer[16];
    FixedStringBuilder canonical{Span<char>(buffer)};
    static_cast<void>(canonical.TryAppendFormat("{}", parsed.Value()));
    if (!canonical.Status() || canonical.Truncated() || canonical.View() != key) {
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

// 항목 변수가 등록돼 있으면 그 설정 번호다(이름이 같고 정의가 항목의 정의일 때 — 아니면 IndexNone).
usize RegisteredItem(const ConfigState& state, const GroupItemState& item) noexcept {
    const usize index = state.FindSetting(item.name.View());
    return index != IndexNone && state.settings[index]->definition == item.target ? index
                                                                                  : IndexNone;
}

// 항목 값 텍스트를 등록된 항목 변수의 규칙(층 허용·형식·범위·Validate·묶음 등급)으로 검사한다.
// 받으면 빈 view, 아니면 사유와 code다.
Result<StringView> CheckItemValue(ConfigState& state, usize setting, StringView text,
                                  ConfigDiagnosticCode& code) noexcept {
    FieldValue scratch(state.allocator);
    code = ConfigDiagnosticCode::InvalidSetting;
    return CheckConsoleValue(state, *state.settings[setting], ConfigLayer::Quality, text, scratch,
                             code, false);
}

// 묶음 출처 표기(`quality.shadow@2`)다.
Result<void> GroupSource(const GroupState& group, int32 level, StringBuilder<160>& out) noexcept {
    static_cast<void>(out.TryAppendFormat("{}@{}", group.name.View(), level));
    return out.Status();
}

// 코드 프리셋 값 하나를 검사한다(항목 변수가 등록됐을 때만). 맞지 않으면 (묶음, 등급, 항목)마다
// Error 한 번을 남긴다. 반환: 펼칠 수 있는지(등록되지 않았으면 참 — 등록 때 다시 본다).
Result<bool> CheckCodeItem(ConfigState& state, const GroupState& group, int32 level,
                           const GroupItemState& item) noexcept {
    const usize setting = RegisteredItem(state, item);
    if (setting == IndexNone) {
        return Result<bool>::Success(true);
    }

    ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
    auto checked = CheckItemValue(state, setting, item.text.View(), code);
    if (!checked) {
        return Result<bool>::Failure(checked.Error());
    }

    if (checked.Value().IsEmpty()) {
        return Result<bool>::Success(true);
    }

    StringBuilder<192> key(state.allocator);
    StringBuilder<160> source(state.allocator);
    static_cast<void>(
        key.TryAppendFormat("{}\n{}\n{}", group.name.View(), level, item.name.View()));
    if (auto status = key.Status(); !status) {
        return Result<bool>::Failure(status.Error());
    }

    if (auto status = GroupSource(group, level, source); !status) {
        return Result<bool>::Failure(status.Error());
    }

    auto reported =
        ReportOnce(state, state.reportedPresetProblems, key.View(), code, ConfigSeverity::Error,
                   ConfigLayer::Quality, source.View(), item.name.View(), checked.Value());
    if (!reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

// 묶음 펼침에 쓸 항목 값 표기를 고른다: `Quality.json`의 값(강한 파일부터), 그다음 코드 값. 항목
// 변수에 맞지 않는 값은 그 값만 건너뛰고(같은 (입력, 묶음, 등급, 항목)에 Error 한 번) 다음으로 강한
// 것을 본다. 항목 변수가 아직 등록되지 않았으면 JSON 종류만 본다(나머지는 등록 때 본다). 반환: 펼칠
// 값이 있는지.
Result<bool> ChooseItemText(ConfigState& state, Span<ConfigEntry* const> order,
                            const GroupState& group, int32 level, StringView levelKey,
                            const GroupItemState& item, String& out) noexcept {
    const usize setting = RegisteredItem(state, item);
    String converted(state.allocator);
    for (usize position = order.Size(); position-- > 0;) {
        const ConfigEntry& entry = *order[position];
        if (!IsQualityDocument(state, entry)) {
            continue;
        }

        const JsonValue value =
            entry.document->Root().Find(group.name.View()).Find(levelKey).Find(item.name.View());
        if (!value.IsValid()) {
            continue;
        }

        auto matched = ConsoleTextOfJson(value, ConsoleValueKind(item.target->type), converted);
        if (!matched) {
            return Result<bool>::Failure(matched.Error());
        }

        StringView problem;
        ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
        if (!matched.Value()) {
            problem = "the JSON value type does not match the preset entry type";
        } else if (setting != IndexNone) {
            auto checked = CheckItemValue(state, setting, converted.View(), code);
            if (!checked) {
                return Result<bool>::Failure(checked.Error());
            }

            problem = checked.Value();
        }

        if (problem.IsEmpty()) {
            if (auto assigned = out.TryAssign(converted.View()); !assigned) {
                return Result<bool>::Failure(assigned.Error());
            }

            return Result<bool>::Success(true);
        }

        StringBuilder<192> key(state.allocator);
        static_cast<void>(key.TryAppendFormat("{}\n{}\n{}\n{}", entry.sequence, group.name.View(),
                                              levelKey, item.name.View()));
        if (auto status = key.Status(); !status) {
            return Result<bool>::Failure(status.Error());
        }

        auto reported = ReportOnce(state, state.reportedQualityProblems, key.View(), code,
                                   ConfigSeverity::Error, entry.layer, entry.sourceName.View(),
                                   item.name.View(), problem);
        if (!reported) {
            return Result<bool>::Failure(reported.Error());
        }
    }

    auto usable = CheckCodeItem(state, group, level, item);
    if (!usable || !usable.Value()) {
        return usable;
    }

    if (auto assigned = out.TryAssign(item.text.View()); !assigned) {
        return Result<bool>::Failure(assigned.Error());
    }

    return Result<bool>::Success(true);
}

// 묶음 하나를 level로 펼친 입력을 generated 뒤에 붙인다(품질 묶음 층, 태그 = 묶음 이름, 출처
// `quality.shadow@2`). 항목 변수가 아직 등록되지 않았으면 그 이름의 대기 값이 된다(11절 보류).
Result<void> ExpandGroup(ConfigState& state, Span<ConfigEntry* const> order,
                         const GroupState& group, int32 levelValue,
                         Array<UniquePtr<ConfigEntry>>& generated) noexcept {
    const GroupLevelState* level = FindLevel(group, levelValue);
    if (level == nullptr) {
        return Result<void>::Success();
    }

    StringBuilder<16> levelKey(state.allocator);
    StringBuilder<160> source(state.allocator);
    static_cast<void>(levelKey.TryAppendFormat("{}", level->level));
    if (auto status = levelKey.Status(); !status) {
        return status;
    }

    if (auto status = GroupSource(group, level->level, source); !status) {
        return status;
    }

    for (const UniquePtr<GroupItemState>& item : level->items) {
        auto made = UniquePtr<ConfigEntry>::TryMake(state.allocator, state.allocator);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        ConfigEntry& entry = *made.Value();
        auto chosen =
            ChooseItemText(state, order, group, level->level, levelKey.View(), *item, entry.value);
        if (!chosen) {
            return Result<void>::Failure(chosen.Error());
        }

        if (!chosen.Value()) {
            continue;
        }

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

        if (!filled) {
            return filled;
        }

        entry.sequence = state.nextSequence++;
        if (auto pushed = generated.TryPushBack(Move(made.Value())); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 묶음 번호(state.groups)의 자식 묶음 번호다(항목이 등록된 묶음인 것 — 겹치지 않게).
Result<void> ChildGroups(const ConfigState& state, usize parent, Array<usize>& out) noexcept {
    out.Clear();
    for (const UniquePtr<GroupLevelState>& level : state.groups[parent]->levels) {
        for (const UniquePtr<GroupItemState>& item : level->items) {
            if (item->target->kind != cvar::HandleKind::Group) {
                continue;
            }

            for (usize child = 0; child < state.groups.Size(); ++child) {
                if (state.groups[child]->definition != item->target) {
                    continue;
                }

                bool seen = false;
                for (const usize existing : out) {
                    seen = seen || existing == child;
                }

                if (!seen) {
                    if (auto pushed = out.TryPushBack(child); !pushed) {
                        return pushed;
                    }
                }
            }
        }
    }

    return Result<void>::Success();
}

// 묶음의 위상 순서다(부모 → 자식, 같으면 등록 순서 — Register가 순환을 막았다).
Result<void> TopologicalGroups(const ConfigState& state, Array<usize>& out) noexcept {
    const usize count = state.groups.Size();
    Array<usize> incoming(state.allocator);
    Array<bool> placed(state.allocator);
    Array<usize> children(state.allocator);
    if (auto sized = incoming.TryResize(count); !sized) {
        return sized;
    }

    if (auto sized = placed.TryResize(count); !sized) {
        return sized;
    }

    for (usize index = 0; index < count; ++index) {
        incoming[index] = 0;
        placed[index] = false;
    }

    for (usize parent = 0; parent < count; ++parent) {
        if (auto listed = ChildGroups(state, parent, children); !listed) {
            return listed;
        }

        for (const usize child : children) {
            ++incoming[child];
        }
    }

    out.Clear();
    while (out.Size() < count) {
        usize next = IndexNone;
        for (usize index = 0; index < count && next == IndexNone; ++index) {
            next = !placed[index] && incoming[index] == 0 ? index : IndexNone;
        }

        // 순환은 등록 때 막혔다. 혹시 남으면 등록 순서로 둔다.
        for (usize index = 0; index < count && next == IndexNone; ++index) {
            next = !placed[index] ? index : IndexNone;
        }

        placed[next] = true;
        if (auto pushed = out.TryPushBack(next); !pushed) {
            return pushed;
        }

        if (auto listed = ChildGroups(state, next, children); !listed) {
            return listed;
        }

        for (const usize child : children) {
            incoming[child] = incoming[child] == 0 ? 0 : incoming[child] - 1;
        }
    }

    return Result<void>::Success();
}

// order(펼침 제외, 적용 순서)의 품질 묶음 층 자리에 generated를 끼운 적용 순서다(펼침은 모두 품질
// 묶음 층이고 그 층의 다른 입력은 없다).
Result<void> MergeOrder(Span<ConfigEntry* const> order, usize split,
                        const Array<UniquePtr<ConfigEntry>>& generated,
                        Array<ConfigEntry*>& out) noexcept {
    out.Clear();
    if (auto reserved = out.TryReserve(order.Size() + generated.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < split; ++index) {
        static_cast<void>(out.TryPushBack(order[index])); // 예약했다
    }

    for (const UniquePtr<ConfigEntry>& entry : generated) {
        static_cast<void>(out.TryPushBack(entry.Get())); // 예약했다
    }

    for (usize index = split; index < order.Size(); ++index) {
        static_cast<void>(out.TryPushBack(order[index])); // 예약했다
    }

    return Result<void>::Success();
}

// 묶음 변수의 유효 등급이다(order로 다시 계산 — 등록되지 않았으면 기본 등급).
Result<int32> EffectiveLevel(ConfigState& state, const GroupState& group,
                             Span<ConfigEntry* const> order) noexcept {
    const usize index = state.FindSetting(group.name.View());
    if (index == IndexNone) {
        return Result<int32>::Success(static_cast<int32>(group.definition->defaultValue.integer));
    }

    FieldValue value(state.allocator);
    String text(state.allocator);
    Array<uint64> sources(state.allocator);
    if (auto computed = RecomputeConsoleSetting(state, *state.settings[index], order, false, value,
                                                text, sources);
        !computed) {
        return Result<int32>::Failure(computed.Error());
    }

    return Result<int32>::Success(static_cast<int32>(value.signedInteger));
}

// 요청 등급(state.groups 순서)을 조합 검증으로 펼칠 등급으로 바꾼다(규칙이 없는 묶음은 그대로).
Result<void> ResolveLevels(ConfigState& state, Span<const int32> requested,
                           QualityResolutionSet& out) noexcept {
    Array<QualityRequest> requests(state.allocator);
    for (usize index = 0; index < state.groups.Size(); ++index) {
        const GroupState& group = *state.groups[index];
        QualityRequest request;
        request.group = group.name.View();
        request.level = requested[index];
        request.levels = group.levelValues.View();
        if (auto pushed = requests.TryPushBack(request); !pushed) {
            return pushed;
        }
    }

    return ResolveQualityLevels(state.qualityRules, state.qualityBudgets, requests.View(),
                                state.qualityTraits, out);
}

// 정의 하나의 자식 묶음 정의(항목이 묶음인 것)를 out에 쓴다.
Result<void> ChildDefinitions(const cvar::detail::GroupDefinition& group,
                              Array<const cvar::detail::GroupDefinition*>& out) noexcept {
    out.Clear();
    for (uint32 level = 0; level < group.levelCount; ++level) {
        for (uint32 entry = 0; entry < group.levels[level].count; ++entry) {
            const cvar::detail::VarDefinition* target = group.levels[level].entries[entry].Target();
            if (target == nullptr || target->kind != cvar::HandleKind::Group) {
                continue;
            }

            if (auto pushed =
                    out.TryPushBack(static_cast<const cvar::detail::GroupDefinition*>(target));
                !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

// 묶음 정의가 target을 항목으로 갖는지다.
bool HasEntry(const cvar::detail::GroupDefinition& group,
              const cvar::detail::VarDefinition* target) noexcept {
    for (uint32 level = 0; level < group.levelCount; ++level) {
        for (uint32 entry = 0; entry < group.levels[level].count; ++entry) {
            if (group.levels[level].entries[entry].Target() == target) {
                return true;
            }
        }
    }

    return false;
}

// start에서 all 안의 묶음 간선을 따라 start로 돌아오는 경로를 찾는다(깊이 우선 — path에 지나온
// 묶음을 쌓는다). 반환: 찾았는지(path가 순환 경로).
Result<bool> FindCycle(Span<const cvar::detail::GroupDefinition* const> all,
                       const cvar::detail::GroupDefinition* start,
                       const cvar::detail::GroupDefinition* node,
                       Array<const cvar::detail::GroupDefinition*>& path,
                       Array<const cvar::detail::GroupDefinition*>& visited,
                       Allocator& allocator) noexcept {
    if (auto pushed = path.TryPushBack(node); !pushed) {
        return Result<bool>::Failure(pushed.Error());
    }

    Array<const cvar::detail::GroupDefinition*> children(allocator);
    if (auto listed = ChildDefinitions(*node, children); !listed) {
        return Result<bool>::Failure(listed.Error());
    }

    for (const cvar::detail::GroupDefinition* child : children) {
        bool known = false;
        for (const cvar::detail::GroupDefinition* candidate : all) {
            known = known || candidate == child;
        }

        if (!known) {
            continue;
        }

        if (child == start) {
            if (auto pushed = path.TryPushBack(child); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }

            return Result<bool>::Success(true);
        }

        bool seen = false;
        for (const cvar::detail::GroupDefinition* earlier : visited) {
            seen = seen || earlier == child;
        }

        if (seen) {
            continue;
        }

        if (auto pushed = visited.TryPushBack(child); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }

        auto found = FindCycle(all, start, child, path, visited, allocator);
        if (!found || found.Value()) {
            return found;
        }
    }

    static_cast<void>(path.TryResize(path.Size() - 1)); // 줄이기는 할당하지 않는다
    return Result<bool>::Success(false);
}

} // namespace

Result<void> CheckGroupGraph(ConfigState& state,
                             Span<const cvar::detail::VarDefinition* const> added) noexcept {
    using cvar::detail::GroupDefinition;
    Array<const GroupDefinition*> all(state.allocator);
    for (const UniquePtr<GroupState>& group : state.groups) {
        if (auto pushed = all.TryPushBack(group->definition); !pushed) {
            return pushed;
        }
    }

    for (const cvar::detail::VarDefinition* definition : added) {
        if (definition->kind == cvar::HandleKind::Group) {
            if (auto pushed = all.TryPushBack(static_cast<const GroupDefinition*>(definition));
                !pushed) {
                return pushed;
            }
        }
    }

    auto conflict = [&](StringView path, StringView message) noexcept -> Result<void> {
        if (auto reported =
                Report(state, ConfigDiagnosticCode::GroupConflict, ConfigSeverity::Error,
                       ConfigLayer::CodeDefault, "Register", nullptr, path, message);
            !reported) {
            return reported;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    };

    for (const cvar::detail::VarDefinition* definition : added) {
        if (definition->kind != cvar::HandleKind::Group) {
            continue;
        }

        const auto& group = static_cast<const GroupDefinition&>(*definition);
        // 한 변수는 한 묶음의 항목만 된다(11절).
        for (uint32 level = 0; level < group.levelCount; ++level) {
            for (uint32 entry = 0; entry < group.levels[level].count; ++entry) {
                const cvar::detail::VarDefinition* target =
                    group.levels[level].entries[entry].Target();
                for (const GroupDefinition* other : all) {
                    if (other == &group || target == nullptr || !HasEntry(*other, target)) {
                        continue;
                    }

                    StringBuilder<256> message(state.allocator);
                    static_cast<void>(message.TryAppendFormat(
                        "preset groups '{}' and '{}' both have this variable as an entry; a "
                        "variable belongs to one preset group",
                        group.name, other->name));
                    if (auto status = message.Status(); !status) {
                        return status;
                    }

                    return conflict(target->name, message.View());
                }
            }
        }

        // 묶음의 묶음은 DAG다(자기 참조·순환 거부 — 11절).
        Array<const GroupDefinition*> path(state.allocator);
        Array<const GroupDefinition*> visited(state.allocator);
        auto found = FindCycle(all.View(), &group, &group, path, visited, state.allocator);
        if (!found) {
            return Result<void>::Failure(found.Error());
        }

        if (found.Value()) {
            StringBuilder<256> cycle(state.allocator);
            for (usize index = 0; index < path.Size(); ++index) {
                static_cast<void>(
                    cycle.TryAppendFormat("{}{}", index == 0 ? "" : " -> ", path[index]->name));
            }

            StringBuilder<320> message(state.allocator);
            static_cast<void>(message.TryAppendFormat(
                "preset groups form a cycle ({}); nested groups must not refer back",
                cycle.View()));
            if (auto status = message.Status(); !status) {
                return status;
            }

            return conflict(group.name, message.View());
        }
    }

    return Result<void>::Success();
}

Result<void> CheckPresetValues(ConfigState& state,
                               Span<const cvar::detail::VarDefinition* const> added) noexcept {
    auto isAdded = [&](const cvar::detail::VarDefinition* definition) noexcept {
        for (const cvar::detail::VarDefinition* candidate : added) {
            if (candidate == definition) {
                return true;
            }
        }

        return false;
    };

    for (const UniquePtr<GroupState>& group : state.groups) {
        const bool groupAdded = isAdded(group->definition);
        for (const UniquePtr<GroupLevelState>& level : group->levels) {
            for (const UniquePtr<GroupItemState>& item : level->items) {
                if (!groupAdded && !isAdded(item->target)) {
                    continue;
                }

                // 새로 등록한 항목 변수는 다음 Sync가 다시 펼친다(검사로 버린 값을 대신할 값을
                // 고른다).
                state.dirty = true;
                auto checked = CheckCodeItem(state, *group, level->level, *item);
                if (!checked) {
                    return Result<void>::Failure(checked.Error());
                }
            }
        }
    }

    return Result<void>::Success();
}

void PruneQualityProblems(ConfigState& state) noexcept {
    usize kept = 0;
    for (usize index = 0; index < state.reportedQualityProblems.Size(); ++index) {
        StringView sequenceText;
        StringView rest;
        const bool split = state.reportedQualityProblems[index].View().Split(
            "\n", sequenceText, rest, StringComparison::CaseSensitive);
        auto sequence = split ? TryParseUInt64(sequenceText)
                              : Result<uint64>::Failure(ErrorCode::InvalidArgument);
        if (!sequence || state.FindEntry(sequence.Value()) == nullptr) {
            continue;
        }

        if (kept != index) {
            state.reportedQualityProblems[kept] = Move(state.reportedQualityProblems[index]);
        }

        ++kept;
    }

    // 뒤쪽 남은 자리를 지운다(제거는 할당하지 않는다).
    if (kept < state.reportedQualityProblems.Size()) {
        static_cast<void>(state.reportedQualityProblems.TryRemoveAt(
            kept, state.reportedQualityProblems.Size() - kept));
    }
}

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
                reported = problem(items, groupName,
                                   "a level key is a decimal integer in canonical form (no sign "
                                   "or leading zeros)");
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
    const usize count = state.groups.Size();
    Array<usize> topology(state.allocator);
    if (auto sorted = TopologicalGroups(state, topology); !sorted) {
        return sorted;
    }

    // 펼침이 들어갈 자리(품질 묶음 층 끝)다.
    usize split = 0;
    while (split < order.Size() && order[split]->layer <= ConfigLayer::Quality) {
        ++split;
    }

    Array<int32> requested(state.allocator);
    Array<int32> previousRequested(state.allocator);
    QualityResolutionSet previous(state.allocator);
    if (auto sized = requested.TryResize(count); !sized) {
        return sized;
    }

    if (auto sized = expanded.TryResize(count); !sized) {
        return sized;
    }

    // 차례마다 같은 시작점(묶음 변수의 유효 등급)에서 위상 순서로 펼친다. 앞 차례의 조합 검증
    // 결과는 요청 등급이 같은 묶음에만 쓴다. 검증이 낮추지 않으면 첫 차례에서 끝난다(10절 2단계).
    bool settled = false;
    for (usize pass = 0; pass < cvar::MaxGroupPasses; ++pass) {
        generated.Clear();
        for (const usize index : topology) {
            const GroupState& group = *state.groups[index];
            if (auto merged = MergeOrder(order, split, generated, settingOrder); !merged) {
                return merged;
            }

            auto level = EffectiveLevel(state, group, settingOrder.View());
            if (!level) {
                return Result<void>::Failure(level.Error());
            }

            requested[index] = level.Value();
            expanded[index] = level.Value();
            if (pass != 0 && previousRequested[index] == level.Value()) {
                expanded[index] = previous.groups[index]->applied;
            }

            if (auto grown = ExpandGroup(state, order, group, expanded[index], generated); !grown) {
                return grown;
            }
        }

        if (auto resolved = ResolveLevels(state, requested.View(), resolutions); !resolved) {
            return resolved;
        }

        settled = true;
        for (usize index = 0; index < count; ++index) {
            settled = settled && resolutions.groups[index]->applied == expanded[index];
        }

        if (settled || pass + 1 == cvar::MaxGroupPasses) {
            break;
        }

        previous.groups = Move(resolutions.groups);
        previous.unmetBudgets = Move(resolutions.unmetBudgets);
        previousRequested.Clear();
        if (auto copied = previousRequested.TryAppend(requested.View()); !copied) {
            return copied;
        }
    }

    if (auto merged = MergeOrder(order, split, generated, settingOrder); !merged) {
        return merged;
    }

    settingResults.Clear();
    if (auto computed =
            ComputeSettingResults(state, settingOrder.View(), sectionValues, settingResults);
        !computed) {
        return computed;
    }

    // 멈추지 않았으면 마지막 결과를 쓰고 같은 상태(요청·펼친 등급)에 한 번만 알린다.
    if (settled) {
        state.groupNoConverge.Clear();
    } else {
        StringBuilder<256> signature(state.allocator);
        StringView first;
        for (usize index = 0; index < count; ++index) {
            static_cast<void>(signature.TryAppendFormat(
                "{}={}>{};", state.groups[index]->name.View(), requested[index], expanded[index]));
            if (first.IsEmpty() && resolutions.groups[index]->applied != expanded[index]) {
                first = state.groups[index]->name.View();
            }
        }

        if (auto status = signature.Status(); !status) {
            return status;
        }

        if (signature.View() != state.groupNoConverge.View()) {
            StringBuilder<160> message(state.allocator);
            static_cast<void>(message.TryAppendFormat(
                "quality validation did not settle the preset groups within {} passes; the last "
                "result is used",
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

            if (auto assigned = state.groupNoConverge.TryAssign(signature.View()); !assigned) {
                return assigned;
            }
        }
    }

    // 낮춘 결과가 바뀌었으면 알린다(정보 — 같은 결과는 다시 내지 않는다).
    for (usize index = 0; index < count; ++index) {
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

    // 조합 검증 규칙: 등록되지 않은 묶음, 정의되지 않은 등급의 규칙(CVar 20절 CVR5).
    for (const QualityGroupRule& rule : state.qualityRules) {
        const GroupState* group = FindGroup(state, rule.group);
        StringBuilder<192> key(state.allocator);
        static_cast<void>(key.TryAppendFormat("rule {}", rule.group));
        if (auto status = key.Status(); !status) {
            return status;
        }

        if (group == nullptr) {
            auto done = ReportOnce(state, state.reportedUnresolved, key.View(),
                                   ConfigDiagnosticCode::UnresolvedName, ConfigSeverity::Warning,
                                   ConfigLayer::CodeDefault, "quality validation", rule.group,
                                   "no preset group has this name (quality validation rule)");
            if (!done) {
                return Result<void>::Failure(done.Error());
            }

            reported += done.Value() ? 1 : 0;
            continue;
        }

        for (const QualityLevelRule& level : rule.rules) {
            if (FindLevel(*group, level.level) != nullptr) {
                continue;
            }

            StringBuilder<192> levelKey(state.allocator);
            static_cast<void>(levelKey.TryAppendFormat("rule {}@{}", rule.group, level.level));
            if (auto status = levelKey.Status(); !status) {
                return status;
            }

            auto done =
                ReportOnce(state, state.reportedUnresolved, levelKey.View(),
                           ConfigDiagnosticCode::UndefinedLevel, ConfigSeverity::Warning,
                           ConfigLayer::CodeDefault, "quality validation", rule.group,
                           "the preset group does not define this level (quality validation rule)");
            if (!done) {
                return Result<void>::Failure(done.Error());
            }

            reported += done.Value() ? 1 : 0;
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
