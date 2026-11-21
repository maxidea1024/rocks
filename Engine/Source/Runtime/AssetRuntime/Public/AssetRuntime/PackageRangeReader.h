#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>

// 패키지 파일의 범위 읽기다(Docs/Design/AssetSystem.ko.md 7.1절 2·13 — A2b). 부분 읽기
// (ReadPackageIndex)가 헤더·섹션 표·고른 섹션만 읽을 때 쓴다. 메모리 구현은 시험·이미 읽은 byte에,
// 파일 구현은 색인 검색에 쓴다.
//
// 사용 예:
//   FilePackageRangeReader reader;
//   if (auto opened = reader.Open(platform::PhysicalFileSystem(), path); !opened) { ... }
//   auto view = ReadPackageIndex(allocator, reader, options, &diagnostic);
namespace rocks {

class PackageRangeReader {
public:
    // 파일 전체 byte 수다(Stat 크기 — 헤더의 fileBytes와 대조한다).
    virtual uint64 FileBytes() const noexcept = 0;

    // [offset, offset + out.Size())를 out에 정확히 읽는다. 범위가 파일 밖이면 InvalidArgument, IO
    // 오류는 그 오류(FileErrorCode 대응)다.
    virtual Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept = 0;

protected:
    ~PackageRangeReader() = default;
};

// 메모리 byte를 읽는다(byte는 이 객체보다 오래 살아야 한다).
class MemoryPackageRangeReader final : public PackageRangeReader {
public:
    explicit MemoryPackageRangeReader(Span<const uint8> bytes) noexcept : bytes_(bytes) {
    }

    uint64 FileBytes() const noexcept override {
        return bytes_.Size();
    }

    Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept override;

private:
    Span<const uint8> bytes_;
};

// 파일을 읽는다(열린 동안 파일 핸들을 쥔다 — 한 스레드에서 쓴다).
class FilePackageRangeReader final : public PackageRangeReader {
public:
    FilePackageRangeReader() noexcept = default;

    FilePackageRangeReader(const FilePackageRangeReader&) = delete;
    FilePackageRangeReader& operator=(const FilePackageRangeReader&) = delete;

    // path(UTF-8, NUL 종료가 아니어도 된다)를 읽기로 열고 크기를 잰다. 다시 열면 앞 파일을 닫는다.
    // 오류: 열기·크기 실패는 그 파일 오류(FileErrorCode 대응).
    Result<void> Open(platform::FileSystem& files, StringView path) noexcept;

    uint64 FileBytes() const noexcept override {
        return fileBytes_;
    }

    Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept override;

private:
    platform::File file_;
    uint64 fileBytes_ = 0;
};

} // namespace rocks
