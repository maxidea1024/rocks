#include <CurvesTestFixtures/CurveFixture.generated.h>
#include <CoreTypeBindings/InterpCurveTypes.generated.h>
#include <CurvesTestFixtures/CurvesTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ModuleRegistration.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <ObjectSerialization/Tests/GraphTestSupport.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextJson.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// InterpCurve 리플렉션 미러의 시험이다(Docs/Design/CurveData.ko.md 12.1절·16절 — CV9, 미러
// CoreTypeBindings/InterpCurveTypes.h, fixture CurvesTestFixtures/CurveFixture.h의
// InterpCurveHolder): 다섯 형식(float·Vec2d·Vec3d·Quatd·LinearColor)과 Array 원소·Optional 자리의
// 그래프 왕복·다시 저장 byte 같음, private 멤버의 도구 이름(points·looped·loopKeyOffset)·키
// Atomic·enum 미러, JSON 왕복, 검증 hook(정렬·유한 입력값·루프 정합)이 graph decode·JSON
// import·대입 묶음·경로 쓰기에서 어기는 값을 거부하고 값이 그대로인지, 미러 enum의 미선언 값 거부,
// 생성 깊은 복사의 할당 실패 원자성.
using namespace rocks;
namespace interp_curve_reflection_tests_file {

using namespace rocks::reflection_text;
using curves_fixture::InterpCurveHolder;

namespace {

struct Objects {
    Objects() noexcept
        : types(system), registration(types, system), objects(system, types, {0x4943, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::CurvesTestFixtures::RegisterModuleTypes(registration));
    }

    ~Objects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<InterpCurveHolder> Make() noexcept {
        auto created = objects.NewObject<InterpCurveHolder>(access, TypeIdOf<InterpCurveHolder>());
        if (!created) {
            platform::Fatal("test object creation failed");
        }

        return Move(created.Value());
    }

    template <class Edit>
    void With(ObjectHandle object, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<InterpCurveHolder>(access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    // 자리마다 고유한 키 입력값(2.5 / 1.625 / 8.5 / 11.5 / 13.25 / 15.75 / 17.5 — 변조 시험이
    // 찾는다)을 둔다. float 커브는 루프(끝 6.8125 — offset 2.0625)이고 모드를 섞는다.
    void Fill(ObjectHandle object) noexcept {
        With(object, [&](InterpCurveHolder& value) noexcept {
            REQUIRE(value.floats.TryAddPoint(1.25f, 3.0f));
            REQUIRE(value.floats.TryAddPoint(
                InterpCurvePoint<float>(2.5f, -1.0f, 0.5f, -0.25f, InterpCurveMode::CurveUser)));
            REQUIRE(value.floats.TryAddPoint(
                InterpCurvePoint<float>(4.75f, 2.0f, 0.0f, 0.0f, InterpCurveMode::Constant)));
            REQUIRE(value.floats.TrySetLoopKey(6.8125f));
            REQUIRE(value.vectors2.TryAddPoint(0.5f, Vec2d(1.0, 2.0)));
            REQUIRE(value.vectors2.TryAddPoint(1.625f, Vec2d(3.0, 4.0)));
            REQUIRE(value.vectors.TryAddPoint(
                InterpCurvePoint<Vec3d>(7.25f, Vec3d(1.0, 2.0, 3.0), Vec3d(0.5, 0.0, 0.0),
                                        Vec3d(0.0, 0.5, 0.0), InterpCurveMode::CurveBreak)));
            REQUIRE(value.vectors.TryAddPoint(8.5f, Vec3d(-4.0, 5.0, -6.0)));
            REQUIRE(value.rotations.TryAddPoint(10.25f, Quatd(0.0, 0.0, 0.0, 1.0)));
            REQUIRE(value.rotations.TryAddPoint(
                11.5f, Quatd(0.0, 0.0, 0.70710678118654757, 0.70710678118654757)));
            REQUIRE(value.colors.TryAddPoint(12.75f, LinearColor(1.0f, 0.5f, 0.25f, 1.0f)));
            REQUIRE(value.colors.TryAddPoint(InterpCurvePoint<LinearColor>(
                13.25f, LinearColor(0.0f, 0.125f, 0.75f, 0.5f), LinearColor(), LinearColor(),
                InterpCurveMode::CurveAutoClamped)));
            REQUIRE(value.list.TryPushBack(InterpCurve<float>(system)));
            REQUIRE(value.list[0].TryAddPoint(14.5f, 7.0f));
            REQUIRE(value.list[0].TryAddPoint(15.75f, 8.0f));
            value.maybe.Emplace(InterpCurve<Vec3d>(system));
            REQUIRE(value.maybe.Value().TryAddPoint(16.25f, Vec3d(9.0, 9.0, 9.0)));
            REQUIRE(value.maybe.Value().TryAddPoint(17.5f, Vec3d(10.0, 10.0, 10.0)));
        });
    }

    Result<void> Apply(ObjectHandle object, Span<const TextAssignment> inputs) noexcept {
        diagnostics.Clear();
        auto prepared = PrepareAssignments(AssignmentTarget::Live(objects, access, object), inputs,
                                           options, diagnostics);
        if (!prepared) {
            return Result<void>::Failure(prepared.Error());
        }

        auto published = prepared.Value().Publish();
        return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
    }

    Result<void> Export(ObjectHandle object, StringBuilderBase& out) noexcept {
        out.Clear();
        JsonWriter writer(out);
        if (auto written =
                ExportJson(AssignmentTarget::Live(objects, access, object), writer, options);
            !written) {
            return written;
        }

        return writer.Finish();
    }

    Result<void> Import(ObjectHandle object, StringView json) noexcept {
        diagnostics.Clear();
        JsonDocument document(system);
        if (!document.Parse(json).Succeeded()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto imported = ImportJson(document.Root(), AssignmentTarget::Live(objects, access, object),
                                   options, diagnostics);
        if (!imported) {
            return Result<void>::Failure(imported.Error());
        }

        auto published = imported.Value().Publish();
        return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    TextDiagnostics diagnostics;
    TextCodecOptions options;
};

TextAssignment Set(const char* path, const char* value) noexcept {
    TextAssignment assignment;
    assignment.kind = AssignmentKind::Set;
    assignment.path = StringView(path);
    assignment.value = StringView(value);
    return assignment;
}

template <typename T>
bool SameCurve(const InterpCurve<T>& left, const InterpCurve<T>& right) noexcept {
    if (left.PointCount() != right.PointCount() || left.IsLooped() != right.IsLooped() ||
        platform::BitCast<uint32>(left.LoopKeyOffset()) !=
            platform::BitCast<uint32>(right.LoopKeyOffset())) {
        return false;
    }

    for (usize index = 0; index < left.PointCount(); ++index) {
        if (!(left.GetPoint(index) == right.GetPoint(index))) {
            return false;
        }
    }

    return true;
}

// 같은 Fill을 한 두 값이 같은지다.
void CheckSame(const InterpCurveHolder& value, const InterpCurveHolder& expected) noexcept {
    CHECK(SameCurve(value.floats, expected.floats));
    CHECK(SameCurve(value.vectors2, expected.vectors2));
    CHECK(SameCurve(value.vectors, expected.vectors));
    CHECK(SameCurve(value.rotations, expected.rotations));
    CHECK(SameCurve(value.colors, expected.colors));
    REQUIRE(value.list.Size() == 1 && value.maybe.HasValue());
    CHECK(SameCurve(value.list[0], expected.list[0]));
    CHECK(SameCurve(value.maybe.Value(), expected.maybe.Value()));
    CHECK(value.floats.IsLooped() && value.floats.LoopKeyOffset() == 2.0625f);
    CHECK(value.floats.GetPoint(1).mode == InterpCurveMode::CurveUser);
}

usize CountOccurrences(Span<const uint8> haystack, Span<const uint8> needle,
                       usize& first) noexcept {
    usize count = 0;
    first = IndexNone;
    for (usize start = 0; start + needle.Size() <= haystack.Size(); ++start) {
        bool same = true;
        for (usize index = 0; same && index < needle.Size(); ++index) {
            same = haystack[start + index] == needle[index];
        }

        if (same) {
            first = count == 0 ? start : first;
            ++count;
        }
    }

    return count;
}

bool PatchFloat(Array<uint8>& bytes, float from, float to) noexcept {
    uint8 needle[4];
    uint8 replacement[4];
    platform::MemoryCopy(needle, &from, 4);
    platform::MemoryCopy(replacement, &to, 4);
    usize at = IndexNone;
    if (CountOccurrences(bytes.View(), Span<const uint8>(needle, 4), at) != 1) {
        return false;
    }

    platform::MemoryCopy(bytes.Data() + at, replacement, 4);
    return true;
}

void ReplaceOnce(StringView text, StringView from, StringView to, StringBuilderBase& out) noexcept {
    out.Clear();
    usize found = IndexNone;
    usize count = 0;
    for (usize start = 0; start + from.ByteCount() <= text.ByteCount(); ++start) {
        auto slice = text.TrySliceBytes(start, from.ByteCount());
        if (slice && slice.Value() == from) {
            found = count == 0 ? start : found;
            ++count;
        }
    }

    if (count != 1) {
        return;
    }

    const usize after = found + from.ByteCount();
    CHECK(out.TryAppend(text.TrySliceBytes(0, found).Value()));
    CHECK(out.TryAppend(to));
    CHECK(out.TryAppend(text.TrySliceBytes(after, text.ByteCount() - after).Value()));
}

const FieldSchema& FieldNamed(const TypeSchema& schema, const char* name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == StringView(name)) {
            return field;
        }
    }

    platform::Fatal("interp curve schema field missing");
}

} // namespace

// 스키마: 커브 미러의 private 멤버는 C++ 이름(points_·looped_·loopKeyOffset_)과 도구
// 이름(points·looped· loopKeyOffset)을 갖고, 키 미러는 Atomic, enum 미러는 실제 enum의 번호다.
// 스키마 이름은 미러 이름이다.
TEST(InterpCurveReflection_SchemaShape) {
    const TypeSchema* const curves[] = {
        &TypeOf<InterpCurve<float>>(), &TypeOf<InterpCurve<Vec2d>>(), &TypeOf<InterpCurve<Vec3d>>(),
        &TypeOf<InterpCurve<Quatd>>(), &TypeOf<InterpCurve<LinearColor>>()};
    const char* const members[][2] = {
        {"points_", "points"}, {"looped_", "looped"}, {"loopKeyOffset_", "loopKeyOffset"}};
    for (const TypeSchema* schema : curves) {
        REQUIRE(schema->fields.Size() == 3);
        for (const auto& member : members) {
            const FieldSchema& field = FieldNamed(*schema, member[0]);
            CHECK(StringView(field.toolName.Data(), field.toolName.Size()) ==
                  StringView(member[1]));
        }

        CHECK((schema->flags & static_cast<uint32>(TypeFlags::Atomic)) == 0);
    }

    const TypeSchema* const points[] = {
        &TypeOf<InterpCurvePoint<float>>(), &TypeOf<InterpCurvePoint<Vec2d>>(),
        &TypeOf<InterpCurvePoint<Vec3d>>(), &TypeOf<InterpCurvePoint<Quatd>>(),
        &TypeOf<InterpCurvePoint<LinearColor>>()};
    for (const TypeSchema* schema : points) {
        CHECK(schema->fields.Size() == 5);
        CHECK((schema->flags & static_cast<uint32>(TypeFlags::Atomic)) != 0);
    }

    const TypeSchema& floats = TypeOf<InterpCurve<float>>();
    CHECK(StringView(floats.name.Data(), floats.name.Size()) ==
          StringView("rocks::core_type_bindings::InterpCurveFloatBinding"));
    const EnumSchema& modes = GeneratedType<InterpCurveMode>::Schema();
    CHECK(StringView(modes.name.Data(), modes.name.Size()) ==
          StringView("rocks::core_type_bindings::InterpCurveModeBinding"));
    REQUIRE(modes.values.Size() == 6);
    CHECK(modes.values[5].value == static_cast<int64>(InterpCurveMode::CurveAutoClamped));
}

// 모든 자리의 그래프 왕복과 다시 저장한 byte 같음.
TEST(InterpCurveReflection_GraphRoundTrip) {
    Objects fixture;
    ObjectRegistry target(fixture.system, fixture.types, {0x4943, 2});
    {
        auto holder = fixture.Make();
        fixture.Fill(holder.Handle());
        auto saved =
            SaveObjectGraph(fixture.system, fixture.objects, fixture.access, holder.Handle());
        REQUIRE(saved);
        auto loaded = LoadObjectGraph(fixture.system, target, fixture.types, target.OwnerAccess(),
                                      saved.Value().View());
        REQUIRE(loaded);
        {
            auto borrowed =
                target.Resolve<InterpCurveHolder>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            fixture.With(holder.Handle(), [&](InterpCurveHolder& expected) noexcept {
                CheckSame(*value.Value(), expected);
            });
            // 풀어 낸 커브를 평가할 수 있다(루프 구간 안 — NaN이 아니다).
            CHECK(!platform::PlatformMath::IsNaN(value.Value()->floats.Eval(5.5f)));
        }

        auto again =
            SaveObjectGraph(fixture.system, target, target.OwnerAccess(), loaded.Value().Handle());
        REQUIRE(again);
        CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
    }

    REQUIRE(target.Shutdown(target.OwnerAccess()));
}

// graph decode의 hook: 자리마다 키 입력값 하나를 앞 키보다 작게(또는 NaN으로), float 커브의 루프
// offset을 음수로 바꾸면 로드가 InvalidArgument이고 대상에 아무것도 남지 않는다.
TEST(InterpCurveReflection_DecodeRejectsBrokenCurves) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    auto saved = SaveObjectGraph(fixture.system, fixture.objects, fixture.access, holder.Handle());
    REQUIRE(saved);
    const float times[] = {2.5f, 1.625f, 8.5f, 11.5f, 13.25f, 15.75f, 17.5f};
    for (const float time : times) {
        for (int32 nonFinite = 0; nonFinite < 2; ++nonFinite) {
            Array<uint8> bytes(fixture.system);
            REQUIRE(bytes.TryAppend(saved.Value().View()));
            REQUIRE(
                PatchFloat(bytes, time,
                           nonFinite != 0 ? platform::PlatformMath::QuietNaN<float>() : -1000.0f));
            ObjectRegistry target(fixture.system, fixture.types, {0x4943, 3});
            auto loaded = LoadObjectGraph(fixture.system, target, fixture.types,
                                          target.OwnerAccess(), bytes.View());
            CHECK(!loaded && loaded.Error() == ErrorCode::CorruptData);
            CHECK(target.Stats().liveObjects == 0);
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }
    }

    Array<uint8> bytes(fixture.system);
    REQUIRE(bytes.TryAppend(saved.Value().View()));
    REQUIRE(PatchFloat(bytes, 2.0625f, -2.0625f)); // 루프 offset
    ObjectRegistry target(fixture.system, fixture.types, {0x4943, 4});
    auto loaded =
        LoadObjectGraph(fixture.system, target, fixture.types, target.OwnerAccess(), bytes.View());
    CHECK(!loaded && loaded.Error() == ErrorCode::CorruptData);
    REQUIRE(target.Shutdown(target.OwnerAccess()));
}

// JSON 왕복(도구 이름 points·looped·loopKeyOffset, enum 이름)과, 자리마다 정렬을 깨는 JSON import
// 거부 (InvalidStructValue·InvalidArgument — 값 그대로).
TEST(InterpCurveReflection_JsonRoundTripAndHookRejection) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    StringBuilder<16384> text(fixture.system);
    REQUIRE(fixture.Export(holder.Handle(), text));
    CHECK(text.View().Contains("\"points\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"loopKeyOffset\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"CurveUser\"", StringComparison::CaseSensitive));
    CHECK(!text.View().Contains("points_", StringComparison::CaseSensitive));

    auto copy = fixture.Make();
    REQUIRE(fixture.Import(copy.Handle(), text.View()));
    fixture.With(holder.Handle(), [&](InterpCurveHolder& expected) noexcept {
        fixture.With(copy.Handle(),
                     [&](InterpCurveHolder& value) noexcept { CheckSame(value, expected); });
    });
    StringBuilder<16384> again(fixture.system);
    REQUIRE(fixture.Export(copy.Handle(), again));
    CHECK(again.View() == text.View());

    const char* const times[] = {"2.5", "1.625", "8.5", "11.5", "13.25", "15.75", "17.5"};
    for (const char* time : times) {
        StringBuilder<16384> broken(fixture.system);
        ReplaceOnce(text.View(), StringView(time), StringView("-1000"), broken);
        REQUIRE(broken.View().ByteCount() != 0);
        CHECK(fixture.Import(copy.Handle(), broken.View()).Error() == ErrorCode::InvalidArgument);
        CHECK(fixture.diagnostics.Count() >= 1 &&
              fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidStructValue);
        fixture.With(holder.Handle(), [&](InterpCurveHolder& expected) noexcept {
            fixture.With(copy.Handle(),
                         [&](InterpCurveHolder& value) noexcept { CheckSame(value, expected); });
        });
    }
}

// 대입 묶음(명령행): 도구 이름으로 쓴다. 정렬을 깨는 입력값·루프를 켜면서 offset 0은 거부되고 묶음
// 전체가 게시되지 않는다. enum 값은 이름으로 쓰고 미선언 이름은 거부된다.
TEST(InterpCurveReflection_AssignmentsUseToolNamesAndValidate) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    const TextAssignment valid[] = {Set("floats.points[0].mode", "CurveAutoClamped"),
                                    Set("vectors2.points[1].inValue", "2.25"),
                                    Set("floats.loopKeyOffset", "0.5")};
    REQUIRE(fixture.Apply(holder.Handle(), valid));
    fixture.With(holder.Handle(), [](InterpCurveHolder& value) noexcept {
        CHECK(value.floats.GetPoint(0).mode == InterpCurveMode::CurveAutoClamped);
        CHECK(value.vectors2.GetPoint(1).inValue == 2.25f && value.floats.LoopKeyOffset() == 0.5f);
    });

    const TextAssignment unsorted[] = {Set("floats.points[0].mode", "Linear"),
                                       Set("list[0].points[0].inValue", "16")};
    CHECK(fixture.Apply(holder.Handle(), unsorted).Error() == ErrorCode::InvalidArgument);
    CHECK(fixture.diagnostics.Count() >= 1 &&
          fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidStructValue);
    const TextAssignment loop[] = {Set("vectors.looped", "true")};
    CHECK(fixture.Apply(holder.Handle(), loop).Error() == ErrorCode::InvalidArgument);
    const TextAssignment mode[] = {Set("colors.points[0].mode", "Cubic")};
    CHECK(!fixture.Apply(holder.Handle(), mode));
    fixture.With(holder.Handle(), [](InterpCurveHolder& value) noexcept {
        CHECK(value.floats.GetPoint(0).mode == InterpCurveMode::CurveAutoClamped);
        CHECK(value.list[0].GetPoint(0).inValue == 14.5f && !value.vectors.IsLooped());
        CHECK(value.colors.GetPoint(0).mode == InterpCurveMode::Linear);
    });
}

// 경로 쓰기: 커브 안 키 입력값 쓰기는 커브 hook으로 검증된다(필드·원소·Optional) — 정렬을 지키면
// 게시되고 깨면 InvalidArgument(StructValidation)이며 그대로다. 미러 enum의 미선언 값(6)도
// 거부된다.
TEST(InterpCurveReflection_PathWritesValidate) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    fixture.Fill(handle);
    struct Case {
        const char* path;
        float valid;
        float invalid;
    };
    const Case cases[] = {
        {"floats.points[1].inValue", 2.0f, 5.0f},   {"vectors2.points[0].inValue", 0.25f, 2.0f},
        {"vectors.points[1].inValue", 9.0f, 7.0f},  {"rotations.points[0].inValue", 10.0f, 12.0f},
        {"colors.points[1].inValue", 14.0f, 12.5f}, {"list[0].points[1].inValue", 16.0f, 14.0f},
        {"maybe.points[0].inValue", 16.0f, 18.0f},  {"floats.loopKeyOffset", 3.0f, -1.0f}};
    for (const Case& item : cases) {
        PropertyPathBuffer path;
        REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle, StringView(item.path),
                                  path));
        FieldValue input(fixture.system);
        input.kind = ValueKind::Float32;
        input.float32 = item.invalid;
        PropertyPathFailure failure;
        auto rejected = PathWrite(fixture.objects, fixture.access, handle, path.View(), input,
                                  nullptr, &failure);
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::StructValidation);
        input.float32 = item.valid;
        CHECK(PathWrite(fixture.objects, fixture.access, handle, path.View(), input));
    }

    PropertyPathBuffer modePath;
    REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle,
                              StringView("vectors.points[0].mode"), modePath));
    FieldValue mode(fixture.system);
    mode.kind = ValueKind::Enum;
    mode.type = TypeIdOf<InterpCurveMode>();
    mode.signedInteger = 6;
    PropertyPathFailure modeFailure;
    CHECK(!PathWrite(fixture.objects, fixture.access, handle, modePath.View(), mode, nullptr,
                     &modeFailure));
    CHECK(modeFailure.reason == PropertyPathError::InvalidValue);
    mode.signedInteger = static_cast<int64>(InterpCurveMode::CurveUser);
    CHECK(PathWrite(fixture.objects, fixture.access, handle, modePath.View(), mode));

    fixture.With(handle, [](InterpCurveHolder& value) noexcept {
        CHECK(value.floats.GetPoint(1).inValue == 2.0f && value.floats.LoopKeyOffset() == 3.0f);
        CHECK(value.vectors2.GetPoint(0).inValue == 0.25f);
        CHECK(value.vectors.GetPoint(1).inValue == 9.0f);
        CHECK(value.vectors.GetPoint(0).mode == InterpCurveMode::CurveUser);
        CHECK(value.rotations.GetPoint(0).inValue == 10.0f);
        CHECK(value.colors.GetPoint(1).inValue == 14.0f);
        CHECK(value.list[0].GetPoint(1).inValue == 16.0f);
        CHECK(value.maybe.Value().GetPoint(0).inValue == 16.0f);
    });
}

// 생성 깊은 복사(RuntimeStructOps::copy — 리플렉션 쓰기가 사본을 만들 때 쓴다)는 할당에 실패하면
// OutOfMemory이고 대상이 그대로다(주입 단언).
TEST(InterpCurveReflection_CopyAllocationFailureIsAtomic) {
    SystemAllocator system;
    InterpCurve<Vec3d> source(system);
    REQUIRE(source.TryAddPoint(0.0f, Vec3d(1.0, 2.0, 3.0)));
    REQUIRE(source.TryAddPoint(1.0f, Vec3d(4.0, 5.0, 6.0)));
    REQUIRE(source.TrySetLoopKey(2.0f));
    bool injected = false;
    for (uint64 attempt = 1; attempt <= 6; ++attempt) {
        test::NFailAllocator failing(system);
        InterpCurve<Vec3d> destination(failing);
        REQUIRE(destination.TryAddPoint(5.0f, Vec3d(7.0, 8.0, 9.0)));
        failing.FailOnAttempt(attempt); // 시도 수는 여기서 0부터 센다
        const Result<void> copied =
            GeneratedType<InterpCurve<Vec3d>>::CopyValue(destination, source, failing);
        failing.DisableFailure();
        if (!copied) {
            injected = true;
            CHECK(copied.Error() == ErrorCode::OutOfMemory);
            REQUIRE(destination.PointCount() == 1);
            CHECK(destination.GetPoint(0).inValue == 5.0f && !destination.IsLooped());
        } else {
            CHECK(SameCurve(destination, source));
        }
    }

    CHECK(injected);
}

// 묶음 리뷰 1: 키를 모두 지운 루프 커브(InterpCurve::Reset·TryRemovePoint는 루프를 남긴다 — UE와
// 같다)도 그래프와 JSON으로 왕복한다(hook은 루프면 offset이 유한한 양수인지만 본다).
TEST(InterpCurveReflection_EmptyLoopedCurveRoundTrips) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    fixture.With(holder.Handle(), [](InterpCurveHolder& value) noexcept {
        value.floats.Reset();
        CHECK(value.floats.IsLooped() && value.floats.PointCount() == 0);
    });
    auto saved = SaveObjectGraph(fixture.system, fixture.objects, fixture.access, holder.Handle());
    REQUIRE(saved);
    ObjectRegistry target(fixture.system, fixture.types, {0x4943, 5});
    {
        auto loaded = LoadObjectGraph(fixture.system, target, fixture.types, target.OwnerAccess(),
                                      saved.Value().View());
        REQUIRE(loaded);
        auto borrowed =
            target.Resolve<InterpCurveHolder>(target.OwnerAccess(), loaded.Value().Handle());
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        CHECK(value.Value()->floats.IsLooped() && value.Value()->floats.PointCount() == 0);
        CHECK(value.Value()->floats.LoopKeyOffset() == 2.0625f);
    }

    REQUIRE(target.Shutdown(target.OwnerAccess()));
    StringBuilder<16384> text(fixture.system);
    REQUIRE(fixture.Export(holder.Handle(), text));
    auto copy = fixture.Make();
    REQUIRE(fixture.Import(copy.Handle(), text.View()));
    fixture.With(copy.Handle(), [](InterpCurveHolder& value) noexcept {
        CHECK(value.floats.IsLooped() && value.floats.PointCount() == 0);
    });
}

// 묶음 리뷰 5: 미선언 모드(Core API로 넣은 9)를 담은 값은 graph decode가 거부하고(저장이 먼저
// 거부해도 된다), JSON의 미선언 모드 이름은 import가 거부한다(값 그대로).
TEST(InterpCurveReflection_UndeclaredModeIsRejected) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    StringBuilder<16384> text(fixture.system);
    REQUIRE(fixture.Export(holder.Handle(), text));
    StringBuilder<16384> broken(fixture.system);
    ReplaceOnce(text.View(), StringView("\"CurveUser\""), StringView("\"Cubic\""), broken);
    REQUIRE(broken.View().ByteCount() != 0);
    auto copy = fixture.Make();
    CHECK(!fixture.Import(copy.Handle(), broken.View()));
    fixture.With(copy.Handle(),
                 [](InterpCurveHolder& value) noexcept { CHECK(value.floats.PointCount() == 0); });

    fixture.With(holder.Handle(), [](InterpCurveHolder& value) noexcept {
        value.floats.SetMode(0, static_cast<InterpCurveMode>(9));
    });
    auto saved = SaveObjectGraph(fixture.system, fixture.objects, fixture.access, holder.Handle());
    if (saved) {
        ObjectRegistry target(fixture.system, fixture.types, {0x4943, 6});
        auto loaded = LoadObjectGraph(fixture.system, target, fixture.types, target.OwnerAccess(),
                                      saved.Value().View());
        CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
        CHECK(target.Stats().liveObjects == 0);
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }
}
} // namespace interp_curve_reflection_tests_file
