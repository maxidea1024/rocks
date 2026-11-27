#include "ExchangeTestSupport.h"

#include <Core/FileError.h>
#include <Core/Math/RandomStream.h>
#include <MeshSource/MeshSourceTypes.h>
#include <Object/NameField.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// Unreal 교환 blob 시험이다(Docs/Design/UnrealExchange 3.3절, 10.4절 11·12번): fixture blob
// 파일 변조(없음·잘림·SHA 불일치·같은 hex 다른 bytes·파일 실패), 가변 배치(names·boneInfluences·
// sparseVec3) 규칙, 64 KiB 조각 경계, 흘려 읽기(읽기 중 검증)와 메모리 읽기(LoadExchangeBlob)의
// 판정 일치 퍼징.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_blob_tests_file {

constexpr uint64 BlobFuzzSeed = 0x5558316142424c42ull;
constexpr uint32 BlobFuzzCount = 400;

constexpr uint32 SkinnedMeshAsset = 1;
constexpr uint32 RockMeshAsset = 6;
constexpr uint32 CollisionMeshAsset = 7;
constexpr uint32 SharedTextureAsset = 8;
constexpr uint32 RockTextureAsset = 9;

constexpr StringView PixelsHex = "0334fbb141542de36bbfbced8364565fcdbdb4cbf754b7ac48badeebd9eebba8";
constexpr StringView PositionsHex =
    "4fb8f2852abd9d09c1bffa387cb002163d2be61141df31898e80fe15aaf76148";
constexpr StringView InfluencesHex =
    "1b078469a70f9e779ea345e02680569039df190587576cf015d8982f000083ad";

// fixture blob 파일의 메모리 경로다.
bool FixtureBlobPath(StringView hex, StringBuilderBase& out) noexcept {
    return out.TryAppend(FixtureDirectory) && out.TryAppend("/blobs/") &&
           out.TryAppend(StringView(hex.Data(), 2)) && out.TryAppendAscii('/') &&
           out.TryAppend(hex) && out.TryAppend(".bin");
}

// 읽기 결과에서 실패한 에셋 번호 목록이 expected와 같고, 각 실패의 첫 진단이 (issue, error)인지
// 본다.
bool FailedExactly(const ExchangeBundle& bundle, const ExchangeDiagnostics& diagnostics,
                   Span<const uint32> expected, ExchangeIssue issue, ErrorCode error) noexcept {
    for (uint32 index = 0; index < bundle.Assets().Size(); ++index) {
        bool listed = false;
        for (const uint32 asset : expected) {
            listed = listed || asset == index;
        }

        const bool failed = bundle.Assets()[index].state == ExchangeAssetState::Failed;
        if (failed != listed) {
            return false;
        }

        if (!listed) {
            continue;
        }

        const ExchangeDiagnostic* first = FindAssetDiagnostic(diagnostics, index);
        if (first == nullptr || first->issue != issue || first->error != error ||
            first->file != ExchangeFile::Blob) {
            return false;
        }
    }

    return true;
}

// fixture blob 파일 변조: 그 blob을 쓰는 에셋 모두가 실패하고 나머지는 Ready다.
TEST(UnrealExchange_FixtureBlobTampering) {
    SystemAllocator allocator;
    {
        // 없음: 픽셀 blob을 쓰는 텍스처 둘.
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureBlobPath(PixelsHex, path));
        REQUIRE(files.Remove(path.View()));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        const uint32 textures[] = {SharedTextureAsset, RockTextureAsset};
        CHECK(FailedExactly(bundle, diagnostics, Span<const uint32>(textures),
                            ExchangeIssue::ReadFailed, ErrorCode::NotFound));
        CHECK(diagnostics.Items()[1].text.View() == PixelsHex);
    }

    const uint32 positionUsers[] = {SkinnedMeshAsset, RockMeshAsset, CollisionMeshAsset};
    {
        // 잘림: 크기 불일치.
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureBlobPath(PositionsHex, path));
        Array<uint8>* bytes = files.Find(path.View());
        REQUIRE(bytes != nullptr && bytes->TryResize(bytes->Size() - 1));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        CHECK(FailedExactly(bundle, diagnostics, Span<const uint32>(positionUsers),
                            ExchangeIssue::BlobSize, ErrorCode::CorruptData));
    }

    {
        // SHA 불일치(크기는 같음).
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureBlobPath(InfluencesHex, path));
        Array<uint8>* bytes = files.Find(path.View());
        REQUIRE(bytes != nullptr);
        (*bytes)[bytes->Size() - 1] ^= 1;
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        const uint32 skinned[] = {SkinnedMeshAsset};
        CHECK(FailedExactly(bundle, diagnostics, Span<const uint32>(skinned),
                            ExchangeIssue::BlobHash, ErrorCode::CorruptData));

        // 검증을 끄면 읽기는 통과하고 임포트 때 LoadExchangeBlob이 잡는다.
        ExchangeBundle unverified(allocator);
        ExchangeDiagnostics later(allocator);
        ExchangeReadOptions options;
        options.verifyBlobs = false;
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, options, unverified, later));
        REQUIRE(unverified.Assets()[SkinnedMeshAsset].state == ExchangeAssetState::Ready);
        Array<uint8> out(allocator);
        CHECK(LoadExchangeBlob(unverified, SkinnedMeshAsset,
                               unverified.Assets()[SkinnedMeshAsset].blobs[1], out, later)
                  .Error() == ErrorCode::CorruptData);
        CHECK(out.IsEmpty());
        const ExchangeDiagnostic* error = later.Find(AssetDiagnosticCode::CorruptBundle);
        REQUIRE(error != nullptr);
        CHECK(error->issue == ExchangeIssue::BlobHash && error->asset == SkinnedMeshAsset &&
              error->file == ExchangeFile::Blob && error->text.View() == InfluencesHex);
    }

    {
        // 에셋 사이 같은 hex·다른 bytes: 그 hex를 쓰는 에셋 모두가 BlobConflict다.
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("assets/Game/Textures/T_Rock.json", path));
        REQUIRE(files.ReplaceOnce(path.View(), PixelsHex, PositionsHex));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        const uint32 conflicted[] = {SkinnedMeshAsset, RockMeshAsset, CollisionMeshAsset,
                                     RockTextureAsset};
        CHECK(FailedExactly(bundle, diagnostics, Span<const uint32>(conflicted),
                            ExchangeIssue::BlobConflict, ErrorCode::CorruptData));
    }

    const FileFault faults[] = {FileFault::Stat, FileFault::Open, FileFault::Size, FileFault::Read,
                                FileFault::Short};
    for (const FileFault fault : faults) {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureBlobPath(PositionsHex, path));
        REQUIRE(files.faultPath.TryAssign(path.View()));
        files.fault = fault;
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        const ErrorCode expected =
            fault == FileFault::Short ? ErrorCode::CorruptData : FileErrorCode(FaultStatus);
        CHECK(FailedExactly(bundle, diagnostics, Span<const uint32>(positionUsers),
                            fault == FileFault::Short ? ExchangeIssue::BlobSize
                                                      : ExchangeIssue::ReadFailed,
                            expected));
        CHECK(files.OpenHandles() == 0);
    }

    // digest 앞 8 byte가 같고 나머지가 다른 두 blob은 다른 blob이다(수집 사슬의 키 충돌 — 10.4절
    // 21번). 파일 확인은 끈다(hex만 바꾼 참조라 파일이 없다).
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("assets/Game/Meshes/SM_Rock.json", path));
        REQUIRE(files.ReplaceOnce(
            path.View(), "0f88646c73f1452bd75860b1581d665ce9d289117aa4da99bc4e27a237467769",
            "4fb8f2852abd9d09d75860b1581d665ce9d289117aa4da99bc4e27a237467769"));
        ExchangeReadOptions options;
        options.verifyBlobs = false;
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, options, bundle, diagnostics));
        const ExchangeAsset& rock = bundle.Assets()[RockMeshAsset];
        REQUIRE(rock.state == ExchangeAssetState::Ready);
        REQUIRE(rock.blobs.Size() == 2);
        CHECK(!(rock.blobs[0].digest == rock.blobs[1].digest));
    }

    // blob 자리가 일반 파일이 아니면(디렉터리) 그 blob을 쓰는 에셋이 NotRegularFile로 실패한다.
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureBlobPath(PositionsHex, path));
        REQUIRE(files.SetKind(path.View(), platform::FileType::Directory, false));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        CHECK(FailedExactly(bundle, diagnostics, Span<const uint32>(positionUsers),
                            ExchangeIssue::NotRegularFile, ErrorCode::CorruptData));
        CHECK(files.OpenHandles() == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// LoadExchangeBlob: fixture blob을 그대로 읽고, 읽은 뒤 바뀐·없어진 파일을 잡고, 읽지 않은 묶음은
// InvalidArgument다. 할당 실패는 진단 없이 그 오류다.
TEST(UnrealExchange_LoadBlob) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        uint32 loaded = 0;
        for (uint32 index = 0; index < bundle.Assets().Size(); ++index) {
            for (const ExchangeBlobRef& blob : bundle.Assets()[index].blobs) {
                Array<uint8> out(allocator);
                REQUIRE(LoadExchangeBlob(bundle, index, blob, out, diagnostics));
                CHECK(out.Size() == blob.bytes && Sha256::Hash(out.View()) == blob.digest);
                ++loaded;
            }
        }

        CHECK(loaded == 9 && diagnostics.Items().Size() == 1);
        const ExchangeBlobRef blob = bundle.Assets()[RockTextureAsset].blobs[0];
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureBlobPath(PixelsHex, path));
        REQUIRE(files.Remove(path.View()));
        Array<uint8> out(allocator);
        CHECK(LoadExchangeBlob(bundle, RockTextureAsset, blob, out, diagnostics).Error() ==
              ErrorCode::NotFound);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::ReadFailed) == 1);

        // 메모리 읽기의 파일 실패: 읽기 실패와 크기 불일치.
        BlobBytes pixels(allocator);
        REQUIRE(pixels.Fill(16, 1));
        REQUIRE(files.Put(path.View(), pixels.bytes.View()));
        REQUIRE(files.faultPath.TryAssign(path.View()));
        files.fault = FileFault::Read;
        CHECK(LoadExchangeBlob(bundle, RockTextureAsset, blob, out, diagnostics).Error() ==
              FileErrorCode(FaultStatus));
        files.fault = FileFault::Short;
        CHECK(LoadExchangeBlob(bundle, RockTextureAsset, blob, out, diagnostics).Error() ==
              ErrorCode::CorruptData);
        files.fault = FileFault::None;
        CHECK(LoadExchangeBlob(bundle, RockTextureAsset, blob, out, diagnostics).Error() ==
              ErrorCode::CorruptData);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::CorruptBundle,
                          ExchangeIssue::BlobHash) == 1);
        CHECK(out.IsEmpty() && files.OpenHandles() == 0);

        const ExchangeBlobRef positions = bundle.Assets()[RockMeshAsset].blobs[0];
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            Array<uint8> target(failing);
            ExchangeDiagnostics quiet(allocator);
            failing.FailOnAttempt(attempt);
            auto result = LoadExchangeBlob(bundle, RockMeshAsset, positions, target, quiet);
            if (!result) {
                CHECK(result.Error() == ErrorCode::OutOfMemory && target.IsEmpty() &&
                      quiet.Items().IsEmpty());
                ++failures;
                continue;
            }

            CHECK(target.Size() == positions.bytes);
            if (failing.Attempts() < attempt) {
                break;
            }
        }

        CHECK(failures >= 1);
        bundle.Clear();
        CHECK(LoadExchangeBlob(bundle, 0, positions, out, diagnostics).Error() ==
              ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 합성 묶음 하나에 blob 하나를 두고, 읽기 중 검증과 LoadExchangeBlob의 판정을 함께 돌려준다.
struct LayoutVerdict {
    bool streamed = false;
    bool loaded = false;
    ExchangeIssue streamedIssue = ExchangeIssue::None;
    ExchangeIssue loadedIssue = ExchangeIssue::None;
};

LayoutVerdict JudgeLayout(Allocator& allocator, Span<const uint8> bytes, StringView layout,
                          uint64 components, uint64 count, uint64 declaredBytes) noexcept {
    LayoutVerdict verdict;
    MemoryFiles files(allocator);
    StringBuilder<512> data(allocator);
    StringBuilder<256> reference(allocator);
    if (!WriteSyntheticBlob(files, bytes, layout, components, count, reference)) {
        return verdict;
    }

    // declaredBytes가 실제 크기와 다르면 참조의 bytes를 바꾼다(크기 불일치 시험).
    StringBuilder<64> actual(allocator);
    StringBuilder<64> declared(allocator);
    if (!actual.TryAppendFormat("\"bytes\": {},", bytes.Size()) ||
        !declared.TryAppendFormat("\"bytes\": {},", declaredBytes)) {
        return verdict;
    }

    const usize at = reference.View().Find(actual.View(), StringComparison::CaseSensitive);
    if (at == StringView::NotFound || !data.TryAppend("{\"values\": ") ||
        !data.TryAppend(StringView(reference.View().Data(), at)) ||
        !data.TryAppend(declared.View()) ||
        !data.TryAppend(StringView(reference.View().Data() + at + actual.ByteCount(),
                                   reference.ByteCount() - at - actual.ByteCount())) ||
        !data.TryAppendAscii('}')) {
        return verdict;
    }

    const SyntheticAsset assets[] = {{"/Game/S/A.A", "SkeletalMesh", {}, data.View()}};
    if (!WriteSyntheticBundle(allocator, files, Span<const SyntheticAsset>(assets))) {
        return verdict;
    }

    {
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        if (!ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics)) {
            return verdict;
        }

        verdict.streamed = bundle.Assets()[0].state == ExchangeAssetState::Ready;
        const ExchangeDiagnostic* error = diagnostics.Find(AssetDiagnosticCode::CorruptBundle);
        verdict.streamedIssue = error != nullptr ? error->issue : ExchangeIssue::None;
    }

    ExchangeBundle bundle(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    ExchangeReadOptions options;
    options.verifyBlobs = false;
    if (!ReadExchangeBundle(files, SyntheticDirectory, options, bundle, diagnostics) ||
        bundle.Assets()[0].state != ExchangeAssetState::Ready) {
        verdict.loadedIssue = ExchangeIssue::InvalidValue;
        return verdict;
    }

    Array<uint8> out(allocator);
    verdict.loaded = static_cast<bool>(
        LoadExchangeBlob(bundle, 0, bundle.Assets()[0].blobs[0], out, diagnostics));
    const ExchangeDiagnostic* error = diagnostics.Find(AssetDiagnosticCode::CorruptBundle);
    verdict.loadedIssue = error != nullptr ? error->issue : ExchangeIssue::None;
    return verdict;
}

// 두 경로가 같은 판정이고 그것이 expected(통과 또는 BlobLayout·BlobSize 실패)인지 본다.
bool Judged(const LayoutVerdict& verdict, bool pass, ExchangeIssue issue) noexcept {
    if (verdict.streamed != verdict.loaded || verdict.streamedIssue != verdict.loadedIssue) {
        return false;
    }

    return pass ? verdict.streamed : !verdict.streamed && verdict.streamedIssue == issue;
}

bool Layout(Allocator& allocator, const BlobBytes& bytes, StringView layout, uint64 components,
            uint64 count, bool pass) noexcept {
    return Judged(
        JudgeLayout(allocator, bytes.bytes.View(), layout, components, count, bytes.bytes.Size()),
        pass, ExchangeIssue::BlobLayout);
}

// names 배치: count × components개의 (길이 + byte)를 끝까지 정확히 소비, 길이 ≤ MaxNameFieldBytes.
TEST(UnrealExchange_NamesLayout) {
    SystemAllocator allocator;
    {
        BlobBytes valid(allocator);
        REQUIRE(valid.Text("ab") && valid.Text(""));
        CHECK(Layout(allocator, valid, "names", 1, 2, true));
        CHECK(Layout(allocator, valid, "names", 2, 1, true));
        CHECK(Layout(allocator, valid, "names", 1, 1, false));
        CHECK(Layout(allocator, valid, "names", 1, 3, false));
        CHECK(Layout(allocator, valid, "names", 1, 100, false));

        BlobBytes empty(allocator);
        CHECK(Layout(allocator, empty, "names", 1, 0, true));

        BlobBytes leftover(allocator);
        REQUIRE(leftover.Text("ab") && leftover.Fill(1, 0));
        CHECK(Layout(allocator, leftover, "names", 1, 1, false));

        BlobBytes truncated(allocator);
        REQUIRE(truncated.Word(5) && truncated.Fill(3, 'a'));
        CHECK(Layout(allocator, truncated, "names", 1, 1, false));

        BlobBytes shortWord(allocator);
        REQUIRE(shortWord.Text("") && shortWord.Fill(6, 0));
        CHECK(Layout(allocator, shortWord, "names", 1, 2, false));

        BlobBytes longest(allocator);
        REQUIRE(longest.Word(static_cast<uint32>(MaxNameFieldBytes)) &&
                longest.Fill(MaxNameFieldBytes, 'n'));
        CHECK(Layout(allocator, longest, "names", 1, 1, true));
        BlobBytes tooLong(allocator);
        REQUIRE(tooLong.Word(static_cast<uint32>(MaxNameFieldBytes + 1)) &&
                tooLong.Fill(MaxNameFieldBytes + 1, 'n'));
        CHECK(Layout(allocator, tooLong, "names", 1, 1, false));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// boneInfluences 배치: 시작 표(첫 값 0, 단조, 차 ≤ MeshMaxSourceInfluences) + 영향 4 byte × 마지막
// 값.
TEST(UnrealExchange_InfluencesLayout) {
    SystemAllocator allocator;
    {
        auto make = [&](Span<const uint32> starts, uint32 influences, BlobBytes& out) noexcept {
            for (const uint32 start : starts) {
                if (!out.Word(start)) {
                    return false;
                }
            }

            return out.Fill(static_cast<usize>(influences) * 4, 1);
        };

        const uint32 valid[] = {0, 1, 3};
        BlobBytes good(allocator);
        REQUIRE(make(Span<const uint32>(valid), 3, good));
        CHECK(Layout(allocator, good, "boneInfluences", 1, 2, true));

        const uint32 nonZero[] = {1, 1, 3};
        BlobBytes first(allocator);
        REQUIRE(make(Span<const uint32>(nonZero), 2, first));
        CHECK(Layout(allocator, first, "boneInfluences", 1, 2, false));

        const uint32 decreasing[] = {0, 2, 1};
        BlobBytes down(allocator);
        REQUIRE(make(Span<const uint32>(decreasing), 1, down));
        CHECK(Layout(allocator, down, "boneInfluences", 1, 2, false));

        const uint32 widest[] = {0, MeshMaxSourceInfluences};
        BlobBytes wide(allocator);
        REQUIRE(make(Span<const uint32>(widest), MeshMaxSourceInfluences, wide));
        CHECK(Layout(allocator, wide, "boneInfluences", 1, 1, true));

        const uint32 tooWide[] = {0, MeshMaxSourceInfluences + 1};
        BlobBytes over(allocator);
        REQUIRE(make(Span<const uint32>(tooWide), MeshMaxSourceInfluences + 1, over));
        CHECK(Layout(allocator, over, "boneInfluences", 1, 1, false));

        BlobBytes extra(allocator);
        REQUIRE(make(Span<const uint32>(valid), 4, extra));
        CHECK(Layout(allocator, extra, "boneInfluences", 1, 2, false));

        // 시작 표가 byte 수보다 크다(count 5에 12 byte).
        CHECK(Layout(allocator, good, "boneInfluences", 1, 5, false));

        const uint32 none[] = {0};
        BlobBytes empty(allocator);
        REQUIRE(make(Span<const uint32>(none), 0, empty));
        CHECK(Layout(allocator, empty, "boneInfluences", 1, 0, true));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// sparseVec3 배치: K + 번호 K(엄격 증가, < count) + Vec3 K, 크기 = 4 + 16 × K.
TEST(UnrealExchange_SparseLayout) {
    SystemAllocator allocator;
    {
        auto make = [&](Span<const uint32> indices, BlobBytes& out) noexcept {
            if (!out.Word(static_cast<uint32>(indices.Size()))) {
                return false;
            }

            for (const uint32 index : indices) {
                if (!out.Word(index)) {
                    return false;
                }
            }

            return out.Fill(indices.Size() * 12, 0x3f);
        };

        const uint32 valid[] = {0, 2};
        BlobBytes good(allocator);
        REQUIRE(make(Span<const uint32>(valid), good));
        CHECK(Layout(allocator, good, "sparseVec3", 1, 3, true));
        CHECK(Layout(allocator, good, "sparseVec3", 1, 2, false));

        BlobBytes empty(allocator);
        REQUIRE(make(Span<const uint32>(), empty));
        CHECK(Layout(allocator, empty, "sparseVec3", 1, 0, true));

        const uint32 repeated[] = {2, 2};
        BlobBytes same(allocator);
        REQUIRE(make(Span<const uint32>(repeated), same));
        CHECK(Layout(allocator, same, "sparseVec3", 1, 3, false));

        const uint32 many[] = {0, 1, 2, 3};
        BlobBytes tooMany(allocator);
        REQUIRE(make(Span<const uint32>(many), tooMany));
        CHECK(Layout(allocator, tooMany, "sparseVec3", 1, 3, false));

        BlobBytes extra(allocator);
        REQUIRE(make(Span<const uint32>(valid), extra) && extra.Fill(1, 0));
        CHECK(Layout(allocator, extra, "sparseVec3", 1, 3, false));

        BlobBytes nothing(allocator);
        CHECK(Layout(allocator, nothing, "sparseVec3", 1, 3, false));

        // 참조 bytes가 파일 크기와 다르면 배치 전에 BlobSize다.
        CHECK(Judged(
            JudgeLayout(allocator, good.bytes.View(), "sparseVec3", 1, 3, good.bytes.Size() + 16),
            false, ExchangeIssue::BlobSize));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 64 KiB 조각 경계를 넘는 blob: 길이 word·이름이 경계에 걸쳐도 같은 판정이다.
TEST(UnrealExchange_BlobChunkBoundaries) {
    SystemAllocator allocator;
    {
        BlobBytes names(allocator);
        uint32 count = 0;
        while (names.bytes.Size() < 3 * UnrealExchangeBlobChunkBytes) {
            const usize length = 1 + (count * 7919u) % 61u;
            REQUIRE(names.Word(static_cast<uint32>(length)) && names.Fill(length, 'x'));
            ++count;
        }

        CHECK(Layout(allocator, names, "names", 1, count, true));
        CHECK(Layout(allocator, names, "names", 1, count + 1, false));
        names.bytes[names.bytes.Size() - 1] = 0;
        REQUIRE(names.Fill(1, 0));
        CHECK(Layout(allocator, names, "names", 1, count, false));

        BlobBytes pixels(allocator);
        REQUIRE(pixels.Fill(2 * UnrealExchangeBlobChunkBytes + 3, 9));
        CHECK(Layout(allocator, pixels, "u8", 1, pixels.bytes.Size(), true));

        BlobBytes influences(allocator);
        const uint32 vertices = 40000;
        for (uint32 vertex = 0; vertex <= vertices; ++vertex) {
            REQUIRE(influences.Word(vertex * 2));
        }

        REQUIRE(influences.Fill(static_cast<usize>(vertices) * 2 * 4, 5));
        CHECK(Layout(allocator, influences, "boneInfluences", 1, vertices, true));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 배치 규칙을 지키는 blob을 만든다(names: 짧은 이름, boneInfluences: 차 0–3, sparseVec3: 증가
// 번호).
bool MakeValidLayout(RandomStream& random, uint32 which, uint32 count, BlobBytes& out) noexcept {
    if (which == 0) {
        for (uint32 entry = 0; entry < count; ++entry) {
            const uint32 length = random.NextUInt32Below(6);
            if (!out.Word(length) || !out.Fill(length, static_cast<uint8>('a' + entry))) {
                return false;
            }
        }

        return true;
    }

    if (which == 1) {
        uint32 start = 0;
        if (!out.Word(start)) {
            return false;
        }

        for (uint32 vertex = 0; vertex < count; ++vertex) {
            start += random.NextUInt32Below(4);
            if (!out.Word(start)) {
                return false;
            }
        }

        return out.Fill(static_cast<usize>(start) * 4, 2);
    }

    uint32 entries = 0;
    uint32 picked[8] = {};
    for (uint32 index = 0; index < count; ++index) {
        if (random.NextBool()) {
            picked[entries++] = index;
        }
    }

    if (!out.Word(entries)) {
        return false;
    }

    for (uint32 entry = 0; entry < entries; ++entry) {
        if (!out.Word(picked[entry])) {
            return false;
        }
    }

    return out.Fill(static_cast<usize>(entries) * 12, 0x40);
}

// 퍼징: 규칙을 지키는 가변 배치 blob을 만든 뒤 절반은 byte 하나를 바꾸거나 끝을 늘이고 줄여 SHA를
// 맞춰 넣으면, 읽기 중 검증과 메모리 읽기가 언제나 같은 판정이다(통과 또는 BlobLayout).
TEST(UnrealExchange_BlobLayoutFuzz) {
    SystemAllocator allocator;
    {
        RandomStream random(BlobFuzzSeed);
        const StringView layouts[] = {"names", "boneInfluences", "sparseVec3"};
        uint32 passed = 0;
        uint32 failed = 0;
        for (uint32 iteration = 0; iteration < BlobFuzzCount; ++iteration) {
            const uint32 which = random.NextUInt32Below(3);
            const uint32 count = random.NextUInt32Below(7);
            BlobBytes bytes(allocator);
            REQUIRE(MakeValidLayout(random, which, count, bytes));
            const uint32 mutation = random.NextUInt32Below(4);
            if (mutation == 1 && !bytes.bytes.IsEmpty()) {
                const usize at = random.NextUInt32Below(static_cast<uint32>(bytes.bytes.Size()));
                bytes.bytes[at] = static_cast<uint8>(random.NextUInt32Below(256));
            } else if (mutation == 2) {
                REQUIRE(bytes.Fill(1 + random.NextUInt32Below(4), 0));
            } else if (mutation == 3 && !bytes.bytes.IsEmpty()) {
                REQUIRE(bytes.bytes.TryResize(bytes.bytes.Size() - 1));
            }

            const LayoutVerdict verdict = JudgeLayout(allocator, bytes.bytes.View(), layouts[which],
                                                      1, count, bytes.bytes.Size());
            CHECK(verdict.streamed == verdict.loaded &&
                  verdict.streamedIssue == verdict.loadedIssue);
            CHECK(verdict.streamed || verdict.streamedIssue == ExchangeIssue::BlobLayout);
            CHECK(verdict.streamed || mutation != 0);
            passed += verdict.streamed ? 1 : 0;
            failed += verdict.streamed ? 0 : 1;
        }

        CHECK(passed > BlobFuzzCount / 4 && failed > BlobFuzzCount / 4);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_blob_tests_file
