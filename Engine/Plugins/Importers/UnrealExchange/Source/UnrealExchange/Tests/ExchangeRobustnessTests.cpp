#include "ExchangeTestSupport.h"

#include <Core/Math/RandomStream.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// Unreal 교환 리더의 할당 실패와 JSON 퍼징 시험이다(Docs/Design/UnrealExchange.ko.md 12.1절
// "JSON·blob 퍼징 — 리더는 오류로 끝나거나 통과하고, 통과한 것은 순서 계산이 정상 진단으로
// 끝난다").
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_robustness_tests_file {

constexpr uint64 JsonFuzzSeed = 0x555831614a534f4eull;
constexpr uint32 JsonFuzzCount = 300;

// fixture JSON 파일들(bundle.json과 에셋 JSON)이다.
constexpr StringView FuzzTargets[] = {
    "bundle.json",
    "assets/Game/Characters/SK_Hero.json",
    "assets/Game/Materials/M_Base.json",
    "assets/Game/Meshes/SM_Rock.json",
    "assets/Game/Textures/T_Rock.json",
};

// 할당 실패: 묶음 allocator의 n번째 할당을 실패시키면 OutOfMemory이고 묶음은 비어 있으며, 끝내
// 성공하면 실패 없는 결과와 같다. 진단 목록 할당 실패도 그 오류다.
TEST(UnrealExchange_ReadAllocationFailures) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            ExchangeBundle bundle(failing);
            ExchangeDiagnostics diagnostics(allocator);
            failing.FailOnAttempt(attempt);
            auto read = ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics);
            if (!read) {
                CHECK(read.Error() == ErrorCode::OutOfMemory && bundle.Assets().IsEmpty() &&
                      bundle.Directory().IsEmpty());
                CHECK(diagnostics.Count(ExchangeSeverity::Error) == 0);
                CHECK(files.OpenHandles() == 0);
                ++failures;
                continue;
            }

            CHECK(bundle.Assets().Size() == 10 && diagnostics.Items().Size() == 1);
            for (const ExchangeAsset& asset : bundle.Assets()) {
                CHECK(asset.state != ExchangeAssetState::Failed);
            }

            if (failing.Attempts() < attempt) {
                break;
            }
        }

        CHECK(failures > 50);

        for (uint64 attempt = 1; attempt <= 2; ++attempt) {
            test::NFailAllocator failing(allocator);
            ExchangeBundle bundle(allocator);
            ExchangeDiagnostics diagnostics(failing);
            failing.FailOnAttempt(attempt);
            CHECK(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics).Error() ==
                  ErrorCode::OutOfMemory);
            CHECK(bundle.Assets().IsEmpty());
            failing.DisableFailure();
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 손상 진단을 담다가 실패하는 경로: bundle.json 손상, blob 손상.
TEST(UnrealExchange_ReadDiagnosticAllocationFailures) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("bundle.json", path));
        REQUIRE(files.ReplaceOnce(path.View(), "\"formatVersion\": 1,", "\"formatVersion\": 3,"));
        test::NFailAllocator failing(allocator);
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(failing);
        failing.FailOnAttempt(1);
        CHECK(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics).Error() ==
              ErrorCode::OutOfMemory);
        failing.DisableFailure();
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("assets/Game/Materials/MI_Rock.json", path));
        REQUIRE(files.ReplaceOnce(path.View(), "\"class\": \"Material\",",
                                  "\"class\": \"Texture2D\","));
        StringBuilder<256> blob(allocator);
        REQUIRE(FixtureFile("blobs/03/"
                            "0334fbb141542de36bbfbced8364565fcdbdb4cbf754b7ac48badeebd9eebba8.bin",
                            blob));
        REQUIRE(files.Remove(blob.View()));
        uint32 failures = 0;
        for (uint64 attempt = 1; attempt <= 8; ++attempt) {
            test::NFailAllocator failing(allocator);
            ExchangeBundle bundle(allocator);
            ExchangeDiagnostics diagnostics(failing);
            failing.FailOnAttempt(attempt);
            auto read = ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics);
            failures += !read ? 1 : 0;
            CHECK(read || read.Error() == ErrorCode::OutOfMemory);
            failing.DisableFailure();
        }

        CHECK(failures >= 4);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// JSON 퍼징: fixture JSON 하나를 byte 단위로 망가뜨리면 리더는 묶음 수준 오류(진단 있음)로 끝나거나
// 통과하고, 통과한 묶음의 순서 계산은 성공한다. 같은 입력은 같은 결과다.
TEST(UnrealExchange_JsonFuzz) {
    SystemAllocator allocator;
    {
        RandomStream random(JsonFuzzSeed);
        const char replacements[] = {'"', '{', '}', '[',  ']', ',',  ':',    '0',    '9',
                                     '-', 'e', ' ', '\\', 'x', '\n', '\x80', '\xff', '\0'};
        uint32 accepted = 0;
        uint32 rejected = 0;
        uint32 failedAssets = 0;
        for (uint32 iteration = 0; iteration < JsonFuzzCount; ++iteration) {
            MemoryFiles files(allocator);
            REQUIRE(LoadFixture(allocator, files));
            StringBuilder<256> path(allocator);
            const StringView target =
                FuzzTargets[random.NextUInt32Below(sizeof(FuzzTargets) / sizeof(FuzzTargets[0]))];
            REQUIRE(FixtureFile(target, path));
            Array<uint8>* bytes = files.Find(path.View());
            REQUIRE(bytes != nullptr && !bytes->IsEmpty());
            const uint32 edits = 1 + random.NextUInt32Below(4);
            for (uint32 edit = 0; edit < edits; ++edit) {
                const usize at = random.NextUInt32Below(static_cast<uint32>(bytes->Size()));
                const uint32 kind = random.NextUInt32Below(3);
                if (kind == 0) {
                    (*bytes)[at] = static_cast<uint8>(
                        replacements[random.NextUInt32Below(sizeof(replacements))]);
                } else if (kind == 1 && bytes->Size() > 1) {
                    REQUIRE(bytes->TryRemoveAt(at));
                } else {
                    REQUIRE(bytes->TryResize(at + 1));
                }
            }

            ExchangeBundle bundle(allocator);
            ExchangeDiagnostics diagnostics(allocator);
            auto read = ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics);
            ExchangeBundle again(allocator);
            ExchangeDiagnostics againDiagnostics(allocator);
            auto reread = ReadExchangeBundle(files, FixtureDirectory, {}, again, againDiagnostics);
            CHECK(static_cast<bool>(read) == static_cast<bool>(reread));
            CHECK(diagnostics.Items().Size() == againDiagnostics.Items().Size());
            CHECK(files.OpenHandles() == 0);
            if (!read) {
                CHECK(read.Error() != ErrorCode::OutOfMemory);
                CHECK(diagnostics.Count(ExchangeSeverity::Error) +
                          diagnostics.Count(AssetDiagnosticCode::UnsupportedBundleVersion) >=
                      1);
                CHECK(bundle.Assets().IsEmpty());
                ++rejected;
                continue;
            }

            ++accepted;
            for (const ExchangeAsset& asset : bundle.Assets()) {
                failedAssets += asset.state == ExchangeAssetState::Failed ? 1 : 0;
            }

            ExchangeImportPlan plan(allocator);
            CHECK(PlanExchangeImport(bundle, {}, plan, diagnostics));
            usize members = 0;
            for (const ExchangeImportUnit& unit : plan.Units()) {
                members += unit.memberCount;
            }

            usize supported = 0;
            for (const ExchangeAsset& asset : bundle.Assets()) {
                supported += asset.state != ExchangeAssetState::Unsupported ? 1 : 0;
            }

            CHECK(members == supported);
        }

        CHECK(accepted > 20 && rejected > 20 && failedAssets > 20);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_robustness_tests_file
