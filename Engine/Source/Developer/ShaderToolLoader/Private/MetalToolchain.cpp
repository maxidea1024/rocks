#include <ShaderToolLoader/MetalToolchain.h>

#include "MetalToolchainInternal.h"
#include <Platform/File.h>
#include <Platform/Process.h>
#include <string.h>

// Metal 도구 찾기와 지문(Docs/Design/ShaderSystem 11.1절 SH1e E7)이다. 규칙은 공개 헤더 주석을
// 따른다.
namespace rocks::shader {

namespace metal_toolchain_detail {

// metal -v 출력을 모을 버퍼 크기다.
constexpr usize MetalVersionOutputBytes = 16 * 1024;
// 실행 파일 해시 읽기 단위다.
constexpr usize HashChunkBytes = 64 * 1024;

constexpr StringView VersionPrefix("Apple ");
constexpr StringView VersionMarker(" version ");
constexpr StringView TargetPrefix("Target:");

bool HashFile(const char* path, Sha256Digest& out) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    Sha256 hasher;
    uint8 chunk[HashChunkBytes];
    while (true) {
        const platform::FileTransfer read = file.Read(chunk, sizeof(chunk));
        if (!read.status && read.status.code != platform::FileCode::EndOfFile) {
            (void)file.Close();
            return false;
        }

        if (read.bytes == 0) {
            break;
        }

        hasher.Update(Span<const uint8>(chunk, read.bytes));
    }

    out = hasher.Finish();
    return static_cast<bool>(file.Close());
}

bool Exists(const char* path) noexcept {
    bool exists = false;
    return platform::PhysicalFileSystem().FileExists(path, strlen(path), exists) && exists;
}

} // namespace metal_toolchain_detail

bool ParseMetalVersion(StringView output, char (&out)[MaxMetalVersionBytes]) noexcept {
    using namespace metal_toolchain_detail;
    out[0] = '\0';
    StringView version;
    StringView target;
    StringView rest = output;
    while (!rest.IsEmpty()) {
        StringView line = rest;
        StringView after;
        if (!rest.Split("\n", line, after, StringComparison::CaseSensitive)) {
            after = StringView();
        }

        rest = after;
        if (line.EndsWith("\r", StringComparison::CaseSensitive)) {
            line = line.SliceBytes(0, line.ByteCount() - 1);
        }

        if (version.IsEmpty() && line.StartsWith(VersionPrefix, StringComparison::CaseSensitive) &&
            line.Contains(VersionMarker, StringComparison::CaseSensitive)) {
            version = line;
        } else if (target.IsEmpty() &&
                   line.StartsWith(TargetPrefix, StringComparison::CaseSensitive)) {
            target = line;
        }
    }

    if (version.IsEmpty()) {
        return false;
    }

    const usize bytes = version.ByteCount() + (target.IsEmpty() ? 0 : 1 + target.ByteCount());
    if (bytes + 1 > MaxMetalVersionBytes) {
        return false;
    }

    memcpy(out, version.Data(), version.ByteCount());
    usize written = version.ByteCount();
    if (!target.IsEmpty()) {
        out[written++] = '\n';
        memcpy(out + written, target.Data(), target.ByteCount());
        written += target.ByteCount();
    }

    out[written] = '\0';
    return true;
}

void LocateMetalToolchain(MetalToolchain& out) noexcept {
    using namespace metal_toolchain_detail;
    out = MetalToolchain{};
    char metal[MaxShaderToolPathBytes] = {};
    char metallib[MaxShaderToolPathBytes] = {};
    if (!FindMetalExecutables(metal, metallib) || !Exists(metal) || !Exists(metallib)) {
        return;
    }

    // metal -v는 버전을 오류 출력에 쓴다(UE MetalShaderFormat.cpp:888).
    char output[MetalVersionOutputBytes];
    const platform::ProcessArgument arguments[] = {{"-v", 2}};
    platform::ProcessStartInfo info;
    info.executable = metal;
    info.executableBytes = strlen(metal);
    info.arguments = arguments;
    info.argumentCount = 1;
    info.output = platform::ProcessStreamMode::Pipe;
    info.error = platform::ProcessStreamMode::Pipe;
    info.mergeErrorIntoOutput = true;
    platform::ProcessRunOptions options;
    options.output = output;
    options.outputCapacity = sizeof(output);
    options.timeoutMilliseconds = MetalToolTimeoutMilliseconds;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    if (!run.status || run.timedOut ||
        !ParseMetalVersion(StringView(output, run.outputBytes), out.version)) {
        out = MetalToolchain{};
        return;
    }

    if (!HashFile(metal, out.sha256[MetalCompilerIndex]) ||
        !HashFile(metallib, out.sha256[MetalLibrarianIndex])) {
        out = MetalToolchain{};
        return;
    }

    memcpy(out.executables[MetalCompilerIndex], metal, sizeof(metal));
    memcpy(out.executables[MetalLibrarianIndex], metallib, sizeof(metallib));
    out.found = true;
}

} // namespace rocks::shader
