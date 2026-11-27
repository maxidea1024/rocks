#include <ShaderCompilerCommon/ShaderDebugSymbols.h>

#include <Platform/Memory.h>

// 값 3 DebugSymbols 배치(Docs/Design/ShaderSystem 16.3절 SSD73)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace debug_symbols_detail {

// 머리 byte 수(u32 버전 + u8 종류 + u16 이름 byte 수)다.
constexpr usize HeaderBytes = 4 + 1 + 2;

bool IsValidKind(uint8 kind) noexcept {
    return kind == static_cast<uint8>(ShaderDebugSymbolKind::Pdb) ||
           kind == static_cast<uint8>(ShaderDebugSymbolKind::SpirvModule);
}

bool IsValidName(StringView name) noexcept {
    if (name.ByteCount() > MaxShaderDebugSymbolNameBytes || name == StringView(".") ||
        name == StringView("..")) {
        return false;
    }

    for (const char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0') {
            return false;
        }
    }

    return name.IsValidUtf8();
}

} // namespace debug_symbols_detail

const char* ShaderDebugSymbolExtension(ShaderDebugSymbolKind kind) noexcept {
    switch (kind) {
    case ShaderDebugSymbolKind::Pdb:
        return ".pdb";
    case ShaderDebugSymbolKind::SpirvModule:
        return ".spv";
    }

    return ".bin";
}

Result<void> EncodeShaderDebugSymbols(ShaderDebugSymbolKind kind, StringView name,
                                      Span<const uint8> contents, Array<uint8>& out) noexcept {
    using namespace debug_symbols_detail;
    out.Clear();
    if (!IsValidKind(static_cast<uint8>(kind)) || !IsValidName(name)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!out.TryResize(HeaderBytes + name.ByteCount() + contents.Size())) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    uint8* data = out.Data();
    const uint32 version = ShaderDebugSymbolsVersion;
    const uint16 nameBytes = static_cast<uint16>(name.ByteCount());
    data[0] = static_cast<uint8>(version);
    data[1] = static_cast<uint8>(version >> 8);
    data[2] = static_cast<uint8>(version >> 16);
    data[3] = static_cast<uint8>(version >> 24);
    data[4] = static_cast<uint8>(kind);
    data[5] = static_cast<uint8>(nameBytes);
    data[6] = static_cast<uint8>(nameBytes >> 8);
    if (nameBytes != 0) {
        platform::MemoryCopy(data + HeaderBytes, name.Data(), nameBytes);
    }

    if (!contents.IsEmpty()) {
        platform::MemoryCopy(data + HeaderBytes + nameBytes, contents.Data(), contents.Size());
    }

    return Result<void>::Success();
}

Result<ShaderDebugSymbolsView> DecodeShaderDebugSymbols(Span<const uint8> bytes) noexcept {
    using namespace debug_symbols_detail;
    const auto corrupt = []() noexcept {
        return Result<ShaderDebugSymbolsView>::Failure(ErrorCode::CorruptData);
    };
    if (bytes.Size() < HeaderBytes) {
        return corrupt();
    }

    const uint32 version = static_cast<uint32>(bytes[0]) | (static_cast<uint32>(bytes[1]) << 8) |
                           (static_cast<uint32>(bytes[2]) << 16) |
                           (static_cast<uint32>(bytes[3]) << 24);
    const uint8 kind = bytes[4];
    const usize nameBytes = static_cast<usize>(bytes[5]) | (static_cast<usize>(bytes[6]) << 8);
    if (version != ShaderDebugSymbolsVersion || !IsValidKind(kind) ||
        bytes.Size() - HeaderBytes < nameBytes) {
        return corrupt();
    }

    ShaderDebugSymbolsView view;
    view.kind = static_cast<ShaderDebugSymbolKind>(kind);
    view.name = StringView(reinterpret_cast<const char*>(bytes.Data()) + HeaderBytes, nameBytes);
    if (!IsValidName(view.name)) {
        return corrupt();
    }

    view.contents = Span<const uint8>(bytes.Data() + HeaderBytes + nameBytes,
                                      bytes.Size() - HeaderBytes - nameBytes);
    return Result<ShaderDebugSymbolsView>::Success(view);
}

} // namespace rocks::shader
