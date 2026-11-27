#include <ShaderCompiler/ShaderNugget.h>

#include <Core/Containers/Span.h>
#include <HostConfig/HostSettings.h>

// 셰이더 Record 값 표와 너겟 위치(Docs/Design/ShaderSystem 10절)다.
namespace rocks::shader {

namespace shader_nugget_detail {

// 엔진 폴더 경로 버퍼 크기다(FindEngineDirectory).
constexpr usize MaxEngineDirectoryBytes = 4096;

} // namespace shader_nugget_detail

Array<uint8>& ShaderJobValues::Part(ShaderNuggetValue value) noexcept {
    switch (value) {
    case ShaderNuggetValue::BindingLayout:
        return bindingLayout;
    case ShaderNuggetValue::GlesCopyTable:
        return glesCopyTable;
    case ShaderNuggetValue::DebugSymbols:
        return debugSymbols;
    case ShaderNuggetValue::Code:
        break;
    }

    return code;
}

const Array<uint8>& ShaderJobValues::Part(ShaderNuggetValue value) const noexcept {
    return const_cast<ShaderJobValues*>(this)->Part(value);
}

void ShaderJobValues::Set(ShaderNuggetValue value, Array<uint8>&& bytes) noexcept {
    Part(value) = Move(bytes);
    present |= 1u << static_cast<uint32>(value);
}

void ShaderJobValues::Reset() noexcept {
    code.Clear();
    bindingLayout.Clear();
    glesCopyTable.Clear();
    debugSymbols.Clear();
    present = 0;
}

Result<void> MoveShaderValuesToNugget(ShaderJobValues& values, Array<NuggetValue>& out) noexcept {
    out.Clear();
    if (!values.Has(ShaderNuggetValue::Code)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 id = 0; id < ShaderNuggetValueCount; ++id) {
        const auto value = static_cast<ShaderNuggetValue>(id);
        if (!values.Has(value)) {
            continue;
        }

        auto added = out.TryEmplaceBack(out.GetAllocator());
        if (!added) {
            out.Clear();
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        added.Value()->id = id;
        added.Value()->bytes = Move(values.Part(value));
    }

    values.Reset();
    return Result<void>::Success();
}

Result<void> MoveShaderValuesFromNugget(Array<NuggetValue>& record,
                                        ShaderJobValues& values) noexcept {
    values.Reset();
    uint32 previous = 0;
    for (usize index = 0; index < record.Size(); ++index) {
        const uint32 id = record[index].id;
        if (id >= ShaderNuggetValueCount || (index != 0 && id <= previous)) {
            values.Reset();
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        previous = id;
    }

    if (record.IsEmpty() || record[0].id != static_cast<uint32>(ShaderNuggetValue::Code)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    for (NuggetValue& value : record) {
        values.Set(static_cast<ShaderNuggetValue>(value.id), Move(value.bytes));
    }

    return Result<void>::Success();
}

Result<void> ResolveShaderNuggetRoot(StringView projectRoot, String& out) noexcept {
    if (!projectRoot.IsEmpty()) {
        return out.TryAssign(projectRoot);
    }

    char engine[shader_nugget_detail::MaxEngineDirectoryBytes];
    const Result<usize> found = FindEngineDirectory(Span<char>(engine, sizeof(engine)));
    if (!found) {
        return Result<void>::Failure(found.Error());
    }

    return out.TryAssign(StringView(engine, found.Value()));
}

Result<void> OpenShaderNuggetCache(NuggetCache& cache, Allocator& allocator,
                                   platform::FileSystem& files, StringView root,
                                   NuggetSettingsFailure* failure) noexcept {
    auto settings = ReadNuggetCacheSettings(allocator, files, root, failure);
    if (!settings) {
        return Result<void>::Failure(settings.Error());
    }

    return cache.Open(settings.Value(), root);
}

} // namespace rocks::shader
