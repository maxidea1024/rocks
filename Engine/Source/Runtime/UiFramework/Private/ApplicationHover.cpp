#include <UiFramework/Application.h>

namespace rocks::ui {

bool Application::IsHovered(const Element& target, uint32 user) const noexcept {
    for (const auto& entry : hover_) {
        if (entry.user != user) {
            continue;
        }

        for (const auto& weak : entry.path) {
            if (weak.Lock().Get() == &target) {
                return root_ && target.IsInputPathActive(*root_, true);
            }
        }
    }

    return false;
}

void Application::ClearHover(uint32 user) noexcept {
    CloseToolTip(user);
    for (usize i = 0; i < hover_.Size(); ++i) {
        if (hover_[i].user != user) {
            continue;
        }

        auto path = Move(hover_[i].path);
        (void)hover_.TryRemoveAt(i);
        ++hoverVersion_;
        for (const auto& weak : path) {
            if (auto item = weak.Lock()) {
                item->OnMouseLeave(user);
            }
        }

        return;
    }
}

Result<void> Application::UpdateHover(uint32 user, Element* hit,
                                      const SharedPtr<Element>& scope) noexcept {
    usize index = 0;
    while (index < hover_.Size() && hover_[index].user != user) {
        ++index;
    }

    if (!hit) {
        ClearHover(user);
        return Result<void>::Success();
    }

    // 안정된 포인터 경로에서는 배열 할당과 사용자 콜백을 생략한다.
    if (index < hover_.Size()) {
        const auto& path = hover_[index].path;
        const Element* node = hit;
        usize n = 0;
        while (n < path.Size() && node && path[n].Lock().Get() == node) {
            ++n;
            if (node == scope.Get()) {
                node = nullptr;
                break;
            }

            node = node->Parent();
        }

        if (!node && n == path.Size()) {
            return Result<void>::Success();
        }
    }

    Array<SharedPtr<Element>> path(allocator_);
    auto built = BuildPath(hit, scope, path);
    if (!built) {
        return built;
    }

    Array<WeakPtr<Element>> next(allocator_);
    auto reserved = next.TryReserve(path.Size());
    if (!reserved) {
        return reserved;
    }

    for (const auto& item : path) {
        (void)next.TryPushBack(WeakPtr<Element>(item));
    }

    if (index == hover_.Size()) {
        auto added = hover_.TryPushBack(Hover{user, Array<WeakPtr<Element>>(allocator_)});
        if (!added) {
            return added;
        }
    }

    auto previous = Move(hover_[index].path);
    hover_[index].path = Move(next);
    const uint64 version = ++hoverVersion_;
    usize exits = previous.Size(), enters = path.Size();
    while (exits && enters && previous[exits - 1].Lock() == path[enters - 1]) {
        --exits;
        --enters;
    }

    for (usize i = 0; i < exits && version == hoverVersion_; ++i) {
        if (auto item = previous[i].Lock()) {
            item->OnMouseLeave(user);
        }
    }
    while (enters && version == hoverVersion_) {
        auto& item = path[--enters];
        if (root_ && item->IsInputPathActive(*root_, true)) {
            item->OnMouseEnter(user);
        }
    }

    return Result<void>::Success();
}
} // namespace rocks::ui
