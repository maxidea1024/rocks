#include "VulkanDeviceImpl.h"
#include "VulkanFormats.h"

// render pass·파이프라인 layout·파이프라인이다(Docs/Design/VulkanGraphics.ko.md 6·7·11절).
namespace rocks::graphics::vulkan {

namespace {

// 진단 문자열 조립 칸이다(셰이더 입력 대조 실패의 location·형식·셰이더 타입).
class Diagnostic {
public:
    Diagnostic& Text(const char* text) noexcept {
        while (*text != '\0' && length_ + 1 < sizeof(buffer_)) {
            buffer_[length_++] = *text++;
        }

        buffer_[length_] = '\0';
        return *this;
    }

    Diagnostic& Number(uint32 value) noexcept {
        char digits[12] = {};
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0 && length_ + 1 < sizeof(buffer_)) {
            buffer_[length_++] = digits[--count];
        }

        buffer_[length_] = '\0';
        return *this;
    }

    const char* CStr() const noexcept {
        return buffer_;
    }

private:
    char buffer_[256] = {};
    usize length_ = 0;
};

// 앞면은 화면 반시계다. 음수 viewport 높이로 framebuffer가 표시 화면과 같으므로 Vulkan의
// COUNTER_CLOCKWISE와 BACK·FRONT가 D3D12(FrontCounterClockwise = TRUE)와 같은 뜻이다. UE는
// CLOCKWISE·CM_CW → FRONT_BIT로 컬링만 맞추고 SV_IsFrontFace를 셰이더에서 뒤집는다(VulkanGraphics
// 11절).
VkCullModeFlags CullModeOf(CullMode mode) noexcept {
    switch (mode) {
    case CullMode::Back:
        return VK_CULL_MODE_BACK_BIT;
    case CullMode::Front:
        return VK_CULL_MODE_FRONT_BIT;
    case CullMode::None:
        break;
    }

    return VK_CULL_MODE_NONE;
}

VkCompareOp CompareOf(CompareFunction function) noexcept {
    // VkCompareOp는 NEVER(0)부터 ALWAYS(7)까지 Rocks 순서와 같다.
    return static_cast<VkCompareOp>(function);
}

VkStencilOp StencilOf(StencilOperation operation) noexcept {
    switch (operation) {
    case StencilOperation::Keep:
        return VK_STENCIL_OP_KEEP;
    case StencilOperation::Zero:
        return VK_STENCIL_OP_ZERO;
    case StencilOperation::Replace:
        return VK_STENCIL_OP_REPLACE;
    case StencilOperation::IncrementSaturate:
        return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case StencilOperation::DecrementSaturate:
        return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case StencilOperation::Invert:
        return VK_STENCIL_OP_INVERT;
    case StencilOperation::IncrementWrap:
        return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case StencilOperation::DecrementWrap:
        return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }

    return VK_STENCIL_OP_KEEP;
}

VkBlendFactor BlendOf(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::Zero:
        return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One:
        return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SourceColor:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::InverseSourceColor:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SourceAlpha:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::InverseDestinationColor:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::DestinationAlpha:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InverseDestinationAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::SourceAlphaSaturate:
        return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }

    return VK_BLEND_FACTOR_ONE;
}

// 알파 계수 자리의 색 계수는 알파 계수로 읽는다(D3D 알파 블렌드 규칙과 같은 결과).
VkBlendFactor AlphaBlendOf(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::SourceColor:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InverseSourceColor:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InverseDestinationColor:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default:
        return BlendOf(factor);
    }
}

VkBlendOp BlendOperationOf(BlendOperation operation) noexcept {
    // VkBlendOp는 ADD(0)·SUBTRACT·REVERSE_SUBTRACT·MIN·MAX로 Rocks 순서와 같다.
    return static_cast<VkBlendOp>(operation);
}

VkPrimitiveTopology TopologyOf(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::TriangleStrip:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::LineStrip:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PrimitiveTopology::PointList:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    default:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkStencilOpState StencilFaceOf(const StencilFace& face, const DepthStencilState& state) noexcept {
    VkStencilOpState op = {};
    op.failOp = StencilOf(face.fail);
    op.passOp = StencilOf(face.pass);
    op.depthFailOp = StencilOf(face.depthFail);
    op.compareOp = CompareOf(face.compare);
    op.compareMask = state.stencilReadMask;
    op.writeMask = state.stencilWriteMask;
    // 기준 값은 동적 상태다(SetStencilReference).
    op.reference = 0;
    return op;
}

} // namespace

bool SameRenderPassKey(const RenderPassKey& left, const RenderPassKey& right) noexcept {
    if (left.colorCount != right.colorCount || left.depth != right.depth ||
        left.depthLoad != right.depthLoad || left.depthStore != right.depthStore ||
        left.stencilLoad != right.stencilLoad || left.stencilStore != right.stencilStore ||
        left.depthReadOnly != right.depthReadOnly || left.samples != right.samples) {
        return false;
    }

    for (uint32 index = 0; index < left.colorCount; ++index) {
        if (left.colors[index] != right.colors[index] ||
            left.colorLoad[index] != right.colorLoad[index] ||
            left.colorStore[index] != right.colorStore[index]) {
            return false;
        }
    }

    return true;
}

Result<void> VulkanDevice::CreatePipelineLayout() noexcept {
    // 공용 layout이다: 두 set 모두 빈 layout이고 push constant만 있다. 파이프라인마다 리플렉션으로
    // set layout을 만들고(BuildPipelineLayout) push constant 범위는 이 layout과 같아 그리기 상수를
    // 이 layout으로 올린다(사양 "Pipeline Layout Compatibility": push constant 범위가 같으면 호환).
    VkDescriptorSetLayoutCreateInfo empty = {};
    empty.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    VkResult result =
        functions_.vkCreateDescriptorSetLayout(device_, &empty, nullptr, &emptySetLayout_);
    if (result != VK_SUCCESS) {
        emptySetLayout_ = VK_NULL_HANDLE;
        return Fail("vkCreateDescriptorSetLayout", result);
    }

    // 그리기별 상수는 push constant다(ShaderSystem 7.2절 2항, 계약 최대 128 byte = 최소 보장).
    const VkDescriptorSetLayout layouts[] = {emptySetLayout_, emptySetLayout_};
    VkPushConstantRange constants = {};
    constants.stageFlags = ConstantStages;
    constants.offset = 0;
    constants.size = ConstantBytes;
    VkPipelineLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 2;
    info.pSetLayouts = layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &constants;
    result = functions_.vkCreatePipelineLayout(device_, &info, nullptr, &pipelineLayout_);
    if (result != VK_SUCCESS) {
        pipelineLayout_ = VK_NULL_HANDLE;
        return Fail("vkCreatePipelineLayout", result);
    }

    VkPipelineCacheCreateInfo cache = {};
    cache.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    result = functions_.vkCreatePipelineCache(device_, &cache, nullptr, &pipelineCache_);
    if (result != VK_SUCCESS) {
        pipelineCache_ = VK_NULL_HANDLE;
        return Fail("vkCreatePipelineCache", result);
    }

    return Result<void>::Success();
}

Result<VkRenderPass> VulkanDevice::RenderPassFor(VkFormat format, bool load) noexcept {
    RenderPassKey key;
    key.colorCount = 1;
    key.colors[0] = format;
    key.colorLoad[0] = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    key.colorStore[0] = VK_ATTACHMENT_STORE_OP_STORE;
    return RenderPassFor(key);
}

Result<VkRenderPass> VulkanDevice::RenderPassFor(const RenderPassKey& key) noexcept {
    for (const RenderPassEntry& entry : renderPasses_) {
        if (SameRenderPassKey(entry.key, key)) {
            return Result<VkRenderPass>::Success(entry.renderPass);
        }
    }

    // layout은 시작·끝 모두 attachment layout(색 COLOR_ATTACHMENT, 깊이
    // DEPTH_STENCIL_ATTACHMENT·읽기 전용이면 DEPTH_STENCIL_READ_ONLY)이고 전이는 바깥 barrier가
    // 한다. load·store op만 다른 render pass끼리는 호환이다(사양 "Render Pass Compatibility".
    // 의존은 호환성에 들지 않는다).
    VkAttachmentDescription attachments[MaxPassAttachments] = {};
    VkAttachmentReference colors[MaxColorTargets] = {};
    for (uint32 index = 0; index < key.colorCount; ++index) {
        VkAttachmentDescription& attachment = attachments[index];
        attachment.format = key.colors[index];
        attachment.samples = key.samples;
        attachment.loadOp = key.colorLoad[index];
        attachment.storeOp = key.colorStore[index];
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[index] = {index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }

    const bool hasDepth = key.depth != VK_FORMAT_UNDEFINED;
    const VkImageLayout depthLayout = key.depthReadOnly
                                          ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth = {key.colorCount, depthLayout};
    if (hasDepth) {
        VkAttachmentDescription& attachment = attachments[key.colorCount];
        attachment.format = key.depth;
        attachment.samples = key.samples;
        attachment.loadOp = key.depthLoad;
        attachment.storeOp = key.depthStore;
        attachment.stencilLoadOp = key.stencilLoad;
        attachment.stencilStoreOp = key.stencilStore;
        attachment.initialLayout = depthLayout;
        attachment.finalLayout = depthLayout;
    }

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = key.colorCount;
    subpass.pColorAttachments = key.colorCount != 0 ? colors : nullptr;
    subpass.pDepthStencilAttachment = hasDepth ? &depth : nullptr;
    // 바깥 의존: 앞 명령(앞 render pass·앞 제출 포함)의 attachment 쓰기 뒤에 이 pass의 attachment
    // 읽기·쓰기가 온다. 같은 대상이 attachment 상태로 남은 채 이어지는 pass(풀에서 다시 쓴 대상,
    // 전이·dispatch로 닫았다 다시 연 LOAD pass)의 WAW·RAW를 이 의존이 막는다. 암묵 바깥 의존은
    // TOP_OF_PIPE·access 0이라 이 일을 하지 않는다(사양 "Render Pass Creation").
    constexpr VkPipelineStageFlags AttachmentStages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = AttachmentStages;
    dependency.dstStageMask = AttachmentStages;
    dependency.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = key.colorCount + (hasDepth ? 1u : 0u);
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    RenderPassEntry entry;
    entry.key = key;
    const VkResult result =
        functions_.vkCreateRenderPass(device_, &info, nullptr, &entry.renderPass);
    if (result != VK_SUCCESS) {
        return FailWith<VkRenderPass>("vkCreateRenderPass", result);
    }

    auto pushed = renderPasses_.TryPushBack(entry);
    if (!pushed) {
        functions_.vkDestroyRenderPass(device_, entry.renderPass, nullptr);
        return Result<VkRenderPass>::Failure(pushed.Error());
    }

    return Result<VkRenderPass>::Success(entry.renderPass);
}

Result<void> VulkanDevice::Reject(const char* message) noexcept {
    auto assigned = lastError_.TryAssign(StringView(message));
    (void)assigned;
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> VulkanDevice::RejectDrawConstantSize(bool pipelineBound, usize bytes,
                                                  uint32 drawConstantBytes) noexcept {
    Diagnostic message;
    if (!pipelineBound) {
        message.Text("SetDrawConstants needs a pipeline bound while recording");
    } else {
        message.Text("SetDrawConstants got ")
            .Number(bytes > NumericLimits<uint32>::Max() ? NumericLimits<uint32>::Max()
                                                         : static_cast<uint32>(bytes))
            .Text(" bytes but the bound pipeline declares drawConstantBytes ")
            .Number(drawConstantBytes);
    }

    return Reject(message.CStr());
}

// 컴파일 훅의 정점 입력 리플렉션을 배치와 대조한다(RenderBase 10.4절 셰이더 입력 대조, D3D12와
// 같은 규칙과 진단 문구).
Result<void> VulkanDevice::MatchVertexInputs(const CompiledShader& vertex,
                                             const VertexLayout& layout) noexcept {
    for (const ShaderVertexInput& input : vertex.vertexInputs) {
        Diagnostic message;
        if (input.location >= MaxVertexAttributes) {
            message.Text("vertex shader input location ")
                .Number(input.location)
                .Text(" must use semantic ATTRIBUTE0-15");
            return Reject(message.CStr());
        }

        const VertexAttribute* attribute = nullptr;
        for (const VertexAttribute& candidate : layout.attributes) {
            if (candidate.location == input.location) {
                attribute = &candidate;
            }
        }

        if (attribute == nullptr) {
            message.Text("vertex shader reads location ")
                .Number(input.location)
                .Text(" which the vertex layout does not provide");
            return Reject(message.CStr());
        }

        if (VertexFormatShaderType(attribute->format) != input.type) {
            message.Text("vertex input location ")
                .Number(input.location)
                .Text(": format ")
                .Text(VertexFormatName(attribute->format))
                .Text(" feeds ")
                .Text(VertexShaderTypeName(VertexFormatShaderType(attribute->format)))
                .Text(" but the shader reads ")
                .Text(VertexShaderTypeName(input.type));
            return Reject(message.CStr());
        }
    }

    return Result<void>::Success();
}

namespace {

// 리플렉션 binding 하나를 고정 배치(PipelineDesc·ComputePipelineDesc)의 입력에 잇는다. PS의 t0
// 이미지와 s0 샘플러, 단계 모두의 t1–t4 버퍼 SRV(typed·구조화·raw), compute의 u0–u3 버퍼 UAV만
// 받는다. 그 밖 (상수 버퍼, 다른 register, space, 그래픽 단계의 쓰기 자원)이면 거짓이다.
bool MapBinding(const ShaderBinding& binding, ShaderStage stage, PipelineBinding& out,
                DrawBufferSlots& slots, DrawBufferSlots& writeSlots,
                uint32& writeTextures) noexcept {
    if (binding.sourceSpace != 0 || (binding.writable && stage != ShaderStage::Compute)) {
        return false;
    }

    out.binding = binding.binding;
    out.set = binding.set;
    // t0·s0(SetTexture)는 PS와 CS가 읽는다(텍스처 GX2 A부).
    const bool textureStage = stage == ShaderStage::Pixel || stage == ShaderStage::Compute;
    if (binding.shape == ShaderResourceShape::Image &&
        binding.kind == ShaderBindingKind::SampledImage) {
        out.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        out.source = BindingSource::Texture;
        return textureStage && binding.sourceRegister == 0;
    }

    if (binding.shape == ShaderResourceShape::Sampler) {
        out.type = VK_DESCRIPTOR_TYPE_SAMPLER;
        out.source = BindingSource::Sampler;
        return textureStage && binding.sourceRegister == 0;
    }

    // compute 쓰기 slot의 storage image(RWTexture*)는 SetWritableTexture slot이다.
    if (binding.shape == ShaderResourceShape::Image &&
        binding.kind == ShaderBindingKind::StorageImage) {
        out.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        out.source = BindingSource::WritableTexture;
        out.slot = binding.sourceRegister;
        if (!binding.writable || binding.sourceRegister >= MaxDrawBufferViews ||
            (writeSlots.usedMask & (1u << binding.sourceRegister)) != 0) {
            return false;
        }

        writeTextures |= 1u << binding.sourceRegister;
        return true;
    }

    // 쓰기 자원은 register u(slot), 읽기는 t(slot + 1)이다.
    const uint32 slot = binding.writable ? binding.sourceRegister : binding.sourceRegister - 1;
    if ((!binding.writable && binding.sourceRegister == 0) || slot >= MaxDrawBufferViews) {
        return false;
    }

    BufferViewKind kind = BufferViewKind::Typed;
    if (binding.shape == ShaderResourceShape::TexelBuffer) {
        out.type = binding.writable ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                    : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    } else if (binding.shape == ShaderResourceShape::StructuredBuffer ||
               binding.shape == ShaderResourceShape::RawBuffer) {
        out.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        kind = binding.shape == ShaderResourceShape::StructuredBuffer ? BufferViewKind::Structured
                                                                      : BufferViewKind::Raw;
    } else {
        return false;
    }

    out.source = binding.writable ? BindingSource::WritableBufferView : BindingSource::BufferView;
    out.slot = slot;
    const PixelShaderType type =
        kind == BufferViewKind::Typed ? binding.sampledType : PixelShaderType::None;
    return AddDrawBufferSlot(binding.writable ? writeSlots : slots, slot, kind, type,
                             binding.structureStride);
}

} // namespace

Result<void> VulkanDevice::BuildPipelineLayout(const CompiledShader& first,
                                               const CompiledShader* pixel, bool compute,
                                               PipelineRecord& record) noexcept {
    // 그리기별 상수 블록은 파이프라인이 선언한 크기 안이어야 한다(작은 것은 허용).
    const CompiledShader* stages[2] = {&first, pixel};
    for (uint32 stage = 0; stage < 2; ++stage) {
        if (stages[stage] != nullptr &&
            stages[stage]->rootConstantBytes > record.drawConstantBytes) {
            Diagnostic message;
            message.Text(compute ? "compute" : (stage == 0 ? "vertex" : "pixel"))
                .Text(" shader reads ")
                .Number(stages[stage]->rootConstantBytes)
                .Text(" bytes of draw constants but the pipeline declares drawConstantBytes ")
                .Number(record.drawConstantBytes);
            return Reject(message.CStr());
        }
    }

    // set = 단계다: VS(compute면 CS) 0, PS 1(ShaderSystem 7.2절).
    const CompiledShader* shaders[2] = {&first, pixel};
    const uint32 stageCount = compute ? 1u : 2u;
    VkDescriptorSetLayoutBinding layoutBindings[2][MaxPipelineBindings] = {};
    uint32 layoutCounts[2] = {};
    for (uint32 stage = 0; stage < stageCount; ++stage) {
        const ShaderStage shaderStage =
            compute ? ShaderStage::Compute
                    : (stage == 0 ? ShaderStage::Vertex : ShaderStage::Pixel);
        for (const ShaderBinding& binding : shaders[stage]->bindings) {
            PipelineBinding mapped;
            if (binding.set != stage || record.bindingCount == MaxPipelineBindings ||
                !MapBinding(binding, shaderStage, mapped, record.bufferSlots, record.writeSlots,
                            record.writeTextures)) {
                Diagnostic message;
                message.Text(compute ? "compute" : (stage == 0 ? "vertex" : "pixel"))
                    .Text(" shader resource at register ")
                    .Number(binding.sourceRegister)
                    .Text(" is outside the fixed layout (pixel t0 and s0, buffer views t1-t4, "
                          "compute buffer UAVs u0-u3; no UAVs in graphics stages)");
                return Reject(message.CStr());
            }

            record.bindings[record.bindingCount++] = mapped;
            VkDescriptorSetLayoutBinding& entry = layoutBindings[stage][layoutCounts[stage]++];
            entry.binding = mapped.binding;
            entry.descriptorType = mapped.type;
            entry.descriptorCount = 1;
            entry.stageFlags = compute      ? VK_SHADER_STAGE_COMPUTE_BIT
                               : stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT
                                            : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
    }

    VkDescriptorSetLayout layouts[2] = {emptySetLayout_, emptySetLayout_};
    for (uint32 stage = 0; stage < stageCount; ++stage) {
        if (layoutCounts[stage] == 0) {
            continue;
        }

        VkDescriptorSetLayoutCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = layoutCounts[stage];
        info.pBindings = layoutBindings[stage];
        const VkResult result = functions_.vkCreateDescriptorSetLayout(device_, &info, nullptr,
                                                                       &record.setLayouts[stage]);
        if (result != VK_SUCCESS) {
            record.setLayouts[stage] = VK_NULL_HANDLE;
            DestroyPipelineLayout(record);
            return Fail("vkCreateDescriptorSetLayout", result);
        }

        layouts[stage] = record.setLayouts[stage];
        record.setMask |= 1u << stage;
    }

    // set 수와 push constant 범위는 모든 파이프라인이 같다(공용 layout과 호환).
    VkPushConstantRange constants = {};
    constants.stageFlags = ConstantStages;
    constants.offset = 0;
    constants.size = ConstantBytes;
    VkPipelineLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 2;
    info.pSetLayouts = layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &constants;
    const VkResult result =
        functions_.vkCreatePipelineLayout(device_, &info, nullptr, &record.layout);
    if (result != VK_SUCCESS) {
        record.layout = VK_NULL_HANDLE;
        DestroyPipelineLayout(record);
        return Fail("vkCreatePipelineLayout", result);
    }

    return Result<void>::Success();
}

void VulkanDevice::DestroyPipelineLayout(PipelineRecord& record) noexcept {
    if (record.layout != VK_NULL_HANDLE) {
        functions_.vkDestroyPipelineLayout(device_, record.layout, nullptr);
        record.layout = VK_NULL_HANDLE;
    }

    for (VkDescriptorSetLayout& layout : record.setLayouts) {
        if (layout != VK_NULL_HANDLE) {
            functions_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
            layout = VK_NULL_HANDLE;
        }
    }
}

Result<VkShaderModule> VulkanDevice::CreateShaderModule(const CompiledShader& shader) noexcept {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = shader.code.Size() * sizeof(uint32);
    info.pCode = shader.code.Data();
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = functions_.vkCreateShaderModule(device_, &info, nullptr, &module);
    if (result != VK_SUCCESS) {
        return FailWith<VkShaderModule>("vkCreateShaderModule", result);
    }

    return Result<VkShaderModule>::Success(module);
}

// 다른 스레드에서도 부를 수 있다(concurrentResourceCreation, 내부 잠금으로 직렬화한다. 컴파일
// 훅도 이 잠금 안에서 부른다).
Result<PipelineHandle> VulkanDevice::CreatePipeline(const PipelineDesc& desc) noexcept {
    platform::LockGuard lock(gate_);
    if (desc.vertex.hlsl.ByteCount() == 0 || desc.pixel.hlsl.ByteCount() == 0 ||
        !ValidatePipelineDesc(desc)) {
        auto failed = Reject("pipeline description breaks the pipeline rules");
        return Result<PipelineHandle>::Failure(failed.Error());
    }

    // 호환 render pass 열쇠다(형식만 맞으면 load·store·layout이 다른 패스에서도 쓸 수 있다).
    RenderPassKey passKey;
    passKey.colorCount = desc.colorTargetCount;
    passKey.samples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);
    for (uint32 index = 0; index < desc.colorTargetCount; ++index) {
        const PixelFormat format = desc.colorFormats[index];
        const PixelFormatCaps required =
            desc.blend[index].enable ? PixelFormatCaps::RenderTarget | PixelFormatCaps::Blendable
                                     : PixelFormatCaps::RenderTarget;
        passKey.colors[index] = TextureFormat(format).format;
        passKey.colorLoad[index] = VK_ATTACHMENT_LOAD_OP_LOAD;
        passKey.colorStore[index] = VK_ATTACHMENT_STORE_OP_STORE;
        if (passKey.colors[index] == VK_FORMAT_UNDEFINED ||
            !HasPixelFormatCaps(GetPixelFormatCaps(format), required)) {
            return UnsupportedFormat<PipelineHandle>(format);
        }
    }

    if (desc.depthFormat != PixelFormat::Unknown) {
        passKey.depth = TextureFormat(desc.depthFormat).format;
        if (passKey.depth == VK_FORMAT_UNDEFINED ||
            !HasPixelFormatCaps(GetPixelFormatCaps(desc.depthFormat),
                                PixelFormatCaps::DepthStencil)) {
            return UnsupportedFormat<PipelineHandle>(desc.depthFormat);
        }
    }

    if (!CheckPipelineFeatures(caps_, desc)) {
        auto failed = FailCode("CreatePipeline", ErrorCode::Unsupported,
                               "pipeline needs more color targets or a raster feature than the "
                               "device supports");
        return Result<PipelineHandle>::Failure(failed.Error());
    }

    auto layoutChecked = ValidateVertexLayout(desc.layout);
    if (!layoutChecked) {
        auto failed = Reject("vertex layout is invalid (RenderBase 10.4 limits)");
        return Result<PipelineHandle>::Failure(failed.Error());
    }

    // 정점 형식의 VERTEX_BUFFER 기능이 없으면 다른 형식으로 바꾸지 않고 공개한다.
    VkVertexInputAttributeDescription attributes[MaxVertexAttributes] = {};
    for (usize index = 0; index < desc.layout.attributes.Size(); ++index) {
        const VertexAttribute& attribute = desc.layout.attributes[index];
        const VkFormat format = VertexAttributeFormat(attribute.format);
        VkFormatProperties support = {};
        functions_.vkGetPhysicalDeviceFormatProperties(physical_, format, &support);
        if ((support.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0) {
            Diagnostic message;
            message.Text("vertex format ")
                .Text(VertexFormatName(attribute.format))
                .Text(" is not supported by this Vulkan device");
            auto failed = Reject(message.CStr());
            (void)failed;
            return Result<PipelineHandle>::Failure(ErrorCode::Unsupported);
        }

        attributes[index].location = attribute.location;
        attributes[index].binding = attribute.stream;
        attributes[index].format = format;
        attributes[index].offset = attribute.offset;
    }

    if (shaderCompiler_ == nullptr) {
        auto failed = FailCode("CreatePipeline", ErrorCode::Unsupported,
                               "Vulkan needs cooked SPIR-V (ShaderSystem SH2) or a development "
                               "shader compiler (DeviceDesc::shaderCompiler)");
        return Result<PipelineHandle>::Failure(failed.Error());
    }

    CompiledShader vertex(allocator_);
    CompiledShader pixel(allocator_);
    lastError_.Clear();
    auto vertexCompiled =
        shaderCompiler_->CompileSpirv(ShaderStage::Vertex, desc.vertex.hlsl, vertex, lastError_);
    if (!vertexCompiled) {
        return Result<PipelineHandle>::Failure(vertexCompiled.Error());
    }

    auto pixelCompiled =
        shaderCompiler_->CompileSpirv(ShaderStage::Pixel, desc.pixel.hlsl, pixel, lastError_);
    if (!pixelCompiled) {
        return Result<PipelineHandle>::Failure(pixelCompiled.Error());
    }

    auto matched = MatchVertexInputs(vertex, desc.layout);
    if (!matched) {
        return Result<PipelineHandle>::Failure(matched.Error());
    }

    auto renderPass = RenderPassFor(passKey);
    if (!renderPass) {
        return Result<PipelineHandle>::Failure(renderPass.Error());
    }

    auto reserved = ReserveRetireCapacity(1);
    if (!reserved) {
        return Result<PipelineHandle>::Failure(reserved.Error());
    }

    PipelineRecord record;
    record.drawConstantBytes = desc.drawConstantBytes;
    auto layout = BuildPipelineLayout(vertex, &pixel, false, record);
    if (!layout) {
        return Result<PipelineHandle>::Failure(layout.Error());
    }

    auto vertexModule = CreateShaderModule(vertex);
    if (!vertexModule) {
        DestroyPipelineLayout(record);
        return Result<PipelineHandle>::Failure(vertexModule.Error());
    }

    auto pixelModule = CreateShaderModule(pixel);
    if (!pixelModule) {
        functions_.vkDestroyShaderModule(device_, vertexModule.Value(), nullptr);
        DestroyPipelineLayout(record);
        return Result<PipelineHandle>::Failure(pixelModule.Error());
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule.Value();
    stages[0].pName = "VSMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pixelModule.Value();
    stages[1].pName = "PSMain";

    VkVertexInputBindingDescription bindings[MaxVertexStreams] = {};
    for (usize index = 0; index < desc.layout.streams.Size(); ++index) {
        const VertexStream& stream = desc.layout.streams[index];
        bindings[index].binding = static_cast<uint32>(index);
        bindings[index].stride = stream.stride;
        bindings[index].inputRate = stream.stepRate == VertexStepRate::Instance
                                        ? VK_VERTEX_INPUT_RATE_INSTANCE
                                        : VK_VERTEX_INPUT_RATE_VERTEX;
        record.strides[index] = stream.stride;
        record.streamMask |= 1u << index;
    }

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32>(desc.layout.streams.Size());
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32>(desc.layout.attributes.Size());
    vertexInput.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly = {};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = TopologyOf(desc.topology);
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    // 깊이 clamp는 D3D DepthClipEnable = FALSE와 같다(clip 대신 clamp).
    raster.depthClampEnable = desc.raster.depthClamp ? VK_TRUE : VK_FALSE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = CullModeOf(desc.raster.cullMode);
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    const bool bias = desc.raster.depthBias != 0 || desc.raster.depthBiasSlope != 0.0f;
    raster.depthBiasEnable = bias ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = static_cast<float>(desc.raster.depthBias);
    raster.depthBiasClamp = desc.raster.depthBiasClamp;
    raster.depthBiasSlopeFactor = desc.raster.depthBiasSlope;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.sampleCount);
    multisample.alphaToCoverageEnable = desc.alphaToCoverage ? VK_TRUE : VK_FALSE;
    const DepthStencilState& state = desc.depthStencil;
    VkPipelineDepthStencilStateCreateInfo depth = {};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = state.depthTest || state.depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = state.depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = state.depthTest ? CompareOf(state.depthCompare) : VK_COMPARE_OP_ALWAYS;
    depth.stencilTestEnable = state.stencilTest ? VK_TRUE : VK_FALSE;
    depth.front = StencilFaceOf(state.front, state);
    depth.back = StencilFaceOf(state.back, state);
    depth.minDepthBounds = 0.0f;
    depth.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState blends[MaxColorTargets] = {};
    for (uint32 index = 0; index < desc.colorTargetCount; ++index) {
        const ColorTargetBlend& source = desc.blend[index];
        VkPipelineColorBlendAttachmentState& blend = blends[index];
        blend.blendEnable = source.enable ? VK_TRUE : VK_FALSE;
        blend.srcColorBlendFactor = BlendOf(source.sourceColor);
        blend.dstColorBlendFactor = BlendOf(source.destinationColor);
        blend.colorBlendOp = BlendOperationOf(source.colorOperation);
        blend.srcAlphaBlendFactor = AlphaBlendOf(source.sourceAlpha);
        blend.dstAlphaBlendFactor = AlphaBlendOf(source.destinationAlpha);
        blend.alphaBlendOp = BlendOperationOf(source.alphaOperation);
        // ColorWriteMask 비트(R 1, G 2, B 4, A 8)는 VkColorComponentFlags와 같다.
        blend.colorWriteMask = static_cast<VkColorComponentFlags>(source.writeMask);
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = desc.colorTargetCount;
    colorBlend.pAttachments = blends;
    const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                       VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32>(sizeof(dynamics) / sizeof(dynamics[0]));
    dynamic.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = desc.depthFormat != PixelFormat::Unknown ? &depth : nullptr;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = record.layout;
    info.renderPass = renderPass.Value();
    info.subpass = 0;
    const VkResult result = functions_.vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &info,
                                                                 nullptr, &record.pipeline);
    functions_.vkDestroyShaderModule(device_, vertexModule.Value(), nullptr);
    functions_.vkDestroyShaderModule(device_, pixelModule.Value(), nullptr);
    if (result != VK_SUCCESS) {
        DestroyPipelineLayout(record);
        return FailWith<PipelineHandle>("vkCreateGraphicsPipelines", result);
    }

    record.writesDepthStencil = PipelineWritesDepthStencil(desc);
    const VkPipeline pipeline = record.pipeline;
    PipelineRecord cleanup = record;
    auto added = pipelines_.TryAdd(Move(record));
    if (!added) {
        functions_.vkDestroyPipeline(device_, pipeline, nullptr);
        DestroyPipelineLayout(cleanup);
        return Result<PipelineHandle>::Failure(added.Error());
    }

    return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
}

} // namespace rocks::graphics::vulkan
