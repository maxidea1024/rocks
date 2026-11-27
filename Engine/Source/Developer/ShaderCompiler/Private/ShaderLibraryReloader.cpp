#include <ShaderCompiler/ShaderLibraryReloader.h>

#include <Core/Move.h>
#include <Platform/Memory.h>
#include <stdio.h>

// 개발 실행의 형식 라이브러리와 다시 만들기(Docs/Design/ShaderSystem 16.3절 SSD69–SSD71)다. 규칙은
// 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace library_reloader_detail {

// 형식 정의 디렉터리 경로 버퍼 byte 수다(LoadShaderFormatSnapshot이 NUL 종료 경로를 받는다).
constexpr usize FormatDirectoryBytes = 1024;

// 만든 형식 하나다(snapshot은 관리자 job이 가리키므로 옮기지 않는다 — UniquePtr로 둔다).
struct BuiltFormat {
    explicit BuiltFormat(Allocator& allocator) noexcept : snapshot(allocator), bytes(allocator) {
    }

    ShaderFormatSnapshot snapshot;
    Array<uint8> bytes;
    uint64 revision = 0;
};

// 원문 하나와 그것을 읽은 셰이더 타입 이름 해시다.
struct SourceUse {
    explicit SourceUse(Allocator& allocator) noexcept : virtualPath(allocator), types(allocator) {
    }

    String virtualPath;
    Array<uint64> types;
};

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || platform::MemoryCompare(left.Data(), right.Data(), left.Size()) == 0);
}

void CopyText(String& out, const char* text) noexcept {
    (void)out.TryAssign(StringView(text));
}

} // namespace library_reloader_detail

struct ShaderLibraryReloader::State {
    explicit State(const ShaderLibraryReloaderDesc& source) noexcept
        : desc(source), allocator(*source.allocator), formatDirectory(allocator),
          symbolDirectory(allocator), formats(allocator), sources(allocator) {
        valid = formatDirectory.TryAssign(source.formatDirectory).IsOk() &&
                symbolDirectory.TryAssign(source.symbolDirectory).IsOk() &&
                source.formatDirectory.ByteCount() < library_reloader_detail::FormatDirectoryBytes;
        desc.formatDirectory = formatDirectory.View();
        desc.symbolDirectory = symbolDirectory.View();
    }

    using BuiltFormat = library_reloader_detail::BuiltFormat;
    using SourceUse = library_reloader_detail::SourceUse;

    // libraryGate 아래에서 부른다.
    BuiltFormat* FindLocked(StringView formatId) const noexcept {
        for (const UniquePtr<BuiltFormat>& format : formats) {
            if (format->snapshot.definition.Id() == formatId) {
                return format.Get();
            }
        }

        return nullptr;
    }

    ShaderLibraryCookDesc CookDesc(const ShaderFormatSnapshot* const& snapshot,
                                   const ShaderCompileControl& control) const noexcept {
        ShaderLibraryCookDesc cook;
        cook.control = control;
        cook.allocator = &allocator;
        cook.registry = desc.registry;
        cook.formats = Span<const ShaderFormatSnapshot* const>(&snapshot, 1);
        cook.library = desc.library;
        cook.includeDevelopment = desc.includeDevelopment;
        cook.manager = desc.manager;
        cook.files = desc.files;
        cook.debugSymbols = desc.debugSymbols;
        cook.symbolDirectory = desc.symbolDirectory;
        return cook;
    }

    // 보고서의 원문 사용을 의존 지도에 더한다(합집합 — 줄이지 않는다).
    Result<void> AddSources(const ShaderLibraryCookReport& report) noexcept {
        for (const ShaderLibrarySourceUse& use : report.sources) {
            SourceUse* found = nullptr;
            for (SourceUse& existing : sources) {
                if (existing.virtualPath.View() == use.virtualPath.View()) {
                    found = &existing;
                    break;
                }
            }

            if (found == nullptr) {
                auto added = sources.TryEmplaceBack(allocator);
                if (!added || !added.Value()->virtualPath.TryAssign(use.virtualPath.View())) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }

                found = added.Value();
            }

            for (const uint64 type : use.types) {
                bool known = false;
                for (const uint64 existing : found->types) {
                    known = known || existing == type;
                }

                if (!known && !found->types.TryPushBack(type)) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }
            }
        }

        return Result<void>::Success();
    }

    // changed 가운데 의존 지도에 있는 원문의 타입 수(겹침 없이)다.
    Result<uint32> CountAffected(Span<const StringView> changed) noexcept {
        Array<uint64> affected(allocator);
        for (const StringView path : changed) {
            for (const SourceUse& use : sources) {
                if (use.virtualPath.View() != path) {
                    continue;
                }

                for (const uint64 type : use.types) {
                    bool known = false;
                    for (const uint64 existing : affected) {
                        known = known || existing == type;
                    }

                    if (!known && !affected.TryPushBack(type)) {
                        return Result<uint32>::Failure(ErrorCode::OutOfMemory);
                    }
                }
            }
        }

        return Result<uint32>::Success(static_cast<uint32>(affected.Size()));
    }

    ShaderLibraryReloaderDesc desc;
    Allocator& allocator;
    String formatDirectory;
    String symbolDirectory;
    // 만들기·다시 만들기(관리자 Compile)를 차례로 돌린다.
    platform::Mutex compileGate;
    // formats의 byte·revision과 목록을 지킨다(짧게만 잡는다).
    mutable platform::Mutex libraryGate;
    Array<UniquePtr<BuiltFormat>> formats;
    // 의존 지도다(compileGate 아래에서만 바꾸고 읽는다).
    Array<SourceUse> sources;
    bool lastFailed = false;
    bool valid = false;
};

const char* ShaderReloadOutcomeName(ShaderReloadOutcome outcome) noexcept {
    switch (outcome) {
    case ShaderReloadOutcome::NoShaderAffected:
        return "NoShaderAffected";
    case ShaderReloadOutcome::Unchanged:
        return "Unchanged";
    case ShaderReloadOutcome::Rebuilt:
        return "Rebuilt";
    case ShaderReloadOutcome::Failed:
        return "Failed";
    }

    return "unknown";
}

Result<void> CollectShaderLibraryDiagnostics(const ShaderLibraryCookReport& report,
                                             Array<ShaderDiagnostic>& out) noexcept {
    Array<ShaderDiagnostic> parsed(out.GetAllocator());
    for (const ShaderLibraryCookJobFailure& failure : report.failures) {
        parsed.Clear();
        if (auto read = ParseShaderDiagnostics(failure.diagnostics.View(), parsed); !read) {
            return read;
        }

        if (auto merged = MergeShaderDiagnostics(out, parsed); !merged) {
            return merged;
        }
    }

    return Result<void>::Success();
}

ShaderLibraryReloader::ShaderLibraryReloader(const ShaderLibraryReloaderDesc& desc) noexcept {
    if (desc.allocator != nullptr) {
        auto state = UniquePtr<State>::TryMake(*desc.allocator, desc);
        if (state) {
            state_ = Move(state.Value());
        }
    }
}

ShaderLibraryReloader::~ShaderLibraryReloader() noexcept = default;

Result<void> ShaderLibraryReloader::Build(StringView formatId, ShaderLibraryCookReport& report,
                                          const ShaderCompileControl& control) noexcept {
    using namespace library_reloader_detail;
    if (!state_ || !state_->valid || state_->desc.registry == nullptr ||
        state_->desc.manager == nullptr || formatId.IsEmpty()) {
        return Result<void>::Failure(state_ ? ErrorCode::InvalidArgument : ErrorCode::OutOfMemory);
    }

    State& state = *state_;
    platform::LockGuard compile(state.compileGate);
    {
        platform::LockGuard library(state.libraryGate);
        if (state.FindLocked(formatId) != nullptr) {
            return Result<void>::Success();
        }
    }

    auto created = UniquePtr<BuiltFormat>::TryMake(state.allocator, state.allocator);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    BuiltFormat& format = *created.Value();
    char directory[FormatDirectoryBytes];
    (void)snprintf(directory, sizeof(directory), "%.*s",
                   static_cast<int>(state.formatDirectory.ByteCount()),
                   state.formatDirectory.Data());
    if (!LoadShaderFormatSnapshot(directory, formatId, format.snapshot)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const ShaderFormatSnapshot* snapshot = &format.snapshot;
    const ShaderLibraryCookDesc cook = state.CookDesc(snapshot, control);
    if (auto compiled = CompileShaderLibrary(cook, report, format.bytes); !compiled) {
        (void)state.AddSources(report);
        return compiled;
    }

    if (auto added = state.AddSources(report); !added) {
        return added;
    }

    format.revision = 1;
    platform::LockGuard library(state.libraryGate);
    if (!state.formats.TryPushBack(Move(created.Value()))) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

Result<uint64> ShaderLibraryReloader::CopyLibrary(StringView formatId,
                                                  Array<uint8>& out) const noexcept {
    if (!state_) {
        return Result<uint64>::Failure(ErrorCode::NotFound);
    }

    platform::LockGuard library(state_->libraryGate);
    const auto* format = state_->FindLocked(formatId);
    if (format == nullptr) {
        return Result<uint64>::Failure(ErrorCode::NotFound);
    }

    out.Clear();
    if (!out.TryAppend(format->bytes.View())) {
        return Result<uint64>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<uint64>::Success(format->revision);
}

uint64 ShaderLibraryReloader::Revision(StringView formatId) const noexcept {
    if (!state_) {
        return 0;
    }

    platform::LockGuard library(state_->libraryGate);
    const auto* format = state_->FindLocked(formatId);
    return format != nullptr ? format->revision : 0;
}

usize ShaderLibraryReloader::FormatCount() const noexcept {
    if (!state_) {
        return 0;
    }

    platform::LockGuard library(state_->libraryGate);
    return state_->formats.Size();
}

Result<void> ShaderLibraryReloader::Reload(Span<const StringView> changed, bool everything,
                                           ShaderReloadReport& report,
                                           const ShaderCompileControl& control) noexcept {
    using namespace library_reloader_detail;
    report.outcome = ShaderReloadOutcome::NoShaderAffected;
    report.affectedTypes = 0;
    report.formats = 0;
    report.changedFormats = 0;
    report.failedJobs = 0;
    report.diagnostics.Clear();
    report.failedType.Clear();
    report.failedFormat.Clear();
    if (!state_) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    State& state = *state_;
    platform::LockGuard compile(state.compileGate);
    auto affected = state.CountAffected(changed);
    if (!affected) {
        return Result<void>::Failure(affected.Error());
    }

    report.affectedTypes =
        everything ? static_cast<uint32>(state.desc.registry->Types().Size()) : affected.Value();
    // 직전이 실패였으면 지도에 없는 원문 변경도 다시 시도한다(헤더 주석).
    if (!everything && report.affectedTypes == 0 && !(state.lastFailed && !changed.IsEmpty())) {
        return Result<void>::Success();
    }

    // 만든 형식 목록의 사본이다(formats의 원소는 UniquePtr라 주소가 그대로다 — 다시 만드는 동안
    // 목록이 늘어나지 않는다: Build도 compileGate를 잡는다).
    Array<BuiltFormat*> targets(state.allocator);
    {
        platform::LockGuard library(state.libraryGate);
        for (const UniquePtr<BuiltFormat>& format : state.formats) {
            if (!targets.TryPushBack(format.Get())) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    bool failed = false;
    for (BuiltFormat* format : targets) {
        ++report.formats;
        const ShaderFormatSnapshot* snapshot = &format->snapshot;
        const ShaderLibraryCookDesc cook = state.CookDesc(snapshot, control);
        ShaderLibraryCookReport cooked(state.allocator);
        Array<uint8> bytes(state.allocator);
        const Result<void> compiled = CompileShaderLibrary(cook, cooked, bytes);
        if (auto added = state.AddSources(cooked); !added) {
            return added;
        }

        if (!compiled) {
            if (compiled.Error() != ErrorCode::DependencyFailed) {
                return Result<void>::Failure(compiled.Error());
            }

            failed = true;
            for (const ShaderLibraryCookFormat& line : cooked.formats) {
                report.failedJobs += line.failedJobs;
            }

            if (report.failedType.IsEmpty() && !cooked.failures.IsEmpty()) {
                CopyText(report.failedType, cooked.failures[0].typeName);
                CopyText(report.failedFormat, cooked.failures[0].formatId);
            }

            if (auto collected = CollectShaderLibraryDiagnostics(cooked, report.diagnostics);
                !collected) {
                return collected;
            }

            continue;
        }

        platform::LockGuard library(state.libraryGate);
        if (SameBytes(format->bytes.View(), bytes.View())) {
            continue;
        }

        format->bytes = Move(bytes);
        ++format->revision;
        ++report.changedFormats;
    }

    state.lastFailed = failed;
    report.outcome = failed                       ? ShaderReloadOutcome::Failed
                     : report.changedFormats != 0 ? ShaderReloadOutcome::Rebuilt
                                                  : ShaderReloadOutcome::Unchanged;
    return Result<void>::Success();
}

Result<void> ShaderLibraryReloader::DependentTypes(StringView virtualPath,
                                                   Array<uint64>& out) const noexcept {
    out.Clear();
    if (!state_) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    platform::LockGuard compile(state_->compileGate);
    for (const State::SourceUse& use : state_->sources) {
        if (use.virtualPath.View() == virtualPath) {
            if (!out.TryAppend(use.types.View())) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            return Result<void>::Success();
        }
    }

    return Result<void>::Success();
}

bool ShaderLibraryReloader::LastReloadFailed() const noexcept {
    if (!state_) {
        return false;
    }

    platform::LockGuard compile(state_->compileGate);
    return state_->lastFailed;
}

} // namespace rocks::shader
