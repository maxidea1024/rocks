#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/DrawBatch.h>
#include <UiCore/Rendering/NullUiRenderer.h>
#include <UiCore/Rendering/SurfaceRecorder.h>
#include <UiCore/Rendering/ViewportSource.h>

using namespace rocks;
using namespace rocks::ui;

// UI 렌더 경계 계약 시험이다(Docs/Design/UiToolkit 18.3·18.7절 — UiRenderer·UiDrawBuffer·
// SurfaceRecorder·장면 packet). 그래픽 없이 NullUiRenderer로 pass 순서·자원 수명을 단언한다.

namespace {

// 장면 packet의 소멸·기록 횟수다(시험 안에서만 센다).
int scenePacketsDestroyed = 0;
int scenePacketsRecorded = 0;

struct ScenePacket {
    uint32 value = 0;
    bool live = true;

    ScenePacket() noexcept = default;

    explicit ScenePacket(uint32 v) noexcept : value(v) {
    }

    ScenePacket(ScenePacket&& other) noexcept : value(other.value), live(other.live) {
        other.live = false;
    }

    ScenePacket& operator=(ScenePacket&&) = delete;

    ~ScenePacket() noexcept {
        if (live) {
            ++scenePacketsDestroyed;
        }
    }
};

} // namespace

// 장면 packet은 렌더 lane packet과 같은 등록이 필요하다(UiSceneExtract::Post — RenderBase 5.2절).
ROCKS_LANE_TRANSFERABLE(ScenePacket);

namespace {

void RecordScene(UiScenePassContext&, ScenePacket& packet) noexcept {
    scenePacketsRecorded += static_cast<int>(packet.value);
}

class FakeScene final : public ViewportSource {
public:
    Result<void> Advance(uint64 frame, const FrameTime& time) noexcept override {
        lastTime = time;
        if (frame != lastFrame) {
            lastFrame = frame;
            ++advances;
        }

        return Result<void>::Success();
    }

    LinearColor Background() const noexcept override {
        return {0.1f, 0.2f, 0.3f, 1.0f};
    }

    Result<void> Extract(UiSceneExtract& extract, const ViewportTarget& target) noexcept override {
        lastTarget = target;
        return extract.Post(ScenePacket{1}, &RecordScene);
    }

    uint64 lastFrame = 0;
    FrameTime lastTime{};
    int advances = 0;
    ViewportTarget lastTarget{};
};

class FakeRetained final : public Element, public WidgetSurface {
    ROCKS_UI_ELEMENT(FakeRetained, Element)

public:
    explicit FakeRetained(Allocator& allocator) noexcept : Element(allocator) {
    }

    WidgetSurface* GetWidgetSurface() noexcept override {
        return this;
    }

    WidgetSurfaceKind SurfaceKind() const noexcept override {
        return WidgetSurfaceKind::RetainedContent;
    }

    bool WantsManagedSurface() const noexcept override {
        return true;
    }

    SlotHandle ManagedSurface() const noexcept override {
        return managed;
    }

    void SetManagedSurface(SlotHandle texture) noexcept override {
        managed = texture;
    }

    bool NeedsCapture(uint64) const noexcept override {
        return capture;
    }

    Result<void> RecordContent(PaintList& output) const noexcept override {
        Brush brush;
        brush.mode = BrushMode::Box;
        return output.TryAddBrush(GetGeometry(), brush, {1, 1, 1, 1}, 0);
    }

    SlotHandle managed{};
    bool capture = false;
};

class FakeViewport final : public Element, public WidgetSurface {
    ROCKS_UI_ELEMENT(FakeViewport, Element)

public:
    explicit FakeViewport(Allocator& allocator) noexcept : Element(allocator) {
    }

    WidgetSurface* GetWidgetSurface() noexcept override {
        return this;
    }

    WidgetSurfaceKind SurfaceKind() const noexcept override {
        return WidgetSurfaceKind::Scene;
    }

    bool WantsManagedSurface() const noexcept override {
        return true;
    }

    SlotHandle ManagedSurface() const noexcept override {
        return managed;
    }

    void SetManagedSurface(SlotHandle texture) noexcept override {
        managed = texture;
    }

    SharedPtr<ViewportSource> GetViewportSource() const noexcept override {
        return scene;
    }

    SlotHandle managed{};
    SharedPtr<ViewportSource> scene;
};

Geometry Size(float width, float height) noexcept {
    return Geometry{{width, height}, {}, {}};
}

// 보관 표면(바깥) 안에 viewport(안쪽)가 있는 트리다.
struct NestedTree {
    SharedPtr<Element> root;
    SharedPtr<FakeRetained> retained;
    SharedPtr<FakeViewport> viewport;
    SharedPtr<FakeScene> scene;
};

NestedTree MakeNestedTree(Allocator& allocator) noexcept {
    NestedTree tree;
    tree.root = SharedPtr<Element>::TryMake(allocator, allocator).Value();
    tree.retained = SharedPtr<FakeRetained>::TryMake(allocator, allocator).Value();
    tree.viewport = SharedPtr<FakeViewport>::TryMake(allocator, allocator).Value();
    tree.scene = SharedPtr<FakeScene>::TryMake(allocator).Value();
    tree.viewport->scene = tree.scene;
    tree.root->SetGeometry(Size(200, 100));
    tree.retained->SetGeometry(Size(120, 80));
    tree.viewport->SetGeometry(Size(64, 48));
    (void)tree.retained->TryAddChild(tree.viewport);
    (void)tree.root->TryAddChild(tree.retained);
    return tree;
}

} // namespace

TEST(UiCore_SurfaceRecorderOrdersInnerScenesBeforeRetained) {
    SystemAllocator allocator;
    NullUiRenderer renderer(allocator);
    auto tree = MakeNestedTree(allocator);
    SurfaceRecorder recorder(allocator, renderer);
    auto buffer = renderer.AcquireDrawBuffer();
    REQUIRE(buffer);
    auto changed = recorder.Record(tree.root, 1, 200, 100, *buffer.Value());
    REQUIRE(changed);
    CHECK(changed.Value());
    // 자식 먼저: RetainedPanel 안 viewport의 장면 pass가 보관 pass보다 앞이다(18.3절).
    const Span<const UiSurfacePass> passes = buffer.Value()->Passes();
    REQUIRE(passes.Size() == 2);
    CHECK(passes[0].kind == UiSurfacePassKind::Scene);
    CHECK(passes[0].width == 64 && passes[0].height == 48);
    CHECK(passes[0].packetCount == 1);
    CHECK(passes[0].clear == LinearColor(0.1f, 0.2f, 0.3f, 1.0f));
    CHECK(passes[1].kind == UiSurfacePassKind::Retained);
    CHECK(passes[1].width == 120 && passes[1].height == 80);
    CHECK(passes[1].clear == LinearColor(0, 0, 0, 0));
    REQUIRE(passes[1].content != nullptr);
    CHECK(passes[1].content->Commands().Size() == 1);
    // 표면 Element가 렌더 타깃 id를 받고, 렌더 타깃의 sampling은 표면 종류를 따른다.
    CHECK(tree.viewport->managed == passes[0].target);
    CHECK(tree.retained->managed == passes[1].target);
    CHECK(tree.scene->lastTarget.texture == passes[0].target);
    CHECK(renderer.DescribeTexture(passes[0].target).Value().sampling == UiTextureSampling::Image);
    CHECK(renderer.DescribeTexture(passes[1].target).Value().sampling ==
          UiTextureSampling::Premultiplied);
    // 대상이 쌓인 pass를 닫는다.
    DrawBatch window(allocator);
    REQUIRE(buffer.Value()->AddWindow(
        renderer.CreateWindowSurface({.width = 200, .height = 100}).Value(), 200, 100, {0, 0, 0, 1},
        window));
    REQUIRE(buffer.Value()->Targets().Size() == 1);
    CHECK(buffer.Value()->Targets()[0].firstPass == 0);
    CHECK(buffer.Value()->Targets()[0].passCount == 2);
    CHECK(buffer.Value()->PendingPassCount() == 0);
    REQUIRE(renderer.DrawWindows(*buffer.Value()));
    CHECK(renderer.Statistics().targetsDrawn == 1 && renderer.Statistics().passesDrawn == 2);
    renderer.ReleaseDrawBuffer(*buffer.Value());
}

TEST(UiCore_SurfaceRecorderRedrawsRetainedOnlyWhenNeeded) {
    SystemAllocator allocator;
    NullUiRenderer renderer(allocator);
    auto tree = MakeNestedTree(allocator);
    SurfaceRecorder recorder(allocator, renderer);
    UiDrawBuffer buffer(allocator);
    // 루트에 단 묶음의 프레임 시각이 장면 진행에 간다(20.3절).
    auto host = SharedPtr<UiHostServices>::TryMake(allocator, allocator);
    REQUIRE(host);
    host.Value()->SetFrameTime({3.0, 0.5});
    REQUIRE(tree.root->SetUiHostServices(host.Value()));
    REQUIRE(recorder.Record(tree.root, 1, 200, 100, buffer));
    CHECK(tree.scene->lastTime.current == 3.0);
    CHECK(tree.scene->lastTime.delta == 0.5);
    const UiTextureId retained = tree.retained->managed;
    buffer.Clear();
    // 바뀌지 않은 보관 표면은 다시 캡처하지 않는다. 그 안의 장면은 상태만 진행하고(Advance) pass를
    // 쌓지 않는다 — 볼 곳이 없고, 쌓으면 대상을 매 프레임 다시 그리게 된다(18.4절).
    auto second = recorder.Record(tree.root, 2, 200, 100, buffer);
    REQUIRE(second);
    CHECK(buffer.Passes().Size() == 0);
    CHECK(tree.scene->advances == 2);
    CHECK(!second.Value());
    // 같은 프레임 번호로 다시 기록하면 장면을 두 번 진행하지 않는다(FakeScene이 프레임마다 센다).
    REQUIRE(recorder.Record(tree.root, 2, 200, 100, buffer));
    CHECK(tree.scene->advances == 2);
    buffer.Clear();
    // 캡처를 요청하면 보관 pass가 다시 쌓이고 같은 id를 쓴다.
    tree.retained->capture = true;
    auto third = recorder.Record(tree.root, 3, 200, 100, buffer);
    REQUIRE(third);
    CHECK(third.Value());
    REQUIRE(buffer.Passes().Size() == 2);
    CHECK(buffer.Passes()[1].target == retained);
    // 크기를 바꿔도 id는 그대로다(18.4절).
    buffer.Clear();
    tree.retained->capture = false;
    tree.retained->SetGeometry(Size(150, 90));
    REQUIRE(recorder.Record(tree.root, 4, 200, 100, buffer));
    REQUIRE(buffer.Passes().Size() == 2);
    CHECK(buffer.Passes()[1].target == retained);
    CHECK(renderer.DescribeTexture(retained).Value().width == 150);
}

TEST(UiCore_SurfaceRecorderReleasesTargetsOfDetachedSurfaces) {
    SystemAllocator allocator;
    NullUiRenderer renderer(allocator);
    auto tree = MakeNestedTree(allocator);
    {
        SurfaceRecorder recorder(allocator, renderer);
        UiDrawBuffer buffer(allocator);
        REQUIRE(recorder.Record(tree.root, 1, 200, 100, buffer));
        CHECK(renderer.LiveTextures() == 2);
        CHECK(recorder.RenderTargetCount() == 2);
        buffer.Clear();
        // 트리에서 떨어진 viewport의 타깃은 다음 Record에서 돌려주고 id 알림도 지운다.
        REQUIRE(tree.retained->RemoveChild(*tree.viewport));
        REQUIRE(recorder.Record(tree.root, 2, 200, 100, buffer));
        CHECK(renderer.LiveTextures() == 1);
        CHECK(tree.viewport->managed.IsNull());
    }

    // 소멸하면 남은 타깃을 모두 돌려준다.
    CHECK(renderer.LiveTextures() == 0);
    CHECK(tree.retained->managed.IsNull());
}

TEST(UiCore_DrawBufferKeepsScenePacketsContiguousPerPass) {
    SystemAllocator allocator;
    NullUiRenderer renderer(allocator);
    UiDrawBuffer buffer(allocator);
    const UiTextureId target = renderer.CreateRenderTarget(8, 8, UiTextureSampling::Image).Value();
    DrawBatch content(allocator);
    scenePacketsDestroyed = 0;
    {
        auto first = buffer.AddScenePass(target, 8, 8, {0, 0, 0, 1});
        REQUIRE(first);
        REQUIRE(first.Value().Post(ScenePacket{1}, &RecordScene));
        REQUIRE(buffer.AddRetainedPass(target, 8, 8, {0, 0, 0, 0}, content));
        // 다른 pass를 쌓은 뒤의 게시는 거부한다(packet이 pass마다 연속이어야 한다).
        CHECK(first.Value().Post(ScenePacket{1}, &RecordScene).Error() ==
              ErrorCode::InvalidArgument);
    }

    // 거부된 packet은 게임 lane에서 부서졌다.
    CHECK(scenePacketsDestroyed == 1);
    CHECK(buffer.Packets().Size() == 1);
    // 빈 대상·크기 0은 거부한다.
    CHECK(buffer.AddRetainedPass({}, 8, 8, {}, content).Error() == ErrorCode::InvalidArgument);
    CHECK(buffer.AddScenePass(target, 0, 8, {}).Error() == ErrorCode::InvalidArgument);
    // 닫지 않은 pass와 그 packet을 버린다.
    buffer.DiscardPending();
    CHECK(buffer.Passes().Size() == 0);
    CHECK(buffer.Packets().Size() == 0);
    CHECK(scenePacketsDestroyed == 2);
    renderer.ReleaseTexture(target);
}

TEST(UiCore_ScenePacketMovesAndDestroysOnce) {
    SystemAllocator allocator;
    scenePacketsDestroyed = 0;
    scenePacketsRecorded = 0;
    auto packet = UiScenePacket::TryMake(allocator, ScenePacket{7}, &RecordScene);
    REQUIRE(packet);
    CHECK(packet.Value().Size() == sizeof(ScenePacket));
    CHECK(packet.Value().Alignment() == alignof(ScenePacket));
    alignas(ScenePacket) uint8 storage[sizeof(ScenePacket)];
    UiSceneRecord record = packet.Value().MoveTo(storage);
    // 옮긴 원본은 비고, 그 자리의 이동 원본은 살아 있지 않은 값이라 세지 않는다.
    CHECK(packet.Value().IsEmpty());
    CHECK(scenePacketsDestroyed == 0);
    record.Record(*reinterpret_cast<UiScenePassContext*>(storage));
    CHECK(scenePacketsRecorded == 7);
    record.Destroy();
    record.Destroy();
    CHECK(scenePacketsDestroyed == 1);
    // 기록 함수가 없으면 InvalidArgument다.
    CHECK(
        UiScenePacket::TryMake(allocator, ScenePacket{1}, UiScenePassFunction<ScenePacket>{nullptr})
            .Error() == ErrorCode::InvalidArgument);
}

TEST(UiCore_NullRendererValidatesIdsAndBuffers) {
    SystemAllocator allocator;
    NullUiRenderer renderer(allocator);
    auto buffer = renderer.AcquireDrawBuffer();
    REQUIRE(buffer);
    // 그리기 버퍼는 하나다(18.3절): 반환 전 다시 얻으면 Busy다.
    CHECK(renderer.AcquireDrawBuffer().Error() == ErrorCode::Busy);
    // 픽셀 크기가 틀리면 거부한다.
    const uint8 pixels[3]{};
    CHECK(renderer.CreateTexture({.width = 1, .height = 1}, pixels).Error() ==
          ErrorCode::InvalidArgument);
    const uint8 r8[4]{};
    auto mask = renderer.CreateTexture({.width = 2,
                                        .height = 2,
                                        .format = UiTextureFormat::R8,
                                        .usage = UiTextureUsage::Dynamic,
                                        .sampling = UiTextureSampling::Mask},
                                       r8);
    REQUIRE(mask);
    CHECK(renderer.UpdateTexture(mask.Value(), r8));
    // 렌더 타깃이 아닌 텍스처는 크기를 바꾸거나 읽지 못한다.
    CHECK(renderer.ResizeRenderTarget(mask.Value(), 4, 4).Error() == ErrorCode::InvalidArgument);
    CHECK(renderer.RequestCapture(mask.Value()).Error() == ErrorCode::InvalidArgument);
    // 모르는 창 표면은 NotFound다.
    DrawBatch batch(allocator);
    auto window = renderer.CreateWindowSurface({.width = 4, .height = 4});
    REQUIRE(window);
    REQUIRE(renderer.DestroyWindowSurface(window.Value()));
    REQUIRE(buffer.Value()->AddWindow(window.Value(), 4, 4, {0, 0, 0, 1}, batch));
    CHECK(renderer.DrawWindows(*buffer.Value()).Error() == ErrorCode::NotFound);
    renderer.ReleaseDrawBuffer(*buffer.Value());
    CHECK(renderer.AcquireDrawBuffer());
    // 캡처는 렌더 타깃 크기의 RGBA8이다.
    auto target = renderer.CreateRenderTarget(3, 2, UiTextureSampling::Image);
    REQUIRE(target);
    auto capture = renderer.RequestCapture(target.Value());
    REQUIRE(capture);
    REQUIRE(renderer.WaitCapture(capture.Value()));
    Array<uint8> rgba(allocator);
    REQUIRE(renderer.TakeCapture(capture.Value(), rgba));
    CHECK(rgba.Size() == 3 * 2 * 4);
    CHECK(renderer.TakeCapture(capture.Value(), rgba).Error() == ErrorCode::NotFound);
    renderer.ReleaseTexture(target.Value());
    renderer.ReleaseTexture(mask.Value());
    CHECK(renderer.LiveTextures() == 0);
}

TEST(UiCore_SurfaceRecorderKeepsNestedRetainedPasses) {
    SystemAllocator allocator;
    NullUiRenderer renderer(allocator);
    auto root = SharedPtr<Element>::TryMake(allocator, allocator).Value();
    auto outer = SharedPtr<FakeRetained>::TryMake(allocator, allocator).Value();
    auto inner = SharedPtr<FakeRetained>::TryMake(allocator, allocator).Value();
    root->SetGeometry(Size(100, 100));
    outer->SetGeometry(Size(80, 80));
    inner->SetGeometry(Size(40, 40));
    REQUIRE(outer->TryAddChild(inner));
    REQUIRE(root->TryAddChild(outer));
    SurfaceRecorder recorder(allocator, renderer);
    UiDrawBuffer buffer(allocator);
    auto first = recorder.Record(root, 1, 100, 100, buffer);
    REQUIRE(first);
    CHECK(first.Value() && buffer.Passes().Size() == 2);
    buffer.Clear();
    // 안쪽 보관 표면만 다시 캡처하면 대상은 바뀌지 않지만(false) pass는 쌓인다. 호출자는 그 pass를
    // 버리지 말고 대상과 함께 그린다(SurfaceRecorder 사용 예 — 버리면 안쪽 타깃이 비게 된다).
    inner->capture = true;
    auto second = recorder.Record(root, 2, 100, 100, buffer);
    REQUIRE(second);
    CHECK(!second.Value());
    REQUIRE(buffer.PendingPassCount() == 1);
    CHECK(buffer.Passes()[0].target == inner->managed);
}

TEST(UiCore_NullRendererSkipsUnknownPassTargets) {
    SystemAllocator allocator;
    NullUiRenderer renderer(allocator);
    auto target = renderer.CreateRenderTarget(8, 8, UiTextureSampling::Image);
    auto released = renderer.CreateRenderTarget(4, 4, UiTextureSampling::Premultiplied);
    REQUIRE(target && released);
    renderer.ReleaseTexture(released.Value());
    DrawBatch batch(allocator);
    UiDrawBuffer buffer(allocator);
    // 해제된 표면 pass 대상은 그 pass만 건너뛴다(실제 렌더러가 import하지 못한 타깃과 같다).
    REQUIRE(buffer.AddRetainedPass(released.Value(), 4, 4, {}, batch));
    REQUIRE(buffer.AddOffscreen(target.Value(), 8, 8, {0, 0, 0, 1}, batch));
    REQUIRE(renderer.DrawWindows(buffer));
    CHECK(renderer.Statistics().passesSkipped == 1);
    // 오프스크린 대상 크기가 렌더 타깃과 다르면 InvalidArgument다(Lane 렌더러와 같은 계약).
    buffer.Clear();
    REQUIRE(buffer.AddOffscreen(target.Value(), 4, 8, {0, 0, 0, 1}, batch));
    CHECK(renderer.DrawWindows(buffer).Error() == ErrorCode::InvalidArgument);
    // 렌더 타깃은 RGBA8 sRGB만 된다.
    CHECK(renderer
              .CreateTexture({.width = 2,
                              .height = 2,
                              .format = UiTextureFormat::R8,
                              .usage = UiTextureUsage::RenderTarget},
                             {})
              .Error() == ErrorCode::InvalidArgument);
    renderer.ReleaseTexture(target.Value());
}
