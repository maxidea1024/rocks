#include "ConfigState.h"

#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>
#include <ReflectionText/TextJson.h>

// 실행 중 변경(Docs/Design/ConfigSystem.ko.md 8절 — 설정 CF3): 재시작 대기 값(시작 뒤 바뀐
// RestartRequired 값은 지금 값을 두고 대기로 기록), sync 지점 앞 hook, 층 입력 바꾸기(hot reload).
namespace rocks::config {

namespace detail {

namespace {

StringView ToView(Span<const char> text) noexcept {
    return StringView(text.Data(), text.Size());
}

// value를 compact JSON으로 out에 쓴다(재시작 대기 표시용).
Result<void> CompactText(Allocator& allocator, JsonValue value, String& out) noexcept {
    StringBuilder<128> text(allocator);
    JsonWriterOptions options;
    options.compact = true;
    JsonWriter writer(text, options);
    if (auto written = writer.Value(value); !written) {
        return written;
    }

    if (auto finished = writer.Finish(); !finished) {
        return finished;
    }

    // Finish가 붙인 끝 줄바꿈은 표시에 넣지 않는다.
    StringView view = text.View();
    if (view.EndsWith("\n", StringComparison::CaseSensitive)) {
        view = view.TryLeftBytes(view.ByteCount() - 1).Value();
    }

    return out.TryAssign(view);
}

// path가 필드 tool의 경로(그 필드이거나 그 아래 — `tool`·`tool.x`·`tool[2]`)인지다.
bool IsUnderField(StringView path, StringView tool) noexcept {
    if (!path.StartsWith(tool, StringComparison::CaseSensitive)) {
        return false;
    }

    if (path.ByteCount() == tool.ByteCount()) {
        return true;
    }

    const char next = path.Data()[tool.ByteCount()];
    return next == '.' || next == '[';
}

// 새 출처 목록에서 필드 tool 아래 스택을 지우고 지금(live) 스택의 사본으로 바꾼다(재시작 대기 값의
// 출처는 지금 값의 출처다).
Result<void> RestoreSources(ConfigState& state, const SectionState& section, StringView tool,
                            Array<UniquePtr<SourceStack>>& sources) noexcept {
    usize kept = 0;
    for (usize index = 0; index < sources.Size(); ++index) {
        if (!IsUnderField(sources[index]->path.View(), tool)) {
            if (kept != index) {
                sources[kept] = Move(sources[index]);
            }

            ++kept;
        }
    }

    static_cast<void>(sources.TryResize(kept)); // 줄이기는 할당하지 않는다
    for (const UniquePtr<SourceStack>& live : section.sources) {
        if (!IsUnderField(live->path.View(), tool)) {
            continue;
        }

        auto made = UniquePtr<SourceStack>::TryMake(state.allocator, state.allocator);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        if (auto assigned = made.Value()->path.TryAssign(live->path.View()); !assigned) {
            return assigned;
        }

        if (auto copied = made.Value()->entries.TryAppend(live->entries.View()); !copied) {
            return copied;
        }

        if (auto pushed = sources.TryPushBack(Move(made.Value())); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 대기 값 하나를 더하고, 마지막 Sync에 없던(또는 대기 값이 바뀐) 것이면 진단 Note와 Info 로그를
// 남긴다.
Result<void> AddPending(ConfigState& state, StringView name, StringView pending, StringView current,
                        Array<UniquePtr<PendingRestart>>& out) noexcept {
    auto made = UniquePtr<PendingRestart>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    Result<void> filled = made.Value()->name.TryAssign(name);
    if (filled) {
        filled = made.Value()->pending.TryAssign(pending);
    }

    if (filled) {
        filled = made.Value()->current.TryAssign(current);
    }

    if (filled) {
        filled = out.TryPushBack(Move(made.Value()));
    }

    if (!filled) {
        return filled;
    }

    for (const UniquePtr<PendingRestart>& previous : state.pendingRestarts) {
        if (previous->name.View() == name && previous->pending.View() == pending) {
            return Result<void>::Success();
        }
    }

    Log::Info(LogConfig, "{Name} changes to {Pending} at the next start (now {Current})", name,
              pending, current);
    StringBuilder<128> message(state.allocator);
    static_cast<void>(message.TryAppendFormat(
        "the value {} takes effect at the next start; the current value {} stays", pending,
        current));
    if (auto status = message.Status(); !status) {
        return status;
    }

    return Report(state, ConfigDiagnosticCode::RestartRequired, ConfigSeverity::Note,
                  ConfigLayer::CodeDefault, {}, nullptr, name, message.View());
}

} // namespace

Result<void> HoldRestartSections(ConfigState& state, Span<void* const> values,
                                 Span<Array<UniquePtr<SourceStack>>* const> sources,
                                 Array<UniquePtr<PendingRestart>>& pending) noexcept {
    if (!state.startupComplete) {
        return Result<void>::Success();
    }

    for (usize index = 0; index < state.sections.Size(); ++index) {
        SectionState& section = *state.sections[index];
        bool restartFields = false;
        for (const SettingsFieldSchema& field : section.ops.settings->fields) {
            restartFields =
                restartFields || HasFlag(field.flags, SettingsFieldFlags::RestartRequired);
        }

        if (!restartFields) {
            continue;
        }

        String liveText(state.allocator);
        String nextText(state.allocator);
        if (auto exported = ExportSection(state, section, section.value, liveText); !exported) {
            return exported;
        }

        if (auto exported = ExportSection(state, section, values[index], nextText); !exported) {
            return exported;
        }

        JsonDocument live(state.allocator);
        JsonDocument next(state.allocator);
        if (!live.Parse(liveText.View()).Succeeded() || !next.Parse(nextText.View()).Succeeded()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 바뀐 RestartRequired 필드의 지금 값을 모아 새 값 위에 다시 읽는다(Map은 통째 교체 —
        // 지금 값 그대로가 된다).
        StringBuilder<256> restoreText(state.allocator);
        JsonWriter restore(restoreText);
        static_cast<void>(restore.BeginObject());
        bool restoring = false;
        for (const SettingsFieldSchema& field : section.ops.settings->fields) {
            if (!HasFlag(field.flags, SettingsFieldFlags::RestartRequired)) {
                continue;
            }

            const usize schemaIndex = section.lease.FieldIndex(field.field);
            const StringView tool =
                ToView(EffectiveToolName(section.lease.Schema().fields[schemaIndex]));
            const JsonValue before = live.Root().Find(tool);
            const JsonValue after = next.Root().Find(tool);
            if (!before.IsValid() || !after.IsValid() || JsonEquals(before, after)) {
                continue;
            }

            restoring = true;
            static_cast<void>(restore.Key(tool));
            static_cast<void>(restore.Value(before));
            StringBuilder<128> name(state.allocator);
            static_cast<void>(name.TryAppendFormat(
                "{}.{}.{}", state.branches[section.branch].View(), section.section.View(), tool));
            String pendingText(state.allocator);
            String currentText(state.allocator);
            Result<void> described = name.Status();
            if (described) {
                described = CompactText(state.allocator, after, pendingText);
            }

            if (described) {
                described = CompactText(state.allocator, before, currentText);
            }

            if (described) {
                described =
                    AddPending(state, name.View(), pendingText.View(), currentText.View(), pending);
            }

            if (described) {
                described = RestoreSources(state, section, tool, *sources[index]);
            }

            if (!described) {
                return described;
            }
        }

        if (!restoring) {
            continue;
        }

        if (auto ended = restore.EndObject(); !ended) {
            return ended;
        }

        if (auto finished = restore.Finish(); !finished) {
            return finished;
        }

        JsonDocument document(state.allocator);
        if (!document.Parse(restoreText.View()).Succeeded()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        reflection_text::TextDiagnostics diagnostics(state.allocator);
        reflection_text::TextCodecOptions options = state.CodecOptions();
        options.mapMerge = reflection_text::MapMergeMode::Replace;
        auto prepared = reflection_text::ImportJson(
            document.Root(),
            reflection_text::AssignmentTarget::Value(state.objects, state.access, section.lease,
                                                     values[index], state.allocator),
            options, diagnostics);
        if (!prepared) {
            return Result<void>::Failure(prepared.Error());
        }

        if (auto published = prepared.Value().Publish(); !published) {
            return Result<void>::Failure(published.Error());
        }
    }

    return Result<void>::Success();
}

Result<void> HoldRestartSettings(ConfigState& state, Span<const UniquePtr<SettingResult>> settings,
                                 Array<UniquePtr<PendingRestart>>& pending) noexcept {
    if (!state.startupComplete) {
        return Result<void>::Success();
    }

    for (usize index = 0; index < settings.Size(); ++index) {
        const ConsoleSettingState& setting = *state.settings[index];
        SettingResult& result = *settings[index];
        if (setting.section != IndexNone ||
            !HasFlag(setting.flags, ConsoleSettingFlags::RestartRequired) ||
            result.text.View() == setting.text.View()) {
            continue;
        }

        if (auto added = AddPending(state, setting.name.View(), result.text.View(),
                                    setting.text.View(), pending);
            !added) {
            return added;
        }

        Result<void> restored = CopyFieldValue(setting.value, result.value);
        if (restored) {
            restored = result.text.TryAssign(setting.text.View());
        }

        if (restored) {
            result.sources.Clear();
            restored = result.sources.TryAppend(setting.sources.View());
        }

        if (!restored) {
            return restored;
        }

        result.changed = false;
    }

    return Result<void>::Success();
}

} // namespace detail

using detail::ConfigState;

Result<void> ConfigContext::ReplaceLayerText(ConfigLayer layer, StringView tag, StringView branch,
                                             StringView sourceName, StringView text) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 새 입력을 먼저 검사해 넣고, 받았으면 그 앞에 Unset을 끼운다(큐의 Unset은 그보다 먼저 넣은
    // 입력만 빼므로 새 입력은 남는다).
    const usize before = state->queue.Size();
    if (auto added = AddLayerText(layer, tag, branch, sourceName, text); !added) {
        return added;
    }

    if (auto unset = Unset(layer, tag); !unset) {
        // 새 입력만 남기면 옛 입력과 겹치므로 둘 다 되돌린다.
        static_cast<void>(state->queue.TryResize(before)); // 줄이기는 할당하지 않는다
        return unset;
    }

    UniquePtr<detail::QueuedChange> removal = Move(state->queue[before + 1]);
    state->queue[before + 1] = Move(state->queue[before]);
    state->queue[before] = Move(removal);
    return Result<void>::Success();
}

Result<void> ConfigContext::AddSyncHook(ConfigSyncHook hook, void* context) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || hook == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    for (const detail::SyncHook& existing : state->syncHooks) {
        if (existing.hook == hook && existing.context == context) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    return state->syncHooks.TryPushBack(detail::SyncHook{hook, context});
}

Result<void> ConfigContext::RemoveSyncHook(ConfigSyncHook hook, void* context) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (state->syncing) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    for (usize index = 0; index < state->syncHooks.Size(); ++index) {
        if (state->syncHooks[index].hook == hook && state->syncHooks[index].context == context) {
            static_cast<void>(state->syncHooks.TryRemoveAt(index, 1)); // 줄이기는 할당하지 않는다
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ConfigContext::GetPendingRestarts(Array<ConfigPendingRestart>& out) const noexcept {
    const ConfigState* state = state_.Get();
    out.Clear();
    if (state == nullptr) {
        return Result<void>::Success();
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    for (const UniquePtr<detail::PendingRestart>& item : state->pendingRestarts) {
        if (auto pushed = out.TryPushBack(ConfigPendingRestart{
                item->name.View(), item->pending.View(), item->current.View()});
            !pushed) {
            return pushed;
        }
    }

    out.Sort([](const ConfigPendingRestart& left, const ConfigPendingRestart& right) noexcept {
        const usize count = left.name.ByteCount() < right.name.ByteCount() ? left.name.ByteCount()
                                                                           : right.name.ByteCount();
        for (usize index = 0; index < count; ++index) {
            const auto a = static_cast<uint8>(left.name.Data()[index]);
            const auto b = static_cast<uint8>(right.name.Data()[index]);
            if (a != b) {
                return a < b;
            }
        }

        return left.name.ByteCount() < right.name.ByteCount();
    });
    return Result<void>::Success();
}

} // namespace rocks::config
