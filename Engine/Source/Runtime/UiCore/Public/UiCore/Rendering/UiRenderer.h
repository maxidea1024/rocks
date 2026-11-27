#pragma once

#include <Core/Containers/Span.h>
#include <UiCore/Rendering/UiDrawBuffer.h>

namespace rocks::ui {

// UI 렌더러 계약이다(UE FSlateRenderer, Docs/Design/UiToolkit 3.7·18.3절). 그래픽 타입이 없고 모든
// 호출은 게임 lane 하나에서 한다(그래서 UE의 자원 critical section이 없다). UiRender가 RenderLane
// 위에 구현하고(LaneUiRenderer), NullUiRenderer는 headless 실행·CPU 시험에 쓴다.
//
// 자원은 id로만 오간다. 텍스처를 해제·크기 변경해도 이미 넘긴 프레임은 그때의 장치 자원을 쓰고,
// 장치 자원은 그 프레임의 GPU 완료 뒤 회수된다(구현의 책임).
//
// 사용 예(한 프레임):
//   auto buffer = renderer.AcquireDrawBuffer();              // 하나뿐이라 반환 전 다시 얻으면 Busy
//   recorder.Record(root, frame, width, height, *buffer.Value());
//   buffer.Value()->AddWindow(surface, width, height, background, batch);
//   glyphs.Upload(renderer);                                 // atlas 페이지는 기록 뒤·그리기 전
//   renderer.DrawWindows(*buffer.Value());
//   renderer.ReleaseDrawBuffer(*buffer.Value());
class ROCKS_UICORE_API UiRenderer {
public:
    explicit UiRenderer(Allocator& allocator) noexcept;
    virtual ~UiRenderer() noexcept = default;

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    // 창 표면(스왑 체인)을 만든다. 결과를 동기로 돌려준다(도킹의 탭 띄우기가 실패를 바로 알도록).
    virtual Result<UiWindowSurfaceId>
    CreateWindowSurface(const UiWindowSurfaceDesc& desc) noexcept = 0;
    virtual Result<void> ResizeWindowSurface(UiWindowSurfaceId surface, uint32 width,
                                             uint32 height) noexcept = 0;
    // 돌아오면 그 표면으로 게시한 그리기가 모두 끝나 있다. 그 뒤 native 창을 부순다.
    virtual Result<void> DestroyWindowSurface(UiWindowSurfaceId surface) noexcept = 0;

    // 텍스처를 만든다. pixels는 비었거나(렌더 타깃·나중에 채울 갱신 텍스처) 위에서 아래 순서의 전체
    // 픽셀(width × height × 형식 byte)이다. 크기가 틀리거나 렌더 타깃이 픽셀을 받거나 Rgba8Srgb가
    // 아니면 InvalidArgument다.
    virtual Result<UiTextureId> CreateTexture(const UiTextureDesc& desc,
                                              Span<const uint8> pixels) noexcept = 0;
    // 내용 전체를 바꾼다(영역 갱신은 Graphics 업로드 ring GX8 뒤). 정적 텍스처·렌더 타깃은
    // InvalidArgument다.
    virtual Result<void> UpdateTexture(UiTextureId texture, Span<const uint8> pixels) noexcept = 0;
    // 렌더 타깃 크기를 바꾼다. id는 그대로이고 내용은 정해지지 않는다.
    virtual Result<void> ResizeRenderTarget(UiTextureId texture, uint32 width,
                                            uint32 height) noexcept = 0;
    virtual void ReleaseTexture(UiTextureId texture) noexcept = 0;
    // 텍스처 설명이다. 모르는 id는 NotFound다.
    virtual Result<UiTextureDesc> DescribeTexture(UiTextureId texture) const noexcept = 0;

    // buffer의 대상을 그린다. 내용은 돌아오기 전에 복사된다. 대상이 없으면 아무것도 하지 않는다.
    // 실패: 모르는 창 표면·오프스크린 대상 NotFound, 오프스크린 대상이 렌더 타깃이 아니거나 크기가
    // 다르면 InvalidArgument. 모르는(해제된) 표면 pass 대상은 그 pass만 건너뛰고, 모르는 텍스처
    // id를 읽는 그리기 명령도 건너뛴다(단색으로 그리지 않는다).
    virtual Result<void> DrawWindows(UiDrawBuffer& buffer) noexcept = 0;

    // 렌더 타깃 내용을 읽는다(그 텍스처를 그린 DrawWindows 뒤). 결과는 WaitCapture 뒤 TakeCapture로
    // 받는다: 위에서 아래 순서 RGBA8(형식 그대로, 4 byte/픽셀). TakeCapture는 모르는(이미 가져간)
    // 번호면 NotFound, 아직 결과가 없으면 NotReady다.
    virtual Result<uint64> RequestCapture(UiTextureId target) noexcept = 0;
    virtual Result<void> WaitCapture(uint64 capture) noexcept = 0;
    virtual Result<void> TakeCapture(uint64 capture, Array<uint8>& rgba) noexcept = 0;

    // 지금까지 넘긴 그리기·자원 작업이 렌더러에서 처리될 때까지 기다린다(UE
    // FlushRenderingCommands). 창 그리기는 이때 present까지 끝나 있다 — 크기 조절 중 그리기가 OS
    // 크기 변경 루프로 돌아가기 전에 부른다(Docs/Design/UiToolkit 18.5절).
    virtual Result<void> Flush() noexcept = 0;

    // 렌더 타깃(UiTextureUsage::RenderTarget, 형식 Rgba8Srgb)을 만든다.
    Result<UiTextureId> CreateRenderTarget(uint32 width, uint32 height,
                                           UiTextureSampling sampling) noexcept;

    // 그리기 버퍼를 얻는다(UE AcquireDrawBuffer). 비워져 있다. 반환하기 전에 다시 얻으면 Busy다.
    Result<UiDrawBuffer*> AcquireDrawBuffer() noexcept;
    // 얻은 버퍼를 돌려준다(내용은 버린다).
    void ReleaseDrawBuffer(UiDrawBuffer& buffer) noexcept;

private:
    UiDrawBuffer buffer_;
    bool acquired_ = false;
};

} // namespace rocks::ui
