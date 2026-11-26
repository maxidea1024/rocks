#include <UiWidget/NativeWidgetHost.h>
#include "MakeWidgetElement.h"
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

Result<void> NativeWidgetHost::SetContent(const SharedPtr<Element>& content) noexcept {
    if (content.Get() == content_.Get()) {
        return Result<void>::Success();
    }

    if (content && content->Parent()) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (auto host = CachedElement()) {
        if (content_) {
            (void)host->RemoveChild(*content_);
        }

        if (content) {
            auto attached = host->TryAddChild(content);
            if (!attached) {
                if (content_) {
                    (void)host->TryAddChild(content_);
                }

                return attached;
            }
        }
    }

    content_ = content;
    return Result<void>::Success();
}

Result<SharedPtr<Element>> NativeWidgetHost::BuildElement(ObjectRegistry& objects) noexcept {
    auto host = MakeWidgetElement<SizeBox>(objects.ObjectAllocator());
    if (host && content_) {
        auto attached = host.Value()->TryAddChild(content_);
        if (!attached) {
            return Result<SharedPtr<Element>>::Failure(attached.Error());
        }
    }

    return host;
}

void NativeWidgetHost::ReleaseElements() noexcept {
    if (auto host = CachedElement(); host && content_) {
        (void)host->RemoveChild(*content_);
    }

    Widget::ReleaseElements();
}

} // namespace rocks::ui
