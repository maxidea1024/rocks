#include "RenderGraphInternal.h"

// 그래프 안 업로드 선언이다(Docs/Design/RenderBase 8절 RG2, UE FRDGBuilder::QueueBufferUpload
// RC/Public/RenderGraphBuilder.h:309–333, RC/Private/RenderGraphBuilder.cpp:2691–2837). 데이터는
// 선언 때 프레임 arena에 복사해 두고, Deferred는 그래프 시작(첫 기록을 열기 전)에, Immediate는 그
// 자원의 첫 pass 직전에 장치 업로드 경로(Device::UpdateBuffer·UpdateTexture, 업로드 ring)로 올린다.
namespace rocks::render {

using namespace detail;

namespace {

// 업로드 공통 검사다: 그래프 handle, 만든 자원, 아직 어느 pass도 접근을 선언하지 않음.
Result<void> CheckUploadTarget(GraphState& state, GraphResourceKind kind, uint32 index,
                               uint32 graph) noexcept {
    if (state.executed) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (graph != state.serial || index >= state.resources.Size() ||
        state.resources[index].kind != kind) {
        return Result<void>::Failure(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                                      "upload into a resource from another graph"));
    }

    const ResourceRecord& resource = state.resources[index];
    if (resource.imported) {
        return Result<void>::Failure(
            RecordGraphError(state, ErrorCode::InvalidArgument, true,
                             "'{}' is imported; graph uploads go to graph-created resources "
                             "(update imported resources through the render lane)",
                             resource.name));
    }

    if (resource.accessed) {
        return Result<void>::Failure(
            RecordGraphError(state, ErrorCode::InvalidArgument, true,
                             "upload into '{}' after a pass declared it; uploads run before the "
                             "graph's passes and must be declared first",
                             resource.name));
    }

    return Result<void>::Success();
}

// data를 프레임 arena에 복사한다.
Result<uint8*> CopyToArena(FrameArena& arena, Span<const uint8> data) noexcept {
    auto memory = arena.TryAllocate(data.Size(), alignof(uint64));
    if (!memory) {
        return Result<uint8*>::Failure(memory.Error());
    }

    auto* bytes = static_cast<uint8*>(memory.Value());
    platform::MemoryCopy(bytes, data.Data(), data.Size());
    return Result<uint8*>::Success(bytes);
}

} // namespace

Result<void> RenderGraph::UploadBuffer(GraphBuffer buffer, Span<const uint8> data,
                                       uint64 offset) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    GraphState& state = *state_;
    auto checked = CheckUploadTarget(state, GraphResourceKind::Buffer, buffer.index, buffer.graph);
    if (!checked) {
        return checked;
    }

    ResourceRecord& resource = state.resources[buffer.index];
    const uint64 size = resource.bufferDesc.size;
    if (data.IsEmpty() || offset > size || data.Size() > size - offset) {
        return Result<void>::Failure(
            RecordGraphError(state, ErrorCode::InvalidArgument, true,
                             "upload of {} bytes at offset {} does not fit buffer '{}' ({} bytes)",
                             data.Size(), offset, resource.name, size));
    }

    auto copied = CopyToArena(arena_, data);
    if (!copied) {
        return Result<void>::Failure(copied.Error());
    }

    BufferUpload upload;
    upload.resource = buffer.index;
    upload.offset = offset;
    upload.data = copied.Value();
    upload.size = data.Size();
    auto pushed = state.bufferUploads.TryPushBack(arena_, upload);
    if (!pushed) {
        return pushed;
    }

    resource.uploadTarget = true;
    resource.produced = true;
    state.tracks[resource.firstTrack].uploaded = true;
    return Result<void>::Success();
}

Result<void> RenderGraph::UploadTexture(GraphTexture texture, uint32 mipLevel,
                                        Span<const uint8> pixels) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    GraphState& state = *state_;
    auto checked =
        CheckUploadTarget(state, GraphResourceKind::Texture, texture.index, texture.graph);
    if (!checked) {
        return checked;
    }

    ResourceRecord& resource = state.resources[texture.index];
    const graphics::TextureDesc& desc = resource.textureDesc;
    if (desc.usage != graphics::TextureUsage::Sampled) {
        return Result<void>::Failure(RecordGraphError(
            state, ErrorCode::Unsupported, false,
            "'{}' is a render target graph texture; uploads go to sampled graph textures (a "
            "render target upload needs a buffer-to-texture copy command, GX4)",
            resource.name));
    }

    // mip 하나는 모든 slice·깊이 면이다(장치 UpdateTexture와 같은 배치).
    const uint64 expected = graphics::TextureMipBytes(desc, mipLevel);
    if (mipLevel >= resource.mipLevels || pixels.Size() != expected) {
        return Result<void>::Failure(RecordGraphError(
            state, ErrorCode::InvalidArgument, true,
            "upload of {} bytes into mip {} of '{}' does not match the mip ({} mips)",
            pixels.Size(), mipLevel, resource.name, resource.mipLevels));
    }

    SubresourceTrack& track = state.tracks[resource.firstTrack + mipLevel];
    if (track.uploaded) {
        return Result<void>::Failure(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                                      "mip {} of '{}' is uploaded twice", mipLevel,
                                                      resource.name));
    }

    // 초기 데이터 자리를 처음 업로드 때 잡는다(장치 생성 초기 데이터 배치: mip 0부터 빈틈 없이).
    if (resource.uploadPixels == nullptr) {
        const uint64 chain = graphics::TextureDataBytes(desc);
        // CreateTexture가 넘침을 이미 거르지만 arena 할당 전에 다시 본다.
        if (chain == 0 || chain > NumericLimits<usize>::Max()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto memory = arena_.TryAllocate(static_cast<usize>(chain), alignof(uint64));
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        resource.uploadPixels = static_cast<uint8*>(memory.Value());
    }

    uint64 offset = 0;
    for (uint32 mip = 0; mip < mipLevel; ++mip) {
        offset += graphics::TextureMipBytes(desc, mip);
    }

    platform::MemoryCopy(resource.uploadPixels + offset, pixels.Data(), pixels.Size());
    // 그 mip의 모든 slice가 내용을 받았다(slice 0 칸이 중복 검사를 맡는다).
    for (uint32 slice = 0; slice < resource.slices; ++slice) {
        state.tracks[resource.firstTrack + SubresourceIndex(resource, mipLevel, slice)].uploaded =
            true;
    }
    resource.uploadTarget = true;
    resource.produced = true;
    return Result<void>::Success();
}

} // namespace rocks::render
