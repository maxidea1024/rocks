#pragma once

#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Result.h>
#include <Platform/File.h>

// 연 파일의 SHA-256이다(Docs/Design/AssetSystem 10.2절 "A4d 구현 세부" — 색인 콘텐츠 해시·
// 쿠킹 게시 대조·재사용 판정이 함께 쓴다). 파일 전체를 메모리에 두지 않고 buffer 조각으로 흘려
// 읽는다.
namespace rocks {

// file의 지금 위치부터 끝까지 buffer 크기 조각으로 읽어 digest와 읽은 byte 수(bytes)를 쓴다(file은
// 읽기로 연 파일 — 닫지 않는다). 오류: 빈 buffer InvalidArgument, 읽기 실패는 FileErrorCode
// 대응(그때 digest·bytes는 정의되지 않는다).
//
// 사용 예:
//   uint8 buffer[64 * 1024];
//   Sha256Digest digest;
//   uint64 bytes = 0;
//   if (auto hashed = HashFileSha256(file, buffer, digest, bytes); !hashed) { ... }
ROCKS_CORE_API Result<void> HashFileSha256(platform::File& file, Span<uint8> buffer,
                                           Sha256Digest& digest, uint64& bytes) noexcept;

} // namespace rocks
