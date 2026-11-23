#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Source.h>
#include <SparksVm/Vm.h>

// Sparks `@test` 실행기다(Docs/Design/SparksTools.ko.md 9절, SparksLanguage 13.1절).
//
// 언제 쓰나: 진단 없이 컴파일한 작업 공간에서 `@test fn`(인자 없는 최상위 함수 — `async`도 된다)을
// 찾아 headless host로 돌린다. `sparks test`(SparksTools)와 시험이 쓴다. 엔진 객체·필드·native
// 함수·문맥·로드를 쓰는 시험은 엔진 fixture가 필요해 NeedsEngine으로 끝난다(엔진 연동 EI가 World
// fixture host를 끼운다).
//
// 사용 예:
//   testing::TestReport report(allocator);
//   const uint32 modules[] = {gameModule};
//   if (auto ran = testing::RunTests(allocator, compilation, sources, modules, {}, report); !ran) {
//       // OutOfMemory, 또는 InvalidArgument(report.Reason() — 링크 거부 등)
//   }
//   for (const testing::TestResult& result : report.Results()) {
//       if (result.outcome != testing::TestOutcome::Passed) {
//           testing::AppendTestReport(text, compilation, sources, result);
//       }
//   }
//
// 실행: 시험마다 새 VmSession에서 최상위 진입(VmSession::CallFunction)으로 부르고, 정지하면 프레임
// (FixedStep → Advance(TestFrameSeconds) → ApplyRequests → EndFrame)을 maxFrames까지 진행한다.
// 시계는 프레임 수로만 진행한다(실제 시간 없음). 시험 사이에 공유하는 상태가 없어 결과는 시험
// 하나만 돌려도 같다.
//
// 스레드: 한 호출은 한 스레드에서 돈다. 전역 상태를 쓰지 않는다.

namespace rocks::sparks::testing {

// 정지한 시험이 진행할 프레임 수의 기본값(60 Hz 10초)과 상한이다(SparksTools 9.5절).
inline constexpr uint32 DefaultTestMaxFrames = 600;
inline constexpr uint32 MaxTestFrames = 1000000;

// 시험 프레임 하나의 시간(초)이다(Game·UI 시계 모두).
inline constexpr double TestFrameSeconds = 1.0 / 60.0;

enum class TestOutcome : uint8 {
    Passed,
    Faulted,        // VM fault(AssertFailed 포함 — fault)
    EndedWithError, // `Result`가 아닌 시험의 `try` 실패(SL 8.4절 — errorCode)
    NeedsEngine,    // headless host가 거부한 엔진 기능(detail — 처음 거부한 묶음 이름)
    DidNotFinish    // maxFrames 프레임 뒤에도 정지해 있다
};

// 시험 하나의 결과다. 위치(source·span)는 fault 자리의 토큰, 없으면 시험 이름이다.
struct TestResult {
    explicit TestResult(Allocator& allocator) noexcept : detail(allocator) {
    }

    uint32 module = NoModule; // compilation 모듈 번호
    uint32 symbol = NoSymbol; // 시험 함수 심볼
    TestOutcome outcome = TestOutcome::Passed;
    VmFaultKind fault = VmFaultKind::None; // Faulted·NeedsEngine
    uint64 errorCode = 0;                  // EndedWithError: `Error` case 번호 + 1
    SourceId source;
    SourceSpan span;
    uint32 frames = 0; // 진행한 프레임 수
    String detail;     // NeedsEngine: 거부한 묶음 이름(`World`·`Actor.Destroy` …)
};

struct TestOptions {
    uint32 maxFrames = DefaultTestMaxFrames; // 1–MaxTestFrames
};

// 실행 결과다. 결과는 modules 순서, 모듈 안은 원문 위치 순이다.
class TestReport final {
public:
    explicit TestReport(Allocator& allocator) noexcept
        : allocator_(&allocator), results_(allocator), reason_(allocator) {
    }

    Span<const TestResult> Results() const noexcept {
        return results_.View();
    }

    ROCKS_SPARKSTESTHOST_API uint32 PassedCount() const noexcept;
    ROCKS_SPARKSTESTHOST_API uint32 FailedCount() const noexcept;

    // RunTests가 InvalidArgument로 끝났을 때의 사유(영어 한 줄)다.
    StringView Reason() const noexcept {
        return reason_.View();
    }

private:
    friend ROCKS_SPARKSTESTHOST_API Result<void>
    RunTests(Allocator& allocator, const Compilation& compilation, const SourceSet& sources,
             Span<const uint32> modules, const TestOptions& options, TestReport& out) noexcept;

    Allocator* allocator_;
    Array<TestResult> results_;
    String reason_;
};

// modules(compilation 모듈 번호 — 호출자가 정한 순서)의 `@test` 함수를 돌려 out에 결과를 둔다(out의
// 이전 내용은 버린다). compilation은 진단 없이 모든 단계를 마친 것이어야 하고(모든 모듈에
// bytecode), sources는 그 원문 묶음이다. 링크는 compilation의 모든 모듈을 묶는다(시험이 다른 모듈
// 함수를 부른다). 실패: bytecode 없는 모듈·링크 거부·범위 밖 옵션·실행기 불변식 위반은
// InvalidArgument(out.Reason()), 할당 실패는 OutOfMemory. 시험의 실패는 결과이지 실패가 아니다.
ROCKS_SPARKSTESTHOST_API Result<void> RunTests(Allocator& allocator, const Compilation& compilation,
                                               const SourceSet& sources, Span<const uint32> modules,
                                               const TestOptions& options,
                                               TestReport& out) noexcept;

// 결과의 종류 이름이다(보고 첫 줄의 코드 자리): fault 종류 이름(`AssertFailed` …),
// `EndedWithError`·`NeedsEngine`·`DidNotFinish`, 통과는 `Passed`.
ROCKS_SPARKSTESTHOST_API StringView TestOutcomeName(const TestResult& result) noexcept;

// 실패한 결과 하나를 진단과 같은 형식(SL 8.1절 — AppendSourceReport)으로 붙인다: 첫 줄
// `<경로>:<줄>:<열>: error <종류>: Test '<이름>' failed.`, 원문 줄과 밑줄, 설명 줄(DidNotFinish는
// result.frames를 쓴다). 실패: out의 할당 실패.
ROCKS_SPARKSTESTHOST_API Result<void> AppendTestReport(StringBuilderBase& out,
                                                       const Compilation& compilation,
                                                       const SourceSet& sources,
                                                       const TestResult& result) noexcept;

} // namespace rocks::sparks::testing
