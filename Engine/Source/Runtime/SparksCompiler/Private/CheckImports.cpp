#include <Core/Algorithms.h>
#include <Core/Text/EditDistance.h>

#include "CompilationChecker.h"

// 모듈 구문 분석과 import 해석이다(Docs/Design/SparksLanguage.ko.md 3.4절 "import 해석",
// S0301–S0305).

namespace rocks::sparks::detail {

namespace {

// 모르는 모듈에 비슷한 모듈을 제안하는 편집 거리 문턱이다.
constexpr usize MaxModuleSuggestionDistance = 2;

// 어휘 진단(S01xx)의 번호 범위다.
constexpr uint32 FirstLexicalCode = 101;
constexpr uint32 LastLexicalCode = 199;

// lexicalErrors_의 열쇠다(원문 번호가 위, 위치가 아래 32bit).
constexpr uint32 LexicalKeySourceShift = 32;

constexpr uint64 LexicalKey(SourceId source, uint32 offset) noexcept {
    return (static_cast<uint64>(source.index) << LexicalKeySourceShift) | offset;
}

} // namespace

void CompilationChecker::ParseModules() noexcept {
    for (uint32 index = 0; index < compilation_.modules_.Size() && error_ == ErrorCode::None;
         ++index) {
        auto& module = compilation_.modules_[index];
        if (!compilation_.packages_[module.package].valid ||
            !compilation_.sources_->Get(module.source).IsUsable()) {
            continue;
        }

        if (auto parsed = ParseFile(*compilation_.sources_, module.source, diagnostics_,
                                    compilation_.trees_[index]);
            !parsed) {
            error_ = parsed.Error();
            return;
        }

        module.parsed = true;
    }
}

void CompilationChecker::CollectLexicalErrors() noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    for (const Diagnostic& diagnostic : diagnostics_.View()) {
        const uint32 code = static_cast<uint32>(diagnostic.Code());
        if (code < FirstLexicalCode || code > LastLexicalCode || !diagnostic.HasSpan()) {
            continue;
        }

        if (auto pushed = lexicalErrors_.TryPushBack(
                LexicalKey(diagnostic.Source(), diagnostic.Span().begin));
            !pushed) {
            error_ = pushed.Error();
            return;
        }
    }

    algo::Sort(lexicalErrors_.View(),
               [](uint64 left, uint64 right) noexcept { return left < right; });
}

bool CompilationChecker::HasLexicalError(SourceId source, SourceSpan span) const noexcept {
    const uint64 first = LexicalKey(source, span.begin);
    usize low = 0;
    usize high = lexicalErrors_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (lexicalErrors_[middle] < first) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < lexicalErrors_.Size() && lexicalErrors_[low] < LexicalKey(source, span.end);
}

StringView CompilationChecker::SuggestModule(uint32 package, StringView name) const noexcept {
    StringView best;
    usize bestDistance = MaxModuleSuggestionDistance + 1;
    const uint32 first = compilation_.sortedFirst_[package];
    for (uint32 index = first; index < first + compilation_.sortedCount_[package]; ++index) {
        const StringView candidate =
            compilation_.Text(compilation_.modules_[compilation_.sortedModules_[index]].importPath);
        const usize distance = EditDistance(name, candidate, MaxModuleSuggestionDistance,
                                            EditDistanceCase::AsciiInsensitive);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }

    return best;
}

void CompilationChecker::ResolveImports(uint32 moduleNumber) noexcept {
    auto& module = compilation_.modules_[moduleNumber];
    module.firstImport = static_cast<uint32>(compilation_.imports_.Size());
    module.importCount = 0;
    if (!module.parsed) {
        return;
    }

    const SyntaxTree& tree = compilation_.trees_[moduleNumber];
    const StringView text = compilation_.sources_->Get(module.source).Text();
    const PackageManifest& manifest = compilation_.packages_[module.package].manifest;
    const uint32 imports = tree.Child(tree.Root(), 0);
    for (const uint32 node : tree.Children(imports)) {
        if (error_ != ErrorCode::None) {
            return;
        }

        if (tree.Node(node).kind != SyntaxKind::Import) {
            continue; // 첫 선언 전의 잡토큰(S0203)
        }

        ImportResolution resolution;
        resolution.node = node;
        const uint32 path = tree.Child(node, 0);
        const SourceSpan span = tree.Node(path).span;

        // 구간 글자를 모은다. 구문 오류가 있는 경로는 판정하지 않는다.
        StringBuilder<CompilationMessageInlineBytes> full(allocator_);
        StringView firstSegment;
        bool complete = true;
        for (const uint32 segment : tree.Children(path)) {
            if (tree.Node(segment).kind != SyntaxKind::Name) {
                complete = false;
                break;
            }

            const SourceSpan name = tree.Tokens()[tree.Node(segment).token].span;
            const StringView piece(text.Data() + name.begin, name.end - name.begin);
            if (firstSegment.IsEmpty()) {
                firstSegment = piece;
            } else {
                full.Append(".");
            }

            full.Append(piece);
        }

        if (!Succeeded(full)) {
            return;
        }

        // 경로 토큰에 어휘 오류(S0105 따위)가 있으면 이미 그 원인을 알렸으므로 판정하지 않는다.
        uint32 target = NoModule;
        bool judged = complete && !HasLexicalError(module.source, span);
        if (judged && firstSegment == EnginePackageName) {
            Report(DiagnosticCode::RocksImport, module.source, span,
                   "The Rocks package is not imported.",
                   "Engine types and the prelude are available in every module.",
                   "Remove this import.");
            judged = false;
        }

        bool isDependency = false;
        if (judged) {
            for (const ManifestName& dependency : manifest.Dependencies()) {
                isDependency = isDependency || manifest.Text(dependency) == firstSegment;
            }
        }

        if (judged && isDependency) {
            const uint32 package = compilation_.FindPackage(firstSegment);
            if (package == NoPackage) {
                judged = false; // 없거나 manifest가 틀린 패키지(S1106 따위로 이미 알렸다)
            } else if (full.ByteCount() == firstSegment.ByteCount()) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat("'{}' is a package, not a module.", firstSegment);
                if (Succeeded(message)) {
                    Report(DiagnosticCode::UnknownModule, module.source, span, message.View(),
                           "An import names one module of a package by its file path.",
                           "Import a module of the package, as in 'import Weapons.Sword;'.");
                }

                judged = false;
            } else {
                const StringView rest(full.View().Data() + firstSegment.ByteCount() + 1,
                                      full.ByteCount() - firstSegment.ByteCount() - 1);
                target = compilation_.FindModule(package, rest);
                if (target == NoModule) {
                    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                    StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
                    message.AppendFormat("Package '{}' has no module '{}'.", firstSegment, rest);
                    const StringView suggestion = SuggestModule(package, rest);
                    if (!suggestion.IsEmpty()) {
                        fix.AppendFormat("Write 'import {}.{};'.", firstSegment, suggestion);
                    } else {
                        fix.Append("Check the file path of the module in that package.");
                    }

                    if (Succeeded(message) && Succeeded(fix)) {
                        Report(DiagnosticCode::UnknownModule, module.source, span, message.View(),
                               "A module's import path is its file path in the package, as in "
                               "'Combat/Score.spark' -> 'Combat.Score'.",
                               fix.View());
                    }

                    judged = false;
                }
            }
        } else if (judged) {
            target = compilation_.FindModule(module.package, full.View());
            if (target == NoModule) {
                const uint32 other = compilation_.FindNamedPackage(firstSegment);
                const bool ownName =
                    other == module.package && full.ByteCount() > firstSegment.ByteCount();
                if (other != NoPackage && other != module.package) {
                    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                    StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
                    message.AppendFormat("Package '{}' is not a dependency of this package.",
                                         firstSegment);
                    fix.AppendFormat("Add \"{}\" to 'dependencies' in SparksPackage.json5.",
                                     firstSegment);
                    if (Succeeded(message) && Succeeded(fix)) {
                        Report(DiagnosticCode::UndeclaredDependency, module.source, span,
                               message.View(),
                               "A package imports only from the packages listed in its manifest "
                               "'dependencies'.",
                               fix.View());
                    }
                } else {
                    // 자기 패키지 이름으로 시작하면 이름을 뗀 경로를 먼저 제안한다.
                    const StringView rest =
                        ownName ? StringView(full.View().Data() + firstSegment.ByteCount() + 1,
                                             full.ByteCount() - firstSegment.ByteCount() - 1)
                                : StringView();
                    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                    StringBuilder<CompilationMessageInlineBytes> fix(allocator_);
                    message.AppendFormat("Unknown module '{}'.", full.View());
                    const StringView suggestion =
                        ownName && compilation_.FindModule(module.package, rest) != NoModule
                            ? rest
                            : SuggestModule(module.package, full.View());
                    if (!suggestion.IsEmpty()) {
                        fix.AppendFormat("Write 'import {};'.", suggestion);
                    } else {
                        fix.Append("Check the file path of the module.");
                    }

                    if (Succeeded(message) && Succeeded(fix)) {
                        Report(DiagnosticCode::UnknownModule, module.source, span, message.View(),
                               ownName ? StringView("Modules of the same package are imported "
                                                    "without the package name.")
                                       : StringView("A module's import path is its file path in "
                                                    "the package, as in 'Combat/Score.spark' -> "
                                                    "'Combat.Score'."),
                               fix.View());
                    }
                }

                judged = false;
            }
        }

        resolution.target = target;
        if (judged && target == moduleNumber) {
            Report(DiagnosticCode::SelfImport, module.source, span,
                   "A module cannot import itself.",
                   "Its own declarations are already visible in it.", "Remove this import.");
            target = NoModule;
        } else if (judged) {
            for (uint32 index = 0; index < module.importCount; ++index) {
                if (compilation_.imports_[module.firstImport + index].module == target) {
                    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                    message.AppendFormat("Module '{}' is already imported.", full.View());
                    if (Succeeded(message)) {
                        Report(DiagnosticCode::DuplicateImport, module.source, span, message.View(),
                               "", "Remove this import.");
                    }

                    target = NoModule;
                    break;
                }
            }
        }

        resolution.module = target;
        if (auto pushed = compilation_.imports_.TryPushBack(resolution); !pushed) {
            error_ = pushed.Error();
            return;
        }

        ++module.importCount;
    }
}

} // namespace rocks::sparks::detail
