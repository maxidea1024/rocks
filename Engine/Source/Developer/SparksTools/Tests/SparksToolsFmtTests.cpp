#include <TestSupport/NFailAllocator.h>
#include "SparksToolsTestSupport.h"

// `sparks fmt` 시험이다(Docs/Design/SparksTools.ko.md 8.3절·8.4절 — LC8c): 서식이 다른 파일만
// 바꾸고(같은 파일은 수정 시각 그대로), `--check`는 쓰지 않고 알리며, 구문 오류 파일은 건드리지
// 않는다. 파일 인자, 교체 실패, 할당 실패 전수.
using namespace rocks;
namespace sparks_tools_fmt_tests_file {

using namespace rocks::test::sparks_tools;

namespace {

constexpr StringView Messy = "fn F( ):int{return 1+2;}\n";
constexpr StringView Tidy = "fn F(): int {\n    return 1 + 2;\n}\n";
constexpr StringView Clean = "fn G(): int {\n    return 3;\n}\n";
constexpr StringView Broken = "fn H( {\n}\n";

// 수정 시각을 고정 값으로 둔다(다시 쓰지 않았는지 시각 해상도와 무관하게 본다).
constexpr int64 FixedModifiedTicks = 630822816000000000ll; // 2000-01-01 UTC

bool ReadFile(Allocator& allocator, StringView path, String& out) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    Array<char> bytes(allocator);
    const bool ok = file.Size(size) && bytes.TryResize(static_cast<usize>(size)) &&
                    (size == 0 || file.ReadExact(bytes.Data(), bytes.Size()).status);
    const bool closed = static_cast<bool>(file.Close());
    return ok && closed && out.TryAssign(StringView(bytes.Data(), bytes.Size()));
}

int64 ModifiedTicks(StringView path) noexcept {
    platform::FileInfo info;
    CHECK(platform::PhysicalFileSystem().Stat(path.Data(), path.ByteCount(), info));
    return info.modifiedTicks;
}

bool Holds(Allocator& allocator, StringView path, StringView content) noexcept {
    String text(allocator);
    return ReadFile(allocator, path, text) && text.View() == content;
}

// Replace만 실패시키는 실제 파일 시스템 wrapper다.
class FailingReplace final : public platform::FileSystem {
public:
    platform::FileStatus Open(const char* path, usize pathBytes, platform::FileOpenMode mode,
                              platform::FileHandle*& out) noexcept override {
        return inner_.Open(path, pathBytes, mode, out);
    }

    platform::FileStatus FileExists(const char* path, usize pathBytes,
                                    bool& exists) noexcept override {
        return inner_.FileExists(path, pathBytes, exists);
    }

    platform::FileStatus Delete(const char* path, usize pathBytes) noexcept override {
        return inner_.Delete(path, pathBytes);
    }

    platform::FileStatus Stat(const char* path, usize pathBytes,
                              platform::FileInfo& info) noexcept override {
        return inner_.Stat(path, pathBytes, info);
    }

    platform::FileStatus EnumerateDirectory(const char* path, usize pathBytes,
                                            platform::DirectoryVisitor visitor,
                                            void* context) noexcept override {
        return inner_.EnumerateDirectory(path, pathBytes, visitor, context);
    }

    platform::FileStatus Replace(const char*, usize, const char*, usize) noexcept override {
        return platform::FileStatus{platform::FileCode::AccessDenied, 0};
    }

private:
    platform::FileSystem& inner_ = platform::PhysicalFileSystem();
};

} // namespace

TEST(SparksTools_FmtRewritesAndChecks) {
    SystemAllocator allocator;
    {
        TempFolder temp(allocator);
        REQUIRE(temp.Write("Pkg/SparksPackage.json5", Manifest));
        REQUIRE(temp.Write("Pkg/A.spark", Messy));
        REQUIRE(temp.Write("Pkg/B.spark", Clean));
        REQUIRE(temp.Write("Pkg/C.spark", Broken));
        const String folder = temp.PathOf("Pkg");
        const String shown = Slashed(allocator, folder.View());
        const String a = temp.PathOf("Pkg/A.spark");
        const String b = temp.PathOf("Pkg/B.spark");
        const String c = temp.PathOf("Pkg/C.spark");
        REQUIRE(platform::PhysicalFileSystem().SetModifiedTime(b.Data(), b.ByteCount(),
                                                               FixedModifiedTicks));

        // --check: 쓰지 않고 다른 파일을 알린다. 구문 오류 파일은 진단(1).
        {
            const StringView arguments[] = {"fmt", "--check", folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitFailure);
            StringBuilder<512> notFormatted(allocator);
            REQUIRE(notFormatted.TryAppendFormat("{}/A.spark: not formatted\n", shown.View()));
            StringBuilder<512> diagnostic(allocator);
            REQUIRE(diagnostic.TryAppendFormat("{}/C.spark:1:", shown.View()));
            CHECK(output.output.View().StartsWith(diagnostic.View()));
            CHECK(output.output.View().Contains(notFormatted.View(),
                                                StringComparison::CaseSensitive));
            CHECK(output.output.View().EndsWith("1 errors in 1 files (not formatted)\n"
                                                "1 of 3 files need formatting\n"));
            CHECK(Holds(allocator, a.View(), Messy));
        }

        // fmt: 다른 파일만 바꾼다(같은 파일은 수정 시각 그대로), 구문 오류 파일은 그대로(1).
        {
            const StringView arguments[] = {"fmt", folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitFailure);
            CHECK(output.output.View().EndsWith("formatted 1 of 3 files\n"));
            CHECK(Holds(allocator, a.View(), Tidy));
            CHECK(Holds(allocator, b.View(), Clean));
            CHECK(ModifiedTicks(b.View()) == FixedModifiedTicks);
            CHECK(Holds(allocator, c.View(), Broken));
        }

        // 구문 오류를 고치면 모두 서식됨(0), 파일 인자는 그 파일만.
        REQUIRE(temp.Write("Pkg/C.spark", "fn H() {\n}\n"));
        REQUIRE(temp.Write("Pkg/A.spark", Messy));
        {
            const StringView arguments[] = {"fmt", "--check", b.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "0 of 1 files need formatting\n");
        }

        {
            const StringView arguments[] = {"fmt", a.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "formatted 1 of 1 files\n");
            CHECK(Holds(allocator, a.View(), Tidy));
        }

        {
            const StringView arguments[] = {"fmt", "--check", folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "0 of 3 files need formatting\n");
        }

        // 교체 실패: 3, 원본 그대로.
        REQUIRE(temp.Write("Pkg/A.spark", Messy));
        {
            FailingReplace files;
            const StringView arguments[] = {"fmt", folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, files, arguments, output) == ExitError);
            CHECK(output.error.View().Contains("A.spark: cannot write the file",
                                               StringComparison::CaseSensitive));
            CHECK(Holds(allocator, a.View(), Messy));
        }

        // 사용법: `--check`는 fmt에만, 한 번만.
        {
            const StringView misuse[] = {"check", "--check", folder.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), misuse, output) ==
                  ExitUsage);
            CHECK(output.error.View().StartsWith("sparks: '--check' is only for 'fmt'\n"));
            const StringView twice[] = {"fmt", "--check", "--check", folder.View()};
            BufferOutput again(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), twice, again) == ExitUsage);
            CHECK(again.error.View().StartsWith("sparks: '--check' is given twice\n"));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTools_FmtSelectsArgumentModules) {
    // 대상은 인자 아래 모듈이다: 폴더 인자는 그 폴더 아래(패키지 안 하위 폴더면 그 아래만), 파일
    // 인자는 그 파일 — 둘을 섞어도 모두 대상이다.
    SystemAllocator allocator;
    {
        TempFolder temp(allocator);
        REQUIRE(temp.Write("One/SparksPackage.json5", Manifest));
        REQUIRE(temp.Write("One/A.spark", Messy));
        REQUIRE(temp.Write("One/Sub/B.spark", Messy));
        REQUIRE(temp.Write("Two/SparksPackage.json5",
                           "{ name: \"Two\", sparks: 1, profile: \"core\" }\n"));
        REQUIRE(temp.Write("Two/C.spark", Messy));
        REQUIRE(temp.Write("Two/D.spark", Messy));
        const String one = temp.PathOf("One");
        const String sub = temp.PathOf("One/Sub");
        const String d = temp.PathOf("Two/D.spark");
        {
            const StringView arguments[] = {"fmt", "--check", one.View(), d.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitFailure);
            CHECK(output.output.View().EndsWith("3 of 3 files need formatting\n"));
            CHECK(!output.output.View().Contains("C.spark", StringComparison::CaseSensitive));
        }

        {
            const StringView arguments[] = {"fmt", sub.View()};
            BufferOutput output(allocator);
            CHECK(RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output) ==
                  ExitSuccess);
            CHECK(output.output.View() == "formatted 1 of 1 files\n");
            CHECK(Holds(allocator, temp.PathOf("One/Sub/B.spark").View(), Tidy));
            CHECK(Holds(allocator, temp.PathOf("One/A.spark").View(), Messy));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTools_FmtAllocationFailure) {
    // fmt·fmt --check의 모든 할당 실패 자리: ExitError로 끝나고 새는 할당이 없다. 시도마다 원문을
    // 되돌린다.
    SystemAllocator backing;
    TempFolder temp(backing);
    REQUIRE(temp.Write("Pkg/SparksPackage.json5", Manifest));
    REQUIRE(temp.Write("Pkg/B.spark", Clean));
    REQUIRE(temp.Write("Pkg/C.spark", Broken));
    const String folder = temp.PathOf("Pkg");
    const StringView fmt[] = {"fmt", folder.View()};
    const StringView check[] = {"fmt", "--check", folder.View()};
    const Span<const StringView> commands[] = {fmt, check};
    for (const Span<const StringView> arguments : commands) {
        uint64 attempts = 0;
        int32 expected = 0;
        uint64 failures = 0;
        for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
            REQUIRE(temp.Write("Pkg/A.spark", Messy));
            test::NFailAllocator allocator(backing);
            const uint64 before = backing.Stats().liveAllocations;
            {
                BufferOutput output(backing);
                if (failAt != 0) {
                    allocator.FailOnAttempt(failAt);
                }

                const int32 code =
                    RunCommand(allocator, platform::PhysicalFileSystem(), arguments, output);
                const uint64 used = allocator.Attempts();
                allocator.DisableFailure();
                if (failAt == 0) {
                    attempts = used;
                    expected = code;
                    CHECK(code == ExitFailure);
                } else if (code == ExitError) {
                    ++failures;
                } else {
                    CHECK(code == expected);
                }
            }

            CHECK(backing.Stats().liveAllocations == before);
        }

        CHECK(failures > 0);
    }
}

} // namespace sparks_tools_fmt_tests_file
