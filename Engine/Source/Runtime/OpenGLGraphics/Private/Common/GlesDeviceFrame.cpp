#include "GlesDevice.h"

// 프레임 기록·CommandContext·present다(GlesDevice.h).
namespace rocks::graphics::gles {

Result<CommandContext*> GlesDevice::BeginFrame(SwapChainHandle handle,
                                               const ClearColor& clear) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    const SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return Result<CommandContext*>::Failure(ErrorCode::StaleHandle);
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

    // 스왑체인은 offscreen 색 텍스처(첫 행 = 이미지 위쪽)에 그리고 EndFrame의 present가 창에
    // 옮긴다(ShaderSystem 7.2절 8항). 그래서 창·RenderTarget 패스의 규약이 같다.
    presentTarget_ = handle;
    pass_ = {};
    pass_.framebuffer = record->framebuffer;
    pass_.width = record->width;
    pass_.height = record->height;
    pass_.format = PixelFormat::RGBA8UnormSrgb;
    pass_.swapChain = handle;
    pass_.colorCount = 1;
    PassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].clear = clear;
    StartPass(pass);
    return Result<CommandContext*>::Success(this);
}

// 패스 끝에 MSAA 색 attachment를 resolve 대상에 glBlitFramebuffer로 resolve한다(ColorAttachment::
// resolveTarget, 결정 R17). 패스 상태(scissor 등)를 되돌린 뒤 부른다(blit은 scissor를 따른다).
void GlesDevice::ResolveAttachments() noexcept {
    for (uint32 index = 0; index < pass_.colorCount; ++index) {
        const TextureRecord* resolved = textures_.Find(pass_.resolves[index].slot);
        if (resolved == nullptr) {
            continue;
        }

        // 대상의 resolve mip·slice를 resolve framebuffer에 붙인다(텍스처 GX2 C부).
        if (resolveFramebuffer_ == 0) {
            gl_.GenFramebuffers(1, &resolveFramebuffer_);
        }

        const auto width = static_cast<GLint>(pass_.width);
        const auto height = static_cast<GLint>(pass_.height);
        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, pass_.framebuffer);
        gl_.ReadBuffer(GL_COLOR_ATTACHMENT0 + index);
        gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFramebuffer_);
        AttachTextureLevel(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, *resolved,
                           pass_.resolveMips[index], pass_.resolveSlices[index]);
        gl_.BlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                            GL_NEAREST);
        gl_.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    }

    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

Result<GpuFencePoint> GlesDevice::EndFrame() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<GpuFencePoint>::Failure(owner.Error());
    }

    if (!recording_) {
        return Result<GpuFencePoint>::Failure(ErrorCode::InvalidArgument);
    }

    // 끝나지 않은 분리 전이는 계약 위반이다(검증 장치는 오류로 센다).
    const usize unended = splits_.Close();
    if (validation_) {
        validationErrors_ += unended;
    }

    recording_ = false;
    ResetPassState();
    ResolveAttachments();
    // 패스 framebuffer의 attachment를 뗀다(놓인 텍스처 이름을 붙들지 않는다).
    if (pass_.framebuffer != 0 && pass_.framebuffer == passFramebuffer_) {
        gl_.BindFramebuffer(GL_FRAMEBUFFER, passFramebuffer_);
        for (GLenum index = 0; index < MaxColorTargets; ++index) {
            gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_TEXTURE_2D, 0,
                                     0);
        }

        gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    }

    gl_.BindVertexArray(0);
    gl_.UseProgram(0);
    for (; markerDepth_ != 0; --markerDepth_) {
        gl_.PopDebugGroup();
    }

    Result<void> outcome = Result<void>::Success();
    if (!presentTarget_.IsNull()) {
        // 기록 중 Retire된 스왑체인은 present를 생략한다.
        const SwapChainRecord* record = swapChains_.Find(presentTarget_.slot);
        if (record != nullptr) {
            Diagnostic error;
            if (!PresentBlit(*record)) {
                outcome = Fail("present blit failed: cannot make the window surface current");
            } else if (!platform_->Present(record->surface, record->vsync, error)) {
                outcome = Reject(error, ErrorCode::DependencyFailed);
            }

            // present 실패는 장치가 남긴다(LoggingSystem 13절). 연속 실패는 한 번만 남긴다.
            if (!outcome && !presentFailureLogged_) {
                presentFailureLogged_ = true;
                Log::Error(LogGraphicsOpenGL, "OpenGL ES present failed: {Reason}",
                           lastError_.View());
            } else if (outcome) {
                presentFailureLogged_ = false;
            }
        }
    }

    gl_.BindFramebuffer(GL_FRAMEBUFFER, 0);
    const uint64 previous = fenceValue_;
    const uint64 value = Submit();
    if (value == previous) {
        // fence를 넣지 못했다(context를 잃음). Submit이 장치 손실로 게시했다.
        return Result<GpuFencePoint>::Failure(ErrorCode::Destroyed);
    }

    timestamps_.Close(value, true);
    AssignPendingRetires(value);
    DeleteRetired(timeline_.Completed());
    if (!outcome) {
        return Result<GpuFencePoint>::Failure(outcome.Error());
    }

    return Result<GpuFencePoint>::Success(timeline_.Point(value));
}

Result<void> GlesDevice::ReadPixels(TextureHandle handle, Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const usize rowBytes = static_cast<usize>(PixelFormatRowBytes(record->format, record->width));
    if (record->framebuffer == 0 || record->sampleCount > 1 ||
        out.Size() != rowBytes * record->height || recording_) {
        return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
    }

    if (!MakeCurrentForResources()) {
        return Fail("cannot make the OpenGL context current");
    }

    // RenderTarget 저장소의 첫 행이 이미지 위쪽이므로 뒤집지 않고 읽는다. sRGB 형식도 저장된
    // 값(인코딩된 byte)을 그대로 읽는다.
    const TextureRecord copy = *record;
    return ReadColorLevel(copy, 0, 0, out);
}

void GlesDevice::StartPass(const PassDesc& pass) noexcept {
    recording_ = true;
    markerDepth_ = 0;
    boundPipeline_ = {};
    for (BoundVertexBuffer& bound : boundVertex_) {
        bound = {};
    }

    boundIndex_ = {};
    for (BufferViewHandle& view : boundViews_) {
        view = {};
    }

    for (BufferViewHandle& view : boundWriteViews_) {
        view = {};
    }

    for (TextureHandle& view : boundWriteTextures_) {
        view = {};
    }

    stencilReference_ = 0;
    constantsDirty_ = true;
    platform::MemoryZero(constants_, sizeof(constants_));
    for (auto& stage : boundConstants_) {
        for (BoundConstant& bound : stage) {
            bound = {};
        }
    }

    transientConstants_.Clear();
    gl_.BindFramebuffer(GL_FRAMEBUFFER, pass_.framebuffer);
    gl_.Viewport(0, 0, static_cast<GLsizei>(pass_.width), static_cast<GLsizei>(pass_.height));
    gl_.DepthRangef(0.0f, 1.0f);
    // load 동작이다. clear는 쓰기 mask·scissor를 따르므로 앞 패스·파이프라인 상태를 먼저 기본값으로
    // 돌린다(ResetPassState).
    ResetPassState();
    for (uint32 index = 0; index < pass_.colorCount; ++index) {
        if (pass.colors[index].load != LoadAction::Clear) {
            continue;
        }

        const ClearColor& clear = pass.colors[index].clear;
        const PixelFormat format = pass_.swapChain.IsNull()
                                       ? textures_.Find(pass_.colors[index].slot)->format
                                       : pass_.format;
        const PixelShaderType type = GetPixelFormatInfo(format).shaderType;
        const GLint drawBuffer = static_cast<GLint>(index);
        if (type == PixelShaderType::Uint) {
            const GLuint values[4] = {static_cast<GLuint>(clear.r), static_cast<GLuint>(clear.g),
                                      static_cast<GLuint>(clear.b), static_cast<GLuint>(clear.a)};
            gl_.ClearBufferuiv(GL_COLOR, drawBuffer, values);
        } else if (type == PixelShaderType::Int) {
            const GLint values[4] = {static_cast<GLint>(clear.r), static_cast<GLint>(clear.g),
                                     static_cast<GLint>(clear.b), static_cast<GLint>(clear.a)};
            gl_.ClearBufferiv(GL_COLOR, drawBuffer, values);
        } else {
            const GLfloat values[4] = {clear.r, clear.g, clear.b, clear.a};
            gl_.ClearBufferfv(GL_COLOR, drawBuffer, values);
        }
    }

    const TextureRecord* depth = textures_.Find(pass_.depth.slot);
    if (depth != nullptr && !pass_.depthReadOnly) {
        const bool stencil = GetPixelFormatInfo(depth->format).stencilBits != 0;
        const bool clearDepth = pass.depth.depthLoad == LoadAction::Clear;
        const bool clearStencil = stencil && pass.depth.stencilLoad == LoadAction::Clear;
        const GLint stencilValue = static_cast<GLint>(pass.depth.clearStencil);
        if (clearDepth && clearStencil) {
            gl_.ClearBufferfi(GL_DEPTH_STENCIL, 0, pass.depth.clearDepth, stencilValue);
        } else if (clearDepth) {
            gl_.ClearBufferfv(GL_DEPTH, 0, &pass.depth.clearDepth);
        } else if (clearStencil) {
            gl_.ClearBufferiv(GL_STENCIL, 0, &stencilValue);
        }
    }

    // 패스 동안 scissor는 늘 켜고 대상 전체에서 시작한다(SetScissor).
    gl_.Enable(GL_SCISSOR_TEST);
    gl_.Scissor(0, 0, static_cast<GLsizei>(pass_.width), static_cast<GLsizei>(pass_.height));
    // 텍스처는 그리기·dispatch 때 파이프라인 텍스처 유닛에 건다(ApplySlots, 비면 흰색).
    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        for (uint32 slot = 0; slot < MaxResourceSlots; ++slot) {
            boundTextures_[stage][slot] = {};
            boundSamplers_[stage][slot] = NoBoundSampler;
        }
    }
}

void GlesDevice::BindPassTexture(const TextureRecord& record, GLuint unit) noexcept {
    gl_.ActiveTexture(GL_TEXTURE0 + unit);
    gl_.BindTexture(record.target, record.texture);
    gl_.BindSampler(unit, record.sampler);
    // 깊이·스텐실 텍스처는 읽을 plane이 텍스처 객체 상태(ES 3.1 core)이므로 묶을 때 정한다. 한
    // 그리기에 텍스처 하나만 묶으므로 깊이 view와 스텐실 view가 서로 덮지 않는다.
    // 스텐실 plane은 다른 backend와 같이 값을 G 채널에서 읽는다(성분 대응 (R, R, 0, 1),
    // Docs/Design/PixelFormat 10절).
    const bool stencil = record.depthStencilTexture && record.plane == 1;
    if (record.depthStencilTexture) {
        gl_.TexParameteri(record.target, GL_DEPTH_STENCIL_TEXTURE_MODE,
                          static_cast<GLint>(stencil ? GL_STENCIL_INDEX : GL_DEPTH_COMPONENT));
    }

    // 성분 대응도 텍스처 객체 상태(view와 원 텍스처가 공유)라 묶을 때마다 정한다. 사용자
    // swizzle(record.swizzle)을 형식 흉내(A8 = R8 + (0, 0, 0, R))·스텐실 plane 대응 위에 합성해
    // 채널별 TEXTURE_SWIZZLE_R/G/B/A로 둔다(TEXTURE_SWIZZLE_RGBA는 ES에 없다).
    ChannelSwizzle inner;
    const GlesTextureFormat* format = FindTextureFormat(record.format);
    if (stencil) {
        inner = {SwizzleSource::Red, SwizzleSource::Red, SwizzleSource::Zero, SwizzleSource::One};
    } else if (format != nullptr && format->Is(GlesAlphaSwizzle)) {
        inner = {SwizzleSource::Zero, SwizzleSource::Zero, SwizzleSource::Zero, SwizzleSource::Red};
    }

    const ChannelSwizzle applied = ComposeSwizzle(record.swizzle, inner);
    constexpr GLenum Parameters[4] = {GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
                                      GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A};
    for (uint32 channel = 0; channel < 4; ++channel) {
        gl_.TexParameteri(record.target, Parameters[channel],
                          SwizzleParameter(applied.Channel(channel)));
    }
}

bool GlesDevice::PresentBlit(const SwapChainRecord& record) noexcept {
    if (!platform_->MakeCurrentOnSurface(record.surface)) {
        // wglMakeCurrent·eglMakeCurrent가 실패하면 이 스레드의 context가 풀릴 수 있다. 장치
        // 표면으로 되돌려 뒤의 GL 호출이 context 없이 실행되지 않게 한다(되돌리지 못하면 Submit이
        // 손실로 게시한다).
        platform_->MakeCurrent();
        return false;
    }

    uint32 width = record.width;
    uint32 height = record.height;
    if (!platform_->SurfaceExtent(record.surface, width, height)) {
        width = record.width;
        height = record.height;
    }

    // 텍스처 첫 행(이미지 위쪽)을 창 위쪽에 두도록 대상 y를 뒤집는다(기본 framebuffer는 아래쪽이
    // 원점). 창 크기가 다르면 늘려 그린다(D3D12 스왑체인의 표시 확대와 같다). sRGB byte는 그대로
    // 옮긴다(아래).
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, record.framebuffer);
    gl_.ReadBuffer(GL_COLOR_ATTACHMENT0);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // 데스크톱식 드라이버는 sRGB 쓰기 스위치를 끄고 byte를 그대로 옮긴다(UE도 표시 blit에서
    // GL_FRAMEBUFFER_SRGB를 끈다, WindowsOpenGLPlatform.cpp:505–517). ES 사양대로라면 스위치를 켠
    // 채 sRGB 창 표면으로 옮겨 디코딩·인코딩이 상쇄된다(PlatformInfo::rawSrgbBlit).
    if (rawSrgbBlit_) {
        gl_.Disable(GL_FRAMEBUFFER_SRGB);
    }

    const bool sameSize = width == record.width && height == record.height;
    gl_.BlitFramebuffer(0, 0, static_cast<GLint>(record.width), static_cast<GLint>(record.height),
                        0, static_cast<GLint>(height), static_cast<GLint>(width), 0,
                        GL_COLOR_BUFFER_BIT, sameSize ? GL_NEAREST : GL_LINEAR);
    if (rawSrgbBlit_) {
        gl_.Enable(GL_FRAMEBUFFER_SRGB);
    }

    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    // 기록 중(CopyPassTarget)이면 패스 대상을 다시 그리기 framebuffer로 둔다. EGL은 context가
    // 처음 표면에 붙을 때 viewport를 그 표면 크기로 바꾸므로 패스 viewport도 다시 둔다.
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, recording_ ? pass_.framebuffer : 0u);
    if (recording_) {
        gl_.Viewport(0, 0, static_cast<GLsizei>(pass_.width), static_cast<GLsizei>(pass_.height));
    }

    return true;
}

void GlesDevice::SetPipeline(PipelineHandle handle) noexcept {
    // 레코드 포인터는 프레임 중 파이프라인 생성으로 재배치될 수 있으므로 handle만 기억한다.
    boundPipeline_ = handle;
    const PipelineRecord* pipeline = pipelines_.Find(handle.slot);
    if (pipeline == nullptr) {
        return;
    }

    gl_.UseProgram(pipeline->program);
    // 프로그램마다 uniform 값이 따로이므로 새 프로그램에 상수를 다시 올린다.
    constantsDirty_ = true;
    if (pipeline->compute) {
        // 그래픽 상태·정점 배열은 그대로 둔다(같은 패스의 다음 그리기가 SetPipeline으로 다시 건다).
        return;
    }

    gl_.BindVertexArray(pipeline->vertexArray);
    ApplyPipelineState(*pipeline);
    // 앞서 묶은 정점·index 버퍼를 새 파이프라인의 정점 배열·간격으로 다시 묶는다.
    for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
        if (!boundVertex_[stream].buffer.IsNull()) {
            ApplyVertexBuffer(stream);
        }
    }

    ApplyIndexBuffer();
}

void GlesDevice::SetVertexBuffers(uint32 firstStream,
                                  Span<const VertexBufferBinding> buffers) noexcept {
    for (usize index = 0; index < buffers.Size(); ++index) {
        const usize stream = firstStream + index;
        if (stream >= MaxVertexStreams) {
            return;
        }

        boundVertex_[stream] = {buffers[index].buffer, buffers[index].offset};
        ApplyVertexBuffer(static_cast<uint32>(stream));
    }
}

void GlesDevice::SetIndexBuffer(BufferHandle buffer, IndexFormat format, uint64 offset) noexcept {
    boundIndex_ = {buffer, format, offset};
    ApplyIndexBuffer();
}

void GlesDevice::SetTexture(ShaderStage stage, uint32 slot, TextureHandle handle) noexcept {
    const uint32 stageIndex = ConstantBufferStageIndex(stage);
    if (stageIndex >= ConstantBufferStageCount || slot >= MaxResourceSlots) {
        return;
    }

    const TextureRecord* record = textures_.Find(handle.slot);
    const bool substitute = record == nullptr || record->writable;
    nullTextureBindings_ += substitute ? 1 : 0;
    boundTextures_[stageIndex][slot] = substitute ? white_.slot : handle.slot;
}

Result<void> GlesDevice::SetSampler(ShaderStage stage, uint32 slot,
                                    const SamplerDesc* sampler) noexcept {
    const uint32 stageIndex = ConstantBufferStageIndex(stage);
    if (!recording_ || stageIndex >= ConstantBufferStageCount || slot >= MaxResourceSlots ||
        (sampler != nullptr && !IsValidSamplerDesc(*sampler))) {
        (void)Fail("SetSampler needs a recording, a stage and slot in range and a valid sampler "
                   "description");
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (sampler == nullptr) {
        boundSamplers_[stageIndex][slot] = NoBoundSampler;
        return Result<void>::Success();
    }

    for (uint32 index = 0; index < samplerCacheCount_; ++index) {
        if (samplerCache_[index].desc == *sampler) {
            boundSamplers_[stageIndex][slot] = static_cast<uint16>(index);
            return Result<void>::Success();
        }
    }

    if (samplerCacheCount_ == SamplerCacheCapacity) {
        (void)Fail("SetSampler: the sampler cache of the device is full");
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    SamplerCacheEntry& entry = samplerCache_[samplerCacheCount_];
    entry.desc = *sampler;
    entry.filtering = CreateSampler(*sampler, true);
    entry.point = CreateSampler(*sampler, false);
    boundSamplers_[stageIndex][slot] = static_cast<uint16>(samplerCacheCount_++);
    return Result<void>::Success();
}

Result<void> GlesDevice::SetDrawConstants(Span<const uint8> bytes) noexcept {
    const PipelineRecord* pipeline = recording_ ? pipelines_.Find(boundPipeline_.slot) : nullptr;
    if (pipeline == nullptr || bytes.Size() != pipeline->drawConstantBytes) {
        return RejectDrawConstantSize(pipeline != nullptr, bytes.Size(),
                                      pipeline != nullptr ? pipeline->drawConstantBytes : 0u);
    }

    if (bytes.Size() != 0) {
        platform::MemoryCopy(constants_, bytes.Data(), bytes.Size());
        constantsDirty_ = true;
    }

    return Result<void>::Success();
}

void GlesDevice::ApplyVertexBuffer(uint32 stream) noexcept {
    // 묶인 정점 버퍼 하나를 지금 파이프라인의 정점 배열(묶여 있는 VAO)에 건다.
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
    const BufferRecord* buffer = buffers_.Find(boundVertex_[stream].buffer.slot);
    if (pipeline == nullptr || buffer == nullptr) {
        return;
    }

    gl_.BindVertexBuffer(stream, buffer->buffer, static_cast<GLintptr>(boundVertex_[stream].offset),
                         static_cast<GLsizei>(pipeline->strides[stream]));
}

void GlesDevice::ApplyIndexBuffer() noexcept {
    // GL_ELEMENT_ARRAY_BUFFER binding은 묶인 정점 배열의 상태다.
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
    const BufferRecord* buffer = buffers_.Find(boundIndex_.buffer.slot);
    if (pipeline != nullptr) {
        gl_.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer != nullptr ? buffer->buffer : 0u);
    }
}

const PipelineRecord* GlesDevice::DrawablePipeline() const noexcept {
    // 묶인 파이프라인이 선언한 stream에 모두 유효한 버퍼가 있는지다.
    const PipelineRecord* pipeline = pipelines_.Find(boundPipeline_.slot);
    if (pipeline == nullptr) {
        return nullptr;
    }

    for (uint32 stream = 0; stream < MaxVertexStreams; ++stream) {
        if ((pipeline->streamMask & (1u << stream)) == 0) {
            continue;
        }

        const BufferRecord* buffer = buffers_.Find(boundVertex_[stream].buffer.slot);
        if (buffer == nullptr || boundVertex_[stream].offset >= buffer->size) {
            return nullptr;
        }
    }

    return BufferViewsBound(*pipeline) ? pipeline : nullptr;
}

void GlesDevice::ApplyConstants(const PipelineRecord& pipeline) noexcept {
    if (!constantsDirty_) {
        return;
    }

    // 그리기별 상수 byte를 복사 표대로 타입별 vec4 배열에 올린다(루트 상수 에뮬레이션,
    // ShaderSystem 7.2절 4항·SSD42). 행렬은 행 우선 바이트 그대로이며 쿠킹된 ESSL이
    // mat4(a[k..k+3])로 읽어(열 우선 → Mᵀ) Mᵀ * v = v·M으로 쓴다. 전치하지 않는다(7.2절 6항).
    for (uint32 index = 0; index < pipeline.rootCopyCount; ++index) {
        const GlesRootCopy& copy = pipeline.rootCopies[index];
        const uint8* source = constants_ + copy.sourceOffset;
        switch (copy.type) {
        case shader::ShaderGlesUniformType::Float:
            gl_.Uniform4fv(copy.location, copy.vectors, reinterpret_cast<const GLfloat*>(source));
            break;
        case shader::ShaderGlesUniformType::Int:
            gl_.Uniform4iv(copy.location, copy.vectors, reinterpret_cast<const GLint*>(source));
            break;
        case shader::ShaderGlesUniformType::Uint:
            gl_.Uniform4uiv(copy.location, copy.vectors, reinterpret_cast<const GLuint*>(source));
            break;
        }
    }

    constantsDirty_ = false;
}

// OpenGL ES는 드라이버가 자원 상태를 암묵으로 관리하므로 명시 전이를 낮출 곳이 없다(RenderBase GX5,
// UE OpenGL RHI도 전이 함수가 비어 있다). compute의 image·storage buffer 쓰기 뒤 읽기에 필요한
// glMemoryBarrier는 dispatch마다 모든 bit로 낸다(GlesDeviceCommands.cpp).
void GlesDevice::Transition(Span<const ResourceTransition> transitions) noexcept {
    (void)transitions;
}

SplitTransition GlesDevice::BeginTransitions(Span<const ResourceTransition> transitions) noexcept {
    if (!recording_) {
        return {};
    }

    return splits_.Begin(transitions);
}

void GlesDevice::EndTransitions(SplitTransition split,
                                Span<const ResourceTransition> transitions) noexcept {
    if (!recording_) {
        return;
    }

    const SplitEndResult ended = splits_.End(split, transitions);
    if (ended == SplitEndResult::Unknown) {
        ++ignoredDraws_;
    } else if (ended == SplitEndResult::Mismatch && validation_) {
        ++validationErrors_;
    }
}

Result<void> GlesDevice::CopyPassTarget(TextureHandle destination) noexcept {
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord* record = textures_.Find(destination.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    // 패스 대상은 2D mip 0 attachment여야 한다(텍스처 GX2 C부).
    const TextureRecord* passTexture = textures_.Find(pass_.texture.slot);
    // 창 패스(pass_.texture가 null)는 중간 2D 텍스처다.
    const bool wholeTarget =
        passTexture == nullptr || (passTexture->framebuffer != 0 && pass_.firstColorMip == 0);
    if (pass_.colorCount == 0 || record->framebuffer == 0 || record->sampleCount > 1 ||
        pass_.sampleCount > 1 || !wholeTarget || destination == pass_.texture ||
        record->width != pass_.width || record->height != pass_.height ||
        record->format != pass_.format) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // blit은 scissor를 따르므로 복사 동안 끈다(패스 scissor 사각형은 그대로 남는다).
    gl_.Disable(GL_SCISSOR_TEST);
    const GLint width = static_cast<GLint>(pass_.width);
    const GLint height = static_cast<GLint>(pass_.height);
    GLint sourceWidth = width;
    GLint sourceHeight = height;
    bool window = false;
    if (!pass_.swapChain.IsNull()) {
        // 창 경로: present와 같은 blit으로 창의 기본 framebuffer에 옮긴 뒤 그것을 읽는다. 그래서
        // 창에 실제로 표시될 byte(방향 포함)가 offscreen 경로와 같은지 시험할 수 있다. 기본
        // framebuffer는 저장소 첫 행이 화면 아래쪽이므로 읽을 때 다시 뒤집는다.
        const SwapChainRecord* swapChain = swapChains_.Find(pass_.swapChain.slot);
        if (swapChain == nullptr) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        if (!PresentBlit(*swapChain)) {
            return Fail("present blit for the pass copy failed");
        }

        uint32 extentWidth = 0;
        uint32 extentHeight = 0;
        if (platform_->SurfaceExtent(swapChain->surface, extentWidth, extentHeight)) {
            sourceWidth = static_cast<GLint>(extentWidth);
            sourceHeight = static_cast<GLint>(extentHeight);
        }

        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        gl_.ReadBuffer(GL_BACK);
        window = true;
    } else {
        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, pass_.framebuffer);
        gl_.ReadBuffer(GL_COLOR_ATTACHMENT0);
    }

    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, record->framebuffer);
    if (rawSrgbBlit_) {
        gl_.Disable(GL_FRAMEBUFFER_SRGB);
    }

    gl_.BlitFramebuffer(0, 0, sourceWidth, sourceHeight, 0, window ? height : 0, width,
                        window ? 0 : height, GL_COLOR_BUFFER_BIT,
                        sourceWidth == width && sourceHeight == height ? GL_NEAREST : GL_LINEAR);
    if (rawSrgbBlit_) {
        gl_.Enable(GL_FRAMEBUFFER_SRGB);
    }

    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, pass_.framebuffer);
    gl_.Enable(GL_SCISSOR_TEST);
    return Result<void>::Success();
}

Result<uint64> GlesDevice::WriteTimestamp() noexcept {
    if (!timestampsReady_) {
        return Result<uint64>::Failure(ErrorCode::Unsupported);
    }

    if (!recording_) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    timestamps_.Harvest(timeline_.Completed(), timestampSource_);
    auto write = timestamps_.Allocate();
    if (!write) {
        return Result<uint64>::Failure(write.Error());
    }

    timer_.QueryCounter(timestampQueries_[write.Value().index], GL_TIMESTAMP);
    return Result<uint64>::Success(write.Value().query);
}

void GlesDevice::PushMarker(StringView name) noexcept {
    if (!recording_) {
        return;
    }

    const StringView clipped = ClipDebugMarkerName(name);
    gl_.PushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, static_cast<GLsizei>(clipped.ByteCount()),
                       clipped.Data());
    ++markerDepth_;
}

void GlesDevice::PopMarker() noexcept {
    if (!recording_ || markerDepth_ == 0) {
        ++ignoredDraws_;
        return;
    }

    gl_.PopDebugGroup();
    --markerDepth_;
}

void GlesDevice::Draw(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept {
    const PipelineRecord* pipeline = PrepareDraw(false);
    if (pipeline == nullptr) {
        ++ignoredDraws_;
        return;
    }

    // base instance는 0으로 고정한다(RenderBase 10.4절).
    gl_.DrawArraysInstanced(pipeline->topology, static_cast<GLint>(firstVertex),
                            static_cast<GLsizei>(vertexCount), static_cast<GLsizei>(instanceCount));
}

void GlesDevice::DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept {
    const PipelineRecord* pipeline = PrepareDraw(true);
    if (pipeline == nullptr) {
        ++ignoredDraws_;
        return;
    }

    const uint64 start =
        boundIndex_.offset + static_cast<uint64>(firstIndex) * IndexFormatSize(boundIndex_.format);
    // base vertex·base instance는 0으로 고정한다.
    gl_.DrawElementsInstanced(pipeline->topology, static_cast<GLsizei>(indexCount),
                              boundIndex_.format == IndexFormat::UInt16 ? GL_UNSIGNED_SHORT
                                                                        : GL_UNSIGNED_INT,
                              reinterpret_cast<const void*>(static_cast<uptr>(start)),
                              static_cast<GLsizei>(instanceCount));
}

} // namespace rocks::graphics::gles
