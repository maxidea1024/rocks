#include <Core/ScopeExit.h>
#include <UiWidget/WidgetFieldBindings.h>

namespace rocks::ui {

namespace {

struct FieldConnection {
    FieldConnection(ObjectHandle sourceHandle, ObjectHandle targetHandle,
                    WidgetFieldBindings::Setter callback) noexcept
        : source(sourceHandle), target(targetHandle), setter(Move(callback)) {
    }

    Result<void> Apply(ObjectRegistry& objects) noexcept {
        if (applying) {
            return Result<void>::Failure(ErrorCode::CycleDetected);
        }

        applying = true;
        ROCKS_ON_SCOPE_EXIT {
            applying = false;
        };
        FieldValue value(objects.ObjectAllocator());
        auto read = PathRead(objects, objects.OwnerAccess(), source, path.View(), value);
        if (!read) {
            return read;
        }

        if (!setter.IsBound()) {
            return Result<void>::Failure(ErrorCode::Destroyed);
        }

        return setter.Execute(objects, target, value);
    }

    ObjectHandle source;
    ObjectHandle target;
    WidgetFieldBindings::Setter setter;
    PropertyPathBuffer path;
    DelegateHandle handle;
    bool applying = false;
};

} // namespace

WidgetFieldBindings::WidgetFieldBindings(ObjectRegistry& objects) noexcept
    : objects_(objects), changed_(objects.ObjectAllocator()) {
}

Result<DelegateHandle> WidgetFieldBindings::Bind(ObjectHandle source, StringView path,
                                                 ObjectHandle target, Setter setter) noexcept {
    if (!setter.IsBound()) {
        return Result<DelegateHandle>::Failure(ErrorCode::InvalidArgument);
    }

    PropertyPathBuffer resolved;
    auto parsed = ParsePropertyPath(objects_, objects_.OwnerAccess(), source, path, resolved);
    if (!parsed) {
        return Result<DelegateHandle>::Failure(parsed.Error());
    }

    return Connect(source, resolved, target, Move(setter), true);
}

Result<DelegateHandle> WidgetFieldBindings::SubscribeField(ObjectHandle source, Guid field,
                                                           ObjectHandle target,
                                                           Setter setter) noexcept {
    if (!setter.IsBound() || field == Guid{}) {
        return Result<DelegateHandle>::Failure(ErrorCode::InvalidArgument);
    }

    PropertyPathBuffer resolved;
    resolved.steps[0] = {field};
    resolved.count = 1;
    return Connect(source, resolved, target, Move(setter), false);
}

Result<DelegateHandle> WidgetFieldBindings::Connect(ObjectHandle source,
                                                    const PropertyPathBuffer& path,
                                                    ObjectHandle target, Setter setter,
                                                    bool initial) noexcept {
    auto made = SharedPtr<FieldConnection>::TryMake(objects_.ObjectAllocator(), source, target,
                                                    Move(setter));
    if (!made) {
        return Result<DelegateHandle>::Failure(made.Error());
    }

    auto& entry = *made.Value();
    entry.path = path;
    auto subscribed =
        changed_.TryAddLambda([this, binding = made.Value()](ObjectHandle changed, Guid field,
                                                             ErrorCode& first) noexcept {
            if (binding->source != changed || binding->path.steps[0].fieldId != field) {
                return;
            }

            auto result = binding->Apply(objects_);
            if (!result) {
                const auto error = result.Error();
                if (error == ErrorCode::Destroyed || error == ErrorCode::StaleHandle) {
                    changed_.Remove(binding->handle);
                } else if (first == ErrorCode::None) {
                    first = error;
                }
            }
        });
    if (!subscribed) {
        return subscribed;
    }

    entry.handle = subscribed.Value();
    if (!initial) {
        return subscribed;
    }

    auto applied = entry.Apply(objects_);
    if (!applied) {
        changed_.Remove(entry.handle);
        return Result<DelegateHandle>::Failure(applied.Error());
    }

    return subscribed;
}

Result<void> WidgetFieldBindings::ApplyOnce(ObjectHandle source, StringView path,
                                            ObjectHandle target, Setter setter) noexcept {
    if (!setter.IsBound()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    FieldConnection entry(source, target, Move(setter));
    auto parsed = ParsePropertyPath(objects_, objects_.OwnerAccess(), source, path, entry.path);
    if (!parsed) {
        return parsed;
    }

    return entry.Apply(objects_);
}

void WidgetFieldBindings::Unbind(DelegateHandle handle) noexcept {
    changed_.Remove(handle);
}

Result<void> WidgetFieldBindings::Notify(ObjectHandle source, Guid field) noexcept {
    ErrorCode first = ErrorCode::None;
    changed_.Broadcast(source, field, first);
    return first == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(first);
}

ObjectRegistry& WidgetFieldBindings::Registry() const noexcept {
    return objects_;
}

Result<void> WidgetFieldBindings::NotifyNamed(ObjectHandle source, StringView fieldName) noexcept {
    if (!changed_.IsBound()) {
        return Result<void>::Success();
    }

    auto type = objects_.AcquireType(objects_.OwnerAccess(), source);
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    for (const auto& field : type.Value().Schema().fields) {
        // 이벤트 필드는 값 속성이 아니다(EventReflection ED9 — 변경 통지 대상이 아니다).
        if (IsEventField(field)) {
            continue;
        }

        auto name = field.toolName.IsEmpty() ? field.name : field.toolName;
        if (StringView(name.Data(), name.Size()) == fieldName) {
            return Notify(source, field.id);
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> WidgetFieldBindings::Write(ObjectHandle source, PropertyPath path,
                                        const FieldValue& value) noexcept {
    auto written = PathWrite(objects_, objects_.OwnerAccess(), source, path, value);
    if (!written) {
        return written;
    }

    return Notify(source, path.steps[0].fieldId);
}

} // namespace rocks::ui
