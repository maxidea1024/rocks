#include <ReflectionText/TextCodecOptions.h>

namespace rocks::reflection_text {

namespace {

bool IsValidMapMerge(MapMergeMode mode) noexcept {
    switch (mode) {
    case MapMergeMode::Replace:
    case MapMergeMode::MergeByKey:
        return true;
    }

    return false;
}

} // namespace

Result<void> ValidateTextCodecOptions(const TextCodecOptions& options) noexcept {
    if (!IsValidTextUse(options.use) || !IsValidMapMerge(options.mapMerge)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (options.maxDiagnostics == 0 || options.maxInputBytes == 0 || options.maxJsonDepth == 0 ||
        options.maxElements == 0 || options.maxElements > MaxArrayElements) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks::reflection_text
