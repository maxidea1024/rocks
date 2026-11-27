#include <UiRender/UiFramePacket.h>

namespace rocks::ui {

UiFramePacket::~UiFramePacket() noexcept {
    ReleaseScenes();
}

UiFramePacket::UiFramePacket(UiFramePacket&& other) noexcept
    : renderer_(other.renderer_), targets_(other.targets_) {
    other.renderer_ = nullptr;
    other.targets_ = {};
}

UiFramePacket& UiFramePacket::operator=(UiFramePacket&& other) noexcept {
    if (this != &other) {
        ReleaseScenes();
        renderer_ = other.renderer_;
        targets_ = other.targets_;
        other.renderer_ = nullptr;
        other.targets_ = {};
    }

    return *this;
}

void UiFramePacket::ReleaseScenes() noexcept {
    // 장면 packet은 프레임 arena 원시 저장소에 있어 arena Reset이 소멸자를 부르지 않는다. 여기서
    // 한 번만 부순다(UiSceneRecord::Destroy는 두 번째 호출을 무시한다).
    for (UiFrameTarget& target : targets_) {
        for (UiFramePass& pass : target.passes) {
            for (UiSceneRecord& scene : pass.scenes) {
                scene.Destroy();
            }
        }
    }

    targets_ = {};
}

} // namespace rocks::ui
