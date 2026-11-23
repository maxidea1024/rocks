#pragma once

#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <SparksTools/SparksCommand.h>
#include <TestSupport/Test.h>

// `sparks` 명령 시험의 공용 준비다(Docs/Design/SparksTools.ko.md 5절·7.3절): 출력 버퍼, 임시 폴더,
// 경로 표기.
namespace rocks::test::sparks_tools {

using namespace rocks::sparks::tools;

// 출력을 모으는 명령 출력 대상이다.
class BufferOutput final : public CommandOutput {
public:
    explicit BufferOutput(Allocator& allocator) noexcept : output(allocator), error(allocator) {
    }

    Result<void> WriteOutput(StringView text) noexcept override {
        return output.TryAppend(text);
    }

    Result<void> WriteError(StringView text) noexcept override {
        return error.TryAppend(text);
    }

    String output;
    String error;
};

// 시험마다 쓰는 임시 폴더다(끝에 지운다).
class TempFolder final {
public:
    explicit TempFolder(Allocator& allocator) noexcept : root_(allocator), allocator_(&allocator) {
        CHECK(root_.TryAppendFormat("rocks-sparks-tools-{}-{}", platform::CurrentProcessId(),
                                    platform::MonotonicTicks()));
    }

    ~TempFolder() noexcept {
        static_cast<void>(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(),
                                                        root_.View().Data(), root_.ByteCount()));
    }

    StringView Root() const noexcept {
        return root_.View();
    }

    // relative(`/` 구분)에 content를 쓴다(폴더를 만든다).
    bool Write(StringView relative, StringView content) noexcept {
        platform::FileSystem& files = platform::PhysicalFileSystem();
        auto path = Path::Combine(*allocator_, root_.View(), relative);
        if (!path) {
            return false;
        }

        const StringView full = path.Value().View();
        usize slash = full.ByteCount();
        while (slash > 0 && full.Data()[slash - 1] != '/' && full.Data()[slash - 1] != '\\') {
            --slash;
        }

        if (slash > 0 && !platform::MakeDirectoryTree(files, full.Data(), slash - 1)) {
            return false;
        }

        platform::File file;
        if (!file.Open(files, full.Data(), full.ByteCount(),
                       platform::FileOpenMode::CreateAlways)) {
            return false;
        }

        const platform::FileTransfer written = file.WriteAll(content.Data(), content.ByteCount());
        const bool closed = static_cast<bool>(file.Close());
        return static_cast<bool>(written.status) && closed;
    }

    // root 아래 relative의 경로다.
    String PathOf(StringView relative) const noexcept {
        auto path = Path::Combine(*allocator_, root_.View(), relative);
        CHECK(path);
        return path ? Move(path.Value()) : String(*allocator_);
    }

private:
    StringBuilder<128> root_;
    Allocator* allocator_;
};

inline constexpr StringView Manifest = "{ name: \"Game\", sparks: 1, profile: \"core\" }\n";

// 폴더 경로 표기(`\` → `/`)다 — 진단 경로 기대값을 만든다.
inline String Slashed(Allocator& allocator, StringView path) noexcept {
    String out(allocator);
    for (usize at = 0; at < path.ByteCount(); ++at) {
        const char c = path.Data()[at] == '\\' ? '/' : path.Data()[at];
        CHECK(out.TryAppend(StringView(&c, 1)));
    }

    return out;
}

} // namespace rocks::test::sparks_tools
