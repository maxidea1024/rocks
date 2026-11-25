#include "WidgetPropertyAccess.h"
#include <UiWidget/WidgetTemplate.h>
#include <UiWidget/WidgetAnimation.generated.h>
#include <UiWidget/WidgetEventCatalog.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

WidgetTemplate::WidgetTemplate(Allocator& allocator) noexcept
    : properties_(allocator), animations_(allocator), graph_(allocator), bindings_(allocator),
      events_(allocator) {
}

Result<RootRef<UserWidget>> WidgetTemplate::Instantiate(ObjectRegistry& objects, ObjectHandle outer,
                                                        StringView name,
                                                        const WidgetEventCatalog* events,
                                                        WidgetModelSource model) const noexcept {
    const auto access = objects.OwnerAccess();
    auto& allocator = objects.ObjectAllocator();
    ObjectGraphStage graph(allocator, objects, objects.Types(), access);
    ConstructionContext transaction(objects, access);
    auto owner = transaction.Create<UserWidget>(controller_.Id(), outer, name);
    if (!owner) {
        return Result<RootRef<UserWidget>>::Failure(owner.Error());
    }
    auto base = transaction.DescribeOuter(owner.Value());
    if (!base) {
        return Result<RootRef<UserWidget>>::Failure(base.Error());
    }
    ObjectGraphInput input{};
    input.bytes = graph_.View();
    input.hasBase = true;
    input.base = base.Value();
    auto loaded = graph.Validate(input);
    if (loaded) {
        loaded = graph.CreateObjects(transaction, owner.Value());
    }
    if (loaded) {
        loaded = graph.ApplyFields(transaction);
    }
    if (loaded) {
        loaded = graph.CheckRequired(transaction);
    }
    if (!loaded) {
        return Result<RootRef<UserWidget>>::Failure(loaded.Error());
    }
    auto user = transaction.GetForWrite<UserWidget>(owner.Value());
    if (!user) {
        return Result<RootRef<UserWidget>>::Failure(user.Error());
    }
    auto attached = transaction.SetReference(owner.Value(), user.Value()->tree_, graph.Nodes()[0]);
    if (!attached) {
        return Result<RootRef<UserWidget>>::Failure(attached.Error());
    }

    Array<ObjectHandle> clips(allocator);
    if (!animations_.IsEmpty()) {
        for (const auto& bytes : animations_) {
            ObjectGraphStage clip(allocator, objects, objects.Types(), access);
            input.bytes = bytes.View();
            auto restored = clip.Validate(input);
            if (restored) {
                restored = clip.CreateObjects(transaction, owner.Value());
            }
            if (restored) {
                restored = clip.ApplyFields(transaction);
            }
            if (restored) {
                restored = clip.CheckRequired(transaction);
            }
            if (!restored) {
                return Result<RootRef<UserWidget>>::Failure(restored.Error());
            }
            auto stored = clips.TryPushBack(clip.Nodes()[0]);
            if (!stored) {
                return Result<RootRef<UserWidget>>::Failure(stored.Error());
            }
        }
        auto attachedClips =
            transaction.AssignReferences(owner.Value(), user.Value()->animations_, clips.View());
        if (!attachedClips) {
            return Result<RootRef<UserWidget>>::Failure(attachedClips.Error());
        }
    }

    for (const auto& binding : bindings_) {
        // 다른 저장소에 등록된 스키마에서도 필드 ID로 다시 찾고, 실제 참조 기록은 생성된 thunk에
        // 맡긴다.
        auto type = objects.Types().Acquire(binding.declaringType);
        if (!type) {
            return Result<RootRef<UserWidget>>::Failure(type.Error());
        }
        const auto index = type.Value().FieldIndex(binding.field);
        const auto* ops = index == TypeLease::NoField ? nullptr : type.Value().FieldOps(index);
        if (!ops || !ops->writeStaged) {
            return Result<RootRef<UserWidget>>::Failure(ErrorCode::NotFound);
        }
        const auto& schema = type.Value().Schema().fields[index];
        if (schema.value.kind != ValueKind::ObjectReference) {
            return Result<RootRef<UserWidget>>::Failure(ErrorCode::TypeMismatch);
        }
        FieldValue value(allocator);
        value.kind = ValueKind::ObjectReference;
        if (binding.nodeIndex != IndexNone) {
            value.reference =
                binding.animation ? clips[binding.nodeIndex] : graph.Nodes()[binding.nodeIndex];
        }
        auto written = ops->writeStaged(transaction, owner.Value(), value);
        if (!written) {
            return Result<RootRef<UserWidget>>::Failure(written.Error());
        }
    }

    for (const auto& event : events_) {
        if (!events) {
            return Result<RootRef<UserWidget>>::Failure(ErrorCode::NotReady);
        }
        // 대상 모듈의 등록표에서 다시 검사하고 트리를 공개하기 전에 결합한다.
        auto connected =
            events->Connect(transaction, objects, graph.Nodes()[event.nodeIndex], event.senderType,
                            owner.Value(), controller_.Id(), event.bindings.View());
        if (!connected) {
            return Result<RootRef<UserWidget>>::Failure(connected.Error());
        }
    }
    auto bound = ConnectViewModel(objects, transaction, owner.Value(), *user.Value(), graph.Nodes(),
                                  Move(model));
    if (!bound) {
        return Result<RootRef<UserWidget>>::Failure(bound.Error());
    }
    auto root = transaction.Commit();
    if (!root) {
        return Result<RootRef<UserWidget>>::Failure(root.Error());
    }
    return objects.TryPin<UserWidget>(access, root.Value().Handle());
}

namespace {

Result<SharedPtr<WidgetFieldBindings>>
SourceNotifications(ObjectRegistry& objects, UserWidget& screen, const TypeLease& sourceType,
                    const WidgetModelSource& source) noexcept {
    if (source.object.IsNull()) {
        return screen.FieldNotifications(objects);
    }
    if (!sourceType.IsNativeA(NativeTypeKey<Widget>())) {
        return source.notifications
                   ? Result<SharedPtr<WidgetFieldBindings>>::Success(source.notifications)
                   : Result<SharedPtr<WidgetFieldBindings>>::Failure(ErrorCode::NotReady);
    }
    auto widget = objects.ResolveForWrite<Widget>(objects.OwnerAccess(), source.object);
    if (!widget) {
        return Result<SharedPtr<WidgetFieldBindings>>::Failure(widget.Error());
    }
    return widget.Value().Get().Value()->FieldNotifications(objects);
}

} // namespace

Result<void> WidgetTemplate::ConnectViewModel(ObjectRegistry& objects, ConstructionContext& stage,
                                              ObjectHandle owner, UserWidget& screen,
                                              Span<const ObjectHandle> nodes,
                                              WidgetModelSource model) const noexcept {
    if (properties_.IsEmpty()) {
        return Result<void>::Success();
    }
    const bool external = viewModelType_ != TypeId{};
    if (external != !model.object.IsNull() ||
        (model.notifications && &model.notifications->Registry() != &objects)) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }
    const auto access = objects.OwnerAccess();
    auto& allocator = objects.ObjectAllocator();
    const ObjectHandle handle = external ? model.object : owner;
    const PathTarget origin = external ? PathTarget(handle) : PathTarget::Staged(stage, handle);
    auto type =
        external ? objects.AcquireType(access, handle) : objects.Types().Acquire(controller_.Id());
    if (!type || (external && !type.Value().IsA(viewModelType_))) {
        return Result<void>::Failure(type ? ErrorCode::TypeMismatch : type.Error());
    }
    auto pinned = stage.SetReference(owner, screen.viewModel_, handle);
    if (!pinned) {
        return pinned;
    }
    for (const auto& connection : properties_) {
        auto destination = objects.Types().Acquire(connection.targetType);
        if (!destination) {
            return Result<void>::Failure(destination.Error());
        }
        auto accessor =
            ValidateWidgetPropertyBinding(type.Value(), destination.Value(), connection.binding);
        if (!accessor) {
            return Result<void>::Failure(accessor.Error());
        }
        FieldValue value(allocator);
        const PathStep step{connection.binding.sourceField};
        auto read = PathRead(objects, access, origin, PropertyPath{{&step, 1}}, value);
        if (!read) {
            return read;
        }
        const auto target = nodes[connection.nodeIndex];
        auto applied = accessor.Value().apply(stage, target, value, allocator);
        if (!applied) {
            return applied;
        }
        if (connection.binding.once) {
            continue;
        }
        if (!screen.modelConnections_) {
            auto source = SourceNotifications(objects, screen, type.Value(), model);
            if (!source) {
                return Result<void>::Failure(source.Error());
            }
            if (model.notifications && model.notifications.Get() != source.Value().Get()) {
                return Result<void>::Failure(ErrorCode::WrongContext);
            }
            auto links = SharedPtr<WidgetModelConnections>::TryMake(allocator, allocator,
                                                                    Move(source.Value()));
            if (!links) {
                return Result<void>::Failure(links.Error());
            }
            screen.modelConnections_ = Move(links.Value());
        }
        Result<void> linked = Result<void>::Success();
        if (connection.binding.twoWay) {
            auto widget = stage.GetForWrite<Widget>(target);
            if (!widget) {
                return Result<void>::Failure(widget.Error());
            }
            auto stream = widget.Value()->FieldNotifications(objects);
            if (!stream) {
                return Result<void>::Failure(stream.Error());
            }
            WidgetFieldBindings::Setter reverse;
            if (type.Value().IsNativeA(NativeTypeKey<Widget>())) {
                auto sourceProperty = ResolveWidgetProperty(type.Value(), step.fieldId);
                if (!sourceProperty) {
                    return Result<void>::Failure(sourceProperty.Error());
                }
                reverse = sourceProperty.Value().live();
            }
            linked = screen.modelConnections_->AddTwoWay(
                handle, step.fieldId, target, connection.binding.targetField, Move(stream.Value()),
                accessor.Value().live(), Move(reverse));
        } else {
            linked = screen.modelConnections_->Add(handle, step.fieldId, target,
                                                   accessor.Value().live());
        }
        if (!linked) {
            return linked;
        }
    }
    return Result<void>::Success();
}

} // namespace rocks::ui
