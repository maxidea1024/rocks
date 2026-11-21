#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/PackageManifest.h>
#include <SparksCompiler/SyntaxTree.h>

#include "ConformanceInternal.h"

// 사례 하나 실행이다(Docs/Design/SparksLanguage.ko.md 13.1절): 사례를 패키지로 컴파일하고 진단과
// 구문 트리 golden을 기대값과 비교한다.

namespace rocks::sparks::conformance {

namespace {

// 두 텍스트의 첫 다른 줄(1부터)과 그 줄의 내용이다. 같으면 line은 0이다.
struct LineDifference {
    usize line = 0;
    StringView expected;
    StringView actual;
};

StringView LineAt(StringView text, usize& offset) noexcept {
    const usize begin = offset;
    while (offset < text.ByteCount() && text.Data()[offset] != '\n') {
        ++offset;
    }

    const StringView line(text.Data() + begin, offset - begin);
    if (offset < text.ByteCount()) {
        ++offset;
    }

    return line;
}

LineDifference FindLineDifference(StringView expected, StringView actual) noexcept {
    LineDifference difference;
    usize expectedOffset = 0;
    usize actualOffset = 0;
    usize line = 1;
    while (expectedOffset < expected.ByteCount() || actualOffset < actual.ByteCount()) {
        const StringView left = LineAt(expected, expectedOffset);
        const StringView right = LineAt(actual, actualOffset);
        if (left != right) {
            difference.line = line;
            difference.expected = left;
            difference.actual = right;
            return difference;
        }

        ++line;
    }

    // 줄 내용은 같고 끝의 LF만 다르다.
    difference.line = line;
    return difference;
}

// 구문 트리 golden을 비교하고 다르면 실패 항목을 더한다.
Result<void> CompareSyntaxTree(Allocator& allocator, const CaseInput& input,
                               const SourceSet& sources, const SyntaxTree* tree,
                               Report& report) noexcept {
    StringBuilder<detail::MessageInlineBytes> message(allocator);
    if (tree == nullptr) {
        if (auto formatted = message.TryAppendFormat(
                "{}: has a .ast file but the source check stopped before parsing", input.name);
            !formatted) {
            return formatted;
        }

        return report.TryAddFailure(message.View());
    }

    StringBuilder<detail::MessageInlineBytes> dump(allocator);
    if (auto dumped = AppendSyntaxDump(dump, sources, *tree); !dumped) {
        return dumped;
    }

    if (dump.View() == input.syntaxTree) {
        return Result<void>::Success();
    }

    const LineDifference difference = FindLineDifference(input.syntaxTree, dump.View());
    if (auto formatted = message.TryAppendFormat("{}: syntax tree differs from .ast at line {}\n"
                                                 "  expected: {}\n  actual:   {}\n  actual tree:\n",
                                                 input.name, difference.line, difference.expected,
                                                 difference.actual);
        !formatted) {
        return formatted;
    }

    if (auto appended = message.TryAppend(dump.View()); !appended) {
        return appended;
    }

    return report.TryAddFailure(message.View());
}

// 덤프 golden의 종류다.
enum class DumpKind : uint8 {
    Names,
    Types,
    Ir,
    Bytecode
};

// 덤프 golden(`.names`·`.types`·`.ir`·`.bc`)을 비교하고 다르면 실패 항목을 더한다. 파일 사례는 그
// 모듈의 덤프, 폴더 사례는 분석한 모듈마다 "== <경로>" 줄과 덤프를 경로 byte 순으로 이은
// 것이다(13.1절).
Result<void> CompareDump(Allocator& allocator, const CaseInput& input, const SourceSet& sources,
                         const Compilation& compilation, uint32 mainModule, CompilationStage stage,
                         DumpKind kind, Report& report) noexcept {
    StringBuilder<detail::MessageInlineBytes> message(allocator);
    StringBuilder<detail::MessageInlineBytes> dump(allocator);
    const bool types = kind == DumpKind::Types;
    const bool lowered = kind == DumpKind::Ir || kind == DumpKind::Bytecode;
    const StringView expected = kind == DumpKind::Ir         ? input.ir
                                : kind == DumpKind::Bytecode ? input.bytecode
                                : types                      ? input.types
                                                             : input.names;
    const StringView label = kind == DumpKind::Ir         ? StringView(".ir")
                             : kind == DumpKind::Bytecode ? StringView(".bc")
                             : types                      ? StringView(".types")
                                                          : StringView(".names");
    const StringView noun = kind == DumpKind::Ir         ? StringView("ir")
                            : kind == DumpKind::Bytecode ? StringView("bytecode")
                            : types                      ? StringView("types")
                                                         : StringView("names");
    auto append = [&](StringBuilderBase& out, uint32 module) noexcept {
        return kind == DumpKind::Ir         ? compilation.AppendIrDump(out, module)
               : kind == DumpKind::Bytecode ? compilation.AppendBytecodeDump(out, module)
               : types                      ? compilation.AppendTypeDump(out, module)
                                            : compilation.AppendNameDump(out, module);
    };
    const CompilationStage needed = lowered ? CompilationStage::Codegen
                                    : types ? CompilationStage::Types
                                            : CompilationStage::Names;
    bool resolved = stage >= needed;
    if (resolved && !input.folder) {
        auto dumped = append(dump, mainModule);
        if (!dumped && dumped.Error() != ErrorCode::InvalidArgument) {
            return dumped;
        }

        resolved = static_cast<bool>(dumped);
    } else if (resolved) {
        Array<uint32> modules(allocator);
        for (uint32 module = 0; module < compilation.ModuleCount(); ++module) {
            if (auto pushed = modules.TryPushBack(module); !pushed) {
                return pushed;
            }
        }

        algo::Sort(modules.View(), [&](uint32 left, uint32 right) noexcept {
            return StringView::CompareBytes(sources.Get(compilation.ModuleSource(left)).Path(),
                                            sources.Get(compilation.ModuleSource(right)).Path()) <
                   0;
        });
        for (const uint32 module : modules) {
            StringBuilder<detail::MessageInlineBytes> one(allocator);
            auto dumped = append(one, module);
            if (!dumped) {
                if (dumped.Error() != ErrorCode::InvalidArgument) {
                    return dumped;
                }

                continue; // 이름 해석을 하지 않은 모듈(manifest 오류 등)
            }

            if (auto formatted = dump.TryAppendFormat(
                    "== {}\n", sources.Get(compilation.ModuleSource(module)).Path());
                !formatted) {
                return formatted;
            }

            if (auto appended = dump.TryAppend(one.View()); !appended) {
                return appended;
            }
        }
    }

    if (!resolved) {
        if (auto formatted = message.TryAppendFormat(
                "{}: has a {} file but the module was not {}", input.name, label,
                lowered ? StringView("lowered to IR (it has diagnostics)")
                : types ? StringView("type-checked")
                        : StringView("name-resolved"));
            !formatted) {
            return formatted;
        }

        return report.TryAddFailure(message.View());
    }

    if (dump.View() == expected) {
        return Result<void>::Success();
    }

    const LineDifference difference = FindLineDifference(expected, dump.View());
    if (auto formatted = message.TryAppendFormat("{}: {} differ from {} at line {}\n"
                                                 "  expected: {}\n  actual:   {}\n  actual {}:\n",
                                                 input.name, noun, label, difference.line,
                                                 difference.expected, difference.actual, noun);
        !formatted) {
        return formatted;
    }

    if (auto appended = message.TryAppend(dump.View()); !appended) {
        return appended;
    }

    return report.TryAddFailure(message.View());
}

Result<void> AddCaseFailure(Allocator& allocator, Report& report, StringView caseName,
                            StringView reason) noexcept {
    StringBuilder<detail::MessageInlineBytes> message(allocator);
    if (auto formatted = message.TryAppendFormat("{}: {}", caseName, reason); !formatted) {
        return formatted;
    }

    return report.TryAddFailure(message.View());
}

// 폴더 사례의 패키지 하나다: 경로 앞부분("" 또는 "Game/")과 패키지 번호.
struct PackageRoot {
    StringView prefix;
    uint32 package = 0;
};

// 폴더 사례의 파일을 패키지·모듈로 넣는다. 배치가 틀리면 reason에 이유를 쓰고 false.
Result<bool> AddFolderCase(Allocator& allocator, const CaseInput& input, SourceSet& sources,
                           DiagnosticList& diagnostics, Compilation& compilation,
                           StringView& reason) noexcept {
    bool rootPackage = false;
    for (const CaseFile& file : input.files) {
        rootPackage = rootPackage || file.path == ManifestFileName;
    }

    Array<PackageRoot> roots(allocator);
    for (const CaseFile& file : input.files) {
        StringView prefix;
        if (rootPackage) {
            if (file.path != ManifestFileName) {
                continue;
            }
        } else {
            // "<패키지>/SparksPackage.json5"
            const usize slash = file.path.Find("/", StringComparison::CaseSensitive);
            if (slash == StringView::NotFound ||
                StringView(file.path.Data() + slash + 1, file.path.ByteCount() - slash - 1) !=
                    ManifestFileName) {
                continue;
            }

            prefix = StringView(file.path.Data(), slash + 1);
        }

        auto id = sources.TryAdd(file.path, file.text, diagnostics);
        if (!id) {
            return Result<bool>::Failure(id.Error());
        }

        auto package = compilation.AddPackage(id.Value());
        if (!package) {
            return Result<bool>::Failure(package.Error());
        }

        if (auto pushed = roots.TryPushBack(PackageRoot{prefix, package.Value()}); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    if (roots.IsEmpty()) {
        reason = "the case folder has no SparksPackage.json5";
        return Result<bool>::Success(false);
    }

    for (const CaseFile& file : input.files) {
        if (file.path.EndsWith(ManifestFileName, StringComparison::CaseSensitive)) {
            continue;
        }

        const PackageRoot* owner = nullptr;
        for (const PackageRoot& root : roots) {
            if (file.path.StartsWith(root.prefix, StringComparison::CaseSensitive)) {
                owner = &root;
            }
        }

        if (owner == nullptr) {
            reason = "a file is outside every package folder";
            return Result<bool>::Success(false);
        }

        auto id = sources.TryAdd(file.path, file.text, diagnostics);
        if (!id) {
            return Result<bool>::Failure(id.Error());
        }

        const StringView modulePath(file.path.Data() + owner->prefix.ByteCount(),
                                    file.path.ByteCount() - owner->prefix.ByteCount());
        if (auto added = compilation.AddModule(owner->package, id.Value(), modulePath); !added) {
            return Result<bool>::Failure(added.Error());
        }
    }

    return Result<bool>::Success(true);
}

} // namespace

Result<void> RunCase(Allocator& allocator, const CaseInput& input, Report& report) noexcept {
    report.CountCase();
    const StringView caseName = input.name;
    StringBuilder<detail::MessageInlineBytes> message(allocator);
    Expectation expectation(allocator);
    String problem(allocator);
    auto parsed = TryParseExpectation(input.expect, expectation, problem);
    if (!parsed) {
        return Result<void>::Failure(parsed.Error());
    }

    if (!parsed.Value()) {
        if (auto formatted =
                message.TryAppendFormat("{}: invalid .expect: {}", caseName, problem.View());
            !formatted) {
            return formatted;
        }

        return report.TryAddFailure(message.View());
    }

    for (const ExpectedDiagnostic& expected : expectation.Diagnostics()) {
        if (input.folder && expected.path.IsEmpty()) {
            return AddCaseFailure(allocator, report, caseName,
                                  "invalid .expect: a folder case names the file of every "
                                  "diagnostic");
        }

        if (!input.folder && !expected.path.IsEmpty()) {
            return AddCaseFailure(allocator, report, caseName,
                                  "invalid .expect: a file case does not name files");
        }
    }

    // 컴파일: schema → 패키지(manifest·모듈) → Check.
    SourceSet sources(allocator);
    DiagnosticList diagnostics(allocator);
    Compilation compilation(allocator, sources);
    for (const CaseFile& schema : input.schemas) {
        auto id = sources.TryAdd(schema.path, schema.text, diagnostics);
        if (!id) {
            return Result<void>::Failure(id.Error());
        }

        if (auto added = compilation.AddBindingSchema(id.Value()); !added) {
            return added;
        }
    }

    uint32 mainModule = NoModule;
    if (input.folder) {
        StringView reason;
        auto added = AddFolderCase(allocator, input, sources, diagnostics, compilation, reason);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        if (!added.Value()) {
            return AddCaseFailure(allocator, report, caseName, reason);
        }
    } else {
        auto manifest = sources.TryAdd(ManifestFileName, detail::FileCaseManifest, diagnostics);
        if (!manifest) {
            return Result<void>::Failure(manifest.Error());
        }

        auto package = compilation.AddPackage(manifest.Value());
        if (!package) {
            return Result<void>::Failure(package.Error());
        }

        auto source = sources.TryAdd(caseName, input.source, diagnostics);
        if (!source) {
            return Result<void>::Failure(source.Error());
        }

        auto module =
            compilation.AddModule(package.Value(), source.Value(), detail::FileCaseModulePath);
        if (!module) {
            return Result<void>::Failure(module.Error());
        }

        mainModule = module.Value();
    }

    if (auto staged = compilation.SetLastStage(expectation.Stage()); !staged) {
        return staged;
    }

    if (auto checked = compilation.Check(diagnostics); !checked) {
        return checked;
    }

    if (input.hasSyntaxTree && !input.folder) {
        if (auto compared = CompareSyntaxTree(allocator, input, sources,
                                              compilation.ModuleTree(mainModule), report);
            !compared) {
            return compared;
        }
    }

    if (input.hasNames) {
        if (auto compared = CompareDump(allocator, input, sources, compilation, mainModule,
                                        expectation.Stage(), DumpKind::Names, report);
            !compared) {
            return compared;
        }
    }

    if (input.hasTypes) {
        if (auto compared = CompareDump(allocator, input, sources, compilation, mainModule,
                                        expectation.Stage(), DumpKind::Types, report);
            !compared) {
            return compared;
        }
    }

    if (input.hasIr) {
        if (auto compared = CompareDump(allocator, input, sources, compilation, mainModule,
                                        expectation.Stage(), DumpKind::Ir, report);
            !compared) {
            return compared;
        }
    }

    if (input.hasBytecode) {
        if (auto compared = CompareDump(allocator, input, sources, compilation, mainModule,
                                        expectation.Stage(), DumpKind::Bytecode, report);
            !compared) {
            return compared;
        }
    }

    diagnostics.Sort(sources);
    const Span<const ExpectedDiagnostic> expected = expectation.Diagnostics();
    bool same = expected.Size() == diagnostics.Count();
    for (usize index = 0; same && index < expected.Size(); ++index) {
        const Diagnostic& actual = diagnostics[index];
        const SourceFile& file = sources.Get(actual.Source());
        const StringView expectedPath = input.folder ? expected[index].path : caseName;
        if (static_cast<uint16>(actual.Code()) != expected[index].code ||
            file.Path() != expectedPath) {
            same = false;
        } else if (!actual.HasSpan()) {
            same = expected[index].line == 0;
        } else {
            const SourceLocation location = file.Locate(actual.Span().begin);
            same =
                location.line == expected[index].line && location.column == expected[index].column;
        }
    }

    if (same) {
        return Result<void>::Success();
    }

    if (auto formatted =
            message.TryAppendFormat("{}: diagnostics differ from .expect\n  expected:\n", caseName);
        !formatted) {
        return formatted;
    }

    if (expected.IsEmpty()) {
        if (auto appended = message.TryAppend("    clean\n"); !appended) {
            return appended;
        }
    }

    for (const ExpectedDiagnostic& item : expected) {
        if (auto appended = detail::FormatExpected(message, item); !appended) {
            return appended;
        }
    }

    if (auto appended = message.TryAppend("  actual:\n"); !appended) {
        return appended;
    }

    if (diagnostics.IsEmpty()) {
        if (auto appended = message.TryAppend("    (no diagnostics)\n"); !appended) {
            return appended;
        }
    }

    if (auto appended = AppendDiagnostics(message, sources, diagnostics); !appended) {
        return appended;
    }

    return report.TryAddFailure(message.View());
}

} // namespace rocks::sparks::conformance
