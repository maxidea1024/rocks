#include "VulkanDeviceImpl.h"

#include <Platform/Atomics.h>

// 명시 접근 상태와 전이(RenderBase 4.4절 GX5, Docs/Design/VulkanGraphics.ko.md 8절)다. 추상 접근
// 상태를 image layout과 pipeline stage·access mask로 낮춘다(UE VulkanBarriers.cpp의
// GetVkStageAndAccessFlags와 같은 대응). 1.1 core vkCmdPipelineBarrier를 쓴다(sync2는 1.3).
namespace rocks::graphics::vulkan {

namespace {

struct StageAccess {
    VkPipelineStageFlags stage = 0;
    VkAccessFlags access = 0;
};

constexpr VkPipelineStageFlags GraphicsShaderStages =
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
constexpr VkPipelineStageFlags DepthStages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

} // namespace

ResourceAccess CanonicalAccess(ResourceAccess access) noexcept {
    if (HasAnyAccess(access, ResourceAccess::SrvGraphics)) {
        access = access | ResourceAccess::SrvCompute;
    }

    if (HasAnyAccess(access, ResourceAccess::VertexOrIndex)) {
        access = access | ResourceAccess::Constant;
    }

    if (HasAnyAccess(access, UavAccessMask)) {
        access = access | UavAccessMask;
    }

    return access;
}

namespace {

// 접근 상태의 image layout이다. 셰이더 읽기만이면 SHADER_READ_ONLY, 복사 원본만이면 TRANSFER_SRC,
// 읽기가 섞이거나 UAV면 GENERAL이다. Unknown·Discard는 내용이 필요 없으므로 UNDEFINED다.
VkImageLayout LayoutFor(ResourceAccess access) noexcept {
    if (access == ResourceAccess::Unknown || access == ResourceAccess::Discard) {
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if (HasAnyAccess(access, UavAccessMask)) {
        return VK_IMAGE_LAYOUT_GENERAL;
    }

    if (access == ResourceAccess::RenderTarget) {
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    if (access == ResourceAccess::CopyDestination) {
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }

    if (access == ResourceAccess::CopySource) {
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    if (access == ResourceAccess::Present) {
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

    if (HasAnyAccess(access, ResourceAccess::DepthWrite)) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    if (HasAnyAccess(access, ResourceAccess::DepthRead)) {
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }

    if ((access & ~SrvAccessMask) == ResourceAccess::Unknown) {
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    return VK_IMAGE_LAYOUT_GENERAL;
}

// 접근 상태가 쓰는 pipeline 단계와 access mask다.
StageAccess StageAccessFor(ResourceAccess access) noexcept {
    StageAccess result;
    auto add = [&](ResourceAccess bit, VkPipelineStageFlags stage, VkAccessFlags flags) noexcept {
        if (HasAnyAccess(access, bit)) {
            result.stage |= stage;
            result.access |= flags;
        }
    };
    add(ResourceAccess::VertexOrIndex, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT);
    add(ResourceAccess::Constant, GraphicsShaderStages | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_UNIFORM_READ_BIT);
    add(ResourceAccess::IndirectArgs, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    add(ResourceAccess::SrvGraphics, GraphicsShaderStages, VK_ACCESS_SHADER_READ_BIT);
    add(ResourceAccess::SrvCompute, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    add(ResourceAccess::CopySource, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    add(ResourceAccess::Present, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    add(ResourceAccess::DepthRead, DepthStages, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    add(ResourceAccess::UavGraphics, GraphicsShaderStages,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    add(ResourceAccess::UavCompute, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    add(ResourceAccess::RenderTarget, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    add(ResourceAccess::DepthWrite, DepthStages,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    add(ResourceAccess::CopyDestination, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    if (result.stage == 0) {
        result.stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }

    return result;
}

// 앞 상태에서 다음 상태를 기다릴 때 앞쪽 access mask다. 읽기는 보일 쓰기가 없으므로 실행
// 의존만 남긴다(WAR).
VkAccessFlags WriteAccess(VkAccessFlags access) noexcept {
    return access & (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                     VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}

// 앞 상태의 단계가 다음 상태의 단계를 모두 덮는지다. 덮지 않으면 같은 layout의 읽기 → 읽기라도
// 새 단계에 앞 쓰기가 보이도록 실행 의존을 이어야 한다(barrier 사슬: srcAccess 0, dstAccess는 새
// 읽기).
bool CoversStages(ResourceAccess from, ResourceAccess to) noexcept {
    const VkPipelineStageFlags fromStages = StageAccessFor(from).stage;
    return (StageAccessFor(to).stage & ~fromStages) == 0;
}

// from → to에 barrier가 필요한지다.
// - 같은 상태: UAV는 생략 표시가 없을 때(uavOrder)만 쓰기 순서를 둔다. 복사 대상처럼 attachment가
//   아닌 쓰기 상태는 앞 쓰기와의 WAW 순서를 둔다. 렌더 타깃·깊이 쓰기는 render pass의 바깥 의존이
//   맡는다(RenderPassFor).
// - 다른 상태: layout이 바뀌거나, 쓰기가 끼거나, 새 단계가 생기면 둔다. 같은 layout에서 단계가
//   줄어드는 읽기 → 읽기만 의존이 없다.
bool BarrierNeeded(ResourceAccess from, ResourceAccess to, bool uavOrder) noexcept {
    if (from == to) {
        if (HasAnyAccess(to, UavAccessMask)) {
            return uavOrder;
        }

        return IsWritableAccess(to) && !HasAnyAccess(to, ResourceAccess::RenderTarget) &&
               !HasAnyAccess(to, ResourceAccess::DepthWrite);
    }

    return LayoutFor(from) != LayoutFor(to) || IsWritableAccess(from) || IsWritableAccess(to) ||
           !CoversStages(from, to);
}

} // namespace

bool VulkanDevice::AccessBarrier(VkCommandBuffer buffer, VkImage image, uint32 firstMip,
                                 uint32 mipCount, ResourceAccess from, ResourceAccess to,
                                 bool uavOrder) noexcept {
    if (!BarrierNeeded(from, to, uavOrder)) {
        return false;
    }

    const StageAccess source = StageAccessFor(from);
    const StageAccess destination = StageAccessFor(to);
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = WriteAccess(source.access);
    barrier.dstAccessMask = destination.access;
    barrier.oldLayout = LayoutFor(from);
    barrier.newLayout = LayoutFor(to);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, firstMip, mipCount, 0, 1};
    functions_.vkCmdPipelineBarrier(buffer, source.stage, destination.stage, 0, 0, nullptr, 0,
                                    nullptr, 1, &barrier);
    return true;
}

void VulkanDevice::SuspendRenderPass() noexcept {
    if (insideRenderPass_) {
        functions_.vkCmdEndRenderPass(frame_);
        insideRenderPass_ = false;
        renderPassSuspended_ = true;
    }
}

void VulkanDevice::ResumeRenderPass() noexcept {
    if (!renderPassSuspended_) {
        return;
    }

    // 패스 대상 자신이 전이로 렌더 타깃 상태를 떠났으면 다시 열지 않는다(그 사이 그리기는
    // 무시된다). 닫힌 표시는 남겨 대상이 렌더 타깃으로 돌아오는 전이에서 다시 연다.
    const TextureRecord* target = textures_.Find(target_.texture.slot);
    if (target != nullptr && target->current[0] != ResourceAccess::RenderTarget) {
        return;
    }

    renderPassSuspended_ = false;
    BeginRenderPass(true, nullptr);
}

void VulkanDevice::TrackTransition(TextureRecord& record, SlotHandle slot, uint32 first,
                                   uint32 count, ResourceAccess after, bool uavOrder) noexcept {
    after = CanonicalAccess(after);
    // mip마다 상태가 다를 수 있으므로 같은 상태가 이어지는 구간씩 barrier를 낸다.
    uint32 mip = first;
    while (mip < first + count) {
        const ResourceAccess from = record.current[mip];
        uint32 end = mip + 1;
        while (end < first + count && record.current[end] == from) {
            ++end;
        }

        if (BarrierNeeded(from, after, uavOrder)) {
            SuspendRenderPass();
        }

        (void)AccessBarrier(frame_, record.image, mip, end - mip, from, after, uavOrder);
        mip = end;
    }

    for (uint32 index = first; index < first + count; ++index) {
        record.current[index] = after;
    }

    if (!record.touched) {
        record.touched = true;
        if (!touchedTextures_.TryPushBack(slot)) {
            touchedOverflow_ = true;
        }
    }
}

void VulkanDevice::SettleTracking(bool executed) noexcept {
    auto settle = [executed](TextureRecord& record) noexcept {
        for (uint32 mip = 0; mip < record.mipLevels; ++mip) {
            if (executed) {
                record.committed[mip] = record.current[mip];
            } else {
                record.current[mip] = record.committed[mip];
            }
        }

        record.touched = false;
    };
    if (touchedOverflow_) {
        for (TextureRecord& record : textures_) {
            if (record.touched) {
                settle(record);
            }
        }
    } else {
        for (const SlotHandle slot : touchedTextures_) {
            TextureRecord* record = textures_.Find(slot);
            if (record != nullptr) {
                settle(*record);
            }
        }
    }

    touchedTextures_.Clear();
    touchedOverflow_ = false;
    renderPassSuspended_ = false;
}

// 버퍼에는 layout이 없다. 버퍼 쓰기 명령(GX4) 전에는 업로드 묶음의 barrier가 버퍼 내용을
// 맞추므로, 여기서는 쓰기가 끼거나 새 단계가 생기는 전이만 메모리 barrier로 낮춘다(앞 상태를
// 모르거나 내용 버림이면 모든 쓰기를 기다린다. UE도 Discard를 Unknown처럼 다룬다). 단계가 줄어드는
// 읽기 → 읽기는 아무것도 하지 않는다.
void VulkanDevice::LowerBufferTransition(const ResourceTransition& transition) noexcept {
    if (buffers_.Find(transition.buffer.slot) == nullptr) {
        ++ignoredDraws_;
        return;
    }

    const ResourceAccess before = CanonicalAccess(transition.before);
    const ResourceAccess after = CanonicalAccess(transition.after);
    const bool unknown = before == ResourceAccess::Unknown || before == ResourceAccess::Discard;
    if (!unknown && !IsWritableAccess(before) && !IsWritableAccess(after) &&
        CoversStages(before, after)) {
        return;
    }

    const StageAccess source =
        unknown ? StageAccess{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT}
                : StageAccessFor(before);
    const StageAccess destination = StageAccessFor(after);
    SuspendRenderPass();
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = WriteAccess(source.access);
    barrier.dstAccessMask = destination.access;
    functions_.vkCmdPipelineBarrier(frame_, source.stage, destination.stage, 0, 1, &barrier, 0,
                                    nullptr, 0, nullptr);
}

// 명시 전이를 pipeline barrier로 낮춘다(GX5, Device.h Transition 주석). render pass 안에서는
// layout 전이를 할 수 없으므로 barrier가 필요하면 render pass를 닫고 전이한 뒤 LOAD로 다시 연다.
void VulkanDevice::Transition(Span<const ResourceTransition> transitions) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return;
    }

    for (const ResourceTransition& transition : transitions) {
        if (!CanCombineAccess(transition.after)) {
            ++ignoredDraws_;
            continue;
        }

        if (!transition.buffer.IsNull()) {
            LowerBufferTransition(transition);
            continue;
        }

        TextureRecord* record = textures_.Find(transition.texture.slot);
        if (record == nullptr || transition.range.firstMip >= record->mipLevels) {
            ++ignoredDraws_;
            continue;
        }

        // 내용 버림은 지금 낮출 것이 없다(aliasing은 RG5·GX12).
        if (transition.after == ResourceAccess::Discard) {
            continue;
        }

        // 이미지 usage에 없는 layout으로는 바꿀 수 없다. 렌더 타깃이 아닌 텍스처의 렌더 타깃 상태와
        // 깊이 상태(깊이 텍스처는 GX4)는 무시로 센다.
        if ((HasAnyAccess(transition.after, ResourceAccess::RenderTarget) &&
             !record->renderTarget) ||
            HasAnyAccess(transition.after, DepthAccessMask)) {
            ++ignoredDraws_;
            continue;
        }

        const uint32 first = transition.range.firstMip;
        const uint32 available = record->mipLevels - first;
        const uint32 count =
            transition.range.mipCount < available ? transition.range.mipCount : available;
        if (validation_ && transition.before != ResourceAccess::Unknown &&
            transition.before != ResourceAccess::Discard) {
            for (uint32 mip = first; mip < first + count; ++mip) {
                if (record->current[mip] != CanonicalAccess(transition.before)) {
                    platform::AtomicFetchAdd<platform::MemoryOrder::AcquireRelease>(
                        &validationState_.errors, uint64{1});
                    auto failed =
                        FailCode("Transition", ErrorCode::InvalidArgument,
                                 "transition before-state differs from the tracked state");
                    (void)failed;
                    break;
                }
            }
        }

        const bool uav = HasAnyAccess(transition.after, UavAccessMask);
        TrackTransition(*record, transition.texture.slot, first, count, transition.after, uav);
    }

    ResumeRenderPass();
}

} // namespace rocks::graphics::vulkan
