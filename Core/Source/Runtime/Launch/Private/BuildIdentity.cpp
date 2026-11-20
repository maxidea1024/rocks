#include "BuildIdentity.h"

#include <Core/Build/BuildInfo.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringBuilder.h>

namespace rocks::launch::detail {

usize FormatBuildIdentity(char* buffer, usize capacity) noexcept {
    if (buffer == nullptr || capacity == 0) {
        return 0;
    }

    char version[EngineVersion::TextBytes];
    build::CurrentEngineVersion().ToChars(version);
    char toolBuildId[Sha256Digest::HexBytes + 1];
    build::ToolBuildId().ToHex(toolBuildId);
    FixedStringBuilder text{Span<char>(buffer, capacity)};
    if (!text.TryAppendFormat("\"engineVersion\": \"{}\", \"toolBuildId\": \"{}\"",
                              StringView(version), StringView(toolBuildId)) ||
        text.Truncated()) {
        buffer[0] = '\0';
        return 0;
    }

    return text.ByteCount();
}

} // namespace rocks::launch::detail
