#include <GraphicsTypes/VertexFormat.h>
#include <Platform/Memory.h>

namespace rocks::graphics {

namespace {

// 번호표다. 행 i가 번호 i + 1이다.
struct FormatInfo {
    const char* name;
    uint8 bytes;
    uint8 components;
    VertexShaderType shader;
    bool normalized;
};

constexpr FormatInfo Formats[VertexFormatCount] = {
    {"Float1", 4, 1, VertexShaderType::Float, false},
    {"Float2", 8, 2, VertexShaderType::Float, false},
    {"Float3", 12, 3, VertexShaderType::Float, false},
    {"Float4", 16, 4, VertexShaderType::Float, false},
    {"Byte4Snorm", 4, 4, VertexShaderType::Float, true},
    {"UByte4", 4, 4, VertexShaderType::Uint, false},
    {"UByte4Unorm", 4, 4, VertexShaderType::Float, true},
    {"Bgra8Unorm", 4, 4, VertexShaderType::Float, true},
    {"Short2", 4, 2, VertexShaderType::Int, false},
    {"Short4", 8, 4, VertexShaderType::Int, false},
    {"Short2Snorm", 4, 2, VertexShaderType::Float, true},
    {"Short4Snorm", 8, 4, VertexShaderType::Float, true},
    {"Half2", 4, 2, VertexShaderType::Float, false},
    {"Half4", 8, 4, VertexShaderType::Float, false},
    {"UShort2", 4, 2, VertexShaderType::Uint, false},
    {"UShort4", 8, 4, VertexShaderType::Uint, false},
    {"UShort2Unorm", 4, 2, VertexShaderType::Float, true},
    {"UShort4Unorm", 8, 4, VertexShaderType::Float, true},
    {"Rgb10A2Unorm", 4, 4, VertexShaderType::Float, true},
    {"UInt1", 4, 1, VertexShaderType::Uint, false},
};

const FormatInfo* Find(VertexFormat format) noexcept {
    const uint32 number = VertexFormatNumber(format);
    if (number == 0 || number > VertexFormatCount) {
        return nullptr;
    }

    return &Formats[number - 1];
}

} // namespace

bool IsValidVertexFormat(VertexFormat format) noexcept {
    return Find(format) != nullptr;
}

uint32 VertexFormatSize(VertexFormat format) noexcept {
    const FormatInfo* info = Find(format);
    return info != nullptr ? info->bytes : 0u;
}

uint32 VertexFormatComponents(VertexFormat format) noexcept {
    const FormatInfo* info = Find(format);
    return info != nullptr ? info->components : 0u;
}

VertexShaderType VertexFormatShaderType(VertexFormat format) noexcept {
    const FormatInfo* info = Find(format);
    return info != nullptr ? info->shader : VertexShaderType::Float;
}

bool IsNormalizedVertexFormat(VertexFormat format) noexcept {
    const FormatInfo* info = Find(format);
    return info != nullptr && info->normalized;
}

Result<VertexFormat> VertexFormatFromNumber(uint32 number) noexcept {
    if (number == 0 || number > VertexFormatCount) {
        return Result<VertexFormat>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<VertexFormat>::Success(static_cast<VertexFormat>(number));
}

const char* VertexFormatName(VertexFormat format) noexcept {
    const FormatInfo* info = Find(format);
    return info != nullptr ? info->name : "Unknown";
}

Result<VertexFormat> ParseVertexFormatName(StringView name) noexcept {
    for (uint32 index = 0; index < VertexFormatCount; ++index) {
        const usize bytes = platform::CStringLength(Formats[index].name);
        if (name.ByteCount() == bytes &&
            platform::MemoryCompare(name.Data(), Formats[index].name, bytes) == 0) {
            return Result<VertexFormat>::Success(static_cast<VertexFormat>(index + 1));
        }
    }

    return Result<VertexFormat>::Failure(ErrorCode::NotFound);
}

const char* VertexShaderTypeName(VertexShaderType type) noexcept {
    switch (type) {
    case VertexShaderType::Float:
        return "float";
    case VertexShaderType::Int:
        return "int";
    case VertexShaderType::Uint:
        return "uint";
    }

    return "unknown";
}

} // namespace rocks::graphics
