#include <UiCore/Rendering/SurfaceRecorder.h>
#include <UiCore/DrawBatch.h>
#include <UiCore/Rendering/UiSceneExtract.h>
#include <UiCore/Rendering/ViewportSource.h>

namespace rocks::ui {

namespace {

// 보관 표면은 투명 바탕에 그린다(합성 셰이더가 미리 곱한 알파로 읽는다).
constexpr LinearColor RetainedSurfaceClear{0, 0, 0, 0};

} // namespace

// post buffer 채널 묶음이다. 같은 pass(대상 또는 감싸는 보관 표면)의 같은 채널은 한 타깃을
// 공유한다.
struct SurfaceRecorder::PostBufferGroup {
    UiTextureId targets[PostBufferChannelCount]{};
    uint32 widths[PostBufferChannelCount]{};
    uint32 heights[PostBufferChannelCount]{};
};

struct SurfaceRecorder::Entry {
    Entry(Allocator& allocator, UiRenderer& owner, const SharedPtr<Element>& node) noexcept
        : renderer(owner), widget(node), list(allocator), batch(allocator) {
    }

    ~Entry() noexcept {
        if (auto node = widget.Lock()) {
            if (auto* source = node->GetWidgetSurface()) {
                for (uint32 i = 0; i < PostBufferChannelCount; ++i) {
                    if (!channels[i].IsNull() && source->GetPostBuffer(i) == channels[i]) {
                        source->SetPostBuffer(i, {});
                    }
                }

                if (!target.IsNull() && source->ManagedSurface() == target) {
                    source->SetManagedSurface({});
                }
            }
        }

        if (!target.IsNull()) {
            renderer.ReleaseTexture(target);
        }

        for (UiTextureId group : postBuffers.targets) {
            if (!group.IsNull()) {
                renderer.ReleaseTexture(group);
            }
        }
    }

    UiRenderer& renderer;
    WeakPtr<Element> widget;
    WeakPtr<Element> enclosing;
    PaintList list;
    DrawBatch batch;
    // 이 표면이 감싸는 post buffer 묶음이다(보관 표면일 때만 쓴다).
    PostBufferGroup postBuffers;
    UiTextureId channels[PostBufferChannelCount]{};
    UiTextureId target{};
    uint32 width = 0;
    uint32 height = 0;
    bool found = false;
};

SurfaceRecorder::SurfaceRecorder(Allocator& allocator, UiRenderer& renderer) noexcept
    : allocator_(allocator), renderer_(renderer), entries_(allocator), order_(allocator) {
}

SurfaceRecorder::~SurfaceRecorder() noexcept {
    entries_.Clear();
    if (postBuffers_) {
        for (UiTextureId target : postBuffers_->targets) {
            if (!target.IsNull()) {
                renderer_.ReleaseTexture(target);
            }
        }
    }
}

void SurfaceRecorder::SetVectorImages(VectorImageResolver* resolver) noexcept {
    vectorImages_ = resolver;
}

UiTextureId SurfaceRecorder::GetPostBuffer(uint32 channel) const noexcept {
    return postBuffers_ && channel < PostBufferChannelCount ? postBuffers_->targets[channel]
                                                            : UiTextureId{};
}

usize SurfaceRecorder::RenderTargetCount() const noexcept {
    usize count = 0;
    const auto countGroup = [&count](const PostBufferGroup& group) noexcept {
        for (UiTextureId target : group.targets) {
            count += target.IsNull() ? 0 : 1;
        }
    };
    if (postBuffers_) {
        countGroup(*postBuffers_);
    }

    for (const auto& entry : entries_) {
        count += entry->target.IsNull() ? 0 : 1;
        countGroup(entry->postBuffers);
    }

    return count;
}

Result<void> SurfaceRecorder::Discover(const SharedPtr<Element>& node,
                                       const SharedPtr<Element>& enclosingRetainer) noexcept {
    const auto visible = node->GetVisibility();
    if (visible == Visibility::Hidden || visible == Visibility::Collapsed) {
        return Result<void>::Success();
    }

    auto* source = node->GetWidgetSurface();
    const auto enclosing = source && source->SurfaceKind() == WidgetSurfaceKind::RetainedContent
                               ? node
                               : enclosingRetainer;
    for (const auto& child : node->ActiveChildren()) {
        if (auto result = Discover(child, enclosing); !result) {
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
        auto created = UniquePtr<Entry>::TryMake(allocator_, allocator_, renderer_, node);
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        current = created.Value().Get();
        if (auto added = entries_.TryPushBack(Move(created.Value())); !added) {
            return added;
        }
    }

    current->found = true;
    current->enclosing = enclosingRetainer;
    return order_.TryPushBack(current);
}

Result<UiTextureId> SurfaceRecorder::PrepareTarget(UiTextureId current, uint32 currentWidth,
                                                   uint32 currentHeight, uint32 width,
                                                   uint32 height,
                                                   UiTextureSampling sampling) noexcept {
    if (current.IsNull()) {
        return renderer_.CreateRenderTarget(width, height, sampling);
    }

    if (currentWidth != width || currentHeight != height) {
        if (auto resized = renderer_.ResizeRenderTarget(current, width, height); !resized) {
            return Result<UiTextureId>::Failure(resized.Error());
        }
    }

    return Result<UiTextureId>::Success(current);
}

bool SurfaceRecorder::EnclosingCaptures(const Entry& entry, uint64 frame) const noexcept {
    const auto outer = entry.enclosing.Lock();
    if (!outer) {
        return true;
    }

    const Entry* outerEntry = nullptr;
    for (const auto& candidate : entries_) {
        if (candidate->widget.Lock().Get() == outer.Get()) {
            outerEntry = candidate.Get();
            break;
        }
    }

    auto* surface = outer->GetWidgetSurface();
    if (!outerEntry || !surface || outerEntry->target.IsNull() ||
        surface->ManagedSurface() != outerEntry->target) {
        return true;
    }

    // 바깥 Update와 같은 크기 판정이다(크기가 바뀌면 다시 캡처한다).
    const auto size = outer->GetGeometry().size;
    int32 x, y;
    if (!math::TryTruncToInt32(math::Ceil(size.x), x) ||
        !math::TryTruncToInt32(math::Ceil(size.y), y) ||
        static_cast<uint32>(x) != outerEntry->width ||
        static_cast<uint32>(y) != outerEntry->height) {
        return true;
    }

    return surface->NeedsCapture(frame);
}

void SurfaceRecorder::ReleaseTarget(Entry& entry) noexcept {
    if (!entry.target.IsNull()) {
        renderer_.ReleaseTexture(entry.target);
        entry.target = {};
    }

    entry.width = 0;
    entry.height = 0;
}

Result<bool> SurfaceRecorder::Update(Entry& entry, uint64 frame, uint32 width, uint32 height,
                                     UiDrawBuffer& buffer) noexcept {
    auto node = entry.widget.Lock();
    if (!node) {
        return Result<bool>::Success(false);
    }

    auto& source = *node->GetWidgetSurface();
    const auto kind = source.SurfaceKind();
    const auto scene = source.GetViewportSource();
    const uint32 channelMask = source.RequestedPostBuffers();
    // 표면이 필요 없거나 장면 source 없는 viewport(외부 장면 텍스처도 없음)는 타깃을 놓는다.
    if ((!source.WantsManagedSurface() && channelMask == 0) ||
        (kind == WidgetSurfaceKind::Scene && !scene)) {
        for (uint32 i = 0; i < PostBufferChannelCount; ++i) {
            if (!entry.channels[i].IsNull() && source.GetPostBuffer(i) == entry.channels[i]) {
                source.SetPostBuffer(i, {});
            }

            entry.channels[i] = {};
        }

        const bool removed = !entry.target.IsNull() && source.ManagedSurface() == entry.target;
        if (removed) {
            source.SetManagedSurface({});
        }

        ReleaseTarget(entry);
        return Result<bool>::Success(removed);
    }

    // post buffer는 자신이 기록되는 pass와 같은 크기여야 한다.
    const auto renderArea = kind == WidgetSurfaceKind::PostBuffer ? entry.enclosing.Lock() : node;
    if (renderArea) {
        const auto size = renderArea->GetGeometry().size;
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
        PostBufferGroup* group = nullptr;
        if (renderArea && channelMask) {
            for (const auto& candidate : entries_) {
                if (candidate->widget.Lock().Get() == renderArea.Get()) {
                    group = &candidate->postBuffers;
                    break;
                }
            }
        }

        if (!group && channelMask) {
            if (!postBuffers_) {
                auto made = UniquePtr<PostBufferGroup>::TryMake(allocator_);
                if (!made) {
                    return Result<bool>::Failure(made.Error());
                }

                postBuffers_ = Move(made.Value());
            }

            group = postBuffers_.Get();
        }

        for (uint32 i = 0; i < PostBufferChannelCount; ++i) {
            UiTextureId handle{};
            if ((channelMask & (1u << i)) != 0) {
                auto prepared =
                    PrepareTarget(group->targets[i], group->widths[i], group->heights[i], width,
                                  height, UiTextureSampling::Image);
                if (!prepared) {
                    return Result<bool>::Failure(prepared.Error());
                }

                channelsChanged |= group->widths[i] != width || group->heights[i] != height;
                group->targets[i] = prepared.Value();
                group->widths[i] = width;
                group->heights[i] = height;
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
                source.SetManagedSurface({});
                renderer_.ReleaseTexture(entry.target);
                entry.target = {};
            }

            const bool resized = width != entry.width || height != entry.height;
            entry.width = width;
            entry.height = height;
            return Result<bool>::Success(channelsChanged || resized);
        }
    }

    const bool resize = entry.width != width || entry.height != height;
    if (kind == WidgetSurfaceKind::RetainedContent) {
        if (!resize && !entry.target.IsNull() && source.ManagedSurface() == entry.target &&
            !source.NeedsCapture(frame)) {
            return Result<bool>::Success(false);
        }

        entry.list.Clear();
        entry.list.SetVectorImageResolver(vectorImages_);
        auto recorded = source.RecordContent(entry.list);
        if (recorded) {
            recorded = entry.batch.Build(entry.list);
        }

        if (!recorded) {
            return Result<bool>::Failure(recorded.Error());
        }

        auto target = PrepareTarget(entry.target, entry.width, entry.height, width, height,
                                    UiTextureSampling::Premultiplied);
        if (!target) {
            return Result<bool>::Failure(target.Error());
        }

        entry.target = target.Value();
        auto added =
            buffer.AddRetainedPass(entry.target, width, height, RetainedSurfaceClear, entry.batch);
        if (!added) {
            return Result<bool>::Failure(added.Error());
        }
    } else {
        auto target = PrepareTarget(entry.target, entry.width, entry.height, width, height,
                                    UiTextureSampling::Image);
        if (!target) {
            return Result<bool>::Failure(target.Error());
        }

        entry.target = target.Value();
        if (kind == WidgetSurfaceKind::Scene) {
            if (auto advanced = scene->Advance(frame); !advanced) {
                return Result<bool>::Failure(advanced.Error());
            }

            // 감싸는 보관 표면이 이번에 캡처하지 않으면 장면 결과를 볼 곳이 없다. 장면 상태만
            // 진행하고 pass는 쌓지 않는다(옛 경로처럼 창을 다시 그리지 않는다). 새로 만들었거나
            // 크기를 바꾼 타깃은 내용이 없으므로 언제나 그린다.
            if (resize || EnclosingCaptures(entry, frame)) {
                auto extract =
                    buffer.AddScenePass(entry.target, width, height, scene->Background());
                if (!extract) {
                    return Result<bool>::Failure(extract.Error());
                }

                if (auto posted = scene->Extract(extract.Value(), {entry.target, width, height});
                    !posted) {
                    return Result<bool>::Failure(posted.Error());
                }
            }
        } else if (!channelsChanged && !resize && source.ManagedSurface() == entry.target) {
            return Result<bool>::Success(false);
        }
    }

    entry.width = width;
    entry.height = height;
    source.SetManagedSurface(entry.target);
    return Result<bool>::Success(true);
}

Result<bool> SurfaceRecorder::Record(const SharedPtr<Element>& root, uint64 frame, uint32 width,
                                     uint32 height, UiDrawBuffer& buffer) noexcept {
    if (!root || width == 0 || height == 0) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (root_.Lock().Get() != root.Get() || revision_ != root->PaintPlanRevision()) {
        root_ = {};
        order_.Clear();
        for (const auto& entry : entries_) {
            entry->found = false;
        }

        if (auto found = Discover(root, {}); !found) {
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
        auto updated = Update(*entry, frame, width, height, buffer);
        if (!updated) {
            return updated;
        }

        // 안쪽 표면만 변한 프레임에는 바깥 보관 표면이 다음 캡처를 마칠 때까지 대상을 다시 그릴
        // 필요가 없다.
        changed |= updated.Value() && !entry->enclosing.Lock();
    }

    revision_ = root->PaintPlanRevision();
    return Result<bool>::Success(changed);
}

} // namespace rocks::ui
