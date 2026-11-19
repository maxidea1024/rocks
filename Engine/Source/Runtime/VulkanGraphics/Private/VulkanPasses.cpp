#include "VulkanDeviceImpl.h"
#include "VulkanFormats.h"

// 패스 attachment(BeginPass, RenderBase 4.4절 GX4)다: MRT·깊이 attachment의 render pass 열쇠, 시작
// 전이, framebuffer 캐시, 닫았다 다시 여는 render pass(Docs/Design/VulkanGraphics.ko.md 8절).
namespace rocks::graphics::vulkan {

namespace {

VkAttachmentLoadOp LoadOf(LoadAction action) noexcept {
    switch (action) {
    case LoadAction::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadAction::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadAction::DontCare:
        break;
    }

    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp StoreOf(StoreAction action) noexcept {
    return action == StoreAction::Store ? VK_ATTACHMENT_STORE_OP_STORE
                                        : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

// clear 값은 형식의 셰이더 값 계열로 쓴다(정수 형식은 D3D처럼 실수를 정수로 바꾼 값).
VkClearColorValue ClearValueOf(PixelFormat format, const ClearColor& clear) noexcept {
    const float values[4] = {clear.r, clear.g, clear.b, clear.a};
    VkClearColorValue value = {};
    const PixelShaderType type = GetPixelFormatInfo(format).shaderType;
    for (uint32 index = 0; index < 4; ++index) {
        if (type == PixelShaderType::Uint) {
            value.uint32[index] = static_cast<uint32>(values[index]);
        } else if (type == PixelShaderType::Int) {
            value.int32[index] = static_cast<int32>(values[index]);
        } else {
            value.float32[index] = values[index];
        }
    }

    return value;
}

} // namespace

Result<VkFramebuffer> VulkanDevice::FramebufferFor(VkRenderPass renderPass,
                                                   const VkImageView* views, uint32 count,
                                                   uint32 width, uint32 height) noexcept {
    for (const FramebufferEntry& entry : framebuffers_) {
        bool same = entry.count == count && entry.width == width && entry.height == height;
        for (uint32 index = 0; same && index < count; ++index) {
            same = entry.views[index] == views[index];
        }

        if (same) {
            return Result<VkFramebuffer>::Success(entry.framebuffer);
        }
    }

    // framebuffer는 형식이 같은(호환) render pass면 load·store·layout이 달라도 함께 쓴다.
    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<VkFramebuffer>::Failure(reserved.Error());
    }

    FramebufferEntry entry;
    entry.count = count;
    entry.width = width;
    entry.height = height;
    for (uint32 index = 0; index < count; ++index) {
        entry.views[index] = views[index];
    }

    VkFramebufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = renderPass;
    info.attachmentCount = count;
    info.pAttachments = views;
    info.width = width;
    info.height = height;
    info.layers = 1;
    const VkResult result =
        functions_.vkCreateFramebuffer(device_, &info, nullptr, &entry.framebuffer);
    if (result != VK_SUCCESS) {
        return FailWith<VkFramebuffer>("vkCreateFramebuffer", result);
    }

    auto pushed = framebuffers_.TryPushBack(entry);
    if (!pushed) {
        functions_.vkDestroyFramebuffer(device_, entry.framebuffer, nullptr);
        return Result<VkFramebuffer>::Failure(pushed.Error());
    }

    return Result<VkFramebuffer>::Success(entry.framebuffer);
}

void VulkanDevice::RetireFramebuffers(VkImageView view) noexcept {
    if (view == VK_NULL_HANDLE) {
        return;
    }

    usize index = 0;
    while (index < framebuffers_.Size()) {
        const FramebufferEntry& entry = framebuffers_[index];
        bool uses = false;
        for (uint32 attachment = 0; attachment < entry.count; ++attachment) {
            uses = uses || entry.views[attachment] == view;
        }

        if (!uses) {
            ++index;
            continue;
        }

        Retired retired;
        retired.framebuffer = entry.framebuffer;
        RetireEntry(Move(retired));
        auto removed = framebuffers_.TryRemoveAtSwap(index);
        (void)removed;
    }
}

Result<VkImageView> VulkanDevice::AttachmentViewFor(const TextureRecord& record, uint32 mipLevel,
                                                    uint32 slice) noexcept {
    // mip 하나인 2D 텍스처는 기존 view다(attachment view가 따로 있으면 그것). framebuffer
    // attachment view는 mip 하나여야 한다(VUID-VkFramebufferCreateInfo-pAttachments-00883).
    if (record.dimension == TextureDimension::Texture2D && record.mipLevels == 1) {
        return Result<VkImageView>::Success(
            record.attachmentView != VK_NULL_HANDLE ? record.attachmentView : record.view);
    }

    for (const AttachmentViewEntry& entry : attachmentViews_) {
        if (entry.image == record.image && entry.mipLevel == mipLevel && entry.layer == slice) {
            return Result<VkImageView>::Success(entry.view);
        }
    }

    auto reserved = attachmentViews_.TryReserve(attachmentViews_.Size() + 1);
    if (!reserved) {
        return Result<VkImageView>::Failure(reserved.Error());
    }

    // 성분 대응은 항등, aspect는 이미지 전체(깊이·스텐실)다.
    VkImageViewCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = record.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = TextureFormat(record.format).format;
    info.subresourceRange = {record.aspect, mipLevel, 1, slice, 1};
    AttachmentViewEntry entry;
    entry.image = record.image;
    entry.mipLevel = mipLevel;
    entry.layer = slice;
    const VkResult result = functions_.vkCreateImageView(device_, &info, nullptr, &entry.view);
    if (result != VK_SUCCESS) {
        return Result<VkImageView>::Failure(
            FailWith<VkImageView>("vkCreateImageView (attachment slice)", result).Error());
    }

    auto pushed = attachmentViews_.TryPushBack(entry);
    (void)pushed;
    return Result<VkImageView>::Success(entry.view);
}

void VulkanDevice::RetireAttachmentViews(VkImage image) noexcept {
    usize index = 0;
    while (index < attachmentViews_.Size()) {
        if (attachmentViews_[index].image != image) {
            ++index;
            continue;
        }

        RetireFramebuffers(attachmentViews_[index].view);
        Retired retired;
        retired.view = attachmentViews_[index].view;
        RetireEntry(Move(retired));
        auto removed = attachmentViews_.TryRemoveAtSwap(index);
        (void)removed;
    }
}

bool VulkanDevice::OverlapsPassAttachment(SlotHandle slot,
                                          const ResolvedSubresourceRange& range) const noexcept {
    const auto overlaps = [&](SlotHandle attachment, uint32 mip, uint32 slice) noexcept {
        if (attachment != slot) {
            return false;
        }

        const TextureRecord* record = textures_.Find(slot);
        const ResolvedSubresourceRange used = AttachmentSubresource(
            record != nullptr ? record->dimension : TextureDimension::Texture2D, mip, slice);
        return used.firstMip >= range.firstMip && used.firstMip < range.firstMip + range.mipCount &&
               used.firstSlice >= range.firstSlice &&
               used.firstSlice < range.firstSlice + range.sliceCount;
    };
    for (uint32 index = 0; index < target_.colorCount; ++index) {
        if (overlaps(target_.colors[index].slot, target_.colorMips[index],
                     target_.colorSlices[index])) {
            return true;
        }
    }

    return !target_.depth.IsNull() &&
           overlaps(target_.depth.slot, target_.depthMip, target_.depthSlice);
}

bool VulkanDevice::IsPassAttachment(SlotHandle slot) const noexcept {
    for (uint32 index = 0; index < target_.colorCount; ++index) {
        if (target_.colors[index].slot == slot) {
            return true;
        }
    }

    return !target_.depth.IsNull() && target_.depth.slot == slot;
}

bool VulkanDevice::AttachmentsReady() const noexcept {
    for (uint32 index = 0; index < target_.colorCount; ++index) {
        const TextureRecord* color = textures_.Find(target_.colors[index].slot);
        if (color == nullptr) {
            continue;
        }

        const ResolvedSubresourceRange used = AttachmentSubresource(
            color->dimension, target_.colorMips[index], target_.colorSlices[index]);
        if (color->current.At(used.firstMip, used.firstSlice) != ResourceAccess::RenderTarget) {
            return false;
        }
    }

    const TextureRecord* depth = textures_.Find(target_.depth.slot);
    const ResourceAccess expected =
        target_.depthReadOnly ? ResourceAccess::DepthRead : ResourceAccess::DepthWrite;
    if (depth == nullptr) {
        return true;
    }

    const ResolvedSubresourceRange used =
        AttachmentSubresource(depth->dimension, target_.depthMip, target_.depthSlice);
    return depth->current.At(used.firstMip, used.firstSlice) == expected;
}

void VulkanDevice::BeginRenderPass(bool resume) noexcept {
    if (target_.framebuffer == VK_NULL_HANDLE) {
        return;
    }

    // 다시 열 때는 모든 attachment를 LOAD로 연다(앞 render pass의 결과를 잇는다).
    RenderPassKey key = target_.key;
    if (resume) {
        for (uint32 index = 0; index < key.colorCount; ++index) {
            key.colorLoad[index] = VK_ATTACHMENT_LOAD_OP_LOAD;
        }

        key.depthLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (key.stencilStore == VK_ATTACHMENT_STORE_OP_STORE) {
            key.stencilLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
    }

    auto renderPass = RenderPassFor(key);
    if (!renderPass) {
        return;
    }

    VkRenderPassBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = renderPass.Value();
    begin.framebuffer = target_.framebuffer;
    begin.renderArea.extent = {target_.width, target_.height};
    begin.clearValueCount = key.colorCount + (key.depth != VK_FORMAT_UNDEFINED ? 1u : 0u);
    begin.pClearValues = target_.clears;
    functions_.vkCmdBeginRenderPass(frame_, &begin, VK_SUBPASS_CONTENTS_INLINE);
    insideRenderPass_ = true;
}

void VulkanDevice::StartPass() noexcept {
    // attachment 전이다(GX5). offscreen attachment는 추적 상태에서 렌더 타깃·깊이 상태로 바꾼다.
    // swapchain 중간 이미지는 앞 프레임 blit(TRANSFER 읽기) 뒤일 수 있다(WAR).
    if (!presentTarget_.IsNull()) {
        Barrier(frame_, target_.image, target_.restLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                0, VK_REMAINING_MIP_LEVELS, VK_PIPELINE_STAGE_TRANSFER_BIT);
    } else {
        // attachment의 subresource 하나만 바꾼다(텍스처 GX2 C부).
        for (uint32 index = 0; index < target_.colorCount; ++index) {
            TextureRecord* color = textures_.Find(target_.colors[index].slot);
            TrackTransition(*color, target_.colors[index].slot,
                            AttachmentSubresource(color->dimension, target_.colorMips[index],
                                                  target_.colorSlices[index]),
                            ResourceAccess::RenderTarget, false);
        }

        TextureRecord* depth = textures_.Find(target_.depth.slot);
        if (depth != nullptr) {
            TrackTransition(
                *depth, target_.depth.slot,
                AttachmentSubresource(depth->dimension, target_.depthMip, target_.depthSlice),
                target_.depthReadOnly ? ResourceAccess::DepthRead : ResourceAccess::DepthWrite,
                false);
        }
    }

    renderPassSuspended_ = false;
    BeginRenderPass(false);
    if (target_.width != 0) {
        // NDC y 위를 화면 위로 둔다(음수 viewport 높이, Vulkan 1.1 core, ShaderSystem 7.2절 8항).
        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = static_cast<float>(target_.height);
        viewport.width = static_cast<float>(target_.width);
        viewport.height = -static_cast<float>(target_.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        functions_.vkCmdSetViewport(frame_, 0, 1, &viewport);
        VkRect2D scissor = {};
        scissor.extent = {target_.width, target_.height};
        functions_.vkCmdSetScissor(frame_, 0, 1, &scissor);
    }

    functions_.vkCmdSetStencilReference(frame_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    // 그리기별 상수는 기록마다 0으로 시작한다(CommandContext::SetDrawConstants).
    const uint8 zeros[ConstantBytes] = {};
    functions_.vkCmdPushConstants(frame_, pipelineLayout_, ConstantStages, 0, ConstantBytes, zeros);
}

Result<CommandContext*> VulkanDevice::BeginPass(const PassDesc& pass) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    platform::LockGuard lock(gate_);
    const auto fail = [this](ErrorCode code, const char* detail) noexcept {
        auto failed = FailCode("BeginPass", code, detail);
        return Result<CommandContext*>::Failure(failed.Error());
    };
    if (pass.colorCount > MaxColorTargets) {
        return fail(ErrorCode::InvalidArgument,
                    "pass has more color attachments than MaxColorTargets");
    }

    if (pass.colorCount > caps_.limits.colorAttachments) {
        return fail(ErrorCode::Unsupported,
                    "pass has more color attachments than the device supports");
    }

    // attachment 확인: 살아 있는 텍스처, view 아님, 용도, 같은 크기·표본 수, 중복 없음.
    Target target;
    bool sized = false;
    const auto check = [&](TextureHandle handle, bool depth, uint32 mip,
                           uint32 slice) noexcept -> ErrorCode {
        const TextureRecord* record = textures_.Find(handle.slot);
        if (record == nullptr) {
            return ErrorCode::StaleHandle;
        }

        // 크기는 attachment mip의 크기다(텍스처 GX2 C부).
        if (record->isView || (depth ? !record->depthTarget : !record->renderTarget) ||
            !IsAttachmentInRange(record->dimension, record->depth, record->mipLevels,
                                 record->slices, mip, slice)) {
            return ErrorCode::InvalidArgument;
        }

        const uint32 mipWidth = MipExtent(record->width, mip);
        const uint32 mipHeight = MipExtent(record->height, mip);
        if (sized && (mipWidth != target.width || mipHeight != target.height ||
                      record->sampleCount != target.sampleCount)) {
            return ErrorCode::InvalidArgument;
        }

        target.width = mipWidth;
        target.height = mipHeight;
        target.sampleCount = record->sampleCount;
        sized = true;
        return ErrorCode::None;
    };
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const ErrorCode checked = check(pass.colors[index].texture, false,
                                        pass.colors[index].mipLevel, pass.colors[index].slice);
        if (checked != ErrorCode::None) {
            return fail(checked, "color attachment is stale, not a render target, a view or of a "
                                 "different size");
        }

        for (uint32 other = 0; other < index; ++other) {
            if (pass.colors[other].texture == pass.colors[index].texture &&
                pass.colors[other].mipLevel == pass.colors[index].mipLevel &&
                pass.colors[other].slice == pass.colors[index].slice) {
                return fail(ErrorCode::InvalidArgument,
                            "color attachment appears twice in the pass");
            }
        }
    }

    const bool hasDepth = !pass.depth.texture.IsNull();
    if (hasDepth) {
        const ErrorCode checked =
            check(pass.depth.texture, true, pass.depth.mipLevel, pass.depth.slice);
        if (checked != ErrorCode::None) {
            return fail(checked, "depth attachment is stale, not a depth target, a view or of a "
                                 "different size");
        }
    }

    // resolve 대상: MSAA attachment의 1표본 RenderTarget, 같은 형식·크기, view 아님, 겹침 없음.
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const TextureHandle resolve = pass.colors[index].resolveTarget;
        if (resolve.IsNull()) {
            continue;
        }

        const TextureRecord* source = textures_.Find(pass.colors[index].texture.slot);
        const TextureRecord* resolved = textures_.Find(resolve.slot);
        if (resolved == nullptr) {
            return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
        }

        bool overlaps = resolve == pass.depth.texture;
        for (uint32 other = 0; other < pass.colorCount; ++other) {
            overlaps = overlaps || pass.colors[other].texture == resolve ||
                       (other != index && pass.colors[other].resolveTarget == resolve);
        }

        // 대상은 그 mip·slice가 원본과 같은 크기다. 3D 대상(깊이 면 resolve)은 받지 않는다.
        const uint32 resolveMip = pass.colors[index].resolveMipLevel;
        const uint32 resolveSlice = pass.colors[index].resolveSlice;
        if (source->sampleCount < 2 || resolved->isView || !resolved->renderTarget ||
            resolved->sampleCount != 1 || resolved->format != source->format ||
            resolved->dimension == TextureDimension::Texture3D ||
            !IsAttachmentInRange(resolved->dimension, resolved->depth, resolved->mipLevels,
                                 resolved->slices, resolveMip, resolveSlice) ||
            MipExtent(resolved->width, resolveMip) != source->width ||
            MipExtent(resolved->height, resolveMip) != source->height || overlaps) {
            return fail(ErrorCode::InvalidArgument,
                        "resolve target needs an MSAA color attachment and a distinct "
                        "single-sample render target of the same format and size");
        }

        target.resolves[index] = resolve;
        target.resolveMips[index] = resolveMip;
        target.resolveSlices[index] = resolveSlice;
    }

    if (recording_) {
        return Result<CommandContext*>::Failure(ErrorCode::Busy);
    }

    if (timeline_.IsLost()) {
        return Result<CommandContext*>::Failure(ErrorCode::Destroyed);
    }

    // render pass 열쇠·clear 값·framebuffer view다.
    VkImageView views[MaxPassAttachments] = {};
    target.colorCount = pass.colorCount;
    target.key.colorCount = pass.colorCount;
    target.key.samples = static_cast<VkSampleCountFlagBits>(target.sampleCount);
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        const ColorAttachment& color = pass.colors[index];
        const TextureRecord* record = textures_.Find(color.texture.slot);
        target.colors[index] = color.texture;
        target.key.colors[index] = TextureFormat(record->format).format;
        target.key.colorLoad[index] = LoadOf(color.load);
        target.key.colorStore[index] = StoreOf(color.store);
        target.clears[index].color = ClearValueOf(record->format, color.clear);
        target.colorMips[index] = color.mipLevel;
        target.colorSlices[index] = color.slice;
        auto view = AttachmentViewFor(*record, color.mipLevel, color.slice);
        if (!view) {
            return Result<CommandContext*>::Failure(view.Error());
        }

        views[index] = view.Value();
        if (index == 0) {
            target.image = record->image;
            target.viewFormat = target.key.colors[0];
            target.format = record->format;
            target.texture = color.texture;
        }
    }

    if (hasDepth) {
        const DepthAttachment& depth = pass.depth;
        const TextureRecord* record = textures_.Find(depth.texture.slot);
        const bool stencil = GetPixelFormatInfo(record->format).stencilBits != 0;
        target.depth = depth.texture;
        target.depthReadOnly = depth.readOnly;
        target.key.depth = TextureFormat(record->format).format;
        target.key.depthReadOnly = depth.readOnly;
        // 읽기 전용 깊이는 내용을 그대로 읽는다(clear하지 않는다, D3D와 같다).
        target.key.depthLoad =
            depth.readOnly ? VK_ATTACHMENT_LOAD_OP_LOAD : LoadOf(depth.depthLoad);
        target.key.depthStore = StoreOf(depth.depthStore);
        target.key.stencilLoad =
            !stencil ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                     : (depth.readOnly ? VK_ATTACHMENT_LOAD_OP_LOAD : LoadOf(depth.stencilLoad));
        target.key.stencilStore =
            stencil ? StoreOf(depth.stencilStore) : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        target.clears[pass.colorCount].depthStencil = {depth.clearDepth, depth.clearStencil};
        target.depthMip = depth.mipLevel;
        target.depthSlice = depth.slice;
        auto view = AttachmentViewFor(*record, depth.mipLevel, depth.slice);
        if (!view) {
            return Result<CommandContext*>::Failure(view.Error());
        }

        views[pass.colorCount] = view.Value();
    }

    const uint32 viewCount = pass.colorCount + (hasDepth ? 1u : 0u);
    if (viewCount != 0) {
        auto renderPass = RenderPassFor(target.key);
        if (!renderPass) {
            return Result<CommandContext*>::Failure(renderPass.Error());
        }

        auto framebuffer =
            FramebufferFor(renderPass.Value(), views, viewCount, target.width, target.height);
        if (!framebuffer) {
            return Result<CommandContext*>::Failure(framebuffer.Error());
        }

        target.framebuffer = framebuffer.Value();
    }

    auto begun = BeginRecording();
    if (!begun) {
        return Result<CommandContext*>::Failure(begun.Error());
    }

    target_ = target;
    presentTarget_ = {};
    StartPass();
    return Result<CommandContext*>::Success(this);
}

} // namespace rocks::graphics::vulkan
