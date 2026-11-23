#include <ShaderCompiler/ShaderSourceTree.h>

#include "ShaderSourceStatusInternal.h"

#include <Core/Text/Path.h>
#include <Platform/Memory.h>

// 셰이더 소스 트리(Docs/Design/ShaderSystem 11.1절 SC2)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace source_tree_detail {

constexpr StringView EngineRootName("Engine");
constexpr StringView ProjectRootName("Project");
constexpr StringView PluginRootName("Plugin");
constexpr StringView EntryExtension("rsf");
constexpr StringView IncludeExtension("rsh");
constexpr StringView SharedExtension("h");

// 가상 경로 성분 문자다(ASCII [A-Za-z0-9_.-]).
bool IsVirtualPathCharacter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '.' || value == '-';
}

// 플러그인 이름 문자다(가상 디렉터리에 '.'이 없어야 하므로 '.'를 뺀다).
bool IsPluginNameCharacter(char value) noexcept {
    return value != '.' && IsVirtualPathCharacter(value);
}

bool IsPluginName(StringView name) noexcept {
    if (name.IsEmpty() || name.ByteCount() > MaxShaderPluginNameBytes) {
        return false;
    }

    for (const char value : name) {
        if (!IsPluginNameCharacter(value)) {
            return false;
        }
    }

    return true;
}

// '/' 뒤의 다음 성분을 잘라 낸다. cursor는 성분 시작 위치이며 다음 성분 시작으로 옮긴다.
StringView NextComponent(StringView path, usize& cursor) noexcept {
    const usize start = cursor;
    while (cursor < path.ByteCount() && path.Data()[cursor] != '/') {
        ++cursor;
    }

    const StringView component(path.Data() + start, cursor - start);
    if (cursor < path.ByteCount()) {
        ++cursor;
    }

    return component;
}

// 확장자(마지막 '.' 뒤)다. '.'이 없으면 빈 값이다.
StringView ExtensionOf(StringView fileName) noexcept {
    for (usize index = fileName.ByteCount(); index > 0; --index) {
        if (fileName.Data()[index - 1] == '.') {
            return StringView(fileName.Data() + index, fileName.ByteCount() - index);
        }
    }

    return StringView();
}

// 가상 경로의 루트 성분 수(/Engine·/Project는 1, /Plugin/<이름>은 2)다. 잘못된 루트면 0이다.
usize RootComponentCount(StringView first, StringView second) noexcept {
    if (first == EngineRootName || first == ProjectRootName) {
        return 1;
    }

    if (first == PluginRootName && IsPluginName(second)) {
        return 2;
    }

    return 0;
}

// 가상 경로를 성분으로 나눈다. 성분은 MaxShaderVirtualPathBytes / 2개를 넘을 수 없다.
constexpr usize MaxVirtualPathComponents = MaxShaderVirtualPathBytes / 2;

struct VirtualPathParts {
    StringView components[MaxVirtualPathComponents];
    usize count = 0;
    usize rootCount = 0;
};

ShaderSourceStatus SplitVirtualPath(StringView path, VirtualPathParts& parts) noexcept {
    using source_detail::MakeSourceStatus;
    if (path.IsEmpty() || path.Data()[0] != '/') {
        return MakeSourceStatus(ShaderSourceError::InvalidPath, path, 0,
                                "virtual shader path must start with '/'");
    }

    if (path.ByteCount() >= MaxShaderVirtualPathBytes) {
        return MakeSourceStatus(ShaderSourceError::InvalidPath, StringView(), 0,
                                "virtual shader path is too long");
    }

    usize cursor = 1;
    while (true) {
        const StringView component = NextComponent(path, cursor);
        if (component.IsEmpty() || component == StringView(".") || component == StringView("..")) {
            return MakeSourceStatus(ShaderSourceError::InvalidPath, path, 0,
                                    "virtual shader path has an empty, '.' or '..' component");
        }

        for (const char value : component) {
            if (!IsVirtualPathCharacter(value)) {
                return MakeSourceStatus(
                    ShaderSourceError::InvalidPath, path, 0,
                    "virtual shader path characters are limited to [A-Za-z0-9_.-] and '/'");
            }
        }

        parts.components[parts.count++] = component;
        if (cursor >= path.ByteCount()) {
            // 끝 '/'는 빈 성분이다.
            if (path.Data()[path.ByteCount() - 1] == '/') {
                return MakeSourceStatus(ShaderSourceError::InvalidPath, path, 0,
                                        "virtual shader path must name a file");
            }

            break;
        }
    }

    parts.rootCount = RootComponentCount(parts.components[0],
                                         parts.count > 1 ? parts.components[1] : StringView());
    if (parts.rootCount == 0) {
        return MakeSourceStatus(
            ShaderSourceError::InvalidPath, path, 0,
            "virtual shader path must be under /Engine, /Project or /Plugin/<name>");
    }

    if (parts.count <= parts.rootCount) {
        return MakeSourceStatus(ShaderSourceError::InvalidPath, path, 0,
                                "virtual shader path must name a file under its root");
    }

    return ShaderSourceStatus{};
}

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
    using namespace source_tree_detail;
    VirtualPathParts parts;
    if (const ShaderSourceStatus split = SplitVirtualPath(path, parts); !split) {
        return split;
    }

    const StringView fileName = parts.components[parts.count - 1];
    const StringView extension = ExtensionOf(fileName);
    const bool shared = parts.count > parts.rootCount + 1 &&
                        parts.components[parts.rootCount] == StringView(SharedShaderDirectoryName);
    if (shared) {
        if (extension != SharedExtension) {
            return source_detail::MakeSourceStatus(
                ShaderSourceError::InvalidExtension, path, 0,
                "only .h files are allowed under a Shared shader directory");
        }
    } else if (extension != EntryExtension && extension != IncludeExtension) {
        return source_detail::MakeSourceStatus(ShaderSourceError::InvalidExtension, path, 0,
                                               "shader source files must end with .rsf or .rsh");
    }

    return ShaderSourceStatus{};
}

ShaderSourceStatus ValidateShaderVirtualDirectory(StringView directory) noexcept {
    using namespace source_tree_detail;
    if (directory == StringView(EngineShaderVirtualRoot) ||
        directory == StringView(ProjectShaderVirtualRoot)) {
        return ShaderSourceStatus{};
    }

    const StringView prefix(PluginShaderVirtualPrefix);
    if (directory.StartsWith(prefix, StringComparison::CaseSensitive) &&
        IsPluginName(StringView(directory.Data() + prefix.ByteCount(),
                                directory.ByteCount() - prefix.ByteCount()))) {
        return ShaderSourceStatus{};
    }

    return source_detail::MakeSourceStatus(
        ShaderSourceError::InvalidMount, directory, 0,
        "virtual shader directory must be /Engine, /Project or /Plugin/<name> with "
        "[A-Za-z0-9_-] names and no trailing '/'");
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

} // namespace rocks::shader
