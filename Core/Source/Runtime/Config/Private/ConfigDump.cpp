#include "ConfigState.h"
#include "ConfigText.h"

#include <Config/ConfigDump.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>
#include <ReflectionText/TextJson.h>

namespace rocks::config {

using detail::ConfigEntry;
using detail::ConfigState;
using detail::SectionState;
using detail::SourceStack;

namespace {

// 출처 목록(약한 것부터)을 배열 하나로 쓴다. writer 오류는 이후 호출이 모두 실패하는 것으로 전한다.
void WriteSourceList(const ConfigState& state, Span<const uint64> entries,
                     JsonWriter& writer) noexcept {
    static_cast<void>(writer.BeginArray());
    for (const uint64 sequence : entries) {
        const ConfigEntry* entry = state.FindEntry(sequence);
        if (entry == nullptr) {
            continue;
        }

        static_cast<void>(writer.BeginObject());
        static_cast<void>(writer.Key("layer"));
        static_cast<void>(writer.String(ConfigLayerName(entry->layer)));
        static_cast<void>(writer.Key("tag"));
        static_cast<void>(writer.String(entry->tag.View()));
        static_cast<void>(writer.Key("source"));
        static_cast<void>(writer.String(entry->sourceName.View()));
        static_cast<void>(writer.EndObject());
    }

    static_cast<void>(writer.EndArray());
}

Result<void> WriteStack(const ConfigState& state, const SourceStack& stack,
                        JsonWriter& writer) noexcept {
    static_cast<void>(writer.BeginObject());
    static_cast<void>(writer.Key("path"));
    static_cast<void>(writer.String(stack.path.View()));
    static_cast<void>(writer.Key("stack"));
    WriteSourceList(state, stack.entries.View(), writer);
    return writer.EndObject();
}

// 독립 콘솔 설정(설정 struct 필드가 아닌 것)을 이름 순서로 쓴다(필드 설정은 섹션 values·sources에
// 있다).
Result<void> WriteSettings(ConfigState& state, JsonWriter& writer) noexcept {
    Array<const detail::ConsoleSettingState*> settings(state.allocator);
    for (const UniquePtr<detail::ConsoleSettingState>& setting : state.settings) {
        if (setting->section != IndexNone) {
            continue;
        }

        if (auto pushed = settings.TryPushBack(setting.Get()); !pushed) {
            return pushed;
        }
    }

    settings.Sort([](const detail::ConsoleSettingState* left,
                     const detail::ConsoleSettingState* right) noexcept {
        return detail::LessBytes(left->name.View(), right->name.View());
    });
    static_cast<void>(writer.BeginArray());
    for (const detail::ConsoleSettingState* setting : settings) {
        static_cast<void>(writer.BeginObject());
        static_cast<void>(writer.Key("name"));
        static_cast<void>(writer.String(setting->name.View()));
        static_cast<void>(writer.Key("value"));
        static_cast<void>(writer.String(setting->text.View()));
        static_cast<void>(writer.Key("stack"));
        WriteSourceList(state, setting->sources.View(), writer);
        static_cast<void>(writer.EndObject());
    }

    return writer.EndArray();
}

Result<int32> RunConfigDump(void* context, Span<const StringView> arguments,
                            CommandletOutput& output) noexcept {
    auto& config = *static_cast<ConfigContext*>(context);
    if (!arguments.IsEmpty()) {
        if (auto written = output.write(output.context, CommandletStream::Error,
                                        "usage: config-dump (takes no arguments)\n");
            !written) {
            return Result<int32>::Failure(written.Error());
        }

        return Result<int32>::Success(2);
    }

    ConfigState* state = config.State();
    if (state == nullptr) {
        return Result<int32>::Failure(ErrorCode::InvalidArgument);
    }

    StringBuilder<1024> text(state->allocator);
    JsonWriter writer(text);
    if (auto dumped = WriteConfigDump(config, writer); !dumped) {
        return Result<int32>::Failure(dumped.Error());
    }

    if (auto finished = writer.Finish(); !finished) {
        return Result<int32>::Failure(finished.Error());
    }

    if (auto written = output.write(output.context, CommandletStream::Output, text.View());
        !written) {
        return Result<int32>::Failure(written.Error());
    }

    return Result<int32>::Success(0);
}

} // namespace

Result<void> WriteConfigDump(ConfigContext& context, JsonWriter& writer) noexcept {
    ConfigState* state = context.State();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    // 섹션은 (branch, section) 이름 순서, 출처는 경로 순서다(등록·적용 순서와 무관한 결정적 출력).
    Array<const SectionState*> sections(state->allocator);
    for (const UniquePtr<SectionState>& section : state->sections) {
        if (auto pushed = sections.TryPushBack(section.Get()); !pushed) {
            return pushed;
        }
    }

    sections.Sort([state](const SectionState* left, const SectionState* right) noexcept {
        const StringView leftBranch = state->branches[left->branch].View();
        const StringView rightBranch = state->branches[right->branch].View();
        if (leftBranch != rightBranch) {
            return detail::LessBytes(leftBranch, rightBranch);
        }

        return detail::LessBytes(left->section.View(), right->section.View());
    });
    const reflection_text::TextCodecOptions options = state->CodecOptions();
    Result<void> result = writer.BeginObject();
    static_cast<void>(writer.Key("sections"));
    static_cast<void>(writer.BeginArray());
    for (usize index = 0; result && index < sections.Size(); ++index) {
        const SectionState& section = *sections[index];
        result = writer.BeginObject();
        static_cast<void>(writer.Key("branch"));
        static_cast<void>(writer.String(state->branches[section.branch].View()));
        static_cast<void>(writer.Key("section"));
        static_cast<void>(writer.String(section.section.View()));
        static_cast<void>(writer.Key("values"));
        if (result) {
            result = reflection_text::ExportJson(
                reflection_text::AssignmentTarget::Value(
                    state->objects, state->access, section.lease, section.value, state->allocator),
                writer, options);
        }

        Array<const SourceStack*> stacks(state->allocator);
        for (usize stack = 0; result && stack < section.sources.Size(); ++stack) {
            result = stacks.TryPushBack(section.sources[stack].Get());
        }

        stacks.Sort([](const SourceStack* left, const SourceStack* right) noexcept {
            return detail::LessBytes(left->path.View(), right->path.View());
        });
        static_cast<void>(writer.Key("sources"));
        static_cast<void>(writer.BeginArray());
        for (usize stack = 0; result && stack < stacks.Size(); ++stack) {
            result = WriteStack(*state, *stacks[stack], writer);
        }

        static_cast<void>(writer.EndArray());
        static_cast<void>(writer.EndObject());
    }

    // writer는 한 번 실패하면 이후 호출이 모두 실패하므로 마지막 호출이 writer 오류를 전한다. 그
    // 밖의 오류(ExportJson·할당)는 result가 쥔다.
    static_cast<void>(writer.EndArray());
    if (!result) {
        return result;
    }

    static_cast<void>(writer.Key("settings"));
    if (auto written = WriteSettings(*state, writer); !written) {
        return written;
    }

    return writer.EndObject();
}

Result<void> RegisterConfigDumpCommandlet(CommandletRegistry& registry,
                                          ConfigContext& context) noexcept {
    CommandletDefinition definition;
    definition.name = ConfigDumpCommandletName;
    definition.description = "Writes config values and their source stacks as deterministic JSON";
    definition.execute = &RunConfigDump;
    definition.context = &context;
    return registry.Register(definition);
}

} // namespace rocks::config
