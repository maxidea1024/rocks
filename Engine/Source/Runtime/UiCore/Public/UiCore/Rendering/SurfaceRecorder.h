#pragma once

#include <Core/Memory/UniquePtr.h>
#include <UiCore/Element.h>
#include <UiCore/Rendering/UiRenderer.h>
#include <UiCore/WidgetSurface.h>

namespace rocks::ui {

// 그리기 대상 하나(창·오프스크린)의 Element 트리에서 보관 표면·post buffer·viewport를 찾아, 대상
// 앞에 그릴 pass를 draw buffer에 쌓는다(Docs/Design/UiToolkit 18.3절, UE SRetainerWidget의
// FWidgetRenderer 갱신과 SViewport 그리기). 그래픽 타입이 없고 게임 lane에서만 쓴다.
//
// - 트리를 자식 먼저 순회하므로 pass는 안쪽 표면부터 쌓인다(RetainedPanel 안 viewport의 장면 pass가
//   보관 pass보다 앞).
// - 표면의 렌더 타깃(보관·viewport·post buffer)은 이 객체가 UiRenderer로 만들고 크기를 바꾸며, 표면
//   Element에 id를 알린다(SetManagedSurface·SetPostBuffer). 트리에서 떨어진 표면의 타깃은 다음
//   Record나 소멸 때 돌려준다.
// - 보관 표면 내용은 다시 그릴 때만 pass를 쌓는다(바뀌지 않으면 지난 내용을 그대로 쓴다).
//
// 사용 예(대상 하나, 매 프레임):
//   auto changed = recorder.Record(root, frame, width, height, buffer);   // 표면 pass
//   // 바깥 보관 표면 안쪽만 바뀐 프레임은 changed가 false여도 pass가 쌓인다. 표면 상태는 이미
//   // 갱신됐으므로 그 pass를 버리면 타깃 내용이 비게 된다 — 쌓인 pass가 있으면 대상과 함께 그린다.
//   if (changed.Value() || windowDirty || buffer.PendingPassCount() > 0) {
//       buffer.AddWindow(surface, width, height, background, windowBatch);  // pass를 닫는다
//   }
//
// 수명: renderer는 이 객체보다 오래 산다. 쌓인 pass의 DrawBatch는 이 객체가 소유하므로
// DrawWindows가 끝날 때까지 이 객체를 부수거나 다시 Record하지 않는다.
class ROCKS_UICORE_API SurfaceRecorder {
public:
    SurfaceRecorder(Allocator& allocator, UiRenderer& renderer) noexcept;
    ~SurfaceRecorder() noexcept;

    SurfaceRecorder(const SurfaceRecorder&) = delete;
    SurfaceRecorder& operator=(const SurfaceRecorder&) = delete;

    // 보관 표면 내용의 SVG brush를 풀 resolver다(Docs/Design/UiToolkit 17.2절). resolver는 이
    // 객체보다 오래 산다. nullptr이면 보관 표면 안의 벡터 brush는 그리지 않는다.
    void SetVectorImages(VectorImageResolver* resolver) noexcept;

    // 대상 수준(감싸는 보관 표면 없음) post buffer 채널의 렌더 타깃이다.
    UiTextureId GetPostBuffer(uint32 channel) const noexcept;

    // root 트리의 표면을 갱신하고 pass를 buffer에 쌓는다. true면 대상도 다시 그려야 한다(감싸는
    // 보관 표면 없는 표면이 바뀌었다). width·height는 대상 크기다(감싸는 보관 표면 없는 post buffer
    // 크기). 쌓인 pass는 버리지 말고 대상과 함께 그린다(위 사용 예). 실패하면 그때까지 쌓은 pass가
    // buffer에 남으므로 호출자가 DiscardPending한다(그 프레임의 표면 상태는 정해지지 않는다).
    Result<bool> Record(const SharedPtr<Element>& root, uint64 frame, uint32 width, uint32 height,
                        UiDrawBuffer& buffer) noexcept;

    // 이 객체가 가진 렌더 타깃 수다(시험·누수 확인).
    usize RenderTargetCount() const noexcept;

private:
    struct PostBufferGroup;
    struct Entry;

    Result<void> Discover(const SharedPtr<Element>& node,
                          const SharedPtr<Element>& enclosingRetainer) noexcept;
    Result<bool> Update(Entry& entry, uint64 frame, uint32 width, uint32 height,
                        UiDrawBuffer& buffer) noexcept;
    Result<UiTextureId> PrepareTarget(UiTextureId current, uint32 currentWidth,
                                      uint32 currentHeight, uint32 width, uint32 height,
                                      UiTextureSampling sampling) noexcept;
    void ReleaseTarget(Entry& entry) noexcept;
    // entry를 감싸는 보관 표면이 이번 프레임에 다시 캡처하는지다(없으면 true).
    bool EnclosingCaptures(const Entry& entry, uint64 frame) const noexcept;

    Allocator& allocator_;
    UiRenderer& renderer_;
    UniquePtr<PostBufferGroup> postBuffers_;
    Array<UniquePtr<Entry>> entries_;
    Array<Entry*> order_;
    WeakPtr<Element> root_;
    uint64 revision_ = 0;
    VectorImageResolver* vectorImages_ = nullptr;
};

} // namespace rocks::ui
