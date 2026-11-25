#include "WidgetPropertyAccess.h"
#include <UiWidget/WidgetAsset.h>
#include <UiWidget/WidgetAnimation.generated.h>
#include <UiWidget/WidgetEventCatalog.h>

#include <Core/Containers/HashMap.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <UiWidget/WidgetAsset.generated.h>

#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/UserWidget.generated.h>

namespace rocks::ui {

WidgetAsset::WidgetAsset(Allocator& allocator) noexcept
    : animations_(allocator), controller_(ClassRef<UserWidget>::Of<UserWidget>()),
      bindings_(allocator), propertyBindings_(allocator), eventBindings_(allocator) {
}

Result<void> WidgetAsset::Initialize(ObjectRegistry& objects) noexcept {
    if (!tree_.Handle().IsNull()) {
        return Result<void>::Success();
    }
    const auto access = objects.OwnerAccess();
    auto tree = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id(), Handle(),
                                              "WidgetTree");
    if (!tree) {
        return Result<void>::Failure(tree.Error());
    }
    return objects.SetReference(access, Handle(), tree_, tree.Value().Handle());
}

ObjectHandle WidgetAsset::Tree() const noexcept {
    return tree_.Handle();
}

void WidgetAsset::SetController(ClassRef<UserWidget> controller) noexcept {
    controller_ = controller;
}

Result<void> WidgetAsset::AddBinding(WidgetBinding binding) noexcept {
    if (binding.field == Guid{} || binding.widget == Guid{}) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    for (const auto& old : bindings_) {
        if (old.field == binding.field) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }
    return bindings_.TryPushBack(binding);
}

void WidgetAsset::SetViewModelType(TypeId model) noexcept {
    viewModelType_ = model;
}

Result<void> WidgetAsset::AddPropertyBinding(WidgetPropertyBinding binding) noexcept {
    if (binding.widget == Guid{} || binding.sourceField == Guid{} ||
        binding.targetField == Guid{}) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    for (const auto& previous : propertyBindings_) {
        if (previous.widget == binding.widget && previous.targetField == binding.targetField) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }
    return propertyBindings_.TryPushBack(binding);
}

Result<void> WidgetAsset::AddEventBinding(WidgetEventBinding binding) noexcept {
    if (binding.widget == Guid{} || binding.eventId == Guid{} || binding.methodId == Guid{} ||
        binding.signature == Guid{}) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    for (const auto& prior : eventBindings_) {
        if (binding.widget == prior.widget && binding.eventId == prior.eventId &&
            binding.methodId == prior.methodId) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }
    return eventBindings_.TryPushBack(binding);
}

Result<WidgetTemplate> WidgetAsset::Compile(ObjectRegistry& source, Allocator& allocator,
                                            const WidgetEventCatalog* events) const noexcept {
    const auto access = source.OwnerAccess();
    auto controller = controller_.Resolve(source.Types());
    if (!controller) {
        return Result<WidgetTemplate>::Failure(controller.Error());
    }
    if (controller.Value().Schema().flags & static_cast<uint32>(TypeFlags::Abstract)) {
        return Result<WidgetTemplate>::Failure(ErrorCode::InvalidArgument);
    }
    const auto tree = tree_.Handle();
    auto outer = source.GetOuter(access, tree);
    if (!outer || outer.Value() != Handle()) {
        return Result<WidgetTemplate>::Failure(ErrorCode::WrongContext);
    }
    auto treeBorrow = source.Resolve<WidgetTree>(access, tree);
    if (!treeBorrow) {
        return Result<WidgetTemplate>::Failure(treeBorrow.Error());
    }
    auto members = treeBorrow.Value().Get().Value()->IndexWidgets(source);
    if (!members) {
        return Result<WidgetTemplate>::Failure(members.Error());
    }
    WidgetTemplate result(allocator);
    result.controller_ = controller_;
    ObjectGraphSaveOptions options;
    options.externals = GraphExternalPolicy::Include;
    options.writeNodeTable = true;
    auto graph = SaveObjectGraphSet(allocator, source, access, {&tree, 1}, options);
    if (!graph) {
        return Result<WidgetTemplate>::Failure(graph.Error());
    }
    const auto nodes = graph.Value().Nodes();
    HashMap<Guid, usize> targets(allocator);
    for (usize i = 1; i < nodes.Size(); ++i) {
        auto parent = source.GetOuter(access, nodes[i]);
        if (!parent) {
            return Result<WidgetTemplate>::Failure(parent.Error());
        }
        // 다른 UserWidget의 내부 트리와 슬롯은 인스턴스에 복제하되 이 컨트롤러의 결합 후보에서는
        // 제외한다.
        if (parent.Value() == Handle()) {
            return Result<WidgetTemplate>::Failure(ErrorCode::WrongContext);
        }
        if (parent.Value() != tree) {
            continue;
        }
        auto widget = source.Resolve<Widget>(access, nodes[i]);
        if (!widget) {
            return Result<WidgetTemplate>::Failure(widget.Error());
        }
        const Guid id = widget.Value().Get().Value()->StableId();
        if (!members.Value().Contains(id) || targets.Contains(id)) {
            return Result<WidgetTemplate>::Failure(ErrorCode::InvalidArgument);
        }
        auto inserted = targets.Insert(id, i);
        if (!inserted) {
            return Result<WidgetTemplate>::Failure(inserted.Error());
        }
    }
    HashMap<Guid, usize> clipIds(allocator);
    for (const auto handle : animations_.View()) {
        auto parent = source.GetOuter(access, handle);
        if (!parent || parent.Value() != Handle()) {
            return Result<WidgetTemplate>::Failure(parent ? ErrorCode::WrongContext
                                                          : parent.Error());
        }
        auto borrowed = source.Resolve<WidgetAnimation>(access, handle);
        if (!borrowed) {
            return Result<WidgetTemplate>::Failure(borrowed.Error());
        }
        const auto* clip = borrowed.Value().Get().Value();
        if (clip->StableId() == Guid{} || clip->Tracks().IsEmpty() ||
            !math::IsFinite(clip->Duration()) || clip->Duration() <= 0) {
            return Result<WidgetTemplate>::Failure(ErrorCode::InvalidArgument);
        }
        if (targets.Contains(clip->StableId())) {
            return Result<WidgetTemplate>::Failure(ErrorCode::AlreadyExists);
        }
        auto unique = clipIds.Insert(clip->StableId(), result.animations_.Size());
        if (!unique || unique.Value() != InsertOutcome::Inserted) {
            return Result<WidgetTemplate>::Failure(unique ? ErrorCode::AlreadyExists
                                                          : unique.Error());
        }
        HashMap<Guid, uint8> channels(allocator);
        for (const auto& track : clip->Tracks()) {
            if (track.channel > static_cast<uint8>(WidgetAnimationChannel::Angle) ||
                !track.curve.KeyCount()) {
                return Result<WidgetTemplate>::Failure(ErrorCode::InvalidArgument);
            }
            const auto flag = static_cast<uint8>(1u << track.channel);
            auto* mask = channels.Find(track.target);
            if (mask) {
                if ((*mask & flag) != 0) {
                    return Result<WidgetTemplate>::Failure(ErrorCode::AlreadyExists);
                }
                *mask |= flag;
            } else {
                auto recorded = channels.Insert(track.target, flag);
                if (!recorded) {
                    return Result<WidgetTemplate>::Failure(recorded.Error());
                }
            }
            if (track.target != Guid{} && !targets.Contains(track.target)) {
                return Result<WidgetTemplate>::Failure(ErrorCode::NotFound);
            }
        }
        auto bytes = SaveObjectGraph(allocator, source, access, handle, options);
        if (!bytes) {
            return Result<WidgetTemplate>::Failure(bytes.Error());
        }
        auto stored = result.animations_.TryPushBack(Move(bytes.Value()));
        if (!stored) {
            return Result<WidgetTemplate>::Failure(stored.Error());
        }
    }

    for (const auto& binding : bindings_) {
        for (const auto& prior : result.bindings_) {
            if (prior.field == binding.field) {
                return Result<WidgetTemplate>::Failure(ErrorCode::AlreadyExists);
            }
        }
        if (binding.widget == Guid{}) {
            return Result<WidgetTemplate>::Failure(ErrorCode::InvalidArgument);
        }
        const auto index = controller.Value().FieldIndex(binding.field);
        if (index == TypeLease::NoField) {
            return Result<WidgetTemplate>::Failure(ErrorCode::NotFound);
        }
        const auto& field = controller.Value().Schema().fields[index];
        const auto* ops = controller.Value().FieldOps(index);
        if (field.value.kind != ValueKind::ObjectReference || !ops || !ops->writeStaged) {
            return Result<WidgetTemplate>::Failure(ErrorCode::TypeMismatch);
        }
        auto expected = source.Types().Acquire(field.value.type);
        if (!expected) {
            return Result<WidgetTemplate>::Failure(expected.Error());
        }
        const bool animation = expected.Value().IsNativeA(NativeTypeKey<WidgetAnimation>());
        if (!animation && !expected.Value().IsNativeA(NativeTypeKey<Widget>())) {
            return Result<WidgetTemplate>::Failure(ErrorCode::TypeMismatch);
        }
        const auto* node = animation ? clipIds.Find(binding.widget) : targets.Find(binding.widget);
        if (!node &&
            (animation ? targets.Contains(binding.widget) : clipIds.Contains(binding.widget))) {
            return Result<WidgetTemplate>::Failure(ErrorCode::TypeMismatch);
        }
        if (!node && !binding.optional) {
            return Result<WidgetTemplate>::Failure(ErrorCode::NotFound);
        }
        if (node) {
            const auto handle = animation ? animations_[*node] : nodes[*node];
            auto actual = source.AcquireType(access, handle);
            if (!actual || !actual.Value().IsA(field.value.type)) {
                return Result<WidgetTemplate>::Failure(ErrorCode::TypeMismatch);
            }
        }
        auto stored = result.bindings_.TryPushBack(
            {controller.Value().Schema().id, binding.field, node ? *node : IndexNone, animation});
        if (!stored) {
            return Result<WidgetTemplate>::Failure(stored.Error());
        }
    }
    for (const auto& binding : eventBindings_) {
        if (!events) {
            return Result<WidgetTemplate>::Failure(ErrorCode::NotReady);
        }
        const auto* node = targets.Find(binding.widget);
        if (!node) {
            return Result<WidgetTemplate>::Failure(ErrorCode::NotFound);
        }
        WidgetTemplate::EventConnection* group = nullptr;
        for (auto& prior : result.events_) {
            if (prior.nodeIndex == *node && prior.bindings[0].eventId == binding.eventId) {
                group = &prior;
                break;
            }
        }
        auto actual = source.AcquireType(access, nodes[*node]);
        if (!actual) {
            return Result<WidgetTemplate>::Failure(actual.Error());
        }
        auto valid = events->Validate(actual.Value(), controller.Value(), binding);
        if (!valid) {
            return Result<WidgetTemplate>::Failure(valid.Error());
        }
        if (!group) {
            auto added = result.events_.TryEmplaceBack(WidgetTemplate::EventConnection{
                Array<WidgetEventBinding>(allocator), actual.Value().Schema().id, *node});
            if (!added) {
                return Result<WidgetTemplate>::Failure(added.Error());
            }
            group = added.Value();
        }
        for (const auto& prior : group->bindings) {
            if (prior.methodId == binding.methodId) {
                return Result<WidgetTemplate>::Failure(ErrorCode::AlreadyExists);
            }
        }
        auto stored = group->bindings.TryPushBack(binding);
        if (!stored) {
            return Result<WidgetTemplate>::Failure(stored.Error());
        }
    }
    result.viewModelType_ = viewModelType_;
    if (!propertyBindings_.IsEmpty()) {
        auto model =
            source.Types().Acquire(viewModelType_ == TypeId{} ? controller_.Id() : viewModelType_);
        if (!model) {
            return Result<WidgetTemplate>::Failure(model.Error());
        }
        for (const auto& binding : propertyBindings_) {
            const auto* node = targets.Find(binding.widget);
            if (!node) {
                return Result<WidgetTemplate>::Failure(ErrorCode::NotFound);
            }
            for (const auto& existing : result.properties_) {
                if (existing.nodeIndex == *node &&
                    existing.binding.targetField == binding.targetField) {
                    return Result<WidgetTemplate>::Failure(ErrorCode::AlreadyExists);
                }
            }
            auto destination = source.AcquireType(access, nodes[*node]);
            if (!destination) {
                return Result<WidgetTemplate>::Failure(destination.Error());
            }
            auto valid = ValidateWidgetPropertyBinding(model.Value(), destination.Value(), binding);
            if (!valid) {
                return Result<WidgetTemplate>::Failure(valid.Error());
            }
            auto stored =
                result.properties_.TryPushBack({binding, destination.Value().Schema().id, *node});
            if (!stored) {
                return Result<WidgetTemplate>::Failure(stored.Error());
            }
        }
    }
    result.graph_ = graph.Value().TakeBytes();
    return Result<WidgetTemplate>::Success(Move(result));
}

} // namespace rocks::ui
