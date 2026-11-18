#include "GlesDevice.h"

#include <Platform/Memory.h>

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

bool StartsWith(const char* text, const char* prefix) noexcept {
    while (*prefix != '\0') {
        if (*text++ != *prefix++) {
            return false;
        }
    }

    return true;
}

// 버퍼가 아닌 텍스처 sampler 타입인지다.
bool IsTextureSampler(GLenum type) noexcept {
    switch (type) {
    case GL_SAMPLER_2D:
    case GL_SAMPLER_3D:
    case GL_SAMPLER_CUBE:
    case GL_SAMPLER_2D_SHADOW:
    case GL_SAMPLER_2D_ARRAY:
    case GL_SAMPLER_2D_ARRAY_SHADOW:
    case GL_SAMPLER_CUBE_SHADOW:
    case GL_INT_SAMPLER_2D:
    case GL_INT_SAMPLER_3D:
    case GL_INT_SAMPLER_CUBE:
    case GL_INT_SAMPLER_2D_ARRAY:
    case GL_UNSIGNED_INT_SAMPLER_2D:
    case GL_UNSIGNED_INT_SAMPLER_3D:
    case GL_UNSIGNED_INT_SAMPLER_CUBE:
    case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
    case GL_SAMPLER_CUBE_MAP_ARRAY:
    case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
    case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
    case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
    case GL_SAMPLER_2D_MULTISAMPLE:
    case GL_INT_SAMPLER_2D_MULTISAMPLE:
    case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
    case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
    case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
    case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        return true;
    default:
        return false;
    }
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
    features.anisotropicPointFilter = true;
    features.srgbPairView = false;
    features.vertexStores = limits.vertex.storageBuffers != 0 || limits.vertex.storageImages != 0;
    features.pixelStores = limits.pixel.storageBuffers != 0 || limits.pixel.storageImages != 0;
    features.hdrOutput = false;
    features.resourceBindingTier = 0;
    features.shaderModel = 0;
    apiVersion_ = Query(gl_, GL_MAJOR_VERSION) * 100 + Query(gl_, GL_MINOR_VERSION) * 10;
    (void)desktopContext;

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

void GlesDevice::ApplyBufferViews(const PipelineRecord& pipeline) noexcept {
    for (uint32 slot = 0; slot < MaxDrawBufferViews; ++slot) {
        if ((pipeline.bufferSlots.usedMask & (1u << slot)) == 0) {
            continue;
        }

        const BufferViewRecord* view = bufferViews_.Find(boundViews_[slot].slot);
        if (view == nullptr) {
            continue;
        }

        if (view->kind == BufferViewKind::Typed) {
            // 같은 slot을 VS·PS가 각자 유닛으로 읽는다(ShaderSystem 7.2절 4항 단계별 유닛).
            gl_.ActiveTexture(GL_TEXTURE0 + PixelBufferViewUnitBase + slot);
            gl_.BindTexture(GL_TEXTURE_BUFFER, view->texture);
            gl_.ActiveTexture(GL_TEXTURE0 + VertexBufferViewUnitBase + slot);
            gl_.BindTexture(GL_TEXTURE_BUFFER, view->texture);
        } else {
            gl_.BindBufferRange(GL_SHADER_STORAGE_BUFFER, slot, view->buffer,
                                static_cast<GLintptr>(view->offset),
                                static_cast<GLsizeiptr>(view->size));
        }
    }
}

Result<void> GlesDevice::ReflectBufferSlots(GLuint program, DrawBufferSlots& slots,
                                            DrawBufferSlots* writeSlots) noexcept {
    GLint uniforms = 0;
    gl_.GetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniforms);
    for (GLint index = 0; index < uniforms; ++index) {
        GLchar name[128] = {};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        gl_.GetActiveUniform(program, static_cast<GLuint>(index), sizeof(name), &length, &size,
                             &type, name);
        // 그래픽 단계에는 쓰기 자원이 없다(image uniform 거부, compute는 image buffer만 받는다).
        // 텍스처 sampler는 PS 텍스처 유닛 0(ps0)만 받는다.
        const bool imageBuffer = type == GL_IMAGE_BUFFER || type == GL_INT_IMAGE_BUFFER ||
                                 type == GL_UNSIGNED_INT_IMAGE_BUFFER;
        if (type >= GL_FIRST_IMAGE_TYPE && type <= GL_LAST_IMAGE_TYPE &&
            !(imageBuffer && writeSlots != nullptr)) {
            Diagnostic message;
            message.Text("image uniform ")
                .Text(name)
                .Text(" is outside the fixed layout (no UAVs in graphics stages)");
            return Reject(message, ErrorCode::InvalidArgument);
        }

        GLint unit = -1;
        if (IsTextureSampler(type)) {
            gl_.GetUniformiv(program, gl_.GetUniformLocation(program, name), &unit);
            if (size != 1 || unit != static_cast<GLint>(PixelTextureUnit)) {
                Diagnostic message;
                message.Text("texture sampler ")
                    .Text(name)
                    .Text(" is outside the fixed layout (pixel texture unit 0)");
                return Reject(message, ErrorCode::InvalidArgument);
            }

            continue;
        }

        PixelShaderType shaderType = PixelShaderType::None;
        if (type == GL_SAMPLER_BUFFER || type == GL_IMAGE_BUFFER) {
            shaderType = PixelShaderType::Float;
        } else if (type == GL_INT_SAMPLER_BUFFER || type == GL_INT_IMAGE_BUFFER) {
            shaderType = PixelShaderType::Int;
        } else if (type == GL_UNSIGNED_INT_SAMPLER_BUFFER || type == GL_UNSIGNED_INT_IMAGE_BUFFER) {
            shaderType = PixelShaderType::Uint;
        } else {
            continue;
        }

        if (imageBuffer) {
            // compute 쓰기 typed view는 image unit slot이다(ComputePipelineDesc 고정 배치).
            gl_.GetUniformiv(program, gl_.GetUniformLocation(program, name), &unit);
            if (size != 1 || unit < 0 || static_cast<uint32>(unit) >= MaxDrawBufferViews ||
                !AddDrawBufferSlot(*writeSlots, static_cast<uint32>(unit), BufferViewKind::Typed,
                                   shaderType)) {
                Diagnostic message;
                message.Text("image buffer ")
                    .Text(name)
                    .Text(" is outside the fixed layout (compute image units 0-3)");
                return Reject(message, ErrorCode::InvalidArgument);
            }

            continue;
        }

        gl_.GetUniformiv(program, gl_.GetUniformLocation(program, name), &unit);
        const GLuint value = unit >= 0 ? static_cast<GLuint>(unit) : 0u;
        uint32 slot = MaxDrawBufferViews;
        if (value >= PixelBufferViewUnitBase &&
            value < PixelBufferViewUnitBase + MaxDrawBufferViews) {
            slot = value - PixelBufferViewUnitBase;
        } else if (value >= VertexBufferViewUnitBase &&
                   value < VertexBufferViewUnitBase + MaxDrawBufferViews) {
            slot = value - VertexBufferViewUnitBase;
        }

        if (size != 1 || slot == MaxDrawBufferViews ||
            !AddDrawBufferSlot(slots, slot, BufferViewKind::Typed, shaderType)) {
            Diagnostic message;
            message.Text("buffer sampler ")
                .Text(name)
                .Text(" is outside the fixed layout (pixel units 1-4, vertex units 17-20)");
            return Reject(message, ErrorCode::InvalidArgument);
        }
    }

    GLint blocks = 0;
    gl_.GetProgramInterfaceiv(program, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &blocks);
    for (GLint index = 0; index < blocks; ++index) {
        const GLenum property = GL_BUFFER_BINDING;
        GLint binding = -1;
        gl_.GetProgramResourceiv(program, GL_SHADER_STORAGE_BLOCK, static_cast<GLuint>(index), 1,
                                 &property, 1, nullptr, &binding);
        GLchar name[128] = {};
        gl_.GetProgramResourceName(program, GL_SHADER_STORAGE_BLOCK, static_cast<GLuint>(index),
                                   sizeof(name), nullptr, name);
        const BufferViewKind kind = StartsWith(name, RawStorageBlockPrefix) ||
                                            StartsWith(name, RawWritableStorageBlockPrefix)
                                        ? BufferViewKind::Raw
                                        : BufferViewKind::Structured;
        // 구조화 block의 원소 byte는 첫 buffer 변수의 최상위 배열 stride다(모르면 0).
        uint32 stride = 0;
        const GLenum countProperty = GL_NUM_ACTIVE_VARIABLES;
        GLint variables = 0;
        gl_.GetProgramResourceiv(program, GL_SHADER_STORAGE_BLOCK, static_cast<GLuint>(index), 1,
                                 &countProperty, 1, nullptr, &variables);
        if (kind == BufferViewKind::Structured && variables > 0) {
            const GLenum listProperty = GL_ACTIVE_VARIABLES;
            GLint variable = -1;
            gl_.GetProgramResourceiv(program, GL_SHADER_STORAGE_BLOCK, static_cast<GLuint>(index),
                                     1, &listProperty, 1, nullptr, &variable);
            const GLenum strideProperty = GL_TOP_LEVEL_ARRAY_STRIDE;
            GLint arrayStride = 0;
            if (variable >= 0) {
                gl_.GetProgramResourceiv(program, GL_BUFFER_VARIABLE, static_cast<GLuint>(variable),
                                         1, &strideProperty, 1, nullptr, &arrayStride);
            }

            stride = arrayStride > 0 ? static_cast<uint32>(arrayStride) : 0u;
        }

        // 읽기는 binding 0–3, compute 쓰기는 binding 4–7이다.
        const bool written = writeSlots != nullptr && binding >= 0 &&
                             static_cast<uint32>(binding) >= WritableStorageBindingBase;
        const uint32 slot = written ? static_cast<uint32>(binding) - WritableStorageBindingBase
                                    : static_cast<uint32>(binding);
        if (binding < 0 || slot >= MaxDrawBufferViews ||
            !AddDrawBufferSlot(written ? *writeSlots : slots, slot, kind, PixelShaderType::None,
                               stride)) {
            Diagnostic message;
            message.Text("storage block ")
                .Text(name)
                .Text(" is outside the fixed layout (storage bindings 0-3, compute writes 4-7)");
            return Reject(message, ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::graphics::gles
