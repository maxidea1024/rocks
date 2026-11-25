#include <TestSupport/NFailAllocator.h>
#include "SparksToolsTestSupport.h"

// `sparks test` 시험이다(Docs/Design/SparksTools.ko.md 9.5절·9.6절 — LC8d): 실패 보고(진단 형식)와
// 요약·종료 코드, 인자 아래 모듈만, `--max-frames` 적용, 진단 있는 작업 공간·시험 없음, 할당 실패
// 전수. 결과 종류 전체는 실행기 시험(SparksTestRunner_*)이 본다.
using namespace rocks;
namespace sparks_tools_test_command_tests_file {

using namespace rocks::test::sparks_tools;

namespace {

constexpr StringView FullManifest = "{ name: \"Game\", sparks: 1, profile: \"full\" }\n";

constexpr StringView Mixed = R"(@test fn Adds() {
    Assert(1 + 2 == 3, "sum");
}

@test fn Wrong() {
    Assert(1 + 2 == 4, "sum");
}

@test async fn Ticks() {
    await NextTick();
    await NextTick();
    await NextTick();
}
)";

constexpr StringView Passing = R"(@test fn Fine() {
    Assert(true, "fine");
}
)";

} // namespace

TEST(SparksTools_TestRunsAndReports) {
    SystemAllocator allocator;
    {
        TempFolder temp(allocator);
        REQUIRE(temp.Write("Game/SparksPackage.json5", FullManifest));
        REQUIRE(temp.Write("Game/Mixed.spark", Mixed));
        REQUIRE(temp.Write("Game/Sub/Passing.spark", Passing));
        const String folder = temp.PathOf("Game");
        const String sub = temp.PathOf("Game/Sub");
        {
            // 실패한 시험만 진단 형식으로, 끝에 요약. 실패가 있으면 1.
            const StringView arguments[] = {"test", folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitFailure);
            const String shown = Slashed(allocator, folder.View());
            StringBuilder<1024> expected(allocator);
            expected.AppendFormat("{}/Mixed.spark:6:5: error AssertFailed: Test 'Wrong' failed.\n"
                                  "        Assert(1 + 2 == 4, \"sum\");\n"
                                  "        ^^^^^^\n"
                                  "  The assertion is false.\n"
                                  "3 passed, 1 failed\n",
                                  shown.View());
            CHECK(output.output.View() == expected.View());
            CHECK(output.error.IsEmpty());
        }

        {
            // 프레임 상한: 세 프레임이 필요한 시험이 두 프레임에서 끝나지 않는다.
            const StringView arguments[] = {"test", "--max-frames", "2", folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitFailure);
            CHECK(output.output.View().Contains("error DidNotFinish: Test 'Ticks' failed.",
                                                StringComparison::CaseSensitive));
            CHECK(output.output.View().Contains("still waiting after 2 frames",
                                                StringComparison::CaseSensitive));
            CHECK(output.output.View().EndsWith("2 passed, 2 failed\n"));
        }

        {
            // 인자 아래 모듈만: 하위 폴더 인자는 그 아래 시험만 돌린다(컴파일은 패키지 전체).
            const StringView arguments[] = {"test", sub.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "1 passed, 0 failed\n");
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTools_TestNoTestsAndDiagnostics) {
    SystemAllocator allocator;
    {
        TempFolder temp(allocator);
        REQUIRE(temp.Write("Game/SparksPackage.json5", FullManifest));
        REQUIRE(temp.Write("Game/Plain.spark", "fn F(): int {\n    return 1;\n}\n"));
        const String folder = temp.PathOf("Game");
        {
            const StringView arguments[] = {"test", folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "no tests\n");
        }

        // 진단이 있으면 진단과 요약만 쓰고 시험은 돌리지 않는다.
        REQUIRE(temp.Write("Game/Broken.spark",
                           "@test fn T() {\n    Assert(1 == 2, \"x\");\n}\n\nfn G( {\n}\n"));
        {
            const StringView arguments[] = {"test", folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitFailure);
            CHECK(output.output.View().Contains("error S02", StringComparison::CaseSensitive));
            CHECK(output.output.View().EndsWith(" modules\n"));
            CHECK(!output.output.View().Contains("Test 'T'", StringComparison::CaseSensitive));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTools_TestAllocationFailure) {
    // test의 모든 할당 실패 자리: ExitError로 끝나거나 같은 결과이고 새는 할당이 없다.
    SystemAllocator backing;
    TempFolder temp(backing);
    REQUIRE(temp.Write("Game/SparksPackage.json5", FullManifest));
    REQUIRE(temp.Write("Game/Mixed.spark", Mixed));
    const String folder = temp.PathOf("Game");
    const StringView arguments[] = {"test", folder.View()};
    uint64 attempts = 0;
    uint64 failures = 0;
    String baseline(backing);
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        const uint64 before = backing.Stats().liveAllocations;
        {
            BufferOutput output(backing);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            const int32 code =
                RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output);
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                attempts = used;
                CHECK(code == ExitFailure);
                REQUIRE(baseline.TryAssign(output.output.View()));
            } else if (code == ExitError) {
                ++failures;
            } else {
                CHECK(code == ExitFailure);
                CHECK(output.output.View() == baseline.View());
            }
        }

        if (failAt != 0) {
            CHECK(backing.Stats().liveAllocations == before); // 기준 출력은 살아 있다
        }
    }

    CHECK(failures > 0);
}

} // namespace sparks_tools_test_command_tests_file
