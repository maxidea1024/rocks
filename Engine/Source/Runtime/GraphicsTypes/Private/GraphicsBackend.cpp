#include <GraphicsTypes/GraphicsBackend.h>
#include <GraphicsTypes/GraphicsPlatform.h>
#include <Platform/Memory.h>

namespace rocks::graphics {

namespace {

// name이 C 문자열 text와 byte 단위로 같은지다(대소문자 구분).
bool SameGraphicsName(StringView name, const char* text) noexcept {
    const usize bytes = platform::CStringLength(text);
    return name.ByteCount() == bytes && platform::MemoryCompare(name.Data(), text, bytes) == 0;
}

} // namespace

const char* BackendName(Backend backend) noexcept {
    switch (backend) {
    case Backend::D3D12:
        return "D3D12";
    case Backend::D3D11:
        return "D3D11";
    case Backend::OpenGL:
        return "OpenGL";
    case Backend::Vulkan:
        return "Vulkan";
    case Backend::Metal:
        return "Metal";
    }

    return "Unknown";
}

Result<Backend> ParseBackendName(StringView name) noexcept {
    for (uint32 value = 0; value < BackendCount; ++value) {
        const auto backend = static_cast<Backend>(value);
        if (SameGraphicsName(name, BackendName(backend))) {
            return Result<Backend>::Success(backend);
        }
    }

    return Result<Backend>::Failure(ErrorCode::NotFound);
}

const char* GraphicsPlatformName(GraphicsPlatform platform) noexcept {
    switch (platform) {
    case GraphicsPlatform::Windows:
        return "Windows";
    case GraphicsPlatform::Mac:
        return "Mac";
    case GraphicsPlatform::Android:
        return "Android";
    }

    return "Unknown";
}

Result<GraphicsPlatform> ParseGraphicsPlatformName(StringView name) noexcept {
    for (uint32 value = 0; value < GraphicsPlatformCount; ++value) {
        const auto platform = static_cast<GraphicsPlatform>(value);
        if (SameGraphicsName(name, GraphicsPlatformName(platform))) {
            return Result<GraphicsPlatform>::Success(platform);
        }
    }

    return Result<GraphicsPlatform>::Failure(ErrorCode::NotFound);
}

} // namespace rocks::graphics
