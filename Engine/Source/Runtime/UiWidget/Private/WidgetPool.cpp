#include <Core/ScopeExit.h>
#include <UiWidget/WidgetPool.h>

#include <UiWidget/UserWidget.generated.h>

namespace rocks::ui {

WidgetPool::WidgetPool(ObjectRegistry& objects, TypeId widgetClass) noexcept
    : objects_(objects), widgetClass_(widgetClass), entries_(objects.ObjectAllocator()) {
}

WidgetPool::~WidgetPool() noexcept {
    (void)Reset();
}

usize WidgetPool::ActiveCount() const noexcept {
    return activeCount_;
}

usize WidgetPool::InactiveCount() const noexcept {
    return entries_.Size() - activeCount_;
}

Result<WidgetPoolEntry> WidgetPool::Acquire() noexcept {
    if (updating_) {
        return Result<WidgetPoolEntry>::Failure(ErrorCode::Busy);
    }
    updating_ = true;
    ROCKS_ON_SCOPE_EXIT {
        updating_ = false;
    };

    const auto access = objects_.OwnerAccess();
    const bool create = activeCount_ == entries_.Size();
    RootRef<UserWidget> created;
    if (create) {
        auto object = objects_.NewObject<UserWidget>(access, widgetClass_);
        if (!object) {
            return Result<WidgetPoolEntry>::Failure(object.Error());
        }
        created = Move(object.Value());
    }
    const auto handle = create ? created.Handle() : entries_[activeCount_].widget.Handle();
    auto borrowed = objects_.ResolveForWrite<UserWidget>(access, handle);
    if (!borrowed) {
        return Result<WidgetPoolEntry>::Failure(borrowed.Error());
    }
    auto element = borrowed.Value().Get().Value()->TakeElement(objects_);
    if (!element) {
        return Result<WidgetPoolEntry>::Failure(element.Error());
    }
    if (create) {
        auto added = entries_.TryPushBack(Entry{Move(created), element.Value()});
        if (!added) {
            return Result<WidgetPoolEntry>::Failure(added.Error());
        }
    } else {
        entries_[activeCount_].element = element.Value();
    }
    ++activeCount_;
    return Result<WidgetPoolEntry>::Success({handle, Move(element.Value())});
}

Result<void> WidgetPool::Release(ObjectHandle widget, bool cacheElement) noexcept {
    if (updating_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    for (usize i = 0; i < activeCount_; ++i) {
        auto& entry = entries_[i];
        if (entry.widget.Handle() != widget) {
            continue;
        }
        if (entry.element->Parent()) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        updating_ = true;
        ROCKS_ON_SCOPE_EXIT {
            updating_ = false;
        };

        if (!cacheElement) {
            auto borrowed = objects_.ResolveForWrite<UserWidget>(objects_.OwnerAccess(), widget);
            if (!borrowed) {
                return Result<void>::Failure(borrowed.Error());
            }
            borrowed.Value().Get().Value()->ReleaseElements();
            entry.element.Reset();
        }
        --activeCount_;
        if (i != activeCount_) {
            Entry other = Move(entries_[activeCount_]);
            entries_[activeCount_] = Move(entry);
            entry = Move(other);
        }
        return Result<void>::Success();
    }
    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> WidgetPool::Reset() noexcept {
    if (updating_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    updating_ = true;
    ROCKS_ON_SCOPE_EXIT {
        updating_ = false;
    };
    for (auto& entry : entries_) {
        if (entry.element) {
            if (auto* parent = entry.element->Parent()) {
                (void)parent->RemoveChild(*entry.element);
            }
            auto widget =
                objects_.ResolveForWrite<UserWidget>(objects_.OwnerAccess(), entry.widget.Handle());
            if (widget) {
                widget.Value().Get().Value()->ReleaseElements();
            }
        }
    }
    entries_.Clear();
    activeCount_ = 0;
    return Result<void>::Success();
}

} // namespace rocks::ui
