#include <UiCore/Geometry.h>
#include <TestSupport/Test.h>

using namespace rocks::ui;

TEST(UiCore_GeometryAppliesRenderTransformForHitTest) {
    Geometry geometry{{20, 10}, {1, 0, 0, 1, 100, 50}, {0, -1, 1, 0, -50, 100}};
    CHECK(geometry.Contains({-55, 110}));
    CHECK(!geometry.Contains({105, 55}));
    CHECK(!geometry.Contains({-60, 120}));
}

TEST(UiCore_ChildGeometryAccumulatesParentAndLocalTransforms) {
    const Geometry root = Geometry::Root({100, 100}, {1, 0, 0, 1, 100, 50});
    const Geometry child =
        root.MakeChild({20, 10}, {1, 0, 0, 1, 10, 5}, {0, -1, 1, 0, 0, 0}, {0.5f, 0.5f});
    CHECK(child.layout.Apply({0, 0}) == rocks::Vec2(110, 55));
    CHECK(child.Contains({120, 60}));
    CHECK(!child.Contains({110, 55}));
}

TEST(UiCore_SingularTransformHasNoHitAndPreservesInverseOutput) {
    AffineTransform2D singular{0, 0, 0, 1, 0, 0};
    AffineTransform2D inverse;
    inverse.tx = 42;
    CHECK(!singular.TryInverse(inverse));
    CHECK(inverse.tx == 42);
    Geometry geometry{{10, 10}, {}, singular};
    CHECK(!geometry.Contains({0, 1}));
}

TEST(UiCore_SmallInvertibleScaleRemainsHittable) {
    const AffineTransform2D scale{0.00001f, 0, 0, 0.00001f, 0, 0};
    const Geometry geometry = Geometry::Root({100, 100}, scale);
    CHECK(geometry.Contains({0.0005f, 0.0005f}));
}
