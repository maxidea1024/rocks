#pragma once

#include <Core/Delegate.h>
#include <Core/Memory/UniquePtr.h>
#include <UiCore/Element.h>
#include <UiCore/WidgetSurface.h>
#include <UiRender/RetainedSurface.h>
#include <UiRender/PostBufferSet.h>

namespace rocks::ui {

// 창 패스를 시작하기 전에 배치된 트리의 오프스크린 표면을 준비한다. 중첩된 표면은
// 안쪽부터 처리한다. 창에서 분리된 위젯의 텍스처는 다음 Prepare 또는 소멸 시 반환한다.
// 단일 스레드 호스트용이다. 분리된 Game/Render lane에서는 RecordContent의 결과를 전달한다.
class VectorImageAtlas;

class ROCKS_UIRENDER_API WidgetSurfaceHost {
public:
    using ScenePainter = Delegate<Result<void>(Element&, graphics::Device&, graphics::TextureHandle,
                                               uint32, uint32)>;

    WidgetSurfaceHost(
        Allocator& allocator, graphics::Device& device, render::ShaderSet& shaders,
        SlotArray<BrushTexture>& textures,
        graphics::PixelFormat format = graphics::PixelFormat::RGBA8UnormSrgb) noexcept;
    ~WidgetSurfaceHost() noexcept;
    WidgetSurfaceHost(const WidgetSurfaceHost&) = delete;
    WidgetSurfaceHost& operator=(const WidgetSurfaceHost&) = delete;

    // 장면 렌더러는 타깃의 패스를 직접 열고 닫는다. 외부 장면 텍스처가 연결된 뷰포트는 건너뛴다.
    void SetScenePainter(ScenePainter painter) noexcept;
    // 보관 표면 내용의 SVG brush를 풀 atlas와 그 페이지를 올릴 image 파이프라인이다(Docs/Design/
    // UiToolkit 17.2절). 내용을 기록한 뒤 렌더 전에 atlas를 Sync한다. atlas는 이 호스트보다 오래
    // 살아야 한다. nullptr이면 보관 표면 안의 벡터 brush는 그리지 않는다.
    void SetVectorImages(VectorImageAtlas* atlas, graphics::PipelineHandle imagePipeline) noexcept;
    SlotHandle GetPostBuffer(uint32 channel) const noexcept;
    // true면 같은 정점 배치도 다시 표시해야 한다. frame은 호스트의 논리 프레임 번호다.
    Result<bool> Prepare(const SharedPtr<Element>& root, uint64 frame, uint32 width, uint32 height,
                         graphics::PipelineHandle solid) noexcept;

private:
    struct Entry;
    Result<void> Discover(const SharedPtr<Element>& node,
                          const SharedPtr<Element>& enclosingRetainer) noexcept;
    Result<bool> Update(Entry& entry, uint64 frame, uint32 width, uint32 height,
                        graphics::PipelineHandle solid) noexcept;
    Allocator& allocator_;
    graphics::Device& device_;
    render::ShaderSet& shaders_;
    SlotArray<BrushTexture>& textures_;
    graphics::PixelFormat format_;
    PostBufferSet postBuffers_;
    Array<UniquePtr<Entry>> entries_;
    Array<Entry*> order_;
    WeakPtr<Element> root_;
    uint64 revision_ = 0;
    graphics::PipelineHandle composite_{};
    graphics::PipelineHandle image_{};
    ScenePainter scenePainter_{};
    VectorImageAtlas* vectorImages_ = nullptr;
    graphics::PipelineHandle vectorPipeline_{};
};

} // namespace rocks::ui
