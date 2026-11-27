#include <ShaderCompiler/ShaderSourceTree.h>

#include "ShaderSourceStatusInternal.h"

#include <Core/Text/Path.h>
#include <Platform/Memory.h>

// 셰이더 소스 트리(Docs/Design/ShaderSystem 11.1절 SC2)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace source_tree_detail {

// 경로가 비었는지(디렉터리 아닌 파일이 없는지)와 관계없이 실제 디렉터리가 절대 경로인가.
bool IsAbsoluteDirectory(StringView path) noexcept {
    const Result<PathRoot> root = Path::Root(path);
    if (!root) {
        return false;
    }

    const PathRoot kind = root.Value();
    return kind == PathRoot::PosixRoot || kind == PathRoot::WindowsDriveAbsolute ||
           kind == PathRoot::WindowsUnc;
}

bool EndsWithSeparator(StringView path) noexcept {
    if (path.IsEmpty()) {
        return false;
    }

    const char last = path.Data()[path.ByteCount() - 1];
    return last == '/' || last == '\\';
}

} // namespace source_tree_detail

namespace source_detail {

ShaderSourceStatus MakeSourceStatus(ShaderSourceError code, StringView path, uint32 line,
                                    StringView message, StringView detail) noexcept {
    ShaderSourceStatus status;
    status.code = code;
    status.line = line;
    const usize pathBytes =
        path.ByteCount() < sizeof(status.path) - 1 ? path.ByteCount() : sizeof(status.path) - 1;
    if (pathBytes != 0) {
        platform::MemoryCopy(status.path, path.Data(), pathBytes);
    }

    status.path[pathBytes] = '\0';
    usize used = 0;
    const auto append = [&status, &used](StringView part) noexcept {
        const usize room = sizeof(status.message) - 1 - used;
        const usize bytes = part.ByteCount() < room ? part.ByteCount() : room;
        if (bytes != 0) {
            platform::MemoryCopy(status.message + used, part.Data(), bytes);
        }

        used += bytes;
    };
    append(message);
    if (!detail.IsEmpty()) {
        append(StringView(": '"));
        append(detail);
        append(StringView("'"));
    }

    status.message[used] = '\0';
    return status;
}

} // namespace source_detail

const char* ShaderSourceErrorName(ShaderSourceError code) noexcept {
    switch (code) {
    case ShaderSourceError::None:
        return "None";
    case ShaderSourceError::InvalidArgument:
        return "InvalidArgument";
    case ShaderSourceError::OutOfMemory:
        return "OutOfMemory";
    case ShaderSourceError::InvalidMount:
        return "InvalidMount";
    case ShaderSourceError::MountDirectoryNotFound:
        return "MountDirectoryNotFound";
    case ShaderSourceError::GeneratedDirectoryExists:
        return "GeneratedDirectoryExists";
    case ShaderSourceError::DuplicateMount:
        return "DuplicateMount";
    case ShaderSourceError::InvalidPath:
        return "InvalidPath";
    case ShaderSourceError::InvalidExtension:
        return "InvalidExtension";
    case ShaderSourceError::UnknownMount:
        return "UnknownMount";
    case ShaderSourceError::GeneratedSource:
        return "GeneratedSource";
    case ShaderSourceError::FileNotFound:
        return "FileNotFound";
    case ShaderSourceError::ReadFailed:
        return "ReadFailed";
    case ShaderSourceError::FileTooLarge:
        return "FileTooLarge";
    case ShaderSourceError::InvalidEncoding:
        return "InvalidEncoding";
    case ShaderSourceError::Trigraph:
        return "Trigraph";
    case ShaderSourceError::UnterminatedComment:
        return "UnterminatedComment";
    case ShaderSourceError::UnterminatedLiteral:
        return "UnterminatedLiteral";
    case ShaderSourceError::InvalidDirective:
        return "InvalidDirective";
    case ShaderSourceError::UnsupportedInclude:
        return "UnsupportedInclude";
    case ShaderSourceError::IncludeDepthExceeded:
        return "IncludeDepthExceeded";
    case ShaderSourceError::InvalidMacroDefinition:
        return "InvalidMacroDefinition";
    case ShaderSourceError::MacroRedefined:
        return "MacroRedefined";
    case ShaderSourceError::InvalidMacroCall:
        return "InvalidMacroCall";
    case ShaderSourceError::MacroDepthExceeded:
        return "MacroDepthExceeded";
    case ShaderSourceError::InvalidPaste:
        return "InvalidPaste";
    case ShaderSourceError::UnbalancedConditional:
        return "UnbalancedConditional";
    case ShaderSourceError::InvalidExpression:
        return "InvalidExpression";
    case ShaderSourceError::CompilerBuiltinInCondition:
        return "CompilerBuiltinInCondition";
    case ShaderSourceError::NondeterministicMacro:
        return "NondeterministicMacro";
    case ShaderSourceError::ErrorDirective:
        return "ErrorDirective";
    case ShaderSourceError::OutputTooLarge:
        return "OutputTooLarge";
    case ShaderSourceError::InvalidDefine:
        return "InvalidDefine";
    case ShaderSourceError::ReservedDefine:
        return "ReservedDefine";
    case ShaderSourceError::DuplicateDefine:
        return "DuplicateDefine";
    case ShaderSourceError::UnsupportedStage:
        return "UnsupportedStage";
    }

    return "unknown";
}

ShaderSourceStatus ValidateShaderVirtualPath(StringView path) noexcept {
    ShaderVirtualPathInfo info;
    const ShaderVirtualPathCheck checked = CheckShaderVirtualPath(path, info);
    if (checked) {
        return ShaderSourceStatus{};
    }

    return source_detail::MakeSourceStatus(checked.error == ShaderVirtualPathError::InvalidExtension
                                               ? ShaderSourceError::InvalidExtension
                                               : ShaderSourceError::InvalidPath,
                                           checked.pathTooLong ? StringView() : path, 0,
                                           checked.reason);
}

ShaderSourceStatus ValidateShaderVirtualDirectory(StringView directory) noexcept {
    const ShaderVirtualPathCheck checked = CheckShaderVirtualDirectory(directory);
    if (checked) {
        return ShaderSourceStatus{};
    }

    return source_detail::MakeSourceStatus(ShaderSourceError::InvalidMount, directory, 0,
                                           checked.reason);
}

ShaderSourceTree::ShaderSourceTree(Allocator& allocator, platform::FileSystem& files) noexcept
    : allocator_(&allocator), files_(&files), mounts_(allocator) {
}

ShaderSourceStatus ShaderSourceTree::Mount(StringView virtualDirectory,
                                           StringView realDirectory) noexcept {
    using namespace source_tree_detail;
    using source_detail::MakeSourceStatus;
    if (const ShaderSourceStatus valid = ValidateShaderVirtualDirectory(virtualDirectory); !valid) {
        return valid;
    }

    for (const MountEntry& mount : mounts_) {
        if (mount.virtualDirectory.View() == virtualDirectory) {
            return MakeSourceStatus(ShaderSourceError::DuplicateMount, virtualDirectory, 0,
                                    "virtual shader directory is already mounted");
        }
    }

    if (!IsAbsoluteDirectory(realDirectory)) {
        return MakeSourceStatus(ShaderSourceError::MountDirectoryNotFound, virtualDirectory, 0,
                                "real shader directory must be an absolute path", realDirectory);
    }

    bool exists = false;
    if (const platform::FileStatus checked =
            files_->DirectoryExists(realDirectory.Data(), realDirectory.ByteCount(), exists);
        !checked) {
        return MakeSourceStatus(ShaderSourceError::ReadFailed, virtualDirectory, 0,
                                "cannot check real shader directory", realDirectory);
    }

    if (!exists) {
        return MakeSourceStatus(ShaderSourceError::MountDirectoryNotFound, virtualDirectory, 0,
                                "real shader directory does not exist", realDirectory);
    }

    // <실제 디렉터리>/Generated가 있으면 거부한다(UE ShaderCore.cpp:4245–4248과 같다).
    String generated(*allocator_);
    if (!generated.TryAssign(realDirectory) ||
        (!EndsWithSeparator(realDirectory) && !generated.TryAppend(StringView("/"))) ||
        !generated.TryAppend(StringView(GeneratedShaderDirectoryName))) {
        return MakeSourceStatus(ShaderSourceError::OutOfMemory, virtualDirectory, 0,
                                "out of memory");
    }

    exists = false;
    if (const platform::FileStatus checked =
            files_->DirectoryExists(generated.Data(), generated.ByteCount(), exists);
        !checked) {
        return MakeSourceStatus(ShaderSourceError::ReadFailed, virtualDirectory, 0,
                                "cannot check Generated directory", generated.View());
    }

    if (exists) {
        return MakeSourceStatus(ShaderSourceError::GeneratedDirectoryExists, virtualDirectory, 0,
                                "a Generated directory is reserved for generated shader source",
                                generated.View());
    }

    MountEntry entry(*allocator_);
    if (!entry.virtualDirectory.TryAssign(virtualDirectory) ||
        !entry.realDirectory.TryAssign(realDirectory) || !mounts_.TryPushBack(Move(entry))) {
        return MakeSourceStatus(ShaderSourceError::OutOfMemory, virtualDirectory, 0,
                                "out of memory");
    }

    return ShaderSourceStatus{};
}

const ShaderSourceTree::MountEntry*
ShaderSourceTree::FindMount(StringView virtualPath) const noexcept {
    for (const MountEntry& mount : mounts_) {
        const StringView directory = mount.virtualDirectory.View();
        if (virtualPath.ByteCount() > directory.ByteCount() &&
            virtualPath.Data()[directory.ByteCount()] == '/' &&
            virtualPath.StartsWith(directory, StringComparison::CaseSensitive)) {
            return &mount;
        }
    }

    return nullptr;
}

ShaderSourceStatus ShaderSourceTree::Load(StringView virtualPath, Array<uint8>& bytes,
                                          usize maxBytes) const noexcept {
    using namespace source_tree_detail;
    using source_detail::MakeSourceStatus;
    bytes.Clear();
    if (const ShaderSourceStatus valid = ValidateShaderVirtualPath(virtualPath); !valid) {
        return valid;
    }

    const MountEntry* mount = FindMount(virtualPath);
    if (mount == nullptr) {
        return MakeSourceStatus(ShaderSourceError::UnknownMount, virtualPath, 0,
                                "virtual shader root is not mounted");
    }

    const StringView relative(virtualPath.Data() + mount->virtualDirectory.ByteCount() + 1,
                              virtualPath.ByteCount() - mount->virtualDirectory.ByteCount() - 1);
    const StringView generatedPrefix(GeneratedShaderDirectoryName);
    if (relative.StartsWith(generatedPrefix, StringComparison::CaseSensitive) &&
        relative.ByteCount() > generatedPrefix.ByteCount() &&
        relative.Data()[generatedPrefix.ByteCount()] == '/') {
        return MakeSourceStatus(ShaderSourceError::GeneratedSource, virtualPath, 0,
                                "Generated shader source is memory-only and was not provided");
    }

    String real(*allocator_);
    const StringView root = mount->realDirectory.View();
    if (!real.TryAssign(root) || (!EndsWithSeparator(root) && !real.TryAppend(StringView("/"))) ||
        !real.TryAppend(relative)) {
        return MakeSourceStatus(ShaderSourceError::OutOfMemory, virtualPath, 0, "out of memory");
    }

    // 일반 파일만 원문이다(디렉터리는 열기 오류가 OS마다 달라 먼저 거른다).
    bool exists = false;
    if (const platform::FileStatus checked =
            files_->FileExists(real.Data(), real.ByteCount(), exists);
        !checked || !exists) {
        return MakeSourceStatus(
            checked ? ShaderSourceError::FileNotFound : ShaderSourceError::ReadFailed, virtualPath,
            0, checked ? "shader source file not found" : "cannot check shader source file");
    }

    platform::File file;
    if (const platform::FileStatus opened =
            file.Open(*files_, real.Data(), real.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        const bool missing = opened.code == platform::FileCode::NotFound ||
                             opened.code == platform::FileCode::WrongType;
        return MakeSourceStatus(
            missing ? ShaderSourceError::FileNotFound : ShaderSourceError::ReadFailed, virtualPath,
            0, missing ? "shader source file not found" : "cannot open shader source file");
    }

    uint64 size = 0;
    if (const platform::FileStatus sized = file.Size(size); !sized) {
        (void)file.Close();
        return MakeSourceStatus(ShaderSourceError::ReadFailed, virtualPath, 0,
                                "cannot read shader source file size");
    }

    if (size > maxBytes) {
        (void)file.Close();
        return MakeSourceStatus(ShaderSourceError::FileTooLarge, virtualPath, 0,
                                "shader source file exceeds the size limit");
    }

    if (!bytes.TryResize(static_cast<usize>(size))) {
        (void)file.Close();
        return MakeSourceStatus(ShaderSourceError::OutOfMemory, virtualPath, 0, "out of memory");
    }

    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    const platform::FileStatus closed = file.Close();
    if (!read.status || !closed) {
        bytes.Clear();
        return MakeSourceStatus(ShaderSourceError::ReadFailed, virtualPath, 0,
                                "cannot read shader source file");
    }

    return ShaderSourceStatus{};
}

Result<void> ShaderSourceTree::ResolveDiskPath(StringView virtualPath, String& out) const noexcept {
    using namespace source_tree_detail;
    out.Clear();
    if (!ValidateShaderVirtualPath(virtualPath)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const MountEntry* mount = FindMount(virtualPath);
    if (mount == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const StringView relative(virtualPath.Data() + mount->virtualDirectory.ByteCount() + 1,
                              virtualPath.ByteCount() - mount->virtualDirectory.ByteCount() - 1);
    const StringView generatedPrefix(GeneratedShaderDirectoryName);
    if (relative.StartsWith(generatedPrefix, StringComparison::CaseSensitive) &&
        relative.ByteCount() > generatedPrefix.ByteCount() &&
        relative.Data()[generatedPrefix.ByteCount()] == '/') {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const StringView root = mount->realDirectory.View();
    if (!out.TryAssign(root) || (!EndsWithSeparator(root) && !out.TryAppend(StringView("/"))) ||
        !out.TryAppend(relative)) {
        out.Clear();
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

bool MakeWatchedShaderPath(StringView virtualRoot, StringView relative, String& out) noexcept {
    out.Clear();
    const StringView extensions[] = {".rsf", ".rsh", ".h"};
    bool source = false;
    for (const StringView extension : extensions) {
        source = source || relative.EndsWith(extension, StringComparison::CaseSensitive);
    }

    if (!source || relative.IsEmpty() || !out.TryAssign(virtualRoot) || !out.TryAppend("/") ||
        !out.TryAppend(relative) || !ValidateShaderVirtualPath(out.View())) {
        out.Clear();
        return false;
    }

    return true;
}

} // namespace rocks::shader
