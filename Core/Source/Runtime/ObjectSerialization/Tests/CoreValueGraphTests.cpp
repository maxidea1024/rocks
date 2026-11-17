#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>

// B4c Core 값 타입 필드의 그래프 저장·복원 시험이다(Docs/Design/ReflectionFoundations.ko.md 4.5절).
// CoreTypeBindings의 Binds 미러 17종이 생성한 실제 타입의 GeneratedType(Vector2·3·4<float|double>,
// Quaternion<float|double>, Rotator<float|double>, Color, LinearColor, IntVec2·3, Transform,
// Transform3<float|double> — 마지막 둘은 리플렉션 B9b2)을 모듈 집계 등록(fixture 모듈이
// CoreTypeBindings를 먼저 등록한다)으로 등록하고 태그형 중첩 Record로 왕복한다. 사용자 리플렉션
// struct(CoreValueBundle)가 다른 모듈의 미러 struct를 멤버로 담는 경로도 거친다. Atomic은 저장
// 형식을 바꾸지 않으므로 Atomic 미러도 같은 Record다.
using namespace rocks;
using reflection_fixture::CoreValueBundle;
using reflection_fixture::CoreValueFields;

// 생성 특수화는 실제 Core 타입에 있고(미러 타입이 아니다) Transform은 다른 미러 struct를 담는다.
static_assert(GeneratedType<Vec3>::Depth == 1);
static_assert(GeneratedType<Transform>::Depth == 2);
static_assert(GeneratedType<CoreValueBundle>::Depth == 2);
static_assert(!GeneratedType<Transform>::ContainsStrongReference);
static_assert(TypeIdOf<Vec3>() != TypeIdOf<Vec3d>());
static_assert(GeneratedType<Transform3f>::Depth == 2 && GeneratedType<Transform3d>::Depth == 2);
static_assert(TypeIdOf<Transform3f>() != TypeIdOf<Transform3d>() &&
              TypeIdOf<Transform3d>() != TypeIdOf<Transform>());

namespace {

constexpr uint32 EditorOnly = static_cast<uint32>(FieldFlags::EditorOnly);

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 모든 Core 값 타입 멤버에 기본값과 다른 값을 넣는다(정확히 표현되는 값만 쓴다 — 비교는 정확
// 일치다).
void Fill(CoreValueFields& fields) noexcept {
    fields.position = Vec3(1.5f, -2.25f, 3.0f);
    fields.rotation = Quat(0.0f, 0.0f, 0.75f, -0.5f);
    fields.tint.r = 255;
    fields.tint.g = 128;
    fields.tint.b = 7;
    fields.tint.a = 200;
    fields.placement.translation = Vec3d(1.0e9 + 0.25, -4.5, 0.125);
    fields.placement.rotation = Quatd(0.5, 0.5, 0.5, 0.5);
    fields.placement.scale = Vec3d(2.0, 3.0, 4.0);
    fields.cell.x = -1;
    fields.cell.y = 2;
    fields.cell.z = 2147483647;
    fields.debugColor.r = 0.25f;
    fields.debugColor.a = -1.0f;
    CoreValueBundle& bundle = fields.bundle;
    bundle.thresholds.x = 0.25;
    bundle.thresholds.y = 0.5;
    bundle.thresholds.z = 0.75;
    bundle.thresholds.w = 1.0e-9;
    bundle.uv.x = 0.125f;
    bundle.uv.y = -1.0f;
    bundle.uvPrecise.x = 1.0e12 + 0.5;
    bundle.uvPrecise.y = -3.25;
    bundle.weights.x = 1.0f;
    bundle.weights.y = 2.0f;
    bundle.weights.z = 3.0f;
    bundle.weights.w = 4.0f;
    bundle.orientation.x = 0.5;
    bundle.orientation.y = -0.5;
    bundle.orientation.z = 0.5;
    bundle.orientation.w = -0.5;
    bundle.heading.yaw = 0.25f;
    bundle.heading.pitch = -0.5f;
    bundle.heading.roll = 1.0f;
    bundle.headingPrecise.yaw = -2.0;
    bundle.headingPrecise.pitch = 0.0625;
    bundle.headingPrecise.roll = 3.0;
    bundle.size.x = -7;
    bundle.size.y = 9;
    fields.pose.translation = Vec3(-8.5f, 0.25f, 1.0e30f);
    fields.pose.rotation = Quat(0.5f, -0.5f, 0.5f, -0.5f);
    fields.pose.scale = Vec3(0.5f, 2.0f, -3.0f);
    fields.precisePose.translation = Vec3d(1.0e15 + 0.5, -2.0e-300, 7.0);
    fields.precisePose.rotation = Quatd(0.0, 0.0, -0.5, 0.75);
    fields.precisePose.scale = Vec3d(4.0, 1.0e-9, 0.125);
}

// Fill의 값이 그대로인지다(EditorOnly debugColor는 따로 본다).
bool Filled(const CoreValueFields& fields) noexcept {
    const CoreValueBundle& bundle = fields.bundle;
    return fields.position.x == 1.5f && fields.position.y == -2.25f && fields.position.z == 3.0f &&
           fields.rotation.x == 0.0f && fields.rotation.y == 0.0f && fields.rotation.z == 0.75f &&
           fields.rotation.w == -0.5f && fields.tint.r == 255 && fields.tint.g == 128 &&
           fields.tint.b == 7 && fields.tint.a == 200 &&
           fields.placement.translation.x == 1.0e9 + 0.25 &&
           fields.placement.translation.y == -4.5 && fields.placement.translation.z == 0.125 &&
           fields.placement.rotation.x == 0.5f && fields.placement.rotation.w == 0.5f &&
           fields.placement.scale.x == 2.0f && fields.placement.scale.y == 3.0f &&
           fields.placement.scale.z == 4.0f && fields.cell.x == -1 && fields.cell.y == 2 &&
           fields.cell.z == 2147483647 && bundle.thresholds.x == 0.25 &&
           bundle.thresholds.y == 0.5 && bundle.thresholds.z == 0.75 &&
           bundle.thresholds.w == 1.0e-9 && bundle.uv.x == 0.125f && bundle.uv.y == -1.0f &&
           bundle.uvPrecise.x == 1.0e12 + 0.5 && bundle.uvPrecise.y == -3.25 &&
           bundle.weights.x == 1.0f && bundle.weights.y == 2.0f && bundle.weights.z == 3.0f &&
           bundle.weights.w == 4.0f && bundle.orientation.x == 0.5 &&
           bundle.orientation.y == -0.5 && bundle.orientation.z == 0.5 &&
           bundle.orientation.w == -0.5 && bundle.heading.yaw == 0.25f &&
           bundle.heading.pitch == -0.5f && bundle.heading.roll == 1.0f &&
           bundle.headingPrecise.yaw == -2.0 && bundle.headingPrecise.pitch == 0.0625 &&
           bundle.headingPrecise.roll == 3.0 && bundle.size.x == -7 && bundle.size.y == 9 &&
           fields.pose.translation.x == -8.5f && fields.pose.translation.y == 0.25f &&
           fields.pose.translation.z == 1.0e30f && fields.pose.rotation.x == 0.5f &&
           fields.pose.rotation.y == -0.5f && fields.pose.rotation.z == 0.5f &&
           fields.pose.rotation.w == -0.5f && fields.pose.scale.x == 0.5f &&
           fields.pose.scale.y == 2.0f && fields.pose.scale.z == -3.0f &&
           fields.precisePose.translation.x == 1.0e15 + 0.5 &&
           fields.precisePose.translation.y == -2.0e-300 &&
           fields.precisePose.translation.z == 7.0 && fields.precisePose.rotation.x == 0.0 &&
           fields.precisePose.rotation.y == 0.0 && fields.precisePose.rotation.z == -0.5 &&
           fields.precisePose.rotation.w == 0.75 && fields.precisePose.scale.x == 4.0 &&
           fields.precisePose.scale.y == 1.0e-9 && fields.precisePose.scale.z == 0.125;
}

// 채운 CoreValueFields 하나를 저장한다.
Result<Array<uint8>> SaveFilled(Allocator& allocator, ObjectRegistry& objects,
                                ObjectGraphSaveOptions options = {}) noexcept {
    auto created =
        objects.NewObject<CoreValueFields>(objects.OwnerAccess(), TypeIdOf<CoreValueFields>());
    if (!created) {
        return Result<Array<uint8>>::Failure(created.Error());
    }

    {
        auto borrowed =
            objects.Resolve<CoreValueFields>(objects.OwnerAccess(), created.Value().Handle());
        if (!borrowed) {
            return Result<Array<uint8>>::Failure(borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return Result<Array<uint8>>::Failure(value.Error());
        }

        Fill(*value.Value());
    }

    return SaveObjectGraph(allocator, objects, objects.OwnerAccess(), created.Value().Handle(),
                           options);
}

} // namespace

// 등록된 struct 레코드는 미러 이름의 스키마와 실제 타입의 native 키를 갖는다. 값은 모든 멤버가
// 그대로 돌아오고 재저장 byte가 같다.
TEST(CoreValueGraph_BoundCoreTypesRoundTripThroughGeneratedCode) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            auto vector = types.AcquireValueType(TypeIdOf<Vec3>(), TypeKind::Struct);
            REQUIRE(vector);
            CHECK(vector.Value().Name() == StringView("rocks::core_type_bindings::Vec3Binding"));
            CHECK(vector.Value().StructOps().nativeType == NativeTypeKey<Vector3<float>>());
            CHECK(vector.Value().Schema().fields.Size() == 3);
            auto transform = types.AcquireValueType(TypeIdOf<Transform>(), TypeKind::Struct);
            REQUIRE(transform);
            CHECK(transform.Value().Struct().Depth() == 2);
            CHECK(transform.Value().Name() ==
                  StringView("rocks::core_type_bindings::TransformBinding"));
            auto thresholds = types.AcquireValueType(TypeIdOf<Vec4d>(), TypeKind::Struct);
            REQUIRE(thresholds);
            CHECK(thresholds.Value().StructOps().nativeType == NativeTypeKey<Vector4<double>>());
            // 별칭 Binds("rocks::Transform3f")는 실제 템플릿 인스턴스로 풀리고 Atomic이다.
            auto pose = types.AcquireValueType(TypeIdOf<Transform3f>(), TypeKind::Struct);
            REQUIRE(pose);
            CHECK(pose.Value().Name() ==
                  StringView("rocks::core_type_bindings::Transform3fBinding"));
            CHECK(pose.Value().StructOps().nativeType == NativeTypeKey<Transform3<float>>());
            CHECK(pose.Value().Struct().IsAtomic());
            CHECK(!transform.Value().Struct().IsAtomic());
        }

        {
            ObjectRegistry source(allocator, types, {0x4356, 1});
            ObjectRegistry target(allocator, types, {0x4356, 2});
            {
                auto bytes = SaveFilled(allocator, source);
                REQUIRE(bytes);
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View());
                REQUIRE(loaded);
                {
                    auto borrowed = target.Resolve<CoreValueFields>(target.OwnerAccess(),
                                                                    loaded.Value().Handle());
                    REQUIRE(borrowed);
                    auto value = borrowed.Value().Get();
                    REQUIRE(value);
                    const CoreValueFields& fields = *value.Value();
                    CHECK(Filled(fields));
                    CHECK(fields.debugColor.r == 0.25f && fields.debugColor.a == -1.0f);
                }

                auto resaved = SaveObjectGraph(allocator, target, target.OwnerAccess(),
                                               loaded.Value().Handle());
                REQUIRE(resaved);
                CHECK(SameBytes(bytes.Value().View(), resaved.Value().View()));
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// EditorOnly Core 값 필드(debugColor)는 쿠킹 저장에서 빠지고, 쿠킹 로드(excludedFlags =
// EditorOnly)로 다시 읽으면 생성자 기본값이 남는다(누락 = 기준). 나머지 Core 값 필드는 그대로다.
// 쿠킹 로드는 EditorOnly 필드가 있는 편집기 저장을 InvalidArgument로 거부한다.
TEST(CoreValueGraph_EditorOnlyCoreValueFieldIsExcludedAndKeepsDefault) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        {
            ObjectRegistry source(allocator, types, {0x4356, 3});
            ObjectRegistry target(allocator, types, {0x4356, 4});
            {
                ObjectGraphSaveOptions save;
                save.policy.excludedFlags = EditorOnly;
                auto bytes = SaveFilled(allocator, source, save);
                REQUIRE(bytes);
                ObjectGraphLoadOptions load;
                load.policy.excludedFlags = EditorOnly;
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View(), load);
                REQUIRE(loaded);
                {
                    auto borrowed = target.Resolve<CoreValueFields>(target.OwnerAccess(),
                                                                    loaded.Value().Handle());
                    REQUIRE(borrowed);
                    auto value = borrowed.Value().Get();
                    REQUIRE(value);
                    const CoreValueFields& fields = *value.Value();
                    CHECK(Filled(fields));
                    CHECK(fields.debugColor.r == 0.0f && fields.debugColor.g == 0.0f &&
                          fields.debugColor.b == 0.0f && fields.debugColor.a == 0.0f);
                }

                auto editor = SaveFilled(allocator, source);
                REQUIRE(editor);
                CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                      editor.Value().View(), load)
                          .Error() == ErrorCode::InvalidArgument);
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}
