#include "GlesDevice.h"

#include <Base/Compiler.h>

// 프레임 기록·CommandContext·present다(GlesDevice.h).
namespace rocks::graphics::gles {

static_assert(ROCKS_OFFSET_OF(DrawConstants, color) == 16 * sizeof(float),
              "DrawConstants must be transform[16] followed by color[4] without padding");

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
    pass_ = {
        record->framebuffer, record->width, record->height, Format::RGBA8UnormSrgb, {}, handle};
    StartPass(record->framebuffer, record->width, record->height, clear);
    return Result<CommandContext*>::Success(this);
}

Result<CommandContext*> GlesDevice::BeginOffscreen(TextureHandle handle,
                                                   const ClearColor& clear) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<CommandContext*>::Failure(owner.Error());
    }

    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr || record->framebuffer == 0) {
        return Result<CommandContext*>::Failure(record == nullptr ? ErrorCode::StaleHandle
                                                                  : ErrorCode::InvalidArgument);
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

    presentTarget_ = {};
    pass_ = {record->framebuffer, record->width, record->height, record->format, handle, {}};
    StartPass(record->framebuffer, record->width, record->height, clear);
    return Result<CommandContext*>::Success(this);
}

Result<GpuFencePoint> GlesDevice::EndFrame() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<GpuFencePoint>::Failure(owner.Error());
    }

    if (!recording_) {
        return Result<GpuFencePoint>::Failure(ErrorCode::InvalidArgument);
    }

    recording_ = false;
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
                outcome = Fail("present blit failed");
            } else if (!platform_->Present(record->surface, record->vsync, error)) {
                outcome = Reject(error, ErrorCode::DependencyFailed);
            }
        }
    }

    gl_.BindFramebuffer(GL_FRAMEBUFFER, 0);
    const uint64 value = Submit();
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

    const GlesTextureFormat* format = FindTextureFormat(record->format);
    const usize rowBytes = static_cast<usize>(record->width) * BytesPerPixel(record->format);
    if (record->framebuffer == 0 || out.Size() != rowBytes * record->height || recording_) {
        return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
    }

    if (!MakeCurrentForResources()) {
        return Fail("cannot make the OpenGL context current");
    }

    // RenderTarget 저장소의 첫 행이 이미지 위쪽이므로 뒤집지 않고 읽는다. sRGB 형식도 저장된
    // 값(인코딩된 byte)을 그대로 읽는다.
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, record->framebuffer);
    gl_.ReadBuffer(GL_COLOR_ATTACHMENT0);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl_.ReadPixels(0, 0, static_cast<GLsizei>(record->width), static_cast<GLsizei>(record->height),
                   format->externalFormat, format->externalType, out.Data());
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return Result<void>::Success();
}

void GlesDevice::StartPass(GLuint framebuffer, uint32 width, uint32 height,
                           const ClearColor& clear) noexcept {
    recording_ = true;
    markerDepth_ = 0;
    boundPipeline_ = {};
    for (BoundVertexBuffer& bound : boundVertex_) {
        bound = {};
    }

    boundIndex_ = {};
    constantsDirty_ = true;
    constants_ = DrawConstants{};
    gl_.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl_.Viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    gl_.Disable(GL_BLEND);
    gl_.Disable(GL_CULL_FACE);
    gl_.ClearColor(clear.r, clear.g, clear.b, clear.a);
    gl_.Clear(GL_COLOR_BUFFER_BIT);
    const TextureRecord* white = textures_.Find(white_.slot);
    if (white != nullptr) {
        BindPassTexture(*white);
    }
}

void GlesDevice::BindPassTexture(const TextureRecord& record) noexcept {
    gl_.ActiveTexture(GL_TEXTURE0 + PixelTextureUnit);
    gl_.BindTexture(GL_TEXTURE_2D, record.texture);
    gl_.BindSampler(PixelTextureUnit, record.sampler);
}

bool GlesDevice::PresentBlit(const SwapChainRecord& record) noexcept {
    if (!platform_->MakeCurrentOnSurface(record.surface)) {
        return false;
    }

    uint32 width = record.width;
    uint32 height = record.height;
    if (!platform_->SurfaceExtent(record.surface, width, height)) {
        width = record.width;
        height = record.height;
    }

    // 텍스처 첫 행(이미지 위쪽)을 창 위쪽에 두도록 대상 y를 뒤집는다(기본 framebuffer는 아래쪽이
    // 원점). 창 크기가 다르면 늘려 그린다(D3D12 스왑체인의 표시 확대와 같다). sRGB byte는 변환 없이
    // 옮긴다(아래).
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, record.framebuffer);
    gl_.ReadBuffer(GL_COLOR_ATTACHMENT0);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // sRGB 쓰기 변환을 끌 수 있으면 끄고 옮긴다(UE도 표시 blit에서 GL_FRAMEBUFFER_SRGB를 끈다,
    // WindowsOpenGLPlatform.cpp:505–517).
    if (features_.srgbWriteControl) {
        gl_.Disable(GL_FRAMEBUFFER_SRGB);
    }

    const bool sameSize = width == record.width && height == record.height;
    gl_.BlitFramebuffer(0, 0, static_cast<GLint>(record.width), static_cast<GLint>(record.height),
                        0, static_cast<GLint>(height), static_cast<GLint>(width), 0,
                        GL_COLOR_BUFFER_BIT, sameSize ? GL_NEAREST : GL_LINEAR);
    if (features_.srgbWriteControl) {
        gl_.Enable(GL_FRAMEBUFFER_SRGB);
    }

    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    // 기록 중(CopyPassTarget)이면 패스 대상을 다시 그리기 framebuffer로 둔다.
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, recording_ ? pass_.framebuffer : 0u);
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
    gl_.BindVertexArray(pipeline->vertexArray);
    if (pipeline->alphaBlend) {
        gl_.Enable(GL_BLEND);
        gl_.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        gl_.Disable(GL_BLEND);
    }

    // 앞면 감김(glFrontFace)은 장치가 클립 공간 규약에 맞춰 한 번 정했다.
    if (pipeline->cullMode == CullMode::None) {
        gl_.Disable(GL_CULL_FACE);
    } else {
        gl_.Enable(GL_CULL_FACE);
        gl_.CullFace(pipeline->cullMode == CullMode::Back ? GL_BACK : GL_FRONT);
    }

    // 프로그램마다 uniform 값이 따로이므로 새 프로그램에 상수를 다시 올린다.
    constantsDirty_ = true;
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

void GlesDevice::SetTexture(TextureHandle handle) noexcept {
    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr) {
        record = textures_.Find(white_.slot);
    }

    if (record != nullptr) {
        BindPassTexture(*record);
    }
}

void GlesDevice::SetConstants(const DrawConstants& constants) noexcept {
    constants_ = constants;
    constantsDirty_ = true;
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

    return pipeline;
}

void GlesDevice::ApplyConstants(const PipelineRecord& pipeline) noexcept {
    if (!constantsDirty_) {
        return;
    }

    // DrawConstants 80 byte를 vec4 5개로 그대로 올린다(루트 상수 에뮬레이션, ShaderSystem 7.2절
    // 4항). transform은 행 우선 바이트 그대로이며 셰이더가 mat4(vu_h[0..3])로 읽어(열 우선 → Mᵀ)
    // Mᵀ * v = v·M으로 쓴다. 전치하지 않는다(7.2절 6항).
    const auto* vectors = reinterpret_cast<const GLfloat*>(&constants_);
    if (pipeline.vertexConstants >= 0) {
        gl_.Uniform4fv(pipeline.vertexConstants, RootConstantVectors, vectors);
    }

    if (pipeline.pixelConstants >= 0) {
        gl_.Uniform4fv(pipeline.pixelConstants, RootConstantVectors, vectors);
    }

    constantsDirty_ = false;
}

Result<void> GlesDevice::CopyPassTarget(TextureHandle destination) noexcept {
    if (!recording_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TextureRecord* record = textures_.Find(destination.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (record->framebuffer == 0 || destination == pass_.texture || record->width != pass_.width ||
        record->height != pass_.height || record->format != pass_.format) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

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
    if (features_.srgbWriteControl) {
        gl_.Disable(GL_FRAMEBUFFER_SRGB);
    }

    gl_.BlitFramebuffer(0, 0, sourceWidth, sourceHeight, 0, window ? height : 0, width,
                        window ? 0 : height, GL_COLOR_BUFFER_BIT,
                        sourceWidth == width && sourceHeight == height ? GL_NEAREST : GL_LINEAR);
    if (features_.srgbWriteControl) {
        gl_.Enable(GL_FRAMEBUFFER_SRGB);
    }

    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, pass_.framebuffer);
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
    const PipelineRecord* pipeline = DrawablePipeline();
    if (pipeline == nullptr) {
        ++ignoredDraws_;
        return;
    }

    ApplyConstants(*pipeline);
    // base instance는 0으로 고정한다(RenderBase 10.4절).
    gl_.DrawArraysInstanced(GL_TRIANGLES, static_cast<GLint>(firstVertex),
                            static_cast<GLsizei>(vertexCount), static_cast<GLsizei>(instanceCount));
}

void GlesDevice::DrawIndexed(uint32 indexCount, uint32 instanceCount, uint32 firstIndex) noexcept {
    const PipelineRecord* pipeline = DrawablePipeline();
    const BufferRecord* index = buffers_.Find(boundIndex_.buffer.slot);
    if (pipeline == nullptr || index == nullptr || boundIndex_.offset >= index->size) {
        ++ignoredDraws_;
        return;
    }

    ApplyConstants(*pipeline);
    const uint64 start =
        boundIndex_.offset + static_cast<uint64>(firstIndex) * IndexFormatSize(boundIndex_.format);
    // base vertex·base instance는 0으로 고정한다.
    gl_.DrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(indexCount),
                              boundIndex_.format == IndexFormat::UInt16 ? GL_UNSIGNED_SHORT
                                                                        : GL_UNSIGNED_INT,
                              reinterpret_cast<const void*>(static_cast<uptr>(start)),
                              static_cast<GLsizei>(instanceCount));
}

} // namespace rocks::graphics::gles
