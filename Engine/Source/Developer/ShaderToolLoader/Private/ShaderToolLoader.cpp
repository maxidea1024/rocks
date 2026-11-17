#include <Core/Encoding/Sha256.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <ShaderToolLoader/ShaderToolLoader.h>
#include <string.h>

namespace rocks::shader {

namespace {

// 파일 SHA-256을 계산할 때 한 번에 읽는 byte 수다.
constexpr usize DigestReadBytes = 64 * 1024;

ShaderToolLoadStatus Failure(ShaderToolLoadCode code, const ShaderTool* tool,
                             platform::FileStatus file = {}) noexcept {
    ShaderToolLoadStatus status;
    status.code = code;
    status.file = file;
    status.tool = tool != nullptr ? tool->name : nullptr;
    return status;
}

// 호스트 경로 문법의 절대 경로인지다. Windows는 "X:\"·"X:/"·"\\"로, POSIX는 '/'로 시작한다.
bool IsAbsolute(const char* path, usize bytes) noexcept {
    if (platform::HostPathStyle() == platform::PathStyle::Windows) {
        const bool drive =
            bytes >= 3 &&
            ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && (path[2] == '\\' || path[2] == '/');
        const bool unc = bytes >= 2 && path[0] == '\\' && path[1] == '\\';
        return drive || unc;
    }

    return bytes >= 1 && path[0] == '/';
}

// out에 <root>/<storeKey>/<relative>를 호스트 구분자로 쓴다('/'를 바꾼다). NUL로 끝난다.
bool ComposePath(const char* root, usize rootBytes, const char* storeKey, const char* relative,
                 char* out, usize capacity, usize& bytes) noexcept {
    const char separator = platform::NativePathSeparator();
    usize length = rootBytes;
    while (length > 1 && (root[length - 1] == '/' || root[length - 1] == '\\')) {
        --length;
    }

    const usize keyBytes = strlen(storeKey);
    const usize relativeBytes = strlen(relative);
    const usize total = length + 1 + keyBytes + 1 + relativeBytes;
    if (total + 1 > capacity) {
        return false;
    }

    memcpy(out, root, length);
    out[length] = separator;
    memcpy(out + length + 1, storeKey, keyBytes);
    out[length + 1 + keyBytes] = separator;
    char* tail = out + length + 2 + keyBytes;
    for (usize index = 0; index < relativeBytes; ++index) {
        tail[index] = relative[index] == '/' ? separator : relative[index];
    }

    out[total] = '\0';
    bytes = total;
    return true;
}

// path 파일의 크기와 SHA-256을 file과 대조한다.
ShaderToolLoadStatus VerifyFile(const ShaderTool& tool, const ShaderToolFile& file,
                                const char* path, usize pathBytes) noexcept {
    platform::File stream;
    const platform::FileStatus opened = stream.Open(platform::PhysicalFileSystem(), path, pathBytes,
                                                    platform::FileOpenMode::ReadExisting);
    if (!opened) {
        return Failure(opened.code == platform::FileCode::NotFound
                           ? ShaderToolLoadCode::NotBootstrapped
                           : ShaderToolLoadCode::ReadFailed,
                       &tool, opened);
    }

    uint64 size = 0;
    if (const platform::FileStatus sized = stream.Size(size); !sized) {
        (void)stream.Close();
        return Failure(ShaderToolLoadCode::ReadFailed, &tool, sized);
    }

    if (size != file.size) {
        (void)stream.Close();
        return Failure(ShaderToolLoadCode::DigestMismatch, &tool);
    }

    Sha256 hasher;
    static constexpr usize BufferBytes = DigestReadBytes;
    uint8 buffer[BufferBytes];
    uint64 total = 0;
    while (true) {
        const platform::FileTransfer read = stream.Read(buffer, BufferBytes);
        if (!read.status) {
            (void)stream.Close();
            return Failure(ShaderToolLoadCode::ReadFailed, &tool, read.status);
        }

        if (read.bytes == 0) {
            break;
        }

        total += read.bytes;
        hasher.Update(Span<const uint8>(buffer, read.bytes));
    }

    if (const platform::FileStatus closed = stream.Close(); !closed) {
        return Failure(ShaderToolLoadCode::ReadFailed, &tool, closed);
    }

    // 크기를 본 뒤 파일이 바뀌었으면 읽은 양이 다르다.
    if (total != file.size || !(hasher.Finish() == file.sha256)) {
        return Failure(ShaderToolLoadCode::DigestMismatch, &tool);
    }

    return {};
}

} // namespace

const char* ShaderToolLoadCodeName(ShaderToolLoadCode code) noexcept {
    switch (code) {
    case ShaderToolLoadCode::Success:
        return "Success";
    case ShaderToolLoadCode::InvalidArgument:
        return "InvalidArgument";
    case ShaderToolLoadCode::UnsupportedHost:
        return "UnsupportedHost";
    case ShaderToolLoadCode::NotBootstrapped:
        return "NotBootstrapped";
    case ShaderToolLoadCode::DigestMismatch:
        return "DigestMismatch";
    case ShaderToolLoadCode::ReadFailed:
        return "ReadFailed";
    case ShaderToolLoadCode::PathTooLong:
        return "PathTooLong";
    case ShaderToolLoadCode::LoadFailed:
        return "LoadFailed";
    case ShaderToolLoadCode::SymbolMissing:
        return "SymbolMissing";
    case ShaderToolLoadCode::WrongState:
        return "WrongState";
    case ShaderToolLoadCode::CallFailed:
        return "CallFailed";
    }
    return "Unknown";
}

const char* ShaderToolLoadMessage(ShaderToolLoadCode code) noexcept {
    switch (code) {
    case ShaderToolLoadCode::Success:
        return "the tool is open";
    case ShaderToolLoadCode::InvalidArgument:
        return "the store root must be an absolute path and the tool table entry must name a store "
               "directory and files";
    case ShaderToolLoadCode::UnsupportedHost:
        return "the tool has no shared library for this host in the third-party manifest";
    case ShaderToolLoadCode::NotBootstrapped:
        return "the tool is not in the third-party store; run the bootstrap command from the "
               "repository root: dotnet msbuild Rocks.proj /t:ThirdPartyBootstrap";
    case ShaderToolLoadCode::DigestMismatch:
        return "the stored tool file differs from the manifest (size or SHA-256); delete its store "
               "directory and run the bootstrap command from the repository root: dotnet msbuild "
               "Rocks.proj /t:ThirdPartyBootstrap";
    case ShaderToolLoadCode::ReadFailed:
        return "the stored tool file could not be read";
    case ShaderToolLoadCode::PathTooLong:
        return "the tool path is longer than MaxShaderToolPathBytes";
    case ShaderToolLoadCode::LoadFailed:
        return "the operating system could not load the verified tool library";
    case ShaderToolLoadCode::SymbolMissing:
        return "the tool library does not export a required function (version mismatch)";
    case ShaderToolLoadCode::WrongState:
        return "the tool library is not open, or is already open";
    case ShaderToolLoadCode::CallFailed:
        return "a tool function returned an error";
    }
    return "unknown shader tool load result";
}

ShaderToolLibrary::ShaderToolLibrary(ShaderToolLibrary&& other) noexcept
    : library_(static_cast<platform::DynamicLibrary&&>(other.library_)), tool_(other.tool_) {
    other.tool_ = nullptr;
}

ShaderToolLibrary& ShaderToolLibrary::operator=(ShaderToolLibrary&& other) noexcept {
    if (this != &other) {
        library_ = static_cast<platform::DynamicLibrary&&>(other.library_);
        tool_ = other.tool_;
        other.tool_ = nullptr;
    }

    return *this;
}

ShaderToolLoadStatus ShaderToolLibrary::Open(const ShaderTool& tool, const char* storeRoot,
                                             usize storeRootBytes) noexcept {
    if (library_.IsOpen()) {
        return Failure(ShaderToolLoadCode::WrongState, &tool);
    }

    if (storeRoot == nullptr || storeRootBytes == 0 ||
        memchr(storeRoot, '\0', storeRootBytes) != nullptr ||
        !IsAbsolute(storeRoot, storeRootBytes)) {
        return Failure(ShaderToolLoadCode::InvalidArgument, &tool);
    }

    if (tool.origin == ShaderToolOrigin::System || tool.fileCount == 0) {
        return Failure(ShaderToolLoadCode::UnsupportedHost, &tool);
    }

    if (tool.storeKey == nullptr || tool.files == nullptr) {
        return Failure(ShaderToolLoadCode::InvalidArgument, &tool);
    }

    // 모든 runtime 파일을 먼저 대조한다(첫 파일이 같은 디렉터리의 다른 runtime 파일에 의존할 수
    // 있다).
    char path[MaxShaderToolPathBytes];
    usize pathBytes = 0;
    const ShaderToolFile* first = nullptr;
    char firstPath[MaxShaderToolPathBytes];
    usize firstPathBytes = 0;
    for (usize index = 0; index < tool.fileCount; ++index) {
        const ShaderToolFile& file = tool.files[index];
        if (file.role != ShaderToolFileRole::Runtime) {
            continue;
        }

        if (file.path == nullptr || !file.hasDigest) {
            return Failure(ShaderToolLoadCode::InvalidArgument, &tool);
        }

        if (!ComposePath(storeRoot, storeRootBytes, tool.storeKey, file.path, path, sizeof(path),
                         pathBytes)) {
            return Failure(ShaderToolLoadCode::PathTooLong, &tool);
        }

        if (const ShaderToolLoadStatus verified = VerifyFile(tool, file, path, pathBytes);
            !verified) {
            return verified;
        }

        if (first == nullptr) {
            first = &file;
            memcpy(firstPath, path, pathBytes + 1);
            firstPathBytes = pathBytes;
        }
    }

    if (first == nullptr) {
        return Failure(ShaderToolLoadCode::UnsupportedHost, &tool);
    }

    if (const platform::FileStatus opened = library_.Open(firstPath, firstPathBytes); !opened) {
        return Failure(ShaderToolLoadCode::LoadFailed, &tool, opened);
    }

    tool_ = &tool;
    return {};
}

ShaderToolLoadStatus ShaderToolLibrary::Find(platform::DynamicSymbol* symbols, usize count,
                                             platform::DynamicLibraryLease& lease) const noexcept {
    if (!library_.IsOpen()) {
        return Failure(ShaderToolLoadCode::WrongState, tool_);
    }

    usize missing = IndexNone;
    const platform::FileStatus found = library_.Find(symbols, count, lease, &missing);
    if (found) {
        return {};
    }

    if (found.code == platform::FileCode::NotFound && missing != IndexNone) {
        ShaderToolLoadStatus status = Failure(ShaderToolLoadCode::SymbolMissing, tool_, found);
        status.symbol = symbols[missing].name;
        return status;
    }

    return Failure(found.code == platform::FileCode::InvalidArgument
                       ? ShaderToolLoadCode::InvalidArgument
                       : ShaderToolLoadCode::LoadFailed,
                   tool_, found);
}

platform::FileStatus ShaderToolLibrary::Close() noexcept {
    const platform::FileStatus closed = library_.Close();
    if (!library_.IsOpen()) {
        tool_ = nullptr;
    }

    return closed;
}

ShaderToolLoadStatus ShaderToolSet::Open(const char* storeRoot, usize storeRootBytes,
                                         Span<const ShaderTool> tools) noexcept {
    if (IsOpen()) {
        return Failure(ShaderToolLoadCode::WrongState, nullptr);
    }

    auto find = [&](const char* name) noexcept -> const ShaderTool* {
        for (const ShaderTool& tool : tools) {
            if (tool.name != nullptr && strcmp(tool.name, name) == 0) {
                return &tool;
            }
        }

        return nullptr;
    };

    auto open = [&](const char* name, ShaderToolLibrary& library, auto& functions,
                    platform::DynamicLibraryLease& lease) noexcept -> ShaderToolLoadStatus {
        const ShaderTool* tool = find(name);
        if (tool == nullptr) {
            ShaderToolLoadStatus status = Failure(ShaderToolLoadCode::UnsupportedHost, nullptr);
            status.tool = name;
            return status;
        }

        if (ShaderToolLoadStatus opened = library.Open(*tool, storeRoot, storeRootBytes); !opened) {
            return opened;
        }

        return library.FindFunctions(functions, lease);
    };

    ShaderToolLoadStatus status = open(DxcToolName, dxc_, dxcFunctions_, dxcLease_);
    if (status) {
        status = open(SpirvCrossToolName, spirvCross_, spirvCrossFunctions_, spirvCrossLease_);
    }

    if (status) {
        status = open(SpirvToolsToolName, spirvTools_, spirvToolsFunctions_, spirvToolsLease_);
    }

    // FXC는 이 호스트 파일이 표에 있을 때만 연다(Windows 전용). 있는데 열지 못하면 실패다.
    if (const ShaderTool* fxc = find(FxcToolName);
        status && fxc != nullptr && fxc->fileCount != 0) {
        status = open(FxcToolName, fxc_, fxcFunctions_, fxcLease_);
    }

    if (!status) {
        Close();
    }

    return status;
}

void ShaderToolSet::Close() noexcept {
    dxcFunctions_ = {};
    fxcFunctions_ = {};
    spirvCrossFunctions_ = {};
    spirvToolsFunctions_ = {};
    dxcLease_.Reset();
    fxcLease_.Reset();
    spirvCrossLease_.Reset();
    spirvToolsLease_.Reset();
    // 이 객체의 lease를 모두 놓았으므로 Close는 Busy가 아니다. OS 해제 실패는 객체를 닫은 채로
    // 둔다.
    (void)dxc_.Close();
    (void)fxc_.Close();
    (void)spirvCross_.Close();
    (void)spirvTools_.Close();
}

} // namespace rocks::shader
