#include <UiWidget/WidgetEventCatalog.h>

namespace rocks::ui {

WidgetEventCatalog::WidgetEventCatalog(Allocator& allocator) noexcept
    : allocator_(allocator), events_(allocator), methods_(allocator) {
}

Result<void> WidgetEventCatalog::CheckDescriptor(const Descriptor& value) const noexcept {
    if (value.id == Guid{} || value.signature == Guid{} || value.owner == TypeId{}) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const auto conflicts = [&](const Descriptor& other) noexcept {
        return value.signature == other.signature && value.nativeSignature != other.nativeSignature;
    };
    for (const auto& event : events_) {
        if (conflicts(event.desc)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
    }

    for (const auto& method : methods_) {
        if (conflicts(method.desc)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
    }

    return Result<void>::Success();
}

const WidgetEventCatalog::EventEntry* WidgetEventCatalog::FindEvent(Guid id) const noexcept {
    for (const auto& event : events_) {
        if (event.desc.id == id) {
            return &event;
        }
    }

    return nullptr;
}

const WidgetEventCatalog::MethodEntry* WidgetEventCatalog::FindMethod(Guid id) const noexcept {
    for (const auto& method : methods_) {
        if (method.desc.id == id) {
            return &method;
        }
    }

    return nullptr;
}

Result<void> WidgetEventCatalog::AddEvent(EventEntry entry) noexcept {
    if (FindEvent(entry.desc.id)) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto valid = CheckDescriptor(entry.desc);
    if (!valid) {
        return valid;
    }

    return events_.TryPushBack(Move(entry));
}

Result<void> WidgetEventCatalog::AddMethod(MethodEntry entry) noexcept {
    if (FindMethod(entry.desc.id)) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto valid = CheckDescriptor(entry.desc);
    if (!valid) {
        return valid;
    }

    return methods_.TryPushBack(Move(entry));
}

Result<void> WidgetEventCatalog::Validate(const TypeLease& sender, const TypeLease& controller,
                                          const WidgetEventBinding& binding) const noexcept {
    const auto* event = FindEvent(binding.eventId);
    const auto* method = FindMethod(binding.methodId);
    if (!event || !method) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (!sender.IsA(event->desc.owner) || !controller.IsA(method->desc.owner) ||
        binding.signature != event->desc.signature || binding.signature != method->desc.signature ||
        event->desc.nativeSignature != method->desc.nativeSignature) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    return Result<void>::Success();
}

Result<void> WidgetEventCatalog::Connect(ConstructionContext& stage, ObjectRegistry& objects,
                                         ObjectHandle sender, TypeId senderType,
                                         ObjectHandle controller, TypeId controllerType,
                                         Span<const WidgetEventBinding> bindings) const noexcept {
    auto source = objects.Types().Acquire(senderType);
    auto target = objects.Types().Acquire(controllerType);
    if (!source || !target) {
        return Result<void>::Failure(!source ? source.Error() : target.Error());
    }

    auto& allocator = objects.ObjectAllocator();
    auto methods = SharedPtr<MethodList>::TryMake(allocator, allocator);
    if (!methods) {
        return Result<void>::Failure(methods.Error());
    }

    for (const auto& binding : bindings) {
        auto valid = Validate(source.Value(), target.Value(), binding);
        if (!valid) {
            return valid;
        }

        auto stored = methods.Value()->TryPushBack(FindMethod(binding.methodId)->method);
        if (!stored) {
            return stored;
        }
    }

    return FindEvent(bindings[0].eventId)
        ->attach.Execute(stage, sender, objects, controller, methods.Value());
}

} // namespace rocks::ui
