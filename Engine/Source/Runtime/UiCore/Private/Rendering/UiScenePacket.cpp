#include <UiCore/Rendering/UiScenePacket.h>

namespace rocks::ui {

UiScenePacket::~UiScenePacket() noexcept {
    Reset();
}

UiScenePacket::UiScenePacket(UiScenePacket&& other) noexcept
    : allocator_(other.allocator_), object_(other.object_), size_(other.size_),
      alignment_(other.alignment_), move_(other.move_), invoke_(other.invoke_),
      function_(other.function_), destroy_(other.destroy_) {
    other.object_ = nullptr;
    other.allocator_ = nullptr;
}

UiScenePacket& UiScenePacket::operator=(UiScenePacket&& other) noexcept {
    if (this != &other) {
        Reset();
        allocator_ = other.allocator_;
        object_ = other.object_;
        size_ = other.size_;
        alignment_ = other.alignment_;
        move_ = other.move_;
        invoke_ = other.invoke_;
        function_ = other.function_;
        destroy_ = other.destroy_;
        other.object_ = nullptr;
        other.allocator_ = nullptr;
    }

    return *this;
}

UiSceneRecord UiScenePacket::MoveTo(void* destination) noexcept {
    if (!object_ || !destination) {
        return {};
    }

    move_(destination, object_);
    UiSceneRecord record{destination, invoke_, function_, destroy_};
    Reset();
    return record;
}

void UiScenePacket::Reset() noexcept {
    if (object_) {
        destroy_(object_);
        (void)allocator_->Deallocate(object_);
        object_ = nullptr;
    }

    allocator_ = nullptr;
}

} // namespace rocks::ui
