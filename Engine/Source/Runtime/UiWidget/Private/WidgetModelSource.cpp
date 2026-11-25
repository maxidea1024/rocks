#include <Core/ScopeExit.h>
#include <UiWidget/WidgetModelSource.h>

namespace rocks::ui {

WidgetModelConnections::WidgetModelConnections(Allocator& allocator,
                                               SharedPtr<WidgetFieldBindings> source) noexcept
    : source_(Move(source)), handles_(allocator) {
}

WidgetModelConnections::~WidgetModelConnections() noexcept {
    for (const auto& entry : handles_) {
        entry.stream->Unbind(entry.handle);
    }
}

Result<void> WidgetModelConnections::Add(ObjectHandle source, Guid field, ObjectHandle target,
                                         WidgetFieldBindings::Setter setter) noexcept {
    return Subscribe(source_, source, field, target, Move(setter));
}

Result<void> WidgetModelConnections::Subscribe(const SharedPtr<WidgetFieldBindings>& stream,
                                               ObjectHandle source, Guid field, ObjectHandle target,
                                               WidgetFieldBindings::Setter setter) noexcept {
    auto reserved = handles_.TryEnsureCapacity(handles_.Size() + 1);
    if (!reserved) {
        return reserved;
    }
    auto added = stream->SubscribeField(source, field, target, Move(setter));
    if (!added) {
        return Result<void>::Failure(added.Error());
    }
    return handles_.TryPushBack({stream, added.Value()});
}

namespace {

struct BindingPair {
    WidgetFieldBindings::Setter forward;
    WidgetFieldBindings::Setter reverse;
    bool updating = false;

    Result<void> Apply(bool toSource, ObjectRegistry& objects, ObjectHandle target,
                       const FieldValue& value) noexcept {
        if (updating) {
            return Result<void>::Success();
        }
        updating = true;
        ROCKS_ON_SCOPE_EXIT {
            updating = false;
        };
        return (toSource ? reverse : forward).Execute(objects, target, value);
    }
};

} // namespace

Result<void> WidgetModelConnections::AddTwoWay(ObjectHandle model, Guid field, ObjectHandle target,
                                               Guid targetField,
                                               SharedPtr<WidgetFieldBindings> targetChanges,
                                               WidgetFieldBindings::Setter forward,
                                               WidgetFieldBindings::Setter reverse) noexcept {
    auto pair = SharedPtr<BindingPair>::TryMake(source_->Registry().ObjectAllocator());
    if (!pair) {
        return Result<void>::Failure(pair.Error());
    }
    pair.Value()->forward = Move(forward);
    if (!reverse.IsBound()) {
        reverse = WidgetFieldBindings::Setter::CreateLambda(
            [changes = WeakPtr<WidgetFieldBindings>(source_),
             field](ObjectRegistry&, ObjectHandle handle, const FieldValue& value) noexcept {
                auto live = changes.Lock();
                if (!live) {
                    return Result<void>::Failure(ErrorCode::Destroyed);
                }
                const PathStep step{field};
                return live->Write(handle, PropertyPath{{&step, 1}}, value);
            });
    }
    pair.Value()->reverse = Move(reverse);
    auto connect = [&](bool toSource) noexcept {
        return WidgetFieldBindings::Setter::CreateLambda(
            [edge = pair.Value(), toSource](ObjectRegistry& objects, ObjectHandle handle,
                                            const FieldValue& value) noexcept {
                return edge->Apply(toSource, objects, handle, value);
            });
    };
    auto first = Subscribe(source_, model, field, target, connect(false));
    if (!first) {
        return first;
    }
    auto second = Subscribe(targetChanges, target, targetField, model, connect(true));
    if (!second) {
        auto partial = handles_.TryPopBack();
        partial.Value().stream->Unbind(partial.Value().handle);
    }
    return second;
}

} // namespace rocks::ui
