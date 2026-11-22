#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringView.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <ShaderCompiler/ShaderPreprocessor.h>
#include <ShaderCompiler/ShaderSourceTree.h>
#include <Base/InitializerList.h>
#include <stdio.h>
#include <string.h>

// 셰이더 소스 트리·전처리기 시험이 함께 쓰는 fixture 도구다. 시험마다 사용자 임시 디렉터리 아래에
// 고유한 디렉터리를 만들고 소멸 때 지운다. 시험은 그 아래를 /Engine(가짜 Platform.rsh)과
// /Plugin/Test로 mount한다.
namespace rocks::shader_source_test {

using namespace rocks::shader;

// fixture 경로 버퍼 크기다.
inline constexpr usize FixturePathBytes = 1024;
// 시험 fixture의 가상 루트다.
inline constexpr char FixturePlugin[] = "/Plugin/Test";

inline SystemAllocator& TestAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

inline uint32 NextFixtureSerial() noexcept {
    static uint32 serial = 0;
    return platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&serial, 1u) + 1u;
}

class Fixture final {
public:
    Fixture() noexcept = default;
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    ~Fixture() noexcept {
        if (length_ != 0) {
            (void)platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), path_, length_);
        }
    }

    // <임시>/rocks-shader-<tag>-<번호> 디렉터리를 만든다.
    bool Make(const char* tag) noexcept {
        char base[FixturePathBytes];
        usize bytes = 0;
        if (!platform::GetStandardDirectory(platform::StandardDirectory::Temporary, base,
                                            sizeof(base), bytes)) {
            return false;
        }

        const int count = snprintf(
            path_, sizeof(path_), "%s/rocks-shader-%s-%llu-%u", base, tag,
            static_cast<unsigned long long>(platform::MonotonicNanoseconds()), NextFixtureSerial());
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path_)) {
            return false;
        }

        length_ = static_cast<usize>(count);
        if (!platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path_, length_)) {
            length_ = 0;
            return false;
        }

        return true;
    }

    const char* Path() const noexcept {
        return path_;
    }

    // 이 디렉터리 아래 relative('/' 구분)의 절대 경로다.
    bool Child(const char* relative, char* out, usize capacity) const noexcept {
        const int count = snprintf(out, capacity, "%s/%s", path_, relative);
        return count > 0 && static_cast<usize>(count) < capacity;
    }

    bool MakeDirectory(const char* relative) const noexcept {
        char path[FixturePathBytes];
        return Child(relative, path, sizeof(path)) &&
               static_cast<bool>(
                   platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path, strlen(path)));
    }

    // relative 파일을 bytes로 쓴다(부모 디렉터리를 만든다).
    bool WriteBytes(const char* relative, const void* bytes, usize size) const noexcept {
        char path[FixturePathBytes];
        if (!Child(relative, path, sizeof(path))) {
            return false;
        }

        char parent[FixturePathBytes];
        memcpy(parent, path, strlen(path) + 1);
        char* last = strrchr(parent, '/');
        if (last == nullptr) {
            return false;
        }

        *last = '\0';
        if (!platform::MakeDirectoryTree(platform::PhysicalFileSystem(), parent, strlen(parent))) {
            return false;
        }

        platform::File file;
        if (!file.Open(path, platform::FileOpenMode::CreateAlways)) {
            return false;
        }

        const platform::FileTransfer written = file.WriteAll(bytes, size);
        return static_cast<bool>(file.Close()) && static_cast<bool>(written.status) &&
               written.bytes == size;
    }

    bool Write(const char* relative, StringView text) const noexcept {
        return WriteBytes(relative, text.Data(), text.ByteCount());
    }

private:
    char path_[FixturePathBytes] = {};
    usize length_ = 0;
};

// fixture의 Engine(가짜 Platform.rsh)·Plugin 디렉터리를 /Engine·/Plugin/Test로 mount한다.
// platformHeader는 가짜 Platform.rsh 내용이다.
inline bool MountFixture(const Fixture& fixture, shader::ShaderSourceTree& tree,
                         StringView platformHeader = StringView()) noexcept {
    char engine[FixturePathBytes];
    char plugin[FixturePathBytes];
    return fixture.Write("Engine/Public/Platform.rsh", platformHeader) &&
           fixture.MakeDirectory("Plugin") && fixture.Child("Engine", engine, sizeof(engine)) &&
           fixture.Child("Plugin", plugin, sizeof(plugin)) &&
           static_cast<bool>(tree.Mount("/Engine", StringView(engine))) &&
           static_cast<bool>(tree.Mount(FixturePlugin, StringView(plugin)));
}

// 저장소의 Engine/Shaders(시험은 저장소 루트에서 돈다)의 절대 경로다.
inline bool RepositoryShaderDirectory(char* out, usize capacity) noexcept {
    char working[FixturePathBytes];
    usize bytes = 0;
    if (!platform::GetStandardDirectory(platform::StandardDirectory::Working, working,
                                        sizeof(working), bytes)) {
        return false;
    }

    const int count =
        snprintf(out, capacity, "%s/%s", working, shader::EngineShaderSourceDirectory);
    return count > 0 && static_cast<usize>(count) < capacity;
}

// 실패 결과를 사람이 읽게 찍는다(시험 실패 진단).
inline void PrintStatus(const char* what, const shader::ShaderSourceStatus& status) noexcept {
    if (!status) {
        fprintf(stderr, "%s: %s at %s:%u: %s\n", what, shader::ShaderSourceErrorName(status.code),
                status.path, status.line, status.message);
    }
}

// 전처리 시험의 엔트리다.
inline constexpr char Entry[] = "/Plugin/Test/Main.rsf";
// fixture 하나와 그 트리다.
struct Case {
    Case() noexcept : tree(TestAllocator()), shader(TestAllocator()) {
    }

    bool Make(const char* tag, StringView platformHeader = StringView()) noexcept {
        return fixture.Make(tag) && MountFixture(fixture, tree, platformHeader);
    }

    // /Plugin/Test 아래 relative 파일을 쓴다.
    bool File(const char* relative, StringView text) noexcept {
        char path[FixturePathBytes];
        snprintf(path, sizeof(path), "Plugin/%s", relative);
        return fixture.Write(path, text);
    }

    ShaderSourceStatus Run(Span<const ShaderDefine> defines = Span<const ShaderDefine>(),
                           const ShaderPreprocessLimits& limits = {}) noexcept {
        return PreprocessShader(tree, Entry, defines, shader, limits);
    }

    Fixture fixture;
    ShaderSourceTree tree;
    PreprocessedShader shader;
};

// 결과가 expected와 byte 단위로 같은지 본다(다르면 둘 다 찍는다).
inline bool Same(const PreprocessedShader& shader, StringView expected) noexcept {
    if (shader.text.View() == expected) {
        return true;
    }

    fprintf(stderr, "--- expected ---\n%.*s--- actual ---\n%.*s--- end ---\n",
            static_cast<int>(expected.ByteCount()), expected.Data(),
            static_cast<int>(shader.text.ByteCount()), shader.text.Data());
    return false;
}

// source 하나를 엔트리로 전처리한 결과가 expected인지 본다.
inline bool Expect(const char* tag, StringView source, StringView expected) noexcept {
    Case test;
    if (!test.Make(tag) || !test.File("Main.rsf", source)) {
        return false;
    }

    const ShaderSourceStatus status = test.Run();
    PrintStatus(tag, status);
    return static_cast<bool>(status) && Same(test.shader, expected);
}

// source 하나가 code 오류를 그 줄(line)·경로(path, null이면 엔트리)로 내는지 본다.
inline bool ExpectError(const char* tag, StringView source, ShaderSourceError code, uint32 line,
                        const char* path = nullptr) noexcept {
    Case test;
    if (!test.Make(tag) || !test.File("Main.rsf", source)) {
        return false;
    }

    const ShaderSourceStatus status = test.Run();
    const char* expectedPath = path != nullptr ? path : Entry;
    const bool matched = status.code == code && status.line == line &&
                         strcmp(status.path, expectedPath) == 0 && test.shader.text.IsEmpty();
    if (!matched) {
        fprintf(stderr, "[%s] expected %s at %s:%u, got %s at %s:%u (%s)\n", tag,
                ShaderSourceErrorName(code), expectedPath, line, ShaderSourceErrorName(status.code),
                status.path, status.line, status.message);
    }

    return matched;
}

} // namespace rocks::shader_source_test
