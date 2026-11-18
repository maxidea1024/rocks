#include "VulkanDeviceImpl.h"

#include <Platform/Atomics.h>

// 명시 접근 상태와 전이(RenderBase 4.4절 GX5, Docs/Design/VulkanGraphics.ko.md 8절)다. 추상 접근
// 상태를 image layout과 pipeline stage·access mask로 낮춘다(UE VulkanBarriers.cpp의
// GetVkStageAndAccessFlags와 같은 대응). 한 지점 전이는 1.1 core vkCmdPipelineBarrier를 쓰고, 분리
// 전이는 VK_KHR_synchronization2 event(vkCmdSetEvent2·vkCmdWaitEvents2)를 쓴다. sync2의
// stage·access 비트는 1.1 비트와 같은 값이므로 같은 대응을 넓혀 쓴다.
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
                                 bool uavOrder, VkImageAspectFlags aspect, uint32 firstLayer,
                                 uint32 layerCount) noexcept {
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
    barrier.subresourceRange = {aspect, firstMip, mipCount, firstLayer, layerCount};
    functions_.vkCmdPipelineBarrier(buffer, source.stage, destination.stage, 0, 0, nullptr, 0,
                                    nullptr, 1, &barrier);
    return true;
}

void VulkanDevice::MipLayerBarriers(VkCommandBuffer buffer, const TextureRecord& record,
                                    const SubresourceStates<ResourceAccess>& states,
                                    uint32 mipLevel, ResourceAccess to, bool toResting) noexcept {
    uint32 layer = 0;
    while (layer < record.slices) {
        const ResourceAccess resting = states.At(mipLevel, layer);
        uint32 end = layer + 1;
        while (end < record.slices && states.At(mipLevel, end) == resting) {
            ++end;
        }

        (void)AccessBarrier(buffer, record.image, mipLevel, 1, toResting ? to : resting,
                            toResting ? resting : to, false, record.aspect, layer, end - layer);
        layer = end;
    }
}

namespace {

// 1.1 비트로 sync2 image barrier를 만든다(같은 값의 비트다).
VkImageMemoryBarrier2 ImageBarrier2(VkImage image, VkImageAspectFlags aspect, uint32 firstMip,
                                    uint32 mipCount, uint32 layer, uint32 layerCount,
                                    ResourceAccess from, ResourceAccess to) noexcept {
    const StageAccess source = StageAccessFor(from);
    const StageAccess destination = StageAccessFor(to);
    VkImageMemoryBarrier2 barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = source.stage;
    barrier.srcAccessMask = WriteAccess(source.access);
    barrier.dstStageMask = destination.stage;
    barrier.dstAccessMask = destination.access;
    barrier.oldLayout = LayoutFor(from);
    barrier.newLayout = LayoutFor(to);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, firstMip, mipCount, layer, layerCount};
    return barrier;
}

VkDependencyInfo DependencyOf(const VkMemoryBarrier2* memory, uint32 memoryCount,
                              const VkImageMemoryBarrier2* images, uint32 imageCount) noexcept {
    VkDependencyInfo dependency = {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = memoryCount;
    dependency.pMemoryBarriers = memoryCount != 0 ? memory : nullptr;
    dependency.imageMemoryBarrierCount = imageCount;
    dependency.pImageMemoryBarriers = imageCount != 0 ? images : nullptr;
    return dependency;
}

} // namespace

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

    // 패스 attachment가 전이로 attachment 상태를 떠났으면 다시 열지 않는다(그 사이 그리기는
    // 무시된다). 닫힌 표시는 남겨 attachment가 돌아오는 다음 전이에서 다시 연다.
    if (!AttachmentsReady()) {
        return;
    }

    renderPassSuspended_ = false;
    BeginRenderPass(true);
}

namespace {

// layer other가 layer와 범위의 모든 mip에서 같은 상태인지다(같은 layer 구간으로 묶을 수 있다).
bool SameLayerStates(const SubresourceStates<ResourceAccess>& states,
                     const ResolvedSubresourceRange& range, uint32 layer, uint32 other) noexcept {
    for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
        if (states.At(mip, layer) != states.At(mip, other)) {
            return false;
        }
    }

    return true;
}

} // namespace

template <class Emit>
void VulkanDevice::ForEachStateRun(const TextureRecord& record,
                                   const ResolvedSubresourceRange& range, Emit&& emit) noexcept {
    // 범위의 모든 mip에서 상태 열이 같은 이웃 layer를 한 구간으로 묶고, 그 구간 안에서 같은 상태가
    // 이어지는 mip 구간마다 emit(firstMip, mipCount, firstLayer, layerCount, from)을 부른다.
    const uint32 lastMip = range.firstMip + range.mipCount;
    const uint32 lastLayer = range.firstSlice + range.sliceCount;
    uint32 layer = range.firstSlice;
    while (layer < lastLayer) {
        uint32 endLayer = layer + 1;
        while (endLayer < lastLayer && SameLayerStates(record.current, range, layer, endLayer)) {
            ++endLayer;
        }

        uint32 mip = range.firstMip;
        while (mip < lastMip) {
            const ResourceAccess from = record.current.At(mip, layer);
            uint32 end = mip + 1;
            while (end < lastMip && record.current.At(end, layer) == from) {
                ++end;
            }

            emit(mip, end - mip, layer, endLayer - layer, from);
            mip = end;
        }

        layer = endLayer;
    }
}

void VulkanDevice::TrackTransition(TextureRecord& record, SlotHandle slot,
                                   const ResolvedSubresourceRange& range, ResourceAccess after,
                                   bool uavOrder) noexcept {
    after = CanonicalAccess(after);
    // subresource마다 상태가 다를 수 있으므로 같은 상태 열의 layer 구간 × 같은 상태의 mip 구간마다
    // barrier 하나를 낸다.
    ForEachStateRun(record, range,
                    [&](uint32 mip, uint32 mipCount, uint32 layer, uint32 layerCount,
                        ResourceAccess from) noexcept {
                        if (BarrierNeeded(from, after, uavOrder)) {
                            SuspendRenderPass();
                        }

                        (void)AccessBarrier(frame_, record.image, mip, mipCount, from, after,
                                            uavOrder, record.aspect, layer, layerCount);
                    });
    for (uint32 layer = range.firstSlice; layer < range.firstSlice + range.sliceCount; ++layer) {
        for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
            record.current.At(mip, layer) = after;
        }
    }

    MarkTouched(record, slot);
}

void VulkanDevice::MarkTouched(TextureRecord& record, SlotHandle slot) noexcept {
    if (!record.touched) {
        record.touched = true;
        if (!touchedTextures_.TryPushBack(slot)) {
            touchedOverflow_ = true;
        }
    }
}

void VulkanDevice::SettleTracking(bool executed) noexcept {
    auto settle = [executed](TextureRecord& record) noexcept {
        if (executed) {
            record.committed.CopyFrom(record.current);
        } else {
            record.current.CopyFrom(record.committed);
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
    TransitionLocked(transitions);
}

TextureRecord* VulkanDevice::ResolveTextureTransition(const ResourceTransition& transition,
                                                      SlotHandle& slot,
                                                      ResolvedSubresourceRange& range) noexcept {
    // view의 전이는 이미지 상태를 추적하는 원 텍스처 기록에 낸다.
    slot = transition.texture.slot;
    TextureRecord* record = textures_.Find(slot);
    if (record != nullptr && record->isView) {
        slot = record->source;
        record = textures_.Find(slot);
    }

    range = record == nullptr
                ? ResolvedSubresourceRange{}
                : ResolveSubresourceRange(transition.range, record->mipLevels, record->slices);
    if (range.IsEmpty()) {
        ++ignoredDraws_;
        return nullptr;
    }

    // 내용 버림은 지금 낮출 것이 없다(aliasing은 RG5·GX12).
    if (transition.after == ResourceAccess::Discard) {
        return nullptr;
    }

    // 이미지 usage에 없는 layout으로는 바꿀 수 없다. 렌더 타깃이 아닌 텍스처의 렌더 타깃 상태와
    // 깊이 타깃이 아닌 텍스처의 깊이 상태는 무시로 센다.
    if ((HasAnyAccess(transition.after, ResourceAccess::RenderTarget) && !record->renderTarget) ||
        (HasAnyAccess(transition.after, DepthAccessMask) && !record->depthTarget)) {
        ++ignoredDraws_;
        return nullptr;
    }

    if (validation_ && transition.before != ResourceAccess::Unknown &&
        transition.before != ResourceAccess::Discard) {
        bool matched = true;
        for (uint32 layer = range.firstSlice;
             matched && layer < range.firstSlice + range.sliceCount; ++layer) {
            for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
                if (record->current.At(mip, layer) != CanonicalAccess(transition.before)) {
                    matched = false;
                    break;
                }
            }
        }

        if (!matched) {
            platform::AtomicFetchAdd<platform::MemoryOrder::AcquireRelease>(
                &validationState_.errors, uint64{1});
            auto failed = FailCode("Transition", ErrorCode::InvalidArgument,
                                   "transition before-state differs from the tracked state");
            (void)failed;
        }
    }

    return record;
}

void VulkanDevice::TransitionLocked(Span<const ResourceTransition> transitions) noexcept {
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

        SlotHandle slot;
        ResolvedSubresourceRange range;
        TextureRecord* record = ResolveTextureTransition(transition, slot, range);
        if (record == nullptr) {
            continue;
        }

        const bool uav = HasAnyAccess(transition.after, UavAccessMask);
        TrackTransition(*record, slot, range, transition.after, uav);
    }

    ResumeRenderPass();
}

VkEvent VulkanDevice::AcquireSplitEvent() noexcept {
    CommandRecord& record = commands_[frameRecord_];
    if (record.eventCursor < record.eventCount) {
        return record.events[record.eventCursor++];
    }

    if (record.eventCount == MaxSplitEventsPerRecord) {
        return VK_NULL_HANDLE;
    }

    VkEventCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    info.flags = VK_EVENT_CREATE_DEVICE_ONLY_BIT;
    VkEvent event = VK_NULL_HANDLE;
    if (functions_.vkCreateEvent(device_, &info, nullptr, &event) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    record.events[record.eventCount++] = event;
    ++record.eventCursor;
    return event;
}

SplitTransition
VulkanDevice::BeginTransitions(Span<const ResourceTransition> transitions) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return {};
    }

    const SplitTransition split = splits_.Begin(transitions);
    if (split.IsNull() || !sync2_) {
        return split;
    }

    // 담을 자리를 먼저 잡는다(텍스처 전이 하나는 layer마다 mip 구간당 barrier 하나, 많아야 그
    // 텍스처의 subresource 수). 잡지 못하면 이 짝은 event 없이 end에서 Transition처럼 낮춘다
    // (pendingSplits_에 없는 짝).
    usize imageBarriers = 0;
    for (const ResourceTransition& transition : transitions) {
        const TextureRecord* texture =
            transition.texture.IsNull() ? nullptr : textures_.Find(transition.texture.slot);
        if (texture != nullptr && texture->isView) {
            texture = textures_.Find(texture->source);
        }

        imageBarriers += texture != nullptr ? usize{texture->mipLevels} * texture->slices : 0u;
    }

    if (!pendingSplits_.TryEnsureCapacity(pendingSplits_.Size() + 1) ||
        !splitMemoryBarriers_.TryEnsureCapacity(splitMemoryBarriers_.Size() + transitions.Size()) ||
        !splitImageBarriers_.TryEnsureCapacity(splitImageBarriers_.Size() + imageBarriers)) {
        return split;
    }

    PendingSplit pending;
    pending.id = split.id;
    pending.firstImage = static_cast<uint32>(splitImageBarriers_.Size());
    pending.firstMemory = static_cast<uint32>(splitMemoryBarriers_.Size());
    for (const ResourceTransition& transition : transitions) {
        if (!CanCombineAccess(transition.after)) {
            ++ignoredDraws_;
            continue;
        }

        if (!transition.buffer.IsNull()) {
            if (buffers_.Find(transition.buffer.slot) == nullptr) {
                ++ignoredDraws_;
                continue;
            }

            // LowerBufferTransition과 같은 조건이다.
            const ResourceAccess before = CanonicalAccess(transition.before);
            const ResourceAccess after = CanonicalAccess(transition.after);
            const bool unknown =
                before == ResourceAccess::Unknown || before == ResourceAccess::Discard;
            if (!unknown && !IsWritableAccess(before) && !IsWritableAccess(after) &&
                CoversStages(before, after)) {
                continue;
            }

            const StageAccess source = unknown ? StageAccess{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                             VK_ACCESS_MEMORY_WRITE_BIT}
                                               : StageAccessFor(before);
            const StageAccess destination = StageAccessFor(after);
            VkMemoryBarrier2 barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = source.stage;
            barrier.srcAccessMask = WriteAccess(source.access);
            barrier.dstStageMask = destination.stage;
            barrier.dstAccessMask = destination.access;
            (void)splitMemoryBarriers_.TryPushBack(barrier);
            continue;
        }

        SlotHandle slot;
        ResolvedSubresourceRange range;
        TextureRecord* record = ResolveTextureTransition(transition, slot, range);
        if (record == nullptr) {
            continue;
        }

        const ResourceAccess after = CanonicalAccess(transition.after);
        const bool uav = HasAnyAccess(transition.after, UavAccessMask);
        ForEachStateRun(*record, range,
                        [&](uint32 mip, uint32 mipCount, uint32 layer, uint32 layerCount,
                            ResourceAccess from) noexcept {
                            if (BarrierNeeded(from, after, uav)) {
                                (void)splitImageBarriers_.TryPushBack(
                                    ImageBarrier2(record->image, record->aspect, mip, mipCount,
                                                  layer, layerCount, from, after));
                            }
                        });

        // 이미지 layout은 begin 뒤 바뀌므로 추적 상태를 지금 바꾼다(그 사이 render pass를 다시
        // 열지 않는다, 사이 접근은 계약 위반이다).
        for (uint32 layer = range.firstSlice; layer < range.firstSlice + range.sliceCount;
             ++layer) {
            for (uint32 mip = range.firstMip; mip < range.firstMip + range.mipCount; ++mip) {
                record->current.At(mip, layer) = after;
            }
        }

        MarkTouched(*record, slot);
    }

    pending.imageCount = static_cast<uint32>(splitImageBarriers_.Size()) - pending.firstImage;
    pending.memoryCount = static_cast<uint32>(splitMemoryBarriers_.Size()) - pending.firstMemory;
    if (pending.imageCount + pending.memoryCount != 0) {
        pending.event = AcquireSplitEvent();
        if (pending.event != VK_NULL_HANDLE) {
            // vkCmdSetEvent2는 render pass 밖에서만 기록한다.
            SuspendRenderPass();
            const VkDependencyInfo dependency =
                DependencyOf(splitMemoryBarriers_.Data() + pending.firstMemory, pending.memoryCount,
                             splitImageBarriers_.Data() + pending.firstImage, pending.imageCount);
            functions_.vkCmdSetEvent2KHR(frame_, pending.event, &dependency);
            ResumeRenderPass();
        }
    }

    (void)pendingSplits_.TryPushBack(pending);
    return split;
}

void VulkanDevice::EndTransitions(SplitTransition split,
                                  Span<const ResourceTransition> transitions) noexcept {
    platform::LockGuard lock(gate_);
    if (!recording_) {
        return;
    }

    const SplitEndResult ended = splits_.End(split, transitions);
    if (ended == SplitEndResult::Unknown) {
        ++ignoredDraws_;
        TransitionLocked(transitions);
        return;
    }

    if (ended == SplitEndResult::Null || !sync2_) {
        TransitionLocked(transitions);
        return;
    }

    if (ended == SplitEndResult::Mismatch && validation_) {
        platform::AtomicFetchAdd<platform::MemoryOrder::AcquireRelease>(&validationState_.errors,
                                                                        uint64{1});
        auto failed = FailCode("EndTransitions", ErrorCode::InvalidArgument,
                               "split transition ended with a list different from its begin");
        (void)failed;
    }

    usize found = pendingSplits_.Size();
    for (usize index = 0; index < pendingSplits_.Size(); ++index) {
        if (pendingSplits_[index].id == split.id) {
            found = index;
            break;
        }
    }

    if (found == pendingSplits_.Size()) {
        TransitionLocked(transitions);
        return;
    }

    const PendingSplit pending = pendingSplits_[found];
    pendingSplits_[found] = pendingSplits_[pendingSplits_.Size() - 1];
    auto popped = pendingSplits_.TryPopBack();
    (void)popped;
    if (pending.imageCount + pending.memoryCount == 0) {
        return;
    }

    // 레이아웃 전이가 낀 대기는 render pass 밖에서 한다.
    SuspendRenderPass();
    const VkDependencyInfo dependency =
        DependencyOf(splitMemoryBarriers_.Data() + pending.firstMemory, pending.memoryCount,
                     splitImageBarriers_.Data() + pending.firstImage, pending.imageCount);
    if (pending.event != VK_NULL_HANDLE) {
        functions_.vkCmdWaitEvents2KHR(frame_, 1, &pending.event, &dependency);
    } else {
        functions_.vkCmdPipelineBarrier2KHR(frame_, &dependency);
    }

    ResumeRenderPass();
}

void VulkanDevice::CloseSplitTransitions() noexcept {
    const usize unended = splits_.Close();
    if (validation_ && unended != 0) {
        platform::AtomicFetchAdd<platform::MemoryOrder::AcquireRelease>(&validationState_.errors,
                                                                        uint64{unended});
        auto failed = FailCode("EndFrame", ErrorCode::InvalidArgument,
                               "split transitions were begun but not ended before EndFrame");
        (void)failed;
    }

    // 이 기록이 신호한 event를 기록 끝에서 되돌린다(UE FVulkanCommandBuffer::End와 같다). 다음
    // 사용은 이 제출의 fence 뒤다(ResetRecordPools).
    if (sync2_) {
        const CommandRecord& record = commands_[frameRecord_];
        for (uint32 index = 0; index < record.eventCursor; ++index) {
            functions_.vkCmdResetEvent2KHR(frame_, record.events[index],
                                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }
    }

    pendingSplits_.Clear();
    splitImageBarriers_.Clear();
    splitMemoryBarriers_.Clear();
}

} // namespace rocks::graphics::vulkan
