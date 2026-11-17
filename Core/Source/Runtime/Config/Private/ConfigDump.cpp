#include "ConfigState.h"

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

Result<void> WriteStack(const ConfigState& state, const SourceStack& stack,
                        JsonWriter& writer) noexcept {
    Result<void> result = writer.BeginObject();
    static_cast<void>(writer.Key("path"));
    static_cast<void>(writer.String(stack.path.View()));
    static_cast<void>(writer.Key("stack"));
    static_cast<void>(writer.BeginArray());
    for (usize index = 0; result && index < stack.entries.Size(); ++index) {
        const ConfigEntry* entry = state.FindEntry(stack.entries[index]);
        if (entry == nullptr) {
            continue;
        }

        result = writer.BeginObject();
        static_cast<void>(writer.Key("layer"));
        static_cast<void>(writer.String(ConfigLayerName(entry->layer)));
        static_cast<void>(writer.Key("tag"));
        static_cast<void>(writer.String(entry->tag.View()));
        static_cast<void>(writer.Key("source"));
        static_cast<void>(writer.String(entry->sourceName.View()));
        static_cast<void>(writer.EndObject());
    }

    static_cast<void>(writer.EndArray());
    return writer.EndObject();
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
            return LessBytes(leftBranch, rightBranch);
        }

        return LessBytes(left->section.View(), right->section.View());
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
            return LessBytes(left->path.View(), right->path.View());
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
