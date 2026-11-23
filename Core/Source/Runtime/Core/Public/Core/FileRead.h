#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>

// 작은 파일 전체 읽기다(Docs/Design/AssetSystem.ko.md 10.2절 "A4d 구현 세부" — 쿠킹 설정·쿠킹
// 매니페스트 등 상한이 있는 파일을 한 번에 읽는 곳이 함께 쓴다).
namespace rocks {

// path 파일 전체를 out에 읽는다(out을 그 크기로 바꾼다). 파일이 없으면 existed = false로 성공한다
// (out은 비운다). 크기가 maxBytes를 넘으면 CapacityExceeded다(existed = true). 오류: 열기·크기·읽기
// IO 오류(FileErrorCode — 읽는 중 줄어들면 EndOfFile 대응), 할당 실패.
//
// 사용 예:
//   Array<uint8> bytes(allocator);
//   bool existed = false;
//   if (auto read = ReadWholeFile(files, path, MaxBytes, bytes, existed); !read) { ... }
//   if (!existed) { /* 기본값 */ }
ROCKS_CORE_API Result<void> ReadWholeFile(platform::FileSystem& files, StringView path,
                                          uint64 maxBytes, Array<uint8>& out,
                                          bool& existed) noexcept;

} // namespace rocks
