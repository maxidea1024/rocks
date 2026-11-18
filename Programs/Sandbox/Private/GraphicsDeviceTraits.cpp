#include "GraphicsDeviceTraits.h"

namespace sandbox {

using namespace rocks;

Result<void> AppendGraphicsDeviceTraits(const graphics::DeviceCaps& caps,
                                        config::DeviceTraits& traits) noexcept {
    namespace names = config::DeviceTraitNames;
    const struct {
        StringView name;
        double value;
    } values[] = {
        {names::GpuTextureSize2D, static_cast<double>(caps.limits.textureSize2D)},
        {names::GpuColorAttachments, static_cast<double>(caps.limits.colorAttachments)},
        {names::GpuPixelSampledTextures, static_cast<double>(caps.limits.pixel.sampledTextures)},
        {names::GpuSamplerAnisotropy, static_cast<double>(caps.limits.samplerAnisotropy)},
        {names::GpuComputeShaders, caps.features.computeShaders ? 1.0 : 0.0},
        {names::GpuShaderModel, static_cast<double>(caps.features.shaderModel)},
    };
    for (const auto& value : values) {
        if (auto set = traits.SetNumber(value.name, value.value); !set) {
            return set;
        }
    }

    return Result<void>::Success();
}

} // namespace sandbox
