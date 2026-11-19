#include <CurvesTestFixtures/CurveFixture.generated.h>
#include <Curves/CurveTypes.generated.h>
#include <Curves/PackedTangentCurve.generated.h>
#include <Curves/TangentCurve.generated.h>
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
#include <TestSupport/Test.h>

// PackedTangentCurve의 리플렉션 시험이다(Docs/Design/CurveData.ko.md 11.4절·16절 — CV8, fixture
// CurvesTestFixtures/CurveFixture.h의 PackedCurveHolder): 필드·Array 원소·Optional 자리의 그래프
// 왕복과 다시 저장 byte 같음, packed가 Bytes wire(D18 — JSON은 Base64 문자열 하나), 검증 hook
// (D21 — 칸 정합·키 시간)이 graph decode·JSON import·경로 쓰기에서 어기는 값을 거부하고 값이
// 그대로인지.
using namespace rocks;
using namespace rocks::reflection_text;
using curves_fixture::PackedCurveHolder;

namespace {

TangentCurveKey Key(float time, float value, CurveInterp interp) noexcept {
    TangentCurveKey key;
    key.time = time;
    key.value = value;
    key.interp = interp;
    return key;
}

struct Objects {
    Objects() noexcept
        : types(system), registration(types, system), objects(system, types, {0x5043, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::CurvesTestFixtures::RegisterModuleTypes(registration));
    }

    ~Objects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<PackedCurveHolder> Make() noexcept {
        auto created = objects.NewObject<PackedCurveHolder>(access, TypeIdOf<PackedCurveHolder>());
        if (!created) {
            platform::Fatal("test object creation failed");
        }

        return Move(created.Value());
    }

    template <class Edit>
    void With(ObjectHandle object, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<PackedCurveHolder>(access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    // 자리마다 고유한 Float32 키 시간(3.25 / 6.75 / 11.5 — 변조 시험이 찾는다)을 둔다:
    // packed는 Mixed 3키, list[0]은 Weighted 2키, maybe는 기본값 있는 Empty.
    void Fill(ObjectHandle object) noexcept {
        With(object, [&](PackedCurveHolder& value) noexcept {
            TangentCurve source(system);
            const TangentCurveKey mixed[3] = {Key(1.0f, 2.0f, CurveInterp::Constant),
                                              Key(3.25f, -1.0f, CurveInterp::Linear),
                                              Key(4.0f, 0.5f, CurveInterp::Cubic)};
            REQUIRE(source.TrySetKeys(Span<const TangentCurveKey>(mixed, 3)));
            source.postExtrapolation = CurveExtrapolation::Cycle;
            REQUIRE(value.packed.TryPack(source));
            REQUIRE(value.packed.Format() == PackedCurveFormat::Mixed);

            TangentCurveKey weighted[2] = {Key(5.0f, 1.0f, CurveInterp::Cubic),
                                           Key(6.75f, 3.0f, CurveInterp::Cubic)};
            weighted[0].tangentWeight = CurveTangentWeight::Both;
            weighted[0].leaveTangentWeight = 0.5f;
            REQUIRE(source.TrySetKeys(Span<const TangentCurveKey>(weighted, 2)));
            REQUIRE(value.list.TryPushBack(PackedTangentCurve(system)));
            REQUIRE(value.list[0].TryPack(source));
            REQUIRE(value.list[0].Format() == PackedCurveFormat::Weighted);

            TangentCurve empty(system);
            empty.defaultValue = 11.5f;
            value.maybe.Emplace(PackedTangentCurve(system));
            REQUIRE(value.maybe.Value().TryPack(empty));
        });
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

bool SameBytes(Span<const uint8> a, Span<const uint8> b) noexcept {
    if (a.Size() != b.Size()) {
        return false;
    }

    for (usize index = 0; index < a.Size(); ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }

    return true;
}

// Fill한 값이 그대로인지다(expected는 같은 Fill을 한 다른 객체).
void CheckSame(const PackedCurveHolder& value, const PackedCurveHolder& expected) noexcept {
    auto same = [](const PackedTangentCurve& a, const PackedTangentCurve& b) noexcept {
        return a.Format() == b.Format() && a.KeyTimeFormat() == b.KeyTimeFormat() &&
               a.PreExtrapolation() == b.PreExtrapolation() &&
               a.PostExtrapolation() == b.PostExtrapolation() && a.KeyCount() == b.KeyCount() &&
               a.EmptyDefault().HasValue() == b.EmptyDefault().HasValue() &&
               SameBytes(a.Packed(), b.Packed());
    };

    CHECK(same(value.packed, expected.packed));
    REQUIRE(value.list.Size() == 1 && value.maybe.HasValue());
    CHECK(same(value.list[0], expected.list[0]));
    CHECK(same(value.maybe.Value(), expected.maybe.Value()));
    CHECK(value.maybe.Value().EmptyDefault().Value() == 11.5f);
    CHECK(value.packed.Evaluate(3.5f) == expected.packed.Evaluate(3.5f));
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

// text에서 처음 나오는 key 뒤의 첫 숫자열을 replacement로 바꾼다(key가 없으면 out은 빈다).
void ReplaceNumberAfter(StringView text, StringView key, StringView replacement,
                        StringBuilderBase& out) noexcept {
    out.Clear();
    usize found = IndexNone;
    for (usize start = 0; found == IndexNone && start + key.ByteCount() <= text.ByteCount();
         ++start) {
        auto slice = text.TrySliceBytes(start, key.ByteCount());
        if (slice && slice.Value() == key) {
            found = start;
        }
    }

    if (found == IndexNone) {
        return;
    }

    const char* const data = text.Data();
    usize begin = found + key.ByteCount();
    while (begin < text.ByteCount() && !(data[begin] >= '0' && data[begin] <= '9')) {
        ++begin;
    }

    usize end = begin;
    while (end < text.ByteCount() && data[end] >= '0' && data[end] <= '9') {
        ++end;
    }

    CHECK(out.TryAppend(text.TrySliceBytes(0, begin).Value()));
    CHECK(out.TryAppend(replacement));
    CHECK(out.TryAppend(text.TrySliceBytes(end, text.ByteCount() - end).Value()));
}

} // namespace

// 모든 자리의 그래프 왕복과 다시 저장한 byte 같음.
TEST(PackedCurveReflection_GraphRoundTrip) {
    Objects fixture;
    ObjectRegistry target(fixture.system, fixture.types, {0x5043, 2});
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
                target.Resolve<PackedCurveHolder>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            fixture.With(holder.Handle(), [&](PackedCurveHolder& expected) noexcept {
                CheckSame(*value.Value(), expected);
            });
        }

        auto again =
            SaveObjectGraph(fixture.system, target, target.OwnerAccess(), loaded.Value().Handle());
        REQUIRE(again);
        CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
    }

    REQUIRE(target.Shutdown(target.OwnerAccess()));
}

// graph decode의 hook: 자리마다 packed 안의 키 시간을 앞 키보다 작게(또는 NaN으로), Empty의 기본값
// 자리에는 손대지 않고 packed 키 시간만 바꾸면 로드가 InvalidArgument이고 대상에 아무것도 남지
// 않는다.
TEST(PackedCurveReflection_DecodeRejectsBrokenPacked) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    auto saved = SaveObjectGraph(fixture.system, fixture.objects, fixture.access, holder.Handle());
    REQUIRE(saved);
    const float times[] = {3.25f, 6.75f};
    for (const float time : times) {
        for (int32 nonFinite = 0; nonFinite < 2; ++nonFinite) {
            Array<uint8> bytes(fixture.system);
            REQUIRE(bytes.TryAppend(saved.Value().View()));
            REQUIRE(
                PatchFloat(bytes, time,
                           nonFinite != 0 ? platform::PlatformMath::QuietNaN<float>() : -1000.0f));
            ObjectRegistry target(fixture.system, fixture.types, {0x5043, 3});
            auto loaded = LoadObjectGraph(fixture.system, target, fixture.types,
                                          target.OwnerAccess(), bytes.View());
            CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
            CHECK(target.Stats().liveObjects == 0);
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }
    }
}

// JSON 왕복(packed는 Base64 문자열)과 칸 정합을 깨는 import 거부(값 그대로): keyCount가 packed
// 크기와 어긋남.
TEST(PackedCurveReflection_JsonRoundTripAndHookRejection) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    StringBuilder<8192> text(fixture.system);
    REQUIRE(fixture.Export(holder.Handle(), text));
    CHECK(text.View().Contains("\"packed\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"keyCount\"", StringComparison::CaseSensitive));
    CHECK(!text.View().Contains("packed_", StringComparison::CaseSensitive));

    auto copy = fixture.Make();
    REQUIRE(fixture.Import(copy.Handle(), text.View()));
    fixture.With(holder.Handle(), [&](PackedCurveHolder& expected) noexcept {
        fixture.With(copy.Handle(),
                     [&](PackedCurveHolder& value) noexcept { CheckSame(value, expected); });
    });
    StringBuilder<8192> again(fixture.system);
    REQUIRE(fixture.Export(copy.Handle(), again));
    CHECK(again.View() == text.View());

    // 처음 나오는 keyCount(어느 자리든 packed 크기와 어긋난다 — 키 수 3·2·0)를 4로.
    StringBuilder<8192> broken(fixture.system);
    ReplaceNumberAfter(text.View(), StringView("\"keyCount\""), StringView("4"), broken);
    REQUIRE(broken.View().ByteCount() != 0);
    CHECK(fixture.Import(copy.Handle(), broken.View()).Error() == ErrorCode::InvalidArgument);
    CHECK(fixture.diagnostics.Count() >= 1 &&
          fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidStructValue);
    fixture.With(holder.Handle(), [&](PackedCurveHolder& expected) noexcept {
        fixture.With(copy.Handle(),
                     [&](PackedCurveHolder& value) noexcept { CheckSame(value, expected); });
    });
}

// 경로 쓰기: 칸 하나를 바꿔 정합이 깨지면 InvalidArgument(StructValidation)이고 그대로다(필드·
// 원소·Optional). 정합을 지키는 쓰기(값 float의 byte, 외삽)는 게시된다.
TEST(PackedCurveReflection_PathWritesValidate) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    fixture.Fill(handle);
    struct Case {
        const char* path;
        ValueKind kind;
        uint64 invalid;
    };
    const Case rejected[] = {
        {"packed.keyCount", ValueKind::UInt32, 4},
        {"packed.packed[1]", ValueKind::UInt8, 0}, // 보간 byte
        {"list[0].keyCount", ValueKind::UInt32, 3},
        {"list[0].packed[1]", ValueKind::UInt8, 4}, // 가중 byte
        {"maybe.keyCount", ValueKind::UInt32, 2},
    };
    for (const Case& item : rejected) {
        PropertyPathBuffer path;
        REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle, StringView(item.path),
                                  path));
        FieldValue input(fixture.system);
        input.kind = item.kind;
        input.unsignedInteger = item.invalid;
        PropertyPathFailure failure;
        auto written = PathWrite(fixture.objects, fixture.access, handle, path.View(), input,
                                 nullptr, &failure);
        CHECK(!written && written.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::StructValidation);
    }

    // 상수 칸은 Constant 형식에서만 0이 아닐 수 있다.
    {
        PropertyPathBuffer path;
        REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle,
                                  StringView("packed.constantValue"), path));
        FieldValue input(fixture.system);
        input.kind = ValueKind::Float32;
        input.float32 = 1.0f;
        PropertyPathFailure failure;
        auto written = PathWrite(fixture.objects, fixture.access, handle, path.View(), input,
                                 nullptr, &failure);
        CHECK(!written && failure.reason == PropertyPathError::StructValidation);
    }

    // Mixed 3키 Float32: 보간 3 + 틈 1, 시간 12, 데이터는 offset 16 — 둘째 키 값(offset 28)의
    // 최하위 byte를 바꾸는 쓰기는 정합을 지킨다.
    {
        PropertyPathBuffer path;
        REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle,
                                  StringView("packed.packed[28]"), path));
        FieldValue input(fixture.system);
        input.kind = ValueKind::UInt8;
        input.unsignedInteger = 1;
        CHECK(PathWrite(fixture.objects, fixture.access, handle, path.View(), input));
    }

    fixture.With(handle, [](PackedCurveHolder& value) noexcept {
        CHECK(value.packed.KeyCount() == 3 && value.packed.Packed()[1] == 2);
        CHECK(value.packed.ConstantValue() == 0.0f && value.packed.Packed()[28] == 1);
        CHECK(value.list[0].KeyCount() == 2 && value.list[0].Packed()[1] == 3);
        CHECK(value.maybe.Value().KeyCount() == 0);
        CHECK(PackedTangentCurve::Validate(value.packed));
    });
}
