#include "RenderGraphInternal.h"

#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <RenderBase/RenderLane.h>

namespace rocks::render {

using graphics::ResourceAccess;
using namespace detail;

namespace {

// 그래프 일련번호다(다른 그래프 handle 검사). 0은 쓰지 않는다.
uint32 NextGraphSerial() noexcept {
    static uint32 counter = 0;
    uint32 serial = platform::AtomicFetchAdd(&counter, 1u) + 1u;
    if (serial == 0) {
        serial = platform::AtomicFetchAdd(&counter, 1u) + 1u;
    }

    return serial;
}

// 그래프 수명 arena는 필수다(없으면 그래프를 만들 수 없는 호출자 오류다).
FrameArena& RequireArena(const RenderGraphDesc& desc) noexcept {
    if (desc.arena == nullptr) {
        platform::Fatal("RenderGraphDesc::arena is null");
    }

    return *desc.arena;
}

// 최대 mip 수다(장치 텍스처 한계와 같다, 2^15 크기까지).
constexpr uint32 MaxGraphTextureMips = 16;

// 접근 이름을 담을 임시 버퍼 크기다.
constexpr usize AccessNameBytes = 160;

const char* PassKindName(GraphPassKind kind) noexcept {
    switch (kind) {
    case GraphPassKind::Raster:
        return "raster";
    case GraphPassKind::Compute:
        return "compute";
    case GraphPassKind::AsyncCompute:
        return "async compute";
    case GraphPassKind::Copy:
        return "copy";
    }

    return "unknown";
}

// pass 종류가 허용하지 않는 접근 비트다(GraphPassKind 주석). 렌더 타깃·깊이 attachment는 렌더 타깃
// binding으로만 선언한다.
ResourceAccess ForbiddenAccess(GraphPassKind kind) noexcept {
    const ResourceAccess attachments =
        ResourceAccess::RenderTarget | graphics::DepthAccessMask | ResourceAccess::Present;
    switch (kind) {
    case GraphPassKind::Raster:
        return attachments | graphics::UavAccessMask | ResourceAccess::SrvCompute |
               ResourceAccess::CopySource | ResourceAccess::CopyDestination;
    case GraphPassKind::Compute:
    case GraphPassKind::AsyncCompute:
        return attachments | ResourceAccess::VertexOrIndex | ResourceAccess::SrvGraphics |
               ResourceAccess::UavGraphics | ResourceAccess::CopySource |
               ResourceAccess::CopyDestination;
    case GraphPassKind::Copy:
        return ~(ResourceAccess::CopySource | ResourceAccess::CopyDestination);
    }

    return ResourceAccess::Unknown;
}

GraphAccessFlags CombineFlags(GraphAccessFlags left, GraphAccessFlags right) noexcept {
    // 생략 표시는 두 선언이 모두 가질 때만 남는다(하나라도 순서가 필요하면 barrier).
    return left == GraphAccessFlags::NoUavBarrier && right == GraphAccessFlags::NoUavBarrier
               ? GraphAccessFlags::NoUavBarrier
               : GraphAccessFlags::None;
}

} // namespace

namespace detail {

void PublishGraphError(GraphState& state, usize length) noexcept {
    state.messageLength = length;
    state.message[length] = '\0';
    Log::Warning(LogRenderGraph, "Render graph: {Message}", StringView(state.message, length));
}

} // namespace detail

// --- GraphPassAccesses ---

GraphPassAccesses::GraphPassAccesses(RenderGraph& graph) noexcept : arena_(graph.Arena()) {
}

void GraphPassAccesses::Push(const GraphResourceAccess& access) noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    if (size_ == capacity_) {
        const usize capacity = capacity_ == 0 ? 8 : capacity_ * 2;
        auto memory = arena_.TryAllocate(capacity * sizeof(GraphResourceAccess),
                                         alignof(GraphResourceAccess));
        if (!memory) {
            error_ = memory.Error();
            return;
        }

        auto* replacement = static_cast<GraphResourceAccess*>(memory.Value());
        if (size_ != 0) {
            platform::MemoryCopy(replacement, data_, size_ * sizeof(GraphResourceAccess));
        }

        data_ = replacement;
        capacity_ = capacity;
    }

    platform::MemoryCopy(data_ + size_, &access, sizeof(GraphResourceAccess));
    ++size_;
}

GraphPassAccesses& GraphPassAccesses::Texture(GraphTexture texture, ResourceAccess access,
                                              const graphics::SubresourceRange& range,
                                              GraphAccessFlags flags) noexcept {
    GraphResourceAccess entry;
    entry.kind = GraphResourceKind::Texture;
    entry.resource = texture.index;
    entry.graph = texture.graph;
    entry.range = range;
    entry.access = access;
    entry.flags = flags;
    Push(entry);
    return *this;
}

GraphPassAccesses& GraphPassAccesses::Buffer(GraphBuffer buffer, ResourceAccess access,
                                             GraphAccessFlags flags) noexcept {
    GraphResourceAccess entry;
    entry.kind = GraphResourceKind::Buffer;
    entry.resource = buffer.index;
    entry.graph = buffer.graph;
    entry.access = access;
    entry.flags = flags;
    Push(entry);
    return *this;
}

GraphPassAccesses& GraphPassAccesses::RenderTarget(GraphTexture texture, GraphLoadAction load,
                                                   const graphics::ClearColor& clear) noexcept {
    target_ = {};
    target_.kind = GraphTargetKind::Texture;
    target_.texture = texture;
    target_.load = load;
    target_.clear = clear;
    return *this;
}

GraphPassAccesses& GraphPassAccesses::SwapChainTarget(graphics::SwapChainHandle swapChain,
                                                      const graphics::ClearColor& clear) noexcept {
    target_ = {};
    target_.kind = GraphTargetKind::SwapChain;
    target_.swapChain = swapChain;
    target_.clear = clear;
    return *this;
}

// --- GraphPassContext ---

graphics::Device& GraphPassContext::Device() const noexcept {
    return *graph_.state_->device;
}

graphics::TextureHandle GraphPassContext::Texture(GraphTexture texture) const noexcept {
    const GraphState& state = *graph_.state_;
    if (texture.graph != state.serial || texture.index >= state.resources.Size() ||
        state.resources[texture.index].kind != GraphResourceKind::Texture) {
        return {};
    }

    const PassRecord& pass = state.passes[pass_];
    for (uint32 use = pass.firstUse; use < pass.firstUse + pass.useCount; ++use) {
        if (state.uses[use].resource == texture.index) {
            return graphics::TextureHandle{state.resources[texture.index].device};
        }
    }

    return {};
}

graphics::BufferHandle GraphPassContext::Buffer(GraphBuffer buffer) const noexcept {
    const GraphState& state = *graph_.state_;
    if (buffer.graph != state.serial || buffer.index >= state.resources.Size() ||
        state.resources[buffer.index].kind != GraphResourceKind::Buffer) {
        return {};
    }

    const PassRecord& pass = state.passes[pass_];
    for (uint32 use = pass.firstUse; use < pass.firstUse + pass.useCount; ++use) {
        if (state.uses[use].resource == buffer.index) {
            return graphics::BufferHandle{state.resources[buffer.index].device};
        }
    }

    return {};
}

// --- RenderGraph ---

RenderGraphDesc MakeRenderGraphDesc(RenderFrameContext& frame, GraphResourcePool& pool,
                                    GpuTrace* trace, GraphMode mode) noexcept {
    RenderGraphDesc desc;
    desc.device = &frame.Device();
    desc.handles = &frame.Handles();
    desc.pool = &pool;
    desc.arena = &frame.Arena();
    desc.trace = trace;
    desc.frame = frame.FrameNumber();
    desc.mode = mode;
    return desc;
}

RenderGraph::RenderGraph(const RenderGraphDesc& desc) noexcept : arena_(RequireArena(desc)) {
    if (desc.device == nullptr || desc.handles == nullptr || desc.pool == nullptr) {
        return;
    }

    auto state = arena_.TryNew<GraphState>();
    if (!state) {
        return;
    }

    state_ = state.Value();
    state_->device = desc.device;
    state_->handles = desc.handles;
    state_->pool = desc.pool;
    state_->trace = desc.trace;
    state_->frame = desc.frame;
    state_->mode = desc.mode;
    state_->serial = NextGraphSerial();
}

RenderGraph::~RenderGraph() noexcept {
    if (state_ == nullptr) {
        return;
    }

    // 실행하지 않은 그래프도 열린 기록·풀 자원·lease·export handle을 정리한다.
    if (!state_->executed) {
        state_->executed = true;
        auto finished = FinishGraph(arena_, *state_, false);
        (void)finished;
    }

    // pass 실행기(람다 캡처)는 렌더 lane에서 부순다(arena Reset은 게임 lane에서 일어난다).
    for (PassRecord& pass : state_->passes) {
        if (pass.executor != nullptr) {
            pass.executor->Destroy();
            pass.executor = nullptr;
        }
    }
}

Result<GraphTexture> RenderGraph::CreateTexture(const graphics::TextureDesc& desc,
                                                const char* name) noexcept {
    if (state_ == nullptr) {
        return Result<GraphTexture>::Failure(ErrorCode::OutOfMemory);
    }

    GraphState& state = *state_;
    if (state.executed) {
        return Result<GraphTexture>::Failure(ErrorCode::Busy);
    }

    // 렌더 타깃 그래프 텍스처는 GPU가 채우므로 번호표 안의 비압축 형식이어야 한다(블록 압축 형식은
    // 렌더 타깃이 될 수 없다, PixelFormatInfo). Sampled 그래프 텍스처는 내용을 업로드로만 받으므로
    // (UploadTexture) 압축 형식도 된다. 장치가 그 형식을 지원하는지는 풀 할당 때 장치 생성이
    // 판단한다(Unsupported).
    const graphics::PixelFormatInfo& format = graphics::GetPixelFormatInfo(desc.format);
    const bool renderTarget = desc.usage == graphics::TextureUsage::RenderTarget;
    const bool sampled = desc.usage == graphics::TextureUsage::Sampled;
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
        desc.mipLevels > MaxGraphTextureMips || (!renderTarget && !sampled) ||
        !graphics::IsValidPixelFormat(desc.format) ||
        (renderTarget && format.compression != graphics::PixelCompression::None) ||
        (sampled && desc.mipLevels > graphics::FullMipCount(desc.width, desc.height))) {
        return Result<GraphTexture>::Failure(RecordGraphError(
            state, ErrorCode::InvalidArgument, true,
            "texture '{}' has an invalid description (graph textures are uncompressed render "
            "targets or uploaded sampled textures, format {})",
            name != nullptr ? name : "", graphics::PixelFormatName(desc.format)));
    }

    ResourceRecord record;
    record.kind = GraphResourceKind::Texture;
    record.name = name != nullptr ? name : "";
    record.textureDesc = desc;
    record.subresources = desc.mipLevels;
    record.firstTrack = static_cast<uint32>(state.tracks.Size());
    for (uint32 index = 0; index < record.subresources; ++index) {
        auto pushed = state.tracks.TryPushBack(arena_, SubresourceTrack{});
        if (!pushed) {
            return Result<GraphTexture>::Failure(pushed.Error());
        }
    }

    auto pushed = state.resources.TryPushBack(arena_, record);
    if (!pushed) {
        return Result<GraphTexture>::Failure(pushed.Error());
    }

    return Result<GraphTexture>::Success(
        GraphTexture{static_cast<uint32>(state.resources.Size() - 1), state.serial});
}

Result<GraphBuffer> RenderGraph::CreateBuffer(const graphics::BufferDesc& desc,
                                              const char* name) noexcept {
    if (state_ == nullptr) {
        return Result<GraphBuffer>::Failure(ErrorCode::OutOfMemory);
    }

    GraphState& state = *state_;
    if (state.executed) {
        return Result<GraphBuffer>::Failure(ErrorCode::Busy);
    }

    if (desc.size == 0) {
        return Result<GraphBuffer>::Failure(RecordGraphError(state, ErrorCode::InvalidArgument,
                                                             true, "buffer '{}' has size 0",
                                                             name != nullptr ? name : ""));
    }

    ResourceRecord record;
    record.kind = GraphResourceKind::Buffer;
    record.name = name != nullptr ? name : "";
    record.bufferDesc = desc;
    record.subresources = 1;
    record.firstTrack = static_cast<uint32>(state.tracks.Size());
    auto track = state.tracks.TryPushBack(arena_, SubresourceTrack{});
    if (!track) {
        return Result<GraphBuffer>::Failure(track.Error());
    }

    auto pushed = state.resources.TryPushBack(arena_, record);
    if (!pushed) {
        return Result<GraphBuffer>::Failure(pushed.Error());
    }

    return Result<GraphBuffer>::Success(
        GraphBuffer{static_cast<uint32>(state.resources.Size() - 1), state.serial});
}

namespace {

// import 공용 경로다. 성공하면 자원 번호다.
Result<uint32> ImportResource(FrameArena& arena, GraphState& state, GraphResourceKind kind,
                              RenderHandle handle, const char* name,
                              const GraphImportOptions& options) noexcept {
    const char* label = name != nullptr ? name : "";
    if (state.executed) {
        return Result<uint32>::Failure(ErrorCode::Busy);
    }

    const bool texture = kind == GraphResourceKind::Texture;
    if (texture && (options.mipLevels == 0 || options.mipLevels > MaxGraphTextureMips)) {
        return Result<uint32>::Failure(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                                        "texture '{}' import has {} mip levels",
                                                        label, options.mipLevels));
    }

    if ((options.initialAccess != ResourceAccess::Unknown &&
         !graphics::CanCombineAccess(options.initialAccess)) ||
        (options.finalAccess != ResourceAccess::Unknown &&
         !graphics::CanCombineAccess(options.finalAccess))) {
        return Result<uint32>::Failure(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                                        "'{}' import has an invalid access state",
                                                        label));
    }

    auto lease = state.handles->AcquireLease(handle, texture ? RenderResourceKind::Texture
                                                             : RenderResourceKind::Buffer);
    if (!lease) {
        const char* reason = lease.Error() == ErrorCode::StaleHandle
                                 ? "the render handle is retired or unknown"
                             : lease.Error() == ErrorCode::NotFound
                                 ? "the render handle has no device resource yet"
                                 : "the render handle is of another resource kind";
        return Result<uint32>::Failure(RecordGraphError(
            state, lease.Error(), true, "'{}' cannot be imported: {}", label, reason));
    }

    ResourceRecord record;
    record.kind = kind;
    record.name = label;
    record.imported = true;
    record.produced = true;
    record.allocated = true;
    record.handle = handle;
    record.lease = lease.Value();
    record.device = lease.Value().device;
    record.initialAccess = options.initialAccess;
    record.finalAccess = options.finalAccess;
    record.subresources = texture ? options.mipLevels : 1u;
    record.firstTrack = static_cast<uint32>(state.tracks.Size());
    SubresourceTrack initial;
    initial.access = options.initialAccess;
    for (uint32 index = 0; index < record.subresources; ++index) {
        auto pushed = state.tracks.TryPushBack(arena, initial);
        if (!pushed) {
            state.handles->ReleaseLease(lease.Value());
            return Result<uint32>::Failure(pushed.Error());
        }
    }

    auto pushed = state.resources.TryPushBack(arena, record);
    if (!pushed) {
        state.handles->ReleaseLease(lease.Value());
        return Result<uint32>::Failure(pushed.Error());
    }

    return Result<uint32>::Success(static_cast<uint32>(state.resources.Size() - 1));
}

// export 공용 경로다.
Result<RenderHandle> ExportResource(GraphState& state, GraphResourceKind kind, uint32 index,
                                    uint32 graph, ResourceAccess finalAccess) noexcept {
    if (state.executed) {
        return Result<RenderHandle>::Failure(ErrorCode::Busy);
    }

    if (graph != state.serial || index >= state.resources.Size() ||
        state.resources[index].kind != kind) {
        return Result<RenderHandle>::Failure(RecordGraphError(
            state, ErrorCode::InvalidArgument, true, "export of a resource from another graph"));
    }

    ResourceRecord& record = state.resources[index];
    if (record.exported) {
        return Result<RenderHandle>::Failure(RecordGraphError(
            state, ErrorCode::InvalidArgument, true, "'{}' is exported twice", record.name));
    }

    if (finalAccess == ResourceAccess::Unknown || !graphics::CanCombineAccess(finalAccess)) {
        return Result<RenderHandle>::Failure(
            RecordGraphError(state, ErrorCode::InvalidArgument, true,
                             "'{}' export has an invalid final access state", record.name));
    }

    if (!record.imported) {
        auto reserved =
            state.handles->Reserve(kind == GraphResourceKind::Texture ? RenderResourceKind::Texture
                                                                      : RenderResourceKind::Buffer);
        if (!reserved) {
            return Result<RenderHandle>::Failure(reserved.Error());
        }

        record.handle = reserved.Value();
    }

    record.exported = true;
    record.released = false;
    record.finalAccess = finalAccess;
    return Result<RenderHandle>::Success(record.handle);
}

} // namespace

Result<GraphTexture> RenderGraph::ImportTexture(RenderHandle handle, const char* name,
                                                const GraphImportOptions& options) noexcept {
    if (state_ == nullptr) {
        return Result<GraphTexture>::Failure(ErrorCode::OutOfMemory);
    }

    auto imported =
        ImportResource(arena_, *state_, GraphResourceKind::Texture, handle, name, options);
    if (!imported) {
        return Result<GraphTexture>::Failure(imported.Error());
    }

    return Result<GraphTexture>::Success(GraphTexture{imported.Value(), state_->serial});
}

Result<GraphBuffer> RenderGraph::ImportBuffer(RenderHandle handle, const char* name,
                                              const GraphImportOptions& options) noexcept {
    if (state_ == nullptr) {
        return Result<GraphBuffer>::Failure(ErrorCode::OutOfMemory);
    }

    auto imported =
        ImportResource(arena_, *state_, GraphResourceKind::Buffer, handle, name, options);
    if (!imported) {
        return Result<GraphBuffer>::Failure(imported.Error());
    }

    return Result<GraphBuffer>::Success(GraphBuffer{imported.Value(), state_->serial});
}

Result<RenderHandle> RenderGraph::ExportTexture(GraphTexture texture,
                                                ResourceAccess finalAccess) noexcept {
    if (state_ == nullptr) {
        return Result<RenderHandle>::Failure(ErrorCode::OutOfMemory);
    }

    return ExportResource(*state_, GraphResourceKind::Texture, texture.index, texture.graph,
                          finalAccess);
}

Result<RenderHandle> RenderGraph::ExportBuffer(GraphBuffer buffer,
                                               ResourceAccess finalAccess) noexcept {
    if (state_ == nullptr) {
        return Result<RenderHandle>::Failure(ErrorCode::OutOfMemory);
    }

    return ExportResource(*state_, GraphResourceKind::Buffer, buffer.index, buffer.graph,
                          finalAccess);
}

namespace {

// 한 pass의 subresource 접근 하나를 더한다(같은 subresource의 앞 선언과 합치고 상충을 검사한다).
Result<void> AddUse(FrameArena& arena, GraphState& state, const char* passName, usize firstUse,
                    uint32 resource, uint32 subresource, ResourceAccess access,
                    GraphAccessFlags flags) noexcept {
    for (usize index = firstUse; index < state.uses.Size(); ++index) {
        SubresourceUse& use = state.uses[index];
        if (use.resource != resource || use.subresource != subresource) {
            continue;
        }

        if (!CanMergeWithinPass(use.access, access)) {
            char first[AccessNameBytes];
            char second[AccessNameBytes];
            FormatAccess(use.access, first, sizeof(first));
            FormatAccess(access, second, sizeof(second));
            return Result<void>::Failure(
                RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                 "pass '{}' has conflicting accesses {} and {} to '{}' mip {}",
                                 passName, StringView(first), StringView(second),
                                 state.resources[resource].name, subresource));
        }

        use.access |= access;
        use.flags = CombineFlags(use.flags, flags);
        return Result<void>::Success();
    }

    SubresourceUse use;
    use.resource = resource;
    use.subresource = subresource;
    use.access = access;
    use.flags = flags;
    return state.uses.TryPushBack(arena, use);
}

// 생산자 간선을 더한다(같은 생산자는 한 번).
Result<void> AddProducer(FrameArena& arena, GraphState& state, PassRecord& pass,
                         uint32 producer) noexcept {
    for (uint32 link = pass.firstProducer; link != GraphIndexNone;
         link = state.producers[link].next) {
        if (state.producers[link].pass == producer) {
            return Result<void>::Success();
        }
    }

    ProducerLink link;
    link.pass = producer;
    link.next = pass.firstProducer;
    auto pushed = state.producers.TryPushBack(arena, link);
    if (!pushed) {
        return pushed;
    }

    pass.firstProducer = static_cast<uint32>(state.producers.Size() - 1);
    return Result<void>::Success();
}

} // namespace

Result<GraphPass> RenderGraph::AddPassWithAccesses(TraceScopeName& name, GraphPassKind kind,
                                                   GraphPassFlags flags,
                                                   Span<const GraphResourceAccess> accesses,
                                                   const GraphRenderTarget& target,
                                                   GraphPassExecutor* executor) noexcept {
    // 실행기는 성공·실패와 무관하게 그래프가 소유한다. 실패하면 바로 부순다.
    auto reject = [executor](ErrorCode code) noexcept {
        if (executor != nullptr) {
            executor->Destroy();
        }

        return Result<GraphPass>::Failure(code);
    };

    if (state_ == nullptr) {
        return reject(ErrorCode::OutOfMemory);
    }

    GraphState& state = *state_;
    if (state.executed) {
        return reject(ErrorCode::Busy);
    }

    if (executor == nullptr) {
        return reject(ErrorCode::InvalidArgument);
    }

    const char* passName = name.Name() != nullptr ? name.Name() : "";
    if (kind == GraphPassKind::AsyncCompute) {
        return reject(RecordGraphError(state, ErrorCode::Unsupported, false,
                                       "pass '{}' uses async compute, which arrives with RG4",
                                       passName));
    }

    // 렌더 타깃 검사다.
    const bool raster = kind == GraphPassKind::Raster;
    if (raster && target.kind == GraphTargetKind::None) {
        return reject(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                       "raster pass '{}' has no render target", passName));
    }

    if (!raster && target.kind != GraphTargetKind::None) {
        return reject(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                       "{} pass '{}' cannot bind a render target",
                                       PassKindName(kind), passName));
    }

    if (target.kind == GraphTargetKind::Texture) {
        if (target.texture.graph != state.serial ||
            target.texture.index >= state.resources.Size() ||
            state.resources[target.texture.index].kind != GraphResourceKind::Texture) {
            return reject(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                           "pass '{}' renders to a texture from another graph",
                                           passName));
        }

        if (target.load == GraphLoadAction::Load) {
            return reject(RecordGraphError(
                state, ErrorCode::Unsupported, false,
                "pass '{}' loads its render target; attachment load arrives with GX4", passName));
        }
    }

    if (target.kind == GraphTargetKind::SwapChain && target.swapChain.IsNull()) {
        return reject(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                       "pass '{}' renders to a null swap chain", passName));
    }

    const uint32 passIndex = static_cast<uint32>(state.passes.Size());
    const usize firstUse = state.uses.Size();
    auto rollback = [&state, firstUse, &reject](ErrorCode code) noexcept {
        while (state.uses.Size() > firstUse) {
            state.uses.PopBack();
        }

        return reject(code);
    };

    // 선언한 접근을 subresource 단위로 펼친다.
    const ResourceAccess forbidden = ForbiddenAccess(kind);
    for (const GraphResourceAccess& access : accesses) {
        if (access.graph != state.serial || access.resource >= state.resources.Size() ||
            state.resources[access.resource].kind != access.kind) {
            return rollback(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                             "pass '{}' declares a resource from another graph",
                                             passName));
        }

        const ResourceRecord& resource = state.resources[access.resource];
        char accessName[AccessNameBytes];
        FormatAccess(access.access, accessName, sizeof(accessName));
        if (access.access == ResourceAccess::Unknown || access.access == ResourceAccess::Discard ||
            !graphics::CanCombineAccess(access.access)) {
            return rollback(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                             "pass '{}' declares invalid access {} to '{}'",
                                             passName, StringView(accessName), resource.name));
        }

        // Sampled 그래프 텍스처의 내용은 업로드뿐이다(장치 용도에 쓰기가 없다).
        if (resource.kind == GraphResourceKind::Texture && !resource.imported &&
            resource.textureDesc.usage == graphics::TextureUsage::Sampled &&
            graphics::IsWritableAccess(access.access)) {
            return rollback(RecordGraphError(
                state, ErrorCode::InvalidArgument, true,
                "pass '{}' writes sampled graph texture '{}'; its content comes from uploads only",
                passName, resource.name));
        }

        if (graphics::HasAnyAccess(access.access, forbidden)) {
            if (raster && graphics::HasAnyAccess(access.access, graphics::UavAccessMask)) {
                return rollback(RecordGraphError(
                    state, ErrorCode::InvalidArgument, true,
                    "raster pass '{}' declares UAV access {} to '{}'; UAVs are compute only",
                    passName, StringView(accessName), resource.name));
            }

            return rollback(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                             "{} pass '{}' cannot declare access {} to '{}'",
                                             PassKindName(kind), passName, StringView(accessName),
                                             resource.name));
        }

        uint32 first = 0;
        uint32 count = 1;
        if (access.kind == GraphResourceKind::Texture) {
            first = access.range.firstMip;
            if (first >= resource.subresources || access.range.firstSlice != 0 ||
                access.range.sliceCount == 0 || access.range.mipCount == 0) {
                return rollback(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                                 "pass '{}' declares an invalid range of '{}'",
                                                 passName, resource.name));
            }

            const uint32 available = resource.subresources - first;
            count = access.range.mipCount < available ? access.range.mipCount : available;
        }

        for (uint32 subresource = first; subresource < first + count; ++subresource) {
            auto added = AddUse(arena_, state, passName, firstUse, access.resource, subresource,
                                access.access, access.flags);
            if (!added) {
                return rollback(added.Error());
            }
        }
    }

    // 렌더 타깃 접근이다(attachment).
    if (target.kind == GraphTargetKind::Texture) {
        const ResourceRecord& resource = state.resources[target.texture.index];
        if (!resource.imported &&
            resource.textureDesc.usage != graphics::TextureUsage::RenderTarget) {
            return rollback(RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                             "pass '{}' renders to '{}', which is not a render "
                                             "target graph texture",
                                             passName, resource.name));
        }

        auto added = AddUse(arena_, state, passName, firstUse, target.texture.index, 0,
                            ResourceAccess::RenderTarget, GraphAccessFlags::None);
        if (!added) {
            return rollback(added.Error());
        }
    }

    // 생산 전 읽기 검사다(만든 자원만, import는 처음부터 내용이 있다).
    for (usize index = firstUse; index < state.uses.Size(); ++index) {
        const SubresourceUse& use = state.uses[index];
        const ResourceRecord& resource = state.resources[use.resource];
        const SubresourceTrack& track = state.tracks[resource.firstTrack + use.subresource];
        if (!resource.imported && track.lastProducer == GraphIndexNone && !track.uploaded &&
            graphics::HasAnyAccess(use.access, graphics::ReadOnlyAccessMask)) {
            return rollback(
                RecordGraphError(state, ErrorCode::InvalidArgument, true,
                                 "pass '{}' reads '{}' mip {} before any pass produced it",
                                 passName, resource.name, use.subresource));
        }
    }

    // 확정: 생산자 간선·마지막 생산자·외부 출력을 기록한다.
    PassRecord pass;
    pass.name = &name;
    pass.kind = kind;
    pass.flags = flags;
    pass.target = target;
    pass.executor = executor;
    pass.firstUse = static_cast<uint32>(firstUse);
    pass.useCount = static_cast<uint32>(state.uses.Size() - firstUse);
    pass.root = HasGraphPassFlag(flags, GraphPassFlags::HasSideEffects) ||
                target.kind == GraphTargetKind::SwapChain;
    pass.culled = state.mode == GraphMode::Deferred;
    // 생산자 간선을 먼저 모은다(상태를 바꾸지 않는다). 간선은 pass를 넣어야 닿으므로 실패해도 남지
    // 않는다.
    for (usize index = firstUse; index < state.uses.Size(); ++index) {
        const SubresourceUse use = state.uses[index];
        const ResourceRecord& resource = state.resources[use.resource];
        const SubresourceTrack& track = state.tracks[resource.firstTrack + use.subresource];
        if (track.lastProducer != GraphIndexNone) {
            auto linked = AddProducer(arena_, state, pass, track.lastProducer);
            if (!linked) {
                return rollback(RecordGraphError(state, linked.Error(), false,
                                                 "pass '{}' could not be recorded ({})", passName,
                                                 ErrorCodeName(linked.Error())));
            }
        }

        // 그래프 밖 자원에 쓰는 pass는 외부 출력이다.
        pass.root = pass.root || (resource.imported && graphics::IsWritableAccess(use.access));
    }

    auto pushed = state.passes.TryPushBack(arena_, pass);
    if (!pushed) {
        return rollback(RecordGraphError(state, pushed.Error(), false,
                                         "pass '{}' could not be recorded ({})", passName,
                                         ErrorCodeName(pushed.Error())));
    }

    // pass를 넣은 뒤에만 마지막 생산자를 바꾼다(실패한 pass가 생산자로 남지 않는다). 접근을 선언한
    // 자원에는 그 뒤 업로드를 받지 않는다(업로드는 첫 pass 앞에서 실행된다).
    for (usize index = firstUse; index < state.uses.Size(); ++index) {
        const SubresourceUse use = state.uses[index];
        ResourceRecord& resource = state.resources[use.resource];
        resource.accessed = true;
        if (graphics::IsWritableAccess(use.access)) {
            state.tracks[resource.firstTrack + use.subresource].lastProducer = passIndex;
            resource.produced = true;
        }
    }

    ++state.stats.passes;
    if (state.mode == GraphMode::Deferred) {
        if (pass.root) {
            auto uncull = UncullPass(arena_, state, passIndex);
            if (!uncull) {
                return Result<GraphPass>::Failure(uncull.Error());
            }
        }

        return Result<GraphPass>::Success(GraphPass{passIndex});
    }

    // 즉시 실행 모드다. 앞선 오류가 있으면 실행하지 않는다.
    if (state.failure != ErrorCode::None) {
        return Result<GraphPass>::Failure(state.failure);
    }

    auto executed = ExecutePass(*this, arena_, state, passIndex);
    if (!executed) {
        return Result<GraphPass>::Failure(executed.Error());
    }

    return Result<GraphPass>::Success(GraphPass{passIndex});
}

Result<void> RenderGraph::Execute() noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    GraphState& state = *state_;
    if (state.executed) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    state.executed = true;
    Result<void> outcome = Result<void>::Success();
    if (state.failure != ErrorCode::None) {
        outcome = Result<void>::Failure(state.failure);
    } else if (state.mode == GraphMode::Deferred) {
        outcome = CompileGraph(arena_, state);
        // 업로드는 첫 pass 앞에서 한다(UE SubmitBufferUploads와 같은 자리, 기록이 열리기 전).
        if (outcome) {
            outcome = ExecuteUploads(arena_, state);
        }

        if (outcome) {
            for (uint32 pass = 0; pass < state.passes.Size() && outcome; ++pass) {
                if (!state.passes[pass].culled) {
                    outcome = ExecutePass(*this, arena_, state, pass);
                }
            }
        }
    }

    auto finished = FinishGraph(arena_, state, outcome.IsOk());
    if (outcome && !finished) {
        return finished;
    }

    return outcome;
}

bool RenderGraph::IsCulled(GraphPass pass) const noexcept {
    if (state_ == nullptr || pass.index >= state_->passes.Size()) {
        return false;
    }

    return state_->passes[pass.index].culled;
}

namespace {

GraphLifetime LifetimeOf(const GraphState* state, GraphResourceKind kind, uint32 index,
                         uint32 graph) noexcept {
    GraphLifetime lifetime;
    if (state == nullptr || graph != state->serial || index >= state->resources.Size() ||
        state->resources[index].kind != kind) {
        return lifetime;
    }

    const ResourceRecord& record = state->resources[index];
    lifetime.firstPass = record.firstPass;
    lifetime.lastPass = record.lastPass;
    lifetime.references = record.references;
    lifetime.released = record.released;
    lifetime.reusedWithinGraph = record.reusedWithinGraph;
    return lifetime;
}

} // namespace

GraphLifetime RenderGraph::Lifetime(GraphTexture texture) const noexcept {
    return LifetimeOf(state_, GraphResourceKind::Texture, texture.index, texture.graph);
}

GraphLifetime RenderGraph::Lifetime(GraphBuffer buffer) const noexcept {
    return LifetimeOf(state_, GraphResourceKind::Buffer, buffer.index, buffer.graph);
}

Span<const GraphTransitionRecord> RenderGraph::Transitions() const noexcept {
    if (state_ == nullptr) {
        return {};
    }

    return state_->transitions.View();
}

GraphStats RenderGraph::Stats() const noexcept {
    return state_ == nullptr ? GraphStats{} : state_->stats;
}

StringView RenderGraph::LastError() const noexcept {
    if (state_ == nullptr) {
        return StringView("render graph state allocation failed");
    }

    return StringView(state_->message, state_->messageLength);
}

} // namespace rocks::render
