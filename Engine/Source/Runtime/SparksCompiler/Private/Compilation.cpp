#include <SparksCompiler/Compilation.h>

#include <Core/Hash.h>
#include <Platform/Fatal.h>

#include "CompilationChecker.h"

// 컴파일 단위의 입력 API와 결과 접근이다(검사는 CompilationChecker.h의 파일들).

namespace rocks::sparks {

Compilation::Compilation(Allocator& allocator, const SourceSet& sources) noexcept
    : allocator_(&allocator), sources_(&sources), text_(allocator), schemas_(allocator),
      packages_(allocator), modules_(allocator), trees_(allocator), imports_(allocator),
      modulePaths_(allocator, ModulePathHash{this}, ModulePathEqual{this}),
      packagesByName_(allocator), symbols_(allocator), types_(allocator), typeArguments_(allocator),
      typeIndex_(allocator, TypeHash{this}, TypeEqual{this}), signatures_(allocator),
      signatureParameters_(allocator), symbolBases_(allocator), nodeSymbols_(allocator),
      nodeTypes_(allocator), exprTypes_(allocator), nodeBase_(allocator), sortedModules_(allocator),
      sortedFirst_(allocator), sortedCount_(allocator), irDumps_(allocator) {
}

Compilation::TypeProbe Compilation::ProbeOf(uint32 type) const noexcept {
    const TypeEntry& entry = types_[type];
    return TypeProbe{
        entry.symbol,
        Span<const uint32>(typeArguments_.Data() + entry.firstArgument, entry.argumentCount),
        entry.optional};
}

uint64 Compilation::TypeHash::operator()(uint32 type) const noexcept {
    return (*this)(owner->ProbeOf(type));
}

uint64 Compilation::TypeHash::operator()(const TypeProbe& probe) const noexcept {
    uint64 hash = HashCombine(MixHash(probe.symbol), MixHash(probe.optional ? 1 : 0));
    for (const uint32 argument : probe.arguments) {
        hash = HashCombine(hash, MixHash(argument));
    }

    return hash;
}

bool Compilation::TypeEqual::operator()(uint32 left, uint32 right) const noexcept {
    return (*this)(left, owner->ProbeOf(right));
}

bool Compilation::TypeEqual::operator()(uint32 left, const TypeProbe& right) const noexcept {
    const TypeProbe probe = owner->ProbeOf(left);
    if (probe.symbol != right.symbol || probe.optional != right.optional ||
        probe.arguments.Size() != right.arguments.Size()) {
        return false;
    }

    for (usize index = 0; index < probe.arguments.Size(); ++index) {
        if (probe.arguments[index] != right.arguments[index]) {
            return false;
        }
    }

    return true;
}

uint32 Compilation::NodeSymbol(uint32 module, uint32 node) const noexcept {
    if (module >= nodeBase_.Size() || nodeBase_[module] == NoNode ||
        node >= trees_[module].NodeCount()) {
        return NoSymbol;
    }

    const uint32 symbol = nodeSymbols_[nodeBase_[module] + node];
    return symbol == detail::NotRecorded ? NoSymbol : symbol;
}

uint32 Compilation::ExpressionType(uint32 module, uint32 node) const noexcept {
    if (module >= nodeBase_.Size() || nodeBase_[module] == NoNode ||
        node >= trees_[module].NodeCount() || exprTypes_.IsEmpty()) {
        return NoType;
    }

    const uint32 type = exprTypes_[nodeBase_[module] + node];
    return type == detail::NotRecorded ? NoType : type;
}

uint32 Compilation::NodeType(uint32 module, uint32 node) const noexcept {
    if (module >= nodeBase_.Size() || nodeBase_[module] == NoNode ||
        node >= trees_[module].NodeCount()) {
        return NoType;
    }

    const uint32 type = nodeTypes_[nodeBase_[module] + node];
    return type == detail::NotRecorded ? NoType : type;
}

uint64 Compilation::ModulePathHash::operator()(uint32 module) const noexcept {
    const ModuleEntry& entry = owner->modules_[module];
    return (*this)(ModulePathProbe{entry.package, owner->Text(entry.path)});
}

uint64 Compilation::ModulePathHash::operator()(const ModulePathProbe& probe) const noexcept {
    return HashCombine(HashStringBytes(probe.path), MixHash(probe.package));
}

bool Compilation::ModulePathEqual::operator()(uint32 left, uint32 right) const noexcept {
    const ModuleEntry& entry = owner->modules_[right];
    return (*this)(left, ModulePathProbe{entry.package, owner->Text(entry.path)});
}

bool Compilation::ModulePathEqual::operator()(uint32 left,
                                              const ModulePathProbe& right) const noexcept {
    const ModuleEntry& entry = owner->modules_[left];
    return entry.package == right.package && owner->Text(entry.path) == right.path;
}

Result<void> Compilation::AddBindingSchema(SourceId id) noexcept {
    if (checked_ || id.index >= sources_->Count()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return schemas_.TryPushBack(SchemaEntry{BindingSchema(*allocator_), id, false});
}

Result<uint32> Compilation::AddPackage(SourceId manifest) noexcept {
    if (checked_ || manifest.index >= sources_->Count()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto pushed =
            packages_.TryPushBack(PackageEntry{PackageManifest(*allocator_), manifest, false});
        !pushed) {
        return Result<uint32>::Failure(pushed.Error());
    }

    return Result<uint32>::Success(static_cast<uint32>(packages_.Size() - 1));
}

Result<uint32> Compilation::AddModule(uint32 package, SourceId source, StringView path) noexcept {
    if (checked_ || package >= packages_.Size() || source.index >= sources_->Count()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    if (modulePaths_.Contains(ModulePathProbe{package, path})) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    ModuleEntry entry;
    entry.package = package;
    entry.source = source;
    entry.path.offset = static_cast<uint32>(text_.Size());
    entry.path.length = static_cast<uint32>(path.ByteCount());
    if (auto appended = text_.TryAppend(Span<const char>(path.Data(), path.ByteCount()));
        !appended) {
        return Result<uint32>::Failure(appended.Error());
    }

    if (auto reserved = trees_.TryReserve(trees_.Size() + 1); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto pushed = modules_.TryPushBack(entry); !pushed) {
        return Result<uint32>::Failure(pushed.Error());
    }

    // 색인에 넣지 못하면 모듈을 되돌린다(목록 불변).
    if (auto added = modulePaths_.Add(static_cast<uint32>(modules_.Size() - 1)); !added) {
        static_cast<void>(modules_.TryPopBack());
        return Result<uint32>::Failure(added.Error());
    }

    // 용량을 미리 잡았으므로 실패하지 않는다.
    static_cast<void>(trees_.TryPushBack(SyntaxTree(*allocator_)));
    return Result<uint32>::Success(static_cast<uint32>(modules_.Size() - 1));
}

Result<void> Compilation::SetLastStage(CompilationStage stage) noexcept {
    if (checked_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    lastStage_ = stage;
    return Result<void>::Success();
}

Result<void> Compilation::Check(DiagnosticList& diagnostics) noexcept {
    if (checked_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    checked_ = true;
    detail::CompilationChecker checker(*this, diagnostics);
    return checker.Run();
}

bool Compilation::SchemaIsValid(uint32 schema) const noexcept {
    return schema < schemas_.Size() && schemas_[schema].valid;
}

const BindingSchema& Compilation::Schema(uint32 schema) const noexcept {
    if (schema >= schemas_.Size()) {
        platform::Fatal("Sparks schema number is out of range");
    }

    return schemas_[schema].schema;
}

bool Compilation::PackageIsValid(uint32 package) const noexcept {
    return package < packages_.Size() && packages_[package].valid;
}

const PackageManifest& Compilation::Manifest(uint32 package) const noexcept {
    if (package >= packages_.Size()) {
        platform::Fatal("Sparks package number is out of range");
    }

    return packages_[package].manifest;
}

uint32 Compilation::FindPackage(StringView name) const noexcept {
    const uint32 package = FindNamedPackage(name);
    return package != NoPackage && packages_[package].valid ? package : NoPackage;
}

uint32 Compilation::FindNamedPackage(StringView name) const noexcept {
    // 정렬된 색인에서 이분 탐색한다(같은 이름이면 번호가 작은 것이 앞).
    usize low = 0;
    usize high = packagesByName_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (StringView::CompareBytes(packages_[packagesByName_[middle]].manifest.Name(), name) <
            0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low < packagesByName_.Size() && packages_[packagesByName_[low]].manifest.Name() == name &&
        !name.IsEmpty()) {
        return packagesByName_[low];
    }

    return NoPackage;
}

uint32 Compilation::ModulePackage(uint32 module) const noexcept {
    if (module >= modules_.Size()) {
        platform::Fatal("Sparks module number is out of range");
    }

    return modules_[module].package;
}

SourceId Compilation::ModuleSource(uint32 module) const noexcept {
    if (module >= modules_.Size()) {
        platform::Fatal("Sparks module number is out of range");
    }

    return modules_[module].source;
}

StringView Compilation::ModulePath(uint32 module) const noexcept {
    if (module >= modules_.Size()) {
        platform::Fatal("Sparks module number is out of range");
    }

    return Text(modules_[module].path);
}

StringView Compilation::ModuleImportPath(uint32 module) const noexcept {
    if (module >= modules_.Size()) {
        platform::Fatal("Sparks module number is out of range");
    }

    return Text(modules_[module].importPath);
}

const SyntaxTree* Compilation::ModuleTree(uint32 module) const noexcept {
    if (module >= modules_.Size()) {
        platform::Fatal("Sparks module number is out of range");
    }

    return modules_[module].parsed ? &trees_[module] : nullptr;
}

Span<const ImportResolution> Compilation::ModuleImports(uint32 module) const noexcept {
    if (module >= modules_.Size()) {
        platform::Fatal("Sparks module number is out of range");
    }

    const ModuleEntry& entry = modules_[module];
    return Span<const ImportResolution>(imports_.Data() + entry.firstImport, entry.importCount);
}

uint32 Compilation::FindModule(uint32 package, StringView importPath) const noexcept {
    if (package >= sortedFirst_.Size()) {
        return NoModule;
    }

    // 정렬된 구간에서 이분 탐색한다.
    usize low = sortedFirst_[package];
    usize high = low + sortedCount_[package];
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const int order =
            StringView::CompareBytes(Text(modules_[sortedModules_[middle]].importPath), importPath);
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low < sortedFirst_[package] + sortedCount_[package] &&
        Text(modules_[sortedModules_[low]].importPath) == importPath) {
        return sortedModules_[low];
    }

    return NoModule;
}

} // namespace rocks::sparks
