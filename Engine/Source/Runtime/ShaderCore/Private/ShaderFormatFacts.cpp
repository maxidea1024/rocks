#include <ShaderCore/ShaderFormatFacts.h>

#include "ShaderCoreInternal.h"

#include <ShaderCore/ShaderReadStatus.h>
#include <string.h>

// 형식 사실·읽기 판정(Docs/Design/ShaderSystem 12.2.1절 SSD25·SSD7)이다.
namespace rocks::shader {

namespace core_detail {

bool IsShaderFormatId(StringView id) noexcept {
    if (id.IsEmpty() || id.ByteCount() >= MaxShaderFormatIdBytes) {
        return false;
    }

    for (usize index = 0; index < id.ByteCount(); ++index) {
        const char value = id.Data()[index];
        const bool letter = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
        const bool digit = value >= '0' && value <= '9';
        if (!letter && !digit && value != '_') {
            return false;
        }
    }

    return true;
}

} // namespace core_detail

bool SetShaderFormatFactsId(ShaderFormatFacts& facts, StringView id) noexcept {
    if (!core_detail::IsShaderFormatId(id)) {
        return false;
    }

    memset(facts.formatId, 0, sizeof(facts.formatId));
    memcpy(facts.formatId, id.Data(), id.ByteCount());
    return true;
}

const char* ShaderReadErrorName(ShaderReadError error) noexcept {
    switch (error) {
    case ShaderReadError::None:
        return "None";
    case ShaderReadError::UnsupportedVersion:
        return "UnsupportedVersion";
    case ShaderReadError::CorruptPackage:
        return "CorruptPackage";
    }

    return "unknown";
}

} // namespace rocks::shader
