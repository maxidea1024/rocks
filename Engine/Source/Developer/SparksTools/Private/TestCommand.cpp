#include <SparksTestHost/TestRunner.h>
#include "CommandInternal.h"

// `sparks test`다(Docs/Design/SparksTools.ko.md T10·9.5절): 진단 없는 컴파일에서 인자 아래 모듈의
// `@test` 함수를 headless host로 돌리고 실패를 진단과 같은 형식으로 보고한다.
namespace rocks::sparks::tools::detail {

int32 RunTest(CommandContext& context, StringBuilderBase& text) noexcept {
    if (context.diagnostics.Count() != 0) {
        AppendSummary(text, context);
        return ExitFailure; // 진단이 있으면 시험을 돌리지 않는다
    }

    Array<usize> order(context.allocator);
    if (!ModuleOrder(context.workspace, true, order)) {
        return ExitError;
    }

    Array<uint32> modules(context.allocator);
    for (const usize at : order) {
        if (!modules.TryPushBack(context.modules[at])) {
            return ExitError;
        }
    }

    testing::TestOptions options;
    if (context.arguments.maxFrames != 0) {
        options.maxFrames = context.arguments.maxFrames;
    }

    testing::TestReport report(context.allocator);
    if (auto ran = testing::RunTests(context.allocator, context.compilation, context.sources,
                                     modules.View(), options, report);
        !ran) {
        if (ran.Error() != ErrorCode::InvalidArgument) {
            return ExitError;
        }

        StringBuilder<OutputInlineBytes> message(context.allocator);
        message.AppendFormat("cannot run the tests: {}", report.Reason());
        if (message.HasFailed()) {
            return ExitError;
        }

        // 실행기 거부(링크·import — 모듈과 host schema가 어긋남)는 시험 실패와 같은 1이다(9.5절).
        return ReportProblem(context, StringView(), message.View(), ExitFailure);
    }

    for (const testing::TestResult& result : report.Results()) {
        if (result.outcome != testing::TestOutcome::Passed &&
            !testing::AppendTestReport(text, context.compilation, context.sources, result)) {
            return ExitError;
        }
    }

    if (report.Results().IsEmpty()) {
        text.Append("no tests\n");
        return ExitSuccess;
    }

    text.AppendFormat("{} passed, {} failed\n", report.PassedCount(), report.FailedCount());
    return report.FailedCount() == 0 ? ExitSuccess : ExitFailure;
}

} // namespace rocks::sparks::tools::detail
