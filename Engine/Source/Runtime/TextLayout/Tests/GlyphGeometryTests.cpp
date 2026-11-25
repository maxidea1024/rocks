#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <TextLayout/GlyphGeometry.h>

using namespace rocks;
namespace glyph_geometry_tests_file {

// 전처리 geometry 직렬화 시험이다(Docs/Design/Font 6.5·6.8절 FT2e). 형식은 TextLayout이 소유하므로
// 왕복·버전·손상 입력을 정확히 단언한다.

namespace {

// 윤곽 둘(사각형 선분 넷 + 2차 곡선 하나와 3차 곡선 하나)이다.
void MakeSample(text::GlyphGeometry& geometry) noexcept {
    geometry.unitsPerEm = 2048;
    const text::GeometryPoint square[] = {{0, 0},     {100, 0}, {100, 0}, {100, 100},
                                          {100, 100}, {0, 100}, {0, 100}, {0, 0}};
    for (const text::GeometryPoint& point : square) {
        CHECK(static_cast<bool>(geometry.points.TryPushBack(point)));
    }

    for (int index = 0; index < 4; ++index) {
        CHECK(static_cast<bool>(geometry.edgeKinds.TryPushBack(text::GeometryEdgeKind::Line)));
    }

    CHECK(static_cast<bool>(geometry.contourEdgeEnds.TryPushBack(4)));
    const text::GeometryPoint curves[] = {{-5, 7},   {10, 20}, {30, -40}, {30, -40},
                                          {50, -60}, {70, 80}, {-5, 7}};
    for (const text::GeometryPoint& point : curves) {
        CHECK(static_cast<bool>(geometry.points.TryPushBack(point)));
    }

    CHECK(static_cast<bool>(geometry.edgeKinds.TryPushBack(text::GeometryEdgeKind::Quadratic)));
    CHECK(static_cast<bool>(geometry.edgeKinds.TryPushBack(text::GeometryEdgeKind::Cubic)));
    CHECK(static_cast<bool>(geometry.contourEdgeEnds.TryPushBack(6)));
}

bool Same(const text::GlyphGeometry& left, const text::GlyphGeometry& right) noexcept {
    if (left.unitsPerEm != right.unitsPerEm ||
        left.contourEdgeEnds.Size() != right.contourEdgeEnds.Size() ||
        left.edgeKinds.Size() != right.edgeKinds.Size() ||
        left.points.Size() != right.points.Size()) {
        return false;
    }

    for (usize index = 0; index < left.contourEdgeEnds.Size(); ++index) {
        if (left.contourEdgeEnds[index] != right.contourEdgeEnds[index]) {
            return false;
        }
    }

    for (usize index = 0; index < left.edgeKinds.Size(); ++index) {
        if (left.edgeKinds[index] != right.edgeKinds[index]) {
            return false;
        }
    }

    for (usize index = 0; index < left.points.Size(); ++index) {
        if (!(left.points[index] == right.points[index])) {
            return false;
        }
    }

    return true;
}

} // namespace

TEST(TextLayout_GlyphGeometryRoundTrip) {
    SystemAllocator allocator;
    text::GlyphGeometry geometry(allocator);
    MakeSample(geometry);
    Array<uint8> bytes(allocator);
    CHECK(static_cast<bool>(text::WriteGlyphGeometry(geometry, bytes)));
    // 머리 18 byte + 윤곽 끝 2×4 + edge 종류 6 + 점 15×8.
    CHECK(bytes.Size() == 18 + 8 + 6 + 120);
    CHECK(bytes[0] == text::GlyphGeometryFormatVersion && bytes[1] == 0);

    text::GlyphGeometry read(allocator);
    CHECK(static_cast<bool>(text::ReadGlyphGeometry(bytes.View(), read)));
    CHECK(Same(geometry, read));

    // 빈 geometry도 왕복한다.
    text::GlyphGeometry empty(allocator);
    Array<uint8> emptyBytes(allocator);
    CHECK(static_cast<bool>(text::WriteGlyphGeometry(empty, emptyBytes)));
    CHECK(static_cast<bool>(text::ReadGlyphGeometry(emptyBytes.View(), read)) && read.Empty());
}

TEST(TextLayout_GlyphGeometryRejectsBadInput) {
    SystemAllocator allocator;
    text::GlyphGeometry geometry(allocator);
    MakeSample(geometry);
    Array<uint8> bytes(allocator);
    CHECK(static_cast<bool>(text::WriteGlyphGeometry(geometry, bytes)));
    text::GlyphGeometry read(allocator);

    // 다른 버전은 Unsupported(다시 만든다)다.
    Array<uint8> versioned(allocator);
    CHECK(static_cast<bool>(versioned.TryAppend(bytes.View())));
    versioned[0] = static_cast<uint8>(text::GlyphGeometryFormatVersion + 1);
    CHECK(text::ReadGlyphGeometry(versioned.View(), read).Error() == ErrorCode::Unsupported);

    // 잘린 입력과 꼬리 byte는 InvalidArgument다.
    for (usize size = 0; size < bytes.Size(); ++size) {
        CHECK(text::ReadGlyphGeometry(Span<const uint8>(bytes.Data(), size), read).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(read.Empty());
    }

    Array<uint8> trailing(allocator);
    CHECK(static_cast<bool>(trailing.TryAppend(bytes.View())) &&
          static_cast<bool>(trailing.TryPushBack(0)));
    CHECK(text::ReadGlyphGeometry(trailing.View(), read).Error() == ErrorCode::InvalidArgument);

    // edge 종류 0·4, 줄지 않는 윤곽 끝, 점 수 모순이다. 머리 뒤 offset: 윤곽 끝 18, 종류 26.
    const auto corrupt = [&](usize offset, uint8 value) noexcept {
        Array<uint8> copy(allocator);
        (void)copy.TryAppend(bytes.View());
        copy[offset] = value;
        return text::ReadGlyphGeometry(copy.View(), read).Error() == ErrorCode::InvalidArgument;
    };
    CHECK(corrupt(26, 0));
    CHECK(corrupt(26, 4));
    // 첫 윤곽 끝을 6으로 두면 둘째(6)가 늘지 않는다.
    CHECK(corrupt(18, 6));
    // 둘째 edge를 3차로 바꾸면 필요한 점이 하나 늘어 점 수와 맞지 않는다.
    CHECK(corrupt(27, 3));
}

} // namespace glyph_geometry_tests_file
