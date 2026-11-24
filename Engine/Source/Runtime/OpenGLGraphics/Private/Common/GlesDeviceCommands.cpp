#include "GlesDevice.h"

// GX3·GX4 명령 기록이다(GlesDevice.h): 파이프라인 고정 기능 상태, compute 프로그램·dispatch, 패스
// attachment(BeginPass), 간접 그리기·dispatch, viewport·scissor·스텐실 기준, 버퍼·텍스처 복사, UAV
// clear. GL은 명령 순서대로 실행하므로 전이는 없고, compute의 image·storage 쓰기만 dispatch 뒤
// glMemoryBarrier로 다음 명령에 보이게 한다.
namespace rocks::graphics::gles {

namespace {

GLenum CompareOf(CompareFunction function) noexcept {
    // GL_NEVER(0x0200)부터 GL_ALWAYS(0x0207)까지 Rocks 순서와 같다.
    return GL_NEVER + static_cast<GLenum>(function);
}

GLenum StencilOf(StencilOperation operation) noexcept {
    switch (operation) {
    case StencilOperation::Keep:
        return GL_KEEP;
    case StencilOperation::Zero:
        return GL_ZERO;
    case StencilOperation::Replace:
        return GL_REPLACE;
    case StencilOperation::IncrementSaturate:
        return GL_INCR;
    case StencilOperation::DecrementSaturate:
        return GL_DECR;
    case StencilOperation::Invert:
        return GL_INVERT;
    case StencilOperation::IncrementWrap:
        return GL_INCR_WRAP;
    case StencilOperation::DecrementWrap:
        return GL_DECR_WRAP;
    }

    return GL_KEEP;
}

GLenum BlendOf(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::Zero:
        return GL_ZERO;
    case BlendFactor::One:
        return GL_ONE;
    case BlendFactor::SourceColor:
        return GL_SRC_COLOR;
    case BlendFactor::InverseSourceColor:
        return GL_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SourceAlpha:
        return GL_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return GL_DST_COLOR;
    case BlendFactor::InverseDestinationColor:
        return GL_ONE_MINUS_DST_COLOR;
    case BlendFactor::DestinationAlpha:
        return GL_DST_ALPHA;
    case BlendFactor::InverseDestinationAlpha:
        return GL_ONE_MINUS_DST_ALPHA;
    case BlendFactor::SourceAlphaSaturate:
        return GL_SRC_ALPHA_SATURATE;
    }

    return GL_ONE;
}

// 알파 계수 자리의 색 계수는 알파 계수로 읽는다(D3D 알파 블렌드 규칙과 같은 결과).
GLenum AlphaBlendOf(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::SourceColor:
        return GL_SRC_ALPHA;
    case BlendFactor::InverseSourceColor:
        return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return GL_DST_ALPHA;
    case BlendFactor::InverseDestinationColor:
        return GL_ONE_MINUS_DST_ALPHA;
    default:
        return BlendOf(factor);
    }
}

GLenum BlendOperationOf(BlendOperation operation) noexcept {
    switch (operation) {
    case BlendOperation::Add:
        return GL_FUNC_ADD;
    case BlendOperation::Subtract:
        return GL_FUNC_SUBTRACT;
    case BlendOperation::ReverseSubtract:
        return GL_FUNC_REVERSE_SUBTRACT;
    case BlendOperation::Min:
        return GL_MIN;
    case BlendOperation::Max:
        return GL_MAX;
    }

    return GL_FUNC_ADD;
}

GLboolean MaskBit(ColorWriteMask mask, ColorWriteMask bit) noexcept {
    return (mask & bit) == bit ? GL_TRUE : GL_FALSE;
}

} // namespace

void GlesDevice::ResetPassState() noexcept {
    for (GLuint index = 0; index < MaxColorTargets; ++index) {
        gl_.Disablei(GL_BLEND, index);
        gl_.ColorMaski(index, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    gl_.Disable(GL_SCISSOR_TEST);
    gl_.Disable(GL_CULL_FACE);
    gl_.Disable(GL_DEPTH_TEST);
    gl_.DepthMask(GL_TRUE);
    gl_.Disable(GL_STENCIL_TEST);
    gl_.StencilMaskSeparate(GL_FRONT_AND_BACK, NumericLimits<GLuint>::Max());
    gl_.Disable(GL_POLYGON_OFFSET_FILL);
    gl_.Disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    if (caps_.features.depthClamp) {
        gl_.Disable(GL_DEPTH_CLAMP);
    }
}

void GlesDevice::ApplyStencilFunction(const PipelineRecord& pipeline) noexcept {
    const DepthStencilState& state = pipeline.depthStencil;
    const GLint reference = static_cast<GLint>(stencilReference_);
    gl_.StencilFuncSeparate(GL_FRONT, CompareOf(state.front.compare), reference,
                            state.stencilReadMask);
    gl_.StencilFuncSeparate(GL_BACK, CompareOf(state.back.compare), reference,
                            state.stencilReadMask);
}

void GlesDevice::ApplyPipelineState(const PipelineRecord& pipeline) noexcept {
    for (GLuint index = 0; index < MaxColorTargets; ++index) {
        const ColorTargetBlend& blend = pipeline.blend[index];
        if (index < pipeline.colorTargetCount && blend.enable) {
            gl_.Enablei(GL_BLEND, index);
            gl_.BlendFuncSeparatei(index, BlendOf(blend.sourceColor),
                                   BlendOf(blend.destinationColor), AlphaBlendOf(blend.sourceAlpha),
                                   AlphaBlendOf(blend.destinationAlpha));
            gl_.BlendEquationSeparatei(index, BlendOperationOf(blend.colorOperation),
                                       BlendOperationOf(blend.alphaOperation));
        } else {
            gl_.Disablei(GL_BLEND, index);
        }

        const ColorWriteMask mask = blend.writeMask;
        gl_.ColorMaski(index, MaskBit(mask, ColorWriteMask::Red),
                       MaskBit(mask, ColorWriteMask::Green), MaskBit(mask, ColorWriteMask::Blue),
                       MaskBit(mask, ColorWriteMask::Alpha));
    }

    if (pipeline.alphaToCoverage) {
        gl_.Enable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    } else {
        gl_.Disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    }

    // 앞면 감김(glFrontFace)은 장치가 클립 공간 규약에 맞춰 한 번 정했다.
    const RasterState& raster = pipeline.raster;
    if (raster.cullMode == CullMode::None) {
        gl_.Disable(GL_CULL_FACE);
    } else {
        gl_.Enable(GL_CULL_FACE);
        gl_.CullFace(raster.cullMode == CullMode::Back ? GL_BACK : GL_FRONT);
    }

    // 깊이 bias는 D3D 식(상수 × 최소 분해 단위 + 기울기 × 최대 기울기)과 같은 glPolygonOffset이다.
    if (raster.depthBias != 0 || raster.depthBiasSlope != 0.0f) {
        gl_.Enable(GL_POLYGON_OFFSET_FILL);
        if (raster.depthBiasClamp != 0.0f && polygonOffsetClamp_ != nullptr) {
            polygonOffsetClamp_(raster.depthBiasSlope, static_cast<GLfloat>(raster.depthBias),
                                raster.depthBiasClamp);
        } else {
            gl_.PolygonOffset(raster.depthBiasSlope, static_cast<GLfloat>(raster.depthBias));
        }
    } else {
        gl_.Disable(GL_POLYGON_OFFSET_FILL);
    }

    if (caps_.features.depthClamp) {
        if (raster.depthClamp) {
            gl_.Enable(GL_DEPTH_CLAMP);
        } else {
            gl_.Disable(GL_DEPTH_CLAMP);
        }
    }

    // 깊이 쓰기만 켠 상태(시험 없음)는 GL에서 깊이 시험을 켜고 ALWAYS로 둔다(시험을 끄면 쓰기도
    // 꺼진다).
    const DepthStencilState& state = pipeline.depthStencil;
    if (state.depthTest || state.depthWrite) {
        gl_.Enable(GL_DEPTH_TEST);
        gl_.DepthFunc(state.depthTest ? CompareOf(state.depthCompare) : GL_ALWAYS);
        gl_.DepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);
    } else {
        gl_.Disable(GL_DEPTH_TEST);
        gl_.DepthMask(GL_FALSE);
    }

    if (state.stencilTest) {
        gl_.Enable(GL_STENCIL_TEST);
        ApplyStencilFunction(pipeline);
        gl_.StencilOpSeparate(GL_FRONT, StencilOf(state.front.fail),
                              StencilOf(state.front.depthFail), StencilOf(state.front.pass));
        gl_.StencilOpSeparate(GL_BACK, StencilOf(state.back.fail), StencilOf(state.back.depthFail),
                              StencilOf(state.back.pass));
        gl_.StencilMaskSeparate(GL_FRONT_AND_BACK, state.stencilWriteMask);
    } else {
        gl_.Disable(GL_STENCIL_TEST);
    }
}

Result<PipelineHandle> GlesDevice::CreateComputePipeline(const ComputePipelineDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<PipelineHandle>::Failure(owner.Error());
    }

    if (!ValidateComputePipelineDesc(desc)) {
        Diagnostic message;
        message.Text("compute pipeline needs valid drawConstantBytes");
        auto rejected = Reject(message, ErrorCode::InvalidArgument);
        return Result<PipelineHandle>::Failure(rejected.Error());
    }

    char error[PipelineSlotErrorBytes] = {};
    const DeviceShaderCode* shader = nullptr;
    PipelineRecord record;
    record.compute = true;
    record.drawConstantBytes = desc.drawConstantBytes;
    if (!FindComputeShader(shaders_, desc, shader, error) ||
        !ResolvePipelineSlots(Span<const DeviceShaderCode* const>(&shader, 1), desc.bindings,
                              desc.drawConstantBytes, record.slots, error)) {
        Diagnostic message;
        message.Text(error);
        auto rejected = Reject(message, ErrorCode::InvalidArgument);
        return Result<PipelineHandle>::Failure(rejected.Error());
    }

    if (!MakeCurrentForResources()) {
        return FailWith<PipelineHandle>("cannot make the OpenGL context current");
    }

    record.bufferSlots = record.slots.bufferSlots;
    record.writeSlots = record.slots.writeSlots;
    record.writeTextures = record.slots.writeTextures;
    auto linked = LinkProgram(Span<const DeviceShaderCode* const>(&shader, 1), record);
    if (!linked) {
        return Result<PipelineHandle>::Failure(linked.Error());
    }

    auto added = pipelines_.TryAdd(record);
    if (!added) {
        gl_.DeleteProgram(record.program);
        return Result<PipelineHandle>::Failure(added.Error());
    }

    return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
}

Result<CommandContext*> GlesDevice::BeginPass(const PassDesc& pass) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    const auto fail = [this](ErrorCode code, const char* text) noexcept {
        Diagnostic message;
        message.Text(text);
        auto rejected = Reject(message, code);
        return Result<CommandContext*>::Failure(rejected.Error());
    };
    if (pass.queue != QueueKind::Graphics) {
        return fail(ErrorCode::Unsupported, "OpenGL ES has no async compute queue");
    }

    if (pass.colorCount > MaxColorTargets) {
        return fail(ErrorCode::InvalidArgument,
                    "pass has more color attachments than MaxColorTargets");
    }

    if (pass.colorCount > caps_.limits.colorAttachments) {
        return fail(ErrorCode::Unsupported,
                    "pass has more color attachments than the device supports");
    }

    // attachment 확인: 살아 있는 텍스처, view 아님, 용도(색은 framebuffer가 있는 RenderTarget),
    // 같은 크기, 중복 없음.
    PassTarget target;
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

    if (!MakeCurrentForResources()) {
        return FailWith<CommandContext*>("cannot make the OpenGL context current");
    }

    target.colorCount = pass.colorCount;
    for (uint32 index = 0; index < pass.colorCount; ++index) {
        target.colors[index] = pass.colors[index].texture;
    }

    if (pass.colorCount != 0) {
        const TextureRecord* first = textures_.Find(pass.colors[0].texture.slot);
        target.format = first->format;
        target.texture = pass.colors[0].texture;
    }

    target.depth = pass.depth.texture;
    target.depthReadOnly = hasDepth && pass.depth.readOnly;
    target.firstColorMip = pass.colorCount != 0 ? pass.colors[0].mipLevel : 0u;
    const TextureRecord* single =
        pass.colorCount == 1 ? textures_.Find(pass.colors[0].texture.slot) : nullptr;
    if (single != nullptr && !hasDepth && single->framebuffer != 0 &&
        pass.colors[0].mipLevel == 0) {
        // 색 하나(2D mip 0)는 텍스처의 framebuffer를 그대로 쓴다.
        target.framebuffer = textures_.Find(pass.colors[0].texture.slot)->framebuffer;
    } else {
        // 그 밖은 장치의 패스 framebuffer에 attachment를 붙인다(EndFrame이 뗀다).
        if (passFramebuffer_ == 0) {
            gl_.GenFramebuffers(1, &passFramebuffer_);
        }

        gl_.BindFramebuffer(GL_FRAMEBUFFER, passFramebuffer_);
        GLenum drawBuffers[MaxColorTargets] = {};
        for (uint32 index = 0; index < MaxColorTargets; ++index) {
            const TextureRecord* color =
                index < pass.colorCount ? textures_.Find(pass.colors[index].texture.slot) : nullptr;
            if (color != nullptr) {
                AttachTextureLevel(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, *color,
                                   pass.colors[index].mipLevel, pass.colors[index].slice);
            } else {
                gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index,
                                         GL_TEXTURE_2D, 0u, 0);
            }
            drawBuffers[index] = index < pass.colorCount ? GL_COLOR_ATTACHMENT0 + index : GL_NONE;
        }

        const TextureRecord* depth = textures_.Find(pass.depth.texture.slot);
        const bool stencil = depth != nullptr && GetPixelFormatInfo(depth->format).stencilBits != 0;
        gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        if (depth != nullptr) {
            AttachTextureLevel(GL_FRAMEBUFFER,
                               stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, *depth,
                               pass.depth.mipLevel, pass.depth.slice);
        }

        gl_.DrawBuffers(pass.colorCount != 0 ? static_cast<GLsizei>(pass.colorCount) : 1,
                        drawBuffers);
        if (pass.colorCount != 0 || hasDepth) {
            const GLenum status = gl_.CheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                gl_.BindFramebuffer(GL_FRAMEBUFFER, 0);
                return fail(ErrorCode::Unsupported,
                            "the OpenGL ES driver rejects this attachment combination");
            }
        }

        target.framebuffer = passFramebuffer_;
    }

    presentTarget_ = {};
    pass_ = target;
    StartPass(pass);
    return Result<CommandContext*>::Success(this);
}

void GlesDevice::AttachTextureLevel(GLenum target, GLenum attachment, const TextureRecord& record,
                                    uint32 mipLevel, uint32 slice) noexcept {
    if (record.target == GL_TEXTURE_2D || record.target == GL_TEXTURE_2D_MULTISAMPLE ||
        record.target == GL_TEXTURE_CUBE_MAP) {
        const GLenum face = record.target == GL_TEXTURE_CUBE_MAP
                                ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice
                                : record.target;
        gl_.FramebufferTexture2D(target, attachment, face, record.texture,
                                 static_cast<GLint>(mipLevel));
        return;
    }

    // 2D 배열·cube 배열(layer = cube × 6 + 면)·3D(깊이 면)다(ES 3.2 9.2.8절).
    gl_.FramebufferTextureLayer(target, attachment, record.texture, static_cast<GLint>(mipLevel),
                                static_cast<GLint>(slice));
}

void GlesDevice::SetWritableBufferView(uint32 slot, BufferViewHandle view) noexcept {
    if (!recording_ || slot >= MaxDrawBufferViews) {
        return;
    }

    const BufferViewRecord* record = bufferViews_.Find(view.slot);
    const bool writable = record != nullptr && record->access == ViewAccess::ReadWrite;
    boundWriteViews_[slot] = writable ? view : BufferViewHandle{};
    // 쓰기 slot은 버퍼·텍스처가 같은 번호 공간이다(나중에 묶은 하나만 남는다).
    boundWriteTextures_[slot] = {};
}

void GlesDevice::SetWritableTexture(uint32 slot, TextureHandle view) noexcept {
    if (!recording_ || slot >= MaxDrawBufferViews) {
        return;
    }

    const TextureRecord* record = textures_.Find(view.slot);
    const bool writable = record != nullptr && record->writable;
    boundWriteTextures_[slot] = writable ? view : TextureHandle{};
    boundWriteViews_[slot] = {};
}

const PipelineRecord* GlesDevice::PrepareDraw(bool indexed) noexcept {
    const PipelineRecord* pipeline = recording_ ? DrawablePipeline() : nullptr;
    if (pipeline == nullptr || pipeline->compute ||
        (pass_.colorCount == 0 && pass_.depth.IsNull()) ||
        (pass_.depthReadOnly && pipeline->writesDepthStencil)) {
        return nullptr;
    }

    if (indexed) {
        const BufferRecord* index = buffers_.Find(boundIndex_.buffer.slot);
        if (index == nullptr || boundIndex_.offset >= index->size) {
            return nullptr;
        }
    }

    ApplyConstants(*pipeline);
    ApplySlots(*pipeline);
    return pipeline;
}

const PipelineRecord* GlesDevice::PrepareDispatch() noexcept {
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
    if (!recording_ || pipeline == nullptr || !pipeline->compute || !BufferViewsBound(*pipeline)) {
        return nullptr;
    }

    BoundBufferViewInfo written[MaxDrawBufferViews] = {};
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        const BufferViewRecord* view = bufferViews_.Find(boundWriteViews_[slot].slot);
        if (view != nullptr) {
            written[slot] = {true, view->kind, view->shaderType, view->stride};
        }
    }

    if (!DrawBufferSlotsSatisfied(pipeline->writeSlots, written)) {
        return nullptr;
    }

    // 텍스처로 쓰는 slot마다 쓰기 텍스처 view가 묶여야 한다.
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        if ((pipeline->writeTextures & (1u << slot)) != 0 &&
            textures_.Find(boundWriteTextures_[slot].slot) == nullptr) {
            return nullptr;
        }
    }

    ApplyConstants(*pipeline);
    ApplySlots(*pipeline);
    return pipeline;
}

GLuint GlesDevice::IndirectArguments(BufferHandle arguments, uint64 offset,
                                     uint64 bytes) const noexcept {
    const BufferRecord* record = buffers_.Find(arguments.slot);
    if (record == nullptr || !HasAnyBufferUsage(record->desc.usage, BufferUsage::Indirect) ||
        offset % 4 != 0 || offset > record->size || record->size - offset < bytes) {
        return 0;
    }

    return record->buffer;
}

void GlesDevice::Dispatch(uint32 groupCountX, uint32 groupCountY, uint32 groupCountZ) noexcept {
    const uint32 counts[3] = {groupCountX, groupCountY, groupCountZ};
    for (uint32 axis = 0; axis < 3; ++axis) {
        if (counts[axis] > caps_.limits.computeGroupCount[axis]) {
            ++ignoredDraws_;
            return;
        }
    }

    if (PrepareDispatch() == nullptr) {
        ++ignoredDraws_;
        return;
    }

    gl_.DispatchCompute(groupCountX, groupCountY, groupCountZ);
    // image·storage 쓰기를 뒤 명령(그리기 입력·간접 인자·복사·셰이더 읽기)에 보이게 한다.
    gl_.MemoryBarrier(GL_ALL_BARRIER_BITS);
}

void GlesDevice::DispatchIndirect(BufferHandle arguments, uint64 offset) noexcept {
    const GLuint buffer = IndirectArguments(arguments, offset, sizeof(DispatchIndirectArguments));
    if (buffer == 0 || PrepareDispatch() == nullptr) {
        ++ignoredDraws_;
        return;
    }

    gl_.BindBuffer(GL_DISPATCH_INDIRECT_BUFFER, buffer);
    gl_.DispatchComputeIndirect(static_cast<GLintptr>(offset));
    gl_.BindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    gl_.MemoryBarrier(GL_ALL_BARRIER_BITS);
}

void GlesDevice::DrawIndirect(BufferHandle arguments, uint64 offset) noexcept {
    const GLuint buffer = IndirectArguments(arguments, offset, sizeof(DrawIndirectArguments));
    const PipelineRecord* pipeline = buffer != 0 ? PrepareDraw(false) : nullptr;
    if (pipeline == nullptr) {
        ++ignoredDraws_;
        return;
    }

    gl_.BindBuffer(GL_DRAW_INDIRECT_BUFFER, buffer);
    gl_.DrawArraysIndirect(pipeline->topology,
                           reinterpret_cast<const void*>(static_cast<uptr>(offset)));
    gl_.BindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

void GlesDevice::DrawIndexedIndirect(BufferHandle arguments, uint64 offset) noexcept {
    // GL 간접 인자의 firstIndex는 index 버퍼 처음부터 센다. index 버퍼 offset을 더할 곳이 없으므로
    // offset 0만 받는다(Device.h DrawIndexedIndirect 주석).
    const GLuint buffer =
        IndirectArguments(arguments, offset, sizeof(DrawIndexedIndirectArguments));
    const PipelineRecord* pipeline =
        buffer != 0 && boundIndex_.offset == 0 ? PrepareDraw(true) : nullptr;
    if (pipeline == nullptr) {
        ++ignoredDraws_;
        return;
    }

    gl_.BindBuffer(GL_DRAW_INDIRECT_BUFFER, buffer);
    gl_.DrawElementsIndirect(pipeline->topology,
                             boundIndex_.format == IndexFormat::UInt16 ? GL_UNSIGNED_SHORT
                                                                       : GL_UNSIGNED_INT,
                             reinterpret_cast<const void*>(static_cast<uptr>(offset)));
    gl_.BindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

void GlesDevice::SetViewport(const Viewport& viewport) noexcept {
    if (!recording_ || viewport.width <= 0.0f || viewport.height <= 0.0f) {
        return;
    }

    // 클립 공간이 GL_UPPER_LEFT라 창 좌표 y 0이 이미지 위쪽 행이다(Rocks 원점과 같다). ES
    // viewport는 정수다.
    gl_.Viewport(static_cast<GLint>(viewport.x), static_cast<GLint>(viewport.y),
                 static_cast<GLsizei>(viewport.width), static_cast<GLsizei>(viewport.height));
    gl_.DepthRangef(viewport.minDepth, viewport.maxDepth);
}

void GlesDevice::SetScissor(const ScissorRect& scissor) noexcept {
    if (!recording_) {
        return;
    }

    gl_.Scissor(static_cast<GLint>(scissor.x), static_cast<GLint>(scissor.y),
                static_cast<GLsizei>(scissor.width), static_cast<GLsizei>(scissor.height));
}

void GlesDevice::SetStencilReference(uint32 reference) noexcept {
    if (!recording_) {
        return;
    }

    // GL은 기준 값을 비교 함수와 함께 건다. 그래픽 파이프라인이 없으면 SetPipeline이 건다.
    stencilReference_ = reference;
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
    if (pipeline != nullptr && !pipeline->compute && pipeline->depthStencil.stencilTest) {
        ApplyStencilFunction(*pipeline);
    }
}

Result<void> GlesDevice::CopyBuffer(BufferHandle destination, uint64 destinationOffset,
                                    BufferHandle source, uint64 sourceOffset,
                                    uint64 size) noexcept {
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const BufferRecord* to = buffers_.Find(destination.slot);
    const BufferRecord* from = buffers_.Find(source.slot);
    if (to == nullptr || from == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (size == 0 || destination == source || destinationOffset % 4 != 0 || sourceOffset % 4 != 0 ||
        size % 4 != 0 || destinationOffset > to->size || to->size - destinationOffset < size ||
        sourceOffset > from->size || from->size - sourceOffset < size) {
        Diagnostic message;
        message.Text("buffer copy range is empty, out of range, unaligned or within one buffer");
        return Reject(message, ErrorCode::InvalidArgument);
    }

    gl_.BindBuffer(GL_COPY_READ_BUFFER, from->buffer);
    gl_.BindBuffer(GL_COPY_WRITE_BUFFER, to->buffer);
    gl_.CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                          static_cast<GLintptr>(sourceOffset),
                          static_cast<GLintptr>(destinationOffset), static_cast<GLsizeiptr>(size));
    gl_.BindBuffer(GL_COPY_READ_BUFFER, 0);
    gl_.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
    return Result<void>::Success();
}

Result<void> GlesDevice::CopyTexture(TextureHandle destination, TextureHandle source) noexcept {
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord* to = textures_.Find(destination.slot);
    const TextureRecord* from = textures_.Find(source.slot);
    if (to == nullptr || from == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const auto attached = [this](TextureHandle handle) noexcept {
        for (uint32 index = 0; index < pass_.colorCount; ++index) {
            if (pass_.colors[index] == handle) {
                return true;
            }
        }

        return handle == pass_.depth;
    };
    const PixelFormatInfo& info = GetPixelFormatInfo(from->format);
    if (destination == source || to->isView || from->isView || to->width != from->width ||
        to->height != from->height || to->format != from->format || info.planeCount > 1 ||
        to->sampleCount > 1 || from->sampleCount > 1 ||
        to->dimension != TextureDimension::Texture2D ||
        from->dimension != TextureDimension::Texture2D || attached(destination) ||
        attached(source)) {
        Diagnostic message;
        message.Text("texture copy needs two distinct non-view single-plane 2D textures of the "
                     "same size and format outside the current pass");
        return Reject(message, ErrorCode::InvalidArgument);
    }

    gl_.CopyImageSubData(from->texture, GL_TEXTURE_2D, 0, 0, 0, 0, to->texture, GL_TEXTURE_2D, 0, 0,
                         0, 0, static_cast<GLsizei>(from->width),
                         static_cast<GLsizei>(from->height), 1);
    return Result<void>::Success();
}

Result<void> GlesDevice::ClearWritableBufferView(BufferViewHandle view, uint32 value) noexcept {
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const BufferViewRecord* record = bufferViews_.Find(view.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    Diagnostic message;
    if (record->access != ViewAccess::ReadWrite) {
        message.Text("only writable views can be cleared");
        return Reject(message, ErrorCode::InvalidArgument);
    }

    const bool clearable =
        record->kind == BufferViewKind::Raw ||
        (record->kind == BufferViewKind::Typed &&
         (record->format == PixelFormat::R32Uint || record->format == PixelFormat::R32Sint));
    if (!clearable) {
        message.Text("only raw and R32Uint or R32Sint typed views can be cleared");
        return Reject(message, ErrorCode::Unsupported);
    }

    // ES에는 glClearBufferSubData가 없으므로 같은 값의 원소 묶음을 명령 순서대로 덮어
    // 쓴다(업로드처럼 호출이 반환되면 원본을 다시 써도 된다).
    constexpr usize ChunkWords = 1024;
    uint32 pattern[ChunkWords];
    for (uint32& word : pattern) {
        word = value;
    }

    gl_.BindBuffer(GL_COPY_WRITE_BUFFER, record->buffer);
    uint64 written = 0;
    while (written < record->size) {
        const uint64 remaining = record->size - written;
        const uint64 bytes = remaining < sizeof(pattern) ? remaining : sizeof(pattern);
        gl_.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(record->offset + written),
                          static_cast<GLsizeiptr>(bytes), pattern);
        written += bytes;
    }

    gl_.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
    return Result<void>::Success();
}

} // namespace rocks::graphics::gles
