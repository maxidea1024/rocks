#include <CurvesTestFixtures/CurveFixture.generated.h>
#include <Curves/CurveTypes.generated.h>
#include <Curves/PlainCurve.generated.h>
#include <Curves/TangentCurve.generated.h>
#include <CurvesTestFixtures/CurvesTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Curves/CurveKeyHandleTable.h>
#include <Object/ModuleRegistration.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <ObjectSerialization/Tests/GraphTestSupport.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextJson.h>
#include <TestSupport/Test.h>

// 커브 값 타입의 리플렉션 시험이다(Docs/Design/CurveData.ko.md 9절·16절, fixture
// CurvesTestFixtures/CurveFixture.h): 모든 자리(필드·Array 원소·Optional·Map 값·InstancedStruct
// 내용)의 그래프 왕복과 다시 저장 byte 같음, 상속 필드 ID(base 소유)·private 키 배열의 도구 이름
// keys·키 Atomic, JSON 왕복, 검증 hook(정렬·유한 시간)이 graph decode·대입 묶음·JSON import·경로
// 쓰기에서 어기는 값을 거부하고 값이 그대로인지, PlainCurveInterp에 없는 Cubic의 거부, 저작 키 wire
// 크기 실측(9.2절 — D8).
using namespace rocks;
using namespace rocks::reflection_text;
using curves_fixture::CurveHolder;

namespace {

TangentCurveKey Key(float time, float value) noexcept {
    TangentCurveKey key;
    key.time = time;
    key.value = value;
    return key;
}

struct Objects {
    Objects() noexcept
        : types(system), registration(types, system), objects(system, types, {0x4356, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::CurvesTestFixtures::RegisterModuleTypes(registration));
    }

    ~Objects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<CurveHolder> Make() noexcept {
        auto created = objects.NewObject<CurveHolder>(access, TypeIdOf<CurveHolder>());
        if (!created) {
            platform::Fatal("test object creation failed");
        }

        return Move(created.Value());
    }

    template <class Edit>
    void With(ObjectHandle object, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<CurveHolder>(access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    // 모든 자리에 고유한 키 시간(2.5 / 0.8125 / 7.25 / 11.5 / 13.5 / 17.5 — 변조 시험이 찾는다)을
    // 둔다.
    void Fill(ObjectHandle object) noexcept {
        With(object, [&](CurveHolder& value) noexcept {
            TangentCurveKey cubic = Key(2.5f, 4.0f);
            cubic.interp = CurveInterp::Cubic;
            cubic.tangentMode = CurveTangentMode::Break;
            cubic.tangentWeight = CurveTangentWeight::Leave;
            cubic.arriveTangent = -1.25f;
            cubic.leaveTangent = 3.0f;
            cubic.leaveTangentWeight = 0.625f;
            const TangentCurveKey keys[] = {Key(1.5f, 1.0f), cubic, Key(3.5f, -2.0f)};
            REQUIRE(value.tangent.TrySetKeys(keys));
            value.tangent.defaultValue = 0.5f;
            value.tangent.postExtrapolation = CurveExtrapolation::Oscillate;

            value.plain.interp = PlainCurveInterp::Constant;
            value.plain.preExtrapolation = CurveExtrapolation::Linear;
            REQUIRE(value.plain.TryAddKey(0.25f, 8.0f));
            REQUIRE(value.plain.TryAddKey(0.8125f, 9.0f));

            REQUIRE(value.tangents.TryPushBack(TangentCurve(system)));
            REQUIRE(value.tangents.TryPushBack(TangentCurve(system)));
            REQUIRE(value.tangents[1].TryAddKey(6.0f, 1.0f));
            REQUIRE(value.tangents[1].TryAddKey(7.25f, 2.0f));

            value.maybe.Emplace(PlainCurve(system));
            REQUIRE(value.maybe.Value().TryAddKey(10.0f, 1.0f));
            REQUIRE(value.maybe.Value().TryAddKey(11.5f, 1.0f));

            PlainCurve mapped(system);
            REQUIRE(mapped.TryAddKey(12.0f, 3.0f));
            REQUIRE(mapped.TryAddKey(13.5f, 4.0f));
            REQUIRE(value.byKey.Insert(int32{4}, Move(mapped)));

            REQUIRE(value.any.Emplace<TangentCurve>(types, system));
            REQUIRE(value.any.Get<TangentCurve>()->TryAddKey(16.0f, 5.0f));
            REQUIRE(value.any.Get<TangentCurve>()->TryAddKey(17.5f, 6.0f));
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

// Fill한 값이 그대로인지다.
void CheckFilled(const CurveHolder& value) noexcept {
    REQUIRE(value.tangent.KeyCount() == 3);
    const TangentCurveKey& cubic = value.tangent.Keys()[1];
    CHECK(cubic.time == 2.5f && cubic.value == 4.0f && cubic.interp == CurveInterp::Cubic &&
          cubic.tangentMode == CurveTangentMode::Break &&
          cubic.tangentWeight == CurveTangentWeight::Leave && cubic.arriveTangent == -1.25f &&
          cubic.leaveTangent == 3.0f && cubic.leaveTangentWeight == 0.625f);
    CHECK(value.tangent.defaultValue.HasValue() && value.tangent.defaultValue.Value() == 0.5f);
    CHECK(value.tangent.postExtrapolation == CurveExtrapolation::Oscillate);
    CHECK(value.plain.interp == PlainCurveInterp::Constant && value.plain.KeyCount() == 2 &&
          value.plain.preExtrapolation == CurveExtrapolation::Linear &&
          !value.plain.defaultValue.HasValue());
    REQUIRE(value.tangents.Size() == 2);
    CHECK(value.tangents[0].KeyCount() == 0 && value.tangents[1].Keys()[1].time == 7.25f);
    REQUIRE(value.maybe.HasValue());
    CHECK(value.maybe.Value().Keys()[1].time == 11.5f);
    REQUIRE(value.byKey.Find(4) != nullptr);
    CHECK(value.byKey.Find(4)->Keys()[1].time == 13.5f);
    REQUIRE(value.any.Get<TangentCurve>() != nullptr);
    CHECK(value.any.Get<TangentCurve>()->Keys()[1].time == 17.5f);
}

// haystack 안 needle 위치들의 수와 첫 위치다.
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

// 저장한 byte에서 float from의 유일한 자리를 to로 바꾼다(없거나 여럿이면 거짓).
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

// text의 유일한 from 자리를 to로 바꾼 문자열이다(없거나 여럿이면 빈 문자열).
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

// 스키마의 name 필드다(없으면 Fatal — fixture 고정 이름).
const FieldSchema& FieldNamed(const TypeSchema& schema, const char* name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == StringView(name)) {
            return field;
        }
    }

    platform::Fatal("curve schema field missing");
}

} // namespace

// 스키마: base 필드(defaultValue·pre·postExtrapolation)는 ScalarCurveBase가 소유하고(두 파생에서
// 같은 ID), private 키 배열의 도구 이름은 keys다. 키 struct는 Atomic이다.
TEST(CurveReflection_SchemaShape) {
    const TypeSchema& tangent = TypeOf<TangentCurve>();
    const TypeSchema& plain = TypeOf<PlainCurve>();
    CHECK(tangent.baseType == TypeIdOf<ScalarCurveBase>());
    CHECK(plain.baseType == TypeIdOf<ScalarCurveBase>());
    REQUIRE(tangent.fields.Size() == 4 && plain.fields.Size() == 5);
    const char* const baseFields[] = {"defaultValue", "preExtrapolation", "postExtrapolation"};
    for (const char* name : baseFields) {
        const Guid owned = FieldNamed(TypeOf<ScalarCurveBase>(), name).id;
        CHECK(FieldNamed(tangent, name).id == owned && FieldNamed(plain, name).id == owned);
    }

    const FieldSchema& keys = FieldNamed(tangent, "keys_");
    CHECK(StringView(keys.toolName.Data(), keys.toolName.Size()) == StringView("keys"));
    CHECK(FieldNamed(tangent, "keys_").id != FieldNamed(plain, "keys_").id);
    CHECK((TypeOf<TangentCurveKey>().flags & static_cast<uint32>(TypeFlags::Atomic)) != 0);
    CHECK((TypeOf<PlainCurveKey>().flags & static_cast<uint32>(TypeFlags::Atomic)) != 0);
}

// 모든 자리의 그래프 왕복과 다시 저장한 byte 같음.
TEST(CurveReflection_GraphRoundTrip) {
    Objects fixture;
    ObjectRegistry target(fixture.system, fixture.types, {0x4356, 2});
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
                target.Resolve<CurveHolder>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            CheckFilled(*value.Value());
        }

        auto again =
            SaveObjectGraph(fixture.system, target, target.OwnerAccess(), loaded.Value().Handle());
        REQUIRE(again);
        CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
    }

    REQUIRE(target.Shutdown(target.OwnerAccess()));
}

// graph decode의 hook: 저장 byte에서 자리마다 키 시간 하나를 앞 키보다 작게(또는 NaN으로) 바꾸면
// 로드가 InvalidArgument이고 대상 Registry에 아무것도 남지 않는다.
TEST(CurveReflection_DecodeRejectsUnsortedOrNonFiniteKeys) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    auto saved = SaveObjectGraph(fixture.system, fixture.objects, fixture.access, holder.Handle());
    REQUIRE(saved);
    const float times[] = {2.5f, 0.8125f, 7.25f, 11.5f, 13.5f, 17.5f};
    for (const float time : times) {
        for (int32 nonFinite = 0; nonFinite < 2; ++nonFinite) {
            Array<uint8> bytes(fixture.system);
            REQUIRE(bytes.TryAppend(saved.Value().View()));
            REQUIRE(
                PatchFloat(bytes, time,
                           nonFinite != 0 ? platform::PlatformMath::QuietNaN<float>() : -1000.0f));
            ObjectRegistry target(fixture.system, fixture.types, {0x4356, 3});
            auto loaded = LoadObjectGraph(fixture.system, target, fixture.types,
                                          target.OwnerAccess(), bytes.View());
            CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
            CHECK(target.Stats().liveObjects == 0);
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }
    }
}

// JSON 왕복(도구 이름 keys, enum은 이름, 비어 있는 기본값은 null)과, 자리마다 정렬을 깨는 JSON
// import 거부(InvalidStructValue·InvalidArgument — 값 그대로).
TEST(CurveReflection_JsonRoundTripAndHookRejection) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    StringBuilder<8192> text(fixture.system);
    REQUIRE(fixture.Export(holder.Handle(), text));
    CHECK(text.View().Contains("\"keys\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"Oscillate\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"Break\"", StringComparison::CaseSensitive));
    CHECK(!text.View().Contains("keys_", StringComparison::CaseSensitive));

    auto copy = fixture.Make();
    REQUIRE(fixture.Import(copy.Handle(), text.View()));
    fixture.With(copy.Handle(), [](CurveHolder& value) noexcept { CheckFilled(value); });
    StringBuilder<8192> again(fixture.system);
    REQUIRE(fixture.Export(copy.Handle(), again));
    CHECK(again.View() == text.View());

    const char* const times[] = {"2.5", "0.8125", "7.25", "11.5", "13.5", "17.5"};
    for (const char* time : times) {
        StringBuilder<8192> broken(fixture.system);
        ReplaceOnce(text.View(), StringView(time), StringView("-1000"), broken);
        REQUIRE(broken.View().ByteCount() != 0);
        CHECK(fixture.Import(copy.Handle(), broken.View()).Error() == ErrorCode::InvalidArgument);
        CHECK(fixture.diagnostics.Count() >= 1 &&
              fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidStructValue);
        fixture.With(copy.Handle(), [](CurveHolder& value) noexcept { CheckFilled(value); });
    }

    // PlainCurveInterp에는 Cubic이 없다(enum 이름 거부).
    CHECK(!fixture.Import(copy.Handle(), StringView("{\"plain\":{\"interp\":\"Cubic\"}}")));

    fixture.With(copy.Handle(), [](CurveHolder& value) noexcept { CheckFilled(value); });
}

// 대입 묶음(명령행): 도구 이름 keys로 키 칸을 쓴다. 정렬을 깨는 시간·Cubic 보간은 거부되고 묶음
// 전체가 게시되지 않는다.
TEST(CurveReflection_AssignmentsUseToolNameAndValidate) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    const TextAssignment valid[] = {Set("tangent.keys[1].value", "6"),
                                    Set("tangent.keys[2].time", "9")};
    REQUIRE(fixture.Apply(holder.Handle(), valid));
    fixture.With(holder.Handle(), [](CurveHolder& value) noexcept {
        CHECK(value.tangent.Keys()[1].value == 6.0f && value.tangent.Keys()[2].time == 9.0f);
    });

    const TextAssignment broken[] = {Set("tangent.keys[1].value", "7"),
                                     Set("tangents[1].keys[0].time", "8")};
    CHECK(fixture.Apply(holder.Handle(), broken).Error() == ErrorCode::InvalidArgument);
    CHECK(fixture.diagnostics.Count() >= 1 &&
          fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidStructValue);
    const TextAssignment interp[] = {Set("plain.interp", "Cubic")};
    CHECK(!fixture.Apply(holder.Handle(), interp));
    fixture.With(holder.Handle(), [](CurveHolder& value) noexcept {
        CHECK(value.tangent.Keys()[1].value == 6.0f);
        CHECK(value.tangents[1].Keys()[0].time == 6.0f);
        CHECK(value.plain.interp == PlainCurveInterp::Constant);
    });
}

// 경로 쓰기: hook struct(커브) 안의 키 시간 쓰기는 상자 경로로 검증된다 — 정렬을 지키면 게시되고
// 깨면 InvalidArgument(StructValidation)이며 그대로다. 자리마다(필드·원소·Optional·Map 값·
// InstancedStruct 내용) 같다.
TEST(CurveReflection_PathWritesValidate) {
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
        {"tangent.keys[1].time", 2.0f, 4.0f},     {"plain.keys[1].time", 0.5f, 0.0f},
        {"tangents[1].keys[1].time", 7.0f, 5.0f}, {"maybe.keys[0].time", 9.0f, 12.0f},
        {"byKey[4].keys[1].time", 13.0f, 11.0f},  {"any.keys[0].time", 15.0f, 18.0f}};
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

    fixture.With(handle, [](CurveHolder& value) noexcept {
        CHECK(value.tangent.Keys()[1].time == 2.0f && value.plain.Keys()[1].time == 0.5f);
        CHECK(value.tangents[1].Keys()[1].time == 7.0f &&
              value.maybe.Value().Keys()[0].time == 9.0f);
        CHECK(value.byKey.Find(4)->Keys()[1].time == 13.0f &&
              value.any.Get<TangentCurve>()->Keys()[0].time == 15.0f);
    });
}

// 저작 키 wire 크기 실측(설계 9.2절, D8): 키 1,000개 커브의 저장 크기 − 빈 커브 저장 크기를 키 수로
// 나눈 키당 byte다. TangentCurveKey(float 6·enum 3)는 352 byte, PlainCurveKey(float 2)는 116 byte다
// (UE 수동 이진 키는 27·8 byte). 값은 기본값 생략 없는 RecordList wire 그대로다.
TEST(CurveReflection_KeyWireSize) {
    Objects fixture;
    auto measure = [&](usize count, bool tangent) noexcept {
        auto holder = fixture.Make();
        fixture.With(holder.Handle(), [&](CurveHolder& value) noexcept {
            for (usize index = 0; index < count; ++index) {
                const float time = static_cast<float>(index) * 0.03125f;
                const float sample = static_cast<float>(index % 17) * 0.5f;
                if (tangent) {
                    TangentCurveKey key = Key(time, sample);
                    key.interp = CurveInterp::Cubic;
                    key.arriveTangent = 0.25f;
                    key.leaveTangent = 0.25f;
                    REQUIRE(value.tangent.TryAddKey(time, sample));
                    REQUIRE(value.tangent.TrySetKeyInterp(index, CurveInterp::Cubic));
                    REQUIRE(value.tangent.TrySetKeyTangents(index, 0.25f, 0.25f));
                } else {
                    REQUIRE(value.plain.TryAddKey(time, sample));
                }
            }
        });
        auto saved =
            SaveObjectGraph(fixture.system, fixture.objects, fixture.access, holder.Handle());
        CHECK(saved);
        return saved ? saved.Value().Size() : usize{0};
    };

    const usize emptyTangent = measure(0, true);
    const usize tangentPerKey = (measure(1000, true) - emptyTangent) / 1000;
    const usize plainPerKey = (measure(1000, false) - measure(0, false)) / 1000;
    CHECK(tangentPerKey == 352);
    CHECK(plainPerKey == 116);
}

// 리플렉션 경로 쓰기는 표를 거치지 않은 수정이다: 키 핸들 표는 다음 호출에서 StaleHandle이고(설계
// 8절 — 사본 비트 비교), TryRebuild 뒤 다시 쓸 수 있다(CV7).
TEST(CurveReflection_PathWriteMakesHandleTableStale) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    fixture.Fill(handle);
    CurveKeyHandleTable<TangentCurve> table(fixture.system);
    fixture.With(handle,
                 [&](CurveHolder& value) noexcept { REQUIRE(table.TryRebuild(value.tangent)); });
    PropertyPathBuffer path;
    REQUIRE(ParsePropertyPath(fixture.objects, fixture.access, handle,
                              StringView("tangent.keys[1].value"), path));
    FieldValue input(fixture.system);
    input.kind = ValueKind::Float32;
    input.float32 = 42.0f;
    REQUIRE(PathWrite(fixture.objects, fixture.access, handle, path.View(), input));
    fixture.With(handle, [&](CurveHolder& value) noexcept {
        auto stale = table.TryGetHandle(value.tangent, 0);
        CHECK(!stale && stale.Error() == ErrorCode::StaleHandle);
        REQUIRE(table.TryRebuild(value.tangent));
        CHECK(table.TryGetHandle(value.tangent, 1));
    });
}

// 대입 묶음(명령행)으로 커브를 바꾸면 표 사본과 달라져 다음 표 호출이 StaleHandle이다. 거부된
// 묶음은 커브를 바꾸지 않으므로 표가 그대로 맞는다.
TEST(CurveReflection_AssignmentsMakeHandleTableStale) {
    Objects fixture;
    auto holder = fixture.Make();
    const ObjectHandle handle = holder.Handle();
    fixture.Fill(handle);
    CurveKeyHandleTable<TangentCurve> table(fixture.system);
    fixture.With(handle,
                 [&](CurveHolder& value) noexcept { REQUIRE(table.TryRebuild(value.tangent)); });
    const TextAssignment rejected[] = {Set("tangent.keys[1].value", "7"),
                                       Set("tangent.keys[0].time", "100")};
    CHECK(!fixture.Apply(handle, rejected));
    fixture.With(handle,
                 [&](CurveHolder& value) noexcept { CHECK(table.TryGetHandle(value.tangent, 1)); });
    const TextAssignment applied[] = {Set("tangent.keys[1].tangentMode", "User")};
    REQUIRE(fixture.Apply(handle, applied));
    fixture.With(handle, [&](CurveHolder& value) noexcept {
        auto stale = table.TryGetHandle(value.tangent, 1);
        CHECK(!stale && stale.Error() == ErrorCode::StaleHandle);
        CHECK(table.IsStale());
    });
}
