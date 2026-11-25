#include "GlesDevice.h"

#include <Platform/Memory.h>
#include <ShaderCore/ShaderFormatTypes.h>

// 장치 능력(RenderBase 4.4절 GX1)과 버퍼 view·ReadBuffer(GX2)다. typed view는 버퍼 텍스처
// (GL_TEXTURE_BUFFER, ES 3.2 core)라 텍스처 유닛을 차지하고(ShaderSystem 7.2절 4항 RQ10),
// 구조화·raw view는 storage block binding이다.
namespace rocks::graphics::gles {

namespace {

uint32 Query(const GlesFunctions& gl, GLenum name) noexcept {
    GLint value = 0;
    gl.GetIntegerv(name, &value);
    return value > 0 ? static_cast<uint32>(value) : 0u;
}

uint64 Query64(const GlesFunctions& gl, GLenum name) noexcept {
    GLint64 value = 0;
    gl.GetInteger64v(name, &value);
    return value > 0 ? static_cast<uint64>(value) : 0u;
}

uint32 Smaller(uint32 left, uint32 right) noexcept {
    return left < right ? left : right;
}

} // namespace

void GlesDevice::QueryDeviceCaps(bool desktopContext) noexcept {
    DeviceLimits& limits = caps_.limits;
    limits.vertex.constantBuffers = Query(gl_, GL_MAX_VERTEX_UNIFORM_BLOCKS);
    limits.vertex.sampledTextures = Query(gl_, GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS);
    limits.vertex.storageBuffers = Query(gl_, GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS);
    limits.vertex.storageImages = Query(gl_, GL_MAX_VERTEX_IMAGE_UNIFORMS);
    limits.vertex.uniformVectors = Query(gl_, GL_MAX_VERTEX_UNIFORM_VECTORS);
    limits.pixel.constantBuffers = Query(gl_, GL_MAX_FRAGMENT_UNIFORM_BLOCKS);
    limits.pixel.sampledTextures = Query(gl_, GL_MAX_TEXTURE_IMAGE_UNITS);
    limits.pixel.storageBuffers = Query(gl_, GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS);
    limits.pixel.storageImages = Query(gl_, GL_MAX_FRAGMENT_IMAGE_UNIFORMS);
    limits.pixel.uniformVectors = Query(gl_, GL_MAX_FRAGMENT_UNIFORM_VECTORS);
    limits.compute.constantBuffers = Query(gl_, GL_MAX_COMPUTE_UNIFORM_BLOCKS);
    limits.compute.sampledTextures = Query(gl_, GL_MAX_COMPUTE_TEXTURE_IMAGE_UNITS);
    limits.compute.storageBuffers = Query(gl_, GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS);
    limits.compute.storageImages = Query(gl_, GL_MAX_COMPUTE_IMAGE_UNIFORMS);
    limits.compute.uniformVectors = Query(gl_, GL_MAX_COMPUTE_UNIFORM_COMPONENTS) / 4;
    // GLSL ES는 텍스처와 샘플러가 합쳐진 sampler라 샘플러 한도가 텍스처 유닛 한도다.
    limits.vertex.samplers = limits.vertex.sampledTextures;
    limits.pixel.samplers = limits.pixel.sampledTextures;
    limits.compute.samplers = limits.compute.sampledTextures;
    limits.pipelineSampledTextures = Query(gl_, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
    limits.pipelineSamplers = limits.pipelineSampledTextures;
    limits.pipelineStorageBuffers = Query(gl_, GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS);
    limits.pipelineStorageImages = Query(gl_, GL_MAX_COMBINED_IMAGE_UNIFORMS);
    limits.perStageResources = NotApplicableLimit;
    limits.pipelineUavs = NotApplicableLimit;
    limits.boundDescriptorSets = NotApplicableLimit;
    // viewport array는 ES core가 아니다(GL_NV_viewport_array는 쓰지 않는다).
    limits.viewports = 1;
    limits.colorAttachments =
        Smaller(Query(gl_, GL_MAX_DRAW_BUFFERS), Query(gl_, GL_MAX_COLOR_ATTACHMENTS));
    const uint64 uniformBlock = Query64(gl_, GL_MAX_UNIFORM_BLOCK_SIZE);
    limits.constantBufferBytes = uniformBlock > NumericLimits<uint32>::Max()
                                     ? NumericLimits<uint32>::Max()
                                     : static_cast<uint32>(uniformBlock);
    // 그리기 상수는 uniform 에뮬레이션이라 Rocks 루트 상수 계약(128 byte)이다.
    limits.rootConstantBytes = RootConstantContractBytes;
    limits.constantBufferOffsetAlignment = Query(gl_, GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT);
    limits.storageBufferOffsetAlignment = Query(gl_, GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT);
    limits.texelBufferOffsetAlignment = Query(gl_, GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT);
    limits.storageBufferBytes = Query64(gl_, GL_MAX_SHADER_STORAGE_BLOCK_SIZE);
    limits.texelBufferElements = Query(gl_, GL_MAX_TEXTURE_BUFFER_SIZE);
    // GL에는 버퍼 최대 크기 질의가 없다. 보수적 값을 공개한다(가짜로 넓히지 않는다).
    limits.bufferBytes = ConservativeBufferBytes;
    limits.vertexAttributes = Query(gl_, GL_MAX_VERTEX_ATTRIBS);
    limits.vertexStreams = Query(gl_, GL_MAX_VERTEX_ATTRIB_BINDINGS);
    limits.interStageVectors = Query(gl_, GL_MAX_VARYING_VECTORS);
    for (GLuint axis = 0; axis < 3; ++axis) {
        GLint count = 0;
        GLint size = 0;
        gl_.GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, axis, &count);
        gl_.GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, axis, &size);
        limits.computeGroupCount[axis] = count > 0 ? static_cast<uint32>(count) : 0u;
        limits.computeGroupSize[axis] = size > 0 ? static_cast<uint32>(size) : 0u;
    }

    limits.computeGroupInvocations = Query(gl_, GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS);
    limits.computeSharedMemoryBytes = Query(gl_, GL_MAX_COMPUTE_SHARED_MEMORY_SIZE);
    limits.textureSize2D = Query(gl_, GL_MAX_TEXTURE_SIZE);
    limits.textureSize3D = Query(gl_, GL_MAX_3D_TEXTURE_SIZE);
    limits.textureSizeCube = Query(gl_, GL_MAX_CUBE_MAP_TEXTURE_SIZE);
    limits.textureArrayLayers = Query(gl_, GL_MAX_ARRAY_TEXTURE_LAYERS);
    limits.samplerAnisotropy =
        features_.anisotropic ? static_cast<uint32>(features_.maxAnisotropy) : 1u;

    DeviceFeatures& features = caps_.features;
    features.computeShaders = true;
    features.depthClamp = features_.depthClamp;
    // 깊이 bias 상한은 확장 함수를 얻은 뒤 Initialize가 정한다(GL_EXT_polygon_offset_clamp).
    features.depthBiasClamp = false;
    // alpha-to-coverage와 타깃별 블렌드(glBlendFuncSeparatei 등)는 ES 3.2 core다.
    features.alphaToCoverage = true;
    features.independentBlend = true;
    // ES에는 VS 층 출력 확장이 없다(데스크톱 context의 ARB 확장은 ES 판정에 쓰지 않는다).
    features.vertexViewportLayerOutput = false;
    features.atomic64 = false;
    // ES 3.2 간접 그리기 인자의 base instance는 0이어야 하고 multi draw indirect는 core가 아니다.
    features.indirectFirstInstance = false;
    features.multiDrawIndirect = false;
    features.textureCubeArray = true;
    // 채널별 TEXTURE_SWIZZLE_R/G/B/A는 ES 3.0 core다. AnisotropicPoint는 LINEAR_MIPMAP_NEAREST에
    // 이방성(GL_EXT_texture_filter_anisotropic, 없으면 배율 1)을 더한다.
    features.textureViewSwizzle = true;
    // texture view는 ES 3.2 core 밖이다. 확장(GL_OES_texture_view·GL_EXT_texture_view)이 있으면
    // Initialize가 참으로 바꾼다. 없으면 읽기 view는 텍스처 전체를 묶는다(TextureViewBaseMip).
    features.textureViewRanges = false;
    features.anisotropicPointFilter = true;
    features.srgbPairView = false;
    features.vertexStores = limits.vertex.storageBuffers != 0 || limits.vertex.storageImages != 0;
    features.pixelStores = limits.pixel.storageBuffers != 0 || limits.pixel.storageImages != 0;
    features.hdrOutput = false;
    features.resourceBindingTier = 0;
    features.shaderModel = 0;
    apiVersion_ = Query(gl_, GL_MAJOR_VERSION) * 100 + Query(gl_, GL_MINOR_VERSION) * 10;
    // 형식 장치 요구 비트다(DeviceFeatures::shaderDeviceRequirements). 데스크톱 core context는
    // ARB_ES3_2_compatibility·ARB_clip_control이 있어야 만들어진다(WglPlatform).
    constexpr uint32 DesktopGl4_3 = 430;
    constexpr uint32 Es3_2 = 320;
    uint32 requirements = 0;
    if (desktopContext) {
        requirements = shader::DeviceRequiresGlArbEs32Compatibility |
                       shader::DeviceRequiresGlArbClipControl |
                       (apiVersion_ >= DesktopGl4_3 ? shader::DeviceRequiresOpenGL4_3 : 0u);
    } else if (apiVersion_ >= Es3_2) {
        requirements = shader::DeviceRequiresOpenGLES3_2;
    }

    features.shaderDeviceRequirements = requirements;

    // MSAA 표본 수는 렌더 타깃·깊이 형식마다 glGetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE)다
    // (ES 3.1 core, 정수 형식의 한도는 구현이 반영한다).
    for (uint32 number = 1; number <= MaxPixelFormatNumber; ++number) {
        const auto pixelFormat = static_cast<PixelFormat>(number);
        const PixelFormatCaps formatCaps = formatCaps_[number];
        if (!HasPixelFormatCaps(formatCaps, PixelFormatCaps::RenderTarget) &&
            !HasPixelFormatCaps(formatCaps, PixelFormatCaps::DepthStencil)) {
            continue;
        }

        const GlesTextureFormat* entry = FindTextureFormat(pixelFormat);
        if (entry == nullptr) {
            continue;
        }

        uint32 counts = SampleCountBit(1);
        GLint countCount = 0;
        gl_.GetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE, entry->internalFormat,
                                GL_NUM_SAMPLE_COUNTS, 1, &countCount);
        constexpr GLsizei MaxQueried = 16;
        GLint samples[MaxQueried] = {};
        const GLsizei queried = countCount < MaxQueried ? countCount : MaxQueried;
        if (queried > 0) {
            gl_.GetInternalformativ(GL_TEXTURE_2D_MULTISAMPLE, entry->internalFormat, GL_SAMPLES,
                                    queried, samples);
        }

        for (GLsizei index = 0; index < queried; ++index) {
            const uint32 value = samples[index] > 0 ? static_cast<uint32>(samples[index]) : 0u;
            if (value > 1 && value <= MaxSampleCount && (value & (value - 1)) == 0) {
                counts |= SampleCountBit(value);
            }
        }

        sampleCounts_[number] = counts;
        if (counts > SampleCountBit(1)) {
            formatCaps_[number] = formatCaps | PixelFormatCaps::Multisample;
        }
    }

    // 질의가 남긴 오류(지원하지 않는 조합)는 버린다. debug output은 이 뒤에 켠다.
    while (gl_.GetError() != GL_NO_ERROR) {
    }
}

const DeviceCaps& GlesDevice::Caps() const noexcept {
    return caps_;
}

uint32 GlesDevice::GetPixelFormatSampleCounts(PixelFormat format) const noexcept {
    const uint32 number = PixelFormatNumber(format);
    return number <= MaxPixelFormatNumber ? sampleCounts_[number] : 0;
}

Result<BufferViewHandle> GlesDevice::CreateBufferView(BufferHandle handle,
                                                      const BufferViewDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<BufferViewHandle>::Failure(owner.Error());
    }

    BufferRecord* buffer = buffers_.Find(handle.slot);
    if (buffer == nullptr) {
        return Result<BufferViewHandle>::Failure(ErrorCode::StaleHandle);
    }

    auto resolved = ResolveBufferView(buffer->desc, desc);
    if (!resolved) {
        Diagnostic message;
        message.Text("buffer view breaks the view rules (kind, access, format or range)");
        auto rejected = Reject(message, resolved.Error());
        return Result<BufferViewHandle>::Failure(rejected.Error());
    }

    const ResolvedBufferView range = resolved.Value();
    auto limited =
        CheckBufferViewLimits(caps_.limits, GetPixelFormatCaps(desc.format), desc, range);
    if (!limited) {
        Diagnostic message;
        message.Text("buffer view needs a format capability, alignment or range the OpenGL ES "
                     "device does not provide");
        auto rejected = Reject(message, limited.Error());
        return Result<BufferViewHandle>::Failure(rejected.Error());
    }

    if (!MakeCurrentForResources()) {
        return FailWith<BufferViewHandle>("cannot make the OpenGL context current");
    }

    BufferViewRecord record;
    record.buffer = buffer->buffer;
    record.source = handle.slot;
    record.offset = range.offset;
    record.size = range.Bytes();
    record.format = desc.format;
    record.kind = desc.kind;
    record.access = desc.access;
    record.stride = desc.kind == BufferViewKind::Structured ? range.elementBytes : 0u;
    record.shaderType = desc.kind == BufferViewKind::Typed
                            ? GetPixelFormatInfo(desc.format).shaderType
                            : PixelShaderType::None;
    if (desc.kind == BufferViewKind::Typed) {
        const GlesTextureFormat* format = FindTextureFormat(desc.format);
        gl_.GenTextures(1, &record.texture);
        gl_.ActiveTexture(GL_TEXTURE0 + UploadTextureUnit);
        gl_.BindTexture(GL_TEXTURE_BUFFER, record.texture);
        gl_.TexBufferRange(GL_TEXTURE_BUFFER, format->internalFormat, buffer->buffer,
                           static_cast<GLintptr>(range.offset),
                           static_cast<GLsizeiptr>(range.Bytes()));
        gl_.BindTexture(GL_TEXTURE_BUFFER, 0);
    }

    auto added = bufferViews_.TryAdd(record);
    if (!added) {
        gl_.DeleteTextures(1, &record.texture);
        return Result<BufferViewHandle>::Failure(added.Error());
    }

    ++buffer->viewCount;
    return Result<BufferViewHandle>::Success(BufferViewHandle{added.Value()});
}

void GlesDevice::ReleaseViewSource(const BufferViewRecord& view) noexcept {
    BufferRecord* source = buffers_.Find(view.source);
    if (source != nullptr) {
        --source->viewCount;
        return;
    }

    for (usize index = 0; index < orphanBuffers_.Size(); ++index) {
        OrphanBuffer& orphan = orphanBuffers_[index];
        if (orphan.buffer != view.buffer) {
            continue;
        }

        if (--orphan.viewCount == 0) {
            RetiredObject entry;
            entry.kind = RetiredKind::Buffer;
            entry.objects[0] = orphan.buffer;
            Defer(entry);
            auto removed = orphanBuffers_.TryRemoveAtSwap(index);
            (void)removed;
        }

        return;
    }
}

void GlesDevice::Retire(BufferViewHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    const BufferViewRecord* record = bufferViews_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    if (record->texture != 0) {
        RetiredObject entry;
        entry.kind = RetiredKind::Texture;
        entry.objects[0] = record->texture;
        Defer(entry);
    }

    ReleaseViewSource(*record);
    auto removed = bufferViews_.TryRemove(handle.slot);
    (void)removed;
}

// map 읽기가 앞선 GPU 쓰기의 완료를 기다린다(GL 동기 규칙).
Result<void> GlesDevice::ReadBuffer(BufferHandle handle, uint64 offset, Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    const BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (out.Size() == 0 || offset > record->size || out.Size() > record->size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!MakeCurrentForResources()) {
        return Fail("cannot make the OpenGL context current");
    }

    gl_.BindBuffer(GL_COPY_READ_BUFFER, record->buffer);
    const void* mapped = gl_.MapBufferRange(GL_COPY_READ_BUFFER, static_cast<GLintptr>(offset),
                                            static_cast<GLsizeiptr>(out.Size()), GL_MAP_READ_BIT);
    Result<void> outcome = Result<void>::Success();
    if (mapped == nullptr) {
        outcome = Fail("glMapBufferRange (buffer readback) failed");
    } else {
        platform::MemoryCopy(out.Data(), mapped, out.Size());
        (void)gl_.UnmapBuffer(GL_COPY_READ_BUFFER);
    }

    gl_.BindBuffer(GL_COPY_READ_BUFFER, 0);
    return outcome;
}

void GlesDevice::SetBufferView(uint32 slot, BufferViewHandle handle) noexcept {
    if (!recording_ || slot >= MaxDrawBufferViews) {
        return;
    }

    // 그래픽 단계에는 UAV가 없으므로 ReadWrite view와 stale handle은 slot을 비운다.
    const BufferViewRecord* view = bufferViews_.Find(handle.slot);
    const bool readable = view != nullptr && view->access == ViewAccess::Read;
    boundViews_[slot] = readable ? handle : BufferViewHandle{};
}

bool GlesDevice::BufferViewsBound(const PipelineRecord& pipeline) const noexcept {
    BoundBufferViewInfo bound[MaxDrawBufferViews] = {};
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        const BufferViewRecord* view = bufferViews_.Find(boundViews_[slot].slot);
        if (view != nullptr) {
            bound[slot] = {true, view->kind, view->shaderType, view->stride};
        }
    }

    return DrawBufferSlotsSatisfied(pipeline.bufferSlots, bound);
}

void GlesDevice::ApplySlots(const PipelineRecord& pipeline) noexcept {
    for (uint32 index = 0; index < pipeline.slots.count; ++index) {
        const PipelineSlotBinding& binding = pipeline.slots.bindings[index];
        const GLuint location = binding.location;
        switch (binding.role) {
        case PipelineSlotRole::Texture: {
            const TextureRecord* texture = textures_.Find(boundTexture_);
            if (texture == nullptr || texture->writable) {
                texture = textures_.Find(white_.slot);
            }

            if (texture != nullptr) {
                BindPassTexture(*texture, location);
            }

            break;
        }
        case PipelineSlotRole::Sampler:
            // GLES 샘플러는 텍스처 유닛의 샘플러 객체다(BindPassTexture).
            break;
        case PipelineSlotRole::BufferView: {
            const BufferViewRecord* view = bufferViews_.Find(boundViews_[binding.slot].slot);
            if (view == nullptr) {
                break;
            }

            if (view->kind == BufferViewKind::Typed) {
                gl_.ActiveTexture(GL_TEXTURE0 + location);
                gl_.BindTexture(GL_TEXTURE_BUFFER, view->texture);
            } else {
                gl_.BindBufferRange(GL_SHADER_STORAGE_BUFFER, location, view->buffer,
                                    static_cast<GLintptr>(view->offset),
                                    static_cast<GLsizeiptr>(view->size));
            }

            break;
        }
        case PipelineSlotRole::WritableBuffer: {
            const BufferViewRecord* view = bufferViews_.Find(boundWriteViews_[binding.slot].slot);
            if (view == nullptr) {
                break;
            }

            if (view->kind == BufferViewKind::Typed) {
                const GlesTextureFormat* format = FindTextureFormat(view->format);
                gl_.BindImageTexture(location, view->texture, 0, GL_FALSE, 0, GL_READ_WRITE,
                                     format->internalFormat);
            } else {
                gl_.BindBufferRange(GL_SHADER_STORAGE_BUFFER, location, view->buffer,
                                    static_cast<GLintptr>(view->offset),
                                    static_cast<GLsizeiptr>(view->size));
            }

            break;
        }
        case PipelineSlotRole::WritableTexture: {
            // 쓰기 텍스처 view는 level 하나를 image unit에 묶는다(배열·cube·3D는 layered로 모든
            // 층, 층 하나만 고른 view는 그 층).
            const TextureRecord* view = textures_.Find(boundWriteTextures_[binding.slot].slot);
            if (view == nullptr) {
                break;
            }

            const GlesTextureFormat* format = FindTextureFormat(view->format);
            const bool layers = view->dimension == TextureDimension::Texture2DArray ||
                                view->dimension == TextureDimension::Texture3D ||
                                IsCubeDimension(view->dimension);
            const bool layered = layers && view->viewRange.sliceCount == view->slices;
            // imageCube·imageCubeArray(RWTextureCube, ShaderSystem 16.1절 SH2g)는 모든 면을 묶어야
            // 한다. 층 일부만 고른 view는 GLES가 그렇게 묶을 수 없으므로 비워 둔다(면 하나만 고른
            // view는 그 층의 2D 이미지라 RWTexture2D로 쓴다).
            const bool cubeImage = binding.shape == shader::ShaderResourceShape::TextureCube ||
                                   binding.shape == shader::ShaderResourceShape::TextureCubeArray;
            if (cubeImage && !layered) {
                break;
            }

            gl_.BindImageTexture(location, view->texture,
                                 static_cast<GLint>(view->viewRange.firstMip),
                                 layered ? GL_TRUE : GL_FALSE,
                                 layered ? 0 : static_cast<GLint>(view->viewRange.firstSlice),
                                 GL_READ_WRITE, format->internalFormat);
            break;
        }
        }
    }
}

} // namespace rocks::graphics::gles
