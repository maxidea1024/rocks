#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/StringView.h>
#include <UiCore/VectorImageResolver.h>
#include <UiRender/DrawBatchRenderer.h>

namespace rocks::platform {

class FileSystem;

} // namespace rocks::platform

// 벡터 이미지(SVG) atlas다(Docs/Design/UiToolkit 17.2절 — UE FSlateVectorGraphicsCache +
// FSlateSVGRasterizer). SVG를 등록해 파싱해 두고, 그리기 목록 기록 때 (키, 화소 크기)마다 한 번
// nanosvg로 래스터화해 1024×1024 RGBA atlas 페이지에 넣는다. 페이지는 brush 등록부에 자리를 먼저
// 받고, 텍스처 생성·업로드는 Sync가 한다.
//
// 프레임 순서(렌더 소유 스레드가 아니면 호출자가 직렬화한다 — 글리프 cache와 같다):
//   atlas.Register(...) 또는 RegisterDirectory(...)   // 시작 때
//   application.SetVectorImageResolver(&atlas);
//   ... app.UpdatePaintCache(...)  — 벡터 brush가 Resolve를 부른다
//   atlas.Sync(imagePipeline);                         // Draw 전
//   DrawBatchRenderer::Draw(...);
//   atlas.AdvanceFrame();                              // 한도를 넘었으면 비우고 Revision을 올린다
//
// 화소는 nanosvg 결과 그대로인 straight alpha sRGB RGBA(투명 화소에 이웃 색을 번지게 한 것)이고
// 텍스처 형식은 RGBA8UnormSrgb, 파이프라인은 image(straight alpha 섞기)다.
//
// 수명: 장치·등록부·파이프라인이 이 객체보다 오래 살아야 한다.
namespace rocks::ui {

// atlas 페이지 한 변(화소)이다(UE FSlateVectorGraphicsCache AtlasSize).
inline constexpr uint32 VectorAtlasPageSize = 1024;

// 이 수보다 페이지가 많아지면 AdvanceFrame이 atlas를 비운다(UE Slate.MaxSVGAtlasPagesBeforeFlush —
// 17.2절).
inline constexpr usize MaxVectorAtlasPages = 4;

// SVG 파일 하나의 byte 상한이다.
inline constexpr uint64 MaxVectorImageFileBytes = 1u << 20;

class ROCKS_UIRENDER_API VectorImageAtlas final : public VectorImageResolver {
public:
    VectorImageAtlas(Allocator& allocator, graphics::Device& device,
                     SlotArray<BrushTexture>& textures) noexcept;
    ~VectorImageAtlas() noexcept override;

    VectorImageAtlas(const VectorImageAtlas&) = delete;
    VectorImageAtlas& operator=(const VectorImageAtlas&) = delete;

    // path(키 경로 — 아이콘 루트 기준, 확장자 없음, '/' 구분)로 SVG 하나를 등록하고 키를 돌려준다.
    // 빈 경로·파싱 실패·크기 0인 그림은 InvalidArgument, 같은 경로나 해시가 겹치는 경로는
    // AlreadyExists, byte 수가 MaxVectorImageFileBytes를 넘으면 Overflow다.
    Result<VectorImageKey> Register(StringView path, Span<const uint8> svg) noexcept;

    // root 아래 모든 *.svg를 하위 디렉터리까지 등록하고 등록한 수를 돌려준다. 키 경로는 root 기준
    // 상대 경로에서 ".svg"를 뺀 것이다(예: root/Common/Check.svg → "Common/Check"). root가 없으면
    // NotFound, 파일 하나라도 실패하면 그 오류다(이미 등록한 것은 남는다).
    Result<usize> RegisterDirectory(platform::FileSystem& files, StringView root) noexcept;

    // 등록 순서의 그림 수·경로·키다(갤러리 아이콘 목록).
    usize ImageCount() const noexcept;
    StringView ImagePath(usize index) const noexcept;
    VectorImageKey ImageKey(usize index) const noexcept;

    Result<ResolvedVectorImage> Resolve(VectorImageKey key, uint32 width,
                                        uint32 height) noexcept override;
    uint64 Revision() const noexcept override;

    // 새 페이지의 텍스처를 만들고 바뀐 페이지를 올린다. 파이프라인이 비었으면 InvalidArgument,
    // 생성·업로드 실패는 그 오류다.
    Result<void> Sync(graphics::PipelineHandle imagePipeline) noexcept;

    // 프레임을 마친다. 페이지가 MaxVectorAtlasPages를 넘었으면 모두 놓고 Revision을 올린다(다음
    // 프레임의 그리기 목록이 다시 만들어진다). 비웠으면 true다.
    bool AdvanceFrame() noexcept;

    // 시험·진단용이다: 지금 페이지 수, 지금까지 래스터화한 횟수.
    usize PageCount() const noexcept;
    uint64 RasterCount() const noexcept;
    // 페이지 index의 CPU 화소(straight alpha RGBA, 첫 행이 위)와 크기다. 없으면 빈 범위다.
    Span<const uint8> PagePixels(usize index, uint32& width, uint32& height) const noexcept;

private:
    struct State;

    void ReleasePages() noexcept;

    graphics::Device& device_;
    SlotArray<BrushTexture>& textures_;
    UniquePtr<State> state_;
};

} // namespace rocks::ui
