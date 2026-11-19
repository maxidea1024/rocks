#include <ReflectionTestFixtures/BytesFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <Object/ModuleRegistration.h>
#include <Object/PropertyPath.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextJson.h>
#include <ReflectionText/TextValue.h>
#include <TestSupport/Test.h>

// 바이트 배열(`Array<uint8>`)의 명령행·JSON 표기 시험이다(Docs/Design/CurveData.ko.md D18 — CV4,
// fixture ReflectionTestFixtures/BytesFixture.h): 통째 값은 명령행·JSON 모두 RFC 4648 Base64 문자열
// 하나(빈 문자열 = 빈 배열)이고, 원소는 지금처럼 경로 `payload[i]=v`다. 잘못된 Base64는
// InvalidValue, 필드 MaxElements를 넘는 길이는 OutOfRange, JSON 숫자 배열은 InvalidValue다.
using namespace rocks;
using namespace rocks::reflection_text;
using reflection_fixture::BytesHolder;

namespace {

TextAssignment Set(const char* path, const char* value, uint32 source = 0) noexcept {
    TextAssignment assignment;
    assignment.kind = AssignmentKind::Set;
    assignment.path = StringView(path);
    assignment.value = StringView(value);
    assignment.sourceIndex = source;
    return assignment;
}

struct Objects {
    Objects() noexcept
        : types(system), registration(types, system), objects(system, types, {0x5442, 1}),
          access(objects.OwnerAccess()), diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~Objects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    RootRef<BytesHolder> Make() noexcept {
        auto created =
            objects.NewObject<BytesHolder>(access, GeneratedType<BytesHolder>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<BytesHolder>{};
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

    template <class Edit>
    void With(ObjectHandle object, Edit&& edit) noexcept {
        auto borrowed = objects.Resolve<BytesHolder>(access, object);
        REQUIRE(borrowed);
        auto value = borrowed.Value().Get();
        REQUIRE(value);
        edit(*value.Value());
    }

    bool FirstDiagnostic(TextDiagnosticCode code) const noexcept {
        return diagnostics.Count() >= 1 && diagnostics.Get(0).code == code;
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    reflection_text::TextDiagnostics diagnostics;
    TextCodecOptions options;
};

bool SameBytes(const Array<uint8>& bytes, const uint8* expected, usize count) noexcept {
    if (bytes.Size() != count) {
        return false;
    }

    for (usize index = 0; index < count; ++index) {
        if (bytes[index] != expected[index]) {
            return false;
        }
    }

    return true;
}

} // namespace

// 명령행: 통째 대입은 Base64(빈 문자열 = 빈 배열), 원소 대입은 경로 그대로다. 잘못된 Base64·한도
// 초과는 진단이고 값은 그대로다.
TEST(TextBytes_CommandLineUsesBase64) {
    Objects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    // "AAECAw==" = {0, 1, 2, 3}, "/w==" = {255}.
    const TextAssignment whole[] = {Set("payload", "AAECAw=="), Set("blob.data", "/w==", 1)};
    REQUIRE(fixture.Apply(object, whole));
    const uint8 first[] = {0, 1, 2, 3};
    const uint8 single[] = {255};
    fixture.With(object, [&](BytesHolder& value) noexcept {
        CHECK(SameBytes(value.payload, first, 4));
        CHECK(SameBytes(value.blob.data, single, 1));
    });

    const TextAssignment element[] = {Set("payload[1]", "9")};
    REQUIRE(fixture.Apply(object, element));
    const uint8 changed[] = {0, 9, 2, 3};
    fixture.With(object,
                 [&](BytesHolder& value) noexcept { CHECK(SameBytes(value.payload, changed, 4)); });

    const TextAssignment empty[] = {Set("payload", "")};
    REQUIRE(fixture.Apply(object, empty));
    fixture.With(object, [](BytesHolder& value) noexcept { CHECK(value.payload.Size() == 0); });

    // 숫자 목록·패딩 없는 Base64·공백은 Base64가 아니다.
    const char* const invalid[] = {"1,2,3", "AAECAw", "AAEC Aw==", "AAECAw=!"};
    for (const char* text : invalid) {
        const TextAssignment bad[] = {Set("payload", text)};
        CHECK(!fixture.Apply(object, bad));
        CHECK(fixture.FirstDiagnostic(TextDiagnosticCode::InvalidValue));
    }

    // small은 MaxElements 8: 9 byte("AAAAAAAAAAAA")는 OutOfRange, 8 byte("AAAAAAAAAAA=")는 된다.
    const TextAssignment over[] = {Set("small", "AAAAAAAAAAAA")};
    CHECK(!fixture.Apply(object, over));
    CHECK(fixture.FirstDiagnostic(TextDiagnosticCode::OutOfRange));
    const TextAssignment fits[] = {Set("small", "AAAAAAAAAAA=")};
    REQUIRE(fixture.Apply(object, fits));
    fixture.With(object, [](BytesHolder& value) noexcept { CHECK(value.small.Size() == 8); });

    // 정적 배열 `uint8 x[4]`은 지금처럼 쉼표 목록이다.
    const TextAssignment fixed[] = {Set("fixed", "1,2,3,4")};
    REQUIRE(fixture.Apply(object, fixed));
    fixture.With(object, [](BytesHolder& value) noexcept { CHECK(value.fixed[3] == 4); });
}

// 명령행 표기 왕복: 읽은 값을 다시 쓰면 같은 Base64 텍스트다.
TEST(TextBytes_FormatRoundTrips) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    {
        auto lease = types.Acquire(GeneratedType<BytesHolder>::Schema().id);
        REQUIRE(lease);
        TextCodecOptions options;
        TextDiagnostics diagnostics(allocator);
        TextCodecContext context(types, options, diagnostics, allocator);
        TextPath path;
        auto resolved =
            ResolveTextPath(context, lease.Value(), "payload", TextSource::Assignment(0), path);
        REQUIRE(resolved && resolved.Value());
        const char* const texts[] = {"", "AA==", "AAE=", "AAEC", "/+/+", "SGVsbG8sIGJ5dGVzIQ=="};
        for (const char* text : texts) {
            TextValue value(allocator);
            auto parsed = ParseTextValue(context, path.slot, StringView(text),
                                         TextSource::Assignment(0), "p", value);
            REQUIRE(parsed && parsed.Value());
            String formatted(allocator);
            REQUIRE(FormatTextValue(context, path.slot, value, formatted));
            CHECK(formatted.View() == StringView(text));
        }
    }

    CHECK(registration.Unregister());
    CHECK(types.Shutdown());
}

// JSON: 바이트 배열은 Base64 문자열이고 왕복한다. 숫자 배열·숫자는 InvalidValue이고 아무 필드도
// 바뀌지 않는다.
TEST(TextBytes_JsonUsesBase64) {
    Objects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    fixture.With(object, [](BytesHolder& value) noexcept {
        for (int32 index = 0; index < 5; ++index) {
            REQUIRE(value.payload.TryPushBack(static_cast<uint8>(index * 50)));
        }

        REQUIRE(value.blob.data.TryPushBack(uint8{7}));
        value.plain = 3;
    });

    StringBuilder<2048> text(fixture.system);
    REQUIRE(fixture.Export(object, text));
    // {0, 50, 100, 150, 200} = "ADJklsg=", {7} = "Bw==".
    CHECK(text.View().Contains("\"ADJklsg=\"", StringComparison::CaseSensitive));
    CHECK(text.View().Contains("\"Bw==\"", StringComparison::CaseSensitive));

    auto copy = fixture.Make();
    REQUIRE(fixture.Import(copy.Handle(), text.View()));
    StringBuilder<2048> again(fixture.system);
    REQUIRE(fixture.Export(copy.Handle(), again));
    CHECK(again.View() == text.View());

    CHECK(!fixture.Import(copy.Handle(), "{\"plain\": 9, \"payload\": [1, 2]}"));
    CHECK(fixture.FirstDiagnostic(TextDiagnosticCode::InvalidValue));
    CHECK(!fixture.Import(copy.Handle(), "{\"plain\": 9, \"payload\": \"not base64\"}"));
    CHECK(fixture.FirstDiagnostic(TextDiagnosticCode::InvalidValue));
    fixture.With(copy.Handle(), [](BytesHolder& value) noexcept {
        CHECK(value.plain == 3 && value.payload.Size() == 5);
    });
}

namespace {

// size 바이트를 넘는 할당을 거부하는 allocator다(긴 텍스트 값이 큰 버퍼를 먼저 잡는지 본다).
class CappedAllocator final : public Allocator {
public:
    CappedAllocator(Allocator& inner, usize limit) noexcept : inner_(inner), limit_(limit) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (bytes > limit_) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

private:
    Allocator& inner_;
    usize limit_;
};

} // namespace

// 원소 수 한도는 풀기·할당 전에 길이와 패딩으로 본다(CV4 리뷰 5): MaxElements 8인 small에 300,000자
// Base64(225,000 byte)를 주면, 대상 allocator가 128 KiB 넘는 할당을 거부해도 OutOfMemory가 아니라
// OutOfRange 진단이다. 길이가 4의 배수가 아닌 입력은 할당 없이 InvalidValue다.
TEST(TextBytes_LengthIsCheckedBeforeDecoding) {
    SystemAllocator system;
    // 한도는 풀 결과(225,000 byte)보다 작고 Registry의 가장 큰 할당(GC 페이지 약 68 KiB — GC 설계
    // 6절)보다 크다.
    CappedAllocator capped(system, 128 * 1024);
    TypeRegistry types(system);
    ModuleRegistration registration(types, system);
    REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    {
        ObjectRegistry objects(capped, types, {0x5443, 1});
        const ObjectAccessContext access = objects.OwnerAccess();
        auto created = objects.NewObject<BytesHolder>(access, TypeIdOf<BytesHolder>());
        REQUIRE(created);
        RootRef<BytesHolder> holder = Move(created.Value());
        String json(system);
        REQUIRE(json.TryAppend("{\"small\": \""));
        for (int32 index = 0; index < 300000; ++index) {
            REQUIRE(json.TryAppend("A"));
        }

        REQUIRE(json.TryAppend("\"}"));
        JsonDocument document(system);
        REQUIRE(document.Parse(json.View()).Succeeded());
        TextDiagnostics diagnostics(system);
        TextCodecOptions options;
        auto imported =
            ImportJson(document.Root(), AssignmentTarget::Live(objects, access, holder.Handle()),
                       options, diagnostics);
        CHECK(!imported && imported.Error() != ErrorCode::OutOfMemory);
        CHECK(diagnostics.Count() >= 1 &&
              diagnostics.Get(0).code == TextDiagnosticCode::OutOfRange);

        diagnostics.Clear();
        JsonDocument odd(system);
        REQUIRE(odd.Parse("{\"small\": \"AAAAA\"}").Succeeded());
        auto rejected =
            ImportJson(odd.Root(), AssignmentTarget::Live(objects, access, holder.Handle()),
                       options, diagnostics);
        CHECK(!rejected);
        CHECK(diagnostics.Count() >= 1 &&
              diagnostics.Get(0).code == TextDiagnosticCode::InvalidValue);
        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(registration.Unregister());
    CHECK(types.Shutdown());
}
