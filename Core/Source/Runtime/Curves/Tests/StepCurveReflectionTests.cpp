#include <CurvesTestFixtures/CurveFixture.generated.h>
#include <CurvesTestFixtures/CurvesTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
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

// 계단 커브 셋의 리플렉션 시험이다(Docs/Design/CurveData.ko.md 9절·16절 — CV6, fixture
// CurvesTestFixtures/CurveFixture.h의 StepCurveHolder): 필드·Array 원소·Optional·Map 값·
// InstancedStruct 내용 자리의 그래프 왕복(Name 표시 철자·대소문자만 다른 Name의 같음·String 값
// 보존)과 다시 저장 byte 같음, 도구 이름 keys·키 Atomic, JSON 왕복, 검증 hook(정렬·유한 시간)이
// graph decode·JSON import·대입 묶음·경로 쓰기에서 어기는 값을 거부하고 값이 그대로인지.
using namespace rocks;
using namespace rocks::reflection_text;
using curves_fixture::StepCurveHolder;

namespace {

RegistryOptions WithNames(NamePool& pool) noexcept {
    RegistryOptions options;
    options.names = &pool;
    return options;
}

struct Objects {
    Objects() noexcept
        : types(system), registration(types, system), names(system),
          objects(system, types, {0x5343, 1}, WithNames(names)), access(objects.OwnerAccess()),
          diagnostics(system) {
        REQUIRE(names.Initialize());
        REQUIRE(generated::CurvesTestFixtures::RegisterModuleTypes(registration));
        options.names = &names;
    }

    ~Objects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(names.Shutdown());
        CHECK(types.Shutdown());
    }

    RootRef<StepCurveHolder> Make() noexcept {
        auto created = objects.NewObject<StepCurveHolder>(access, TypeIdOf<StepCurveHolder>());
        if (!created) {
            platform::Fatal("test object creation failed");
        }

        return Move(created.Value());
    }

    Name Intern(const char* text) noexcept {
        auto interned = names.TryIntern(StringView(text));
        if (!interned) {
            platform::Fatal("test name intern failed");
        }

        return interned.Value();
    }

    template <class Edit>
    void With(ObjectHandle object, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<StepCurveHolder>(access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    // 자리마다 고유한 키 시간(3.25 / 6.25 / 8.75 / 12.5 / 16.75 / 20.5 / 24.5 — 변조 시험이
    // 찾는다)을 둔다. InstancedStruct 자리에는 대소문자만 다른 Name 두 키("walk"·"WALK")를 둔다.
    void Fill(ObjectHandle object) noexcept {
        const Name walk = Intern("Walk");
        const Name run = Intern("RUN_3");
        With(object, [&](StepCurveHolder& value) noexcept {
            REQUIRE(value.integers.TryAddKey(1.0f, -4));
            REQUIRE(value.integers.TryAddKey(3.25f, 9));
            value.integers.defaultValue = 3;
            value.integers.useDefaultBeforeFirstKey = true;
            REQUIRE(value.names.TryAddKey(5.0f, walk));
            REQUIRE(value.names.TryAddKey(6.25f, run));
            value.names.defaultValue = Name{};
            REQUIRE(value.strings.TryAddKey(7.0f, "Intro \xEC\x95\x88\xEB\x85\x95"));
            REQUIRE(value.strings.TryAddKey(8.75f, ""));
            String label(system);
            REQUIRE(label.TryAssign("none"));
            value.strings.defaultValue.Emplace(Move(label));
            REQUIRE(value.stringList.TryPushBack(StringStepCurve(system)));
            REQUIRE(value.stringList[0].TryAddKey(11.0f, "a"));
            REQUIRE(value.stringList[0].TryAddKey(12.5f, "b"));
            value.maybeNames.Emplace(NameStepCurve(system));
            REQUIRE(value.maybeNames.Value().TryAddKey(15.0f, walk));
            REQUIRE(value.maybeNames.Value().TryAddKey(16.75f, walk));
            IntegerStepCurve mapped(system);
            REQUIRE(mapped.TryAddKey(19.0f, 1));
            REQUIRE(mapped.TryAddKey(20.5f, 2));
            REQUIRE(value.byKey.Insert(int32{7}, Move(mapped)));
            REQUIRE(value.anyNames.Emplace<NameStepCurve>(types, system));
            REQUIRE(value.anyNames.Get<NameStepCurve>()->TryAddKey(22.0f, Intern("walk")));
            REQUIRE(value.anyNames.Get<NameStepCurve>()->TryAddKey(24.5f, Intern("WALK")));
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
    NamePool names;
    ObjectRegistry objects;
    ObjectAccessContext access;
    TextDiagnostics diagnostics;
    TextCodecOptions options;
};

bool DisplayIs(NamePool& pool, Name token, StringView expected) noexcept {
    auto lease = pool.Acquire(token);
    return lease && lease.Value().View() == expected;
}

// Fill한 값이 그대로인지다(pool은 그 Registry의 pool).
void CheckFilled(const StepCurveHolder& value, NamePool& pool) noexcept {
    REQUIRE(value.integers.KeyCount() == 2);
    CHECK(value.integers.Keys()[1].time == 3.25f && value.integers.Keys()[1].value == 9);
    CHECK(value.integers.defaultValue.HasValue() && value.integers.defaultValue.Value() == 3);
    CHECK(value.integers.useDefaultBeforeFirstKey);
    REQUIRE(value.names.KeyCount() == 2);
    CHECK(DisplayIs(pool, value.names.Keys()[0].value, "Walk"));
    CHECK(DisplayIs(pool, value.names.Keys()[1].value, "RUN_3"));
    CHECK(value.names.Keys()[1].value.GetNumber() == 3);
    CHECK(value.names.defaultValue.HasValue() && value.names.defaultValue.Value().IsNone());
    REQUIRE(value.strings.KeyCount() == 2);
    CHECK(value.strings.Keys()[0].value.View() == StringView("Intro \xEC\x95\x88\xEB\x85\x95"));
    CHECK(value.strings.Keys()[1].value.View().ByteCount() == 0);
    CHECK(value.strings.defaultValue.HasValue() &&
          value.strings.defaultValue.Value().View() == StringView("none"));
    REQUIRE(value.stringList.Size() == 1 && value.maybeNames.HasValue() &&
            value.byKey.Find(7) != nullptr);
    CHECK(value.stringList[0].Keys()[1].time == 12.5f);
    CHECK(value.maybeNames.Value().Keys()[1].time == 16.75f);
    CHECK(value.byKey.Find(7)->Keys()[1].time == 20.5f);
    const NameStepCurve* const any = value.anyNames.Get<NameStepCurve>();
    REQUIRE(any != nullptr && any->KeyCount() == 2);
    CHECK(any->Keys()[1].time == 24.5f);
    // 대소문자만 다른 Name은 같고(Name 규칙) 표시 철자는 그대로다.
    CHECK(any->Keys()[0].value == any->Keys()[1].value);
    CHECK(DisplayIs(pool, any->Keys()[0].value, "walk"));
    CHECK(DisplayIs(pool, any->Keys()[1].value, "WALK"));
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

    platform::Fatal("step curve schema field missing");
}

} // namespace

// 스키마: 키 배열의 도구 이름은 keys이고 키 struct는 Atomic이다.
TEST(StepCurveReflection_SchemaShape) {
    const char* const toolName = "keys";
    const TypeSchema* const curves[] = {&TypeOf<IntegerStepCurve>(), &TypeOf<NameStepCurve>(),
                                        &TypeOf<StringStepCurve>()};
    for (const TypeSchema* schema : curves) {
        const FieldSchema& keys = FieldNamed(*schema, "keys_");
        CHECK(StringView(keys.toolName.Data(), keys.toolName.Size()) == StringView(toolName));
    }

    CHECK((TypeOf<IntegerStepKey>().flags & static_cast<uint32>(TypeFlags::Atomic)) != 0);
    CHECK((TypeOf<NameStepKey>().flags & static_cast<uint32>(TypeFlags::Atomic)) != 0);
    CHECK((TypeOf<StringStepKey>().flags & static_cast<uint32>(TypeFlags::Atomic)) != 0);
}

// 모든 자리의 그래프 왕복(다른 pool로 intern — 표시 철자·번호 보존)과 다시 저장한 byte 같음.
TEST(StepCurveReflection_GraphRoundTrip) {
    Objects fixture;
    NamePool targetNames(fixture.system);
    REQUIRE(targetNames.Initialize());
    {
        ObjectRegistry target(fixture.system, fixture.types, {0x5343, 2}, WithNames(targetNames));
        {
            auto holder = fixture.Make();
            fixture.Fill(holder.Handle());
            auto saved =
                SaveObjectGraph(fixture.system, fixture.objects, fixture.access, holder.Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(fixture.system, target, fixture.types,
                                          target.OwnerAccess(), saved.Value().View());
            REQUIRE(loaded);
            {
                auto borrowed =
                    target.Resolve<StepCurveHolder>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                auto value = borrowed.Value().Get();
                REQUIRE(value);
                CheckFilled(*value.Value(), targetNames);
            }

            auto again = SaveObjectGraph(fixture.system, target, target.OwnerAccess(),
                                         loaded.Value().Handle());
            REQUIRE(again);
            CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
        }

        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(targetNames.Shutdown());
}

// graph decode의 hook: 자리마다 키 시간 하나를 앞 키보다 작게(또는 NaN으로) 바꾸면 로드가
// InvalidArgument이고 대상 Registry에 아무것도 남지 않는다.
TEST(StepCurveReflection_DecodeRejectsUnsortedOrNonFiniteKeys) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    auto saved = SaveObjectGraph(fixture.system, fixture.objects, fixture.access, holder.Handle());
    REQUIRE(saved);
    NamePool targetNames(fixture.system);
    REQUIRE(targetNames.Initialize());
    const float times[] = {3.25f, 6.25f, 8.75f, 12.5f, 16.75f, 20.5f, 24.5f};
    for (const float time : times) {
        for (int32 nonFinite = 0; nonFinite < 2; ++nonFinite) {
            Array<uint8> bytes(fixture.system);
            REQUIRE(bytes.TryAppend(saved.Value().View()));
            REQUIRE(
                PatchFloat(bytes, time,
                           nonFinite != 0 ? platform::PlatformMath::QuietNaN<float>() : -1000.0f));
            ObjectRegistry target(fixture.system, fixture.types, {0x5343, 3},
                                  WithNames(targetNames));
            auto loaded = LoadObjectGraph(fixture.system, target, fixture.types,
                                          target.OwnerAccess(), bytes.View());
            CHECK(!loaded && loaded.Error() == ErrorCode::InvalidArgument);
            CHECK(target.Stats().liveObjects == 0);
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }
    }

    CHECK(targetNames.Shutdown());
}

// JSON 왕복(도구 이름 keys, Name·String 값)과, 자리마다 정렬을 깨는 JSON import 거부
// (InvalidStructValue·InvalidArgument — 값 그대로).
TEST(StepCurveReflection_JsonRoundTripAndHookRejection) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    StringBuilder<8192> text(fixture.system);
    REQUIRE(fixture.Export(holder.Handle(), text));
    CHECK(text.View().Contains("\"keys\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"RUN_3\"", StringComparison::CaseSensitive));
    CHECK(!text.View().Contains("keys_", StringComparison::CaseSensitive));

    auto copy = fixture.Make();
    REQUIRE(fixture.Import(copy.Handle(), text.View()));
    fixture.With(copy.Handle(),
                 [&](StepCurveHolder& value) noexcept { CheckFilled(value, fixture.names); });
    StringBuilder<8192> again(fixture.system);
    REQUIRE(fixture.Export(copy.Handle(), again));
    CHECK(again.View() == text.View());

    const char* const times[] = {"3.25", "6.25", "8.75", "12.5", "16.75", "20.5", "24.5"};
    for (const char* time : times) {
        StringBuilder<8192> broken(fixture.system);
        ReplaceOnce(text.View(), StringView(time), StringView("-1000"), broken);
        REQUIRE(broken.View().ByteCount() != 0);
        CHECK(fixture.Import(copy.Handle(), broken.View()).Error() == ErrorCode::InvalidArgument);
        CHECK(fixture.diagnostics.Count() >= 1 &&
              fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidStructValue);
        fixture.With(copy.Handle(),
                     [&](StepCurveHolder& value) noexcept { CheckFilled(value, fixture.names); });
    }
}

// 대입 묶음(명령행): 도구 이름 keys로 키 칸을 쓴다. 정렬을 깨는 시간은 거부되고 묶음 전체가
// 게시되지 않는다.
TEST(StepCurveReflection_AssignmentsUseToolNameAndValidate) {
    Objects fixture;
    auto holder = fixture.Make();
    fixture.Fill(holder.Handle());
    TextAssignment valid[2];
    valid[0].kind = AssignmentKind::Set;
    valid[0].path = StringView("integers.keys[0].value");
    valid[0].value = StringView("11");
    valid[1].kind = AssignmentKind::Set;
    valid[1].path = StringView("strings.keys[1].value");
    valid[1].value = StringView("Outro");
    REQUIRE(fixture.Apply(holder.Handle(), valid));
    TextAssignment broken[2];
    broken[0].kind = AssignmentKind::Set;
    broken[0].path = StringView("integers.keys[0].value");
    broken[0].value = StringView("12");
    broken[1].kind = AssignmentKind::Set;
    broken[1].path = StringView("names.keys[1].time");
    broken[1].value = StringView("1");
    CHECK(fixture.Apply(holder.Handle(), broken).Error() == ErrorCode::InvalidArgument);
    CHECK(fixture.diagnostics.Count() >= 1 &&
          fixture.diagnostics.Get(0).code == TextDiagnosticCode::InvalidStructValue);
    fixture.With(holder.Handle(), [](StepCurveHolder& value) noexcept {
        CHECK(value.integers.Keys()[0].value == 11);
        CHECK(value.strings.Keys()[1].value.View() == StringView("Outro"));
        CHECK(value.names.Keys()[1].time == 6.25f);
    });
}

// 경로 쓰기: 커브 안의 키 시간 쓰기는 상자 경로로 검증된다 — 정렬을 지키면 게시되고 깨면
// InvalidArgument(StructValidation)이며 그대로다(필드·원소·Optional·Map 값).
TEST(StepCurveReflection_PathWritesValidate) {
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
        {"integers.keys[1].time", 2.0f, 0.5f},     {"names.keys[0].time", 4.0f, 7.0f},
        {"strings.keys[1].time", 9.0f, 6.0f},      {"stringList[0].keys[0].time", 10.0f, 13.0f},
        {"maybeNames.keys[1].time", 17.0f, 14.0f}, {"byKey[7].keys[0].time", 18.0f, 21.0f},
        {"anyNames.keys[1].time", 25.0f, 21.5f}};
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

    fixture.With(handle, [](StepCurveHolder& value) noexcept {
        CHECK(value.integers.Keys()[1].time == 2.0f && value.names.Keys()[0].time == 4.0f);
        CHECK(value.strings.Keys()[1].time == 9.0f && value.stringList[0].Keys()[0].time == 10.0f);
        CHECK(value.maybeNames.Value().Keys()[1].time == 17.0f &&
              value.byKey.Find(7)->Keys()[0].time == 18.0f);
        CHECK(value.anyNames.Get<NameStepCurve>()->Keys()[1].time == 25.0f);
    });
}
