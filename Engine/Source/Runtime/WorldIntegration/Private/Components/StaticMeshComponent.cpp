#include <Collision/BodySettings.generated.h>
#include <Mesh/StaticMesh/StaticMeshValidation.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/ComponentValueChecks.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <WorldIntegration/Components/MeshComponent.generated.h>
#include <WorldIntegration/Components/StaticMeshComponent.generated.h>
#include <WorldIntegration/Components/StaticMeshComponent.h>

// 정적 메시 컴포넌트다(Docs/Design/EngineRuntime 30.3절, D2b). 데이터와 로드 정보 사본만 있고 메시
// 로드·렌더·충돌 연결은 30.6절이다.
namespace rocks {

static_assert(MeshComponent::MaxMaterialSlots == static_cast<int32>(StaticMeshMaxMaterialSlots),
              "MeshComponent slot limit must match the static mesh material slot limit");

Result<void> StaticMeshComponent::SetStaticMesh(const SoftObjectRef<StaticMesh>& mesh) noexcept {
    if (auto valid = mesh.Validate(); !valid) {
        return valid;
    }

    if (staticMesh_ == mesh) {
        return Result<void>::Success();
    }

    // UE SetStaticMesh(StaticMeshComponent.cpp:2370) — 게임이 시작된 Static 컴포넌트는 바꾸지
    // 않는다.
    if (auto allowed = CheckDynamicDataChange("staticMesh", false); !allowed) {
        return allowed;
    }

    if (auto assigned = staticMesh_.TryAssign(mesh); !assigned) {
        return assigned;
    }

    ClearLoadedMeshInfo();
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void> StaticMeshComponent::ApplyLoadedMeshInfo(const SoftObjectRef<StaticMesh>& requested,
                                                      const StaticMesh& mesh) noexcept {
    if (requested.IsNull() || !(requested == staticMesh_)) {
        return component_checks::Invalid();
    }

    const MeshBounds bounds = StaticMeshExtendedBounds(mesh);
    loadedMeshBounds_ = ComponentBounds{};
    loadedMeshBounds_.origin = (bounds.box.min + bounds.box.max) * 0.5;
    loadedMeshBounds_.boxExtent = (bounds.box.max - bounds.box.min) * 0.5;
    loadedMeshBounds_.sphereRadius = bounds.sphereRadius;
    loadedMeshSlotCount_ = mesh.materialSlots.Size();
    loadedMeshInfo_ = true;
    InvalidateBounds();
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

void StaticMeshComponent::ClearLoadedMeshInfo() noexcept {
    if (!loadedMeshInfo_) {
        return;
    }

    loadedMeshInfo_ = false;
    loadedMeshSlotCount_ = 0;
    loadedMeshBounds_ = ComponentBounds{};
    InvalidateBounds();
    MarkRenderStateDirtyIfPresent();
}

Result<void> StaticMeshComponent::SetForcedLodModel(int32 model) noexcept {
    if (model < 0) {
        return component_checks::Invalid();
    }

    Change(forcedLodModel_, model);
    return Result<void>::Success();
}

Result<void> StaticMeshComponent::SetMinLod(int32 lod) noexcept {
    if (lod < 0) {
        return component_checks::Invalid();
    }

    Change(minLod_, lod);
    return Result<void>::Success();
}

Result<void> StaticMeshComponent::SetWorldPositionOffsetDisableDistance(int32 distance) noexcept {
    if (distance < 0) {
        return component_checks::Invalid();
    }

    Change(worldPositionOffsetDisableDistance_, distance);
    return Result<void>::Success();
}

Result<void> StaticMeshComponent::SetNanitePixelProgrammableDistance(float distance) noexcept {
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    Change(nanitePixelProgrammableDistance_, distance);
    return Result<void>::Success();
}

Result<void> StaticMeshComponent::SetOverriddenLightMapRes(int32 resolution) noexcept {
    if (resolution < MinOverriddenLightMapRes || resolution > MaxOverriddenLightMapRes ||
        resolution % OverriddenLightMapResAlignment != 0) {
        return component_checks::Invalid();
    }

    Change(overriddenLightMapRes_, resolution);
    return Result<void>::Success();
}

Result<void>
StaticMeshComponent::SetDistanceFieldIndirectShadowMinVisibility(float visibility) noexcept {
    if (!component_checks::InRange(visibility, 0.0f, 1.0f)) {
        return component_checks::Invalid();
    }

    Change(distanceFieldIndirectShadowMinVisibility_, visibility);
    return Result<void>::Success();
}

Result<void> StaticMeshComponent::SetDistanceFieldSelfShadowBias(float bias) noexcept {
    if (!component_checks::NonNegative(bias)) {
        return component_checks::Invalid();
    }

    Change(distanceFieldSelfShadowBias_, bias);
    return Result<void>::Success();
}

Result<void> StaticMeshComponent::SetSubDivisionStepSize(int32 step) noexcept {
    if (step < MinSubDivisionStepSize || step > MaxSubDivisionStepSize) {
        return component_checks::Invalid();
    }

    Change(subDivisionStepSize_, step);
    return Result<void>::Success();
}

Result<void> StaticMeshComponent::SetStreamingDistanceMultiplier(float multiplier) noexcept {
    if (!component_checks::NonNegative(multiplier)) {
        return component_checks::Invalid();
    }

    Change(streamingDistanceMultiplier_, multiplier);
    return Result<void>::Success();
}

ComponentBounds StaticMeshComponent::CalcBounds(const Transform& world) const noexcept {
    ComponentBounds local;
    if (!TryLocalBounds(local)) {
        return MeshComponent::CalcBounds(world);
    }

    return TransformComponentBounds(local, world.ToMatrix());
}

bool StaticMeshComponent::TryLocalBounds(ComponentBounds& local) const noexcept {
    if (!loadedMeshInfo_) {
        return false;
    }

    // UE는 TransformBy 뒤 extent·반지름에 BoundsScale을 곱한다. 두 변환이 extent에 선형이므로 지역
    // 값에 먼저 곱해도 같다(30.3절).
    const double scale = static_cast<double>(boundsScale_);
    local = loadedMeshBounds_;
    local.boxExtent = local.boxExtent * scale;
    local.sphereRadius = local.sphereRadius * scale;
    return true;
}

} // namespace rocks
