#include <UiRender/WidgetSurfaceHost.h>
#include <UiRender/VectorImageAtlas.h>
#include <UiRender/DrawBatchBuffers.h>

namespace rocks::ui {

struct WidgetSurfaceHost::Entry {
    Entry(Allocator& allocator, graphics::Device& owner, render::ShaderSet& shaders,
          SlotArray<BrushTexture>& registry, graphics::PixelFormat format,
          const SharedPtr<Element>& node) noexcept
        : device(owner), textures(registry), widget(node), list(allocator), batch(allocator),
          buffers(owner, shaders, batch, format), retained(owner, registry),
          postBuffers(owner, registry) {
    }

    ~Entry() noexcept {
        if (auto node = widget.Lock()) {
            auto* source = node->GetWidgetSurface();
            if (source) {
                for (uint32 i = 0; i < PostBufferChannelCount; ++i) {
                    if (!channels[i].IsNull() && source->GetPostBuffer(i) == channels[i]) {
                        source->SetPostBuffer(i, {});
                    }
                }
            }

            if (source && !published.IsNull() && source->ManagedSurface() == published) {
                source->SetManagedSurface({});
            }
        }

        if (!depth.IsNull()) {
            device.Retire(depth);
        }

        if (!target.IsNull()) {
            (void)textures.TryRemove(published);
            device.Retire(target);
        }
    }

    graphics::Device& device;
    SlotArray<BrushTexture>& textures;
    WeakPtr<Element> widget;
    WeakPtr<Element> enclosing;
    PaintList list;
    DrawBatch batch;
    DrawBatchBuffers buffers;
    RetainedSurface retained;
    PostBufferSet postBuffers;
    SlotHandle channels[PostBufferChannelCount]{};
    graphics::TextureHandle target{};
    graphics::TextureHandle depth{};
    uint32 depthWidth = 0, depthHeight = 0;
    SlotHandle published{};
    uint32 width = 0;
    uint32 height = 0;
    bool found = false;
};

WidgetSurfaceHost::WidgetSurfaceHost(Allocator& allocator, graphics::Device& device,
                                     render::ShaderSet& shaders, SlotArray<BrushTexture>& textures,
                                     graphics::PixelFormat format) noexcept
    : allocator_(allocator), device_(device), shaders_(shaders), textures_(textures),
      format_(format), postBuffers_(device, textures), entries_(allocator), order_(allocator) {
}

void WidgetSurfaceHost::SetVectorImages(VectorImageAtlas* atlas,
                                        graphics::PipelineHandle imagePipeline) noexcept {
    vectorImages_ = atlas;
    vectorPipeline_ = imagePipeline;
}

WidgetSurfaceHost::~WidgetSurfaceHost() noexcept {
    entries_.Clear();
    if (!composite_.IsNull()) {
        device_.Retire(composite_);
    }

    if (!image_.IsNull()) {
        device_.Retire(image_);
    }
}

SlotHandle WidgetSurfaceHost::GetPostBuffer(uint32 channel) const noexcept {
    return postBuffers_.Get(channel);
}

void WidgetSurfaceHost::SetScenePainter(ScenePainter painter) noexcept {
    scenePainter_ = Move(painter);
}

Result<void> WidgetSurfaceHost::Discover(const SharedPtr<Element>& node,
                                         const SharedPtr<Element>& enclosingRetainer) noexcept {
    auto visible = node->GetVisibility();
    if (visible == Visibility::Hidden || visible == Visibility::Collapsed) {
        return Result<void>::Success();
    }

    auto* source = node->GetWidgetSurface();
    auto enclosing = source && source->SurfaceKind() == WidgetSurfaceKind::RetainedContent
                         ? node
                         : enclosingRetainer;
    for (const auto& child : node->ActiveChildren()) {
        auto result = Discover(child, enclosing);
        if (!result) {
            return result;
        }
    }

    if (!source) {
        return Result<void>::Success();
    }

    Entry* current = nullptr;
    for (const auto& entry : entries_) {
        if (entry->widget.Lock().Get() == node.Get()) {
            current = entry.Get();
            break;
        }
    }

    if (!current) {
        auto created = UniquePtr<Entry>::TryMake(allocator_, allocator_, device_, shaders_,
                                                 textures_, format_, node);
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        current = created.Value().Get();
        auto added = entries_.TryPushBack(Move(created.Value()));
        if (!added) {
            return added;
        }
    }

    current->found = true;
    current->enclosing = enclosingRetainer;
    return order_.TryPushBack(current);
}

Result<bool> WidgetSurfaceHost::Update(Entry& entry, uint64 frame, uint32 width, uint32 height,
                                       graphics::PipelineHandle solid) noexcept {
    auto node = entry.widget.Lock();
    if (!node) {
        return Result<bool>::Success(false);
    }

    auto& source = *node->GetWidgetSurface();
    const auto kind = source.SurfaceKind();
    auto ownedScene = source.SceneSource();
    if ((!ownedScene || !source.WantsManagedSurface() || !ownedScene->UsesDepth()) &&
        !entry.depth.IsNull()) {
        device_.Retire(entry.depth);
        entry.depth = {};
    }

    const uint32 channelMask = source.RequestedPostBuffers();
    if ((!source.WantsManagedSurface() && channelMask == 0) ||
        (kind == WidgetSurfaceKind::Scene && !ownedScene && !scenePainter_.IsBound())) {
        for (uint32 i = 0; i < PostBufferChannelCount; ++i) {
            if (!entry.channels[i].IsNull() && source.GetPostBuffer(i) == entry.channels[i]) {
                source.SetPostBuffer(i, {});
            }

            entry.channels[i] = {};
        }

        const bool removed =
            !entry.published.IsNull() && source.ManagedSurface() == entry.published;
        if (removed) {
            source.SetManagedSurface({});
        }

        if (!entry.target.IsNull()) {
            (void)textures_.TryRemove(entry.published);
            device_.Retire(entry.target);
            entry.target = {};
            entry.published = {};
            entry.width = 0;
            entry.height = 0;
        }

        return Result<bool>::Success(removed);
    }

    // 후처리 버퍼는 자신이 기록되는 패스와 같은 크기여야 한다.
    auto renderArea = kind == WidgetSurfaceKind::PostBuffer ? entry.enclosing.Lock() : node;
    if (renderArea) {
        auto size = renderArea->GetGeometry().size;
        int32 x, y;
        if (!math::TryTruncToInt32(math::Ceil(size.x), x) ||
            !math::TryTruncToInt32(math::Ceil(size.y), y)) {
            return Result<bool>::Failure(ErrorCode::InvalidArgument);
        }

        if (x <= 0 || y <= 0) {
            return Result<bool>::Success(false);
        }

        width = static_cast<uint32>(x);
        height = static_cast<uint32>(y);
    }

    bool channelsChanged = false;
    if (kind == WidgetSurfaceKind::PostBuffer) {
        PostBufferSet* group = &postBuffers_;
        if (renderArea && channelMask) {
            for (const auto& candidate : entries_) {
                if (candidate->widget.Lock().Get() == renderArea.Get()) {
                    group = &candidate->postBuffers;
                    break;
                }
            }
        }

        for (uint32 i = 0; i < PostBufferChannelCount; ++i) {
            SlotHandle handle{};
            if ((channelMask & (1u << i)) != 0) {
                if (image_.IsNull()) {
                    auto made = DrawBatchRenderer::CreateImagePipeline(device_, shaders_, format_);
                    if (!made) {
                        return Result<bool>::Failure(made.Error());
                    }

                    image_ = made.Value();
                }

                auto prepared = group->Prepare(i, width, height, format_, image_);
                if (!prepared) {
                    return Result<bool>::Failure(prepared.Error());
                }

                handle = prepared.Value();
            }

            if (handle != source.GetPostBuffer(i)) {
                source.SetPostBuffer(i, handle);
                channelsChanged = true;
            }

            entry.channels[i] = handle;
        }

        if (!source.WantsManagedSurface()) {
            if (!entry.target.IsNull()) {
                (void)textures_.TryRemove(entry.published);
                device_.Retire(entry.target);
                entry.target = {};
                entry.published = {};
                source.SetManagedSurface({});
            }

            bool resized = width != entry.width || height != entry.height;
            entry.width = width;
            entry.height = height;
            return Result<bool>::Success(channelsChanged || resized);
        }
    }

    const bool resize = entry.width != width || entry.height != height;
    if (kind == WidgetSurfaceKind::RetainedContent) {
        if (!resize && source.ManagedSurface() == entry.published && !source.NeedsCapture(frame)) {
            return Result<bool>::Success(false);
        }

        if (composite_.IsNull()) {
            auto made = DrawBatchRenderer::CreateRetainedPipeline(device_, shaders_, format_);
            if (!made) {
                return Result<bool>::Failure(made.Error());
            }

            composite_ = made.Value();
        }

        entry.list.Clear();
        entry.list.SetVectorImageResolver(vectorImages_);
        auto result = source.RecordContent(entry.list);
        if (result) {
            result = entry.batch.Build(entry.list);
        }

        if (result) {
            result = entry.buffers.Upload();
        }

        // 기록이 받은 SVG 자리의 페이지를 렌더 전에 올린다(17.2절 프레임 순서).
        if (result && vectorImages_) {
            result = vectorImages_->Sync(vectorPipeline_);
        }

        if (result) {
            result =
                entry.retained.Render(entry.buffers, width, height, format_, solid, composite_);
        }

        if (!result) {
            return Result<bool>::Failure(result.Error());
        }

        entry.published = entry.retained.Resource();
    } else {
        if (image_.IsNull()) {
            auto made = DrawBatchRenderer::CreateImagePipeline(device_, shaders_, format_);
            if (!made) {
                return Result<bool>::Failure(made.Error());
            }

            image_ = made.Value();
        }

        if (entry.target.IsNull() || resize) {
            graphics::TextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.format = format_;
            desc.usage = graphics::TextureUsage::RenderTarget;
            auto made = device_.CreateTexture(desc, {});
            if (!made) {
                return Result<bool>::Failure(made.Error());
            }

            const BrushTexture brush{made.Value(), image_};
            if (entry.target.IsNull()) {
                auto registered = textures_.TryAdd(brush);
                if (!registered) {
                    device_.Retire(made.Value());
                    return Result<bool>::Failure(registered.Error());
                }

                entry.published = registered.Value();
            } else {
                auto* existing = textures_.Find(entry.published);
                if (!existing) {
                    device_.Retire(made.Value());
                    return Result<bool>::Failure(ErrorCode::StaleHandle);
                }

                *existing = brush;
                device_.Retire(entry.target);
            }

            entry.target = made.Value();
        }

        if (kind == WidgetSurfaceKind::Scene) {
            Result<void> painted = Result<void>::Success();
            if (ownedScene) {
                painted = ownedScene->Advance(frame);
                if (!painted) {
                    return Result<bool>::Failure(painted.Error());
                }

                const auto color = ownedScene->Background();
                graphics::PassDesc desc{};
                desc.colorCount = 1;
                desc.colors[0].texture = entry.target;
                desc.colors[0].clear = {color.r, color.g, color.b, color.a};
                if (ownedScene->UsesDepth()) {
                    if (entry.depth.IsNull() || entry.depthWidth != width ||
                        entry.depthHeight != height) {
                        graphics::TextureDesc texture{};
                        texture.width = width;
                        texture.height = height;
                        texture.format = graphics::PixelFormat::D32Float;
                        texture.usage = graphics::TextureUsage::DepthStencil;
                        auto made = device_.CreateTexture(texture, {});
                        if (!made) {
                            return Result<bool>::Failure(made.Error());
                        }

                        if (!entry.depth.IsNull()) {
                            device_.Retire(entry.depth);
                        }

                        entry.depth = made.Value();
                        entry.depthWidth = width;
                        entry.depthHeight = height;
                    }

                    desc.depth.texture = entry.depth;
                    desc.depth.clearDepth = 0;
                }

                auto pass = device_.BeginPass(desc);
                if (!pass) {
                    return Result<bool>::Failure(pass.Error());
                }

                painted = ownedScene->Draw(device_, *pass.Value(), width, height);
                auto submitted = device_.EndFrame();
                if (painted && !submitted) {
                    return Result<bool>::Failure(submitted.Error());
                }
            } else {
                painted = scenePainter_.Execute(*node, device_, entry.target, width, height);
            }

            if (!painted) {
                return Result<bool>::Failure(painted.Error());
            }
        } else if (!channelsChanged && !resize && source.ManagedSurface() == entry.published) {
            return Result<bool>::Success(false);
        }
    }

    entry.width = width;
    entry.height = height;
    source.SetManagedSurface(entry.published);
    return Result<bool>::Success(true);
}

Result<bool> WidgetSurfaceHost::Prepare(const SharedPtr<Element>& root, uint64 frame, uint32 width,
                                        uint32 height, graphics::PipelineHandle solid) noexcept {
    if (!root || width == 0 || height == 0 || solid.IsNull()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (root_.Lock().Get() != root.Get() || revision_ != root->PaintPlanRevision()) {
        root_ = {};
        order_.Clear();
        for (const auto& entry : entries_) {
            entry->found = false;
        }

        auto found = Discover(root, {});
        if (!found) {
            return Result<bool>::Failure(found.Error());
        }

        for (usize i = entries_.Size(); i > 0; --i) {
            if (!entries_[i - 1]->found) {
                (void)entries_.TryRemoveAtSwap(i - 1);
            }
        }

        root_ = root;
    }

    bool changed = false;
    for (Entry* entry : order_) {
        auto updated = Update(*entry, frame, width, height, solid);
        if (!updated) {
            return updated;
        }

        // 안쪽 표면만 변한 프레임에는 바깥 보관 박스가 다음 캡처를 마칠 때까지 창을 다시 그릴
        // 필요가 없다.
        changed |= updated.Value() && !entry->enclosing.Lock();
    }

    revision_ = root->PaintPlanRevision();
    return Result<bool>::Success(changed);
}

} // namespace rocks::ui
