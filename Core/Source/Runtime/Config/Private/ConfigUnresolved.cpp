#include "ConfigState.h"
#include "ConfigText.h"

#include <Core/Text/EditDistance.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>

// 시작 뒤 남은 대기 이름 보고(ReportUnresolved — `did you mean`), 진단 꺼내기(TakeDiagnostics),
// 콘솔 설정 설명(DescribeConsoleSetting — Explain·dump의 `[boot]`·`pending restart` 자료)이다
// (Docs/Design/CVarSystem.ko.md 9·12절 — CVR4).
namespace rocks::config {

namespace {

using detail::ConfigEntry;
using detail::ConfigState;
using detail::ConsoleSettingState;
using detail::EntryKind;

static_assert(MaxSuggestionDistance <= MaxEditDistanceLimit,
              "MaxSuggestionDistance must not exceed MaxEditDistanceLimit");

// name에 가장 가까운 등록 이름이다(거리 ≤ MaxSuggestionDistance, 같으면 byte 순서로 앞 — 없으면 빈
// view). 콘솔 이름은 소문자 규칙이므로 대소문자를 구분해 비교한다. 출시 규칙에서 숨긴 설정은
// 제안하지 않는다.
StringView NearestName(const ConfigState& state, StringView name) noexcept {
    StringView best;
    usize bestDistance = MaxSuggestionDistance + 1;
    auto consider = [&](StringView candidate) noexcept {
        const usize distance =
            EditDistance(name, candidate, MaxSuggestionDistance, EditDistanceCase::Sensitive);
        if (distance > MaxSuggestionDistance) {
            return;
        }

        if (distance < bestDistance ||
            (distance == bestDistance && detail::LessBytes(candidate, best))) {
            best = candidate;
            bestDistance = distance;
        }
    };

    for (const UniquePtr<ConsoleSettingState>& setting : state.settings) {
        if (state.releaseRules && HasFlag(setting->flags, ConsoleSettingFlags::DevelopmentOnly)) {
            continue;
        }

        consider(setting->name.View());
    }

    for (const UniquePtr<detail::ConsoleCommandState>& command : state.commands) {
        consider(command->name.View());
    }

    return best;
}

// 값의 대상 이름 하나를 보고한다(등록된 설정이면 무동작, 이 문맥에서 이미 보고한 이름은 다시
// 보고하지 않는다 — state.reportedUnresolved). 명령 이름에 값을 넣었으면 따로 알린다(명령은 값을
// 받지 않는다). 보고했으면 reported를 1 올린다.
Result<void> ReportName(ConfigState& state, const ConfigEntry& entry, StringView name,
                        const JsonPosition* position, usize& reported) noexcept {
    if (state.FindSetting(name) != IndexNone) {
        return Result<void>::Success();
    }

    for (const String& seen : state.reportedUnresolved) {
        if (seen.View() == name) {
            return Result<void>::Success();
        }
    }

    StringBuilder<160> message(state.allocator);
    if (state.FindCommand(name) != IndexNone) {
        static_cast<void>(
            message.TryAppend("is a console command, not a setting; it does not take a value"));
    } else {
        const StringView suggestion = NearestName(state, name);
        if (suggestion.IsEmpty()) {
            static_cast<void>(message.TryAppend("no console setting or command has this name"));
        } else {
            static_cast<void>(message.TryAppendFormat(
                "no console setting or command has this name; did you mean '{}'?", suggestion));
        }
    }

    if (auto status = message.Status(); !status) {
        return status;
    }

    // 자리를 먼저 잡아 보고 뒤 기록이 실패하지 않게 한다(실패하면 다음 호출이 다시 본다).
    String stored(state.allocator);
    if (auto assigned = stored.TryAssign(name); !assigned) {
        return assigned;
    }

    if (auto reserved = state.reportedUnresolved.TryReserve(state.reportedUnresolved.Size() + 1);
        !reserved) {
        return reserved;
    }

    if (auto reportedNow =
            detail::Report(state, ConfigDiagnosticCode::UnresolvedName, ConfigSeverity::Warning,
                           entry.layer, entry.sourceName.View(), position, name, message.View());
        !reportedNow) {
        return reportedNow;
    }

    static_cast<void>(state.reportedUnresolved.TryPushBack(Move(stored))); // 예약했다
    ++reported;
    return Result<void>::Success();
}

} // namespace

Result<usize> ConfigContext::ReportUnresolved() noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<usize>::Failure(ErrorCode::WrongThread);
    }

    // 적용 중인 입력에서 콘솔 이름(파일의 `Settings` 멤버·콘솔 이름 대입)을 넣은 순서로 본다.
    usize reported = 0;
    for (const UniquePtr<ConfigEntry>& owned : state->entries) {
        const ConfigEntry& entry = *owned;
        if (entry.kind == EntryKind::Console) {
            if (auto done = ReportName(*state, entry, entry.path.View(), nullptr, reported);
                !done) {
                return Result<usize>::Failure(done.Error());
            }

            continue;
        }

        if (entry.kind != EntryKind::Document) {
            continue;
        }

        const JsonValue settings = detail::SettingsSectionOf(entry);
        for (usize member = 0; settings.IsObject() && member < settings.Size(); ++member) {
            const JsonPosition at = settings.ValueAt(member).Position();
            if (auto done = ReportName(*state, entry, settings.KeyAt(member), &at, reported);
                !done) {
                return Result<usize>::Failure(done.Error());
            }
        }
    }

    return Result<usize>::Success(reported);
}

Result<void> ConfigContext::TakeDiagnostics(ConfigDiagnostics& out) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (&out == &state->diagnostics) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 전부 옮기거나 하나도 옮기지 않는다(중간 실패면 out을 처음 자리로 되돌린다).
    const ConfigDiagnosticsMark mark = out.Mark();
    for (usize index = 0; index < state->diagnostics.Count(); ++index) {
        if (auto added = out.Add(state->diagnostics.Get(index)); !added) {
            out.Rewind(mark);
            return added;
        }
    }

    out.AddOmitted(state->diagnostics.Omitted());
    state->diagnostics.Clear();
    return Result<void>::Success();
}

Result<ConsoleSettingInfo>
ConfigContext::DescribeConsoleSetting(ConsoleSettingId id) const noexcept {
    const ConfigState* state = state_.Get();
    if (state == nullptr || !id.IsValid() || id.index >= state->settings.Size()) {
        return Result<ConsoleSettingInfo>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<ConsoleSettingInfo>::Failure(ErrorCode::WrongThread);
    }

    const ConsoleSettingState& setting = *state->settings[id.index];
    ConsoleSettingInfo info;
    info.name = setting.name.View();
    info.help = setting.help.View();
    info.kind = setting.kind;
    info.flags = setting.flags;
    info.value = setting.text.View();
    info.boot = detail::HasBootFlags(setting.flags);
    for (const UniquePtr<detail::PendingRestart>& pending : state->pendingRestarts) {
        if (pending->name.View() == info.name) {
            info.pendingRestart = pending->pending.View();
            info.hasPendingRestart = true;
        }
    }

    return Result<ConsoleSettingInfo>::Success(info);
}

} // namespace rocks::config
