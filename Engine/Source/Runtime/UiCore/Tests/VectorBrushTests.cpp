#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/PaintList.h>
#include <UiCore/Styles/StyleBrush.h>

// 벡터 brush 기록 시험이다(Docs/Design/UiToolkit 17.2절). PaintList가 그릴 화소 크기를 정해
// resolver에 묻고, 받은 자리(resource·uv)로 기록하는지 단언한다.

namespace rocks::ui {

namespace {

// 받은 요청을 적고 정한 자리를 돌려주는 resolver다.
class ProbeResolver final : public VectorImageResolver {
public:
    Result<ResolvedVectorImage> Resolve(VectorImageKey key, uint32 width,
                                        uint32 height) noexcept override {
        ++calls;
        lastWidth = width;
        lastHeight = height;
        if (key != known) {
            return Result<ResolvedVectorImage>::Failure(ErrorCode::NotFound);
        }

        return Result<ResolvedVectorImage>::Success({SlotHandle{7, 1}, {{0.25f, 0.5f}, {0.5f, 1}}});
    }

    uint64 Revision() const noexcept override {
        return 0;
    }

    VectorImageKey known = VectorImageKey::FromPath("Common/Check");
    uint32 calls = 0;
    uint32 lastWidth = 0;
    uint32 lastHeight = 0;
};

} // namespace

TEST(UiCore_VectorBrushResolvesAtDrawnPixelSize) {
    SystemAllocator allocator;
    ProbeResolver resolver;
    PaintList list(allocator);
    const Brush brush = Brush::Vector(resolver.known, {16, 16}, {1, 0, 0, 1});
    // resolver가 없으면 아무것도 그리지 않고 성공한다(창·GPU 없는 시험).
    REQUIRE(list.TryAddBrush(Geometry::Root({16, 16}, {}), brush, {1, 1, 1, 1}, 0));
    CHECK(list.Boxes().IsEmpty() && resolver.calls == 0);
    list.SetVectorImageResolver(&resolver);
    REQUIRE(list.TryAddBrush(Geometry::Root({16, 16}, {}), brush, {1, 1, 1, 1}, 0));
    REQUIRE(list.Boxes().Size() == 1);
    CHECK(resolver.lastWidth == 16 && resolver.lastHeight == 16);
    const SlotHandle expected{7, 1};
    CHECK(list.Boxes()[0].resource == expected);
    CHECK(list.Boxes()[0].uvStart == Vec2(0.25f, 0.5f) && list.Boxes()[0].uvEnd == Vec2(0.5f, 1));
    CHECK(list.Boxes()[0].color == LinearColor(1, 0, 0, 1));
    // 배율 2(DPI 등)면 화소 크기도 두 배이고, 소수 크기는 올린다.
    const AffineTransform2D doubled{2, 0, 0, 2, 0, 0};
    const Geometry scaled = Geometry::Root({16, 10.5f}, doubled);
    list.Clear();
    REQUIRE(list.TryAddBrush(scaled, brush, LinearColor(1, 1, 1, 1), 0));
    CHECK(resolver.lastWidth == 32 && resolver.lastHeight == 21);
    // 90도 회전은 열 길이로 본다(가로·세로 배율이 바뀌지 않는다).
    const AffineTransform2D rotated{0, 1, -1, 0, 0, 0};
    const Geometry turned = Geometry::Root({20, 10}, rotated);
    list.Clear();
    REQUIRE(list.TryAddBrush(turned, brush, LinearColor(1, 1, 1, 1), 0));
    CHECK(resolver.lastWidth == 20 && resolver.lastHeight == 10);
    // 회전에 비균등 배율을 섞으면 두 열 길이가 다르다: 로컬 x축은 (m00, m10) = (0, -1)로 1배, 로컬
    // y축은 (m01, m11) = (3, 0)으로 3배다.
    const AffineTransform2D mixed{0, 3, -1, 0, 0, 0};
    const Geometry skewed = Geometry::Root({10, 10}, mixed);
    list.Clear();
    REQUIRE(list.TryAddBrush(skewed, brush, LinearColor(1, 1, 1, 1), 0));
    CHECK(resolver.lastWidth == 10 && resolver.lastHeight == 30);
    // 배율 오차(1.0000001)로 1 px 큰 래스터가 생기지 않는다(반올림 — UE는 버린다).
    const AffineTransform2D nearlyOne{1.0000001f, 0, 0, 1.0000001f, 0, 0};
    list.Clear();
    REQUIRE(
        list.TryAddBrush(Geometry::Root({16, 16}, nearlyOne), brush, LinearColor(1, 1, 1, 1), 0));
    CHECK(resolver.lastWidth == 16 && resolver.lastHeight == 16);
    // brush uv는 받은 자리 안의 부분 범위다(왼쪽 절반 → 자리 uv 0.25..0.375).
    Brush half = brush;
    half.uv = {{0, 0}, {0.5f, 1}};
    list.Clear();
    REQUIRE(list.TryAddBrush(Geometry::Root({16, 16}, {}), half, LinearColor(1, 1, 1, 1), 0));
    CHECK(list.Boxes()[0].uvStart.x == 0.25f && list.Boxes()[0].uvEnd.x == 0.375f);
    // 거울 뒤집기는 받은 uv를 뒤집는다.
    Brush mirrored = brush;
    mirrored.mirrorX = true;
    list.Clear();
    REQUIRE(list.TryAddBrush(Geometry::Root({16, 16}, {}), mirrored, {1, 1, 1, 1}, 0));
    CHECK(list.Boxes()[0].uvStart.x == 0.5f && list.Boxes()[0].uvEnd.x == 0.25f);
    // 모르는 키는 그리기를 실패시킨다(오타를 시험이 바로 잡는다).
    const Brush missing = Brush::Vector(VectorImageKey::FromPath("Common/Missing"), {16, 16});
    CHECK(list.TryAddBrush(Geometry::Root({16, 16}, {}), missing, {1, 1, 1, 1}, 0).Error() ==
          ErrorCode::NotFound);
    // 벡터 9분할은 아직 없어 Image 모드가 아닌 벡터 brush는 유효하지 않다.
    Brush boxed = brush;
    boxed.mode = BrushMode::Box;
    CHECK(!boxed.IsValid());
}

TEST(UiCore_StyleSvgBrushResolvesThemeTint) {
    const VectorImageKey key = VectorImageKey::FromPath("Common/Check");
    const StyleBrush style = StyleBrush::Svg(key, {16, 16}, ThemeSlot::Primary);
    const Brush brush = style.Resolve(DefaultThemeColors(), {});
    CHECK(brush.mode == BrushMode::Image && brush.vectorImage == key);
    CHECK(brush.imageSize == Vec2(16, 16));
    CHECK(brush.tint == DefaultThemeColors().Get(ThemeSlot::Primary));
    // 전경색 tint는 그리는 문맥의 전경색이다.
    const StyleBrush foreground = StyleBrush::SvgForeground(key, {16, 16});
    StyleColorContext context;
    context.foreground = {0.5f, 0.25f, 1, 1};
    CHECK(foreground.Resolve(DefaultThemeColors(), context).tint == context.foreground);
    // 키는 경로 해시이고 constexpr이다.
    static_assert(!VectorImageKey::FromPath("Common/Check").IsNull());
    CHECK(VectorImageKey::FromPath("Common/Check") != VectorImageKey::FromPath("Common/check"));
}

} // namespace rocks::ui
