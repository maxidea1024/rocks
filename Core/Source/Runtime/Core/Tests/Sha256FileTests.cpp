#include <Core/Encoding/Sha256File.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Process.h>
#include <TestSupport/Test.h>

// 연 파일의 SHA-256 시험이다(Core/Encoding/Sha256File.h — Docs/Design/AssetSystem.ko.md 10.2절 "A4d
// 구현 세부"): 조각보다 큰 파일(여러 조각 + 끝 조각)·빈 파일은 한 번에 구한 SHA-256·크기와 같고,
// 읽던 위치부터 끝까지만 본다, 빈 buffer는 InvalidArgument다. 읽기 실패는 메모리 파일 시스템
// 고장으로 EngineTests(`AssetIndex_ContentHashRejects`)가 본다.
using namespace rocks;

namespace sha256_file_tests_file {

namespace {

// 시험 파일 크기와 읽기 조각 크기다(조각 여럿 + 끝 조각).
constexpr usize FileBytes = 1000;
constexpr usize ChunkBytes = 7;

// 작업 디렉터리의 시험 파일이다(소멸 때 지운다).
struct ScratchFile {
    ScratchFile() noexcept {
        FixedStringBuilder text{Span<char>(path)};
        (void)text.TryAppendFormat("rocks-sha256file-{}.bin", platform::CurrentProcessId());
        length = text.ByteCount();
    }

    ~ScratchFile() noexcept {
        (void)platform::PhysicalFileSystem().Delete(path, length);
    }

    bool Write(Span<const uint8> bytes) noexcept {
        platform::File file;
        if (!file.Open(platform::PhysicalFileSystem(), path, length,
                       platform::FileOpenMode::CreateAlways)) {
            return false;
        }

        const bool written = static_cast<bool>(file.WriteAll(bytes.Data(), bytes.Size()).status);
        return static_cast<bool>(file.Close()) && written;
    }

    bool OpenRead(platform::File& file) noexcept {
        return static_cast<bool>(file.Open(platform::PhysicalFileSystem(), path, length,
                                           platform::FileOpenMode::ReadExisting));
    }

    char path[96]{};
    usize length = 0;
};

} // namespace

// 여러 조각·빈 파일: 한 번에 구한 값과 같다. 읽던 위치부터 끝까지만 본다.
TEST(Sha256File_MatchesWholeHash) {
    uint8 bytes[FileBytes];
    for (usize index = 0; index < FileBytes; ++index) {
        bytes[index] = static_cast<uint8>(index * 13 + 5);
    }

    ScratchFile scratch;
    REQUIRE(scratch.Write(Span<const uint8>(bytes, FileBytes)));
    uint8 buffer[ChunkBytes];
    Sha256Digest digest;
    uint64 read = 0;
    {
        platform::File file;
        REQUIRE(scratch.OpenRead(file));
        REQUIRE(HashFileSha256(file, Span<uint8>(buffer, ChunkBytes), digest, read));
        CHECK(read == FileBytes && digest == Sha256::Hash(Span<const uint8>(bytes, FileBytes)));
        CHECK(file.Close());
    }

    // 앞 100 byte를 읽은 뒤에는 나머지만 본다.
    {
        platform::File file;
        REQUIRE(scratch.OpenRead(file));
        uint8 skipped[100];
        REQUIRE(file.ReadExact(skipped, sizeof(skipped)).status);
        REQUIRE(HashFileSha256(file, Span<uint8>(buffer, ChunkBytes), digest, read));
        CHECK(read == FileBytes - 100 &&
              digest == Sha256::Hash(Span<const uint8>(bytes + 100, FileBytes - 100)));
        CHECK(file.Close());
    }

    REQUIRE(scratch.Write(Span<const uint8>()));
    {
        platform::File file;
        REQUIRE(scratch.OpenRead(file));
        REQUIRE(HashFileSha256(file, Span<uint8>(buffer, ChunkBytes), digest, read));
        CHECK(read == 0 && digest == Sha256::Hash(Span<const uint8>()));
        CHECK(file.Close());
    }
}

// 빈 buffer는 InvalidArgument다.
TEST(Sha256File_RejectsEmptyBuffer) {
    ScratchFile scratch;
    const uint8 one[] = {1};
    REQUIRE(scratch.Write(Span<const uint8>(one, 1)));
    platform::File file;
    REQUIRE(scratch.OpenRead(file));
    Sha256Digest digest;
    uint64 read = 0;
    CHECK(HashFileSha256(file, Span<uint8>(), digest, read).Error() == ErrorCode::InvalidArgument);
    CHECK(file.Close());
}

} // namespace sha256_file_tests_file
