#include "../../RenderBase/Tests/RecordingDevice.h"

#include <Core/Memory/FrameArena.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/PaintList.h>
#include <UiCore/Rendering/UiSceneExtract.h>
#include <UiRender/LaneUiRenderer.h>
#include <UiRender/UiLaneRenderer.h>

// 게임 lane 쪽 UI 렌더러 시험이다(Docs/Design/UiToolkit 18.4·18.7절 — lane 분리). draw buffer를
// 프레임 arena로 복사한 packet이 원본과 독립인지, 텍스처 id가 렌더 handle·sampling으로 바뀌는지,
// 장면 packet이 한 번만 옮겨지고 부서지는지, 텍스처 생성·크기 변경·해제가 lane 작업으로 가는지
// 단언한다. 장치 기록(그래프 pass)은 UiDemo self-test가 실제 장치로 본다.

namespace rocks::ui {

namespace {

using render::testing::RecordingDevice;

// 장면 packet의 소멸 횟수다(시험 안에서만 센다).
int laneScenesDestroyed = 0;

struct LaneScene {
    uint32 value = 0;
    bool live = true;

    explicit LaneScene(uint32 v) noexcept : value(v) {
    }

    LaneScene(LaneScene&& other) noexcept : value(other.value), live(other.live) {
        other.live = false;
    }

    LaneScene& operator=(LaneScene&&) = delete;

    ~LaneScene() noexcept {
        if (live) {
            ++laneScenesDestroyed;
        }
    }
};

void RecordLaneScene(UiScenePassContext&, LaneScene&) noexcept {
}

struct LaneBench {
    SystemAllocator allocator;
    RecordingDevice device{allocator};
    render::RenderLane lane{allocator, device};
    UiLaneRenderer laneRenderer{allocator};

    bool Start() noexcept {
        render::RenderLaneConfig config;
        config.mode = render::RenderLaneMode::Inline;
        config.hangTimeoutMs = 0;
        return lane.Start(config).IsOk();
    }

    bool Settle() noexcept {
        auto marker = lane.PostMarker();
        return marker && lane.WaitMarker(marker.Value());
    }
};

Result<void> BuildBoxes(DrawBatch& batch, Allocator& allocator, UiTextureId texture,
                        uint32 count) noexcept {
    PaintList list(allocator);
    for (uint32 index = 0; index < count; ++index) {
        Brush brush;
        brush.mode = BrushMode::Image;
        brush.resource = texture;
        const auto geometry =
            Geometry::Root({4, 4}, {1, 0, 0, 1, static_cast<float>(index) * 8.0f, 0});
        if (auto added = list.TryAddBrush(geometry, brush, {1, 1, 1, 1}, 0); !added) {
            return added;
        }
    }

    return batch.Build(list);
}

} // namespace

} // namespace rocks::ui

ROCKS_LANE_TRANSFERABLE(rocks::ui::LaneScene);

namespace rocks::ui {

TEST(UiRender_LaneRendererCopiesDrawBufferIndependently) {
    LaneBench bench;
    REQUIRE(bench.Start());
    {
        LaneUiRenderer renderer(bench.allocator, bench.lane, bench.laneRenderer);
        const uint8 mask[4 * 4]{};
        auto texture = renderer.CreateTexture({.width = 4,
                                               .height = 4,
                                               .format = UiTextureFormat::R8,
                                               .usage = UiTextureUsage::Dynamic,
                                               .sampling = UiTextureSampling::Mask},
                                              mask);
        auto target = renderer.CreateRenderTarget(32, 16, UiTextureSampling::Image);
        REQUIRE(texture && target);
        REQUIRE(bench.Settle());
        DrawBatch batch(bench.allocator);
        REQUIRE(BuildBoxes(batch, bench.allocator, texture.Value(), 2));
        UiDrawBuffer buffer(bench.allocator);
        REQUIRE(buffer.AddOffscreen(target.Value(), 32, 16, {0, 0, 0, 1}, batch));
        FrameArena arena(bench.allocator);
        auto packet = renderer.RecordWindows(arena, buffer);
        REQUIRE(packet);
        REQUIRE(packet.Value().Targets().Size() == 1);
        const UiFrameTarget& copy = packet.Value().Targets()[0];
        CHECK(copy.renderTarget == renderer.HandleOf(target.Value()));
        CHECK(copy.window.IsNull() && copy.width == 32 && copy.height == 16);
        REQUIRE(copy.batch.commands.Size() == batch.Commands().Size());
        REQUIRE(copy.batch.vertices.Size() == batch.Vertices().Size());
        // 명령의 텍스처 id는 렌더 handle과 sampling으로 바뀐다.
        CHECK(copy.batch.textures[0].handle == renderer.HandleOf(texture.Value()));
        CHECK(copy.batch.textures[0].sampling == UiTextureSampling::Mask);
        // 복사본은 프레임 arena에 있다: 원본 batch를 다시 만들어도 바뀌지 않는다(18.7절 lane 분리).
        const usize vertices = copy.batch.vertices.Size();
        const Vec2 first = copy.batch.vertices[0].position;
        REQUIRE(BuildBoxes(batch, bench.allocator, texture.Value(), 5));
        CHECK(batch.Vertices().Size() != vertices);
        CHECK(copy.batch.vertices.Size() == vertices);
        CHECK(copy.batch.vertices[0].position == first);
        CHECK(copy.batch.vertices.Data() != batch.Vertices().Data());
        renderer.ReleaseTexture(texture.Value());
        renderer.ReleaseTexture(target.Value());
        CHECK(renderer.LiveTextures() == 0);
    }

    REQUIRE(bench.lane.Shutdown());
    CHECK(bench.lane.Handles().Count() == 0);
}

TEST(UiRender_LaneRendererMovesScenePacketsOnce) {
    LaneBench bench;
    REQUIRE(bench.Start());
    laneScenesDestroyed = 0;
    {
        LaneUiRenderer renderer(bench.allocator, bench.lane, bench.laneRenderer);
        auto scene = renderer.CreateRenderTarget(8, 8, UiTextureSampling::Image);
        auto target = renderer.CreateRenderTarget(8, 8, UiTextureSampling::Image);
        REQUIRE(scene && target);
        DrawBatch batch(bench.allocator);
        UiDrawBuffer buffer(bench.allocator);
        auto extract = buffer.AddScenePass(scene.Value(), 8, 8, {0, 0, 1, 1});
        REQUIRE(extract);
        REQUIRE(extract.Value().Post(LaneScene{3}, &RecordLaneScene));
        REQUIRE(extract.Value().Post(LaneScene{4}, &RecordLaneScene));
        REQUIRE(buffer.AddOffscreen(target.Value(), 8, 8, {0, 0, 0, 1}, batch));
        FrameArena arena(bench.allocator);
        {
            auto packet = renderer.RecordWindows(arena, buffer);
            REQUIRE(packet);
            const UiFramePass& pass = packet.Value().Targets()[0].passes[0];
            CHECK(pass.kind == UiSurfacePassKind::Scene);
            CHECK(pass.target == renderer.HandleOf(scene.Value()));
            REQUIRE(pass.scenes.Size() == 2);
            CHECK(static_cast<const LaneScene*>(pass.scenes[1].object)->value == 4);
            // packet을 옮긴 원본은 비어 아무것도 부수지 않는다.
            UiFramePacket moved = Move(packet.Value());
            CHECK(packet.Value().Targets().IsEmpty());
            // 버퍼의 packet은 arena로 옮겨져 비었다(이동 원본은 세지 않는다).
            CHECK(buffer.Packets()[0].IsEmpty() && buffer.Packets()[1].IsEmpty());
            CHECK(laneScenesDestroyed == 0);
            // 이동 대입은 받는 쪽의 장면을 먼저 부순다: 장면 하나를 가진 다른 packet에 옮겨 담으면
            // 그 장면(5)이 한 번 부서지고, 옮긴 원본은 빈다.
            UiDrawBuffer second(bench.allocator);
            auto secondExtract = second.AddScenePass(scene.Value(), 8, 8, {0, 0, 1, 1});
            REQUIRE(secondExtract);
            REQUIRE(secondExtract.Value().Post(LaneScene{5}, &RecordLaneScene));
            REQUIRE(second.AddOffscreen(target.Value(), 8, 8, {0, 0, 0, 1}, batch));
            auto other = renderer.RecordWindows(arena, second);
            REQUIRE(other);
            other.Value() = Move(moved);
            CHECK(moved.Targets().IsEmpty() && laneScenesDestroyed == 1);
        }

        // packet 소멸자가 남은 장면(3·4)을 한 번씩 부순다 — RenderLane이 프레임 값을 slot 반환 전에
        // 부수므로 arena가 아직 유효하다(RenderBase 5.3절). arena Reset은 저장소만 회수한다.
        CHECK(laneScenesDestroyed == 3);
        (void)arena.Reset();
        CHECK(laneScenesDestroyed == 3);
        renderer.ReleaseTexture(scene.Value());
        renderer.ReleaseTexture(target.Value());
    }

    REQUIRE(bench.lane.Shutdown());
}

TEST(UiRender_LaneRendererValidatesTargetsAndTextures) {
    LaneBench bench;
    REQUIRE(bench.Start());
    {
        LaneUiRenderer renderer(bench.allocator, bench.lane, bench.laneRenderer);
        // 렌더 타깃은 초기 픽셀을 받지 않고 RGBA8 sRGB만 된다.
        const uint8 pixels[4]{};
        CHECK(renderer
                  .CreateTexture({.width = 1, .height = 1, .usage = UiTextureUsage::RenderTarget},
                                 pixels)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(renderer
                  .CreateTexture({.width = 1,
                                  .height = 1,
                                  .format = UiTextureFormat::R8,
                                  .usage = UiTextureUsage::RenderTarget},
                                 {})
                  .Error() == ErrorCode::InvalidArgument);
        // 크기를 바꾼 렌더 타깃은 id가 그대로이고 새 렌더 handle을 쓴다(옛 handle은 Retire).
        auto target = renderer.CreateRenderTarget(4, 4, UiTextureSampling::Premultiplied);
        REQUIRE(target);
        const render::RenderHandle before = renderer.HandleOf(target.Value());
        REQUIRE(renderer.ResizeRenderTarget(target.Value(), 8, 4));
        const render::RenderHandle after = renderer.HandleOf(target.Value());
        CHECK(!after.IsNull() && after != before);
        CHECK(renderer.DescribeTexture(target.Value()).Value().width == 8);
        REQUIRE(bench.Settle());
        CHECK(!bench.lane.Handles().Texture(before));
        CHECK(bench.lane.Handles().Texture(after));
        // 모르는 오프스크린 대상은 NotFound다.
        DrawBatch batch(bench.allocator);
        UiDrawBuffer buffer(bench.allocator);
        REQUIRE(buffer.AddOffscreen(SlotHandle{40, 1}, 4, 4, {0, 0, 0, 1}, batch));
        FrameArena arena(bench.allocator);
        CHECK(renderer.RecordWindows(arena, buffer).Error() == ErrorCode::NotFound);
        // 갱신 텍스처가 아니면 UpdateTexture를 거부한다.
        CHECK(renderer.UpdateTexture(target.Value(), pixels).Error() == ErrorCode::InvalidArgument);
        renderer.ReleaseTexture(target.Value());
    }

    REQUIRE(bench.lane.Shutdown());
    CHECK(bench.lane.Handles().Count() == 0);
}

TEST(UiRender_LaneRendererRejectsMismatchedAndMissingResources) {
    LaneBench bench;
    REQUIRE(bench.Start());
    {
        LaneUiRenderer renderer(bench.allocator, bench.lane, bench.laneRenderer);
        auto target = renderer.CreateRenderTarget(8, 4, UiTextureSampling::Image);
        const uint8 pixels[4 * 4 * 4]{};
        auto image = renderer.CreateTexture({.width = 4, .height = 4}, pixels);
        REQUIRE(target && image);
        DrawBatch batch(bench.allocator);
        REQUIRE(BuildBoxes(batch, bench.allocator, image.Value(), 1));
        FrameArena arena(bench.allocator);
        {
            // 오프스크린 대상 크기가 렌더 타깃과 다르면 InvalidArgument다(viewport·화면 읽기 크기).
            UiDrawBuffer buffer(bench.allocator);
            REQUIRE(buffer.AddOffscreen(target.Value(), 4, 4, {0, 0, 0, 1}, batch));
            CHECK(renderer.RecordWindows(arena, buffer).Error() == ErrorCode::InvalidArgument);
        }

        // 해제된 텍스처 id를 읽는 명령은 빈 handle이 된다. 명령의 resource는 남으므로 lane은 그
        // 명령을 단색으로 그리지 않고 건너뛴다(18.4절).
        renderer.ReleaseTexture(image.Value());
        UiDrawBuffer buffer(bench.allocator);
        REQUIRE(buffer.AddOffscreen(target.Value(), 8, 4, {0, 0, 0, 1}, batch));
        auto packet = renderer.RecordWindows(arena, buffer);
        REQUIRE(packet);
        const UiFrameBatch& copy = packet.Value().Targets()[0].batch;
        REQUIRE(copy.commands.Size() == 1);
        CHECK(!copy.commands[0].resource.IsNull());
        CHECK(copy.textures[0].handle.IsNull());
        // 화면 읽기: 요청하지 않은 번호는 NotFound다.
        Array<uint8> rgba(bench.allocator);
        CHECK(renderer.TakeCapture(12345, rgba).Error() == ErrorCode::NotFound);
        renderer.ReleaseTexture(target.Value());
    }

    REQUIRE(bench.lane.Shutdown());
    CHECK(bench.lane.Handles().Count() == 0);
}

} // namespace rocks::ui
