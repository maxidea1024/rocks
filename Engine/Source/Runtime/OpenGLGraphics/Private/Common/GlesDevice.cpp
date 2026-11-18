#include "GlesDevice.h"

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks::graphics::gles {

namespace {

// 문자열을 dst(크기 capacity)에 NUL로 끝나게 옮긴다. 잘랐으면 true다.
bool CopyTruncated(char* destination, usize capacity, const char* source) noexcept {
    usize length = 0;
    while (source != nullptr && source[length] != 0 && length + 1 < capacity) {
        destination[length] = source[length];
        ++length;
    }

    destination[length] = 0;
    return source != nullptr && source[length] != 0;
}

void FillReport(const GlesCreateFailure& failure, DeviceCreateReport& report) noexcept {
    report = DeviceCreateReport{};
    report.code = failure.code;
    report.nativeCode = failure.nativeCode;
    const bool stageCut = CopyTruncated(report.stage, sizeof(report.stage), failure.stage);
    const bool detailCut =
        CopyTruncated(report.detail, sizeof(report.detail), failure.detail.CStr());
    report.truncated = stageCut || detailCut || failure.detail.Truncated();
}

} // namespace

Result<StagingMemory> GlesStagingSource::CreateStaging(uint64 bytes) noexcept {
    auto memory = allocator->Allocate(static_cast<usize>(bytes), 64);
    if (!memory) {
        return Result<StagingMemory>::Failure(memory.Error());
    }

    StagingMemory staging;
    staging.resource = memory.Value();
    staging.data = static_cast<uint8*>(memory.Value());
    staging.size = bytes;
    return Result<StagingMemory>::Success(staging);
}

void GlesStagingSource::DestroyStaging(const StagingMemory& memory) noexcept {
    (void)allocator->Deallocate(memory.resource);
}

GlesDevice::GlesDevice(Allocator& allocator) noexcept
    : adapterName_(allocator), lastError_(allocator), buffers_(allocator), textures_(allocator),
      pipelines_(allocator), swapChains_(allocator), retired_(allocator), readbacks_(allocator),
      bufferViews_(allocator), orphanBuffers_(allocator), timestamps_(allocator),
      timeline_(allocator, NewDeviceEpoch(), QueueKind::Graphics), splits_(allocator) {
    stagingSource_.allocator = &allocator;
}

GlesDevice::~GlesDevice() {
    // 장치는 소유 스레드(또는 소유자가 없을 때 아무 스레드)에서 파괴한다.
    if (!ownership_.IsOwner() && !ownership_.Acquire()) {
        platform::Fatal("graphics device destroyed while another thread owns it");
    }

    const bool current = platform_ && platform_->MakeCurrent();
    if (current) {
        gl_.Finish();
        timeline_.Publish(fenceValue_);
    }

    // 모든 fence가 신호된 뒤(glFinish)이므로 관찰 스레드가 곧 빠져나온다.
    timeline_.StopWatcher();
    timeline_.Close();
    if (staging_) {
        staging_->Clear();
    }

    if (current) {
        if (timestampsReady_) {
            gl_.DeleteQueries(static_cast<GLsizei>(TimestampQueryCount), timestampQueries_);
        }

        for (const ReadbackRecord& record : readbacks_) {
            gl_.DeleteBuffers(1, &record.buffer);
        }

        for (RetiredObject& entry : retired_) {
            entry.pending = false;
        }

        DeleteRetired(NumericLimits<uint64>::Max());
        for (const BufferViewRecord& record : bufferViews_) {
            gl_.DeleteTextures(1, &record.texture);
        }

        for (const OrphanBuffer& orphan : orphanBuffers_) {
            gl_.DeleteBuffers(1, &orphan.buffer);
        }

        for (const BufferRecord& record : buffers_) {
            gl_.DeleteBuffers(1, &record.buffer);
        }

        for (const TextureRecord& record : textures_) {
            ReleaseTexture(record);
        }

        for (const PipelineRecord& record : pipelines_) {
            gl_.DeleteProgram(record.program);
            gl_.DeleteVertexArrays(1, &record.vertexArray);
        }

        gl_.DeleteFramebuffers(1, &passFramebuffer_);
        gl_.DeleteProgram(depthReadProgram_);
        gl_.DeleteProgram(stencilReadProgram_);
        gl_.DeleteVertexArrays(1, &readVertexArray_);
        gl_.DeleteSamplers(1, &readSampler_);

        for (const SwapChainRecord& record : swapChains_) {
            gl_.DeleteFramebuffers(1, &record.framebuffer);
            gl_.DeleteTextures(1, &record.texture);
        }
    }

    if (platform_) {
        // context가 없어 GL 객체를 지우지 못했어도 표면은 놓는다(context 삭제가 GL 객체를
        // 회수한다).
        for (const RetiredObject& entry : retired_) {
            if (entry.kind == RetiredKind::SwapChain) {
                platform_->DetachSurface(entry.surface);
                platform_->DestroySurface(entry.surface);
            }
        }

        for (const SwapChainRecord& record : swapChains_) {
            platform_->DetachSurface(record.surface);
            platform_->DestroySurface(record.surface);
        }

        platform_->ReleaseCurrent();
    }
}

Result<void> GlesDevice::Initialize(const DeviceDesc& desc, const OpenGLDeviceOptions& options,
                                    GlesCreateFailure& failure) noexcept {
    auto platform = CreateGlesPlatform(*stagingSource_.allocator, desc, options, failure);
    if (!platform) {
        return Result<void>::Failure(platform.Error());
    }

    platform_ = Move(platform.Value());
    // 플랫폼 층이 내려가며 시도한 앞 단계의 실패 기록은 지운다(context를 만들었다).
    failure = GlesCreateFailure{};
    gl_ = platform_->Gl();
    const PlatformInfo& context = platform_->Info();

    // adapter 이름은 GL_RENDERER 뒤에 context 종류를 붙인다("... (OpenGL ES 3.2, WGL ES profile)").
    const auto* renderer = reinterpret_cast<const char*>(gl_.GetString(GL_RENDERER));
    if (renderer != nullptr) {
        auto assigned =
            adapterName_.TryAssign(StringView(renderer, platform::CStringLength(renderer)));
        auto opened = adapterName_.TryAppend(StringView(" (", 2));
        auto named = adapterName_.TryAppend(
            StringView(context.contextName, platform::CStringLength(context.contextName)));
        auto closed = adapterName_.TryAppend(StringView(")", 1));
        (void)assigned;
        (void)opened;
        (void)named;
        (void)closed;
    }

    GLint maxTextureSize = 0;
    gl_.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    maxTextureExtent_ = maxTextureSize > 0 && static_cast<uint32>(maxTextureSize) < MaxTextureExtent
                            ? static_cast<uint32>(maxTextureSize)
                            : MaxTextureExtent;
    features_ = DetectFeatures(gl_, context.desktopContext);
    rawSrgbBlit_ = context.rawSrgbBlit && features_.srgbWriteControl;
    for (uint32 number = 1; number <= MaxPixelFormatNumber; ++number) {
        const auto pixelFormat = static_cast<PixelFormat>(number);
        const GlesTextureFormat* entry =
            IsValidPixelFormat(pixelFormat) ? FindTextureFormat(pixelFormat) : nullptr;
        formatCaps_[number] =
            entry != nullptr ? TextureFormatCaps(*entry, features_) : PixelFormatCaps::None;
    }

    // 장치 능력(GX1)과 MSAA 표본 수(Multisample 능력)를 채운다.
    QueryDeviceCaps(context.desktopContext);

    const uint64 stagingBudget =
        desc.stagingBudgetBytes != 0 ? desc.stagingBudgetBytes : DefaultStagingBudgetBytes;
    auto pool = UniquePtr<StagingPool>::TryMake(
        *stagingSource_.allocator, *stagingSource_.allocator, stagingSource_, stagingBudget);
    if (!pool) {
        failure.Set(pool.Error(), "StagingPool", 0, "staging pool allocation failed");
        return Result<void>::Failure(pool.Error());
    }

    staging_ = Move(pool.Value());
    auto watching = timeline_.StartWatcher(*platform_, "OpenGLFence");
    if (!watching) {
        failure.Set(watching.Error(), "StartWatcher", 0, "fence watcher thread creation failed");
        return Result<void>::Failure(watching.Error());
    }

    // timestamp 조사는 debug output을 켜기 전에 한다(조사 호출이 검증 오류로 세어지지 않게).
    InitializeTimestamps();
    if (desc.enableValidation) {
        // KHR_debug는 ES 3.2 core다. debug context가 아니어도 debug output은 켤 수 있다.
        gl_.Enable(GL_DEBUG_OUTPUT);
        gl_.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        gl_.DebugMessageCallback(&GlesDevice::DebugMessage, this);
        validation_ = true;
    }

    // 깊이 bias 상한 확장 함수다(없으면 depthBiasClamp를 공개하지 않는다).
    if (features_.polygonOffsetClamp) {
        polygonOffsetClamp_ = reinterpret_cast<void (*)(GLfloat, GLfloat, GLfloat)>(
            platform_->ProcAddress("glPolygonOffsetClampEXT"));
        caps_.features.depthBiasClamp = polygonOffsetClamp_ != nullptr;
    }

    // 모든 패스가 같은 클립 공간 규약이므로 앞면 감김은 한 번만 정한다.
    gl_.FrontFace(FrontFaceFor(context.clip));
    gl_.Disable(GL_DEPTH_TEST);
    gl_.Disable(GL_CULL_FACE);
    gl_.Disable(GL_SCISSOR_TEST);
    // RGBA8에서도 dither가 허용되므로 backend 간 결과가 ±1 달라지지 않게 끈다.
    gl_.Disable(GL_DITHER);

    const uint8 white[4] = {255, 255, 255, 255};
    TextureDesc whiteDesc;
    whiteDesc.width = 1;
    whiteDesc.height = 1;
    whiteDesc.format = PixelFormat::RGBA8Unorm;
    auto whiteTexture = CreateTexture(whiteDesc, Span<const uint8>(white, 4));
    if (!whiteTexture) {
        failure.Set(whiteTexture.Error(), "CreateTexture", 0, "default white texture failed");
        failure.detail.Text(": ").Text(lastError_.View());
        return Result<void>::Failure(whiteTexture.Error());
    }

    white_ = whiteTexture.Value();
    return Result<void>::Success();
}

DeviceInfo GlesDevice::Info() const noexcept {
    DeviceInfo info;
    info.backend = Backend::OpenGL;
    info.adapterName = adapterName_.View();
    info.deviceEpoch = timeline_.DeviceEpoch();
    info.concurrentResourceCreation = false;
    info.validationEnabled = validation_;
    // GL_TIMESTAMP 값은 ns다. timer query가 없거나 counter bit가 0이면 timestamp가 없다(추정값을
    // 만들지 않는다).
    info.timestamps = timestampsReady_;
    info.timestampFrequency = timestampsReady_ ? TimestampNanosecondFrequency : 0;
    info.timestampDisjointDetection = timestampsReady_ && timestampSource_.disjointDetection;
    info.clockCalibration = timestampsReady_;
    // debug group은 KHR_debug(ES 3.2 core)다.
    info.debugMarkers = true;
    info.apiVersion = apiVersion_;
    return info;
}

Result<SwapChainHandle> GlesDevice::CreateSwapChain(const SwapChainDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<SwapChainHandle>::Failure(owner.Error());
    }

    if (desc.surface.window == nullptr || desc.width == 0 || desc.height == 0) {
        return Result<SwapChainHandle>::Failure(ErrorCode::InvalidArgument);
    }

    if (!MakeCurrentForResources()) {
        return FailWith<SwapChainHandle>("cannot make the OpenGL context current");
    }

    Diagnostic error;
    auto surface = platform_->CreateSurface(desc.surface, error);
    if (!surface) {
        auto rejected = Reject(error, surface.Error());
        return Result<SwapChainHandle>::Failure(rejected.Error());
    }

    SwapChainRecord record;
    record.surface = surface.Value();
    record.vsync = desc.vsync;
    if (!CreateBackBuffer(record, desc.width, desc.height)) {
        platform_->DestroySurface(record.surface);
        return FailWith<SwapChainHandle>("swap chain back buffer creation failed");
    }

    auto added = swapChains_.TryAdd(record);
    if (!added) {
        gl_.DeleteFramebuffers(1, &record.framebuffer);
        gl_.DeleteTextures(1, &record.texture);
        platform_->DestroySurface(record.surface);
        return Result<SwapChainHandle>::Failure(added.Error());
    }

    return Result<SwapChainHandle>::Success(SwapChainHandle{added.Value()});
}

Result<void> GlesDevice::ResizeSwapChain(SwapChainHandle handle, uint32 width,
                                         uint32 height) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (width == 0 || height == 0 || recording_) {
        return Result<void>::Failure(recording_ ? ErrorCode::Busy : ErrorCode::InvalidArgument);
    }

    if (width == record->width && height == record->height) {
        return Result<void>::Success();
    }

    if (!MakeCurrentForResources()) {
        return Fail("cannot make the OpenGL context current");
    }

    // 앞서 제출한 프레임이 옛 텍스처를 읽을 수 있으므로 fence 뒤에 지운다.
    RetiredObject entry;
    entry.kind = RetiredKind::Texture;
    entry.objects[0] = record->texture;
    entry.objects[2] = record->framebuffer;
    SwapChainRecord resized = *record;
    if (!CreateBackBuffer(resized, width, height)) {
        return Fail("swap chain back buffer resize failed");
    }

    Defer(entry);
    record = swapChains_.Find(handle.slot);
    if (record != nullptr) {
        *record = resized;
    }

    return Result<void>::Success();
}

Result<BufferHandle> GlesDevice::CreateBuffer(const BufferDesc& desc,
                                              Span<const uint8> initial) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<BufferHandle>::Failure(owner.Error());
    }

    auto valid = ValidateBufferDesc(desc);
    if (!valid || initial.Size() != desc.size) {
        return Result<BufferHandle>::Failure(ErrorCode::InvalidArgument);
    }

    if (desc.size > caps_.limits.bufferBytes) {
        Diagnostic message;
        message.Text("buffer size exceeds the device limit (bufferBytes)");
        auto rejected = Reject(message, ErrorCode::Unsupported);
        return Result<BufferHandle>::Failure(rejected.Error());
    }

    if (!MakeCurrentForResources()) {
        return FailWith<BufferHandle>("cannot make the OpenGL context current");
    }

    // GL 버퍼 객체는 용도와 무관하게 어느 target에나 묶인다(정점·index·버퍼 텍스처·storage block).
    BufferRecord record;
    record.size = desc.size;
    record.desc = desc;
    gl_.GenBuffers(1, &record.buffer);
    // 정점 배열(VAO) 상태를 건드리지 않도록 복사 대상 binding에 묶어 채운다(GL_ELEMENT_ARRAY_BUFFER
    // binding은 VAO 상태다).
    gl_.BindBuffer(GL_COPY_WRITE_BUFFER, record.buffer);
    gl_.BufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(desc.size), initial.Data(),
                   GL_STATIC_DRAW);
    gl_.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
    auto added = buffers_.TryAdd(record);
    if (!added) {
        gl_.DeleteBuffers(1, &record.buffer);
        return Result<BufferHandle>::Failure(added.Error());
    }

    return Result<BufferHandle>::Success(BufferHandle{added.Value()});
}

PixelFormatCaps GlesDevice::GetPixelFormatCaps(PixelFormat format) const noexcept {
    // 장치 생성 때 대응 표(GlesFormats.cpp)와 확장으로 채운 표다(PixelFormat.ko.md 7·8.6절).
    const auto number = static_cast<uint32>(format);
    if (!IsValidPixelFormat(format) || number > MaxPixelFormatNumber) {
        return PixelFormatCaps::None;
    }

    return formatCaps_[number];
}

Result<TextureHandle> GlesDevice::CreateTexture(const TextureDesc& desc,
                                                Span<const uint8> pixels) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<TextureHandle>::Failure(owner.Error());
    }

    // 계약상 올바른 형식이지만 이 장치(ES 3.2 core + 확장)에서 그 용도로 쓸 수 없으면 다른 형식으로
    // 대신하지 않고 Unsupported다(PixelFormat.ko.md 8.3절). 다른 인자 검사보다 먼저 한다.
    const bool renderTarget = desc.usage == TextureUsage::RenderTarget;
    const bool depthTarget = desc.usage == TextureUsage::DepthStencil;
    // 렌더 타깃·깊이·Storage는 초기 픽셀이 없다(업로드하지 않는다).
    const bool attachment = renderTarget || depthTarget || desc.usage == TextureUsage::Storage;
    const GlesTextureFormat* format = FindTextureFormat(desc.format);
    if (IsValidPixelFormat(desc.format) && IsValidTextureDimension(desc.dimension) &&
        !HasPixelFormatCaps(GetPixelFormatCaps(desc.format), RequiredPixelFormatCaps(desc))) {
        Diagnostic message;
        message.Text("pixel format ")
            .Text(PixelFormatName(desc.format))
            .Text(renderTarget ? " as a render target" : "")
            .Text(depthTarget ? " as a depth stencil target" : "")
            .Text(" is not supported by this OpenGL ES device");
        auto rejected = Reject(message, ErrorCode::Unsupported);
        return Result<TextureHandle>::Failure(rejected.Error());
    }

    auto valid = ValidateTextureDesc(desc);
    if (!valid) {
        return Result<TextureHandle>::Failure(valid.Error());
    }

    // 장치 한도를 크기 계산보다 먼저 본다(한도 안의 desc는 데이터 byte가 넘치지 않는다).
    if (desc.width > maxTextureExtent_ || desc.height > maxTextureExtent_ ||
        !CheckTextureLimits(caps_, desc)) {
        Diagnostic message;
        message.Text("texture format ")
            .Text(PixelFormatName(desc.format))
            .Text(renderTarget ? " as a render target" : "")
            .Text(" or extent ")
            .Number(desc.width)
            .Text("x")
            .Number(desc.height)
            .Text(" is not supported by this OpenGL ES device");
        auto rejected = Reject(message, ErrorCode::Unsupported);
        return Result<TextureHandle>::Failure(rejected.Error());
    }

    const PixelFormatInfo& info = GetPixelFormatInfo(desc.format);
    const uint64 size = TextureDataBytes(desc);
    // BC는 최상위 크기가 블록 배수여야 한다(PixelFormatInfo::topLevelBlockAligned, D3D와 같은
    // 계약).
    const bool aligned = !info.topLevelBlockAligned ||
                         (desc.width % info.blockWidth == 0 && desc.height % info.blockHeight == 0);
    if (format == nullptr || desc.width > MaxTextureExtent || desc.height > MaxTextureExtent ||
        size == 0 || !aligned || desc.mipLevels > MaxMipLevels ||
        (attachment ? pixels.Size() != 0 : pixels.Size() != size)) {
        return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }

    if (!MakeCurrentForResources()) {
        return FailWith<TextureHandle>("cannot make the OpenGL context current");
    }

    TextureRecord record;
    record.dimension = desc.dimension;
    record.target = TextureTarget(desc.dimension);
    record.width = desc.width;
    record.height = desc.height;
    record.depth = desc.depth;
    record.mipLevels = desc.mipLevels;
    record.slices = TextureSliceCount(desc);
    record.format = desc.format;
    record.swizzle = desc.swizzle;
    record.depthTarget = depthTarget;
    record.usage = desc.usage;
    record.viewRange = {0, record.mipLevels, 0, record.slices};
    record.depthStencilTexture = info.kind == PixelFormatKind::DepthStencil;
    record.samplerDesc = desc.sampler;
    gl_.GenTextures(1, &record.texture);
    gl_.ActiveTexture(GL_TEXTURE0 + UploadTextureUnit);
    gl_.BindTexture(record.target, record.texture);
    // 2D·cube는 TexStorage2D(cube는 6면을 한 번에), 2D 배열·cube 배열·3D는 TexStorage3D다(cube
    // 배열의 깊이는 layer 수 = cube × 6, ES 3.2 8.18절).
    const auto levels = static_cast<GLsizei>(desc.mipLevels);
    if (record.target == GL_TEXTURE_2D || record.target == GL_TEXTURE_CUBE_MAP) {
        gl_.TexStorage2D(record.target, levels, format->internalFormat,
                         static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height));
    } else {
        const uint32 layers =
            desc.dimension == TextureDimension::Texture3D ? desc.depth : record.slices;
        gl_.TexStorage3D(record.target, levels, format->internalFormat,
                         static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height),
                         static_cast<GLsizei>(layers));
    }

    if (format->Is(GlesAlphaSwizzle)) {
        // A8: R8 저장소를 (0, 0, 0, R)로 읽는다(텍스처 객체 상태, ES 3.0 core). 묶을 때 사용자
        // swizzle과 합성해 다시 정한다(BindPassTexture).
        gl_.TexParameteri(record.target, GL_TEXTURE_SWIZZLE_R, static_cast<GLint>(GL_ZERO));
        gl_.TexParameteri(record.target, GL_TEXTURE_SWIZZLE_G, static_cast<GLint>(GL_ZERO));
        gl_.TexParameteri(record.target, GL_TEXTURE_SWIZZLE_B, static_cast<GLint>(GL_ZERO));
        gl_.TexParameteri(record.target, GL_TEXTURE_SWIZZLE_A, static_cast<GLint>(GL_RED));
    }

    // 깊이 형식의 비교 샘플러는 선형 필터를 받는다(ES 3.2 8.17절: 깊이 텍스처의 선형 필터 불완전
    // 조건은 TEXTURE_COMPARE_MODE가 NONE일 때만이다).
    const bool depthFormat =
        info.kind == PixelFormatKind::Depth || info.kind == PixelFormatKind::DepthStencil;
    record.sampler = CreateSampler(desc.sampler, HasPixelFormatCaps(GetPixelFormatCaps(desc.format),
                                                                    PixelFormatCaps::Filterable) ||
                                                     (desc.sampler.comparison && depthFormat));
    if (!attachment) {
        // 첫 행을 v = 0에 둔다. UV (0,0)이 이미지 좌상단이 되어 D3D12와 결과가 같다.
        usize offset = 0;
        for (uint32 level = 0; level < desc.mipLevels; ++level) {
            auto uploaded = UploadMip(record, level, pixels.Data() + offset);
            if (!uploaded) {
                ReleaseTexture(record);
                return Result<TextureHandle>::Failure(uploaded.Error());
            }

            offset += static_cast<usize>(TextureMipBytes(desc, level));
        }
    } else if (renderTarget) {
        record.framebuffer = CreateColorFramebuffer(record.texture);
        if (record.framebuffer == 0) {
            ReleaseTexture(record);
            return FailWith<TextureHandle>("render target framebuffer is incomplete");
        }
    }

    auto added = textures_.TryAdd(record);
    if (!added) {
        ReleaseTexture(record);
        return Result<TextureHandle>::Failure(added.Error());
    }

    return Result<TextureHandle>::Success(TextureHandle{added.Value()});
}

// ES core에는 texture view(형식 재해석)가 없다. 같은 텍스처 객체를 공유해 plane만 고르는 view(깊이·
// 스텐실 plane, 같은 형식)를 만들고, 형식을 바꾸는 view(sRGB 짝)는 Unsupported다(PixelFormat.ko.md
// 14.2절).
Result<TextureHandle> GlesDevice::CreateTextureView(TextureHandle texture,
                                                    const TextureViewDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<TextureHandle>::Failure(owner.Error());
    }

    TextureRecord* source = textures_.Find(texture.slot);
    if (source == nullptr || source->retirePending) {
        return Result<TextureHandle>::Failure(ErrorCode::StaleHandle);
    }

    if (source->isView) {
        return Result<TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }

    auto resolved = ResolveTextureViewFormat(source->format, desc, source->samplerDesc.comparison);
    if (!resolved) {
        return Result<TextureHandle>::Failure(resolved.Error());
    }

    auto range = ResolveTextureViewRange(source->dimension, source->usage,
                                         {source->mipLevels, source->slices}, desc);
    if (!range) {
        return Result<TextureHandle>::Failure(range.Error());
    }

    // texture view가 없어 읽기 view는 텍스처 전체를 묶는다(mip은 셰이더가 TextureViewBaseMip으로
    // 고른다). slice 범위를 고를 수단은 없으므로 일부 slice view는 Unsupported다. 쓰기 view는
    // level 하나를 묶고, 배열은 layered(모든 층)로만 묶는다(셰이더 image2DArray의 층 번호가 view
    // 기준이 되도록).
    const bool writable = desc.access == ViewAccess::ReadWrite;
    if (range.Value().sliceCount != source->slices) {
        Diagnostic message;
        message.Text("texture view over part of the slices is not supported by OpenGL ES (no "
                     "texture views in ES core)");
        auto rejected = Reject(message, ErrorCode::Unsupported);
        return Result<TextureHandle>::Failure(rejected.Error());
    }

    if (writable &&
        !HasPixelFormatCaps(GetPixelFormatCaps(resolved.Value()), PixelFormatCaps::StorageWrite)) {
        Diagnostic message;
        message.Text("pixel format ")
            .Text(PixelFormatName(resolved.Value()))
            .Text(" as a storage image is not supported by this OpenGL ES device");
        auto rejected = Reject(message, ErrorCode::Unsupported);
        return Result<TextureHandle>::Failure(rejected.Error());
    }

    const PixelFormatKind kind = GetPixelFormatInfo(source->format).kind;
    const bool depth = kind == PixelFormatKind::Depth || kind == PixelFormatKind::DepthStencil;
    if (!HasPixelFormatCaps(GetPixelFormatCaps(source->format), PixelFormatCaps::Sampled) ||
        (!depth && resolved.Value() != source->format)) {
        Diagnostic message;
        message.Text("texture view of ")
            .Text(PixelFormatName(source->format))
            .Text(" as ")
            .Text(PixelFormatName(resolved.Value()))
            .Text(" is not supported by OpenGL ES (no texture views in ES core)");
        auto rejected = Reject(message, ErrorCode::Unsupported);
        return Result<TextureHandle>::Failure(rejected.Error());
    }

    if (!MakeCurrentForResources()) {
        return FailWith<TextureHandle>("cannot make the OpenGL context current");
    }

    TextureRecord record;
    record.texture = source->texture;
    record.dimension = source->dimension;
    record.target = source->target;
    record.width = source->width;
    record.height = source->height;
    record.depth = source->depth;
    record.mipLevels = source->mipLevels;
    record.slices = source->slices;
    record.format = resolved.Value();
    record.swizzle = desc.swizzle;
    record.plane = desc.plane;
    record.depthStencilTexture = source->depthStencilTexture;
    record.samplerDesc = source->samplerDesc;
    record.usage = source->usage;
    record.viewRange = range.Value();
    record.writable = writable;
    record.isView = true;
    record.source = texture.slot;
    record.sampler =
        CreateSampler(record.samplerDesc, HasPixelFormatCaps(GetPixelFormatCaps(source->format),
                                                             PixelFormatCaps::Filterable) ||
                                              (record.samplerDesc.comparison && depth));
    auto added = textures_.TryAdd(record);
    if (!added) {
        gl_.DeleteSamplers(1, &record.sampler);
        return Result<TextureHandle>::Failure(added.Error());
    }

    ++textures_.Find(texture.slot)->viewCount;
    return Result<TextureHandle>::Success(TextureHandle{added.Value()});
}

Result<PipelineHandle> GlesDevice::CreatePipeline(const PipelineDesc& desc) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<PipelineHandle>::Failure(owner.Error());
    }

    if (desc.vertex.glsl.ByteCount() == 0 || desc.pixel.glsl.ByteCount() == 0 ||
        !ValidatePipelineDesc(desc)) {
        Diagnostic message;
        message.Text("pipeline description breaks the pipeline rules");
        auto rejected = Reject(message, ErrorCode::InvalidArgument);
        return Result<PipelineHandle>::Failure(rejected.Error());
    }

    // 렌더 타깃으로 대응하지 않는 형식은 대신하지 않고 Unsupported다(PixelFormat.ko.md 8.3절).
    // 블렌드를 켜면 그 형식이 블렌드를 받아야 한다(32-bit float는 GL_EXT_float_blend).
    for (uint32 index = 0; index < desc.colorTargetCount; ++index) {
        const PixelFormat format = desc.colorFormats[index];
        const PixelFormatCaps required =
            desc.blend[index].enable ? PixelFormatCaps::RenderTarget | PixelFormatCaps::Blendable
                                     : PixelFormatCaps::RenderTarget;
        if (FindTextureFormat(format) == nullptr ||
            !HasPixelFormatCaps(GetPixelFormatCaps(format), required)) {
            Diagnostic message;
            message.Text("pixel format ")
                .Text(PixelFormatName(format))
                .Text(" is not supported as a render target by this OpenGL ES device");
            auto rejected = Reject(message, ErrorCode::Unsupported);
            return Result<PipelineHandle>::Failure(rejected.Error());
        }
    }

    if (desc.depthFormat != PixelFormat::Unknown &&
        (FindTextureFormat(desc.depthFormat) == nullptr ||
         !HasPixelFormatCaps(GetPixelFormatCaps(desc.depthFormat),
                             PixelFormatCaps::DepthStencil))) {
        Diagnostic message;
        message.Text("pixel format ")
            .Text(PixelFormatName(desc.depthFormat))
            .Text(" is not supported as a depth target by this OpenGL ES device");
        auto rejected = Reject(message, ErrorCode::Unsupported);
        return Result<PipelineHandle>::Failure(rejected.Error());
    }

    if (!CheckPipelineFeatures(caps_, desc)) {
        Diagnostic message;
        message.Text("pipeline needs more color targets or a raster feature than the device "
                     "supports");
        auto rejected = Reject(message, ErrorCode::Unsupported);
        return Result<PipelineHandle>::Failure(rejected.Error());
    }

    if (!ValidateVertexLayout(desc.layout)) {
        Diagnostic message;
        message.Text("vertex layout is invalid (RenderBase 10.4 limits)");
        auto rejected = Reject(message, ErrorCode::InvalidArgument);
        return Result<PipelineHandle>::Failure(rejected.Error());
    }

    for (const VertexAttribute& attribute : desc.layout.attributes) {
        if (!VertexAttributeFormat(attribute.format).supported) {
            Diagnostic message;
            message.Text("vertex format ")
                .Text(VertexFormatName(attribute.format))
                .Text(" is not supported by the OpenGL ES backend");
            auto rejected = Reject(message, ErrorCode::Unsupported);
            return Result<PipelineHandle>::Failure(rejected.Error());
        }
    }

    if (!MakeCurrentForResources()) {
        return FailWith<PipelineHandle>("cannot make the OpenGL context current");
    }

    auto vertex = CompileShader(GL_VERTEX_SHADER, desc.vertex.glsl);
    if (!vertex) {
        return Result<PipelineHandle>::Failure(vertex.Error());
    }

    auto pixel = CompileShader(GL_FRAGMENT_SHADER, desc.pixel.glsl);
    if (!pixel) {
        gl_.DeleteShader(vertex.Value());
        return Result<PipelineHandle>::Failure(pixel.Error());
    }

    // separate shader object를 쓰지 않고 VS·PS를 한 프로그램으로 잇는다(UE OpenGLBase.h:283–287).
    PipelineRecord record;
    record.program = gl_.CreateProgram();
    gl_.AttachShader(record.program, vertex.Value());
    gl_.AttachShader(record.program, pixel.Value());
    gl_.LinkProgram(record.program);
    gl_.DeleteShader(vertex.Value());
    gl_.DeleteShader(pixel.Value());
    GLint linked = GL_FALSE;
    gl_.GetProgramiv(record.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        StoreLog(record.program, false);
        gl_.DeleteProgram(record.program);
        return Result<PipelineHandle>::Failure(ErrorCode::DependencyFailed);
    }

    // 루트 상수 에뮬레이션 배열 위치·크기는 링크 때 한 번 찾는다(ShaderSystem 7.2절 4항).
    record.drawConstantBytes = desc.drawConstantBytes;
    auto matched = MatchVertexInputs(record.program, desc.layout);
    auto reflected =
        matched ? ReflectBufferSlots(record.program, record.bufferSlots, nullptr) : Move(matched);
    auto constants = reflected ? ReflectRootConstants(record.program, record) : Move(reflected);
    if (!constants) {
        gl_.DeleteProgram(record.program);
        return Result<PipelineHandle>::Failure(constants.Error());
    }

    switch (desc.topology) {
    case PrimitiveTopology::TriangleStrip:
        record.topology = GL_TRIANGLE_STRIP;
        break;
    case PrimitiveTopology::LineList:
        record.topology = GL_LINES;
        break;
    case PrimitiveTopology::LineStrip:
        record.topology = GL_LINE_STRIP;
        break;
    case PrimitiveTopology::PointList:
        record.topology = GL_POINTS;
        break;
    default:
        record.topology = GL_TRIANGLES;
        break;
    }

    record.colorTargetCount = desc.colorTargetCount;
    for (uint32 index = 0; index < MaxColorTargets; ++index) {
        record.blend[index] = desc.blend[index];
    }

    record.alphaToCoverage = desc.alphaToCoverage;
    record.depthStencil = desc.depthStencil;
    record.raster = desc.raster;
    record.writesDepthStencil = PipelineWritesDepthStencil(desc);
    for (usize index = 0; index < desc.layout.streams.Size(); ++index) {
        record.strides[index] = desc.layout.streams[index].stride;
        record.streamMask |= 1u << index;
    }

    // 정점 형식은 ES 3.1 vertex attrib binding(glVertexAttribFormat·glVertexAttribBinding)으로
    // 파이프라인의 정점 배열에 한 번 둔다. 버퍼는 그릴 때 glBindVertexBuffer로 묶는다. 속성이 없는
    // 배치도 정점 배열을 둔다.
    gl_.GenVertexArrays(1, &record.vertexArray);
    gl_.BindVertexArray(record.vertexArray);
    for (const VertexAttribute& attribute : desc.layout.attributes) {
        const GlesVertexAttribute format = VertexAttributeFormat(attribute.format);
        gl_.EnableVertexAttribArray(attribute.location);
        if (format.integer) {
            gl_.VertexAttribIFormat(attribute.location, format.size, format.type, attribute.offset);
        } else {
            gl_.VertexAttribFormat(attribute.location, format.size, format.type,
                                   format.normalized ? GL_TRUE : GL_FALSE, attribute.offset);
        }

        gl_.VertexAttribBinding(attribute.location, attribute.stream);
    }

    for (usize index = 0; index < desc.layout.streams.Size(); ++index) {
        gl_.VertexBindingDivisor(
            static_cast<GLuint>(index),
            desc.layout.streams[index].stepRate == VertexStepRate::Instance ? 1u : 0u);
    }

    // 기록 중이면 묶여 있던 파이프라인의 정점 배열로 되돌린다.
    const PipelineRecord* bound = recording_ ? pipelines_.Find(boundPipeline_.slot) : nullptr;
    gl_.BindVertexArray(bound != nullptr ? bound->vertexArray : 0u);
    auto added = pipelines_.TryAdd(record);
    if (!added) {
        gl_.DeleteProgram(record.program);
        gl_.DeleteVertexArrays(1, &record.vertexArray);
        return Result<PipelineHandle>::Failure(added.Error());
    }

    return Result<PipelineHandle>::Success(PipelineHandle{added.Value()});
}

void GlesDevice::Retire(BufferHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    const BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    // view가 남아 있으면 버퍼 이름을 orphan으로 옮겨 마지막 view가 놓일 때 지운다.
    if (record->viewCount != 0) {
        auto kept = orphanBuffers_.TryPushBack(OrphanBuffer{record->buffer, record->viewCount});
        (void)kept;
        // 기록할 자리가 없어도 이름을 지우지 않는다. view가 아직 이 이름을 쓰므로 지우면 GPU가 쓰는
        // 동안 이름이 다른 버퍼에 다시 쓰일 수 있다. 그때는 소멸 때까지 남기고 context 삭제가
        // 회수한다(Vulkan의 KeepUntilDestruction과 같은 선택).
        auto removed = buffers_.TryRemove(handle.slot);
        (void)removed;
        return;
    }

    RetiredObject entry;
    entry.kind = RetiredKind::Buffer;
    entry.objects[0] = record->buffer;
    Defer(entry);
    auto removed = buffers_.TryRemove(handle.slot);
    (void)removed;
}

void GlesDevice::Retire(TextureHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr || handle == white_ || record->retirePending) {
        return;
    }

    if (record->viewCount != 0) {
        record->retirePending = true;
        return;
    }

    const bool view = record->isView;
    const SlotHandle source = record->source;
    RetireTextureRecord(handle.slot, *record);
    if (view) {
        TextureRecord* owner = textures_.Find(source);
        if (owner != nullptr && owner->viewCount != 0 && --owner->viewCount == 0 &&
            owner->retirePending) {
            RetireTextureRecord(source, *owner);
        }
    }
}

void GlesDevice::RetireTextureRecord(SlotHandle slot, const TextureRecord& record) noexcept {
    RetiredObject entry;
    entry.kind = RetiredKind::Texture;
    // view는 텍스처 객체를 원 기록과 공유하므로 자기 샘플러만 놓는다(이름 0은 지우지 않는다).
    entry.objects[0] = record.isView ? 0u : record.texture;
    entry.objects[1] = record.sampler;
    entry.objects[2] = record.framebuffer;
    Defer(entry);
    auto removed = textures_.TryRemove(slot);
    (void)removed;
}

void GlesDevice::Retire(PipelineHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    const PipelineRecord* record = pipelines_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    RetiredObject entry;
    entry.kind = RetiredKind::Pipeline;
    entry.objects[0] = record->program;
    entry.objects[1] = record->vertexArray;
    Defer(entry);
    auto removed = pipelines_.TryRemove(handle.slot);
    (void)removed;
}

void GlesDevice::Retire(SwapChainHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    SwapChainRecord* record = swapChains_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    // 이 창 표면에 context가 붙어 있으면 장치 표면으로 옮긴다. 표면·텍스처는 fence 뒤에 놓는다.
    platform_->DetachSurface(record->surface);
    RetiredObject entry;
    entry.kind = RetiredKind::SwapChain;
    entry.objects[0] = record->texture;
    entry.objects[2] = record->framebuffer;
    entry.surface = record->surface;
    Defer(entry);
    auto removed = swapChains_.TryRemove(handle.slot);
    (void)removed;
}

void GlesDevice::ReclaimCompleted() noexcept {
    if (!ownership_.Check()) {
        return;
    }

    DeleteRetired(timeline_.Completed());
}

Result<void> GlesDevice::WaitIdle() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    if (!MakeCurrentForResources()) {
        return Fail("cannot make the OpenGL context current");
    }

    gl_.Finish();
    // glFinish 뒤에는 앞선 모든 fence가 신호되었다. 관찰 스레드를 기다리지 않고 바로 게시한다.
    timeline_.Publish(fenceValue_);
    DeleteRetired(fenceValue_);
    if (timeline_.IsLost()) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    return Result<void>::Success();
}

usize GlesDevice::PendingRetireCount() const noexcept {
    return retired_.Size();
}

StringView GlesDevice::LastError() const noexcept {
    return lastError_.View();
}

uint64 GlesDevice::IgnoredDraws() const noexcept {
    return ignoredDraws_;
}

uint64 GlesDevice::ValidationErrorCount() const noexcept {
    return validationErrors_;
}

void GlesDevice::DebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity,
                              GLsizei length, const GLchar* message, const void* user) {
    (void)source;
    (void)severity;
    auto* self = static_cast<GlesDevice*>(const_cast<void*>(user));
    if (type == GL_DEBUG_TYPE_ERROR || type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR) {
        ++self->validationErrors_;
        if (message != nullptr && length > 0) {
            const StringView text(message, static_cast<usize>(length));
            auto assigned = self->lastError_.TryAssign(text);
            (void)assigned;
            // 검증 계층 원문을 메시지 ID마다 한 번 남긴다(LoggingSystem 13절).
            if (self->FirstValidationMessage(id)) {
                Log::Warning(LogGraphicsOpenGLValidation, "GL debug message {Id}: {Message}",
                             static_cast<uint32>(id), text);
            }
        }
    }
}

bool GlesDevice::FirstValidationMessage(GLuint id) noexcept {
    for (uint32 index = 0; index < loggedValidationCount_; ++index) {
        if (loggedValidationIds_[index] == id) {
            return false;
        }
    }

    if (loggedValidationCount_ == MaxLoggedValidationIds) {
        return false;
    }

    loggedValidationIds_[loggedValidationCount_++] = id;
    return true;
}

Result<void> GlesDevice::Fail(const char* message) noexcept {
    // 원본 GL 오류를 덧붙인다. context가 없으면 오류를 읽지 않는다.
    Diagnostic text;
    text.Text(message);
    if (platform_ && platform_->IsCurrent()) {
        const GLenum error = gl_.GetError();
        if (error != GL_NO_ERROR) {
            text.Text(" (").Text(GlErrorName(error)).Text(" ").Hex(error).Text(")");
        }
    }

    // 실패는 호출자가 결과로 받는다. 로그는 결정하는 쪽이 남기고(LoggingSystem 13절), 장치는
    // present 실패와 장치 손실만 직접 남긴다.
    auto assigned = lastError_.TryAssign(text.View());
    (void)assigned;
    return Result<void>::Failure(ErrorCode::DependencyFailed);
}

Result<void> GlesDevice::Reject(const Diagnostic& message, ErrorCode error) noexcept {
    auto assigned = lastError_.TryAssign(message.View());
    (void)assigned;
    return Result<void>::Failure(error);
}

Result<void> GlesDevice::RejectDrawConstantSize(bool pipelineBound, usize bytes,
                                                uint32 drawConstantBytes) noexcept {
    Diagnostic message;
    if (!pipelineBound) {
        message.Text("SetDrawConstants needs a pipeline bound while recording");
    } else {
        message.Text("SetDrawConstants got ")
            .Number(bytes)
            .Text(" bytes but the bound pipeline declares drawConstantBytes ")
            .Number(drawConstantBytes);
    }

    return Reject(message, ErrorCode::InvalidArgument);
}

bool GlesDevice::MakeCurrentForResources() noexcept {
    if (platform_->IsCurrent()) {
        return true;
    }

    return platform_->MakeCurrent();
}

uint64 GlesDevice::Submit() noexcept {
    // 앞 단계(창 표면 전환)가 실패해 context가 풀렸으면 장치 표면에 다시 붙인다.
    if (!MakeCurrentForResources()) {
        timeline_.MarkLost();
        if (!lossLogged_) {
            lossLogged_ = true;
            Log::Error(LogGraphicsOpenGL,
                       "OpenGL ES device lost: the context could not be made current to submit "
                       "work after frame {Frame}",
                       fenceValue_);
        }

        return fenceValue_;
    }

    void* token = platform_->InsertFence();
    if (token == nullptr) {
        gl_.Finish();
    }

    const uint64 value = ++fenceValue_;
    timeline_.Submitted(value, token);
    return value;
}

void GlesDevice::Defer(RetiredObject entry) noexcept {
    entry.fenceValue = fenceValue_;
    entry.pending = recording_;
    auto pushed = retired_.TryPushBack(entry);
    if (pushed) {
        return;
    }

    if (recording_ || !MakeCurrentForResources()) {
        // 기록 중인 프레임이 쓸 수 있거나 context가 없다. GL 객체는 지우지 않고 버린다(context 삭제
        // 때 드라이버가 회수한다). 창 표면은 GL 객체가 아니고 기록한 명령이 쓰지 않으므로(그리기는
        // 스왑체인 텍스처에, present는 Retire로 생략된다) 바로 놓는다.
        if (entry.kind == RetiredKind::SwapChain) {
            platform_->DestroySurface(entry.surface);
        }

        return;
    }

    gl_.Finish();
    DeleteObject(entry);
}

void GlesDevice::AssignPendingRetires(uint64 value) noexcept {
    for (RetiredObject& entry : retired_) {
        if (entry.pending) {
            entry.pending = false;
            entry.fenceValue = value;
        }
    }
}

void GlesDevice::DeleteRetired(uint64 completed) noexcept {
    if (retired_.IsEmpty() || !MakeCurrentForResources()) {
        return;
    }

    for (usize index = retired_.Size(); index > 0; --index) {
        const RetiredObject& entry = retired_[index - 1];
        if (entry.pending || entry.fenceValue > completed) {
            continue;
        }

        DeleteObject(entry);
        auto removed = retired_.TryRemoveAtSwap(index - 1);
        (void)removed;
    }
}

void GlesDevice::DeleteObject(const RetiredObject& entry) noexcept {
    switch (entry.kind) {
    case RetiredKind::Buffer:
        gl_.DeleteBuffers(1, &entry.objects[0]);
        break;
    case RetiredKind::Texture:
    case RetiredKind::SwapChain:
        if (entry.objects[2] != 0) {
            gl_.DeleteFramebuffers(1, &entry.objects[2]);
        }

        if (entry.objects[1] != 0) {
            gl_.DeleteSamplers(1, &entry.objects[1]);
        }

        if (entry.objects[0] != 0) {
            gl_.DeleteTextures(1, &entry.objects[0]);
        }

        if (entry.kind == RetiredKind::SwapChain) {
            platform_->DestroySurface(entry.surface);
        }

        break;
    case RetiredKind::Pipeline:
        gl_.DeleteProgram(entry.objects[0]);
        gl_.DeleteVertexArrays(1, &entry.objects[1]);
        break;
    }
}

void GlesDevice::ReleaseTexture(const TextureRecord& record) noexcept {
    if (record.framebuffer != 0) {
        gl_.DeleteFramebuffers(1, &record.framebuffer);
    }

    if (record.sampler != 0) {
        gl_.DeleteSamplers(1, &record.sampler);
    }

    if (!record.isView) {
        gl_.DeleteTextures(1, &record.texture);
    }
}

bool GlesDevice::CreateBackBuffer(SwapChainRecord& record, uint32 width, uint32 height) noexcept {
    const GlesTextureFormat* format = FindTextureFormat(PixelFormat::RGBA8UnormSrgb);
    GLuint texture = 0;
    gl_.GenTextures(1, &texture);
    gl_.ActiveTexture(GL_TEXTURE0 + UploadTextureUnit);
    gl_.BindTexture(GL_TEXTURE_2D, texture);
    gl_.TexStorage2D(GL_TEXTURE_2D, 1, format->internalFormat, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height));
    const GLuint framebuffer = CreateColorFramebuffer(texture);
    if (framebuffer == 0) {
        gl_.DeleteTextures(1, &texture);
        return false;
    }

    record.texture = texture;
    record.framebuffer = framebuffer;
    record.width = width;
    record.height = height;
    return true;
}

GLuint GlesDevice::CreateColorFramebuffer(GLuint texture) noexcept {
    // 기록 중인 패스의 그리기 framebuffer를 건드리지 않도록 읽기 binding에서 만든다.
    GLuint framebuffer = 0;
    gl_.GenFramebuffers(1, &framebuffer);
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    gl_.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const GLenum status = gl_.CheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        gl_.DeleteFramebuffers(1, &framebuffer);
        Diagnostic message;
        message.Text("framebuffer status ").Hex(status);
        auto assigned = lastError_.TryAssign(message.View());
        (void)assigned;
        return 0;
    }

    return framebuffer;
}

GLuint GlesDevice::CreateSampler(const SamplerDesc& desc, bool filterable) noexcept {
    GLuint sampler = 0;
    gl_.GenSamplers(1, &sampler);
    // 선형 필터를 받지 않는 형식(정수·깊이·필터 확장 없는 32-bit float)에 선형 필터를 걸면 ES에서
    // 불완전 텍스처가 되어 texelFetch도 0을 읽는다. 그 형식은 최근접 필터로 둔다(Filterable 능력이
    // 없음을 공개한 형식이다).
    const Filter filter = filterable ? desc.filter : Filter::Point;
    gl_.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, MinFilter(filter));
    gl_.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, MagFilter(filter));
    gl_.SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, WrapMode(desc.addressU));
    gl_.SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, WrapMode(desc.addressV));
    gl_.SamplerParameteri(sampler, GL_TEXTURE_WRAP_R, WrapMode(desc.addressW));
    // 비교 샘플러다. GL 비교 함수(NEVER = 0x0200 …)는 CompareFunction과 같은 순서다.
    gl_.SamplerParameteri(
        sampler, GL_TEXTURE_COMPARE_MODE,
        static_cast<GLint>(desc.comparison ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE));
    if (desc.comparison) {
        gl_.SamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC,
                              static_cast<GLint>(GL_NEVER + static_cast<GLenum>(desc.compare)));
    }
    // border 색은 ES 3.2 core(GL_OES_texture_border_clamp 흡수)다.
    float border[4];
    BorderColorValue(desc.borderColor, border);
    gl_.SamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, border);
    if (IsAnisotropicFilter(filter) && features_.anisotropic) {
        const float requested = static_cast<float>(desc.maxAnisotropy);
        gl_.SamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY,
                              requested < features_.maxAnisotropy ? requested
                                                                  : features_.maxAnisotropy);
    }

    return sampler;
}

Result<void> GlesDevice::UploadMip(const TextureRecord& record, uint32 level,
                                   const uint8* pixels) noexcept {
    gl_.ActiveTexture(GL_TEXTURE0 + UploadTextureUnit);
    gl_.BindTexture(record.target, record.texture);
    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // cube는 면마다 TEXTURE_CUBE_MAP_POSITIVE_X + 면 대상으로 올린다(면 순서는 Rocks·UE·D3D와
    // 같다). 다른 차원은 대상 하나에 그 mip의 모든 layer·깊이 면을 한 번에 올린다.
    if (record.target == GL_TEXTURE_CUBE_MAP) {
        const usize face = static_cast<usize>(PixelFormatImageBytes(
            record.format, MipExtent(record.width, level), MipExtent(record.height, level)));
        for (uint32 index = 0; index < CubeFaceCount; ++index) {
            auto uploaded = UploadImages(record, GL_TEXTURE_CUBE_MAP_POSITIVE_X + index, level, 0,
                                         1, pixels + index * face);
            if (!uploaded) {
                return uploaded;
            }
        }

        return Result<void>::Success();
    }

    const uint32 images = record.dimension == TextureDimension::Texture3D
                              ? MipExtent(record.depth, level)
                              : record.slices;
    return UploadImages(record, record.target, level, 0, images, pixels);
}

Result<void> GlesDevice::UploadImages(const TextureRecord& record, GLenum target, uint32 level,
                                      uint32 firstLayer, uint32 images,
                                      const uint8* pixels) noexcept {
    const GlesTextureFormat* format = FindTextureFormat(record.format);
    const uint32 width = MipExtent(record.width, level);
    const uint32 height = MipExtent(record.height, level);
    const auto glWidth = static_cast<GLsizei>(width);
    const auto glHeight = static_cast<GLsizei>(height);
    // 2D 대상(TEXTURE_2D·cube 면)은 TexSubImage2D, layer 대상(2D 배열·cube 배열·3D)은
    // TexSubImage3D(깊이 = 이미지 수)다.
    const bool layered = target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_3D ||
                         target == GL_TEXTURE_CUBE_MAP_ARRAY;
    auto upload = [&](GLenum external, GLenum type, const void* data) noexcept {
        if (layered) {
            gl_.TexSubImage3D(target, static_cast<GLint>(level), 0, 0,
                              static_cast<GLint>(firstLayer), glWidth, glHeight,
                              static_cast<GLsizei>(images), external, type, data);
        } else {
            gl_.TexSubImage2D(target, static_cast<GLint>(level), 0, 0, glWidth, glHeight, external,
                              type, data);
        }
    };
    if (format->Is(GlesCompressed)) {
        const uint64 bytes = PixelFormatImageBytes(record.format, width, height) * images;
        if (layered) {
            gl_.CompressedTexSubImage3D(target, static_cast<GLint>(level), 0, 0,
                                        static_cast<GLint>(firstLayer), glWidth, glHeight,
                                        static_cast<GLsizei>(images), format->internalFormat,
                                        static_cast<GLsizei>(bytes), pixels);
        } else {
            gl_.CompressedTexSubImage2D(target, static_cast<GLint>(level), 0, 0, glWidth, glHeight,
                                        format->internalFormat, static_cast<GLsizei>(bytes),
                                        pixels);
        }

        return Result<void>::Success();
    }

    // D16·D32F와 색 형식은 Rocks 배치가 GL 외부 배치와 같다. D24는 하위 24비트 값을 32-bit 정규화
    // 정수로 넓히고, 깊이·스텐실은 두 plane을 texel 하나로 엮는다(UNSIGNED_INT_24_8: 깊이 상위
    // 24비트 + 스텐실, FLOAT_32_UNSIGNED_INT_24_8_REV: float 깊이 + 스텐실 하위 8비트 32-bit 칸).
    const PixelFormat depthLayout = record.format;
    if (depthLayout != PixelFormat::D24Unorm && depthLayout != PixelFormat::D24UnormS8Uint &&
        depthLayout != PixelFormat::D32FloatS8Uint) {
        upload(format->externalFormat, format->externalType, pixels);
        return Result<void>::Success();
    }

    // 묶는 깊이 형식은 2D다(다중 plane D24S8·D32S8은 2D만이고, D24Unorm은 이미지마다 같은 변환을
    // 이어서 한다).
    const usize texels = static_cast<usize>(width) * height * images;
    const usize texelBytes = depthLayout == PixelFormat::D32FloatS8Uint ? 8 : 4;
    Array<uint8> packed(*stagingSource_.allocator);
    if (!packed.TryResize(texels * texelBytes)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    const uint8* stencil = pixels + texels * 4;
    for (usize texel = 0; texel < texels; ++texel) {
        uint32 depth = 0;
        platform::MemoryCopy(&depth, pixels + texel * 4, 4);
        uint8* out = packed.Data() + texel * texelBytes;
        if (depthLayout == PixelFormat::D24Unorm) {
            const uint32 value = depth & 0xFFFFFFu;
            const uint32 wide = (value << 8) | (value >> 16);
            platform::MemoryCopy(out, &wide, 4);
        } else if (depthLayout == PixelFormat::D24UnormS8Uint) {
            const uint32 word = ((depth & 0xFFFFFFu) << 8) | stencil[texel];
            platform::MemoryCopy(out, &word, 4);
        } else {
            const uint32 word = stencil[texel];
            platform::MemoryCopy(out, &depth, 4);
            platform::MemoryCopy(out + 4, &word, 4);
        }
    }

    upload(format->externalFormat, format->externalType, packed.Data());
    return Result<void>::Success();
}

Result<void> GlesDevice::MatchVertexInputs(GLuint program, const VertexLayout& layout) noexcept {
    // 링크한 프로그램의 active attribute를 배치와 대조한다(RenderBase 10.4절 셰이더 입력 대조).
    // gl_로 시작하는 내장 입력은 건너뛴다. 셰이더가 읽지 않는 속성은 허용한다.
    GLint count = 0;
    gl_.GetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
    for (GLint index = 0; index < count; ++index) {
        char name[128] = {};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        gl_.GetActiveAttrib(program, static_cast<GLuint>(index), sizeof(name), &length, &size,
                            &type, name);
        if (length >= 3 && name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
            continue;
        }

        const GLint location = gl_.GetAttribLocation(program, name);
        Diagnostic message;
        VertexShaderType shaderType = VertexShaderType::Float;
        if (location < 0 || location >= static_cast<GLint>(MaxVertexAttributes) ||
            !ShaderTypeOf(type, shaderType)) {
            message.Text("vertex shader input ")
                .Text(name)
                .Text(" needs layout(location = 0-15) and a float, int or uint type");
            return Reject(message, ErrorCode::InvalidArgument);
        }

        const VertexAttribute* attribute = nullptr;
        for (const VertexAttribute& candidate : layout.attributes) {
            if (candidate.location == static_cast<uint32>(location)) {
                attribute = &candidate;
            }
        }

        if (attribute == nullptr) {
            message.Text("vertex shader reads location ")
                .Number(static_cast<uint32>(location))
                .Text(" which the vertex layout does not provide");
            return Reject(message, ErrorCode::InvalidArgument);
        }

        if (VertexFormatShaderType(attribute->format) != shaderType) {
            message.Text("vertex input location ")
                .Number(static_cast<uint32>(location))
                .Text(": format ")
                .Text(VertexFormatName(attribute->format))
                .Text(" feeds ")
                .Text(VertexShaderTypeName(VertexFormatShaderType(attribute->format)))
                .Text(" but the shader reads ")
                .Text(VertexShaderTypeName(shaderType));
            return Reject(message, ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

Result<GLuint> GlesDevice::CompileShader(GLenum stage, StringView source) noexcept {
    const GLuint shader = gl_.CreateShader(stage);
    const GLchar* text = source.Data();
    const GLint length = static_cast<GLint>(source.ByteCount());
    gl_.ShaderSource(shader, 1, &text, &length);
    gl_.CompileShader(shader);
    GLint compiled = GL_FALSE;
    gl_.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        StoreLog(shader, true);
        gl_.DeleteShader(shader);
        return Result<GLuint>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<GLuint>::Success(shader);
}

void GlesDevice::StoreLog(GLuint object, bool shader) noexcept {
    char log[InfoLogBytes] = {};
    GLsizei written = 0;
    if (shader) {
        gl_.GetShaderInfoLog(object, InfoLogBytes, &written, log);
    } else {
        gl_.GetProgramInfoLog(object, InfoLogBytes, &written, log);
    }

    // 셰이더 컴파일 실패 로그는 파이프라인 생성을 결정하는 쪽이 이 LastError로 남긴다(LoggingSystem
    // 13절).
    auto assigned = lastError_.TryAssign(StringView(log, static_cast<usize>(written)));
    (void)assigned;
}

Result<TextureSubresources>
GlesDevice::GetTextureSubresources(TextureHandle texture) const noexcept {
    const TextureRecord* record = textures_.Find(texture.slot);
    if (record == nullptr) {
        return Result<TextureSubresources>::Failure(ErrorCode::StaleHandle);
    }

    return Result<TextureSubresources>::Success(
        TextureSubresources{record->mipLevels, record->slices});
}

} // namespace rocks::graphics::gles

namespace rocks::graphics {

Result<UniquePtr<Device>> CreateOpenGLDevice(Allocator& allocator, const DeviceDesc& desc,
                                             DeviceCreateReport* report) noexcept {
    return CreateOpenGLDevice(allocator, desc, OpenGLDeviceOptions{}, report);
}

Result<UniquePtr<Device>> CreateOpenGLDevice(Allocator& allocator, const DeviceDesc& desc,
                                             const OpenGLDeviceOptions& options,
                                             DeviceCreateReport* report) noexcept {
    gles::GlesCreateFailure failure;
    auto created = UniquePtr<gles::GlesDevice>::TryMake(allocator, allocator);
    if (!created) {
        failure.Set(created.Error(), "TryMake", 0, "device allocation failed");
        if (report != nullptr) {
            gles::FillReport(failure, *report);
        }

        return Result<UniquePtr<Device>>::Failure(created.Error());
    }

    auto initialized = created.Value()->Initialize(desc, options, failure);
    if (!initialized) {
        if (failure.code == ErrorCode::None) {
            failure.code = initialized.Error();
        }

        if (report != nullptr) {
            gles::FillReport(failure, *report);
        }

        // 생성 실패 로그는 backend를 고르는 호출자가 report로 남긴다(LoggingSystem 13절: 대체
        // 후보가 있으면 Warning, 모든 후보 실패면 Error).
        return Result<UniquePtr<Device>>::Failure(initialized.Error());
    }

    if (report != nullptr) {
        *report = DeviceCreateReport{};
    }

    const DeviceInfo info = created.Value()->Info();
    Log::Info(gles::LogGraphicsOpenGL,
              "Created {Backend} device on {Adapter} (validation {Validation})",
              BackendName(info.backend), info.adapterName, info.validationEnabled);
    return Result<UniquePtr<Device>>::Success(UniquePtr<Device>(Move(created.Value())));
}

} // namespace rocks::graphics
