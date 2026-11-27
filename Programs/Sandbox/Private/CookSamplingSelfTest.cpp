#include "CookSamplingSelfTest.h"

#include <GraphicsTypes/CookSampling.h>
#include <stdio.h>

namespace sandbox {

using namespace rocks;
using namespace rocks::graphics;

uint32 RunCookSamplingChecks(const char* name, const Device& device) noexcept {
    const Backend backend = device.Info().backend;
    const GraphicsPlatform host = HostGraphicsPlatform();
    // Windows OpenGL ES 미리보기(GLES31_PC)는 Android 열과 견준다(쿠킹 대상이 아니라 보고만).
    const bool preview = backend == Backend::OpenGL && host == GraphicsPlatform::Windows;
    const GraphicsPlatform platform = preview ? GraphicsPlatform::Android : host;
    if (!IsCookSamplingTarget(backend, platform)) {
        printf("  [NOT RUN] %s cook sampling: %s on %s is not a cook target\n", name,
               BackendName(backend), GraphicsPlatformName(platform));
        return 0;
    }

    uint32 filtered = 0;
    uint32 sampledOnly = 0;
    uint32 engine = 0;
    uint32 missing = 0;
    for (uint32 number = 1; number <= MaxPixelFormatNumber; ++number) {
        const auto format = static_cast<PixelFormat>(number);
        const PixelFormatCaps guaranteed = CookSamplingGuarantee(backend, platform, format);
        if (guaranteed == PixelFormatCaps::None) {
            continue;
        }

        const bool filter = HasPixelFormatCaps(guaranteed, PixelFormatCaps::Filterable);
        filtered += filter ? 1u : 0u;
        sampledOnly += filter ? 0u : 1u;
        engine += CookSamplingGuaranteeBasis(backend, platform, format) ==
                          CookSamplingBasis::EngineRequirement
                      ? 1u
                      : 0u;
        const PixelFormatCaps caps = device.GetPixelFormatCaps(format);
        if (!HasPixelFormatCaps(caps, guaranteed)) {
            ++missing;
            printf("    %s %s: %s lacks %s\n", preview ? "[NOTE]" : "[FAIL]", name,
                   PixelFormatName(format), filter ? "Sampled|Filterable" : "Sampled");
        }
    }

    const char* platformName = GraphicsPlatformName(platform);
    if (preview) {
        printf("  [INFO] %s cook sampling vs %s/%s (preview, not a cook target): %u filtered, %u "
               "sampled-only, %u engine-required; %u missing on this preview context\n",
               name, BackendName(backend), platformName, filtered, sampledOnly, engine, missing);
        return 0;
    }

    printf("  [%s] %s cook sampling %s/%s: %u filtered, %u sampled-only, %u engine-required, %u "
           "missing\n",
           missing == 0 ? "PASS" : "FAIL", name, BackendName(backend), platformName, filtered,
           sampledOnly, engine, missing);
    return missing;
}

} // namespace sandbox
