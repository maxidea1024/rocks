#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/PostProcess/PostProcessBlending.h>
#include <World/PostProcess/PostProcessComponent.generated.h>
#include <World/PostProcess/PostProcessComponent.h>
#include <World/PostProcess/PostProcessSettings.generated.h>
#include <World/PostProcess/PostProcessVolumeBlending.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

// 후처리 볼륨 모으기·정렬·가중 혼합이다(Docs/Design/EngineRuntime 28.4절).
namespace rocks {

namespace {

// 0으로 나누기를 피하는 최소 혼합 거리(cm)다(UE DoPostProcessVolume "BlendRadius >= 1").
constexpr float PostProcessMinimumBlendRadius = 1.0f;
// Actor 하나의 컴포넌트를 한 번에 읽는 칸 수다(넘으면 Array로 다시 읽는다).
constexpr usize PostProcessComponentBatch = 16;

struct PostProcessVolumeEntry {
    ObjectHandle handle;
    PostProcessVolumeProperties properties;
};

// UE VolumeGuid 대신 handle(slot, generation) 순이다(28.4절 ERD56).
bool HandleBefore(ObjectHandle first, ObjectHandle second) noexcept {
    if (first.slot != second.slot) {
        return first.slot < second.slot;
    }

    return first.generation < second.generation;
}

bool EntryBefore(const PostProcessVolumeEntry& first,
                 const PostProcessVolumeEntry& second) noexcept {
    if (PostProcessVolumeLess(first.properties, second.properties)) {
        return true;
    }

    if (PostProcessVolumeLess(second.properties, first.properties)) {
        return false;
    }

    return HandleBefore(first.handle, second.handle);
}

bool IsRegisteredState(World& world, ObjectHandle component) noexcept {
    auto state = world.GetComponentState(component);
    return state && (state.Value() == ComponentState::Initialized ||
                     state.Value() == ComponentState::Playing);
}

// component가 PostProcessComponent면 정렬 자리에 넣는다(UE InsertPostProcessVolumeEntry 삽입 정렬).
Result<void> InsertVolume(World& world, ObjectHandle component,
                          Array<PostProcessVolumeEntry>& entries) noexcept {
    if (!IsRegisteredState(world, component)) {
        return Result<void>::Success();
    }

    auto properties = world.With<PostProcessComponent>(
        component, [](PostProcessComponent& volume) noexcept { return volume.VolumeProperties(); });
    if (!properties) {
        return Result<void>::Success();
    }

    const PostProcessVolumeEntry entry{component, properties.Value()};
    usize index = 0;
    while (index < entries.Size() && !EntryBefore(entry, entries[index])) {
        ++index;
    }

    return entries.TryInsert(entry, index);
}

} // namespace

bool PostProcessVolumeLess(const PostProcessVolumeProperties& first,
                           const PostProcessVolumeProperties& second) noexcept {
    if (first.priority != second.priority) {
        return first.priority < second.priority;
    }

    if (first.unbound != second.unbound) {
        // 경계 없는 볼륨이 먼저다.
        return first.unbound;
    }

    // 큰 볼륨이 먼저다.
    return first.size > second.size;
}

float PostProcessVolumeWeight(const PostProcessVolumeProperties& properties,
                              const PostProcessEncompass& encompass) noexcept {
    if (!properties.enabled) {
        return 0.0f;
    }

    float weight = math::Clamp(properties.blendWeight, 0.0f, 1.0f);
    if (properties.unbound) {
        return weight;
    }

    const float distance = static_cast<float>(encompass.distance);
    if (!(distance >= 0.0f) || distance > properties.blendRadius) {
        return 0.0f;
    }

    if (properties.blendRadius >= PostProcessMinimumBlendRadius) {
        weight *= 1.0f - distance / properties.blendRadius;
        if (!(weight >= 0.0f && weight <= 1.0f)) {
            return 0.0f;
        }
    }

    return weight;
}

Result<uint32> BlendPostProcessVolumes(World& world, Allocator& scratch, const Vec3d& viewLocation,
                                       PostProcessSettings& settings) noexcept {
    if (!viewLocation.IsFinite()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    Array<PostProcessVolumeEntry> entries(scratch);
    Array<ObjectHandle> overflow(scratch);
    ErrorCode failure = ErrorCode::None;
    world.ForEachActor([&](ObjectHandle actor) noexcept {
        if (failure != ErrorCode::None) {
            return;
        }

        ObjectHandle batch[PostProcessComponentBatch];
        auto count =
            world.GetComponents(actor, Span<ObjectHandle>(batch, PostProcessComponentBatch));
        if (!count) {
            return;
        }

        const ObjectHandle* components = batch;
        usize componentCount = count.Value();
        if (componentCount > PostProcessComponentBatch) {
            if (auto resized = overflow.TryResize(componentCount); !resized) {
                failure = resized.Error();
                return;
            }

            auto again = world.GetComponents(actor, overflow.View());
            if (!again) {
                return;
            }

            components = overflow.Data();
            componentCount = again.Value() < overflow.Size() ? again.Value() : overflow.Size();
        }

        for (usize index = 0; index < componentCount; ++index) {
            if (auto inserted = InsertVolume(world, components[index], entries); !inserted) {
                failure = inserted.Error();
                return;
            }
        }
    });

    if (failure != ErrorCode::None) {
        return Result<uint32>::Failure(failure);
    }

    uint32 applied = 0;
    for (usize index = 0; index < entries.Size(); ++index) {
        const PostProcessVolumeEntry& entry = entries[index];
        (void)world.With<PostProcessComponent>(
            entry.handle, [&](PostProcessComponent& volume) noexcept {
                // UE DoPostProcessVolume: bounded면 sphereRadius 0으로 거리를 잰다.
                const PostProcessEncompass encompass = entry.properties.unbound
                                                           ? PostProcessEncompass{true, 0.0}
                                                           : volume.EncompassesPoint(viewLocation);
                const float weight = PostProcessVolumeWeight(entry.properties, encompass);
                if (weight > 0.0f) {
                    OverridePostProcessSettings(settings, volume.Settings(), weight);
                    ++applied;
                }
            });
    }

    return Result<uint32>::Success(applied);
}

} // namespace rocks
