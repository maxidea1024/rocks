#include "ExchangeTestSupport.h"

#include <Core/FileError.h>
#include <TestSupport/Test.h>

// Unreal 교환 묶음 리더의 정상 읽기와 스키마 변조 시험이다(Docs/Design/UnrealExchange.ko.md 3절,
// 10.4절 2–11번, 12.1절): 저장소 fixture(실제·메모리 파일 시스템), bundle.json 묶음 수준 오류(버전·
// 구문·BOM·필수 키·타입·정렬·objectPath·file·redirect·파일 실패), 에셋 JSON 에셋 수준 오류, 모르는
// 키 경고.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_bundle_tests_file {

// fixture 에셋 번호(objectPath 순)다.
constexpr uint32 SkeletonAsset = 0;
constexpr uint32 SkinnedMeshAsset = 1;
constexpr uint32 FunctionAsset = 2;
constexpr uint32 WorldAsset = 3;
constexpr uint32 InstanceAsset = 4;
constexpr uint32 MaterialAsset = 5;
constexpr uint32 RockMeshAsset = 6;
constexpr uint32 CollisionMeshAsset = 7;
constexpr uint32 SharedTextureAsset = 8;
constexpr uint32 RockTextureAsset = 9;
constexpr uint32 FixtureAssetCount = 10;

Result<void> ReadFixture(MemoryFiles& files, ExchangeBundle& bundle,
                         ExchangeDiagnostics& diagnostics, bool verify = true) noexcept {
    ExchangeReadOptions options;
    options.verifyBlobs = verify;
    return ReadExchangeBundle(files, FixtureDirectory, options, bundle, diagnostics);
}

// fixture 안 relative 파일의 from 한 곳을 to로 바꾼다.
bool EditFixture(Allocator& allocator, MemoryFiles& files, StringView relative, StringView from,
                 StringView to) noexcept {
    StringBuilder<256> path(allocator);
    return FixtureFile(relative, path) && files.ReplaceOnce(path.View(), from, to);
}

// path 파일 앞에 UTF-8 BOM을 붙인다.
bool PrependBom(Allocator& allocator, MemoryFiles& files, StringView path) noexcept {
    const Array<uint8>* bytes = files.Find(path);
    Array<uint8> edited(allocator);
    const uint8 bom[] = {0xef, 0xbb, 0xbf};
    return bytes != nullptr && edited.TryAppend(Span<const uint8>(bom)) &&
           edited.TryAppend(bytes->View()) && files.Put(path, edited.View());
}

// 정상 fixture 읽기 결과를 확인한다(실제·메모리 파일 시스템 공용).
void CheckFixtureBundle(const ExchangeBundle& bundle, const ExchangeDiagnostics& diagnostics) {
    CHECK(bundle.formatVersion == 1);
    CHECK(bundle.exporter.plugin == StringView("RocksExport"));
    CHECK(bundle.exporter.pluginVersion == StringView("1.0.0"));
    CHECK(bundle.exporter.engineVersion == StringView("5.8.4"));
    CHECK(bundle.exporter.engineCommit == StringView("f45729c0b6f0"));
    CHECK(!bundle.substrate && bundle.retainJpegFormat);
    CHECK(bundle.createdUtc == StringView("2026-11-21T00:00:00Z"));
    CHECK(!bundle.environment.IsValid());
    REQUIRE(bundle.Assets().Size() == FixtureAssetCount);
    REQUIRE(bundle.Redirects().Size() == 1);
    CHECK(bundle.Redirects()[0].from == StringView("/Game/Old/MI_Old.MI_Old"));
    CHECK(bundle.ResolveRedirect("/Game/Old/MI_Old.MI_Old") ==
          StringView("/Game/Materials/MI_Rock.MI_Rock"));
    CHECK(bundle.ResolveRedirect("/Game/Textures/T_Rock.T_Rock") ==
          StringView("/Game/Textures/T_Rock.T_Rock"));
    CHECK(bundle.FindAsset("/Game/Textures/T_Rock.T_Rock") == RockTextureAsset);
    CHECK(bundle.FindAsset("/Game/Textures/T_Missing.T_Missing") == ExchangeNoIndex);
    for (uint32 index = 0; index < FixtureAssetCount; ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        CHECK(asset.state ==
              (index == WorldAsset ? ExchangeAssetState::Unsupported : ExchangeAssetState::Ready));
    }

    const ExchangeAsset& world = bundle.Assets()[WorldAsset];
    CHECK(world.className == StringView("World") &&
          world.exchangeClass == ExchangeClass::Unsupported && !world.data.IsValid());
    const ExchangeAsset& skinned = bundle.Assets()[SkinnedMeshAsset];
    CHECK(skinned.exchangeClass == ExchangeClass::SkeletalMesh);
    CHECK(skinned.ueClass == StringView("/Script/Engine.SkeletalMesh"));
    CHECK(skinned.file == StringView("assets/Game/Characters/SK_Hero.json"));
    REQUIRE(skinned.blobs.Size() == 3);
    CHECK(skinned.blobs[0].layout == ExchangeBlobLayout::F32 && skinned.blobs[0].bytes == 36 &&
          skinned.blobs[0].components == 3 && skinned.blobs[0].count == 3);
    CHECK(skinned.blobs[1].layout == ExchangeBlobLayout::BoneInfluences);
    CHECK(skinned.blobs[2].layout == ExchangeBlobLayout::SparseVec3);
    CHECK(skinned.exporterDiagnostics.IsArray() && skinned.exporterDiagnostics.Size() == 1);
    REQUIRE(skinned.dependencies.Size() == 2);
    CHECK(skinned.dependencies[0].target == SkeletonAsset &&
          skinned.dependencies[0].targetClass == ExchangeClass::Skeleton);
    CHECK(skinned.dependencies[1].target == InstanceAsset);
    const ExchangeAsset& rock = bundle.Assets()[RockMeshAsset];
    REQUIRE(rock.dependencies.Size() == 3);
    CHECK(rock.dependencies[0].target == CollisionMeshAsset);
    // redirect를 푼 대상이다.
    CHECK(rock.dependencies[1].objectPath == StringView("/Game/Old/MI_Old.MI_Old") &&
          rock.dependencies[1].target == InstanceAsset);
    CHECK(rock.dependencies[2].kind == ExchangeDependencyKind::Soft &&
          rock.dependencies[2].target == RockTextureAsset);
    double scale = 0.0;
    REQUIRE(ReadExchangeNumber(rock.settings.Find("DistanceFieldResolutionScale"), scale));
    CHECK(scale > 1.0e308);
    const ExchangeAsset& material = bundle.Assets()[MaterialAsset];
    REQUIRE(material.dependencies.Size() == 3);
    CHECK(material.dependencies[1].target == ExchangeNoIndex &&
          material.dependencies[1].targetClass == ExchangeClass::Texture2D);
    CHECK(bundle.Assets()[RockTextureAsset].blobs.Size() == 1);
    CHECK(bundle.Assets()[RockTextureAsset].blobs[0].digest ==
          bundle.Assets()[SharedTextureAsset].blobs[0].digest);
    CHECK(bundle.Assets()[FunctionAsset].blobs.IsEmpty());
    // 범위 밖 클래스 경고 하나 말고는 진단이 없다.
    REQUIRE(diagnostics.Items().Size() == 1);
    const ExchangeDiagnostic& warning = diagnostics.Items()[0];
    CHECK(warning.code == AssetDiagnosticCode::UnsupportedAsset &&
          warning.severity == ExchangeSeverity::Warning &&
          warning.issue == ExchangeIssue::UnsupportedClass && warning.asset == WorldAsset &&
          warning.text.View() == StringView("World"));
}

// 저장소 fixture를 실제 파일 시스템과 메모리 파일 시스템에서 읽은 결과가 같고, verifyBlobs를 끄면
// blob 파일을 열지 않는다. Clear는 읽기 전 상태로 되돌린다.
TEST(UnrealExchange_ReadFixtureBundle) {
    SystemAllocator allocator;
    {
        StringBuilder<512> source(allocator);
        REQUIRE(FixtureSourcePath("Basic.rxbundle", source));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(platform::PhysicalFileSystem(), source.View(), {}, bundle,
                                   diagnostics));
        CheckFixtureBundle(bundle, diagnostics);
        CHECK(bundle.Directory() == source.View());
        bundle.Clear();
        CHECK(bundle.Assets().IsEmpty() && bundle.Redirects().IsEmpty() &&
              bundle.formatVersion == 0 && bundle.Directory().IsEmpty());
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadFixture(files, bundle, diagnostics));
        CheckFixtureBundle(bundle, diagnostics);
        // bundle.json + 에셋 JSON 9개 + 서로 다른 blob 사용 6개.
        CHECK(files.opens == 16 && files.OpenHandles() == 0);

        files.opens = 0;
        ExchangeBundle unverified(allocator);
        ExchangeDiagnostics unverifiedDiagnostics(allocator);
        REQUIRE(ReadFixture(files, unverified, unverifiedDiagnostics, false));
        CheckFixtureBundle(unverified, unverifiedDiagnostics);
        CHECK(files.opens == 10);

        // 끝에 '/'가 붙은 디렉터리도 같은 경로로 연다.
        ExchangeBundle slashed(allocator);
        ExchangeDiagnostics slashedDiagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, "Basic.rxbundle/", {}, slashed, slashedDiagnostics));
        CHECK(slashed.Assets().Size() == FixtureAssetCount);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// bundle.json 편집 하나와 기대 결과다.
struct ManifestCase {
    StringView from;
    StringView to;
    ErrorCode error;
    AssetDiagnosticCode code;
    ExchangeIssue issue;
};

// bundle.json 묶음 수준 오류: 읽기 실패, out 빈 상태, 마지막 오류 진단이 기대값이다.
TEST(UnrealExchange_ManifestTampering) {
    SystemAllocator allocator;
    const ManifestCase cases[] = {
        {"\"formatVersion\": 1,", "\"formatVersion\": 2,", ErrorCode::Unsupported,
         AssetDiagnosticCode::UnsupportedBundleVersion, ExchangeIssue::Version},
        {"\"formatVersion\": 1,", "\"formatVersion\": 18446744073709551615,",
         ErrorCode::Unsupported, AssetDiagnosticCode::UnsupportedBundleVersion,
         ExchangeIssue::Version},
        {"\"projectSettings\": {", "\"settings\": {", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::MissingKey},
        {"\"file\": \"assets/Game/Textures/T_Rock.json\"",
         "\"path\": \"assets/Game/Textures/T_Rock.json\"", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::MissingKey},
        {"\"from\": \"/Game/Old/MI_Old.MI_Old\",", "\"source\": \"/Game/Old/MI_Old.MI_Old\",",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle, ExchangeIssue::MissingKey},
        {"\"formatVersion\": 1,", "\"formatVersion\": -1,", ErrorCode::Unsupported,
         AssetDiagnosticCode::UnsupportedBundleVersion, ExchangeIssue::Version},
        {"\"formatVersion\": 1,", "\"formatVersion\": \"1\",", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"formatVersion\": 1,", "\"formatVersion\": 1.0,", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"formatVersion\": 1,", "\"version\": 1,", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::MissingKey},
        {"\"plugin\": \"RocksExport\",", "\"plugin\": 5,", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"engineCommit\": \"f45729c0b6f0\"", "\"commit\": \"f45729c0b6f0\"",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle, ExchangeIssue::MissingKey},
        {"\"exporter\": {", "\"exporterX\": {", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::MissingKey},
        {"\"substrate\": false,", "\"substrate\": 0,", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"retainJpegFormat\": true", "\"retainJpegFormat\": null", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"createdUtc\": \"2026-11-21T00:00:00Z\",", "\"createdUtc\": 5,",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"createdUtc\": \"2026-11-21T00:00:00Z\",", "\"environment\": [],",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"assets\": [", "\"assets\": [5, ", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"assets\": [", "\"assets\": {}, \"list\": [", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"assets\": [", "\"assets\": [,", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::JsonSyntax},
        {"\"objectPath\": \"/Game/Textures/T_Rock.T_Rock\",",
         "\"objectPath\": \"/Game/Textures/T_Rock.T_Rock:Sub\",", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::ObjectPathSyntax},
        {"\"objectPath\": \"/Game/Textures/T_Rock.T_Rock\",", "\"objectPath\": 1,",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"class\": \"Texture2D\",\n      \"file\": \"assets/Game/Textures/T_Rock.json\"",
         "\"file\": \"assets/Game/Textures/T_Rock.json\"", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::MissingKey},
        {"\"file\": \"assets/Game/Textures/T_Rock.json\"",
         "\"file\": \"assets/Game/Textures/../T_Rock.json\"", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::FileNameMismatch},
        {"\"file\": \"assets/Game/Textures/T_Rock.json\"",
         "\"file\": \"assets/Game/Textures/T_Rock.jsom\"", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::FileNameMismatch},
        {"\"file\": \"assets/Game/Textures/T_Rock.json\"",
         "\"file\": \"assetz/Game/Textures/T_Rock.json\"", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::FileNameMismatch},
        {"\"file\": \"assets/Game/Textures/T_Rock.json\"",
         "\"file\": \"assets/Game/Textures/T_Rocc.json\"", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::FileNameMismatch},
        // 첫 에셋을 맨 뒤 이름으로 바꾸면 정렬 위반이다.
        {"\"objectPath\": \"/Game/Characters/SKEL_Hero.SKEL_Hero\",\n      \"class\": "
         "\"Skeleton\",\n      \"file\": \"assets/Game/Characters/SKEL_Hero.json\"",
         "\"objectPath\": \"/Game/Z/SKEL_Hero.SKEL_Hero\",\n      \"class\": \"Skeleton\",\n     "
         " \"file\": \"assets/Game/Z/SKEL_Hero.json\"",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle, ExchangeIssue::NotSorted},
        // 두 번째 에셋을 첫 에셋과 같게 하면 엄격 증가 위반이다.
        {"\"objectPath\": \"/Game/Characters/SK_Hero.SK_Hero\",\n      \"class\": "
         "\"SkeletalMesh\",\n      \"file\": \"assets/Game/Characters/SK_Hero.json\"",
         "\"objectPath\": \"/Game/Characters/SKEL_Hero.SKEL_Hero\",\n      \"class\": "
         "\"Skeleton\",\n      \"file\": \"assets/Game/Characters/SKEL_Hero.json\"",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle, ExchangeIssue::NotSorted},
        {"\"from\": \"/Game/Old/MI_Old.MI_Old\",", "\"from\": \"/Game/Textures/T_Rock.T_Rock\",",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle,
         ExchangeIssue::RedirectConflict},
        {"\"to\": \"/Game/Materials/MI_Rock.MI_Rock\"", "\"to\": \"/Game/Old/MI_Old.MI_Old\"",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle,
         ExchangeIssue::RedirectConflict},
        {"\"to\": \"/Game/Materials/MI_Rock.MI_Rock\"", "\"to\": \"MI_Rock\"",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle,
         ExchangeIssue::ObjectPathSyntax},
        {"\"redirects\": [",
         "\"redirects\": [{\"from\": \"/Game/Zed/A.A\", \"to\": "
         "\"/Game/Textures/T_Rock.T_Rock\"}, ",
         ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptBundle, ExchangeIssue::NotSorted},
        {"\"redirects\": [", "\"redirects\": [\"x\", ", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
        {"\"redirects\": [", "\"redirects\": {}, \"later\": [", ErrorCode::InvalidArgument,
         AssetDiagnosticCode::CorruptBundle, ExchangeIssue::WrongType},
    };
    for (const ManifestCase& one : cases) {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        REQUIRE(EditFixture(allocator, files, "bundle.json", one.from, one.to));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        const auto read = ReadFixture(files, bundle, diagnostics);
        CHECK(read.Error() == one.error);
        CHECK(bundle.Assets().IsEmpty() && bundle.Redirects().IsEmpty() &&
              bundle.formatVersion == 0 && bundle.Directory().IsEmpty());
        REQUIRE(!diagnostics.Items().IsEmpty());
        const ExchangeDiagnostic& last = diagnostics.Items()[diagnostics.Items().Size() - 1];
        CHECK(last.code == one.code && last.issue == one.issue && last.error == one.error);
        CHECK(last.file == ExchangeFile::BundleJson && last.asset == ExchangeNoIndex);
        CHECK(diagnostics.Count(ExchangeSeverity::Error) == 1);
        CHECK(files.OpenHandles() == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 버전이 다르면 다른 스키마 차이와 무관하게 버전 오류다. 구문 오류 위치를 진단에 남긴다.
TEST(UnrealExchange_ManifestVersionFirst) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        REQUIRE(EditFixture(allocator, files, "bundle.json", "\"formatVersion\": 1,",
                            "\"formatVersion\": 7, \"newKey\": {},"));
        REQUIRE(EditFixture(allocator, files, "bundle.json", "\"exporter\": {", "\"tool\": {"));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        CHECK(ReadFixture(files, bundle, diagnostics).Error() == ErrorCode::Unsupported);
        REQUIRE(diagnostics.Items().Size() == 1);
        CHECK(diagnostics.Items()[0].code == AssetDiagnosticCode::UnsupportedBundleVersion);
        CHECK(diagnostics.Items()[0].text.View() == StringView("7"));
        CHECK(diagnostics.Items()[0].position.line == 2);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        REQUIRE(EditFixture(allocator, files, "bundle.json", "\"assets\": [", "\"assets\": [,"));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        CHECK(ReadFixture(files, bundle, diagnostics).Error() == ErrorCode::InvalidArgument);
        const ExchangeDiagnostic* syntax = diagnostics.Find(AssetDiagnosticCode::CorruptBundle);
        REQUIRE(syntax != nullptr);
        CHECK(syntax->issue == ExchangeIssue::JsonSyntax && syntax->position.line == 14 &&
              syntax->position.byteOffset > 0 && !syntax->text.View().IsEmpty());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 모르는 키는 경고만 내고 읽는다(bundle.json 루트·exporter·projectSettings·목록·redirect, 에셋 JSON
// 루트·의존·진단·blob 참조). settings·data의 모르는 키는 보지 않는다.
TEST(UnrealExchange_UnknownFieldsWarn) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        REQUIRE(EditFixture(
            allocator, files, "bundle.json", "\"createdUtc\":",
            "\"comment\": \"hello\", \"environment\": {\"rhi\": \"D3D12\"}, \"createdUtc\":"));
        REQUIRE(EditFixture(allocator, files, "bundle.json", "\"plugin\": \"RocksExport\",",
                            "\"plugin\": \"RocksExport\", \"build\": 7,"));
        REQUIRE(EditFixture(allocator, files, "bundle.json", "\"substrate\": false,",
                            "\"substrate\": false, \"nanite\": true,"));
        REQUIRE(EditFixture(allocator, files, "bundle.json",
                            "\"from\": \"/Game/Old/MI_Old.MI_Old\",",
                            "\"from\": \"/Game/Old/MI_Old.MI_Old\", \"reason\": \"rename\","));
        REQUIRE(EditFixture(allocator, files, "bundle.json", "\"redirects\": [",
                            "\"redirects\": [{\"from\": \"/Game/Ol/A.A\", \"to\": "
                            "\"/Game/Textures/T_Rock.T_Rock\"}, "));
        REQUIRE(EditFixture(allocator, files, "bundle.json",
                            "\"file\": \"assets/Game/Textures/T_Rock.json\"",
                            "\"file\": \"assets/Game/Textures/T_Rock.json\", \"size\": 1"));
        REQUIRE(EditFixture(allocator, files, "assets/Game/Textures/T_Rock.json", "\"count\": 4",
                            "\"count\": 4, \"stride\": 4"));
        REQUIRE(EditFixture(allocator, files, "assets/Game/Textures/T_Rock.json", "\"settings\": {",
                            "\"extra\": [], \"settings\": {\"Unknown\": 1, "));
        REQUIRE(EditFixture(allocator, files, "assets/Game/Materials/MI_Rock.json",
                            "\"kind\": \"hard\"", "\"kind\": \"hard\", \"via\": \"Parent\""));
        REQUIRE(EditFixture(allocator, files, "assets/Game/Characters/SK_Hero.json",
                            "\"severity\": \"info\",", "\"severity\": \"warning\", \"line\": 3,"));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadFixture(files, bundle, diagnostics));
        CHECK(diagnostics.Count(AssetDiagnosticCode::UnknownField) == 9);
        CHECK(bundle.environment.IsObject() &&
              bundle.environment.Find("rhi").GetString() == StringView("D3D12"));
        CHECK(diagnostics.Count(ExchangeSeverity::Error) == 0);
        for (const ExchangeAsset& asset : bundle.Assets()) {
            CHECK(asset.state != ExchangeAssetState::Failed);
        }

        const StringView keys[] = {"comment", "build", "nanite", "size",  "reason",
                                   "via",     "line",  "extra",  "stride"};
        for (const StringView& key : keys) {
            bool found = false;
            for (const ExchangeDiagnostic& item : diagnostics.Items()) {
                found =
                    found || (item.code == AssetDiagnosticCode::UnknownField &&
                              item.issue == ExchangeIssue::UnknownKey && item.text.View() == key);
            }

            CHECK(found);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 에셋 JSON 편집 하나와 기대 결과다.
struct AssetCase {
    StringView file;
    StringView from;
    StringView to;
    uint32 asset;
    ExchangeIssue issue;
};

// 에셋 수준 오류: 읽기는 성공하고 그 에셋만 Failed(문서·의존·blob을 놓음), 나머지는 Ready다.
TEST(UnrealExchange_AssetTampering) {
    SystemAllocator allocator;
    const AssetCase cases[] = {
        {"assets/Game/Textures/T_Rock.json", "\"objectPath\": \"/Game/Textures/T_Rock.T_Rock\",",
         "\"objectPath\": \"/Game/Textures/T_Rock2.T_Rock2\",", RockTextureAsset,
         ExchangeIssue::ManifestMismatch},
        {"assets/Game/Textures/T_Rock.json", "\"class\": \"Texture2D\",",
         "\"class\": \"Texture2DArray\",", RockTextureAsset, ExchangeIssue::ManifestMismatch},
        {"assets/Game/Textures/T_Rock.json", "\"ueClass\": \"/Script/Engine.Texture2D\",",
         "\"ueClassName\": \"/Script/Engine.Texture2D\",", RockTextureAsset,
         ExchangeIssue::MissingKey},
        {"assets/Game/Textures/T_Rock.json", "\"objectPath\": \"/Game/Textures/T_Rock.T_Rock\",",
         "\"objectPath\": 3,", RockTextureAsset, ExchangeIssue::WrongType},
        {"assets/Game/Textures/T_Rock.json", "{\n  \"objectPath\"", "[{\n  \"objectPath\"",
         RockTextureAsset, ExchangeIssue::JsonSyntax},
        {"assets/Game/Materials/MI_Rock.json", "\"kind\": \"hard\"", "\"type\": \"hard\"",
         InstanceAsset, ExchangeIssue::MissingKey},
        {"assets/Game/Characters/SK_Hero.json", "\"code\": \"GeneratedLodBaked\",",
         "\"id\": \"GeneratedLodBaked\",", SkinnedMeshAsset, ExchangeIssue::MissingKey},
        {"assets/Game/Characters/SK_Hero.json", "\"severity\": \"info\",", "\"level\": \"info\",",
         SkinnedMeshAsset, ExchangeIssue::MissingKey},
        {"assets/Game/Textures/T_Rock.json", "\"class\": \"Texture2D\",",
         "\"kind\": \"Texture2D\",", RockTextureAsset, ExchangeIssue::MissingKey},
        {"assets/Game/Materials/MI_Rock.json", "\"kind\": \"hard\"", "\"kind\": \"weak\"",
         InstanceAsset, ExchangeIssue::InvalidValue},
        {"assets/Game/Materials/MI_Rock.json", "\"objectPath\": \"/Game/Materials/M_Base.M_Base\",",
         "\"objectPath\": \"M_Base\",", InstanceAsset, ExchangeIssue::ObjectPathSyntax},
        {"assets/Game/Materials/MI_Rock.json", "\"class\": \"Material\",",
         "\"class\": \"Texture2D\",", InstanceAsset, ExchangeIssue::ManifestMismatch},
        {"assets/Game/Materials/MI_Rock.json", "\"class\": \"Material\",", "", InstanceAsset,
         ExchangeIssue::MissingKey},
        {"assets/Game/Materials/M_Base.json", "\"/Game/Textures/T_Missing.T_Missing\"",
         "\"/Game/A/T_Missing.T_Missing\"", MaterialAsset, ExchangeIssue::NotSorted},
        {"assets/Game/Materials/M_Base.json", "\"/Game/Textures/T_Missing.T_Missing\"",
         "\"/Game/Textures/T_Rock.T_Rock\"", MaterialAsset, ExchangeIssue::NotSorted},
        {"assets/Game/Characters/SK_Hero.json", "\"severity\": \"info\",",
         "\"severity\": \"fatal\",", SkinnedMeshAsset, ExchangeIssue::InvalidValue},
        {"assets/Game/Characters/SK_Hero.json", "\"message\": \"LOD 1 was generated\"",
         "\"text\": \"LOD 1 was generated\"", SkinnedMeshAsset, ExchangeIssue::MissingKey},
        {"assets/Game/Characters/SK_Hero.json", "\"diagnostics\": [", "\"diagnostics\": [1, ",
         SkinnedMeshAsset, ExchangeIssue::WrongType},
        {"assets/Game/Characters/SKEL_Hero.json", "\"dependencies\": [],", "\"dependencies\": {},",
         SkeletonAsset, ExchangeIssue::WrongType},
        {"assets/Game/Characters/SKEL_Hero.json", "\"dependencies\": [],", "\"dependencies\": [3],",
         SkeletonAsset, ExchangeIssue::WrongType},
        {"assets/Game/Characters/SKEL_Hero.json", "\"diagnostics\": [],", "\"diagnostics\": 0,",
         SkeletonAsset, ExchangeIssue::WrongType},
        {"assets/Game/Functions/MF_Noise.json", "\"settings\": {},", "\"settings\": [],",
         FunctionAsset, ExchangeIssue::WrongType},
        {"assets/Game/Functions/MF_Noise.json", "\"data\": {", "\"payload\": {", FunctionAsset,
         ExchangeIssue::MissingKey},
        // blob 참조 형식(3.3절).
        {"assets/Game/Textures/T_Rock.json", "\"count\": 4", "\"counts\": 4", RockTextureAsset,
         ExchangeIssue::MissingKey},
        {"assets/Game/Textures/T_Rock.json", "\"count\": 4", "\"count\": 18446744073709551615",
         RockTextureAsset, ExchangeIssue::InvalidValue},
        {"assets/Game/Textures/T_Rock.json", "\"blob\": \"0334fbb1", "\"blob\": \"+334fbb1",
         RockTextureAsset, ExchangeIssue::InvalidValue},
        {"assets/Game/Textures/T_Rock.json", "\"count\": 4", "\"count\": -4", RockTextureAsset,
         ExchangeIssue::InvalidValue},
        {"assets/Game/Textures/T_Rock.json", "\"count\": 4", "\"count\": 4.0", RockTextureAsset,
         ExchangeIssue::WrongType},
        {"assets/Game/Textures/T_Rock.json", "\"blob\": \"0334fbb1", "\"blob\": \"0334FBB1",
         RockTextureAsset, ExchangeIssue::InvalidValue},
        {"assets/Game/Textures/T_Rock.json", "\"blob\": \"0334fbb1", "\"blob\": \"0334fbb",
         RockTextureAsset, ExchangeIssue::InvalidValue},
        {"assets/Game/Textures/T_Rock.json", "\"blob\": \"0334fbb1", "\"blob\": 1, \"x\": \"",
         RockTextureAsset, ExchangeIssue::WrongType},
        {"assets/Game/Textures/T_Rock.json", "\"layout\": \"u8\",", "\"layout\": \"u128\",",
         RockTextureAsset, ExchangeIssue::InvalidValue},
        {"assets/Game/Textures/T_Rock.json", "\"components\": 4,", "\"components\": 0,",
         RockTextureAsset, ExchangeIssue::InvalidValue},
        {"assets/Game/Textures/T_Rock.json", "\"components\": 4,", "\"components\": 4294967296,",
         RockTextureAsset, ExchangeIssue::LimitExceeded},
        {"assets/Game/Textures/T_Rock.json", "\"bytes\": 16,", "\"bytes\": 17,", RockTextureAsset,
         ExchangeIssue::InvalidValue},
        {"assets/Game/Textures/T_Rock.json", "\"bytes\": 16,", "\"bytes\": 20,", RockTextureAsset,
         ExchangeIssue::InvalidValue},
        {"assets/Game/Textures/T_Rock.json", "\"bytes\": 16,", "\"bytes\": 4294967297,",
         RockTextureAsset, ExchangeIssue::LimitExceeded},
        {"assets/Game/Characters/SK_Hero.json",
         "\"layout\": \"boneInfluences\",\n      \"components\": 1,",
         "\"layout\": \"boneInfluences\",\n      \"components\": 2,", SkinnedMeshAsset,
         ExchangeIssue::InvalidValue},
        {"assets/Game/Characters/SK_Hero.json",
         "\"layout\": \"sparseVec3\",\n          "
         "\"components\": 1,",
         "\"layout\": \"sparseVec3\",\n          \"components\": 3,", SkinnedMeshAsset,
         ExchangeIssue::InvalidValue},
        // 한 에셋 안에서 같은 hex를 다른 bytes로 적음.
        {"assets/Game/Meshes/SM_Rock.json",
         "0f88646c73f1452bd75860b1581d665ce9d289117aa4da99bc4e27a237467769",
         "4fb8f2852abd9d09c1bffa387cb002163d2be61141df31898e80fe15aaf76148", RockMeshAsset,
         ExchangeIssue::BlobConflict},
    };
    for (const AssetCase& one : cases) {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        REQUIRE(EditFixture(allocator, files, one.file, one.from, one.to));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadFixture(files, bundle, diagnostics));
        REQUIRE(bundle.Assets().Size() == FixtureAssetCount);
        for (uint32 index = 0; index < FixtureAssetCount; ++index) {
            const ExchangeAssetState expected = index == one.asset ? ExchangeAssetState::Failed
                                                : index == WorldAsset
                                                    ? ExchangeAssetState::Unsupported
                                                    : ExchangeAssetState::Ready;
            CHECK(bundle.Assets()[index].state == expected);
        }

        const ExchangeAsset& failed = bundle.Assets()[one.asset];
        CHECK(failed.document == nullptr && failed.dependencies.IsEmpty() &&
              failed.blobs.IsEmpty() && !failed.data.IsValid() && failed.ueClass.IsEmpty());
        CHECK(diagnostics.Count(ExchangeSeverity::Error) == 1);
        const ExchangeDiagnostic* error = diagnostics.Find(AssetDiagnosticCode::CorruptBundle);
        REQUIRE(error != nullptr);
        CHECK(error->issue == one.issue && error->asset == one.asset &&
              error->file == ExchangeFile::AssetJson);
        CHECK(error->error == (one.issue == ExchangeIssue::LimitExceeded
                                   ? ErrorCode::CapacityExceeded
                                   : ErrorCode::InvalidArgument));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 의존 class가 묶음 안 대상의 class와 다르면 손상이고 관련 번호가 대상이다. 에셋 JSON BOM은
// 손상이다.
TEST(UnrealExchange_AssetDocumentDetails) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        REQUIRE(EditFixture(allocator, files, "assets/Game/Materials/MI_Rock.json",
                            "\"class\": \"Material\",", "\"class\": \"Texture2D\","));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadFixture(files, bundle, diagnostics));
        const ExchangeDiagnostic* error = diagnostics.Find(AssetDiagnosticCode::CorruptBundle);
        REQUIRE(error != nullptr);
        CHECK(error->related == MaterialAsset &&
              error->text.View() == StringView("/Game/Materials/M_Base.M_Base"));
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("assets/Game/Functions/MF_Noise.json", path));
        REQUIRE(PrependBom(allocator, files, path.View()));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadFixture(files, bundle, diagnostics));
        CHECK(bundle.Assets()[FunctionAsset].state == ExchangeAssetState::Failed);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::ByteOrderMark) == 1);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("bundle.json", path));
        REQUIRE(PrependBom(allocator, files, path.View()));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        CHECK(ReadFixture(files, bundle, diagnostics).Error() == ErrorCode::InvalidArgument);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::ByteOrderMark) == 1);
        REQUIRE(files.PutText(path.View(), "[1, 2]"));
        CHECK(ReadFixture(files, bundle, diagnostics).Error() == ErrorCode::InvalidArgument);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::WrongType) == 1);
        REQUIRE(files.PutText(path.View(), ""));
        CHECK(ReadFixture(files, bundle, diagnostics).Error() == ErrorCode::InvalidArgument);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::JsonSyntax) == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 일반 파일이 아닌 항목(10.4절 19번): bundle.json이면 묶음 수준 NotRegularFile, 에셋 JSON이면 그
// 에셋만 실패한다. link도 일반 파일로 보지 않는다.
TEST(UnrealExchange_NotRegularFiles) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("bundle.json", path));
        REQUIRE(files.SetKind(path.View(), platform::FileType::Other, false));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        CHECK(ReadFixture(files, bundle, diagnostics).Error() == ErrorCode::InvalidArgument);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::NotRegularFile) == 1);
        CHECK(files.opens == 0);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("assets/Game/Meshes/SM_Rock.json", path));
        REQUIRE(files.SetKind(path.View(), platform::FileType::File, true));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadFixture(files, bundle, diagnostics));
        CHECK(bundle.Assets()[RockMeshAsset].state == ExchangeAssetState::Failed);
        CHECK(bundle.Assets()[CollisionMeshAsset].state == ExchangeAssetState::Ready);
        const ExchangeDiagnostic* error = FindAssetDiagnostic(diagnostics, RockMeshAsset);
        REQUIRE(error != nullptr);
        CHECK(error->issue == ExchangeIssue::NotRegularFile &&
              error->error == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 묶음 JSON 합 상한(10.4절 18번): 상한이 bundle.json 크기와 같으면 bundle.json은 읽고 에셋 JSON은
// 모두 LimitExceeded로 실패한다. 상한이 그보다 작으면 묶음 읽기가 실패한다.
TEST(UnrealExchange_BundleJsonBudget) {
    SystemAllocator allocator;
    MemoryFiles files(allocator);
    REQUIRE(LoadFixture(allocator, files));
    StringBuilder<256> path(allocator);
    REQUIRE(FixtureFile("bundle.json", path));
    const Array<uint8>* manifest = files.Find(path.View());
    REQUIRE(manifest != nullptr);
    ExchangeReadOptions options;
    options.maxBundleJsonBytes = manifest->Size();
    {
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, options, bundle, diagnostics));
        usize limited = 0;
        for (usize index = 0; index < bundle.Assets().Size(); ++index) {
            const ExchangeAsset& asset = bundle.Assets()[index];
            if (asset.exchangeClass != ExchangeClass::Unsupported) {
                CHECK(asset.state == ExchangeAssetState::Failed);
                ++limited;
            }
        }

        CHECK(limited != 0 && CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                                          ExchangeIssue::LimitExceeded) == limited);
    }

    options.maxBundleJsonBytes = manifest->Size() - 1;
    {
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        CHECK(ReadExchangeBundle(files, FixtureDirectory, options, bundle, diagnostics).Error() ==
              ErrorCode::CapacityExceeded);
    }
}

// 파일 실패 하나와 기대 결과다.
struct FaultCase {
    StringView file;
    FileFault fault;
    ExchangeIssue issue;
    ErrorCode error;
};

// 파일 실패 주입: bundle.json이면 읽기 실패(파일 오류 보존), 에셋 JSON이면 그 에셋만 실패.
TEST(UnrealExchange_FileFaults) {
    SystemAllocator allocator;
    const ErrorCode system = FileErrorCode(FaultStatus);
    const ErrorCode endOfFile = FileErrorCode({platform::FileCode::EndOfFile, 0});
    const FaultCase manifestCases[] = {
        {"bundle.json", FileFault::Stat, ExchangeIssue::ReadFailed, system},
        {"bundle.json", FileFault::Open, ExchangeIssue::ReadFailed, system},
        {"bundle.json", FileFault::Size, ExchangeIssue::ReadFailed, system},
        {"bundle.json", FileFault::Read, ExchangeIssue::ReadFailed, system},
        {"bundle.json", FileFault::Short, ExchangeIssue::ReadFailed, endOfFile},
        {"bundle.json", FileFault::HugeSize, ExchangeIssue::LimitExceeded,
         ErrorCode::CapacityExceeded},
    };
    for (const FaultCase& one : manifestCases) {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile(one.file, path));
        REQUIRE(files.faultPath.TryAssign(path.View()));
        files.fault = one.fault;
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        CHECK(ReadFixture(files, bundle, diagnostics).Error() == one.error);
        REQUIRE(diagnostics.Items().Size() == 1);
        CHECK(diagnostics.Items()[0].issue == one.issue &&
              diagnostics.Items()[0].error == one.error &&
              diagnostics.Items()[0].text.View() == StringView("bundle.json"));
        CHECK(files.OpenHandles() == 0);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("bundle.json", path));
        REQUIRE(files.Remove(path.View()));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        CHECK(ReadFixture(files, bundle, diagnostics).Error() == ErrorCode::NotFound);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::ReadFailed) == 1);
    }

    const FileFault assetFaults[] = {FileFault::Stat, FileFault::Open,  FileFault::Size,
                                     FileFault::Read, FileFault::Short, FileFault::HugeSize};
    for (const FileFault fault : assetFaults) {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("assets/Game/Meshes/SM_Rock.json", path));
        REQUIRE(files.faultPath.TryAssign(path.View()));
        files.fault = fault;
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadFixture(files, bundle, diagnostics));
        CHECK(bundle.Assets()[RockMeshAsset].state == ExchangeAssetState::Failed);
        CHECK(bundle.Assets()[CollisionMeshAsset].state == ExchangeAssetState::Ready);
        const ExchangeDiagnostic* error = FindAssetDiagnostic(diagnostics, RockMeshAsset);
        REQUIRE(error != nullptr);
        CHECK(error->issue == (fault == FileFault::HugeSize ? ExchangeIssue::LimitExceeded
                                                            : ExchangeIssue::ReadFailed));
        CHECK(error->text.View() == StringView("assets/Game/Meshes/SM_Rock.json"));
        CHECK(files.OpenHandles() == 0);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("assets/Game/Meshes/SM_Rock.json", path));
        REQUIRE(files.Remove(path.View()));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadFixture(files, bundle, diagnostics));
        const ExchangeDiagnostic* error = FindAssetDiagnostic(diagnostics, RockMeshAsset);
        REQUIRE(error != nullptr);
        CHECK(error->issue == ExchangeIssue::ReadFailed && error->error == ErrorCode::NotFound);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 상한(10.4절 16번): 에셋 하나의 의존 수, 진단 수(넘으면 개수만).
TEST(UnrealExchange_ReaderLimits) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        Array<String> names(allocator);
        Array<SyntheticDependency> dependencies(allocator);
        for (uint32 index = 0; index <= UnrealExchangeMaxDependencies; ++index) {
            StringBuilder<64> name(allocator);
            REQUIRE(name.TryAppendFormat("/Game/D/A{:06}.A", index));
            String copy(allocator);
            REQUIRE(copy.TryAssign(name.View()));
            REQUIRE(names.TryPushBack(Move(copy)));
        }

        for (const String& name : names) {
            REQUIRE(dependencies.TryPushBack(SyntheticDependency{name.View(), "Texture2D"}));
        }

        const SyntheticAsset assets[] = {
            {"/Game/M/Big.Big", "Material", dependencies.View()},
            {"/Game/M/Fit.Fit", "Material",
             Span<const SyntheticDependency>(dependencies.Data(), UnrealExchangeMaxDependencies)}};
        REQUIRE(WriteSyntheticBundle(allocator, files, Span<const SyntheticAsset>(assets)));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics));
        CHECK(bundle.Assets()[0].state == ExchangeAssetState::Failed);
        CHECK(bundle.Assets()[1].state == ExchangeAssetState::Ready);
        CHECK(bundle.Assets()[1].dependencies.Size() == UnrealExchangeMaxDependencies);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::LimitExceeded) == 1);
    }

    {
        // blob 참조 안 모르는 키 5000개 → 경고 4096개 + 버린 904개.
        MemoryFiles files(allocator);
        BlobBytes blob(allocator);
        REQUIRE(blob.Fill(4, 7));
        StringBuilder<256> data(allocator);
        REQUIRE(data.TryAppend("{\"pixels\": "));
        StringBuilder<256> reference(allocator);
        REQUIRE(WriteSyntheticBlob(files, blob.bytes.View(), "u8", 1, 4, reference));
        REQUIRE(data.TryAppend(StringView(reference.View().Data(), reference.ByteCount() - 1)));
        for (uint32 index = 0; index < 5000; ++index) {
            REQUIRE(data.TryAppendFormat(", \"k{}\": 0", index));
        }

        REQUIRE(data.TryAppend("}}"));
        const SyntheticAsset assets[] = {{"/Game/T/Noisy.Noisy", "Texture2D", {}, data.View()}};
        REQUIRE(WriteSyntheticBundle(allocator, files, Span<const SyntheticAsset>(assets)));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics));
        CHECK(bundle.Assets()[0].state == ExchangeAssetState::Ready);
        CHECK(diagnostics.Items().Size() == UnrealExchangeMaxDiagnostics);
        CHECK(diagnostics.Dropped() == 5000 - UnrealExchangeMaxDiagnostics);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_bundle_tests_file
