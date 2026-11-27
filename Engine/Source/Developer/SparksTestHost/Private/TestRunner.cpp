#include <Core/Memory/UniquePtr.h>
#include <SparksTestHost/TestRunner.h>
#include <SparksVm/BytecodeFile.h>
#include <SparksVm/VmSession.h>
#include "FakeHost.h"

// `@test` 실행기의 구현이다(Docs/Design/SparksTools.ko.md 9절). headless host는 FakeHost의 묶기·
// 리터럴·이름 표기를 그대로 쓰고 엔진 객체를 만들거나 읽는 호출은 모두 거부한다(9.2절).

namespace rocks::sparks::testing {

namespace {

using conformance::detail::FakeBinding;
using conformance::detail::FakeHost;

// 사유 문장 builder의 inline 크기다.
constexpr usize TestReasonInlineBytes = 256;

// 시험 bank 번호다(Scalar 0 — FakeHost와 같은 배치).
constexpr uint8 TestScalarBank = 0;

// 로드 요청을 거부할 때 기억하는 이름이다(묶음이 없는 host 호출 — `Soft<T>.Load`).
constexpr StringView TestLoadName = "Soft.Load";

// UTF-8 이어지는 byte(10xxxxxx)의 판별 mask와 값이다.
constexpr uint8 TestUtf8TrailMask = 0xC0;
constexpr uint8 TestUtf8TrailBits = 0x80;

// headless host다(9.2절): 살아 있는 엔진 객체가 없다. 거부는 ContractViolation이고 처음 거부한 묶음
// 이름을 기억한다(시험마다 Clear).
class TestHeadlessHost final : public VmHost {
public:
    TestHeadlessHost(Allocator& allocator, const Compilation& compilation) noexcept
        : fake_(allocator, compilation), refused_(allocator) {
    }

    bool RanOutOfMemory() const noexcept {
        return fake_.RanOutOfMemory() || outOfMemory_;
    }

    StringView Refused() const noexcept {
        return refused_.View();
    }

    bool HasRefused() const noexcept {
        return refusedAny_;
    }

    void ClearRefused() noexcept {
        refused_.Clear();
        refusedAny_ = false;
    }

    Result<VmHostBinding> Bind(const VmBindRequest& request,
                               StringBuilderBase& reason) noexcept override {
        return fake_.Bind(request, reason);
    }

    bool IsAlive(uint64) noexcept override {
        return false;
    }

    void ReleaseHeld(uint64) noexcept override {
    }

    uint64 StartLoad(uint64, uint64) noexcept override {
        static_cast<void>(Refuse(TestLoadName));
        return 0; // 계약 위반 fault — 실행기가 NeedsEngine으로 바꾼다
    }

    LoadState WatchLoad(uint64, bool, uint64&, uint64&) noexcept override {
        return LoadState::NotRequest;
    }

    VmFaultKind GetContext(VmHostCall& call) noexcept override {
        return RefuseCall(call);
    }

    VmFaultKind GetField(VmHostCall& call, uint64) noexcept override {
        return RefuseCall(call);
    }

    VmFaultKind SetField(VmHostCall& call, uint64) noexcept override {
        return RefuseCall(call);
    }

    VmFaultKind CallNative(VmHostCall& call) noexcept override {
        return RefuseCall(call);
    }

    VmFaultKind MakeLiteral(VmHostCall& call) noexcept override {
        // 값 리터럴(이름 번호 — Tag·Name 등)만 만든다. 객체를 만드는 리터럴은 엔진이 필요하다.
        if (fake_.BindingAt(call.Binding()).bank != TestScalarBank) {
            return RefuseCall(call);
        }

        return fake_.MakeLiteral(call);
    }

    VmFaultKind ResolveVirtual(VmHostCall& call, uint64, VmVirtualTarget&) noexcept override {
        return RefuseCall(call);
    }

    bool IsA(uint64, uint64) noexcept override {
        return false;
    }

    VmFaultKind InitializeStruct(VmHostCall& call) noexcept override {
        return RefuseCall(call);
    }

    bool AppendName(uint64 value, StringBuilderBase& out) noexcept override {
        return fake_.AppendName(value, out);
    }

    bool AppendEnumName(uint64 type, uint64 value, StringBuilderBase& out) noexcept override {
        return fake_.AppendEnumName(type, value, out);
    }

private:
    // 처음 거부한 이름을 기억한다. 실패: 할당 실패(false — RanOutOfMemory도 참이 된다).
    bool Refuse(StringView name) noexcept {
        if (refusedAny_) {
            return true;
        }

        refusedAny_ = true;
        if (!refused_.TryAssign(name)) {
            outOfMemory_ = true;
            return false;
        }

        return true;
    }

    VmFaultKind RefuseCall(VmHostCall& call) noexcept {
        if (!Refuse(fake_.BindingAt(call.Binding()).name.View())) {
            call.Fail(ErrorCode::OutOfMemory);
        }

        return VmFaultKind::ContractViolation;
    }

    FakeHost fake_;
    String refused_;
    bool refusedAny_ = false;
    bool outOfMemory_ = false;
};

// 실행 단위(모듈 하나)다: `.sparkc` 왕복 bytecode(verifier 통과)와 링크. session이 링크를
// 가리키므로 옮기지 않는다(UniquePtr).
struct TestUnit {
    explicit TestUnit(Allocator& allocator) noexcept : loaded(allocator), linked(allocator) {
    }

    uint32 module = NoModule;
    BytecodeModule loaded;
    VmLinkedModule linked;
};

// 시험 하나(찾은 것)다.
struct TestEntry {
    uint32 symbol = NoSymbol;
    uint32 begin = 0; // 이름 위치(원문 순서 key)
};

Result<void> Reject(String& reason, StringView text) noexcept {
    if (auto assigned = reason.TryAssign(text); !assigned) {
        return assigned;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// 원문 위치 location(줄·code point 열 — 1부터)의 토큰 범위다(그 위치를 덮는 구문 트리 토큰, 없으면
// code point 하나). 위치가 원문 밖이면 false.
bool TestSpanAt(const SourceFile& file, const SyntaxTree* tree, BytecodeLocation location,
                SourceSpan& span) noexcept {
    if (location.line == 0 || location.column == 0 || location.line > file.LineCount()) {
        return false;
    }

    const StringView line = file.LineText(location.line);
    auto trail = [&](usize at) noexcept {
        return at < line.ByteCount() &&
               (static_cast<uint8>(line.Data()[at]) & TestUtf8TrailMask) == TestUtf8TrailBits;
    };
    usize at = 0;
    for (uint32 column = 1; column < location.column; ++column) {
        if (at >= line.ByteCount()) {
            return false;
        }

        ++at;
        while (trail(at)) {
            ++at;
        }
    }

    usize end = at < line.ByteCount() ? at + 1 : at;
    while (trail(end)) {
        ++end;
    }

    const uint32 begin = file.LineBegin(location.line);
    span.begin = begin + static_cast<uint32>(at);
    span.end = begin + static_cast<uint32>(end);
    if (tree != nullptr) {
        for (const Token& token : tree->Tokens()) {
            if (token.span.begin <= span.begin && span.begin < token.span.end) {
                span = token.span;
                break;
            }
        }
    }

    return true;
}

} // namespace

uint32 TestReport::PassedCount() const noexcept {
    uint32 count = 0;
    for (const TestResult& result : results_) {
        count += result.outcome == TestOutcome::Passed ? 1 : 0;
    }

    return count;
}

uint32 TestReport::FailedCount() const noexcept {
    return static_cast<uint32>(results_.Size()) - PassedCount();
}

Result<void> RunTests(Allocator& allocator, const Compilation& compilation,
                      const SourceSet& sources, Span<const uint32> modules,
                      const TestOptions& options, TestReport& out) noexcept {
    out.results_.Clear();
    out.reason_.Clear();
    if (options.maxFrames == 0 || options.maxFrames > MaxTestFrames) {
        StringBuilder<TestReasonInlineBytes> reason(allocator);
        reason.AppendFormat("the frame limit must be from 1 to {}", MaxTestFrames);
        if (reason.HasFailed()) {
            return Result<void>::Failure(reason.Status().Error());
        }

        return Reject(out.reason_, reason.View());
    }

    TestHeadlessHost host(allocator, compilation);
    if (host.RanOutOfMemory()) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    // 모든 모듈을 `.sparkc` 왕복(verifier)으로 읽어 한 host로 링크하고 모듈 사이 호출을 푼다.
    Array<UniquePtr<TestUnit>> units(allocator);
    Array<const VmLinkedModule*> linkOf(allocator); // compilation 모듈 번호 → 링크
    if (auto sized = linkOf.TryResize(compilation.ModuleCount()); !sized) {
        return sized;
    }

    for (const VmLinkedModule*& link : linkOf) {
        link = nullptr;
    }

    Array<VmLinkedModule*> linking(allocator);
    for (uint32 module = 0; module < compilation.ModuleCount(); ++module) {
        const BytecodeModule* bytecode = compilation.ModuleBytecode(module);
        if (bytecode == nullptr) {
            return Reject(out.reason_,
                          "a module has no bytecode (the compilation must finish without "
                          "diagnostics)");
        }

        auto made = UniquePtr<TestUnit>::TryMake(allocator, allocator);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        if (auto pushed = units.TryPushBack(Move(made.Value())); !pushed) {
            return pushed;
        }

        TestUnit& unit = *units[units.Size() - 1];
        unit.module = module;
        Array<uint8> file(allocator);
        if (auto written = WriteBytecodeFile(*bytecode, file); !written) {
            return written;
        }

        BytecodeRejection rejection;
        if (auto read = ReadBytecodeFile(file.View(), unit.loaded, rejection); !read) {
            if (read.Error() != ErrorCode::CorruptData) {
                return read;
            }

            return Reject(out.reason_, "the bytecode of a module was rejected");
        }

        StringBuilder<TestReasonInlineBytes> reason(allocator);
        if (auto linked = LinkModule(unit.loaded, &host, unit.linked, reason); !linked) {
            if (linked.Error() != ErrorCode::InvalidArgument || reason.HasFailed()) {
                return Result<void>::Failure(reason.HasFailed() ? reason.Status().Error()
                                                                : linked.Error());
            }

            return Reject(out.reason_, reason.View());
        }

        linkOf[module] = &unit.linked;
        if (auto pushed = linking.TryPushBack(&unit.linked); !pushed) {
            return pushed;
        }
    }

    {
        StringBuilder<TestReasonInlineBytes> reason(allocator);
        if (auto imported = LinkImports(linking.View(), reason); !imported) {
            if (imported.Error() != ErrorCode::InvalidArgument || reason.HasFailed()) {
                return Result<void>::Failure(reason.HasFailed() ? reason.Status().Error()
                                                                : imported.Error());
            }

            return Reject(out.reason_, reason.View());
        }
    }

    const Span<const Symbol> symbols = compilation.Symbols();
    Array<TestEntry> tests(allocator);
    for (const uint32 module : modules) {
        if (module >= compilation.ModuleCount()) {
            return Reject(out.reason_, "a module number is out of range");
        }

        // 그 모듈의 `@test` 최상위 함수를 원문 위치 순으로(삽입 정렬 — 모듈당 시험은 적다).
        tests.Clear();
        const SyntaxTree* tree = compilation.ModuleTree(module);
        for (uint32 symbol = 0; symbol < symbols.Size(); ++symbol) {
            const Symbol& entry = symbols[symbol];
            if (entry.kind != SymbolKind::Function || entry.module != module ||
                entry.owner != NoSymbol || !compilation.HasAttribute(symbol, "test")) {
                continue;
            }

            TestEntry test;
            test.symbol = symbol;
            test.begin = tree != nullptr && entry.nameNode < tree->NodeCount()
                             ? tree->Node(entry.nameNode).span.begin
                             : 0;
            if (auto pushed = tests.TryPushBack(test); !pushed) {
                return pushed;
            }

            for (usize at = tests.Size() - 1; at > 0 && tests[at - 1].begin > tests[at].begin;
                 --at) {
                const TestEntry moved = tests[at];
                tests[at] = tests[at - 1];
                tests[at - 1] = moved;
            }
        }

        const VmLinkedModule& link = *linkOf[module];
        const BytecodeModule& code = link.Module();
        for (const TestEntry& test : tests) {
            const Symbol& entry = symbols[test.symbol];
            uint32 function = NoBytecodeIndex;
            for (uint32 index = 0; index < code.functions.Size(); ++index) {
                function = code.String(code.functions[index].name) == entry.name ? index : function;
            }

            if (function == NoBytecodeIndex) {
                return Reject(out.reason_, "a test function has no bytecode function");
            }

            if (auto pushed = out.results_.TryPushBack(TestResult(*out.allocator_)); !pushed) {
                return pushed;
            }

            TestResult& result = out.results_[out.results_.Size() - 1];
            result.module = module;
            result.symbol = test.symbol;
            result.source = compilation.ModuleSource(module);
            if (tree != nullptr && entry.nameNode < tree->NodeCount()) {
                result.span = tree->Node(entry.nameNode).span;
            }

            // 시험 하나: 새 session(모든 모듈), 최상위 진입, 정지하면 프레임 진행.
            host.ClearRefused();
            VmSession session(allocator, &host);
            for (const UniquePtr<TestUnit>& unit : units) {
                if (auto added = session.AddModule(unit->linked); !added) {
                    return added;
                }
            }

            auto called = session.CallFunction(link, function, Span<const uint64>(),
                                               Span<const uint64>(), Span<VmBuffer* const>());
            if (!called) {
                if (called.Error() != ErrorCode::InvalidArgument) {
                    return Result<void>::Failure(called.Error());
                }

                return Reject(out.reason_, "the VM rejected a test call");
            }

            VmCallResult final = called.Value();
            // 이 시험 실행의 끝(하위 실행이 아닌 재개 기록 중 정지가 아닌 것)을 찾는다.
            auto finished = [&]() noexcept {
                for (const VmSession::Delivery& delivery : session.Deliveries()) {
                    if (delivery.resumed && !delivery.child && delivery.subscriber == 0 &&
                        delivery.module == &link && delivery.function == function &&
                        delivery.result.outcome != VmOutcome::Suspended) {
                        final = delivery.result;
                        return true;
                    }
                }

                return false;
            };
            while (final.outcome == VmOutcome::Suspended && result.frames < options.maxFrames) {
                ++result.frames;
                if (auto stepped = session.FixedStep(); !stepped) {
                    return stepped;
                }

                if (finished()) {
                    break;
                }

                if (auto advanced = session.Advance(TestFrameSeconds, TestFrameSeconds);
                    !advanced) {
                    return advanced;
                }

                if (finished()) {
                    break;
                }

                if (auto applied = session.ApplyRequests(); !applied) {
                    return applied;
                }

                if (finished()) {
                    break;
                }

                if (auto ended = session.EndFrame(); !ended) {
                    return ended;
                }

                if (finished()) {
                    break;
                }

                // 끝 기록 없이 정지 실행이 사라지면(취소 — 엔진 fixture의 Detach·reload 등) 결과를
                // 알 수 없다: 실행기 불변식 위반으로 알린다(조용히 DidNotFinish가 되지 않게).
                if (session.SuspendedCount() == 0) {
                    return Reject(out.reason_, "a test execution was cancelled without a result");
                }
            }

            switch (final.outcome) {
            case VmOutcome::Returned:
                result.outcome = TestOutcome::Passed;
                break;
            case VmOutcome::Faulted: {
                result.fault = final.fault.kind;
                result.outcome =
                    host.HasRefused() && final.fault.kind == VmFaultKind::ContractViolation
                        ? TestOutcome::NeedsEngine
                        : TestOutcome::Faulted;
                if (result.outcome == TestOutcome::NeedsEngine) {
                    if (auto assigned = result.detail.TryAssign(host.Refused()); !assigned) {
                        return assigned;
                    }
                }

                // fault 자리(그 함수의 모듈 원문)의 토큰. 없으면 시험 이름 그대로.
                for (uint32 at = 0; at < linkOf.Size(); ++at) {
                    if (linkOf[at] == final.fault.module) {
                        const SourceId source = compilation.ModuleSource(at);
                        SourceSpan span;
                        if (TestSpanAt(sources.Get(source), compilation.ModuleTree(at),
                                       final.fault.location, span)) {
                            result.source = source;
                            result.span = span;
                        }
                    }
                }

                break;
            }
            case VmOutcome::Ended:
                result.outcome = TestOutcome::EndedWithError;
                result.errorCode = final.errorCode;
                break;
            case VmOutcome::Suspended:
                result.outcome = TestOutcome::DidNotFinish;
                break;
            }

            if (host.RanOutOfMemory()) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            // 닫으면 남은 정지 실행·대기를 취소한다. 뒤의 ledger는 0이어야 한다(9.3절).
            if (auto closed = session.CloseSession(); !closed) {
                return closed;
            }

            session.Execution().ReleaseReturned();
            if (session.Execution().LiveBuffers() != 0) {
                return Reject(out.reason_, "a test left live buffers after the session closed");
            }
        }
    }

    return Result<void>::Success();
}

StringView TestOutcomeName(const TestResult& result) noexcept {
    switch (result.outcome) {
    case TestOutcome::Passed:
        return "Passed";
    case TestOutcome::Faulted:
        return VmFaultKindName(result.fault);
    case TestOutcome::EndedWithError:
        return "EndedWithError";
    case TestOutcome::NeedsEngine:
        return "NeedsEngine";
    case TestOutcome::DidNotFinish:
        return "DidNotFinish";
    }

    return "Passed";
}

Result<void> AppendTestReport(StringBuilderBase& out, const Compilation& compilation,
                              const SourceSet& sources, const TestResult& result) noexcept {
    const StringView name = compilation.Symbols()[result.symbol].name;
    StringBuilder<TestReasonInlineBytes> message(out.GetAllocator());
    message.AppendFormat("Test '{}' failed.", name);
    StringBuilder<TestReasonInlineBytes> explanation(out.GetAllocator());
    switch (result.outcome) {
    case TestOutcome::Passed:
        break;
    case TestOutcome::Faulted:
        if (result.fault == VmFaultKind::AssertFailed) {
            explanation.Append("The assertion is false.");
        } else {
            explanation.AppendFormat("The test stopped with the fault '{}'.",
                                     VmFaultKindName(result.fault));
        }

        break;
    case TestOutcome::EndedWithError: {
        // 오류 코드는 `Error` case 번호 + 1이다(SparksBytecode 2절) — schema의 case 이름으로
        // 보인다.
        StringView caseName;
        for (const Symbol& symbol : compilation.Symbols()) {
            if (symbol.kind != SymbolKind::EngineType || symbol.name != "Error" ||
                !compilation.SchemaIsValid(symbol.index)) {
                continue;
            }

            const BindingSchema& schema = compilation.Schema(symbol.index);
            const SchemaType& type = schema.Types()[symbol.item];
            if (type.kind == SchemaTypeKind::Enum && result.errorCode >= 1 &&
                result.errorCode <= type.caseCount) {
                caseName = schema.Text(schema.Cases()[type.firstCase + result.errorCode - 1].name);
            }

            break;
        }

        if (caseName.IsEmpty()) {
            explanation.AppendFormat("A 'try' failed with error code {}.", result.errorCode);
        } else {
            explanation.AppendFormat("A 'try' failed with 'Error.{}'.", caseName);
        }

        break;
    }
    case TestOutcome::NeedsEngine:
        explanation.AppendFormat("'{}' needs the engine; tests run on the headless host until "
                                 "engine integration (EI).",
                                 result.detail.View());
        break;
    case TestOutcome::DidNotFinish:
        explanation.AppendFormat("The test was still waiting after {} frames.", result.frames);
        break;
    }

    if (message.HasFailed()) {
        return Result<void>::Failure(message.Status().Error());
    }

    if (explanation.HasFailed()) {
        return Result<void>::Failure(explanation.Status().Error());
    }

    return AppendSourceReport(out, sources, result.source, true, result.span,
                              TestOutcomeName(result), message.View(), explanation.View(),
                              StringView());
}

} // namespace rocks::sparks::testing
