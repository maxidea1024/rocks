#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAssets/ColorCurveEvaluate.h>
#include <CurveAssets/CurveAssetsModule.h>
#include <CurveAssets/FloatCurveAsset.generated.h>
#include <CurveAssets/VectorCurveAsset.generated.h>
#include <CurveAssetsTestFixtures/CurveAssetsTestFixtures.module.generated.h>
#include <CurveAssetsTestFixtures/CurveSourceHolder.generated.h>
#include <CurveAssetsTestFixtures/CurveSourceHolder.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 커브 원천 시험이다(Docs/Design/CurveAssets 3.1절 — CA1): 인라인·참조 평가, 색 참조는 조정 적용·
// 인라인은 원값, 평탄화(사본), 풀리지 않는 참조의 오류와 out 불변, 할당 실패.
using namespace rocks;
using rocks::curve_fixture::CurveSourceHolder;

namespace curve_source_tests_file {

namespace {

constexpr uint64 TestIdHigh = 0x43413142ull;

struct Scene {
    SystemAllocator allocator;
    TypeRegistry types{allocator};
    ModuleRegistration modules{types, allocator};
    bool registered = RegisterCurveAssetsTypes(modules).IsOk() &&
                      generated::CurveAssetsTestFixtures::RegisterModuleTypes(modules).IsOk();
};

template <class T, class Fn>
bool Edit(ObjectRegistry& objects, const ObjectAccessContext& access, ObjectHandle handle,
          Fn&& fn) noexcept {
    auto writable = objects.ResolveForWrite<T>(access, handle);
    if (!writable) {
        return false;
    }

    auto pointer = writable.Value().Get();
    if (!pointer) {
        return false;
    }

    fn(*pointer.Value());
    return true;
}

} // namespace

// 인라인·참조 평가와 평탄화(스칼라·벡터·색), 참조를 지우면 인라인이 돌아온다.
TEST(CurveSource_InlineAndAsset) {
    Scene scene;
    REQUIRE(scene.registered);
    ObjectRegistry objects(scene.allocator, scene.types, {TestIdHigh, 1});
    const auto access = objects.OwnerAccess();
    auto holderMade = objects.NewObject<CurveSourceHolder>(access, TypeIdOf<CurveSourceHolder>());
    auto scalarMade = objects.NewObject<FloatCurveAsset>(access, TypeIdOf<FloatCurveAsset>());
    auto vectorMade = objects.NewObject<VectorCurveAsset>(access, TypeIdOf<VectorCurveAsset>());
    auto colorMade = objects.NewObject<ColorCurveAsset>(access, TypeIdOf<ColorCurveAsset>());
    REQUIRE(holderMade && scalarMade && vectorMade && colorMade);
    const ObjectHandle holder = holderMade.Value().Handle();
    const ObjectHandle scalar = scalarMade.Value().Handle();
    const ObjectHandle vector = vectorMade.Value().Handle();
    const ObjectHandle color = colorMade.Value().Handle();
    REQUIRE(Edit<FloatCurveAsset>(objects, access, scalar, [](FloatCurveAsset& value) noexcept {
        (void)value.curve.TryAddKey(0.0f, 100.0f);
    }));
    REQUIRE(Edit<VectorCurveAsset>(objects, access, vector, [](VectorCurveAsset& value) noexcept {
        (void)value.z.TryAddKey(0.0f, 30.0f);
    }));
    REQUIRE(Edit<ColorCurveAsset>(objects, access, color, [](ColorCurveAsset& value) noexcept {
        (void)value.r.TryAddKey(0.0f, 1.0f);
        value.adjustHue = 120.0f;
    }));
    REQUIRE(Edit<CurveSourceHolder>(objects, access, holder, [](CurveSourceHolder& value) noexcept {
        (void)value.floatSource.curve.TryAddKey(0.0f, 1.0f);
        (void)value.vectorSource.curves.x.TryAddKey(0.0f, 2.0f);
        (void)value.colorSource.r.TryAddKey(0.0f, 1.0f);
    }));

    auto read = [&](auto&& fn) noexcept {
        auto borrowed = objects.Resolve<CurveSourceHolder>(access, holder);
        REQUIRE(borrowed);
        auto pointer = borrowed.Value().Get();
        REQUIRE(pointer);
        fn(*pointer.Value());
    };

    // 인라인.
    read([&](const CurveSourceHolder& value) noexcept {
        CHECK(!value.floatSource.HasAsset() && value.floatSource.EvaluateInline(0.0f) == 1.0f);
        auto scalarValue = value.floatSource.TryEvaluate(objects, access, 0.0f);
        auto vectorValue = value.vectorSource.TryEvaluate(objects, access, 0.0f);
        auto colorValue = value.colorSource.TryEvaluate(objects, access, 0.0f);
        REQUIRE(scalarValue && vectorValue && colorValue);
        CHECK(scalarValue.Value() == 1.0f && vectorValue.Value().x == 2.0);
        // 인라인 색은 조정이 없다(알파 키 없음 → 1).
        CHECK(colorValue.Value().r == 1.0f && colorValue.Value().g == 0.0f &&
              colorValue.Value().a == 1.0f);
    });

    // 참조.
    REQUIRE(
        Edit<CurveSourceHolder>(objects, access, holder, [&](CurveSourceHolder& value) noexcept {
            CHECK(objects.SetReference(access, holder, value.floatSource.asset, scalar).IsOk());
            CHECK(objects.SetReference(access, holder, value.vectorSource.asset, vector).IsOk());
            CHECK(objects.SetReference(access, holder, value.colorSource.asset, color).IsOk());
        }));
    read([&](const CurveSourceHolder& value) noexcept {
        CHECK(value.floatSource.HasAsset());
        // 인라인은 남는다.
        CHECK(value.floatSource.curve.KeyCount() == 1);
        auto scalarValue = value.floatSource.TryEvaluate(objects, access, 0.0f);
        auto vectorValue = value.vectorSource.TryEvaluate(objects, access, 0.0f);
        auto colorValue = value.colorSource.TryEvaluate(objects, access, 0.0f);
        REQUIRE(scalarValue && vectorValue && colorValue);
        CHECK(scalarValue.Value() == 100.0f);
        CHECK(vectorValue.Value().x == 0.0 && vectorValue.Value().z == 30.0);
        // 에셋 색은 조정 적용(색상 +120: 빨강 → 초록).
        CHECK(colorValue.Value().r == 0.0f && colorValue.Value().g == 1.0f);

        // 평탄화: 에셋 커브·조정이 사본으로 온다.
        TangentCurve flatScalar(scene.allocator);
        VectorCurveValue flatVector(scene.allocator);
        ColorCurveValue flatColor(scene.allocator);
        REQUIRE(value.floatSource.TryFlattenInto(objects, access, flatScalar));
        REQUIRE(value.vectorSource.TryFlattenInto(objects, access, flatVector));
        REQUIRE(value.colorSource.TryFlattenInto(objects, access, flatColor));
        CHECK(flatScalar.Evaluate(0.0f) == 100.0f && flatVector.Evaluate(0.0f).z == 30.0);
        CHECK(flatColor.adjusted && flatColor.adjustHue == 120.0f &&
              flatColor.Evaluate(0.0f).g == 1.0f);
    });

    // 참조를 지우면 인라인이 돌아온다.
    REQUIRE(
        Edit<CurveSourceHolder>(objects, access, holder, [&](CurveSourceHolder& value) noexcept {
            CHECK(objects.SetReference(access, holder, value.colorSource.asset, ObjectHandle{})
                      .IsOk());
        }));
    read([&](const CurveSourceHolder& value) noexcept {
        ColorCurveValue flatColor(scene.allocator);
        REQUIRE(value.colorSource.TryFlattenInto(objects, access, flatColor));
        CHECK(!flatColor.adjusted && flatColor.Evaluate(0.0f).r == 1.0f);
    });
}

// 풀리지 않는 참조(파괴 요청된 에셋)는 오류이고 인라인으로 대신하지 않는다. 평탄화가 실패하면 out은
// 그대로다. 할당 실패도 out 그대로다.
TEST(CurveSource_UnresolvedAndAllocationFailure) {
    Scene scene;
    REQUIRE(scene.registered);
    ObjectRegistry objects(scene.allocator, scene.types, {TestIdHigh, 2});
    const auto access = objects.OwnerAccess();
    auto holderMade = objects.NewObject<CurveSourceHolder>(access, TypeIdOf<CurveSourceHolder>());
    auto scalarMade = objects.NewObject<FloatCurveAsset>(access, TypeIdOf<FloatCurveAsset>());
    REQUIRE(holderMade && scalarMade);
    const ObjectHandle holder = holderMade.Value().Handle();
    const ObjectHandle scalar = scalarMade.Value().Handle();
    REQUIRE(Edit<FloatCurveAsset>(objects, access, scalar, [](FloatCurveAsset& value) noexcept {
        (void)value.curve.TryAddKey(0.0f, 100.0f);
        (void)value.curve.TryAddKey(1.0f, 200.0f);
    }));
    REQUIRE(
        Edit<CurveSourceHolder>(objects, access, holder, [&](CurveSourceHolder& value) noexcept {
            (void)value.floatSource.curve.TryAddKey(0.0f, 1.0f);
            CHECK(objects.SetReference(access, holder, value.floatSource.asset, scalar).IsOk());
        }));

    test::NFailAllocator failing(scene.allocator);
    {
        auto borrowed = objects.Resolve<CurveSourceHolder>(access, holder);
        REQUIRE(borrowed);
        auto pointer = borrowed.Value().Get();
        REQUIRE(pointer);
        TangentCurve out(failing);
        REQUIRE(out.TryAddKey(9.0f, 9.0f));
        failing.FailOnAttempt(1);
        CHECK(pointer.Value()->floatSource.TryFlattenInto(objects, access, out).Error() ==
              ErrorCode::OutOfMemory);
        CHECK(out.KeyCount() == 1 && out.Keys()[0].time == 9.0f);
        failing.DisableFailure();
    }

    REQUIRE(objects.RequestDestroy(access, scalar));
    auto borrowed = objects.Resolve<CurveSourceHolder>(access, holder);
    REQUIRE(borrowed);
    auto pointer = borrowed.Value().Get();
    REQUIRE(pointer);
    const FloatCurveSource& source = pointer.Value()->floatSource;
    // 원천이 강한 간선으로 쥐므로 회수되지 않고 파괴 요청 상태다 — Resolve는 Destroyed다.
    CHECK(source.TryEvaluate(objects, access, 0.0f).Error() == ErrorCode::Destroyed);
    TangentCurve out(scene.allocator);
    REQUIRE(out.TryAddKey(9.0f, 9.0f));
    CHECK(source.TryFlattenInto(objects, access, out).Error() == ErrorCode::Destroyed);
    CHECK(out.KeyCount() == 1 && out.Keys()[0].time == 9.0f);
    // 인라인은 참조와 무관하게 평가된다.
    CHECK(source.EvaluateInline(0.0f) == 1.0f);
}

// 벡터·색 원천: 에셋 경로의 2·3·4번째 채널 할당 실패, 풀리지 않는 참조 — 모두 out 그대로. 평탄화는
// out 채널마다의 allocator를 지킨다.
TEST(CurveSource_VectorColorFlattenFailures) {
    Scene scene;
    REQUIRE(scene.registered);
    ObjectRegistry objects(scene.allocator, scene.types, {TestIdHigh, 3});
    const auto access = objects.OwnerAccess();
    auto holderMade = objects.NewObject<CurveSourceHolder>(access, TypeIdOf<CurveSourceHolder>());
    auto vectorMade = objects.NewObject<VectorCurveAsset>(access, TypeIdOf<VectorCurveAsset>());
    auto colorMade = objects.NewObject<ColorCurveAsset>(access, TypeIdOf<ColorCurveAsset>());
    REQUIRE(holderMade && vectorMade && colorMade);
    const ObjectHandle holder = holderMade.Value().Handle();
    const ObjectHandle vector = vectorMade.Value().Handle();
    const ObjectHandle color = colorMade.Value().Handle();
    REQUIRE(Edit<VectorCurveAsset>(objects, access, vector, [](VectorCurveAsset& value) noexcept {
        (void)value.x.TryAddKey(0.0f, 1.0f);
        (void)value.y.TryAddKey(0.0f, 2.0f);
        (void)value.z.TryAddKey(0.0f, 3.0f);
    }));
    REQUIRE(Edit<ColorCurveAsset>(objects, access, color, [](ColorCurveAsset& value) noexcept {
        (void)value.r.TryAddKey(0.0f, 1.0f);
        (void)value.g.TryAddKey(0.0f, 0.5f);
        (void)value.b.TryAddKey(0.0f, 0.25f);
        (void)value.a.TryAddKey(0.0f, 0.75f);
    }));
    REQUIRE(
        Edit<CurveSourceHolder>(objects, access, holder, [&](CurveSourceHolder& value) noexcept {
            CHECK(objects.SetReference(access, holder, value.vectorSource.asset, vector).IsOk());
            CHECK(objects.SetReference(access, holder, value.colorSource.asset, color).IsOk());
        }));

    test::NFailAllocator failing(scene.allocator);
    test::NFailAllocator other(scene.allocator);
    {
        auto borrowed = objects.Resolve<CurveSourceHolder>(access, holder);
        REQUIRE(borrowed);
        auto pointer = borrowed.Value().Get();
        REQUIRE(pointer);
        const CurveSourceHolder& value = *pointer.Value();
        for (uint64 attempt = 1; attempt <= 3; ++attempt) {
            VectorCurveValue out(failing);
            REQUIRE(out.x.TryAddKey(9.0f, 9.0f));
            failing.FailOnAttempt(attempt);
            CHECK(value.vectorSource.TryFlattenInto(objects, access, out).Error() ==
                  ErrorCode::OutOfMemory);
            failing.DisableFailure();
            CHECK(out.x.KeyCount() == 1 && out.x.Keys()[0].time == 9.0f && out.y.KeyCount() == 0 &&
                  out.z.KeyCount() == 0);
        }

        for (uint64 attempt = 1; attempt <= 4; ++attempt) {
            ColorCurveValue out(failing);
            REQUIRE(out.r.TryAddKey(9.0f, 9.0f));
            failing.FailOnAttempt(attempt);
            CHECK(value.colorSource.TryFlattenInto(objects, access, out).Error() ==
                  ErrorCode::OutOfMemory);
            failing.DisableFailure();
            CHECK(out.r.KeyCount() == 1 && out.g.KeyCount() == 0 && out.a.KeyCount() == 0 &&
                  !out.adjusted);
        }

        // 채널마다 다른 allocator를 쓰는 out이면 그대로 남는다.
        VectorCurveValue mixed(failing);
        mixed.y = TangentCurve(other);
        REQUIRE(value.vectorSource.TryFlattenInto(objects, access, mixed));
        CHECK(&mixed.x.GetAllocator() == &failing && &mixed.y.GetAllocator() == &other &&
              &mixed.z.GetAllocator() == &failing);
        CHECK(mixed.Evaluate(0.0f).y == 2.0);
        ColorCurveValue colorOut(scene.allocator);
        REQUIRE(value.colorSource.TryFlattenInto(objects, access, colorOut));
        CHECK(
            colorOut.adjusted &&
            EvaluateUnadjustedColorCurve(colorOut.r, colorOut.g, colorOut.b, colorOut.a, 0.0f).a ==
                0.75f);
    }

    REQUIRE(objects.RequestDestroy(access, vector));
    REQUIRE(objects.RequestDestroy(access, color));
    auto borrowed = objects.Resolve<CurveSourceHolder>(access, holder);
    REQUIRE(borrowed);
    auto pointer = borrowed.Value().Get();
    REQUIRE(pointer);
    VectorCurveValue vectorOut(scene.allocator);
    REQUIRE(vectorOut.x.TryAddKey(9.0f, 9.0f));
    CHECK(pointer.Value()->vectorSource.TryFlattenInto(objects, access, vectorOut).Error() ==
          ErrorCode::Destroyed);
    CHECK(vectorOut.x.KeyCount() == 1);
    CHECK(pointer.Value()->vectorSource.TryEvaluate(objects, access, 0.0f).Error() ==
          ErrorCode::Destroyed);
    ColorCurveValue colorOut(scene.allocator);
    REQUIRE(colorOut.r.TryAddKey(9.0f, 9.0f));
    CHECK(pointer.Value()->colorSource.TryFlattenInto(objects, access, colorOut).Error() ==
          ErrorCode::Destroyed);
    CHECK(colorOut.r.KeyCount() == 1 && !colorOut.adjusted);
    CHECK(pointer.Value()->colorSource.TryEvaluate(objects, access, 0.0f).Error() ==
          ErrorCode::Destroyed);
}

} // namespace curve_source_tests_file
