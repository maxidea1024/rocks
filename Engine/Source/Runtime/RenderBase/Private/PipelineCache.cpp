#include <RenderBase/PipelineCache.h>

#include <Core/Hash.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <string.h>

// 파이프라인 캐시(Docs/Design/RenderBase.ko.md 9.7절)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::render {

namespace pipeline_cache_detail {

// 키 byte를 쌓는다(구조체 padding을 담지 않도록 칸마다 쓴다).
class KeyWriter {
public:
    explicit KeyWriter(Array<uint8>& out) noexcept : out_(out) {
    }

    template <class T>
    void Put(const T& value) noexcept {
        Bytes(&value, sizeof(T));
    }

    void Text(StringView text) noexcept {
        Put(static_cast<uint32>(text.ByteCount()));
        Bytes(text.Data(), text.ByteCount());
    }

    void Bytes(const void* data, usize bytes) noexcept {
        if (bytes == 0) {
            return;
        }

        auto appended = out_.TryAppendUninitialized(bytes);
        if (!appended) {
            failed_ = true;
            return;
        }

        memcpy(appended.Value().Data(), data, bytes);
    }

    bool Failed() const noexcept {
        return failed_;
    }

private:
    Array<uint8>& out_;
    bool failed_ = false;
};

void PutStencil(KeyWriter& key, const graphics::StencilFace& face) noexcept {
    key.Put(face.compare);
    key.Put(face.fail);
    key.Put(face.depthFail);
    key.Put(face.pass);
}

void PutNames(KeyWriter& key, const graphics::PipelineBindingNames& names) noexcept {
    key.Text(names.texture);
    for (uint32 slot = 0; slot < graphics::MaxPipelineNamedSlots; ++slot) {
        key.Text(names.bufferViews[slot]);
        key.Text(names.writableBuffers[slot]);
        key.Text(names.writableTextures[slot]);
    }
}

bool BuildKey(const GraphicsPipelineRequest& request, Array<uint8>& out) noexcept {
    out.Clear();
    KeyWriter key(out);
    const graphics::PipelineDesc& state = request.state;
    key.Put(request.set->Epoch());
    key.Put(request.vertexEntry);
    key.Put(request.pixelEntry);
    key.Put(static_cast<uint32>(state.layout.attributes.Size()));
    for (const graphics::VertexAttribute& attribute : state.layout.attributes) {
        key.Put(attribute.location);
        key.Put(attribute.format);
        key.Put(attribute.stream);
        key.Put(attribute.offset);
    }

    key.Put(static_cast<uint32>(state.layout.streams.Size()));
    for (const graphics::VertexStream& stream : state.layout.streams) {
        key.Put(stream.stride);
        key.Put(stream.stepRate);
    }

    key.Put(state.topology);
    key.Put(state.colorTargetCount);
    for (uint32 index = 0; index < state.colorTargetCount && index < graphics::MaxColorTargets;
         ++index) {
        const graphics::ColorTargetBlend& blend = state.blend[index];
        key.Put(state.colorFormats[index]);
        key.Put(blend.enable);
        key.Put(blend.sourceColor);
        key.Put(blend.destinationColor);
        key.Put(blend.colorOperation);
        key.Put(blend.sourceAlpha);
        key.Put(blend.destinationAlpha);
        key.Put(blend.alphaOperation);
        key.Put(blend.writeMask);
    }

    key.Put(state.alphaToCoverage);
    key.Put(state.depthFormat);
    const graphics::DepthStencilState& depth = state.depthStencil;
    key.Put(depth.depthTest);
    key.Put(depth.depthWrite);
    key.Put(depth.depthCompare);
    key.Put(depth.stencilTest);
    key.Put(depth.stencilReadMask);
    key.Put(depth.stencilWriteMask);
    PutStencil(key, depth.front);
    PutStencil(key, depth.back);
    key.Put(state.raster.cullMode);
    key.Put(state.raster.depthBias);
    key.Put(state.raster.depthBiasSlope);
    key.Put(state.raster.depthBiasClamp);
    key.Put(state.raster.depthClamp);
    key.Put(state.drawConstantBytes);
    key.Put(state.sampleCount);
    PutNames(key, state.bindings);
    return !key.Failed();
}

uint64 HashKey(Span<const uint8> key) noexcept {
    uint64 hash = MixHash(key.Size());
    usize index = 0;
    for (; index + sizeof(uint64) <= key.Size(); index += sizeof(uint64)) {
        uint64 word = 0;
        memcpy(&word, key.Data() + index, sizeof(word));
        hash = HashCombine(hash, word);
    }

    uint64 tail = 0;
    if (index < key.Size()) {
        memcpy(&tail, key.Data() + index, key.Size() - index);
        hash = HashCombine(hash, tail);
    }

    return hash;
}

bool SameKey(Span<const uint8> a, Span<const uint8> b) noexcept {
    return a.Size() == b.Size() && (a.Size() == 0 || memcmp(a.Data(), b.Data(), a.Size()) == 0);
}

} // namespace pipeline_cache_detail

using namespace pipeline_cache_detail;

PipelineCache::PipelineCache(Allocator& allocator, graphics::Device& device) noexcept
    : allocator_(allocator), device_(device), entries_(allocator) {
}

PipelineCache::~PipelineCache() noexcept {
    Clear();
}

Result<graphics::PipelineHandle>
PipelineCache::FindOrCreate(const GraphicsPipelineRequest& request) noexcept {
    using Outcome = Result<graphics::PipelineHandle>;
    if (request.set == nullptr) {
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    ShaderSet& set = *request.set;
    Entry candidate(allocator_);
    if (!BuildKey(request, candidate.key)) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    candidate.hash = HashKey(candidate.key.View());
    platform::LockGuard lock(gate_);
    for (const Entry& entry : entries_) {
        if (entry.hash == candidate.hash && SameKey(entry.key.View(), candidate.key.View())) {
            ++hits_;
            return Outcome::Success(entry.pipeline);
        }
    }

    ++misses_;
    if (set.State() == ShaderSetState::Retired) {
        return Outcome::Failure(ErrorCode::Destroyed);
    }

    const shader::ShaderLibraryEntry* vertexEntry = set.Entry(request.vertexEntry);
    const shader::ShaderLibraryEntry* pixelEntry = set.Entry(request.pixelEntry);
    if (set.State() != ShaderSetState::Ready) {
        return Outcome::Failure(ErrorCode::NotReady);
    }

    if (vertexEntry == nullptr || pixelEntry == nullptr ||
        vertexEntry->stage != shader::ShaderStageVertex ||
        pixelEntry->stage != shader::ShaderStagePixel) {
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    const char* reason = "";
    if (!shader::CheckShaderStageLink(*set.BindingLayout(request.vertexEntry),
                                      *set.BindingLayout(request.pixelEntry), &reason)) {
        linkFailure_ = reason;
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    auto vertex = set.DeviceShader(request.vertexEntry);
    if (!vertex) {
        return Outcome::Failure(vertex.Error());
    }

    auto pixel = set.DeviceShader(request.pixelEntry);
    if (!pixel) {
        return Outcome::Failure(pixel.Error());
    }

    if (!entries_.TryReserve(entries_.Size() + 1)) {
        return Outcome::Failure(ErrorCode::OutOfMemory);
    }

    if (!set.TryAcquireLease()) {
        return Outcome::Failure(ErrorCode::Destroyed);
    }

    graphics::PipelineDesc desc = request.state;
    desc.vertexShader = vertex.Value();
    desc.pixelShader = pixel.Value();
    auto pipeline = device_.CreatePipeline(desc);
    if (!pipeline) {
        set.ReleaseLease();
        return pipeline;
    }

    candidate.epoch = set.Epoch();
    candidate.set = &set;
    candidate.pipeline = pipeline.Value();
    (void)entries_.TryPushBack(Move(candidate));
    return pipeline;
}

void PipelineCache::RetireSet(const ShaderSet& set) noexcept {
    platform::LockGuard lock(gate_);
    for (usize index = entries_.Size(); index > 0; --index) {
        Entry& entry = entries_[index - 1];
        if (entry.epoch != set.Epoch()) {
            continue;
        }

        device_.Retire(entry.pipeline);
        entry.set->ReleaseLease();
        (void)entries_.TryRemoveAtSwap(index - 1);
    }
}

void PipelineCache::Clear() noexcept {
    platform::LockGuard lock(gate_);
    for (Entry& entry : entries_) {
        device_.Retire(entry.pipeline);
        entry.set->ReleaseLease();
    }

    entries_.Clear();
}

uint64 PipelineCache::Hits() const noexcept {
    platform::LockGuard lock(gate_);
    return hits_;
}

uint64 PipelineCache::Misses() const noexcept {
    platform::LockGuard lock(gate_);
    return misses_;
}

usize PipelineCache::Size() const noexcept {
    platform::LockGuard lock(gate_);
    return entries_.Size();
}

const char* PipelineCache::LastLinkFailure() const noexcept {
    platform::LockGuard lock(gate_);
    return linkFailure_;
}

} // namespace rocks::render
