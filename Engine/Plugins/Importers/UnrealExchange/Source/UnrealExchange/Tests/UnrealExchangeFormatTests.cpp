#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeBundle.h>
#include <UnrealExchange/ExchangeDiagnostic.h>
#include <UnrealExchange/UnrealExchangeFormat.h>

// Unreal 교환 묶음 형식의 이름표·상수·진단 목록·실수 읽기 시험이다(Docs/Design/UnrealExchange.ko.md
// 3.2·3.3·10.3·10.4절). EngineTests가 이 플러그인을 켜고 Tests/를 모은다(BuildTargets 3.9절 BD40).
using namespace rocks;
namespace unreal_exchange_format_tests_file {

TEST(UnrealExchange_BundleFormatVersion) {
    REQUIRE(UnrealExchangeBundleFormatVersion == 1);
    CHECK(UnrealPathComponentMaxBytes == 64);
    CHECK(UnrealPathHashChars == 8);
    CHECK(UnrealPathMaxBytes == 256);
    CHECK(UnrealImportMaxDepth == 256);
}

// 교환 클래스 14개의 이름 왕복, 범위 밖 이름, 텍스처·머티리얼 계열 분류.
TEST(UnrealExchange_ClassNames) {
    uint32 textures = 0;
    uint32 materials = 0;
    for (uint32 index = 0; index < static_cast<uint32>(ExchangeClass::Unsupported); ++index) {
        const ExchangeClass type = static_cast<ExchangeClass>(index);
        CHECK(FindExchangeClass(ExchangeClassName(type)) == type);
        textures += IsExchangeTextureClass(type) ? 1 : 0;
        materials += IsExchangeMaterialFamily(type) ? 1 : 0;
    }

    CHECK(textures == 5 && materials == 4);
    CHECK(StringView(ExchangeClassName(ExchangeClass::StaticMesh)) == StringView("StaticMesh"));
    CHECK(StringView(ExchangeClassName(ExchangeClass::Unsupported)) == StringView("Unsupported"));
    CHECK(StringView(ExchangeClassName(static_cast<ExchangeClass>(200))) ==
          StringView("Unsupported"));
    CHECK(FindExchangeClass("staticmesh") == ExchangeClass::Unsupported);
    CHECK(FindExchangeClass("World") == ExchangeClass::Unsupported);
    CHECK(FindExchangeClass("") == ExchangeClass::Unsupported);
    CHECK(!IsExchangeMaterialFamily(ExchangeClass::MaterialParameterCollection));
}

// blob 배치 이름과 원소 크기(3.3절).
TEST(UnrealExchange_BlobLayoutNames) {
    const char* const names[] = {
        "u8", "u16", "u32", "i32", "f16", "f32", "f64", "names", "boneInfluences", "sparseVec3"};
    const uint32 sizes[] = {1, 2, 4, 4, 2, 4, 8, 0, 0, 0};
    for (uint32 index = 0; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
        const ExchangeBlobLayout layout = static_cast<ExchangeBlobLayout>(index);
        CHECK(StringView(ExchangeBlobLayoutName(layout)) == StringView(names[index]));
        CHECK(ExchangeBlobElementBytes(layout) == sizes[index]);
    }

    CHECK(StringView(ExchangeBlobLayoutName(static_cast<ExchangeBlobLayout>(99))) ==
          StringView("Unknown"));
}

// 세부 규칙 이름은 모두 다르고 "Unknown"이 아니다. 코드별 수준은 10.3절 표다.
TEST(UnrealExchange_DiagnosticNamesAndSeverity) {
    constexpr uint32 last = static_cast<uint32>(ExchangeIssue::ExporterError);
    for (uint32 index = 0; index <= last; ++index) {
        const StringView name(ExchangeIssueName(static_cast<ExchangeIssue>(index)));
        CHECK(name != StringView("Unknown"));
        for (uint32 other = 0; other < index; ++other) {
            CHECK(name != StringView(ExchangeIssueName(static_cast<ExchangeIssue>(other))));
        }
    }

    CHECK(StringView(ExchangeIssueName(static_cast<ExchangeIssue>(last + 1))) ==
          StringView("Unknown"));
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::CorruptBundle) == ExchangeSeverity::Error);
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::UnsupportedBundleVersion) ==
          ExchangeSeverity::Error);
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::UnrealPathTooLong) == ExchangeSeverity::Error);
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::UnrealPathCollision) == ExchangeSeverity::Error);
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::UnsupportedCycle) == ExchangeSeverity::Error);
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::MissingDependency) == ExchangeSeverity::Error);
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::UnknownField) == ExchangeSeverity::Warning);
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::UnsupportedAsset) == ExchangeSeverity::Warning);
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::MissingUnrealDependency) ==
          ExchangeSeverity::Warning);
    CHECK(ExchangeSeverityOf(AssetDiagnosticCode::UnrealPathRenamed) == ExchangeSeverity::Info);
    const AssetDiagnosticCode codes[] = {
        AssetDiagnosticCode::CorruptBundle,       AssetDiagnosticCode::UnsupportedBundleVersion,
        AssetDiagnosticCode::UnknownField,        AssetDiagnosticCode::UnsupportedAsset,
        AssetDiagnosticCode::UnrealPathRenamed,   AssetDiagnosticCode::UnrealPathTooLong,
        AssetDiagnosticCode::UnrealPathCollision, AssetDiagnosticCode::MissingUnrealDependency};
    const char* const codeNames[] = {"CorruptBundle",       "UnsupportedBundleVersion",
                                     "UnknownField",        "UnsupportedAsset",
                                     "UnrealPathRenamed",   "UnrealPathTooLong",
                                     "UnrealPathCollision", "MissingUnrealDependency"};
    for (usize index = 0; index < sizeof(codes) / sizeof(codes[0]); ++index) {
        CHECK(StringView(AssetDiagnosticName(codes[index])) == StringView(codeNames[index]));
    }
}

// 진단 목록: 수준·코드별 개수, 찾기, 상한(UnrealExchangeMaxDiagnostics)을 넘으면 개수만 셈, 할당
// 실패는 목록을 그대로 두고 그 오류.
TEST(UnrealExchange_DiagnosticList) {
    SystemAllocator allocator;
    {
        ExchangeDiagnostics diagnostics(allocator);
        ExchangeDiagnosticInput input;
        input.code = AssetDiagnosticCode::UnknownField;
        input.issue = ExchangeIssue::UnknownKey;
        input.text = "extra";
        REQUIRE(diagnostics.TryAdd(input));
        input.code = AssetDiagnosticCode::CorruptBundle;
        input.error = ErrorCode::InvalidArgument;
        input.asset = 3;
        REQUIRE(diagnostics.TryAdd(input));
        CHECK(diagnostics.Count(ExchangeSeverity::Warning) == 1);
        CHECK(diagnostics.Count(ExchangeSeverity::Error) == 1);
        CHECK(diagnostics.Count(ExchangeSeverity::Info) == 0);
        CHECK(diagnostics.Count(AssetDiagnosticCode::CorruptBundle) == 1);
        const ExchangeDiagnostic* found = diagnostics.Find(AssetDiagnosticCode::CorruptBundle);
        REQUIRE(found != nullptr);
        CHECK(found->asset == 3 && found->text.View() == StringView("extra"));
        CHECK(diagnostics.Find(AssetDiagnosticCode::UnrealPathRenamed) == nullptr);
        for (uint32 index = 0; index < UnrealExchangeMaxDiagnostics + 5; ++index) {
            REQUIRE(diagnostics.TryAdd(input));
        }

        CHECK(diagnostics.Items().Size() == UnrealExchangeMaxDiagnostics);
        CHECK(diagnostics.Dropped() == 7);
        diagnostics.Clear();
        CHECK(diagnostics.Items().IsEmpty() && diagnostics.Dropped() == 0);
    }

    {
        test::NFailAllocator failing(allocator);
        ExchangeDiagnostics diagnostics(failing);
        ExchangeDiagnosticInput input;
        input.code = AssetDiagnosticCode::UnknownField;
        input.text = "a key long enough to need an allocation";
        failing.FailOnAttempt(1);
        CHECK(diagnostics.TryAdd(input).Error() == ErrorCode::OutOfMemory);
        CHECK(diagnostics.Items().IsEmpty());
        failing.FailOnAttempt(2);
        CHECK(diagnostics.TryAdd(input).Error() == ErrorCode::OutOfMemory);
        CHECK(diagnostics.Items().IsEmpty());
        failing.DisableFailure();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 실수 읽기(3.2절): 숫자, "nan"·"inf"·"-inf" 문자열, 그 밖은 InvalidArgument.
TEST(UnrealExchange_ReadNumber) {
    SystemAllocator allocator;
    {
        JsonDocument document(allocator);
        REQUIRE(
            document
                .Parse("{\"a\": 0.5, \"b\": 3, \"c\": \"nan\", \"d\": \"inf\", \"e\": \"-inf\", "
                       "\"f\": \"NaN\", \"g\": true, \"h\": null, \"i\": 18446744073709551615}")
                .Succeeded());
        const JsonValue root = document.Root();
        double value = 0.0;
        REQUIRE(ReadExchangeNumber(root.Find("a"), value));
        CHECK(value == 0.5);
        REQUIRE(ReadExchangeNumber(root.Find("b"), value));
        CHECK(value == 3.0);
        REQUIRE(ReadExchangeNumber(root.Find("c"), value));
        CHECK(value != value);
        REQUIRE(ReadExchangeNumber(root.Find("d"), value));
        CHECK(value > 0.0 && value * 0.0 != 0.0);
        REQUIRE(ReadExchangeNumber(root.Find("e"), value));
        CHECK(value < 0.0 && value * 0.0 != 0.0);
        REQUIRE(ReadExchangeNumber(root.Find("i"), value));
        CHECK(value > 1.0e19);
        value = 7.0;
        CHECK(ReadExchangeNumber(root.Find("f"), value).Error() == ErrorCode::InvalidArgument);
        CHECK(ReadExchangeNumber(root.Find("g"), value).Error() == ErrorCode::InvalidArgument);
        CHECK(ReadExchangeNumber(root.Find("h"), value).Error() == ErrorCode::InvalidArgument);
        CHECK(ReadExchangeNumber(root.Find("missing"), value).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(value == 7.0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace unreal_exchange_format_tests_file
