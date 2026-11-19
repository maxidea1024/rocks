#include "SelfTestEnvironment.h"

#include <Core/Text/StringView.h>
#include <PlatformWindow/Window.h>

namespace sandbox {

using namespace rocks;

const char* WindowPresentUnavailableReason(graphics::Device& device) noexcept {
    const graphics::DeviceInfo info = device.Info();
    // mesa 소프트웨어 래스터라이저(lavapipe)만 GDI WSI를 쓴다. 장치 이름이 "llvmpipe"로 시작한다.
    const bool gdiPresent = info.backend == graphics::Backend::Vulkan && info.software &&
                            info.adapterName.StartsWith(StringView("llvmpipe"));
    if (gdiPresent &&
        platform::QueryDesktopSessionState() == platform::DesktopSessionState::Disconnected) {
        return "the desktop session is disconnected, and lavapipe presents with GDI StretchBlt to "
               "the window DC, which fails for hidden windows in a disconnected session (mesa "
               "src/vulkan/wsi/wsi_common_win32.cpp:823-832, VK_ERROR_MEMORY_MAP_FAILED)";
    }

    return nullptr;
}

} // namespace sandbox
