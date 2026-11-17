#include "GlesDevice.h"

#include <Platform/Clock.h>
#include <Platform/Memory.h>

// 업로드·readback(GX8)·GPU 계측(GX9)·fence·소유권(GX10)이다(GlesDevice.h).
namespace rocks::graphics::gles {

TimestampReadState GlesTimestampSource::ReadTimestamps(uint32 first, uint32 count,
                                                       uint64* ticks) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        GLuint available = GL_FALSE;
        gl->GetQueryObjectuiv(queries[first + index], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available == GL_FALSE) {
            return TimestampReadState::NotReady;
        }
    }

    for (uint32 index = 0; index < count; ++index) {
        GLuint64 value = 0;
        timer->GetQueryObjectui64v(queries[first + index], GL_QUERY_RESULT, &value);
        ticks[index] = value;
    }

    // GL_GPU_DISJOINT_EXT는 읽으면 지워진다. 끊김이 있었으면 이 묶음 전체를 무효로 한다(UE도 끊김이
    // 있으면 대기 중인 결과를 버린다, OpenGLQuery.cpp:250–265).
    if (disjointDetection) {
        GLint disjoint = GL_FALSE;
        gl->GetIntegerv(GL_GPU_DISJOINT, &disjoint);
        if (disjoint != GL_FALSE) {
            return TimestampReadState::Disjoint;
        }
    }

    return TimestampReadState::Ready;
}

// --- 업로드·readback(GX8) ---
// GL은 드라이버가 명령 순서대로 업로드를 실행하므로 쌓아 둘 기록이 없다. 갱신은 바로
// glBufferSubData·glTexSubImage2D로 보내고 staging은 CPU 메모리다. 그래서 기록 중 갱신은 그 뒤의
// 그리기부터 새 내용을 본다(D3D12는 그 프레임 전체, 공개 제약: OpenGLESGraphics 6절).

Result<void> GlesDevice::UpdateBuffer(BufferHandle handle, uint64 offset,
                                      Span<const uint8> data) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    const BufferRecord* record = buffers_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (data.Size() == 0 || offset > record->size || data.Size() > record->size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!MakeCurrentForResources()) {
        return Fail("cannot make the OpenGL context current");
    }

    gl_.BindBuffer(GL_COPY_WRITE_BUFFER, record->buffer);
    gl_.BufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset),
                      static_cast<GLsizeiptr>(data.Size()), data.Data());
    gl_.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
    return Result<void>::Success();
}

Result<void> GlesDevice::UpdateTexture(TextureHandle handle, uint32 mipLevel,
                                       Span<const uint8> pixels) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    const TextureRecord* record = textures_.Find(handle.slot);
    if (record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const uint32 width = MipExtent(record->width, mipLevel);
    const uint32 height = MipExtent(record->height, mipLevel);
    if (record->isView || mipLevel >= record->mipLevels ||
        pixels.Size() != PixelFormatImageBytes(record->format, width, height)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!MakeCurrentForResources()) {
        return Fail("cannot make the OpenGL context current");
    }

    return UploadMip(*record, mipLevel, pixels.Data());
}

Result<GpuFencePoint> GlesDevice::SubmitUploads() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<GpuFencePoint>::Failure(owner.Error());
    }

    if (recording_) {
        return Result<GpuFencePoint>::Failure(ErrorCode::Busy);
    }

    return Result<GpuFencePoint>::Success(GpuFencePoint{});
}

UploadStats GlesDevice::UploadStatistics() const noexcept {
    return UploadStats{};
}

Result<StagingBuffer> GlesDevice::AcquireStaging(uint64 bytes) noexcept {
    return staging_->Acquire(bytes);
}

Result<void> GlesDevice::UploadTextureFromStaging(TextureHandle handle, uint32 mipLevel,
                                                  StagingHandle staging, uint64 offset) noexcept {
    auto memory = staging_->Find(staging);
    const TextureRecord* record = textures_.Find(handle.slot);
    if (!memory || record == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const uint64 bytes = PixelFormatImageBytes(record->format, MipExtent(record->width, mipLevel),
                                               MipExtent(record->height, mipLevel));
    if (offset > memory.Value().size || bytes > memory.Value().size - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return UpdateTexture(
        handle, mipLevel,
        Span<const uint8>(memory.Value().data + offset, static_cast<usize>(bytes)));
}

Result<void> GlesDevice::UploadBufferFromStaging(BufferHandle handle, uint64 destinationOffset,
                                                 StagingHandle staging, uint64 sourceOffset,
                                                 uint64 size) noexcept {
    auto memory = staging_->Find(staging);
    if (!memory) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (sourceOffset > memory.Value().size || size > memory.Value().size - sourceOffset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return UpdateBuffer(
        handle, destinationOffset,
        Span<const uint8>(memory.Value().data + sourceOffset, static_cast<usize>(size)));
}

void GlesDevice::ReleaseStaging(StagingHandle staging) noexcept {
    // client 메모리 업로드는 반환하면 끝났으므로 바로 재사용할 수 있다.
    staging_->Release(staging);
    staging_->Reclaim(NumericLimits<uint64>::Max());
}

StagingStats GlesDevice::StagingStatistics() const noexcept {
    return staging_->Stats();
}

Result<ReadbackRequest> GlesDevice::RequestReadback(TextureHandle target) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<ReadbackRequest>::Failure(owner.Error());
    }

    const TextureRecord* record = textures_.Find(target.slot);
    if (record == nullptr) {
        return Result<ReadbackRequest>::Failure(ErrorCode::StaleHandle);
    }

    if (record->framebuffer == 0 || recording_) {
        return Result<ReadbackRequest>::Failure(recording_ ? ErrorCode::Busy
                                                           : ErrorCode::InvalidArgument);
    }

    if (!MakeCurrentForResources()) {
        return FailWith<ReadbackRequest>("cannot make the OpenGL context current");
    }

    // pixel pack buffer로 비동기 복사하고(UE OpenGLTexture.cpp:865–902), fence 뒤에 map해서
    // 읽는다(ES에는 glGetBufferSubData가 없다). 읽기 조합이 업로드 배치와 다르면 표준 조합으로 읽고
    // TryReadback이 Rocks 배치로 옮긴다.
    const GlesTextureFormat* format = FindTextureFormat(record->format);
    ReadbackRecord readback;
    readback.width = record->width;
    readback.height = record->height;
    readback.format = record->format;
    const uint64 bytes = PixelFormatImageBytes(record->format, record->width, record->height);
    readback.bytes = bytes;
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, record->framebuffer);
    bool direct = false;
    const GlesReadPair pair = ChooseReadPair(*format, direct);
    readback.convert = !direct;
    readback.readBytes =
        direct ? bytes : static_cast<uint64>(record->width) * record->height * pair.texelBytes;
    gl_.GenBuffers(1, &readback.buffer);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, readback.buffer);
    gl_.BufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(readback.readBytes), nullptr,
                   GL_STREAM_READ);
    gl_.ReadBuffer(GL_COLOR_ATTACHMENT0);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 1);
    // RenderTarget 저장소의 첫 행이 이미지 위쪽이다(ReadPixels와 같다).
    gl_.ReadPixels(0, 0, static_cast<GLsizei>(record->width), static_cast<GLsizei>(record->height),
                   pair.format, pair.type, nullptr);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    const uint64 previous = fenceValue_;
    readback.fenceValue = Submit();
    if (readback.fenceValue == previous) {
        // context를 잃어 장치 손실로 게시했다. 버퍼는 context 삭제 때 드라이버가 회수한다.
        return Result<ReadbackRequest>::Failure(ErrorCode::Destroyed);
    }

    auto added = readbacks_.TryAdd(readback);
    if (!added) {
        gl_.DeleteBuffers(1, &readback.buffer);
        return Result<ReadbackRequest>::Failure(added.Error());
    }

    ReadbackRequest request;
    request.handle = ReadbackHandle{added.Value()};
    request.completion = timeline_.Point(readback.fenceValue);
    request.bytes = bytes;
    return Result<ReadbackRequest>::Success(request);
}

Result<bool> GlesDevice::TryReadback(ReadbackHandle handle, Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<bool>::Failure(owner.Error());
    }

    const ReadbackRecord* record = readbacks_.Find(handle.slot);
    if (record == nullptr) {
        return Result<bool>::Failure(ErrorCode::StaleHandle);
    }

    const usize bytes = static_cast<usize>(record->bytes);
    if (out.Size() != bytes) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (timeline_.Completed() < record->fenceValue) {
        if (timeline_.IsLost()) {
            return Result<bool>::Failure(ErrorCode::Destroyed);
        }

        return Result<bool>::Success(false);
    }

    if (!MakeCurrentForResources()) {
        return FailWith<bool>("cannot make the OpenGL context current");
    }

    // fence가 끝났으므로 map은 GPU를 기다리지 않는다.
    gl_.BindBuffer(GL_COPY_READ_BUFFER, record->buffer);
    const void* mapped = gl_.MapBufferRange(
        GL_COPY_READ_BUFFER, 0, static_cast<GLsizeiptr>(record->readBytes), GL_MAP_READ_BIT);
    if (mapped == nullptr) {
        gl_.BindBuffer(GL_COPY_READ_BUFFER, 0);
        return FailWith<bool>("glMapBufferRange failed on the readback buffer");
    }

    if (record->convert) {
        ConvertReadTexels(*FindTextureFormat(record->format), static_cast<const uint8*>(mapped),
                          out.Data(), static_cast<uint64>(record->width) * record->height);
    } else {
        platform::MemoryCopy(out.Data(), mapped, bytes);
    }
    const bool intact = gl_.UnmapBuffer(GL_COPY_READ_BUFFER) == GL_TRUE;
    gl_.BindBuffer(GL_COPY_READ_BUFFER, 0);
    if (!intact) {
        // map 중 내용이 깨졌다(ES 3.2: 화면 모드 변경 등). 결과를 쓰지 않는다.
        return FailWith<bool>("readback buffer contents were lost while mapped");
    }

    return Result<bool>::Success(true);
}

void GlesDevice::ReleaseReadback(ReadbackHandle handle) noexcept {
    if (!ownership_.Check()) {
        return;
    }

    const ReadbackRecord* record = readbacks_.Find(handle.slot);
    if (record == nullptr) {
        return;
    }

    RetiredObject entry;
    entry.kind = RetiredKind::Buffer;
    entry.objects[0] = record->buffer;
    Defer(entry);
    auto removed = readbacks_.TryRemove(handle.slot);
    (void)removed;
}

// --- GPU 계측(GX9) ---

void GlesDevice::InitializeTimestamps() noexcept {
    // ES는 GL_EXT_disjoint_timer_query가 있어야 timestamp가 있다(끊김 표시 포함). 데스크톱 core
    // context는 GL 3.3 core timer query다(끊김 표시 없음). 둘 다 없거나 counter bit가 0이면
    // timestamp를 "없음"으로 공개한다(장치 생성은 실패하지 않는다, 추정값을 만들지 않는다).
    const bool desktop = platform_->Info().desktopContext;
    if (!features_.disjointTimerQuery && !desktop) {
        return;
    }

    const GlesTimerQueryNames& names =
        features_.disjointTimerQuery ? EsTimerQueryNames : DesktopTimerQueryNames;
    GlesPlatform* platform = platform_.Get();
    auto loader = [](const char* name, void* context) noexcept -> void* {
        return static_cast<GlesPlatform*>(context)->ProcAddress(name);
    };
    if (!LoadTimerQueryFunctions(timer_, names, loader, platform)) {
        return;
    }

    GLint bits = 0;
    timer_.GetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);

    if (bits <= 0 || !timestamps_.Reset(TimestampFrameSlots, MaxTimestampsPerFrame)) {
        return;
    }

    gl_.GenQueries(static_cast<GLsizei>(TimestampQueryCount), timestampQueries_);
    timestampSource_.gl = &gl_;
    timestampSource_.timer = &timer_;
    timestampSource_.queries = timestampQueries_;
    timestampSource_.disjointDetection = features_.disjointTimerQuery;
    timestampsReady_ = true;
}

usize GlesDevice::CollectTimestamps(Span<TimestampSample> out) noexcept {
    if (!ownership_.IsOwner() || !timestampsReady_ || !MakeCurrentForResources()) {
        return 0;
    }

    timestamps_.Harvest(timeline_.Completed(), timestampSource_);
    return timestamps_.Collect(out);
}

Result<ClockCalibration> GlesDevice::CalibrateClocks() noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return Result<ClockCalibration>::Failure(owner.Error());
    }

    if (!timestampsReady_) {
        return Result<ClockCalibration>::Failure(ErrorCode::Unsupported);
    }

    if (timeline_.IsLost()) {
        return Result<ClockCalibration>::Failure(ErrorCode::Destroyed);
    }

    if (!MakeCurrentForResources()) {
        return FailWith<ClockCalibration>("cannot make the OpenGL context current");
    }

    // GL_TIMESTAMP 조회(앞 명령이 서버에 닿은 시각, EXT_disjoint_timer_query는 glGetInteger64v로
    // 읽는다)를 CPU tick 두 번으로 감싸고 가운데를 같은 순간으로 본다. 조회 지연의 절반이 오차
    // 반경이다.
    const uint64 before = platform::MonotonicTicks();
    GLint64 gpu = 0;
    gl_.GetInteger64v(GL_TIMESTAMP, &gpu);
    const uint64 after = platform::MonotonicTicks();
    const uint64 frequency = platform::MonotonicTickFrequency();
    ClockCalibration calibration;
    calibration.gpuTicks = static_cast<uint64>(gpu);
    calibration.cpuTicks = before + (after - before) / 2;
    calibration.cpuNanoseconds = TicksToNanoseconds(calibration.cpuTicks, frequency);
    calibration.gpuFrequency = TimestampNanosecondFrequency;
    calibration.uncertaintyNanoseconds = TicksToNanoseconds(after - before, frequency) / 2 + 1;
    return Result<ClockCalibration>::Success(calibration);
}

// --- fence(GX7, 스레드 안전) ---

GpuFencePoint GlesDevice::LastSubmittedFence() const noexcept {
    return timeline_.LastSubmitted();
}

Result<bool> GlesDevice::IsFenceComplete(GpuFencePoint point) const noexcept {
    return timeline_.IsComplete(point);
}

Result<platform::WaitResult> GlesDevice::WaitFence(GpuFencePoint point, uint32 timeoutMs) noexcept {
    return timeline_.Wait(point, timeoutMs);
}

Result<void> GlesDevice::NotifyFence(GpuFencePoint point, FenceCallback callback,
                                     void* context) noexcept {
    return timeline_.Notify(point, callback, context);
}

bool GlesDevice::IsLost() const noexcept {
    return timeline_.IsLost();
}

// --- 소유권(GX10) ---

bool GlesDevice::IsOwnerThread() const noexcept {
    return ownership_.IsOwner();
}

Result<void> GlesDevice::ReleaseOwnership() noexcept {
    if (!ownership_.IsOwner()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // context는 한 번에 한 스레드에만 붙는다. 새 소유 스레드가 붙일 수 있게 이 스레드에서 뗀다.
    platform_->ReleaseCurrent();
    return ownership_.Release();
}

Result<void> GlesDevice::AcquireOwnership() noexcept {
    const bool alreadyOwner = ownership_.IsOwner();
    auto acquired = ownership_.Acquire();
    if (!acquired) {
        return acquired;
    }

    if (!alreadyOwner && !platform_->MakeCurrent()) {
        auto released = ownership_.Release();
        (void)released;
        return Fail("cannot make the OpenGL context current on the new owner thread");
    }

    return Result<void>::Success();
}

uint64 GlesDevice::RejectedCalls() const noexcept {
    return ownership_.Rejected();
}

// 텍스처 mip 하나의 plane 하나를 읽는다. 색 렌더 타깃 형식은 framebuffer로, 깊이·스텐실 plane은
// 셰이더로 옮겨 읽는다. ES는 블록 압축 텍스처를 읽을 수 없고(glGetCompressedTexImage 없음) 색 렌더
// 타깃이 아닌 형식은 framebuffer에 붙일 수 없으므로 Unsupported다(PixelFormat.ko.md 14.2절).
Result<void> GlesDevice::ReadTexture(TextureHandle handle, uint32 mipLevel, uint32 plane,
                                     Span<uint8> out) noexcept {
    auto owner = ownership_.Check();
    if (!owner) {
        return owner;
    }

    const TextureRecord* found = textures_.Find(handle.slot);
    if (found == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (recording_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const TextureRecord record = *found;
    const uint32 width = MipExtent(record.width, mipLevel);
    const uint32 height = MipExtent(record.height, mipLevel);
    const uint64 bytes = PixelFormatPlaneImageBytes(record.format, plane, width, height);
    if (record.isView || mipLevel >= record.mipLevels ||
        plane >= PixelFormatPlaneCount(record.format) || bytes == 0 || out.Size() != bytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 색 형식은 framebuffer에 붙일 수 있어야 읽는다(렌더 타깃 능력). A8은 렌더 타깃 능력을 주지
    // 않지만 저장소 R8은 색 렌더 타깃이므로 읽는다.
    const GlesTextureFormat* format = FindTextureFormat(record.format);
    const bool colorReadable =
        format->read != GlesReadClass::None && format->read != GlesReadClass::DepthStencil &&
        (HasPixelFormatCaps(GetPixelFormatCaps(record.format), PixelFormatCaps::RenderTarget) ||
         format->Is(GlesAlphaSwizzle));
    if (format->read != GlesReadClass::DepthStencil && !colorReadable) {
        Diagnostic message;
        message.Text("ReadTexture of ")
            .Text(PixelFormatName(record.format))
            .Text(" is not supported by OpenGL ES (not color-renderable or block compressed)");
        return Reject(message, ErrorCode::Unsupported);
    }

    if (!MakeCurrentForResources()) {
        return Fail("cannot make the OpenGL context current");
    }

    if (format->read == GlesReadClass::DepthStencil) {
        return ReadDepthPlane(record, mipLevel, plane, out);
    }

    return ReadColorLevel(record, mipLevel, out);
}

GlesReadPair GlesDevice::ChooseReadPair(const GlesTextureFormat& format, bool& direct) noexcept {
    // 구현 선택 조합(ES 3.2 16.1.2절)이 업로드 배치와 같으면 그대로 읽는다(변환 없음). 아니면 형식
    // 계열의 표준 조합으로 읽고 Rocks 배치로 옮긴다.
    GLint implementationFormat = 0;
    GLint implementationType = 0;
    gl_.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &implementationFormat);
    gl_.GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &implementationType);
    direct = static_cast<GLenum>(implementationFormat) == format.externalFormat &&
             static_cast<GLenum>(implementationType) == format.externalType;
    if (direct) {
        return {format.externalFormat, format.externalType,
                GetPixelFormatInfo(format.format).blockBytes};
    }

    return CanonicalReadPair(format.read);
}

Result<void> GlesDevice::ReadColorLevel(const TextureRecord& record, uint32 mipLevel,
                                        Span<uint8> out) noexcept {
    const GlesTextureFormat* format = FindTextureFormat(record.format);
    const uint32 width = MipExtent(record.width, mipLevel);
    const uint32 height = MipExtent(record.height, mipLevel);
    GLuint framebuffer = 0;
    gl_.GenFramebuffers(1, &framebuffer);
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    gl_.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             record.texture, static_cast<GLint>(mipLevel));
    if (gl_.CheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        gl_.DeleteFramebuffers(1, &framebuffer);
        return Fail("readback framebuffer is incomplete");
    }

    bool direct = false;
    const GlesReadPair pair = ChooseReadPair(*format, direct);
    Result<void> outcome = Result<void>::Success();
    if (direct) {
        outcome = ReadFramebuffer(framebuffer, width, height, pair, out.Data(), out.Size());
    } else {
        const usize texels = static_cast<usize>(width) * height;
        Array<uint8> read(*stagingSource_.allocator);
        if (!read.TryResize(texels * pair.texelBytes)) {
            outcome = Result<void>::Failure(ErrorCode::OutOfMemory);
        } else {
            outcome = ReadFramebuffer(framebuffer, width, height, pair, read.Data(), read.Size());
            if (outcome) {
                ConvertReadTexels(*format, read.Data(), out.Data(), texels);
            }
        }
    }

    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_.DeleteFramebuffers(1, &framebuffer);
    return outcome;
}

Result<void> GlesDevice::ReadFramebuffer(GLuint framebuffer, uint32 width, uint32 height,
                                         const GlesReadPair& pair, uint8* out,
                                         uint64 bytes) noexcept {
    // pixel pack buffer로 읽고 map한다. 동기화 없는 map이 아니므로 map이 복사 완료를 기다린다.
    GLuint buffer = 0;
    gl_.GenBuffers(1, &buffer);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
    gl_.BufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    gl_.ReadBuffer(GL_COLOR_ATTACHMENT0);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl_.ReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), pair.format,
                   pair.type, nullptr);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl_.BindBuffer(GL_COPY_READ_BUFFER, buffer);
    const void* mapped =
        gl_.MapBufferRange(GL_COPY_READ_BUFFER, 0, static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT);
    Result<void> outcome = Result<void>::Success();
    if (mapped == nullptr) {
        outcome = Fail("glMapBufferRange failed on the texture readback buffer");
    } else {
        platform::MemoryCopy(out, mapped, static_cast<usize>(bytes));
        if (gl_.UnmapBuffer(GL_COPY_READ_BUFFER) != GL_TRUE) {
            outcome = Fail("texture readback buffer contents were lost");
        }
    }

    gl_.BindBuffer(GL_COPY_READ_BUFFER, 0);
    gl_.DeleteBuffers(1, &buffer);
    return outcome;
}

namespace {

// 깊이·스텐실 plane 읽기 셰이더다. 화면을 덮는 삼각형이 texel마다 한 번 값을 옮긴다.
constexpr char DepthReadVertex[] = R"(#version 320 es
void main() {
    vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr char DepthReadPixel[] = R"(#version 320 es
precision highp float;
precision highp int;
layout(binding = 0) uniform highp sampler2D source;
uniform int level;
layout(location = 0) out highp vec4 result;
void main() {
    result = vec4(texelFetch(source, ivec2(gl_FragCoord.xy), level).r, 0.0, 0.0, 1.0);
}
)";

constexpr char StencilReadPixel[] = R"(#version 320 es
precision highp float;
precision highp int;
layout(binding = 0) uniform highp usampler2D source;
uniform int level;
layout(location = 0) out highp uvec4 result;
void main() {
    result = uvec4(texelFetch(source, ivec2(gl_FragCoord.xy), level).r, 0u, 0u, 1u);
}
)";

template <usize Count>
StringView SourceText(const char (&text)[Count]) noexcept {
    return StringView(text, Count - 1);
}

} // namespace

Result<void> GlesDevice::PrepareDepthReads() noexcept {
    if (depthReadProgram_ != 0) {
        return Result<void>::Success();
    }

    const StringView pixels[2] = {SourceText(DepthReadPixel), SourceText(StencilReadPixel)};
    GLuint programs[2] = {};
    for (uint32 index = 0; index < 2; ++index) {
        auto vertex = CompileShader(GL_VERTEX_SHADER, SourceText(DepthReadVertex));
        if (!vertex) {
            gl_.DeleteProgram(programs[0]);
            return Fail("depth readback vertex shader failed to compile");
        }

        auto pixel = CompileShader(GL_FRAGMENT_SHADER, pixels[index]);
        if (!pixel) {
            gl_.DeleteShader(vertex.Value());
            gl_.DeleteProgram(programs[0]);
            return Fail("depth readback pixel shader failed to compile");
        }

        programs[index] = gl_.CreateProgram();
        gl_.AttachShader(programs[index], vertex.Value());
        gl_.AttachShader(programs[index], pixel.Value());
        gl_.LinkProgram(programs[index]);
        gl_.DeleteShader(vertex.Value());
        gl_.DeleteShader(pixel.Value());
        GLint linked = GL_FALSE;
        gl_.GetProgramiv(programs[index], GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            StoreLog(programs[index], false);
            gl_.DeleteProgram(programs[0]);
            gl_.DeleteProgram(programs[1]);
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }

    depthReadProgram_ = programs[0];
    stencilReadProgram_ = programs[1];
    depthReadLevel_ = gl_.GetUniformLocation(depthReadProgram_, "level");
    stencilReadLevel_ = gl_.GetUniformLocation(stencilReadProgram_, "level");
    gl_.GenVertexArrays(1, &readVertexArray_);
    // 깊이·정수 텍스처의 완전성 조건(최근접 필터)을 지키는 샘플러다. texelFetch는 필터를 쓰지
    // 않는다.
    gl_.GenSamplers(1, &readSampler_);
    gl_.SamplerParameteri(readSampler_, GL_TEXTURE_MIN_FILTER,
                          static_cast<GLint>(GL_NEAREST_MIPMAP_NEAREST));
    gl_.SamplerParameteri(readSampler_, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_NEAREST));
    return Result<void>::Success();
}

Result<void> GlesDevice::ReadDepthPlane(const TextureRecord& record, uint32 mipLevel, uint32 plane,
                                        Span<uint8> out) noexcept {
    auto prepared = PrepareDepthReads();
    if (!prepared) {
        return prepared;
    }

    const PixelFormatInfo& info = GetPixelFormatInfo(record.format);
    const bool stencil = plane == 1;
    const PixelFormat depthFormat =
        info.kind == PixelFormatKind::DepthStencil ? info.planes[0].format : record.format;
    const uint32 width = MipExtent(record.width, mipLevel);
    const uint32 height = MipExtent(record.height, mipLevel);
    // 깊이는 R32F, 스텐실은 R32UI 색 대상에 texel마다 옮긴다(둘 다 ES 3.2 core 색 렌더 타깃).
    GLuint target = 0;
    gl_.GenTextures(1, &target);
    gl_.ActiveTexture(GL_TEXTURE0 + UploadTextureUnit);
    gl_.BindTexture(GL_TEXTURE_2D, target);
    gl_.TexStorage2D(GL_TEXTURE_2D, 1, stencil ? GL_R32UI : GL_R32F, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height));
    const GLuint framebuffer = CreateColorFramebuffer(target);
    if (framebuffer == 0) {
        gl_.DeleteTextures(1, &target);
        return Fail("depth readback framebuffer is incomplete");
    }

    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    gl_.Viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    gl_.Disable(GL_BLEND);
    gl_.Disable(GL_CULL_FACE);
    gl_.Disable(GL_DEPTH_TEST);
    gl_.Disable(GL_SCISSOR_TEST);
    gl_.UseProgram(stencil ? stencilReadProgram_ : depthReadProgram_);
    gl_.Uniform1i(stencil ? stencilReadLevel_ : depthReadLevel_, static_cast<GLint>(mipLevel));
    gl_.BindVertexArray(readVertexArray_);
    gl_.ActiveTexture(GL_TEXTURE0 + PixelTextureUnit);
    gl_.BindTexture(GL_TEXTURE_2D, record.texture);
    gl_.BindSampler(PixelTextureUnit, readSampler_);
    if (record.depthStencilTexture) {
        gl_.TexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE,
                          static_cast<GLint>(stencil ? GL_STENCIL_INDEX : GL_DEPTH_COMPONENT));
    }

    gl_.DrawArraysInstanced(GL_TRIANGLES, 0, 3, 1);
    gl_.BindVertexArray(0);
    gl_.UseProgram(0);
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    const GlesReadPair pair = stencil ? GlesReadPair{GL_RGBA_INTEGER, GL_UNSIGNED_INT, 16}
                                      : GlesReadPair{GL_RGBA, GL_FLOAT, 16};
    const usize texels = static_cast<usize>(width) * height;
    Array<uint8> read(*stagingSource_.allocator);
    Result<void> outcome = Result<void>::Success();
    if (!read.TryResize(texels * pair.texelBytes)) {
        outcome = Result<void>::Failure(ErrorCode::OutOfMemory);
    } else {
        outcome = ReadFramebuffer(framebuffer, width, height, pair, read.Data(), read.Size());
    }

    // 정규화 깊이는 저장 정수로 되돌린다(D16 ×65535, D24 ×16777215, 반올림). D32F는 float 그대로다.
    for (usize texel = 0; outcome && texel < texels; ++texel) {
        const uint8* value = read.Data() + texel * pair.texelBytes;
        if (stencil) {
            out[texel] = value[0];
            continue;
        }

        float depth = 0.0f;
        platform::MemoryCopy(&depth, value, 4);
        const double clamped = depth < 0.0f ? 0.0 : (depth > 1.0f ? 1.0 : depth);
        if (depthFormat == PixelFormat::D16Unorm) {
            const auto stored = static_cast<uint16>(clamped * 65535.0 + 0.5);
            platform::MemoryCopy(out.Data() + texel * 2, &stored, 2);
        } else if (depthFormat == PixelFormat::D24Unorm) {
            const auto stored = static_cast<uint32>(clamped * 16777215.0 + 0.5);
            platform::MemoryCopy(out.Data() + texel * 4, &stored, 4);
        } else {
            platform::MemoryCopy(out.Data() + texel * 4, &depth, 4);
        }
    }

    gl_.DeleteFramebuffers(1, &framebuffer);
    gl_.DeleteTextures(1, &target);
    return outcome;
}

} // namespace rocks::graphics::gles
