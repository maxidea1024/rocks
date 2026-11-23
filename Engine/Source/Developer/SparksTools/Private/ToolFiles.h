#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>

// 도구 출력 파일의 비교·원자적 교체다(Docs/Design/SparksTools.ko.md 7절 — build의 `.sparkc`, ids의
// ID manifest).
namespace rocks::sparks::tools::detail {

// GUID 32자 소문자 16진 표기(구분자 없음 — C# `Guid.ToString("N")`)의 글자 수다.
inline constexpr usize GuidDigits = 32;

// 새 무작위 GUID(Core TryNewGuid — UUID v4)를 32자 소문자 16진으로 out에 붙인다. 실패: 난수 실패
// (Unsupported), 할당 실패.
Result<void> AppendNewGuidDigits(StringBuilderBase& out) noexcept;

// 파일 비교 결과다. status가 실패면 읽지 못했다(exists·same은 정해지지 않는다).
struct FileComparison {
    platform::FileStatus status;
    bool exists = false; // 일반 파일이 있다
    bool same = false;   // 있고 내용이 bytes와 같다
};

// path의 내용이 bytes와 같은지 본다. 실패(Result): 할당 실패.
Result<FileComparison> CompareFile(Allocator& allocator, platform::FileSystem& files,
                                   StringView path, Span<const uint8> bytes) noexcept;

// path를 bytes로 원자적으로 바꾼다: 같은 폴더의 임시 파일(`<path>.<GUID 32자>.tmp`, CreateNew)에 다
// 쓰고 Flush·Close한 뒤 FileSystem::Replace(교체 뒤 매체 기록 실패 NotDurable도 실패로 돌려준다).
// 실패하면 임시 파일을 지운다(path는 그대로 — NotDurable은 이미 바뀌었다). 값은 파일 상태, 실패
// (Result)는 할당·난수 실패다.
Result<platform::FileStatus> ReplaceFile(Allocator& allocator, platform::FileSystem& files,
                                         StringView path, Span<const uint8> bytes) noexcept;

// ReplaceFile이 NotDurable일 때의 안내다(교체는 보이지만 전원이 끊기면 옛 내용일 수 있다).
inline constexpr StringView NotDurableMessage =
    "the file was replaced, but the folder could not be flushed to the storage";

// 글자열을 byte 범위로 본다.
inline Span<const uint8> BytesOf(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

} // namespace rocks::sparks::tools::detail
