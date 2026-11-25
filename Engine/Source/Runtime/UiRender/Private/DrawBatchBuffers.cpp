#include <UiRender/DrawBatchBuffers.h>
#include <UiRender/DrawBatchRenderer.h>

namespace rocks::ui {

namespace {

// 정점 레이아웃에는 float가 들어가므로 GPU에는 원래 비트 표현을 그대로 전달한다.
Span<const uint8> VertexBytes(Span<const DrawVertex> vertices) noexcept {
    static_assert(IsTriviallyCopyableV<DrawVertex>);
    return {reinterpret_cast<const uint8*>(vertices.Data()), vertices.ByteCount()};
}

} // namespace

DrawBatchBuffers::DrawBatchBuffers(graphics::Device& device, render::ShaderSet& shaders,
                                   const DrawBatch& batch,
                                   graphics::PixelFormat targetFormat) noexcept
    : targetFormat_(targetFormat), device_(device), shaders_(shaders), batch_(batch) {
}

DrawBatchBuffers::~DrawBatchBuffers() noexcept {
    Release();
    if (!backdrop_.IsNull()) {
        device_.Retire(backdrop_);
    }

    if (!blurPipeline_.IsNull()) {
        device_.Retire(blurPipeline_);
    }

    if (!roundedPipeline_.IsNull()) {
        device_.Retire(roundedPipeline_);
    }
}

Result<void> DrawBatchBuffers::PrepareRoundedBox() const noexcept {
    if (!roundedPipeline_.IsNull()) {
        return Result<void>::Success();
    }

    auto pipeline = DrawBatchRenderer::CreateRoundedBoxPipeline(device_, shaders_, targetFormat_);
    if (!pipeline) {
        return Result<void>::Failure(pipeline.Error());
    }

    roundedPipeline_ = pipeline.Value();
    return Result<void>::Success();
}

Result<void> DrawBatchBuffers::PrepareBlur(uint32 width, uint32 height) const noexcept {
    if (blurPipeline_.IsNull()) {
        auto pipeline = DrawBatchRenderer::CreateBlurPipeline(device_, shaders_, targetFormat_);
        if (!pipeline) {
            return Result<void>::Failure(pipeline.Error());
        }

        blurPipeline_ = pipeline.Value();
    }

    if (!backdrop_.IsNull() && backdropWidth_ == width && backdropHeight_ == height) {
        return Result<void>::Success();
    }

    graphics::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = targetFormat_;
    desc.usage = graphics::TextureUsage::RenderTarget;
    auto target = device_.CreateTexture(desc, {});
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    if (!backdrop_.IsNull()) {
        device_.Retire(backdrop_);
    }

    backdrop_ = target.Value();
    backdropWidth_ = width;
    backdropHeight_ = height;
    return Result<void>::Success();
}

void DrawBatchBuffers::Release() noexcept {
    if (!vertices_.IsNull()) {
        device_.Retire(vertices_);
        vertices_ = {};
    }

    if (!indices_.IsNull()) {
        device_.Retire(indices_);
        indices_ = {};
    }
}

Result<void> DrawBatchBuffers::Upload() noexcept {
    if (ready_ && revision_ == batch_.Revision()) {
        return Result<void>::Success();
    }

    if (batch_.Indices().IsEmpty()) {
        Release();
    } else if (vertices_.IsNull() || topology_ != batch_.TopologyRevision()) {
        graphics::BufferDesc desc;
        desc.usage = graphics::BufferUsage::Vertex;
        desc.size = batch_.Vertices().ByteCount();
        auto vertex = device_.CreateBuffer(desc, VertexBytes(batch_.Vertices()));
        if (!vertex) {
            return Result<void>::Failure(vertex.Error());
        }

        desc.usage = graphics::BufferUsage::Index;
        desc.size = batch_.Indices().ByteCount();
        auto index = device_.CreateBuffer(desc, AsBytes(batch_.Indices()));
        if (!index) {
            device_.Retire(vertex.Value());
            return Result<void>::Failure(index.Error());
        }

        Release();
        vertices_ = vertex.Value();
        indices_ = index.Value();
    } else if (ready_ && revision_ + 1 == batch_.Revision()) {
        for (const auto range : batch_.ChangedVertices()) {
            auto updated = device_.UpdateBuffer(
                vertices_, static_cast<uint64>(range.first) * sizeof(DrawVertex),
                VertexBytes(batch_.Vertices().Slice(range.first, range.count)));
            if (!updated) {
                ready_ = false;
                return updated;
            }
        }
    } else {
        auto updated = device_.UpdateBuffer(vertices_, 0, VertexBytes(batch_.Vertices()));
        if (!updated) {
            ready_ = false;
            return updated;
        }
    }

    revision_ = batch_.Revision();
    topology_ = batch_.TopologyRevision();
    ready_ = true;
    return Result<void>::Success();
}

} // namespace rocks::ui
