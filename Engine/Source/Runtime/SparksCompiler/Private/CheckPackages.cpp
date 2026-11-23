#include <Core/Algorithms.h>

#include "CompilationChecker.h"
#include "NameBinder.h"
#include "Names.h"

// 패키지 검사다: binding schema·manifest 읽기, 패키지 이름 중복(S1112), 모듈 경로(S1108–S1110),
// 의존(S1106·S1111), 순환(S1107). 규칙: Docs/Design/SparksLanguage 3.4절, 10.2절.

namespace rocks::sparks::detail {

namespace {

constexpr StringView ModuleExtension = ".spark";

// 스키마 안 이름·ID·모듈 하나의 쓰임이다(파일 사이 중복 검사). 글자는 그 schema가 가진다(Check
// 동안 바뀌지 않는다).
struct SchemaUse {
    StringView text;
    SourceSpan span;
};

// 의존 간선 하나다(유효한 패키지 사이, manifest 순서).
struct DependencyEdge {
    uint32 to = 0;
    uint32 dependency = 0; // 그 manifest의 Dependencies() 번호
};

// 경로 접두(폴더 또는 모듈) 하나다. 글자는 지역 pool에 있다.
struct PathPrefix {
    uint32 lowerOffset = 0;
    uint32 exactOffset = 0;
    uint32 length = 0;
    uint32 module = 0;
};

// path가 모듈 경로 규칙에 맞으면 import 경로(`Combat.Score`)를 out에 쓰고 true다.
bool WriteImportPath(StringView path, StringBuilderBase& out) noexcept {
    if (path.ByteCount() <= ModuleExtension.ByteCount() ||
        StringView(path.Data() + path.ByteCount() - ModuleExtension.ByteCount(),
                   ModuleExtension.ByteCount()) != ModuleExtension) {
        return false;
    }

    const StringView stem(path.Data(), path.ByteCount() - ModuleExtension.ByteCount());
    usize begin = 0;
    for (usize index = 0; index <= stem.ByteCount(); ++index) {
        if (index != stem.ByteCount() && stem.Data()[index] != '/') {
            continue;
        }

        const StringView segment(stem.Data() + begin, index - begin);
        if (!detail::IsPascalName(segment)) {
            return false;
        }

        if (begin != 0) {
            out.Append(".");
        }

        out.Append(segment);
        begin = index + 1;
    }

    return true;
}

// import 경로의 첫 구간이다.
StringView FirstSegment(StringView importPath) noexcept {
    for (usize index = 0; index < importPath.ByteCount(); ++index) {
        if (importPath.Data()[index] == '.') {
            return StringView(importPath.Data(), index);
        }
    }

    return importPath;
}

// 진단 문장에 넣어도 되는 글자(인쇄 가능한 ASCII)인지다.
bool IsPrintableAscii(StringView text) noexcept {
    constexpr char FirstPrintable = 0x20;
    constexpr char LastPrintable = 0x7E;
    for (usize index = 0; index < text.ByteCount(); ++index) {
        if (text.Data()[index] < FirstPrintable || text.Data()[index] > LastPrintable) {
            return false;
        }
    }

    return !text.IsEmpty();
}

char ToLowerAscii(char value) noexcept {
    return detail::IsUpperAsciiLetter(value) ? static_cast<char>(value - 'A' + 'a') : value;
}

// ASCII 대소문자를 접은 byte 순서다.
int CompareIgnoringAsciiCase(StringView left, StringView right) noexcept {
    const usize count = left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    for (usize index = 0; index < count; ++index) {
        const uint8 a = static_cast<uint8>(ToLowerAscii(left.Data()[index]));
        const uint8 b = static_cast<uint8>(ToLowerAscii(right.Data()[index]));
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }

    if (left.ByteCount() == right.ByteCount()) {
        return 0;
    }

    return left.ByteCount() < right.ByteCount() ? -1 : 1;
}

} // namespace

CompilationChecker::CompilationChecker(Compilation& compilation,
                                       DiagnosticList& diagnostics) noexcept
    : compilation_(compilation), diagnostics_(diagnostics), allocator_(*compilation.allocator_),
      lexicalErrors_(*compilation.allocator_) {
}

Result<void> CompilationChecker::Run() noexcept {
    ReadSchemas();
    CheckSchemaDuplicates();
    ReadManifests();
    CheckPackageNames();
    BuildPackageIndex();
    const usize packageCount = compilation_.packages_.Size();
    if (error_ == ErrorCode::None) {
        if (auto resized = compilation_.sortedFirst_.TryResize(packageCount); !resized) {
            return resized;
        }

        if (auto resized = compilation_.sortedCount_.TryResize(packageCount); !resized) {
            return resized;
        }

        for (usize package = 0; package < packageCount; ++package) {
            compilation_.sortedFirst_[package] = 0;
            compilation_.sortedCount_[package] = 0;
        }
    }

    for (uint32 package = 0; package < packageCount && error_ == ErrorCode::None; ++package) {
        if (!compilation_.packages_[package].valid) {
            continue;
        }

        CheckModulePaths(package);
        CheckPathCase(package);
        BuildModuleIndex(package);
        CheckModuleFolders(package);
        CheckDependencies(package);
    }

    CheckCycles();
    ParseModules();
    if (compilation_.lastStage_ == CompilationStage::Syntax) {
        if (error_ != ErrorCode::None) {
            return Result<void>::Failure(error_);
        }

        // 결과 접근이 쓰는 import 구간을 비운 채로 둔다.
        for (auto& module : compilation_.modules_) {
            module.firstImport = 0;
            module.importCount = 0;
        }

        return Result<void>::Success();
    }

    CollectLexicalErrors();
    for (uint32 module = 0; module < compilation_.modules_.Size() && error_ == ErrorCode::None;
         ++module) {
        ResolveImports(module);
    }

    if (error_ != ErrorCode::None) {
        return Result<void>::Failure(error_);
    }

    if (compilation_.lastStage_ == CompilationStage::Imports) {
        return Result<void>::Success();
    }

    NameBinder binder(compilation_, diagnostics_);
    if (auto bound = binder.Run(); !bound) {
        return bound;
    }

    return Result<void>::Success();
}

void CompilationChecker::Report(DiagnosticCode code, SourceId source, SourceSpan span,
                                StringView message, StringView explanation,
                                StringView fix) noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    if (auto added = diagnostics_.TryAdd(code, source, span, message, explanation, fix); !added) {
        error_ = added.Error();
    }
}

void CompilationChecker::ReportFile(DiagnosticCode code, SourceId source, StringView message,
                                    StringView explanation, StringView fix) noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    if (auto added = diagnostics_.TryAddFileLevel(code, source, message, explanation, fix);
        !added) {
        error_ = added.Error();
    }
}

bool CompilationChecker::Succeeded(const StringBuilderBase& builder) noexcept {
    if (builder.HasFailed()) {
        error_ = builder.Status().Error();
        return false;
    }

    return error_ == ErrorCode::None;
}

void CompilationChecker::ReadSchemas() noexcept {
    for (auto& entry : compilation_.schemas_) {
        if (error_ != ErrorCode::None) {
            return;
        }

        if (!compilation_.sources_->Get(entry.source).IsUsable()) {
            continue;
        }

        auto read =
            ReadBindingSchema(*compilation_.sources_, entry.source, diagnostics_, entry.schema);
        if (!read) {
            error_ = read.Error();
            return;
        }

        entry.valid = read.Value();
    }
}

// 여러 schema 파일 사이의 타입 이름(한 패키지)·ID·모듈 이름 중복이다. 더한 순서로 훑으며 앞서
// 받아들인 파일하고만 비교한다(이미 거부한 파일과 겹치는 것은 문제가 아니다). 겹치는 것마다 그
// 자리에 진단을 내고 그 파일을 거부한다(이름이 둘을 가리키지 않게).
void CompilationChecker::CheckSchemaDuplicates() noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    // 받아들인 파일들의 이름·ID·모듈이다(각각 글자 byte 순 정렬).
    Array<SchemaUse> acceptedNames(allocator_);
    Array<SchemaUse> acceptedIds(allocator_);
    Array<SchemaUse> acceptedModules(allocator_);
    Array<SchemaUse> names(allocator_);
    Array<SchemaUse> ids(allocator_);
    Array<SchemaUse> modules(allocator_);
    auto byText = [](const SchemaUse& left, const SchemaUse& right) noexcept {
        return StringView::CompareBytes(left.text, right.text) < 0;
    };
    // 정렬된 list에 use와 같은 글자가 있는지다.
    auto contains = [](const Array<SchemaUse>& list, const SchemaUse& use) noexcept {
        usize low = 0;
        usize high = list.Size();
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            if (StringView::CompareBytes(list[middle].text, use.text) < 0) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        return low < list.Size() && list[low].text == use.text;
    };

    for (uint32 index = 0; index < compilation_.schemas_.Size() && error_ == ErrorCode::None;
         ++index) {
        auto& entry = compilation_.schemas_[index];
        if (!entry.valid) {
            continue;
        }

        const BindingSchema& schema = entry.schema;
        names.Clear();
        ids.Clear();
        modules.Clear();
        auto add = [&](Array<SchemaUse>& list, StringView text, SourceSpan span) noexcept {
            if (text.IsEmpty() || error_ != ErrorCode::None) {
                return;
            }

            if (auto pushed = list.TryPushBack(SchemaUse{text, span}); !pushed) {
                error_ = pushed.Error();
            }
        };
        for (const SchemaType& type : schema.Types()) {
            add(names, schema.Text(type.name), type.span);
            add(ids, schema.Text(type.id), type.idSpan);
        }

        for (const SchemaField& field : schema.Fields()) {
            add(ids, schema.Text(field.id), field.idSpan);
        }

        for (const SchemaFunction& function : schema.Functions()) {
            add(ids, schema.Text(function.id), function.idSpan);
        }

        for (const SchemaEvent& event : schema.Events()) {
            add(ids, schema.Text(event.id), event.idSpan);
        }

        add(modules, schema.Module(), schema.ModuleSpan());

        if (error_ != ErrorCode::None) {
            return;
        }

        bool rejected = false;
        for (const SchemaUse& use : modules) {
            if (contains(acceptedModules, use)) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat(
                    "Module '{}' is also described by another binding schema file.", use.text);
                if (Succeeded(message)) {
                    Report(DiagnosticCode::InvalidBindingSchema, entry.source, use.span,
                           message.View(), "Each C++ module has one binding schema file.",
                           "Regenerate the binding schema; do not edit it by hand.");
                }

                rejected = true;
            }
        }

        for (const SchemaUse& use : names) {
            if (contains(acceptedNames, use)) {
                Report(DiagnosticCode::InvalidBindingSchema, entry.source, use.span,
                       "This type is also declared in another binding schema file.",
                       "The files of a package share one type namespace.",
                       "Regenerate the binding schema; do not edit it by hand.");
                rejected = true;
            }
        }

        for (const SchemaUse& use : ids) {
            if (contains(acceptedIds, use)) {
                Report(DiagnosticCode::InvalidBindingSchema, entry.source, use.span,
                       "This ID is also used in another binding schema file.",
                       "TypeId, FieldId and FunctionId values are unique.",
                       "Regenerate the binding schema; do not edit it by hand.");
                rejected = true;
            }
        }

        if (rejected) {
            entry.valid = false;
            continue;
        }

        // 받아들인 파일의 글자를 더하고 다시 정렬한다(파일 수는 모듈 수 정도로 작다).
        const Array<SchemaUse>* from[] = {&names, &ids, &modules};
        Array<SchemaUse>* to[] = {&acceptedNames, &acceptedIds, &acceptedModules};
        for (usize list = 0; list < 3 && error_ == ErrorCode::None; ++list) {
            if (auto appended = to[list]->TryAppend(from[list]->View()); !appended) {
                error_ = appended.Error();
                return;
            }

            algo::Sort(to[list]->View(), byText);
        }
    }
}

void CompilationChecker::ReadManifests() noexcept {
    for (auto& entry : compilation_.packages_) {
        if (error_ != ErrorCode::None) {
            return;
        }

        if (!compilation_.sources_->Get(entry.source).IsUsable()) {
            continue;
        }

        auto read =
            ReadPackageManifest(*compilation_.sources_, entry.source, diagnostics_, entry.manifest);
        if (!read) {
            error_ = read.Error();
            return;
        }

        entry.valid = read.Value();
    }
}

// 패키지 이름 중복이다. 이름을 읽은 패키지(다른 key에 오류가 있어도 `name` 값이 맞으면 이름이
// 있다)끼리 ASCII 대소문자를 무시하고 비교한다(이름은 폴더·파일 이름이 되고 파일 시스템마다
// 대소문자 구분이 다르므로). 같은 무리에서 처음 더한 패키지가 이름을 갖고, 뒤의 것마다 S1112를 내고
// 무효로 한다(처음 것이 무효여도 그렇다 — 이름이 둘을 가리키지 않게).
void CompilationChecker::CheckPackageNames() noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    Array<uint32> named(allocator_);
    for (uint32 index = 0; index < compilation_.packages_.Size(); ++index) {
        if (compilation_.packages_[index].manifest.Name().IsEmpty()) {
            anyNameUnknown_ = true;
            continue;
        }

        if (auto pushed = named.TryPushBack(index); !pushed) {
            error_ = pushed.Error();
            return;
        }
    }

    Compilation& compilation = compilation_;
    algo::Sort(named.View(), [&compilation](uint32 left, uint32 right) noexcept {
        const int order = CompareIgnoringAsciiCase(compilation.packages_[left].manifest.Name(),
                                                   compilation.packages_[right].manifest.Name());
        return order != 0 ? order < 0 : left < right;
    });
    for (usize index = 1; index < named.Size() && error_ == ErrorCode::None; ++index) {
        auto& entry = compilation_.packages_[named[index]];
        const StringView name = entry.manifest.Name();
        usize first = index;
        while (first > 0 &&
               CompareIgnoringAsciiCase(compilation_.packages_[named[first - 1]].manifest.Name(),
                                        name) == 0) {
            --first;
        }

        if (first == index) {
            continue;
        }

        const StringView original = compilation_.packages_[named[first]].manifest.Name();
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        if (original == name) {
            message.AppendFormat("Package '{}' is defined twice in this compilation.", name);
        } else {
            message.AppendFormat("Package '{}' differs from package '{}' only in letter case.",
                                 name, original);
        }

        if (Succeeded(message)) {
            Report(DiagnosticCode::DuplicatePackage, entry.source, entry.manifest.NameSpan(),
                   message.View(),
                   "Packages are named by their manifest and the name becomes folder and file "
                   "names, so two packages cannot share a name, even in letter case only.",
                   "Rename one of the packages.");
        }

        entry.valid = false;
    }
}

// 이름 있는 패키지를 이름 byte 순(같으면 번호 순)으로 담는다(Compilation::FindNamedPackage).
void CompilationChecker::BuildPackageIndex() noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    auto& index = compilation_.packagesByName_;
    for (uint32 package = 0; package < compilation_.packages_.Size(); ++package) {
        if (compilation_.packages_[package].manifest.Name().IsEmpty()) {
            continue;
        }

        if (auto pushed = index.TryPushBack(package); !pushed) {
            error_ = pushed.Error();
            return;
        }
    }

    Compilation& compilation = compilation_;
    algo::Sort(index.View(), [&compilation](uint32 left, uint32 right) noexcept {
        const int order = StringView::CompareBytes(compilation.packages_[left].manifest.Name(),
                                                   compilation.packages_[right].manifest.Name());
        return order != 0 ? order < 0 : left < right;
    });
}

void CompilationChecker::CheckModulePaths(uint32 package) noexcept {
    for (auto& module : compilation_.modules_) {
        if (module.package != package || error_ != ErrorCode::None) {
            continue;
        }

        const StringView path = compilation_.Text(module.path);
        StringBuilder<CompilationMessageInlineBytes> importPath(allocator_);
        if (WriteImportPath(path, importPath)) {
            if (!Succeeded(importPath)) {
                return;
            }

            module.importPath.offset = static_cast<uint32>(compilation_.text_.Size());
            module.importPath.length = static_cast<uint32>(importPath.ByteCount());
            if (auto appended = compilation_.text_.TryAppend(
                    Span<const char>(importPath.View().Data(), importPath.ByteCount()));
                !appended) {
                error_ = appended.Error();
                return;
            }

            module.validPath = true;
            continue;
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        if (IsPrintableAscii(path)) {
            message.AppendFormat("Module path '{}' is not valid.", path);
        } else {
            message.Append("The module path is not valid.");
        }

        if (Succeeded(message)) {
            ReportFile(DiagnosticCode::InvalidModulePath, module.source, message.View(),
                       "Each folder and file name starts with an uppercase English letter and "
                       "uses only ASCII letters and digits, and the file name ends with '.spark'.",
                       "Rename it, as in 'Combat/Score.spark'.");
        }
    }
}

// 대소문자만 다른 경로 접두(폴더·모듈)다. 같은 소문자 철자 무리에서 byte 순으로 처음인 철자가 아닌
// 쪽의 모듈마다 한 번 낸다.
void CompilationChecker::CheckPathCase(uint32 package) noexcept {
    Array<char> pool(allocator_);
    Array<PathPrefix> prefixes(allocator_);
    for (uint32 index = 0; index < compilation_.modules_.Size() && error_ == ErrorCode::None;
         ++index) {
        const auto& module = compilation_.modules_[index];
        if (module.package != package || !module.validPath) {
            continue;
        }

        const StringView path = compilation_.Text(module.path);
        const StringView stem(path.Data(), path.ByteCount() - ModuleExtension.ByteCount());
        for (usize end = 0; end <= stem.ByteCount(); ++end) {
            if (end != stem.ByteCount() && stem.Data()[end] != '/') {
                continue;
            }

            PathPrefix prefix;
            prefix.lowerOffset = static_cast<uint32>(pool.Size());
            prefix.length = static_cast<uint32>(end);
            prefix.module = index;
            for (usize at = 0; at < end; ++at) {
                if (auto pushed = pool.TryPushBack(ToLowerAscii(stem.Data()[at])); !pushed) {
                    error_ = pushed.Error();
                    return;
                }
            }

            prefix.exactOffset = static_cast<uint32>(pool.Size());
            if (auto appended = pool.TryAppend(Span<const char>(stem.Data(), end)); !appended) {
                error_ = appended.Error();
                return;
            }

            if (auto pushed = prefixes.TryPushBack(prefix); !pushed) {
                error_ = pushed.Error();
                return;
            }
        }
    }

    if (error_ != ErrorCode::None) {
        return;
    }

    auto lower = [&pool](const PathPrefix& prefix) noexcept {
        return StringView(pool.Data() + prefix.lowerOffset, prefix.length);
    };
    auto exact = [&pool](const PathPrefix& prefix) noexcept {
        return StringView(pool.Data() + prefix.exactOffset, prefix.length);
    };
    algo::Sort(prefixes.View(), [&](const PathPrefix& left, const PathPrefix& right) noexcept {
        const int order = StringView::CompareBytes(lower(left), lower(right));
        if (order != 0) {
            return order < 0;
        }

        const int spelling = StringView::CompareBytes(exact(left), exact(right));
        return spelling != 0 ? spelling < 0 : left.module < right.module;
    });

    Array<uint8> reported(allocator_);
    if (auto resized = reported.TryResize(compilation_.modules_.Size()); !resized) {
        error_ = resized.Error();
        return;
    }

    for (usize index = 0; index < reported.Size(); ++index) {
        reported[index] = 0;
    }

    usize groupBegin = 0;
    for (usize index = 1; index <= prefixes.Size(); ++index) {
        if (index != prefixes.Size() && lower(prefixes[index]) == lower(prefixes[groupBegin])) {
            continue;
        }

        const StringView first = exact(prefixes[groupBegin]);
        for (usize at = groupBegin; at < index && error_ == ErrorCode::None; ++at) {
            const PathPrefix& prefix = prefixes[at];
            if (exact(prefix) == first || reported[prefix.module] != 0) {
                continue;
            }

            reported[prefix.module] = 1;
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Path '{}' differs from '{}' only in letter case.", exact(prefix),
                                 first);
            if (Succeeded(message)) {
                ReportFile(DiagnosticCode::ModulePathCaseConflict,
                           compilation_.modules_[prefix.module].source, message.View(),
                           "File systems differ in whether letter case matters, so the two paths "
                           "would name one module on some machines and two on others.",
                           "Rename one of the paths.");
            }
        }

        groupBegin = index;
    }
}

void CompilationChecker::BuildModuleIndex(uint32 package) noexcept {
    const usize first = compilation_.sortedModules_.Size();
    for (uint32 index = 0; index < compilation_.modules_.Size(); ++index) {
        const auto& module = compilation_.modules_[index];
        if (module.package == package && module.validPath) {
            if (auto pushed = compilation_.sortedModules_.TryPushBack(index); !pushed) {
                error_ = pushed.Error();
                return;
            }
        }
    }

    const usize count = compilation_.sortedModules_.Size() - first;
    Compilation& compilation = compilation_;
    algo::Sort(Span<uint32>(compilation_.sortedModules_.Data() + first, count),
               [&compilation](uint32 left, uint32 right) noexcept {
                   return StringView::CompareBytes(
                              compilation.Text(compilation.modules_[left].importPath),
                              compilation.Text(compilation.modules_[right].importPath)) < 0;
               });
    compilation_.sortedFirst_[package] = static_cast<uint32>(first);
    compilation_.sortedCount_[package] = static_cast<uint32>(count);
}

// 같은 폴더의 `X.spark`와 폴더 `X/`다. import 경로 정렬에서 `X` 바로 뒤가 `X.`로 시작하면 겹친다
// ('.'은 영문자·숫자보다 작으므로 `X.…`가 `X0` 따위보다 앞에 온다).
void CompilationChecker::CheckModuleFolders(uint32 package) noexcept {
    const uint32 first = compilation_.sortedFirst_[package];
    const uint32 count = compilation_.sortedCount_[package];
    for (uint32 index = first; index + 1 < first + count && error_ == ErrorCode::None; ++index) {
        const auto& module = compilation_.modules_[compilation_.sortedModules_[index]];
        const StringView name = compilation_.Text(module.importPath);
        const StringView next = compilation_.Text(
            compilation_.modules_[compilation_.sortedModules_[index + 1]].importPath);
        if (next.ByteCount() <= name.ByteCount() ||
            StringView(next.Data(), name.ByteCount()) != name ||
            next.Data()[name.ByteCount()] != '.') {
            continue;
        }

        const StringView path = compilation_.Text(module.path);
        const StringView stem(path.Data(), path.ByteCount() - ModuleExtension.ByteCount());
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("File '{}' has the same name as the folder '{}/'.", path, stem);
        if (Succeeded(message)) {
            ReportFile(DiagnosticCode::ModuleFolderConflict, module.source, message.View(),
                       "Both would have the import path of the file.",
                       "Rename the file or the folder.");
        }
    }
}

void CompilationChecker::CheckDependencies(uint32 package) noexcept {
    const auto& entry = compilation_.packages_[package];
    const PackageManifest& manifest = entry.manifest;
    const uint32 first = compilation_.sortedFirst_[package];
    const uint32 count = compilation_.sortedCount_[package];
    // 최상위 이름(첫 구간)이 name인 첫 모듈(import 경로 byte 순)이다.
    auto topLevel = [&](StringView name) noexcept -> uint32 {
        for (uint32 index = first; index < first + count; ++index) {
            const uint32 module = compilation_.sortedModules_[index];
            if (FirstSegment(compilation_.Text(compilation_.modules_[module].importPath)) == name) {
                return module;
            }
        }

        return NoModule;
    };

    for (const ManifestName& dependency : manifest.Dependencies()) {
        if (error_ != ErrorCode::None) {
            return;
        }

        const StringView name = manifest.Text(dependency);
        // 이름을 읽지 못한 패키지가 있으면 그것이 이 의존일 수 있으므로 S1106을 내지 않는다(그
        // 패키지의 manifest 진단이 이미 있다).
        if (compilation_.FindNamedPackage(name) == NoPackage && !anyNameUnknown_) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Package '{}' is not in this compilation.", name);
            if (Succeeded(message)) {
                Report(DiagnosticCode::MissingDependency, entry.source, dependency.span,
                       message.View(),
                       "A dependency must be a package compiled together with this one.",
                       "Check the package name or add the package.");
            }
        }

        if (topLevel(name) != NoModule) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Dependency '{}' has the same name as a top-level folder or "
                                 "module of this package.",
                                 name);
            if (Succeeded(message)) {
                Report(DiagnosticCode::PackageFolderConflict, entry.source, dependency.span,
                       message.View(),
                       "An import path that starts with this name would mean both the package "
                       "and the folder.",
                       "Rename the folder or the module.");
            }
        }
    }

    const uint32 engine = topLevel(EnginePackageName);
    if (engine != NoModule) {
        ReportFile(DiagnosticCode::PackageFolderConflict, compilation_.modules_[engine].source,
                   "The top-level name 'Rocks' is the engine package name.",
                   "An import path that starts with 'Rocks' would name the engine package.",
                   "Rename the folder or the module.");
    }
}

// 패키지 의존 순환이다. 강하게 연결된 묶음(SCC) 하나에 진단 하나: 묶음에서 이름 byte 순으로 처음인
// 패키지의 manifest에서 묶음 안을 가리키는 첫 의존 항목에 낸다. 묶음은 반복형 Tarjan으로 찾는다
// (O(패키지 + 의존)). 간선은 유효한 패키지 사이만이다(의존 이름의 패키지가 무효면 간선 없음).
void CompilationChecker::CheckCycles() noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    const uint32 count = static_cast<uint32>(compilation_.packages_.Size());
    Array<DependencyEdge> edges(allocator_);
    Array<uint32> edgeFirst(allocator_);
    if (auto resized = edgeFirst.TryResize(count + 1); !resized) {
        error_ = resized.Error();
        return;
    }

    for (uint32 package = 0; package < count; ++package) {
        edgeFirst[package] = static_cast<uint32>(edges.Size());
        if (!compilation_.packages_[package].valid) {
            continue;
        }

        const PackageManifest& manifest = compilation_.packages_[package].manifest;
        const Span<const ManifestName> dependencies = manifest.Dependencies();
        for (uint32 dependency = 0; dependency < dependencies.Size(); ++dependency) {
            const uint32 to =
                compilation_.FindNamedPackage(manifest.Text(dependencies[dependency]));
            if (to == NoPackage || !compilation_.packages_[to].valid) {
                continue;
            }

            if (auto pushed = edges.TryPushBack(DependencyEdge{to, dependency}); !pushed) {
                error_ = pushed.Error();
                return;
            }
        }
    }

    edgeFirst[count] = static_cast<uint32>(edges.Size());

    // Tarjan: order는 방문 번호, low는 닿는 가장 작은 방문 번호, component는 묶음 번호다. 재귀 대신
    // frames(방문 중인 패키지와 다음 간선)를 쓴다(깊은 의존 사슬에서도 stack을 넘지 않게).
    constexpr uint32 Unvisited = NumericLimits<uint32>::Max();
    struct Frame {
        uint32 package = 0;
        uint32 edge = 0;
    };

    Array<uint32> order(allocator_);
    Array<uint32> low(allocator_);
    Array<uint32> component(allocator_);
    Array<uint8> onStack(allocator_);
    Array<uint32> stack(allocator_);
    Array<Frame> frames(allocator_);
    if (!order.TryResize(count) || !low.TryResize(count) || !component.TryResize(count) ||
        !onStack.TryResize(count) || !stack.TryResize(count) || !frames.TryResize(count)) {
        error_ = ErrorCode::OutOfMemory;
        return;
    }

    for (uint32 package = 0; package < count; ++package) {
        order[package] = Unvisited;
        component[package] = Unvisited;
        onStack[package] = 0;
    }

    uint32 visited = 0;
    uint32 components = 0;
    uint32 stackSize = 0;
    uint32 frameCount = 0;
    auto visit = [&](uint32 package) noexcept {
        order[package] = visited;
        low[package] = visited;
        ++visited;
        stack[stackSize++] = package;
        onStack[package] = 1;
        frames[frameCount++] = Frame{package, edgeFirst[package]};
    };
    for (uint32 root = 0; root < count; ++root) {
        if (!compilation_.packages_[root].valid || order[root] != Unvisited) {
            continue;
        }

        visit(root);
        while (frameCount > 0) {
            Frame& frame = frames[frameCount - 1];
            const uint32 package = frame.package;
            if (frame.edge < edgeFirst[package + 1]) {
                const uint32 to = edges[frame.edge].to;
                ++frame.edge;
                if (order[to] == Unvisited) {
                    visit(to);
                } else if (onStack[to] != 0 && order[to] < low[package]) {
                    low[package] = order[to];
                }

                continue;
            }

            --frameCount;
            if (frameCount > 0) {
                const uint32 parent = frames[frameCount - 1].package;
                if (low[package] < low[parent]) {
                    low[parent] = low[package];
                }
            }

            if (low[package] != order[package]) {
                continue;
            }

            uint32 member = Unvisited;
            do {
                member = stack[--stackSize];
                onStack[member] = 0;
                component[member] = components;
            } while (member != package);
            ++components;
        }
    }

    // 묶음마다 크기와 이름이 처음인 패키지다. 자기 의존은 manifest가 막으므로 순환은 둘 이상이다.
    Array<uint32> size(allocator_);
    Array<uint32> leader(allocator_);
    if (!size.TryResize(components) || !leader.TryResize(components)) {
        error_ = ErrorCode::OutOfMemory;
        return;
    }

    for (uint32 index = 0; index < components; ++index) {
        size[index] = 0;
        leader[index] = NoPackage;
    }

    for (uint32 package = 0; package < count; ++package) {
        const uint32 group = component[package];
        if (group == Unvisited) {
            continue;
        }

        ++size[group];
        if (leader[group] == NoPackage ||
            StringView::CompareBytes(compilation_.packages_[package].manifest.Name(),
                                     compilation_.packages_[leader[group]].manifest.Name()) < 0) {
            leader[group] = package;
        }
    }

    Array<uint32> previous(allocator_);
    Array<uint32> queue(allocator_);
    Array<uint32> path(allocator_);
    if (!previous.TryResize(count)) {
        error_ = ErrorCode::OutOfMemory;
        return;
    }

    for (uint32 group = 0; group < components && error_ == ErrorCode::None; ++group) {
        if (size[group] < 2) {
            continue;
        }

        const uint32 start = leader[group];
        const PackageManifest& manifest = compilation_.packages_[start].manifest;
        uint32 next = NoPackage;
        uint32 dependency = 0;
        for (uint32 edge = edgeFirst[start]; edge < edgeFirst[start + 1]; ++edge) {
            if (component[edges[edge].to] == group) {
                next = edges[edge].to;
                dependency = edges[edge].dependency;
                break;
            }
        }

        // next에서 start로 돌아오는 가장 짧은 길(묶음 안, 간선 순서대로 넓이 우선)이다.
        for (uint32 package = 0; package < count; ++package) {
            previous[package] = NoPackage;
        }

        queue.Clear();
        if (auto pushed = queue.TryPushBack(next); !pushed) {
            error_ = pushed.Error();
            return;
        }

        previous[next] = next;
        bool found = next == start;
        for (usize head = 0; head < queue.Size() && !found; ++head) {
            const uint32 from = queue[head];
            for (uint32 edge = edgeFirst[from]; edge < edgeFirst[from + 1]; ++edge) {
                const uint32 to = edges[edge].to;
                if (component[to] != group || previous[to] != NoPackage) {
                    continue;
                }

                previous[to] = from;
                if (to == start) {
                    found = true;
                    break;
                }

                if (auto pushed = queue.TryPushBack(to); !pushed) {
                    error_ = pushed.Error();
                    return;
                }
            }
        }

        // start <- … <- next 를 거꾸로 모은다(묶음 안이므로 길은 언제나 있다).
        path.Clear();
        for (uint32 at = start;;) {
            if (auto pushed = path.TryPushBack(at); !pushed) {
                error_ = pushed.Error();
                return;
            }

            if (at == next && path.Size() > 1) {
                break;
            }

            at = previous[at];
        }

        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("Packages depend on each other in a cycle: {}", manifest.Name());
        for (usize index = path.Size(); index > 0; --index) {
            message.Append(" -> ").Append(compilation_.packages_[path[index - 1]].manifest.Name());
        }

        message.Append(".");
        if (Succeeded(message)) {
            Report(DiagnosticCode::DependencyCycle, compilation_.packages_[start].source,
                   manifest.Dependencies()[dependency].span, message.View(),
                   "Packages are built and loaded in dependency order, so they cannot form a "
                   "cycle.",
                   "Remove one of the dependencies in the cycle.");
        }
    }
}

} // namespace rocks::sparks::detail
